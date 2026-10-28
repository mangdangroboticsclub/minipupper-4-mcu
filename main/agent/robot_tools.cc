/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * See robot_tools.h. These bodies were voice_tools.cc's until Phase 5 made
 * the on-device agent a second caller.
 */

#include "agent/robot_tools.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "esp_log.h"
#include "sdkconfig.h"

#include "mpx_robot.h"
#include "network/http_server.h"
#include "wasm/registry.h"

#if CONFIG_APP_DISPLAY_ENABLE
#include "cap_display.h"
#endif
#if CONFIG_APP_VOICE_ENABLE
#include "cap_music.h"
#endif

namespace robot_tools {

namespace {

constexpr float kMaxRollDeg  = 25.0f;
constexpr float kMaxPitchDeg = 20.0f;
constexpr float kMaxYawDeg   = 30.0f;
constexpr int   kMaxDriveMs  = 10000;

constexpr const char *kGroupRobot   = "cap_robot";
constexpr const char *kGroupDisplay = "cap_display";
constexpr const char *kGroupSkill   = "cap_mpx_skill";
constexpr const char *kGroupMusic   = "cap_music";

/* ── helpers ─────────────────────────────────────────────────────────── */

float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

float num(const cJSON *obj, const char *key, float dflt)
{
    const cJSON *v = obj ? cJSON_GetObjectItem(obj, key) : nullptr;
    return cJSON_IsNumber(v) ? (float)v->valuedouble : dflt;
}

const char *str(const cJSON *obj, const char *key)
{
    const cJSON *v = obj ? cJSON_GetObjectItem(obj, key) : nullptr;
    return (cJSON_IsString(v) && v->valuestring[0]) ? v->valuestring : nullptr;
}

std::string print_and_free(cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);
    std::string out = s ? s : "{}";
    cJSON_free(s);
    cJSON_Delete(obj);
    return out;
}

std::string fail(const char *error)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", false);
    cJSON_AddStringToObject(o, "error", error);
    return print_and_free(o);
}

Reply pi_refuses()
{
    Reply r;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", false);
    cJSON_AddStringToObject(o, "error", "pi_in_control");
    cJSON_AddStringToObject(o, "detail",
        "The Raspberry Pi (ROS) is controlling the robot right now, so it cannot "
        "move on request. It goes back to normal when the Pi releases control.");
    r.text = print_and_free(o);
    return r;
}

Reply not_ready()
{
    return { "Error: the robot did not initialise. The servo driver boards may be "
             "unpowered or unplugged.", true };
}

/* ── robot ───────────────────────────────────────────────────────────── */

Reply robot_move(const cJSON *args)
{
    if (!mpx_robot_ready()) return not_ready();
    if (mpx_robot_pi_in_control()) return pi_refuses();

    if (const char *movement = str(args, "movement")) {
        /* Built-ins first, then a skill that provides the name (mpv2's
         * movement layer did the same: a downloaded skill must not shadow
         * "advance"). */
        if (mpx_robot_gait_by_name(movement) == ESP_OK) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddBoolToObject(o, "ok", true);
            cJSON_AddStringToObject(o, "movement", movement);
            return { print_and_free(o), false };
        }
        std::string slug;
        const char *why = network::skill_start_for_movement(movement, &slug);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", why == nullptr);
        cJSON_AddStringToObject(o, "movement", movement);
        if (why) {
            cJSON_AddStringToObject(o, "error", why);
        } else {
            cJSON_AddStringToObject(o, "skill", slug.c_str());
        }
        return { print_and_free(o), false };
    }

    const float f = clampf(num(args, "forward", 0.0f), -1.0f, 1.0f);
    const float s = clampf(num(args, "strafe", 0.0f), -1.0f, 1.0f);
    const float t = clampf(num(args, "turn", 0.0f), -1.0f, 1.0f);
    int duration = (int)num(args, "duration_ms", 0.0f);
    if (std::fabs(f) < 0.01f && std::fabs(s) < 0.01f && std::fabs(t) < 0.01f && duration == 0) {
        return { "Error: give either a movement name, or forward/strafe/turn.", true };
    }
    mpx_robot_drive(f, s, t);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON *d = cJSON_AddObjectToObject(o, "driving");
    cJSON_AddNumberToObject(d, "forward", f);
    cJSON_AddNumberToObject(d, "strafe", s);
    cJSON_AddNumberToObject(d, "turn", t);
    if (duration > 0) {
        /* The joystick path times out by itself when nothing refreshes it,
         * so a duration is a promise to stop, not a blocking wait. */
        if (duration > kMaxDriveMs) duration = kMaxDriveMs;
        cJSON_AddNumberToObject(o, "stops_after_ms", duration);
    }
    return { print_and_free(o), false };
}

