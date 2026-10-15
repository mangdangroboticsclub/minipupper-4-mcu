/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * The /v1/robot routes -- the movement and calibration half of the API the MPX-Dog PWA
 * speaks. Ported from main/network/http_server.cc.
 *
 * The response shapes are a contract, not a preference: pwa-redesign reads
 * these exact field names. `data.config` seeds the home-screen sliders,
 * `offsets[]` drives the calibration screen. Renaming a key here breaks a
 * screen over there with no compile error in between.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "http_server_priv.h"
#include "mpx_robot.h"
#include "mpx_wasm.h"
#include "sdkconfig.h"

/* mpv2-uros: the RL walk policy on the ESP32-S3 (components/mpx_policy). */
#if defined(CONFIG_MP4_POLICY_ENABLE) && __has_include("mpx_policy.h")
#include "mpx_policy.h"
#define HAVE_MPX_POLICY 1
#else
#define HAVE_MPX_POLICY 0
#endif

/* The Pi link (mpx_pi_link), when it is in the build. Weak so this file
 * builds without it, the same way http_server_mpx_wifi_api.c reads the Pi's
 * address -- no include, no component dependency. */
extern const char *mpx_pi_link_state_str(void) __attribute__((weak));
extern int mpx_pi_link_status_json(char *buf, size_t size) __attribute__((weak));

static const char *TAG = "http_mpx_robot";

/* 423 while the Raspberry Pi (ROS) drives the robot: mpx_robot fences local
 * motion off anyway, and a clear answer beats a move that never happens. */
static esp_err_t send_pi_in_control(httpd_req_t *req)
{
    httpd_resp_set_status(req, "423 Locked");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"pi_in_control\","
                                   "\"detail\":\"The Raspberry Pi (ROS) is controlling the robot\"}");
}

/* ── POST /v1/robot/gait ───────────────────────────────────────────────── */

