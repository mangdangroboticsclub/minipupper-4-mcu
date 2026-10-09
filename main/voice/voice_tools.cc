/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * See voice_tools.h. Wire format and paging follow mpv2-claw's mpx_mcp_ws.c;
 * the tool bodies follow its cap_robot, cap_display, cap_mpx_skill and
 * cap_music descriptors, so a prompt tuned against mpv2 behaves the same.
 */

#include "voice_tools.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "cap_music.h"
#include "mpx_voice_link.h"
#include "mpx_robot.h"
#include "network/http_server.h"
#include "wasm/registry.h"

#if CONFIG_APP_DISPLAY_ENABLE
#include "cap_display.h"
#endif

static const char *TAG = "voice_tools";

namespace {

/* Smaller than xiaozhi's own 8000 -- see mpx_mcp_ws.c: a tools/list page is
 * the largest message this device sends, on a freshly opened TLS socket. */
constexpr size_t kListPageBytes = 4000;
constexpr int    kInboxDepth    = 8;
constexpr int    kTaskStack     = 8192;    /* internal RAM, see the header */
constexpr int    kTaskPrio      = 4;
constexpr float  kMaxRollDeg    = 25.0f;
constexpr float  kMaxPitchDeg   = 20.0f;
constexpr float  kMaxYawDeg     = 30.0f;
constexpr int    kMaxDriveMs    = 10000;
constexpr const char *kProtocolVersion = "2024-11-05";

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

/* A tool reply: text for the model, plus whether MCP should flag it as an
 * error (which makes the model retry or apologise -- so "no such movement"
 * is information, not an error, exactly as mpv2's cap_robot decides). */
struct Reply {
    std::string text;
    bool error = false;
};

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

/* ── the table ───────────────────────────────────────────────────────── */

struct Tool {
    const char *name;
    const char *description;
    const char *schema;     /* JSON Schema for the arguments */
    Reply (*run)(const cJSON *args);
};

const Tool kTools[] = {
    { "robot_move",
      "Make the robot walk, turn, or perform a named movement. Either give a "
      "movement name from robot_get_state.movements, or drive it with "
      "forward/strafe/turn between -1 and 1.",
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
    { "robot_pose",
      "Hold a body attitude in degrees while the feet stay planted. Use it to "
      "make the robot look up, tilt its head, or lean. Roll is limited to 25, "
      "pitch to 20 and yaw to 30 degrees.",
      "{\"type\":\"object\",\"properties\":{"
      "\"roll\":{\"type\":\"number\",\"minimum\":-25,\"maximum\":25},"
      "\"pitch\":{\"type\":\"number\",\"minimum\":-20,\"maximum\":20,"
      "\"description\":\"Positive raises the nose.\"},"
      "\"yaw\":{\"type\":\"number\",\"minimum\":-30,\"maximum\":30},"
      "\"speed_dps\":{\"type\":\"number\",\"minimum\":0,\"description\":"
      "\"Degrees per second. 0 snaps instantly.\"}}}",
      robot_pose },
    { "robot_get_state",
      "Read what the robot is doing: the current movement, gait settings, an "
      "IMU sample, the hottest servo, and every movement name that can be "
      "passed to robot_move. **Do not guess these values.**",
      "{\"type\":\"object\",\"properties\":{}}",
      robot_get_state },
    { "robot_set_config",
      "Change how the robot walks and persist it. Every field is optional; "
      "omitted fields keep their current value. Read robot_get_state first so "
      "you are changing from what is actually set.",
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
    { "display_show_text",
      "Show a short TEXT message on the robot's screen, a few words, big.",
      "{\"type\":\"object\",\"properties\":{"
      "\"text\":{\"type\":\"string\",\"maxLength\":120}},\"required\":[\"text\"]}",
      display_show_text },
    { "display_show_emotion",
      "Show a face on the robot's screen: neutral, happy, excited, sad, sleepy, "
      "angry, surprised, love, confused, wink, dizzy, scared, pleading, "
      "determined, bliss, cool or curious. It holds until the next one; neutral "
      "hands the eyes back to the IMU. A reaction to what was said.",
      "{\"type\":\"object\",\"properties\":{"
      "\"emotion\":{\"type\":\"string\",\"enum\":[\"neutral\",\"happy\",\"excited\","
      "\"sad\",\"sleepy\",\"angry\",\"surprised\",\"love\",\"confused\",\"wink\","
      "\"dizzy\",\"scared\",\"pleading\",\"determined\",\"bliss\",\"cool\","
      "\"curious\"]}},\"required\":[\"emotion\"]}",
      display_show_emotion },
    { "display_clear",
      "Stop showing a message and go back to the robot's resting face.",
      "{\"type\":\"object\",\"properties\":{}}",
      display_clear },
    { "display_set_colors",
      "Change the colours of the robot's face: the screen background and/or the "
      "eyes. Use this when someone asks to make the screen or background a "
      "colour, or to change the eye colour. Colour names (black, white, red, "
      "green, blue, yellow, orange, purple, pink, cyan, grey) or hex like "
      "#00ff00. The default is yellow eyes on black.",
      "{\"type\":\"object\",\"properties\":{"
      "\"background\":{\"type\":\"string\",\"description\":\"screen background colour\"},"
      "\"eyes\":{\"type\":\"string\",\"description\":\"eye colour\"}}}",
      display_set_colors },
    { "display_set_brightness",
      "Set the screen backlight from 0 to 100 percent. 0 turns it off, which is "
      "the polite thing to do when someone says the robot should sleep.",
      "{\"type\":\"object\",\"properties\":{"
      "\"percent\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100}},"
      "\"required\":[\"percent\"]}",
      display_set_brightness },
#endif
    { "mpx_list_skills",
      "List the WebAssembly robot skills installed on this device, which one is "
      "running, and any extra movement names they add for robot_move. **Do not "
      "guess skill names.**",
      "{\"type\":\"object\",\"properties\":{}}",
      mpx_list_skills },
    { "mpx_run_skill",
      "Run an installed robot skill by file name. Only one runs at a time. Get "
      "names from mpx_list_skills first.",
      "{\"type\":\"object\",\"properties\":{"
      "\"skill\":{\"type\":\"string\",\"description\":\"File name, e.g. moonwalk.wasm\"},"
      "\"params\":{\"type\":\"string\",\"description\":"
      "\"Optional name=value;name=value string the skill can read.\"}},"
      "\"required\":[\"skill\"]}",
      mpx_run_skill },
    { "mpx_stop_skill",
      "Stop the running robot skill. Cooperative: the skill gets to clean up, so "
      "the robot settles rather than dropping.",
      "{\"type\":\"object\",\"properties\":{}}",
      mpx_stop_skill },
    { "music_play",
      "Play music on the robot's speaker until music_stop. Give a genre or mood "
      "(chill, pop, indie, 80s, 70s, jazz, lounge, soul, funk, hip hop, house, "
      "country, folk, reggae, metal, trance, ambient, sleep, covers) to pick an "
      "internet radio station, nothing for a random one, or `url`: a direct link "
      "to an audio stream or file (mp3, aac, m4a, flac, wav). It plays radio, not "
      "a named song on demand; for a specific song you need a direct audio URL. "
      "Call it again to change station.",
      "{\"type\":\"object\",\"properties\":{"
      "\"genre\":{\"type\":\"string\",\"description\":\"Genre or mood in English.\"},"
      "\"url\":{\"type\":\"string\",\"description\":\"Direct http(s) link to audio.\"}}}",
      music_play },
    { "music_stop",
      "Stop the music playing on the robot's speaker.",
      "{\"type\":\"object\",\"properties\":{}}",
      music_stop },
};
constexpr size_t kToolCount = sizeof(kTools) / sizeof(kTools[0]);

const Tool *find_tool(const char *wire_name)
{
    if (!wire_name) return nullptr;
    /* Tolerate xiaozhi's "self." prefix in case the server remembered it. */
    if (std::strncmp(wire_name, "self.", 5) == 0) wire_name += 5;
    for (const Tool &t : kTools) {
        if (std::strcmp(t.name, wire_name) == 0) return &t;
    }
    return nullptr;
}

cJSON *tool_descriptor(const Tool &t)
{
    cJSON *tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "name", t.name);
    cJSON_AddStringToObject(tool, "description", t.description);
    cJSON *schema = cJSON_Parse(t.schema);
    cJSON_AddItemToObject(tool, "inputSchema",
                          schema ? schema : cJSON_Parse("{\"type\":\"object\",\"properties\":{}}"));
    return tool;
}

/* ── JSON-RPC ────────────────────────────────────────────────────────── */

cJSON *rpc_reply(const cJSON *id)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id) {
        cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, true));
    } else {
        cJSON_AddNullToObject(resp, "id");
    }
    return resp;
}