Reply robot_pose(const cJSON *args)
{
    if (!mpx_robot_ready()) return not_ready();
    if (mpx_robot_pi_in_control()) return pi_refuses();

    const float roll  = clampf(num(args, "roll", 0.0f), -kMaxRollDeg, kMaxRollDeg);
    const float pitch = clampf(num(args, "pitch", 0.0f), -kMaxPitchDeg, kMaxPitchDeg);
    const float yaw   = clampf(num(args, "yaw", 0.0f), -kMaxYawDeg, kMaxYawDeg);
    const float speed = num(args, "speed_dps", 0.0f);
    mpx_robot_set_attitude_speed(speed < 0.0f ? 0.0f : speed);
    mpx_robot_set_body_attitude(roll, pitch, yaw);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddNumberToObject(o, "roll", roll);
    cJSON_AddNumberToObject(o, "pitch", pitch);
    cJSON_AddNumberToObject(o, "yaw", yaw);
    return { print_and_free(o), false };
}

Reply robot_get_state(const cJSON *)
{
    if (!mpx_robot_ready()) return { fail("robot not initialised"), false };

    mpx_robot_config_t cfg;
    mpx_robot_imu_t imu;
    mpx_robot_get_config(&cfg);
    mpx_robot_imu_read(&imu);

    /* One servo temperature, not twelve: "is anything overheating" is the
     * question this answers; Servo Studio has the per-servo detail. */
    int hottest_id = 0;
    float hottest = -300.0f;
    for (int id = 1; id <= 12; id++) {
        const float c = mpx_robot_read_temperature_c(id);
        if (!std::isnan(c) && c > hottest) {
            hottest = c;
            hottest_id = id;
        }
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "movement", mpx_robot_current_gait_name());
    cJSON *c = cJSON_AddObjectToObject(o, "config");
    cJSON_AddNumberToObject(c, "period", cfg.period);
    cJSON_AddNumberToObject(c, "height", cfg.height);
    cJSON_AddNumberToObject(c, "up_height", cfg.up_height);
    cJSON_AddNumberToObject(c, "stride", cfg.stride);
    cJSON_AddNumberToObject(c, "tilt", cfg.tilt);
    cJSON_AddNumberToObject(c, "speed_mm_s", cfg.sg_speed);
    cJSON *i = cJSON_AddObjectToObject(o, "imu");
    const double accel[3] = { imu.ax, imu.ay, imu.az };
    const double gyro[3]  = { imu.gx, imu.gy, imu.gz };
    cJSON_AddItemToObject(i, "accel_g", cJSON_CreateDoubleArray(accel, 3));
    cJSON_AddItemToObject(i, "gyro_dps", cJSON_CreateDoubleArray(gyro, 3));
    if (hottest_id != 0) {
        cJSON *h = cJSON_AddObjectToObject(o, "hottest_servo");
        cJSON_AddNumberToObject(h, "id", hottest_id);
        cJSON_AddNumberToObject(h, "celsius", hottest);
    }
    cJSON_AddStringToObject(o, "servos", mpx_robot_power_state());
    cJSON *moves = cJSON_AddArrayToObject(o, "movements");
    for (int k = 0; k < mpx_robot_gait_name_count(); k++) {
        if (const char *name = mpx_robot_gait_name_at(k)) {
            if (std::strcmp(name, "none") != 0) {
                cJSON_AddItemToArray(moves, cJSON_CreateString(name));
            }
        }
    }
    for (const skills::Entry &e : skills::all()) {
        if (!e.provides_gait.empty()) {
            cJSON_AddItemToArray(moves, cJSON_CreateString(e.provides_gait.c_str()));
        }
    }
    return { print_and_free(o), false };
}