static esp_err_t robot_gait_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    cJSON *resp = NULL;
    char mode[48] = {0};

    if (http_server_parse_json_body(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }
    http_server_json_read_string(body, "mode", mode, sizeof(mode));
    cJSON_Delete(body);

    if (mode[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing mode");
    }
    if (mpx_robot_pi_in_control()) {
        return send_pi_in_control(req);
    }

    /* Through the movement resolver, not the gait table: a name provided by an
     * installed skill has to work here too. Built-ins still win. */
    const mpx_movement_result_t r = mpx_wasm_movement_run(mode, false);

    resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    http_server_json_add_string(resp, "mode", mode);

    if (r == MPX_MOVEMENT_STARTED) {
        cJSON_AddBoolToObject(resp, "ok", true);
        return http_server_send_json_response(req, resp);
    }

    cJSON_AddBoolToObject(resp, "ok", false);
    http_server_json_add_string(resp, "error", mpx_wasm_movement_result_text(r));

    /* The PWA branches on the status code: 404 means "no such movement, tell
     * the user", 409 means "busy, offer to stop the skill". Collapsing both to
     * 400 would make a correct answer look like a broken robot. */
    httpd_resp_set_status(req, (r == MPX_MOVEMENT_UNKNOWN) ? "404 Not Found" : "409 Conflict");
    return http_server_send_json_response(req, resp);
}

/* ── POST /v1/robot/joy ────────────────────────────────────────────────── */

static double json_num(const cJSON *root, const char *key, double fallback)
{
    const cJSON *item = cJSON_GetObjectItem(root, key);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

static esp_err_t robot_joy_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    cJSON *resp = NULL;
    float f, s, t;

    if (http_server_parse_json_body(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }
    f = (float)json_num(body, "f", 0.0);
    s = (float)json_num(body, "s", 0.0);
    t = (float)json_num(body, "t", 0.0);
    cJSON_Delete(body);

    if (mpx_robot_pi_in_control()) {
        return send_pi_in_control(req);
    }
    mpx_robot_drive(f, s, t);

    resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    return http_server_send_json_response(req, resp);
}

/* ── GET /v1/robot/status ──────────────────────────────────────────────── */

static void add_config_object(cJSON *parent, const mpx_robot_config_t *cfg)
{
    cJSON *c = cJSON_CreateObject();
    if (!c) {
        return;
    }
    cJSON_AddNumberToObject(c, "period",    cfg->period);
    cJSON_AddNumberToObject(c, "height",    cfg->height);
    cJSON_AddNumberToObject(c, "up_height", cfg->up_height);
    cJSON_AddNumberToObject(c, "stride",    cfg->stride);
    cJSON_AddNumberToObject(c, "tilt",      cfg->tilt);
    cJSON_AddNumberToObject(c, "sg_speed",  cfg->sg_speed);
    cJSON_AddItemToObject(parent, "config", c);
}

static esp_err_t robot_status_handler(httpd_req_t *req)
{
    mpx_robot_config_t cfg;
    cJSON *resp = cJSON_CreateObject();
    cJSON *offsets = NULL;

    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    mpx_robot_get_config(&cfg);
    http_server_json_add_string(resp, "mode", mpx_robot_current_gait_name());
    add_config_object(resp, &cfg);

    offsets = cJSON_AddArrayToObject(resp, "offsets");
    if (offsets) {
        for (int id = 1; id <= 12; ++id) {
            cJSON_AddItemToArray(offsets, cJSON_CreateNumber(mpx_robot_get_offset(id)));
        }
    }
    /* mpv2's field: "standalone", "monitor" or "slave". */
    http_server_json_add_string(resp, "pi_link",
                                mpx_pi_link_state_str ? mpx_pi_link_state_str() : "standalone");
    return http_server_send_json_response(req, resp);
}

/* ── GET /v1/pi/status ─────────────────────────────────────────────────────
 * The Raspberry Pi link, for the app and for debugging the wiring: host_pin
 * shows the yellow wire (IO3), heartbeat the UART. Same fields as mpv2. */
static esp_err_t pi_status_handler(httpd_req_t *req)
{
    char buf[1024];

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (!mpx_pi_link_status_json || mpx_pi_link_status_json(buf, sizeof(buf)) < 0) {
        return httpd_resp_sendstr(req, "{\"state\":\"standalone\",\"in_control\":false,"
                                       "\"error\":\"pi link not in this build\"}");
    }
    return httpd_resp_sendstr(req, buf);
}

/* ── GET / POST /v1/robot/config ───────────────────────────────────────── */

static esp_err_t robot_config_get_handler(httpd_req_t *req)
{
    mpx_robot_config_t cfg;
    cJSON *resp = cJSON_CreateObject();

    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    mpx_robot_get_config(&cfg);
    cJSON_AddNumberToObject(resp, "period",    cfg.period);
    cJSON_AddNumberToObject(resp, "height",    cfg.height);
    cJSON_AddNumberToObject(resp, "up_height", cfg.up_height);
    cJSON_AddNumberToObject(resp, "stride",    cfg.stride);
    cJSON_AddNumberToObject(resp, "tilt",      cfg.tilt);
    cJSON_AddNumberToObject(resp, "sg_speed",  cfg.sg_speed);
    return http_server_send_json_response(req, resp);
}

static esp_err_t robot_config_post_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    cJSON *resp = NULL;
    mpx_robot_config_t cfg;

    if (http_server_parse_json_body(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }

    /* Read-modify-write. The PWA's sliders post only what moved, and `tilt`
     * legitimately goes negative -- so "absent" cannot be encoded as -1 the
     * way the original firmware did it. */
    mpx_robot_get_config(&cfg);
    cfg.period    = (int)json_num(body, "period",    cfg.period);
    cfg.height    = (int)json_num(body, "height",    cfg.height);
    cfg.up_height = (int)json_num(body, "up_height", cfg.up_height);
    cfg.stride    = (int)json_num(body, "stride",    cfg.stride);
    cfg.tilt      = (int)json_num(body, "tilt",      cfg.tilt);
    cfg.sg_speed  = (int)json_num(body, "sg_speed",  cfg.sg_speed);
    cJSON_Delete(body);

    mpx_robot_set_config(&cfg);

    resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    return http_server_send_json_response(req, resp);
}

/* ── Calibration ───────────────────────────────────────────────────────── */

static esp_err_t robot_calibrate_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    cJSON *resp = NULL;
    int servo;
    float offset;

    if (http_server_parse_json_body(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }
    servo  = (int)json_num(body, "servo", 0);
    offset = (float)json_num(body, "offset", 0.0);
    cJSON_Delete(body);

    if (servo < 1 || servo > 12) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "servo must be 1-12");
    }

    mpx_robot_set_offset(servo, offset);

    resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "servo", servo);
    cJSON_AddNumberToObject(resp, "offset", offset);
    return http_server_send_json_response(req, resp);
}

