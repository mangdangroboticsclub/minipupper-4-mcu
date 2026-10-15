/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * Four tools that let the agent drive the robot.
 *
 * Design notes that are easy to get wrong here:
 *
 *   Descriptions are capped at CLAW_CAP_TOOL_DESCRIPTION_MAX (256 bytes) and
 *   silently truncated past it. The long explanation -- all 46 movement names,
 *   what "stanford" does that "advance" does not, safe pose limits -- lives in
 *   skills/mpx_robot/SKILL.md, which the model reads when it activates the
 *   skill. Do not try to fit it in here.
 *
 *   execute() runs on the calling agent's task and the agent loop allows 32
 *   tool iterations per turn. A tool that blocks for the length of a movement
 *   spends the user's turn doing nothing visible. So robot_move starts the
 *   movement and returns; robot_get_state reports what happened. That is also
 *   how POST /v1/robot/gait has always behaved.
 */

#include "cap_robot.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "esp_log.h"

#include "mpx_robot.h"
#include "mpx_wasm.h"

static const char *TAG = "cap_robot";

/* Long enough to be useful, short enough that a confused agent cannot park the
 * robot in a movement for a minute. */
#define CAP_ROBOT_MAX_DURATION_MS 10000

/* Attitude limits, matching robot::set_body_attitude()'s own clamps. Repeated
 * here so the tool can explain a rejection instead of silently clamping. */
#define CAP_ROBOT_MAX_ROLL_DEG   25.0f
#define CAP_ROBOT_MAX_PITCH_DEG  20.0f
#define CAP_ROBOT_MAX_YAW_DEG    30.0f