Reply robot_set_config(const cJSON *args)
{
    if (!mpx_robot_ready()) return not_ready();
    if (mpx_robot_pi_in_control()) return pi_refuses();

    /* Read-modify-write: every field is optional and an omitted one keeps
     * its value rather than resetting to a default. */
    mpx_robot_config_t cfg;
    mpx_robot_get_config(&cfg);
    cfg.period    = (int)clampf(num(args, "period",     (float)cfg.period),     1.0f, 200.0f);
    cfg.height    = (int)clampf(num(args, "height",     (float)cfg.height),     0.0f, 150.0f);
    cfg.up_height = (int)clampf(num(args, "up_height",  (float)cfg.up_height),  0.0f,  50.0f);
    cfg.stride    = (int)clampf(num(args, "stride",     (float)cfg.stride),     0.0f,  50.0f);
    cfg.tilt      = (int)clampf(num(args, "tilt",       (float)cfg.tilt),     -30.0f,  30.0f);
    cfg.sg_speed  = (int)clampf(num(args, "speed_mm_s", (float)cfg.sg_speed),   0.0f, 200.0f);
    mpx_robot_set_config(&cfg);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddNumberToObject(o, "period", cfg.period);
    cJSON_AddNumberToObject(o, "height", cfg.height);
    cJSON_AddNumberToObject(o, "up_height", cfg.up_height);
    cJSON_AddNumberToObject(o, "stride", cfg.stride);
    cJSON_AddNumberToObject(o, "tilt", cfg.tilt);
    cJSON_AddNumberToObject(o, "speed_mm_s", cfg.sg_speed);
    return { print_and_free(o), false };
}

/* ── display ─────────────────────────────────────────────────────────── */

#if CONFIG_APP_DISPLAY_ENABLE
Reply display_result(esp_err_t err, const char *bad)
{
    if (err == ESP_OK) return { "{\"ok\":true}", false };
    if (err == ESP_ERR_NOT_SUPPORTED) return { fail("this robot has no screen"), false };
    return { std::string("Error: ") + (bad ? bad : esp_err_to_name(err)), true };
}

Reply display_show_text(const cJSON *args)
{
    const char *text = str(args, "text");
    if (!text) return { "Error: 'text' is required", true };
    return display_result(cap_display_show_text(text), nullptr);
}

Reply display_show_emotion(const cJSON *args)
{
    const char *name = str(args, "emotion");
    if (!name) return { "Error: 'emotion' is required", true };
    const esp_err_t err = cap_display_show_emotion(name);
    if (err == ESP_ERR_NOT_FOUND) return { fail("no such emotion"), false };
    return display_result(err, nullptr);
}

Reply display_clear(const cJSON *) { return display_result(cap_display_clear(), nullptr); }

/* display_draw: mpv2-claw's "draw a crab" without agent-side Lua. The model
 * sends primitives; polylines and polygons are expanded here into lines and
 * triangles (a polygon is fanned from its first point, so convex shapes are
 * exact and the model is told to split concave ones). */
int16_t clamp16(float v) { return (int16_t)clampf(v, -2000.0f, 2000.0f); }

bool point_at(const cJSON *pts, int i, int16_t out[2])
{
    const cJSON *pt = cJSON_GetArrayItem(pts, i);
    if (!cJSON_IsArray(pt) || cJSON_GetArraySize(pt) < 2) return false;
    const cJSON *x = cJSON_GetArrayItem(pt, 0), *y = cJSON_GetArrayItem(pt, 1);
    if (!cJSON_IsNumber(x) || !cJSON_IsNumber(y)) return false;
    out[0] = clamp16((float)x->valuedouble);
    out[1] = clamp16((float)y->valuedouble);
    return true;
}