static esp_err_t robot_calibrate_reset_handler(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();

    mpx_robot_reset_offsets();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    return http_server_send_json_response(req, resp);
}

/* ── POST /v1/robot/diagnostic/ping ────────────────────────────────────── */

static esp_err_t robot_ping_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    cJSON *resp = NULL;
    int servo, result;

    if (http_server_parse_json_body(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }
    servo = (int)json_num(body, "servo", 1);
    cJSON_Delete(body);

    if (servo < 1 || servo > 12) {
        servo = 1;
    }
    result = mpx_robot_ping_servo(servo);

    resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", result > 0);
    cJSON_AddNumberToObject(resp, "servo", servo);
    cJSON_AddNumberToObject(resp, result > 0 ? "model" : "error", result);
    return http_server_send_json_response(req, resp);
}

/* ── GET /v1/robot/movements ───────────────────────────────────────────── */

static esp_err_t robot_movements_handler(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON *arr = NULL;

    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    arr = cJSON_AddArrayToObject(resp, "movements");
    if (!arr) {
        cJSON_Delete(resp);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    for (int i = 0; i < mpx_robot_gait_name_count(); ++i) {
        const char *name = mpx_robot_gait_name_at(i);
        cJSON *item = NULL;
        if (!name) {
            continue;
        }
        item = cJSON_CreateObject();
        if (!item) {
            continue;
        }
        http_server_json_add_string(item, "name", name);
        http_server_json_add_string(item, "source", "builtin");
        cJSON_AddItemToArray(arr, item);
    }

    /* Skill-provided movement names, merged in. This is the only endpoint that
     * reports both, which is why AddActionView should be using it rather than
     * browsing the filesystem. */
    for (int i = 0; i < mpx_wasm_skill_count(); ++i) {
        const char *file = NULL, *slug = NULL, *gait = NULL;
        bool behaviour = false;
        cJSON *item = NULL;
        if (!mpx_wasm_skill_at(i, &file, &slug, &gait, NULL, NULL, &behaviour)) {
            continue;
        }
        if (!gait || gait[0] == '\0') {
            continue;
        }
        item = cJSON_CreateObject();
        if (!item) {
            continue;
        }
        http_server_json_add_string(item, "name", gait);
        http_server_json_add_string(item, "source", "skill");
        http_server_json_add_string(item, "skill", slug ? slug : "");
        cJSON_AddBoolToObject(item, "behaviour", behaviour);
        cJSON_AddItemToArray(arr, item);
    }

    return http_server_send_json_response(req, resp);
}

/* ── GET /v1/robot/look?yaw=Y&pitch=P ──────────────────────────────────────
 *
 * The analog look pad from mpv2 (minipupperesp's /look). Degrees: yaw -30
 * (left) .. +30 (right), pitch -20 (down) .. +20 (up); the head glides there,
 * and 0/0 glides back to the stand and ends the look. The pad sends this about
 * every 80 ms while held.
 *
 * Missing before, and the miss was invisible: an unknown extensionless path
 * fell through to the PWA catch-all and came back as index.html with 200, so
 * a PWA that probes this endpoint (and falls back to the eight discrete look
 * gaits on 404) saw "supported" and sent its look into a web page. Walking and
 * stretch use /v1/robot/joy and /v1/robot/gait, which exist, so only the head
 * moves were dead. */

static float query_float(httpd_req_t *req, const char *key, float fallback)
{
    char raw[24] = {0};
    if (http_server_query_get(req, key, raw, sizeof(raw)) != ESP_OK || raw[0] == '\0') {
        return fallback;
    }
    return strtof(raw, NULL);
}

static esp_err_t robot_look_handler(httpd_req_t *req)
{
    if (mpx_robot_pi_in_control()) {
        return send_pi_in_control(req);
    }
    float yaw = query_float(req, "yaw", 0.0f);
    float pitch = query_float(req, "pitch", 0.0f);
    if (!isfinite(yaw)) {
        yaw = 0.0f;
    }
    if (!isfinite(pitch)) {
        pitch = 0.0f;
    }
    mpx_robot_look(yaw, pitch);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* ── GET /v1/robot/moves ───────────────────────────────────────────────────
 *
 * mpv2's move list: {"moves":[{"name","note","continuous"}]}. The PWA uses
 * `continuous` to decide whether releasing a button should send "none" -- a
 * jump ends by itself and must not be cut short, a walk or a held pose must
 * stop. Built-in gaits only; /v1/robot/movements adds skill-provided ones. */

static esp_err_t robot_moves_handler(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "moves") : NULL;
    if (!arr) {
        cJSON_Delete(resp);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    for (int i = 0; i < mpx_robot_gait_name_count(); ++i) {
        const char *name = mpx_robot_gait_name_at(i);
        cJSON *item = name ? cJSON_CreateObject() : NULL;
        if (!item) {
            continue;
        }
        http_server_json_add_string(item, "name", name);
        http_server_json_add_string(item, "note", mpx_robot_gait_note(name));
        cJSON_AddBoolToObject(item, "continuous", mpx_robot_gait_is_continuous(name));
        cJSON_AddItemToArray(arr, item);
    }
    return http_server_send_json_response(req, resp);
}

/* ── /v1/robot/power ───────────────────────────────────────────────────────
 *
 * GET  -> {"ok":true,"rest_after_s":300,"state":"holding","resting":false}
 * POST {"rest_after_s":N}   0 = never rest; saved on the robot
 * POST {"action":"rest"}    lie down and switch the servos off now
 * POST {"action":"wake"}    stand back up
 * Every POST answers with the same body as GET, after the change. */

static esp_err_t robot_power_reply(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "rest_after_s", mpx_robot_rest_after());
    http_server_json_add_string(resp, "state", mpx_robot_power_state());
    cJSON_AddBoolToObject(resp, "resting", mpx_robot_servos_resting());
    return http_server_send_json_response(req, resp);
}

static esp_err_t robot_power_get_handler(httpd_req_t *req)
{
    return robot_power_reply(req);
}

static esp_err_t robot_power_post_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    char action[16] = "";

    if (http_server_parse_json_body(req, &body) != ESP_OK || !body) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON body");
    }
    if (cJSON_IsNumber(cJSON_GetObjectItem(body, "rest_after_s"))) {
        double s = json_num(body, "rest_after_s", 300);
        mpx_robot_set_rest_after((uint32_t)(s < 0 ? 0 : s > 86400 ? 86400 : s));
    }
    http_server_json_read_string(body, "action", action, sizeof(action));
    cJSON_Delete(body);
    if (strcmp(action, "rest") == 0) {
        mpx_robot_rest_now();
    } else if (strcmp(action, "wake") == 0) {
        mpx_robot_wake();
    }
    return robot_power_reply(req);
}