static float json_number(const cJSON *root, const char *key, float fallback)
{
    const cJSON *item = cJSON_GetObjectItem(root, key);
    return cJSON_IsNumber(item) ? (float)item->valuedouble : fallback;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* The Raspberry Pi (ROS) drives the robot in slave mode (mpx_pi_link), and
 * every local motion call is fenced off in mpx_robot. Say so instead of
 * reporting a move that will never happen -- mpv2's voice tools did the same. */
static bool cap_robot_pi_refuses(char *output, size_t output_size)
{
    if (!mpx_robot_pi_in_control()) {
        return false;
    }
    snprintf(output, output_size,
             "{\"ok\":false,\"error\":\"pi_in_control\",\"detail\":\"The Raspberry Pi (ROS) "
             "is controlling the robot right now, so it cannot move on request. It goes back "
             "to normal when the Pi releases control.\"}");
    return true;
}

/* ── robot_move ────────────────────────────────────────────────────────── */

static esp_err_t cap_robot_execute_move(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char *output, size_t output_size)
{
    (void)ctx;
    cJSON *root = NULL;
    const cJSON *movement = NULL;

    if (!input_json || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!mpx_robot_ready()) {
        snprintf(output, output_size,
                 "Error: the robot did not initialise. The servo driver boards "
                 "may be unpowered or unplugged.");
        return ESP_ERR_INVALID_STATE;
    }
    if (cap_robot_pi_refuses(output, output_size)) {
        return ESP_OK;
    }

    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: input must be a JSON object");
        return ESP_ERR_INVALID_ARG;
    }

    movement = cJSON_GetObjectItem(root, "movement");
    if (cJSON_IsString(movement) && movement->valuestring[0]) {
        /* Route through the movement layer rather than straight to the gait
         * table, so a name provided by an installed skill resolves too.
         * Built-ins still win -- a downloaded skill must not be able to
         * shadow "advance". */
        const mpx_movement_result_t r =
            mpx_wasm_movement_run(movement->valuestring, false);
        const char *text = mpx_wasm_movement_result_text(r);

        if (r == MPX_MOVEMENT_STARTED) {
            snprintf(output, output_size,
                     "{\"ok\":true,\"movement\":\"%s\"}", movement->valuestring);
            cJSON_Delete(root);
            return ESP_OK;
        }
        snprintf(output, output_size,
                 "{\"ok\":false,\"movement\":\"%s\",\"error\":\"%s\"}",
                 movement->valuestring, text);
        cJSON_Delete(root);
        /* Not an esp_err failure: "no such movement" is information the model
         * should act on, not an error it should retry. */
        return ESP_OK;
    }

    /* No movement name -- treat it as analog drive. */
    {
        const float f = clampf(json_number(root, "forward", 0.0f), -1.0f, 1.0f);
        const float s = clampf(json_number(root, "strafe",  0.0f), -1.0f, 1.0f);
        const float t = clampf(json_number(root, "turn",    0.0f), -1.0f, 1.0f);
        int duration  = (int)json_number(root, "duration_ms", 0.0f);

        if (fabsf(f) < 0.01f && fabsf(s) < 0.01f && fabsf(t) < 0.01f && duration == 0) {
            cJSON_Delete(root);
            snprintf(output, output_size,
                     "Error: give either a movement name, or forward/strafe/turn.");
            return ESP_ERR_INVALID_ARG;
        }

        mpx_robot_drive(f, s, t);

        if (duration > 0) {
            if (duration > CAP_ROBOT_MAX_DURATION_MS) {
                duration = CAP_ROBOT_MAX_DURATION_MS;
            }
            /* The joystick path times out on its own if nothing refreshes it,
             * so a duration is a promise to stop rather than a blocking wait:
             * report it and let the timeout do the work. */
            snprintf(output, output_size,
                     "{\"ok\":true,\"driving\":{\"forward\":%.2f,\"strafe\":%.2f,"
                     "\"turn\":%.2f},\"stops_after_ms\":%d}",
                     (double)f, (double)s, (double)t, duration);
        } else {
            snprintf(output, output_size,
                     "{\"ok\":true,\"driving\":{\"forward\":%.2f,\"strafe\":%.2f,"
                     "\"turn\":%.2f}}", (double)f, (double)s, (double)t);
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

/* ── robot_pose ────────────────────────────────────────────────────────── */

static esp_err_t cap_robot_execute_pose(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char *output, size_t output_size)
{
    (void)ctx;
    cJSON *root = NULL;
    float roll, pitch, yaw, speed;

    if (!input_json || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!mpx_robot_ready()) {
        snprintf(output, output_size, "Error: the robot did not initialise.");
        return ESP_ERR_INVALID_STATE;
    }
    if (cap_robot_pi_refuses(output, output_size)) {
        return ESP_OK;
    }

    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: input must be a JSON object");
        return ESP_ERR_INVALID_ARG;
    }

    roll  = clampf(json_number(root, "roll",  0.0f), -CAP_ROBOT_MAX_ROLL_DEG,  CAP_ROBOT_MAX_ROLL_DEG);
    pitch = clampf(json_number(root, "pitch", 0.0f), -CAP_ROBOT_MAX_PITCH_DEG, CAP_ROBOT_MAX_PITCH_DEG);
    yaw   = clampf(json_number(root, "yaw",   0.0f), -CAP_ROBOT_MAX_YAW_DEG,   CAP_ROBOT_MAX_YAW_DEG);
    speed = json_number(root, "speed_dps", 0.0f);
    cJSON_Delete(root);

    mpx_robot_set_attitude_speed(speed < 0.0f ? 0.0f : speed);
    mpx_robot_set_body_attitude(roll, pitch, yaw);

    snprintf(output, output_size,
             "{\"ok\":true,\"roll\":%.1f,\"pitch\":%.1f,\"yaw\":%.1f}",
             (double)roll, (double)pitch, (double)yaw);
    return ESP_OK;
}

/* ── robot_get_state ───────────────────────────────────────────────────── */

static esp_err_t cap_robot_execute_get_state(const char *input_json,
                                             const claw_cap_call_context_t *ctx,
                                             char *output, size_t output_size)
{
    (void)input_json;
    (void)ctx;
    mpx_robot_config_t cfg;
    mpx_robot_imu_t imu;
    size_t at = 0;
    int hottest_id = 0;
    float hottest = -300.0f;

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!mpx_robot_ready()) {
        snprintf(output, output_size,
                 "{\"ok\":false,\"error\":\"robot not initialised\"}");
        return ESP_OK;
    }

    mpx_robot_get_config(&cfg);
    mpx_robot_imu_read(&imu);

    /* One servo temperature, not twelve. The agent has a 32 KiB output buffer
     * but a much smaller attention budget, and "is anything overheating" is
     * the question this actually answers. Per-servo detail is what Servo
     * Studio and /v1/studio/temps are for. */
    for (int id = 1; id <= 12; id++) {
        const float c = mpx_robot_read_temperature_c(id);
        if (!isnan(c) && c > hottest) {
            hottest = c;
            hottest_id = id;
        }
    }

    at += (size_t)snprintf(output + at, output_size - at,
                           "{\"ok\":true,\"movement\":\"%s\","
                           "\"config\":{\"period\":%d,\"height\":%d,\"up_height\":%d,"
                           "\"stride\":%d,\"tilt\":%d,\"speed_mm_s\":%d},"
                           "\"imu\":{\"accel_g\":[%.2f,%.2f,%.2f],"
                           "\"gyro_dps\":[%.1f,%.1f,%.1f]}",
                           mpx_robot_current_gait_name(),
                           cfg.period, cfg.height, cfg.up_height,
                           cfg.stride, cfg.tilt, cfg.sg_speed,
                           (double)imu.ax, (double)imu.ay, (double)imu.az,
                           (double)imu.gx, (double)imu.gy, (double)imu.gz);

    if (hottest_id != 0 && at < output_size) {
        at += (size_t)snprintf(output + at, output_size - at,
                               ",\"hottest_servo\":{\"id\":%d,\"celsius\":%.1f}",
                               hottest_id, (double)hottest);
    }

    if (at < output_size) {
        /* Resting = lying down with the servos off to save power; any
         * robot_move wakes it. */
        at += (size_t)snprintf(output + at, output_size - at, ",\"servos\":\"%s\"",
                               mpx_robot_power_state());
    }

    if (at < output_size) {
        at += (size_t)snprintf(output + at, output_size - at, ",\"movements\":[");
        for (int i = 0; i < mpx_robot_gait_name_count() && at < output_size; i++) {
            const char *name = mpx_robot_gait_name_at(i);
            if (!name) {
                continue;
            }
            at += (size_t)snprintf(output + at, output_size - at,
                                   "%s\"%s\"", i ? "," : "", name);
        }
        if (at < output_size) {
            snprintf(output + at, output_size - at, "]}");
        }
    }
    return ESP_OK;
}

/* ── robot_set_config ──────────────────────────────────────────────────── */

static esp_err_t cap_robot_execute_set_config(const char *input_json,
                                              const claw_cap_call_context_t *ctx,
                                              char *output, size_t output_size)
{
    (void)ctx;
    cJSON *root = NULL;
    mpx_robot_config_t cfg;

    if (!input_json || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!mpx_robot_ready()) {
        snprintf(output, output_size, "Error: the robot did not initialise.");
        return ESP_ERR_INVALID_STATE;
    }
    if (cap_robot_pi_refuses(output, output_size)) {
        return ESP_OK;
    }

    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: input must be a JSON object");
        return ESP_ERR_INVALID_ARG;
    }

    /* Read-modify-write: every field is optional, and an omitted field must
     * keep its current value rather than reset to a default. */
    mpx_robot_get_config(&cfg);
    cfg.period    = (int)clampf(json_number(root, "period",    (float)cfg.period),      1.0f, 200.0f);
    cfg.height    = (int)clampf(json_number(root, "height",    (float)cfg.height),      0.0f, 150.0f);
    cfg.up_height = (int)clampf(json_number(root, "up_height", (float)cfg.up_height),   0.0f,  50.0f);
    cfg.stride    = (int)clampf(json_number(root, "stride",    (float)cfg.stride),      0.0f,  50.0f);
    cfg.tilt      = (int)clampf(json_number(root, "tilt",      (float)cfg.tilt),      -30.0f,  30.0f);
    cfg.sg_speed  = (int)clampf(json_number(root, "speed_mm_s", (float)cfg.sg_speed),   0.0f, 200.0f);
    cJSON_Delete(root);

    mpx_robot_set_config(&cfg);

    snprintf(output, output_size,
             "{\"ok\":true,\"period\":%d,\"height\":%d,\"up_height\":%d,"
             "\"stride\":%d,\"tilt\":%d,\"speed_mm_s\":%d}",
             cfg.period, cfg.height, cfg.up_height, cfg.stride, cfg.tilt, cfg.sg_speed);
    return ESP_OK;
}