Reply display_draw(const cJSON *args)
{
    const cJSON *shapes = args ? cJSON_GetObjectItem(args, "shapes") : nullptr;
    if (!cJSON_IsArray(shapes)) return { "Error: 'shapes' (an array) is required", true };

    uint32_t bg = 0x000000;
    if (const char *b = str(args, "background")) {
        const int32_t v = cap_display_parse_color(b);
        if (v < 0) return { fail("unreadable background colour"), false };
        bg = (uint32_t)v;
    }
    const float secs = clampf(num(args, "seconds", 20.0f), 0.0f, 600.0f);

    std::vector<cap_draw_shape_t> out;
    out.reserve(32);
    int index = -1;
    const cJSON *it = nullptr;
    cJSON_ArrayForEach(it, shapes) {
        index++;
        const char *type = str(it, "type");
        if (!type) return { "Error: shape " + std::to_string(index) + " has no 'type'", true };

        cap_draw_shape_t sh = {};
        int32_t col = 0xFFFFFF;
        if (const char *cs = str(it, "color")) {
            col = cap_display_parse_color(cs);
            if (col < 0) return { "Error: shape " + std::to_string(index) + ": unreadable color", true };
        }
        sh.color = (uint32_t)col;
        sh.border_color = -1;
        if (const char *bs = str(it, "border")) sh.border_color = cap_display_parse_color(bs);
        const cJSON *fill = cJSON_GetObjectItem(it, "fill");
        sh.filled = !cJSON_IsBool(fill) || cJSON_IsTrue(fill);
        sh.width = clamp16(num(it, "width", 0.0f));
        sh.x = clamp16(num(it, "x", 0.0f));
        sh.y = clamp16(num(it, "y", 0.0f));
        const cJSON *pts = cJSON_GetObjectItem(it, "points");
        const int npts = cJSON_IsArray(pts) ? cJSON_GetArraySize(pts) : 0;

        if (!strcmp(type, "rect")) {
            sh.kind = CAP_DRAW_RECT;
            sh.w = clamp16(num(it, "w", 10.0f));
            sh.h = clamp16(num(it, "h", 10.0f));
            sh.r = clamp16(num(it, "radius", 0.0f));
            out.push_back(sh);
        } else if (!strcmp(type, "circle") || !strcmp(type, "oval")) {
            sh.kind = CAP_DRAW_CIRCLE;
            const float r = num(it, "r", 10.0f);
            sh.w = clamp16(2.0f * num(it, "rx", r));
            sh.h = clamp16(2.0f * num(it, "ry", r));
            out.push_back(sh);
        } else if (!strcmp(type, "arc")) {
            sh.kind = CAP_DRAW_ARC;
            sh.r = clamp16(num(it, "r", 20.0f));
            sh.start = clamp16(num(it, "start", 0.0f));
            sh.end = clamp16(num(it, "end", 180.0f));
            out.push_back(sh);
        } else if (!strcmp(type, "text")) {
            sh.kind = CAP_DRAW_TEXT;
            const char *t = str(it, "text");
            if (!t) return { "Error: shape " + std::to_string(index) + ": text needs 'text'", true };
            snprintf(sh.text, sizeof(sh.text), "%s", t);
            const char *size = str(it, "size");
            sh.size = (size && !strcmp(size, "large")) ? 2 : (size && !strcmp(size, "medium")) ? 1 : 0;
            out.push_back(sh);
        } else if (!strcmp(type, "line") || !strcmp(type, "polyline")) {
            if (npts < 2) return { "Error: shape " + std::to_string(index) + ": a line needs 2+ points", true };
            sh.kind = CAP_DRAW_LINE;
            for (int i = 0; i + 1 < npts; i++) {
                if (!point_at(pts, i, sh.p[0]) || !point_at(pts, i + 1, sh.p[1]))
                    return { "Error: shape " + std::to_string(index) + ": points are [x,y] pairs", true };
                out.push_back(sh);
            }
        } else if (!strcmp(type, "triangle") || !strcmp(type, "polygon")) {
            if (npts < 3) return { "Error: shape " + std::to_string(index) + ": needs 3+ points", true };
            sh.kind = CAP_DRAW_TRIANGLE;
            if (!point_at(pts, 0, sh.p[0])) return { "Error: points are [x,y] pairs", true };
            for (int i = 1; i + 1 < npts; i++) {
                if (!point_at(pts, i, sh.p[1]) || !point_at(pts, i + 1, sh.p[2]))
                    return { "Error: shape " + std::to_string(index) + ": points are [x,y] pairs", true };
                out.push_back(sh);
            }
        } else {
            return { "Error: shape " + std::to_string(index) + ": unknown type '" + type +
                     "' (rect, circle, oval, line, polyline, triangle, polygon, arc, text)", true };
        }
        if (out.size() > CAP_DRAW_MAX_SHAPES) {
            return { fail("too many shapes (max 200 after expanding lines and polygons)"), false };
        }
    }

    const esp_err_t err = cap_display_draw(out.data(), out.size(), bg, (uint32_t)(secs * 1000.0f));
    if (err == ESP_OK) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", true);
        cJSON_AddNumberToObject(o, "primitives", (double)out.size());
        cJSON_AddNumberToObject(o, "seconds", secs);
        return { print_and_free(o), false };
    }
    return display_result(err, nullptr);
}