/* ── GET / POST /v1/policy (mpv2-uros) ─────────────────────────────────── */

/* The RL walk policy from the web app: what the console's `policy` command
 * does, without the USB cable.
 *
 *   GET  /v1/policy   status: state, the loaded policy, the last run's numbers
 *   GET  /v1/policy/log  the last logged run, one CSV line per 20 ms step
 *   POST /v1/policy   {"action":"run","seconds":10,"vx":0.05,"vy":0,"wz":0,
 *                      "hold":2,"dry":false,"force":false,"log":true,
 *                      "own_calibration":true,"getup_auto":true}   (the last three optional)
 *                     {"action":"getup","force":false}   (the get-up policy alone)
 *                     {"action":"cmd","vx":..,"vy":..,"wz":..}  (also a keepalive)
 *                     {"action":"keepalive"} | {"action":"stop"} | {"action":"test"}
 *
 * "getup_auto" (never stays down): a walk that falls runs the get-up policy and
 * walks on, when one is built in (status "getup_ready").
 *
 * A run started here has a dead-man (POLICY_WEB_DEADMAN_MS): the app sends
 * "cmd" every quarter second while it runs, and without it the robot ramps to
 * its stand and stops. A locked phone or lost Wi-Fi cannot leave it walking. */
#define POLICY_WEB_DEADMAN_MS 2000

