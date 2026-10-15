/*
 * SPDX-FileCopyrightText: 2026 MangDang
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The C facade declared in mpx_wasm.h, plus the one piece of real wiring this
 * component owns: handing mpx_robot the two predicates its gait loop needs.
 */

#include "mpx_wasm.h"

#include <cstring>

#include "esp_log.h"

#include "autorun.h"
#include "events.h"
#include "movement.h"
#include "mpx_robot.h"
#include "mpx_skill_fs.h"
#include "registry.h"
#include "runner.h"
#include "wasm_host_functions.h"
#include "wasm_sandbox.h"

static const char *TAG = "mpx_wasm";

namespace {

/* ── The seam back to mpx_robot ───────────────────────────────────────────
 *
 * robot.cc used to call wasm::is_running() and sdk::control_owner_is_pose()
 * directly, when both lived in the same component. They do not any more, and
 * the dependency runs this way round -- mpx_wasm drives mpx_robot, not the
 * reverse -- so mpx_robot asks through a hook table instead of depending on
 * us. These two functions are that table.
 *
 * Static storage on purpose: mpx_robot keeps the pointer, so it has to outlive
 * the call.                                                               */
bool hook_skill_is_running(void)
{
    return wasm::is_running();
}

bool hook_skill_owns_pose(void)
{
    return sdk::control_owner_is_pose();
}

void hook_request_stop(void)
{
    wasm::request_stop();
}

const mpx_robot_skill_hooks_t s_skill_hooks = {
    .skill_is_running = hook_skill_is_running,
    .skill_owns_pose  = hook_skill_owns_pose,
    .request_stop     = hook_request_stop,
};

bool s_ready = false;

}  // namespace

extern "C" {

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

bool mpx_wasm_init(void)
{
    if (s_ready) {
        return true;
    }
    if (!wasm::init_sandbox()) {
        ESP_LOGE(TAG, "WAMR init failed - skills unavailable");
        return false;
    }
    mpx_robot_set_skill_hooks(&s_skill_hooks);
    s_ready = true;
    ESP_LOGI(TAG, "WASM runtime ready (ABI %d)", (int)sdk::MPX_ABI_VERSION);
    return true;
}

void mpx_wasm_start_skills(void)
{
    if (!s_ready) {
        ESP_LOGW(TAG, "start_skills before init - ignoring");
        return;
    }
    skills::rescan();
    ESP_LOGI(TAG, "%d skill(s) installed", (int)skills::all().size());
    skills::events_start();
    skills::autorun_boot();
}

/* ── Running a skill ───────────────────────────────────────────────────── */

bool mpx_wasm_run_skill(const char *file, const char *params, const char *why)
{
    if (file == nullptr || file[0] == '\0') {
        return false;
    }
    /* The Raspberry Pi (ROS) drives the robot in slave mode; master::begin()
     * already stopped any running skill, and a new one would only have every
     * motion call refused. Applies to the app, the agent, events and autorun. */
    if (mpx_robot_pi_in_control()) {
        ESP_LOGW(TAG, "not starting %s: the Raspberry Pi is in control", file);
        return false;
    }

    /* Behaviour-mode skills run without a total watchdog, so the registry has
     * to be consulted before starting rather than after -- getting this wrong
     * means a behaviour is killed after 60 seconds for no visible reason. */
    std::string path = (file[0] == '/') ? file : (std::string("/") + file);
    skills::Mode mode = skills::Mode::OneShot;
    std::string name  = path.substr(1);

    for (const skills::Entry &e : skills::all()) {
        if (e.file == path) {
            if (e.behaviour) {
                mode = skills::Mode::Behaviour;
            }
            if (!e.slug.empty()) {
                name = e.slug;
            }
            break;
        }
    }

    return skills::start(path.c_str(), name.c_str(),
                         params != nullptr ? params : "",
                         mode, why != nullptr ? why : "api");
}

void mpx_wasm_stop_skill(void)        { skills::stop(); }
bool mpx_wasm_skill_running(void)     { return skills::running(); }

const char *mpx_wasm_running_skill_name(void)
{
    const std::string &n = skills::current();
    return n.empty() ? nullptr : n.c_str();
}

/* ── The registry ──────────────────────────────────────────────────────── */

void mpx_wasm_rescan(void)      { skills::rescan(); }
int  mpx_wasm_skill_count(void) { return (int)skills::all().size(); }

bool mpx_wasm_skill_at(int index,
                       const char **out_file,
                       const char **out_slug,
                       const char **out_provides_gait,
                       int *out_abi,
                       bool *out_autorun,
                       bool *out_behaviour)
{
    const std::vector<skills::Entry> &all = skills::all();
    if (index < 0 || index >= (int)all.size()) {
        return false;
    }
    const skills::Entry &e = all[(size_t)index];
    if (out_file)          { *out_file = e.file.c_str(); }
    if (out_slug)          { *out_slug = e.slug.c_str(); }
    if (out_provides_gait) { *out_provides_gait = e.provides_gait.c_str(); }
    if (out_abi)           { *out_abi = e.abi; }
    if (out_autorun)       { *out_autorun = e.autorun; }
    if (out_behaviour)     { *out_behaviour = e.behaviour; }
    return true;
}

/* ── Movement ──────────────────────────────────────────────────────────── */

mpx_movement_result_t mpx_wasm_movement_run(const char *name, bool from_skill)
{
    switch (skills::run(name, from_skill)) {
        case skills::MovementResult::Started:       return MPX_MOVEMENT_STARTED;
        case skills::MovementResult::Busy:          return MPX_MOVEMENT_BUSY;
        case skills::MovementResult::NotPermitted:  return MPX_MOVEMENT_NOT_PERMITTED;
        case skills::MovementResult::Unknown:
        default:                                    return MPX_MOVEMENT_UNKNOWN;
    }
}

const char *mpx_wasm_movement_result_text(mpx_movement_result_t r)
{
    switch (r) {
        case MPX_MOVEMENT_STARTED:       return "started";
        case MPX_MOVEMENT_BUSY:          return "a skill is already running";
        case MPX_MOVEMENT_NOT_PERMITTED: return "a skill cannot start another skill";
        case MPX_MOVEMENT_UNKNOWN:
        default:                         return "no such movement";
    }
}

/* ── The skills directory ──────────────────────────────────────────────── */

bool mpx_wasm_skill_file_write(const char *name, const void *data, size_t len)
{
    return name != nullptr && data != nullptr &&
           fs::write_file(name, (const std::uint8_t *)data, len);
}

size_t mpx_wasm_skill_file_read(const char *name, void *buf, size_t buf_size)
{
    if (name == nullptr || buf == nullptr) {
        return 0;
    }
    const std::vector<std::uint8_t> bytes = fs::read_file(name);
    if (bytes.empty() || bytes.size() > buf_size) {
        return 0;
    }
    memcpy(buf, bytes.data(), bytes.size());
    return bytes.size();
}

bool mpx_wasm_skill_file_delete(const char *name)
{
    return name != nullptr && fs::delete_file(name);
}

bool mpx_wasm_skill_file_exists(const char *name)
{
    return name != nullptr && fs::exists(name);
}

/* ── Events and safe mode ──────────────────────────────────────────────── */

bool mpx_wasm_fire_chat(const char *message)
{
    return message != nullptr && skills::fire_chat(message);
}

bool mpx_wasm_safe_mode(void)       { return skills::safe_mode(); }
void mpx_wasm_clear_safe_mode(void) { skills::clear_safe_mode(); }

}  // extern "C"