Reply display_set_colors(const cJSON *args)
{
    const char *bg = str(args, "background");
    const char *eyes = str(args, "eyes");
    if (!bg && !eyes) return { "Error: give 'background' and/or 'eyes'", true };
    const int32_t bg_rgb = bg ? cap_display_parse_color(bg) : -1;
    const int32_t eye_rgb = eyes ? cap_display_parse_color(eyes) : -1;
    if ((bg && bg_rgb < 0) || (eyes && eye_rgb < 0)) {
        return { fail("unreadable colour; use a name like blue or hex like #00ff00"), false };
    }
    return display_result(cap_display_set_colors(bg_rgb, eye_rgb), nullptr);
}

Reply display_set_brightness(const cJSON *args)
{
    const cJSON *p = args ? cJSON_GetObjectItem(args, "percent") : nullptr;
    if (!cJSON_IsNumber(p)) return { "Error: 'percent' is required", true };
    const int pct = (int)clampf((float)p->valuedouble, 0.0f, 100.0f);
    return display_result(cap_display_set_brightness(pct), nullptr);
}
#endif  /* CONFIG_APP_DISPLAY_ENABLE */

/* ── skills ──────────────────────────────────────────────────────────── */

Reply mpx_list_skills(const cJSON *)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddBoolToObject(o, "safe_mode", false);
    if (network::skill_running()) {
        cJSON_AddStringToObject(o, "running", network::skill_running_name());
    } else {
        cJSON_AddNullToObject(o, "running");
    }
    cJSON *arr = cJSON_AddArrayToObject(o, "skills");
    for (const skills::Entry &e : skills::all()) {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "file", e.file.c_str() + (e.file[0] == '/' ? 1 : 0));
        cJSON_AddStringToObject(s, "name", e.slug.c_str());
        cJSON_AddNumberToObject(s, "abi", e.abi);
        cJSON_AddBoolToObject(s, "autorun", e.autorun);
        cJSON_AddBoolToObject(s, "behaviour", e.behaviour);
        if (!e.provides_gait.empty()) {
            cJSON_AddStringToObject(s, "provides_movement", e.provides_gait.c_str());
        }
        cJSON_AddItemToArray(arr, s);
    }
    return { print_and_free(o), false };
}

Reply mpx_run_skill(const cJSON *args)
{
    const char *skill = str(args, "skill");
    if (!skill) {
        return { "Error: 'skill' is required. Call mpx_list_skills for the installed "
                 "file names.", true };
    }
    if (mpx_robot_pi_in_control()) return pi_refuses();
    const char *params = str(args, "params");
    std::string resolved;
    const char *why = network::skill_start(skill, params ? params : "", &resolved);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", why == nullptr);
    if (why) {
        cJSON_AddStringToObject(o, "skill", skill);
        cJSON_AddStringToObject(o, "error", why);
        if (network::skill_running()) {
            cJSON_AddStringToObject(o, "running", network::skill_running_name());
        }
    } else {
        cJSON_AddStringToObject(o, "started", resolved.c_str());
        /* Stay inside the run. The skill has the robot to itself (voice is
         * held, so no wake word and no voice turn), and letting the tool
         * return at once would send the model a result while the skill is
         * still moving, inviting a second tool call into a robot that is
         * busy -- and another LLM request behind it. Blocking here means the
         * next model turn starts only after the skill is done.
         *
         * 90 s: the sandbox's own per-run watchdog is 60 s, so this is the
         * watchdog's decision, not ours -- a skill that runs past it is
         * already being killed. mpx_stop_skill from another turn still
         * works, and gives the semaphore. */
        const bool finished = network::skill_wait(90000);
        cJSON_AddBoolToObject(o, "finished", finished);
        if (!finished && network::skill_running()) {
            cJSON_AddStringToObject(o, "still_running",
                                    network::skill_running_name());
            cJSON_AddStringToObject(
                o, "note",
                "The skill is still running. It has the robot and the "
                "microphone, so do not start another one; call "
                "mpx_stop_skill to stop it, or wait and ask again.");
        }
    }
    return { print_and_free(o), false };
}

Reply mpx_stop_skill(const cJSON *)
{
    const bool was = network::skill_running();
    network::skill_stop();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddBoolToObject(o, "was_running", was);
    return { print_and_free(o), false };
}

/* ── music ───────────────────────────────────────────────────────────── */