#if HAVE_MPX_POLICY
static void json_num_or_null(cJSON *o, const char *key, float v)
{
    if (isfinite(v)) {
        cJSON_AddNumberToObject(o, key, v);
    } else {
        cJSON_AddNullToObject(o, key);
    }
}

static const char *policy_phase(mpx_policy_state_t s)
{
    switch (s) {
    case MPX_POLICY_IDLE:     return "idle";
    case MPX_POLICY_PREP:     return "prep";
    case MPX_POLICY_RAMP_IN:  return "ramp_in";
    case MPX_POLICY_HOLD_IN:  return "hold_in";
    case MPX_POLICY_RUN:      return "run";
    case MPX_POLICY_RAMP_OUT: return "ramp_out";
    case MPX_POLICY_HOLD_OUT: return "hold_out";
    case MPX_POLICY_GETUP:    return "getup";
    }
    return "idle";
}

static esp_err_t policy_status_reply(httpd_req_t *req, const char *status, const char *error)
{
    mpx_policy_status_t st;
    float m[3] = { 0, 0, 0 };
    cJSON *o = cJSON_CreateObject();

    if (!o) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    mpx_policy_get_status(&st);
    const bool loaded = mpx_policy_cmd_max(m);
    cJSON_AddBoolToObject(o, "available", true);
    cJSON_AddBoolToObject(o, "ok", error == NULL);
    if (error) cJSON_AddStringToObject(o, "error", error);
    cJSON_AddStringToObject(o, "state", mpx_policy_state_name(st.state));
    cJSON_AddStringToObject(o, "phase", policy_phase(st.state));
    cJSON_AddBoolToObject(o, "active", st.state != MPX_POLICY_IDLE);
    cJSON_AddBoolToObject(o, "loaded", loaded);
    cJSON_AddStringToObject(o, "name", st.name);
    cJSON_AddStringToObject(o, "robot", st.robot);
    cJSON_AddStringToObject(o, "this_robot", CONFIG_MP4_POLICY_ROBOT);
    cJSON_AddBoolToObject(o, "omni", st.omni);
    cJSON_AddBoolToObject(o, "own_calibration", st.robot_offsets);
    cJSON_AddBoolToObject(o, "dry", st.dry);
    cJSON *cmd = cJSON_AddArrayToObject(o, "cmd");
    cJSON *max = cJSON_AddArrayToObject(o, "cmd_max");
    for (int i = 0; i < 3; ++i) {
        if (cmd) cJSON_AddItemToArray(cmd, cJSON_CreateNumber(st.cmd[i]));
        if (max) cJSON_AddItemToArray(max, cJSON_CreateNumber(m[i]));
    }
    cJSON_AddNumberToObject(o, "steps", st.steps);
    cJSON_AddNumberToObject(o, "late_steps", st.late_steps);
    cJSON_AddNumberToObject(o, "forward_us_max", st.forward_us_max);
    cJSON_AddNumberToObject(o, "tick_us_max", st.tick_us_max);
    cJSON_AddNumberToObject(o, "exchange_us_max", st.exchange_us_max);
    json_num_or_null(o, "tilt_deg", st.state != MPX_POLICY_IDLE ? st.tilt_deg : NAN);
    json_num_or_null(o, "temp_max_c", st.state != MPX_POLICY_IDLE ? st.temp_max_c : NAN);
    cJSON_AddStringToObject(o, "last_stop", st.last_stop);
    cJSON_AddBoolToObject(o, "getup_ready", st.getup_ready);
    cJSON_AddStringToObject(o, "getup_name", st.getup_name);
    cJSON_AddBoolToObject(o, "getup_auto", st.auto_getup);
    cJSON_AddNumberToObject(o, "getups", st.getups);
    cJSON_AddNumberToObject(o, "getups_ok", st.getups_ok);
    size_t dropped = 0;
    cJSON_AddNumberToObject(o, "log_steps", (double)mpx_policy_log_count(&dropped));
    cJSON_AddNumberToObject(o, "log_dropped", (double)dropped);

    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!text) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, text);
    cJSON_free(text);
    return err;
}
#endif