cJSON *rpc_error(const cJSON *id, int code, const char *message)
{
    cJSON *resp = rpc_reply(id);
    cJSON *err = cJSON_AddObjectToObject(resp, "error");
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    return resp;
}

cJSON *handle_initialize(const cJSON *id, const cJSON *params)
{
    /* Echo the client's protocol version when it gives one -- the reference
     * engine does, and a mismatch here is what makes some servers drop the
     * MCP session silently. */
    const char *proto = str(params, "protocolVersion");
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON *resp = rpc_reply(id);
    cJSON *result = cJSON_AddObjectToObject(resp, "result");
    cJSON_AddStringToObject(result, "protocolVersion", proto ? proto : kProtocolVersion);
    cJSON *caps = cJSON_AddObjectToObject(result, "capabilities");
    cJSON_AddObjectToObject(caps, "tools");
    cJSON *info = cJSON_AddObjectToObject(result, "serverInfo");
    cJSON_AddStringToObject(info, "name", "mpx-dog");
    cJSON_AddStringToObject(info, "version", app ? app->version : "0");
    return resp;
}

cJSON *handle_tools_list(const cJSON *id, const cJSON *params)
{
    const char *start = str(params, "cursor");
    if (start && std::strncmp(start, "self.", 5) == 0) start += 5;

    cJSON *resp = rpc_reply(id);
    cJSON *result = cJSON_AddObjectToObject(resp, "result");
    cJSON *tools = cJSON_AddArrayToObject(result, "tools");

    size_t bytes = 0;
    bool started = (start == nullptr);
    for (const Tool &t : kTools) {
        if (!started) {
            started = std::strcmp(t.name, start) == 0;
            if (!started) continue;
        }
        cJSON *tool = tool_descriptor(t);
        char *one = cJSON_PrintUnformatted(tool);
        const size_t len = one ? std::strlen(one) + 1 : 0;
        cJSON_free(one);
        /* Always at least one tool per page, or a giant schema would stall
         * the server's walk forever on the same cursor. */
        if (bytes > 0 && bytes + len > kListPageBytes) {
            cJSON_Delete(tool);
            cJSON_AddStringToObject(result, "nextCursor", t.name);
            break;
        }
        cJSON_AddItemToArray(tools, tool);
        bytes += len;
    }
    ESP_LOGI(TAG, "tools/list: %d tool(s)%s%s", cJSON_GetArraySize(tools),
             start ? " from " : "", start ? start : "");
    return resp;
}