#if CONFIG_APP_VOICE_ENABLE
Reply music_play(const cJSON *args)
{
    const char *url = str(args, "url");
    const char *genre = str(args, "genre");
    const char *what = url ? url : genre;
    const bool matched = !url && what && cap_music_matches_station(what);
    char title[48] = {0};
    const esp_err_t err = cap_music_play(what, title, sizeof(title));
    if (err != ESP_OK) {
        return { fail(err == ESP_ERR_NOT_SUPPORTED ? "this robot has no speaker"
                                                   : esp_err_to_name(err)), false };
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "playing", title);
    cJSON_AddStringToObject(o, "source", url ? "url" : "SomaFM internet radio");
    cJSON_AddStringToObject(o, "note",
        url ? "starts as soon as you finish talking"
        : matched ? "a live radio station; starts as soon as you finish talking"
                  : "no station matched that, so a random one; starts as soon as you finish talking");
    return { print_and_free(o), false };
}

Reply music_stop(const cJSON *)
{
    const bool was = cap_music_active();
    cap_music_stop();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddBoolToObject(o, "was_playing", was);
    return { print_and_free(o), false };
}
#endif  /* CONFIG_APP_VOICE_ENABLE */

/* ── the table ───────────────────────────────────────────────────────── */

const Tool kTools[] = {
    { "robot_move", kGroupRobot,
      "Make the robot walk, turn, or perform a named movement. Either give a "
      "movement name from robot_get_state.movements, or drive it with "
      "forward/strafe/turn between -1 and 1.",
      nullptr,
      "{\"type\":\"object\",\"properties\":{"
      "\"movement\":{\"type\":\"string\",\"description\":"
      "\"A movement name, e.g. advance, stanford, lookup, twerk.\"},"
      "\"forward\":{\"type\":\"number\",\"minimum\":-1,\"maximum\":1},"
      "\"strafe\":{\"type\":\"number\",\"minimum\":-1,\"maximum\":1,"
      "\"description\":\"Positive is left.\"},"
      "\"turn\":{\"type\":\"number\",\"minimum\":-1,\"maximum\":1,"
      "\"description\":\"Positive turns left.\"},"
      "\"duration_ms\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":10000}}}",
      robot_move },
    { "robot_pose", kGroupRobot,
      "Hold a body attitude in degrees while the feet stay planted. Use it to "
      "make the robot look up, tilt its head, or lean. Roll is limited to 25, "
      "pitch to 20 and yaw to 30 degrees.",
      nullptr,
      "{\"type\":\"object\",\"properties\":{"
      "\"roll\":{\"type\":\"number\",\"minimum\":-25,\"maximum\":25},"
      "\"pitch\":{\"type\":\"number\",\"minimum\":-20,\"maximum\":20,"
      "\"description\":\"Positive raises the nose.\"},"
      "\"yaw\":{\"type\":\"number\",\"minimum\":-30,\"maximum\":30},"
      "\"speed_dps\":{\"type\":\"number\",\"minimum\":0,\"description\":"
      "\"Degrees per second. 0 snaps instantly.\"}}}",
      robot_pose },
    { "robot_get_state", kGroupRobot,
      "Read what the robot is doing: the current movement, gait settings, an "
      "IMU sample, the hottest servo, and every movement name that can be "
      "passed to robot_move. **Do not guess these values.**",
      nullptr,
      "{\"type\":\"object\",\"properties\":{}}",
      robot_get_state },
    { "robot_set_config", kGroupRobot,
      "Change how the robot walks and persist it. Every field is optional; "
      "omitted fields keep their current value. Read robot_get_state first so "
      "you are changing from what is actually set.",
      nullptr,
      "{\"type\":\"object\",\"properties\":{"
      "\"period\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":200,"
      "\"description\":\"Gait period in ms per phase. Lower is faster.\"},"
      "\"height\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":150,"
      "\"description\":\"Body height in mm.\"},"
      "\"up_height\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":50,"
      "\"description\":\"Foot lift in mm.\"},"
      "\"stride\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":50},"
      "\"tilt\":{\"type\":\"integer\",\"minimum\":-30,\"maximum\":30},"
      "\"speed_mm_s\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":200}}}",
      robot_set_config },