static esp_err_t policy_get_handler(httpd_req_t *req)
{
#if HAVE_MPX_POLICY
    return policy_status_reply(req, "200 OK", NULL);
#else
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"available\":false}");
#endif
}

static esp_err_t policy_log_handler(httpd_req_t *req)
{
#if HAVE_MPX_POLICY
    if (mpx_policy_active()) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "the run is still on");
    }
    size_t dropped = 0;
    const size_t n = mpx_policy_log_count(&dropped);
    char line[200];

    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"rl_walk_log.csv\"");
    if (httpd_resp_sendstr_chunk(req, mpx_policy_log_header()) != ESP_OK) return ESP_FAIL;
    if (httpd_resp_sendstr_chunk(req, "\n") != ESP_OK) return ESP_FAIL;
    /* About 80 bytes a line, sent in 2 KB pieces. */
    char chunk[2048];
    size_t used = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!mpx_policy_log_row(i, line, sizeof(line))) break;
        const size_t l = strlen(line);
        if (used + l + 2 > sizeof(chunk)) {
            if (httpd_resp_send_chunk(req, chunk, used) != ESP_OK) return ESP_FAIL;
            used = 0;
        }
        memcpy(chunk + used, line, l);
        used += l;
        chunk[used++] = '\n';
    }
    if (used && httpd_resp_send_chunk(req, chunk, used) != ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(req, NULL, 0);
#else
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no RL policy in this build");
#endif
}

static esp_err_t policy_post_handler(httpd_req_t *req)
{
#if HAVE_MPX_POLICY
    cJSON *body = NULL;
    char action[16] = "";

    if (http_server_parse_json_body(req, &body) != ESP_OK || !body) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON body");
    }
    http_server_json_read_string(body, "action", action, sizeof(action));
    const float vx = (float)json_num(body, "vx", 0.0);
    const float vy = (float)json_num(body, "vy", 0.0);
    const float wz = (float)json_num(body, "wz", 0.0);
    const float seconds = (float)json_num(body, "seconds", 10.0);
    const float hold = (float)json_num(body, "hold", 2.0);
    const bool dry = cJSON_IsTrue(cJSON_GetObjectItem(body, "dry"));
    const bool force = cJSON_IsTrue(cJSON_GetObjectItem(body, "force"));
    const bool log = cJSON_IsTrue(cJSON_GetObjectItem(body, "log"));
    const cJSON *own = cJSON_GetObjectItem(body, "own_calibration");
    const bool set_own = cJSON_IsBool(own);
    const bool own_cal = cJSON_IsTrue(own);
    const cJSON *gauto = cJSON_GetObjectItem(body, "getup_auto");
    const bool set_gauto = cJSON_IsBool(gauto);
    const bool getup_auto = cJSON_IsTrue(gauto);
    cJSON_Delete(body);

    if (strcmp(action, "run") == 0) {
        char why[96] = "";
        if (set_own && !mpx_policy_active()) mpx_policy_set_own_calibration(own_cal);
        mpx_policy_set_log(log);
        if (set_gauto && !mpx_policy_active()) (void)mpx_policy_set_auto_getup(getup_auto);
        mpx_policy_set_deadman_ms(POLICY_WEB_DEADMAN_MS);
        if (mpx_policy_start(seconds, hold, force, dry, why, sizeof(why)) != ESP_OK) {
            ESP_LOGW(TAG, "POST /v1/policy run refused: %s", why);
            return policy_status_reply(req, "409 Conflict", why);
        }
        mpx_policy_set_cmd(vx, vy, wz);   /* only once accepted, as the console does */
        ESP_LOGI(TAG, "POST /v1/policy run %.1f s, cmd %.3f %.3f %.2f%s", seconds, vx, vy, wz, dry ? " (dry)" : "");
        return policy_status_reply(req, "200 OK", NULL);
    }
    if (strcmp(action, "getup") == 0) {
        char why[96] = "";
        mpx_policy_set_log(log);
        mpx_policy_set_deadman_ms(POLICY_WEB_DEADMAN_MS);
        if (mpx_policy_getup(force, why, sizeof(why)) != ESP_OK) {
            ESP_LOGW(TAG, "POST /v1/policy getup refused: %s", why);
            return policy_status_reply(req, "409 Conflict", why);
        }
        ESP_LOGI(TAG, "POST /v1/policy getup");
        return policy_status_reply(req, "200 OK", NULL);
    }
    if (strcmp(action, "cmd") == 0) {
        mpx_policy_keepalive();
        if (mpx_policy_active()) mpx_policy_set_cmd(vx, vy, wz);
        return policy_status_reply(req, "200 OK", NULL);
    }
    if (strcmp(action, "keepalive") == 0) {
        mpx_policy_keepalive();
        return policy_status_reply(req, "200 OK", NULL);
    }
    if (strcmp(action, "stop") == 0) {
        mpx_policy_stop("stopped from the app");
        return policy_status_reply(req, "200 OK", NULL);
    }
    if (strcmp(action, "test") == 0) {
        float e = 0, avg = 0, worst = 0;
        if (!mpx_policy_selftest(&e, &avg, &worst)) {
            return policy_status_reply(req, "409 Conflict", "not while a run is on (or no policy loaded)");
        }
        char buf[200];
        snprintf(buf, sizeof(buf),
                 "{\"ok\":%s,\"max_err\":%g,\"us_avg\":%.0f,\"us_max\":%.0f}",
                 e < 1e-4f ? "true" : "false", (double)e, (double)avg, (double)worst);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_sendstr(req, buf);
    }
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "action: run | getup | cmd | keepalive | stop | test");
#else
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no RL policy in this build");
#endif
}