cJSON *handle_tools_call(const cJSON *id, const cJSON *params)
{
    const char *name = str(params, "name");
    if (!name) return rpc_error(id, -32602, "params.name is required");
    const Tool *tool = find_tool(name);
    if (!tool) return rpc_error(id, -32602, "unknown tool");

    const cJSON *arguments = params ? cJSON_GetObjectItem(params, "arguments") : nullptr;
    if (arguments && !cJSON_IsObject(arguments)) arguments = nullptr;
    ESP_LOGI(TAG, "tools/call %s", tool->name);
    const Reply r = tool->run(arguments);

    cJSON *resp = rpc_reply(id);
    cJSON *result = cJSON_AddObjectToObject(resp, "result");
    cJSON *content = cJSON_AddArrayToObject(result, "content");
    cJSON *text = cJSON_CreateObject();
    cJSON_AddStringToObject(text, "type", "text");
    cJSON_AddStringToObject(text, "text", r.text.c_str());
    cJSON_AddItemToArray(content, text);
    cJSON_AddBoolToObject(result, "isError", r.error);
    return resp;
}

/* One JSON-RPC message in, the reply (heap, caller frees) or nullptr out --
 * nullptr for notifications, which get no answer. */
char *dispatch(const char *msg)
{
    cJSON *root = cJSON_Parse(msg);
    if (!root) {
        ESP_LOGW(TAG, "unparseable JSON-RPC (%u bytes)", (unsigned)std::strlen(msg));
        return nullptr;
    }
    const cJSON *method = cJSON_GetObjectItem(root, "method");
    const cJSON *id = cJSON_GetObjectItem(root, "id");
    const cJSON *params = cJSON_GetObjectItem(root, "params");
    cJSON *resp = nullptr;

    if (!cJSON_IsString(method)) {
        /* A response to something we sent? We send nothing, so: ignore. */
        if (!id) resp = rpc_error(nullptr, -32600, "invalid request");
    } else if (std::strncmp(method->valuestring, "notifications/", 14) == 0) {
        resp = nullptr;
    } else if (std::strcmp(method->valuestring, "initialize") == 0) {
        resp = handle_initialize(id, params);
    } else if (std::strcmp(method->valuestring, "ping") == 0) {
        resp = rpc_reply(id);
        cJSON_AddObjectToObject(resp, "result");
    } else if (std::strcmp(method->valuestring, "tools/list") == 0) {
        resp = handle_tools_list(id, params);
    } else if (std::strcmp(method->valuestring, "tools/call") == 0) {
        resp = handle_tools_call(id, params);
    } else {
        ESP_LOGW(TAG, "method not supported: %s", method->valuestring);
        resp = rpc_error(id, -32601, "method not found");
    }
    cJSON_Delete(root);

    char *out = resp ? cJSON_PrintUnformatted(resp) : nullptr;
    cJSON_Delete(resp);
    return out;
}