#if CONFIG_APP_DISPLAY_ENABLE
    { "display_show_text", kGroupDisplay,
      "Show a short TEXT message on the robot's screen, a few words, big.",
      nullptr,
      "{\"type\":\"object\",\"properties\":{"
      "\"text\":{\"type\":\"string\",\"maxLength\":120}},\"required\":[\"text\"]}",
      display_show_text },
    { "display_show_emotion", kGroupDisplay,
      "Show a face on the robot's screen: neutral, happy, excited, sad, sleepy, "
      "angry, surprised, love, confused, wink, dizzy, scared, pleading, "
      "determined, bliss, cool or curious. It holds until the next one; neutral "
      "hands the eyes back to the IMU. A reaction to what was said.",
      "Show a face on the robot's screen (neutral, happy, excited, sad, sleepy, "
      "angry, surprised, love, confused, wink, dizzy, scared, pleading, "
      "determined, bliss, cool, curious). Holds until the next; neutral frees the eyes.",
      "{\"type\":\"object\",\"properties\":{"
      "\"emotion\":{\"type\":\"string\",\"enum\":[\"neutral\",\"happy\",\"excited\","
      "\"sad\",\"sleepy\",\"angry\",\"surprised\",\"love\",\"confused\",\"wink\","
      "\"dizzy\",\"scared\",\"pleading\",\"determined\",\"bliss\",\"cool\","
      "\"curious\"]}},\"required\":[\"emotion\"]}",
      display_show_emotion },
    { "display_clear", kGroupDisplay,
      "Stop showing a message and go back to the robot's resting face.",
      nullptr,
      "{\"type\":\"object\",\"properties\":{}}",
      display_clear },
    { "display_draw", kGroupDisplay,
      "Draw a picture on the robot's 320x240 screen (0,0 = top left) from shapes: "
      "rect, circle, oval, line, polyline, triangle, polygon (convex), arc, text. "
      "Use it for anything that is not one of the faces: an animal, an object, a "
      "symbol, a number. It replaces the face for 'seconds' (default 20, 0 = until "
      "display_clear). Read the mpx_display_draw skill first.",
      "Draw a picture on the 320x240 screen ((0,0) top left) from shapes: rect, "
      "circle, oval, line, polyline, triangle, polygon, arc, text. For anything that "
      "is not a face. Read skill mpx_display_draw first.",
      "{\"type\":\"object\",\"properties\":{"
      "\"background\":{\"type\":\"string\",\"description\":\"colour name or #rrggbb, default black\"},"
      "\"seconds\":{\"type\":\"number\",\"minimum\":0,\"maximum\":600},"
      "\"shapes\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{"
      "\"type\":{\"type\":\"string\",\"enum\":[\"rect\",\"circle\",\"oval\",\"line\","
      "\"polyline\",\"triangle\",\"polygon\",\"arc\",\"text\"]},"
      "\"color\":{\"type\":\"string\"},\"border\":{\"type\":\"string\"},"
      "\"fill\":{\"type\":\"boolean\"},\"width\":{\"type\":\"number\"},"
      "\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},"
      "\"w\":{\"type\":\"number\"},\"h\":{\"type\":\"number\"},\"radius\":{\"type\":\"number\"},"
      "\"r\":{\"type\":\"number\"},\"rx\":{\"type\":\"number\"},\"ry\":{\"type\":\"number\"},"
      "\"start\":{\"type\":\"number\"},\"end\":{\"type\":\"number\"},"
      "\"points\":{\"type\":\"array\",\"items\":{\"type\":\"array\",\"items\":{\"type\":\"number\"}}},"
      "\"text\":{\"type\":\"string\"},"
      "\"size\":{\"type\":\"string\",\"enum\":[\"small\",\"medium\",\"large\"]}},"
      "\"required\":[\"type\"]}}},\"required\":[\"shapes\"]}",
      display_draw },
    { "display_set_colors", kGroupDisplay,
      "Change the colours of the robot's face: the screen background and/or the "
      "eyes. Use this when someone asks to make the screen or background a "
      "colour, or to change the eye colour. Colour names (black, white, red, "
      "green, blue, yellow, orange, purple, pink, cyan, grey) or hex like "
      "#00ff00. The default is yellow eyes on black.",
      "Change the robot's face colours: screen background and/or eyes. Colour "
      "names (black, white, red, green, blue, yellow, orange, purple, pink, cyan, "
      "grey) or hex like #00ff00. Default: yellow eyes on black.",
      "{\"type\":\"object\",\"properties\":{"
      "\"background\":{\"type\":\"string\",\"description\":\"screen background colour\"},"
      "\"eyes\":{\"type\":\"string\",\"description\":\"eye colour\"}}}",
      display_set_colors },
    { "display_set_brightness", kGroupDisplay,
      "Set the screen backlight from 0 to 100 percent. 0 turns it off, which is "
      "the polite thing to do when someone says the robot should sleep.",
      nullptr,
      "{\"type\":\"object\",\"properties\":{"
      "\"percent\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100}},"
      "\"required\":[\"percent\"]}",
      display_set_brightness },
