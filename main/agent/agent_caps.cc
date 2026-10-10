/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * See agent_caps.h. mpv2-claw wrote one C file per group (cap_robot.c,
 * cap_display.c, cap_mpx_skill.c, cap_music.c) with a hand-written execute()
 * per tool. Here the tools already exist as a table (robot_tools, shared
 * with the voice server's MCP), so the descriptors are built from it. A
 * capability's execute() is not told which descriptor it belongs to, hence
 * one trampoline per table slot, generated below.
 */

#include "agent/agent_caps.h"

#include <cstdio>
#include <cstring>
#include <utility>

#include "esp_log.h"

#include "agent/robot_tools.h"
#include "app_capabilities.h"
#include "claw_cap.h"

static const char *TAG = "agent_caps";

namespace {

using robot_tools::Reply;
using robot_tools::Tool;

/* Room for the table to grow without touching this file. */
constexpr size_t kMaxTools = 24;
/* CLAW_CAP_TOOL_DESCRIPTION_MAX: longer descriptions are silently cut. */
constexpr size_t kDescriptionMax = 256;

struct Group {
    const char *id;
    const char *display_name;
    const char *plugin_name;
    const char *family;
    claw_cap_descriptor_t descriptors[kMaxTools];
    size_t count;
    claw_cap_group_t group;
};

Group s_groups[] = {
    { "cap_robot",     "Robot",        "MangDang quadruped",       "robot",   {}, 0, {} },
    { "cap_display",   "Screen",       "MangDang robot screen",    "display", {}, 0, {} },
    { "cap_mpx_skill", "Robot skills", "MangDang robot skills",    "skill",   {}, 0, {} },
    { "cap_music",     "Music",        "MangDang robot music",     "music",   {}, 0, {} },
};
constexpr size_t kGroupCount = sizeof(s_groups) / sizeof(s_groups[0]);

/* Which table row each trampoline runs. */
const Tool *s_slot_tool[kMaxTools];

esp_err_t run_slot(size_t slot, const char *input_json, char *output, size_t output_size)
{
    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const Tool *tool = slot < kMaxTools ? s_slot_tool[slot] : nullptr;
    if (!tool) {
        snprintf(output, output_size, "Error: tool slot %u is not bound", (unsigned)slot);
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "%s", tool->name);
    /* ESP-Claw's agent core logs and reports an error text itself when the
     * execute returns anything but ESP_OK; the Reply's own error flag maps
     * onto that. Information for the model ("no such movement") is ESP_OK. */
    const Reply r = robot_tools::call(tool->name, input_json ? input_json : "{}");
    snprintf(output, output_size, "%s", r.text.c_str());
    return r.error ? ESP_ERR_INVALID_ARG : ESP_OK;
}

template <size_t Slot>
esp_err_t trampoline(const char *input_json, const claw_cap_call_context_t *ctx,
                     char *output, size_t output_size)
{
    (void)ctx;
    return run_slot(Slot, input_json, output, output_size);
}

template <size_t... Slots>
claw_cap_execute_fn trampoline_at(size_t slot, std::index_sequence<Slots...>)
{
    static const claw_cap_execute_fn table[] = { &trampoline<Slots>... };
    return slot < sizeof...(Slots) ? table[slot] : nullptr;
}

claw_cap_execute_fn trampoline_for(size_t slot)
{
    return trampoline_at(slot, std::make_index_sequence<kMaxTools>{});
}

Group *group_for(const char *id)
{
    for (Group &g : s_groups) {
        if (strcmp(g.id, id) == 0) {
            return &g;
        }
    }
    return nullptr;
}

/* Fill every group's descriptor array from the tool table. Once. */
void build_descriptors()
{
    static bool built = false;
    if (built) {
        return;
    }
    built = true;

    size_t count = 0;
    const Tool *table = robot_tools::all(&count);
    size_t slot = 0;
    for (size_t k = 0; k < count && slot < kMaxTools; k++) {
        const Tool &t = table[k];
        Group *g = group_for(t.group);
        if (!g) {
            ESP_LOGW(TAG, "%s: unknown group %s, skipped", t.name, t.group);
            continue;
        }
        if (g->count >= kMaxTools) {
            ESP_LOGW(TAG, "%s: group %s is full, skipped", t.name, g->id);
            continue;
        }
        const char *description = robot_tools::claw_description(t);
        if (strlen(description) >= kDescriptionMax) {
            ESP_LOGW(TAG, "%s: description is %u bytes, the agent sees the first %u",
                     t.name, (unsigned)strlen(description), (unsigned)(kDescriptionMax - 1));
        }
        s_slot_tool[slot] = &t;
        claw_cap_descriptor_t &d = g->descriptors[g->count++];
        d = {};
        d.id = t.name;
        d.name = t.name;
        d.family = g->family;
        d.description = description;
        d.kind = CLAW_CAP_KIND_CALLABLE;
        d.cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM;
        d.input_schema_json = t.schema;
        d.execute = trampoline_for(slot);
        slot++;
    }

    for (Group &g : s_groups) {
        g.group = {};
        g.group.group_id = g.id;
        g.group.plugin_name = g.plugin_name;
        g.group.version = "1";
        g.group.descriptors = g.descriptors;
        g.group.descriptor_count = g.count;
    }
}

esp_err_t register_group(const Group &g)
{
    if (g.count == 0) {
        return ESP_OK;    /* e.g. cap_music in a build without voice */
    }
    if (claw_cap_group_exists(g.id)) {
        return ESP_OK;
    }
    return claw_cap_register_group(&g.group);
}

/* app_claw calls these from app_capabilities_init(), after claw_cap_init(). */
template <size_t Index>
esp_err_t reg_group(const app_claw_config_t *config, const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    build_descriptors();
    return register_group(s_groups[Index]);
}

const app_capability_register_fn kRegFns[kGroupCount] = {
    reg_group<0>, reg_group<1>, reg_group<2>, reg_group<3>,
};

}  // namespace

namespace agent_caps {

esp_err_t register_groups()
{
    build_descriptors();
    esp_err_t result = ESP_OK;
    for (size_t i = 0; i < kGroupCount; i++) {
        const Group &g = s_groups[i];
        if (g.count == 0) {
            continue;
        }
        app_capability_external_group_t ext = {};
        ext.group_id = g.id;
        ext.display_name = g.display_name;
        ext.llm_visible_by_default = true;
        ext.prepare = nullptr;
        ext.reg = kRegFns[i];
        const esp_err_t err = app_capabilities_register_external_group(&ext);
        if (err != ESP_OK) {
            /* Not fatal: a robot whose agent cannot move it is still worth
             * booting into the app so it can be told why. */
            ESP_LOGE(TAG, "could not register %s: %s", g.id, esp_err_to_name(err));
            result = err;
        } else {
            ESP_LOGI(TAG, "%s: %u tool(s)", g.id, (unsigned)g.count);
        }
    }
    return result;
}

}  // namespace agent_caps