esp_err_t send_envelope(const char *jsonrpc)
{
    const char *sid = mpx_voice_session_id();
    const size_t need = std::strlen(jsonrpc) + (sid ? std::strlen(sid) : 0) + 48;
    char *buf = (char *)malloc(need);
    if (!buf) return ESP_ERR_NO_MEM;
    std::snprintf(buf, need, "{\"session_id\":\"%s\",\"type\":\"mcp\",\"payload\":%s}",
                  sid ? sid : "", jsonrpc);
    const esp_err_t err = mpx_voice_send_json(buf);
    free(buf);
    return err;
}

/* ── the task ────────────────────────────────────────────────────────── */

QueueHandle_t s_inbox;

void tools_task(void *)
{
    for (;;) {
        char *msg = nullptr;
        if (xQueueReceive(s_inbox, &msg, portMAX_DELAY) != pdTRUE || !msg) continue;
        char *reply = dispatch(msg);
        if (reply) {
            if (send_envelope(reply) != ESP_OK) {
                ESP_LOGW(TAG, "could not send MCP reply -- socket down?");
            }
            cJSON_free(reply);
        }
        free(msg);
    }
}

/* mpx_voice_link's sink: runs on the websocket task, so only queue. The
 * string is ours to free -- by the task, or here if the queue is full. */
void mcp_incoming(const char *jsonrpc_owned)
{
    if (!jsonrpc_owned) return;
    if (!s_inbox || xQueueSend(s_inbox, &jsonrpc_owned, 0) != pdTRUE) {
        ESP_LOGW(TAG, "MCP inbox full -- dropping message");
        free((void *)jsonrpc_owned);
    }
}

}  // namespace

namespace voice_tools {

esp_err_t start()
{
    if (s_inbox) return ESP_OK;
    s_inbox = xQueueCreate(kInboxDepth, sizeof(char *));
    if (!s_inbox) return ESP_ERR_NO_MEM;
    if (xTaskCreate(tools_task, "voice_tools", kTaskStack, nullptr, kTaskPrio, nullptr) != pdPASS) {
        vQueueDelete(s_inbox);
        s_inbox = nullptr;
        return ESP_ERR_NO_MEM;
    }
    mpx_voice_set_mcp_sink(mcp_incoming);
    ESP_LOGI(TAG, "%u tools offered to the voice server", (unsigned)kToolCount);
    return ESP_OK;
}

std::string list_json()
{
    cJSON *arr = cJSON_CreateArray();
    for (const Tool &t : kTools) {
        cJSON_AddItemToArray(arr, tool_descriptor(t));
    }
    return print_and_free(arr);
}

std::string call(const char *name, const char *arguments_json, bool *is_error)
{
    const Tool *tool = find_tool(name);
    if (!tool) {
        if (is_error) *is_error = true;
        return "unknown tool";
    }
    cJSON *args = (arguments_json && arguments_json[0]) ? cJSON_Parse(arguments_json) : nullptr;
    const Reply r = tool->run(cJSON_IsObject(args) ? args : nullptr);
    cJSON_Delete(args);
    if (is_error) *is_error = r.error;
    return r.text;
}

}  // namespace voice_tools