/* ── Registration ──────────────────────────────────────────────────────── */

esp_err_t http_server_register_mpx_robot_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/v1/robot/gait",             .method = HTTP_POST, .handler = robot_gait_handler },
        { .uri = "/v1/robot/joy",              .method = HTTP_POST, .handler = robot_joy_handler },
        { .uri = "/v1/robot/status",           .method = HTTP_GET,  .handler = robot_status_handler },
        { .uri = "/v1/robot/config",           .method = HTTP_GET,  .handler = robot_config_get_handler },
        { .uri = "/v1/robot/config",           .method = HTTP_POST, .handler = robot_config_post_handler },
        { .uri = "/v1/robot/calibrate",        .method = HTTP_POST, .handler = robot_calibrate_handler },
        { .uri = "/v1/robot/calibrate/reset",  .method = HTTP_POST, .handler = robot_calibrate_reset_handler },
        { .uri = "/v1/robot/diagnostic/ping",  .method = HTTP_POST, .handler = robot_ping_handler },
        { .uri = "/v1/robot/movements",        .method = HTTP_GET,  .handler = robot_movements_handler },
        { .uri = "/v1/robot/look",             .method = HTTP_GET,  .handler = robot_look_handler },
        { .uri = "/v1/robot/moves",            .method = HTTP_GET,  .handler = robot_moves_handler },
        { .uri = "/v1/robot/power",            .method = HTTP_GET,  .handler = robot_power_get_handler },
        { .uri = "/v1/robot/power",            .method = HTTP_POST, .handler = robot_power_post_handler },
        { .uri = "/v1/pi/status",              .method = HTTP_GET,  .handler = pi_status_handler },
        { .uri = "/v1/policy",                 .method = HTTP_GET,  .handler = policy_get_handler },
        { .uri = "/v1/policy",                 .method = HTTP_POST, .handler = policy_post_handler },
        { .uri = "/v1/policy/log",             .method = HTTP_GET,  .handler = policy_log_handler },
    };

    ESP_LOGI(TAG, "registering %u robot routes",
             (unsigned)(sizeof(handlers) / sizeof(handlers[0])));
    return http_server_register_uri_table(server, handlers,
                                          sizeof(handlers) / sizeof(handlers[0]),
                                          "robot");
}