#endif
    { "mpx_list_skills", kGroupSkill,
      "List the WebAssembly robot skills installed on this device, which one is "
      "running, and any extra movement names they add for robot_move. **Do not "
      "guess skill names.**",
      nullptr,
      "{\"type\":\"object\",\"properties\":{}}",
      mpx_list_skills },
    { "mpx_run_skill", kGroupSkill,
      "Run an installed robot skill by file name. Only one runs at a time, and "
      "voice is paused while it runs. This call returns when the skill has "
      "finished. Get names from mpx_list_skills first.",
      nullptr,
      "{\"type\":\"object\",\"properties\":{"
      "\"skill\":{\"type\":\"string\",\"description\":\"File name, e.g. moonwalk.wasm\"},"
      "\"params\":{\"type\":\"string\",\"description\":"
      "\"Optional name=value;name=value string the skill can read.\"}},"
      "\"required\":[\"skill\"]}",
      mpx_run_skill },
    { "mpx_stop_skill", kGroupSkill,
      "Stop the running robot skill. Cooperative: the skill gets to clean up, so "
      "the robot settles rather than dropping.",
      nullptr,
      "{\"type\":\"object\",\"properties\":{}}",
      mpx_stop_skill },
#if CONFIG_APP_VOICE_ENABLE
    { "music_play", kGroupMusic,
      "Play music on the robot's speaker until music_stop. Give a genre or mood "
      "(chill, pop, indie, 80s, 70s, jazz, lounge, soul, funk, hip hop, house, "
      "country, folk, reggae, metal, trance, ambient, sleep, covers) to pick an "
      "internet radio station, nothing for a random one, or `url`: a direct link "
      "to an audio stream or file (mp3, aac, m4a, flac, wav). It plays radio, not "
      "a named song on demand; for a specific song you need a direct audio URL. "
      "Call it again to change station.",
      "Play music on the robot's speaker until music_stop: a genre or mood "
      "(jazz, chill, pop, 80s, ambient...) picks an internet radio station, "
      "nothing picks a random one, or url: a direct audio link. Radio, not songs "
      "on demand.",
      "{\"type\":\"object\",\"properties\":{"
      "\"genre\":{\"type\":\"string\",\"description\":\"Genre or mood in English.\"},"
      "\"url\":{\"type\":\"string\",\"description\":\"Direct http(s) link to audio.\"}}}",
      music_play },
    { "music_stop", kGroupMusic,
      "Stop the music playing on the robot's speaker.",
      nullptr,
      "{\"type\":\"object\",\"properties\":{}}",
      music_stop },
#endif
};
constexpr size_t kToolCount = sizeof(kTools) / sizeof(kTools[0]);

}  // namespace

const Tool *all(size_t *count)
{
    if (count) *count = kToolCount;
    return kTools;
}

const Tool *find(const char *name)
{
    if (!name) return nullptr;
    /* Tolerate xiaozhi's "self." prefix in case the server remembered it. */
    if (std::strncmp(name, "self.", 5) == 0) name += 5;
    for (const Tool &t : kTools) {
        if (std::strcmp(t.name, name) == 0) return &t;
    }
    return nullptr;
}

const char *claw_description(const Tool &t)
{
    return t.brief ? t.brief : t.description;
}

cJSON *descriptor(const Tool &t)
{
    cJSON *tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "name", t.name);
    cJSON_AddStringToObject(tool, "description", t.description);
    cJSON *schema = cJSON_Parse(t.schema);
    cJSON_AddItemToObject(tool, "inputSchema",
                          schema ? schema : cJSON_Parse("{\"type\":\"object\",\"properties\":{}}"));
    return tool;
}

Reply run(const Tool &t, const cJSON *args)
{
    return t.run(cJSON_IsObject(args) ? args : nullptr);
}

Reply call(const char *name, const char *arguments_json)
{
    const Tool *tool = find(name);
    if (!tool) {
        return { "unknown tool", true };
    }
    cJSON *args = (arguments_json && arguments_json[0]) ? cJSON_Parse(arguments_json) : nullptr;
    const Reply r = run(*tool, args);
    cJSON_Delete(args);
    return r;
}

}  // namespace robot_tools