/* ── Descriptors ───────────────────────────────────────────────────────── */

static const claw_cap_descriptor_t s_robot_descriptors[] = {
    {
        .id = "robot_move",
        .name = "robot_move",
        .family = "robot",
        .description =
            "Make the robot walk, turn, or perform a named movement. Either give "
            "a movement name from robot_get_state.movements, or drive it with "
            "forward/strafe/turn between -1 and 1.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{"
            "\"movement\":{\"type\":\"string\",\"description\":"
            "\"A movement name, e.g. advance, stanford, lookup, twerk.\"},"
            "\"forward\":{\"type\":\"number\",\"minimum\":-1,\"maximum\":1},"
            "\"strafe\":{\"type\":\"number\",\"minimum\":-1,\"maximum\":1,"
            "\"description\":\"Positive is left.\"},"
            "\"turn\":{\"type\":\"number\",\"minimum\":-1,\"maximum\":1,"
            "\"description\":\"Positive turns left.\"},"
            "\"duration_ms\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":10000}"
            "}}",
        .execute = cap_robot_execute_move,
    },
    {
        .id = "robot_pose",
        .name = "robot_pose",
        .family = "robot",
        .description =
            "Hold a body attitude in degrees while the feet stay planted. Use it "
            "to make the robot look up, tilt its head, or lean. Roll is limited "
            "to 25, pitch to 20 and yaw to 30 degrees.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{"
            "\"roll\":{\"type\":\"number\",\"minimum\":-25,\"maximum\":25},"
            "\"pitch\":{\"type\":\"number\",\"minimum\":-20,\"maximum\":20,"
            "\"description\":\"Positive raises the nose.\"},"
            "\"yaw\":{\"type\":\"number\",\"minimum\":-30,\"maximum\":30},"
            "\"speed_dps\":{\"type\":\"number\",\"minimum\":0,\"description\":"
            "\"Degrees per second. 0 snaps instantly.\"}}}",
        .execute = cap_robot_execute_pose,
    },
    {
        .id = "robot_get_state",
        .name = "robot_get_state",
        .family = "robot",
        .description =
            "Read what the robot is doing: the current movement, gait settings, "
            "an IMU sample, the hottest servo, and every movement name that can "
            "be passed to robot_move. **Do not guess these values.**",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_robot_execute_get_state,
    },
    {
        .id = "robot_set_config",
        .name = "robot_set_config",
        .family = "robot",
        .description =
            "Change how the robot walks and persist it. Every field is optional; "
            "omitted fields keep their current value. Read robot_get_state first "
            "so you are changing from what is actually set.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{"
            "\"period\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":200,"
            "\"description\":\"Gait period in ms per phase. Lower is faster.\"},"
            "\"height\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":150,"
            "\"description\":\"Body height in mm.\"},"
            "\"up_height\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":50,"
            "\"description\":\"Foot lift in mm.\"},"
            "\"stride\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":50},"
            "\"tilt\":{\"type\":\"integer\",\"minimum\":-30,\"maximum\":30},"
            "\"speed_mm_s\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":200}"
            "}}",
        .execute = cap_robot_execute_set_config,
    },
};

static const claw_cap_group_t s_robot_group = {
    .group_id = "cap_robot",
    .plugin_name = "MangDang quadruped",
    .version = "1",
    .descriptors = s_robot_descriptors,
    .descriptor_count = sizeof(s_robot_descriptors) / sizeof(s_robot_descriptors[0]),
};

esp_err_t cap_robot_register_group(void)
{
    if (claw_cap_group_exists(s_robot_group.group_id)) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "registering %u robot tools",
             (unsigned)s_robot_group.descriptor_count);
    return claw_cap_register_group(&s_robot_group);
}
