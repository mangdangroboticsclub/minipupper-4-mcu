/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * The /v1/studio routes -- Servo Studio: per-servo parameter tuning against
 * the four AT32F413 driver boards.
 *
 * Two rules, both learned the hard way in the MPX-Dog firmware and both
 * enforced here rather than left to the caller:
 *
 *   Every PARAMETER operation requires studio mode. A config exchange is a
 *   request followed by a reply, and gait traffic landing between the two
 *   leaves the reply pending in the AT32's tx buffer, where the next decode
 *   reads it as garbage -- a 31 degree temperature comes back as 1540.
 *
 *   Read-only telemetry deliberately does NOT require studio mode, so the
 *   status and temperature displays keep working while the robot walks.
 *
 * All routes are GET with query parameters, which is how the PWA's
 * ServoStudio.svelte calls them. That is not REST, but it is the contract.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver_board.h"
#include "http_server_priv.h"
#include "mpx_robot.h"

static const char *TAG = "http_mpx_studio";

static int query_int(httpd_req_t *req, const char *key, int fallback)
{
    char raw[16] = {0};
    if (http_server_query_get(req, key, raw, sizeof(raw)) != ESP_OK || raw[0] == '\0') {
        return fallback;
    }
    return (int)strtol(raw, NULL, 10);
}

static float query_float(httpd_req_t *req, const char *key, float fallback)
{
    char raw[24] = {0};
    if (http_server_query_get(req, key, raw, sizeof(raw)) != ESP_OK || raw[0] == '\0') {
        return fallback;
    }
    return strtof(raw, NULL);
}

static esp_err_t send_err_json(httpd_req_t *req, const char *message)
{
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", false);
    http_server_json_add_string(resp, "err", message);
    return http_server_send_json_response(req, resp);
}

/* Guards. These return true when the caller may proceed. */
static bool need_studio(httpd_req_t *req, esp_err_t *out)
{
    if (mpx_robot_studio_mode()) {
        return true;
    }
    *out = send_err_json(req, "studio mode off");
    return false;
}

static bool need_id(httpd_req_t *req, int id, esp_err_t *out)
{
    if (id >= 1 && id <= 12) {
        return true;
    }
    *out = send_err_json(req, "bad id");
    return false;
}

/* ── mode ──────────────────────────────────────────────────────────────── */

static esp_err_t studio_mode_handler(httpd_req_t *req)
{
    char raw[8] = {0};
    cJSON *resp;

    /* `on` absent means "just tell me", which is what the PWA polls with. */
    if (http_server_query_get(req, "on", raw, sizeof(raw)) == ESP_OK && raw[0] != '\0') {
        mpx_robot_set_studio_mode(raw[0] != '0');
    }

    resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "studio", mpx_robot_studio_mode());
    return http_server_send_json_response(req, resp);
}

/* ── status: all twelve servos, no studio mode needed ──────────────────── */

static esp_err_t studio_status_handler(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON *arr;

    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "studio", mpx_robot_studio_mode());
    arr = cJSON_AddArrayToObject(resp, "servos");

    for (int id = 1; arr && id <= 12; ++id) {
        cJSON *item = cJSON_CreateObject();
        const uint16_t scs = driver_board_present_position(id);
        const float c = driver_board_present_temperature(id);
        if (!item) {
            continue;
        }
        cJSON_AddNumberToObject(item, "id", id);
        cJSON_AddNumberToObject(item, "scs", scs);
        cJSON_AddNumberToObject(item, "deg", (double)scs * 270.0 / 1024.0);
        cJSON_AddNumberToObject(item, "ma", driver_board_present_current(id));
        if (c <= DB_TEMP_INVALID) {
            cJSON_AddNullToObject(item, "c");
        } else {
            cJSON_AddNumberToObject(item, "c", (double)c);
        }
        cJSON_AddItemToArray(arr, item);
    }
    return http_server_send_json_response(req, resp);
}

/* ── temps: also free of studio mode ───────────────────────────────────── */

static esp_err_t studio_temps_handler(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON *arr;

    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "studio", mpx_robot_studio_mode());
    arr = cJSON_AddArrayToObject(resp, "temps");

    for (int id = 1; arr && id <= 12; ++id) {
        cJSON *item = cJSON_CreateObject();
        const float c = driver_board_present_temperature(id);
        if (!item) {
            continue;
        }
        cJSON_AddNumberToObject(item, "id", id);
        if (c <= DB_TEMP_INVALID) {
            cJSON_AddNullToObject(item, "c");
        } else {
            cJSON_AddNumberToObject(item, "c", (double)c);
        }
        cJSON_AddItemToArray(arr, item);
    }
    return http_server_send_json_response(req, resp);
}

/* ── scan ──────────────────────────────────────────────────────────────── */

static esp_err_t studio_scan_handler(httpd_req_t *req)
{
    esp_err_t early;
    cJSON *resp, *arr;

    if (!need_studio(req, &early)) {
        return early;
    }
    resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    arr = cJSON_AddArrayToObject(resp, "found");

    for (int id = 1; arr && id <= 12; ++id) {
        float v = 0.0f;
        if (driver_board_get_param(id, DB_PARAM_KP_POSITION, &v)) {
            cJSON_AddItemToArray(arr, cJSON_CreateNumber(id));
        }
    }
    return http_server_send_json_response(req, resp);
}

/* ── dump: every parameter of one servo ────────────────────────────────── */

static esp_err_t studio_dump_handler(httpd_req_t *req)
{
    const int id = query_int(req, "id", 0);
    esp_err_t early;
    cJSON *resp, *arr;

    if (!need_studio(req, &early)) { return early; }
    if (!need_id(req, id, &early)) { return early; }

    resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "id", id);
    arr = cJSON_AddArrayToObject(resp, "params");

    for (int p = 0; arr && p < DB_PARAM_COUNT; ++p) {
        cJSON *item = cJSON_CreateObject();
        float v = 0.0f;
        const char *name = driver_board_param_name(p);
        if (!item) {
            continue;
        }
        cJSON_AddNumberToObject(item, "p", p);
        http_server_json_add_string(item, "n", name ? name : "");
        if (driver_board_get_param(id, p, &v)) {
            cJSON_AddNumberToObject(item, "v", (double)v);
        } else {
            /* null, not 0: "the board did not answer" and "the value is zero"
             * are different things and the UI colours them differently. */
            cJSON_AddNullToObject(item, "v");
        }
        cJSON_AddItemToArray(arr, item);
    }
    return http_server_send_json_response(req, resp);
}

/* ── set / setall ──────────────────────────────────────────────────────── */

static esp_err_t studio_set_handler(httpd_req_t *req)
{
    const int id = query_int(req, "id", 0);
    const int p  = query_int(req, "p", -1);
    const float v = query_float(req, "v", 0.0f);
    esp_err_t early;
    cJSON *resp;
    float readback = 0.0f;

    if (!need_studio(req, &early)) { return early; }
    if (!need_id(req, id, &early)) { return early; }
    if (p < 0 || p >= DB_PARAM_COUNT) {
        return send_err_json(req, "bad param");
    }
    if (!driver_board_set_param(id, p, v)) {
        return send_err_json(req, "no reply");
    }

    resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "p", p);
    /* Report what the board says it now holds, not what we asked for. A write
     * that silently clamps is exactly the thing this screen exists to find. */
    cJSON_AddNumberToObject(resp, "v",
                            driver_board_get_param(id, p, &readback) ? (double)readback : (double)v);
    return http_server_send_json_response(req, resp);
}

static esp_err_t studio_setall_handler(httpd_req_t *req)
{
    const int p = query_int(req, "p", -1);
    const float v = query_float(req, "v", 0.0f);
    const bool save = query_int(req, "save", 0) != 0;
    esp_err_t early;
    cJSON *resp, *fail;
    int ok_count = 0;

    if (!need_studio(req, &early)) { return early; }
    if (p < 0 || p >= DB_PARAM_COUNT) {
        return send_err_json(req, "bad param");
    }

    resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    fail = cJSON_AddArrayToObject(resp, "fail");

    for (int id = 1; id <= 12; ++id) {
        if (driver_board_set_param(id, p, v)) {
            ok_count++;
        } else if (fail) {
            cJSON_AddItemToArray(fail, cJSON_CreateNumber(id));
        }
        /* Pace the bus. Twelve back-to-back request/reply pairs overrun the
         * AT32's handling and the later ones start failing for no reason. */
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    cJSON_AddBoolToObject(resp, "ok", ok_count > 0);
    cJSON_AddNumberToObject(resp, "p", p);
    cJSON_AddNumberToObject(resp, "v", (double)v);
    cJSON_AddNumberToObject(resp, "n", ok_count);
    cJSON_AddBoolToObject(resp, "saved", save ? driver_board_save_config(-1) : false);
    return http_server_send_json_response(req, resp);
}

/* ── save / restore ────────────────────────────────────────────────────── */

static esp_err_t studio_save_handler(httpd_req_t *req)
{
    const int id = query_int(req, "id", 0);
    esp_err_t early;
    cJSON *resp;

    if (!need_studio(req, &early)) { return early; }

    /* id 0 means every board. Otherwise the board is (id-1)/3, since each
     * AT32 carries three servos. */
    if (!driver_board_save_config(id == 0 ? -1 : (id - 1) / 3)) {
        return send_err_json(req, "save failed");
    }
    resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    return http_server_send_json_response(req, resp);
}

static esp_err_t studio_restore_handler(httpd_req_t *req)
{
    const int id = query_int(req, "id", 0);
    esp_err_t early;
    cJSON *resp;

    if (!need_studio(req, &early)) { return early; }
    if (!driver_board_factory_restore(id == 0 ? -1 : (id - 1) / 3)) {
        return send_err_json(req, "restore failed");
    }
    resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    return http_server_send_json_response(req, resp);
}

/* ── direct ────────────────────────────────────────────────────────────── */

static esp_err_t studio_direct_handler(httpd_req_t *req)
{
    const int id = query_int(req, "id", 0);
    const int mode = query_int(req, "m", DB_MODE_POSITION);
    const float deg = query_float(req, "deg", 135.0f);
    const int cur = query_int(req, "cur", 130);
    esp_err_t early;
    cJSON *resp;

    if (!need_studio(req, &early)) { return early; }
    if (!need_id(req, id, &early)) { return early; }

    /* `deg` is the RAW AT32 angle, 0..270 with 135 at centre -- NOT the gait
     * frame, which runs the other way. Servo Studio speaks the AT32 frame
     * throughout, and mixing the two is how a joint ends up mirrored. */
    if (!driver_board_direct(id, (uint16_t)mode, deg, (int16_t)cur)) {
        return send_err_json(req, "spi");
    }
    resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    return http_server_send_json_response(req, resp);
}

/* ── live ──────────────────────────────────────────────────────────────── */

static const char *const kLiveKeys[DB_LIVE_COUNT] = {
    "pos_adc", "cur_adc", "set_deg", "now_deg", "err_deg", "cap_ma",
    "set_ma", "now_ma", "err_ma", "duty", "mode", "loop", "temp_c",
};

static esp_err_t studio_live_handler(httpd_req_t *req)
{
    const int id = query_int(req, "id", 0);
    esp_err_t early;
    cJSON *resp;
    int got = 0;

    if (!need_studio(req, &early)) { return early; }
    if (!need_id(req, id, &early)) { return early; }

    if (!driver_board_poll(id)) {
        return send_err_json(req, "spi");
    }

    resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    for (int i = 0; i < DB_LIVE_COUNT; ++i) {
        float v = 0.0f;
        if (driver_board_get_live(id, i, &v)) {
            cJSON_AddNumberToObject(resp, kLiveKeys[i], (double)v);
            got++;
        }
    }

    /* "full" tells the UI whether the AT32 firmware is new enough to report
     * the whole control loop, or only position and current. Without it the
     * trace view silently plots zeroes and looks like a dead servo. */
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "full", got >= DB_LIVE_COUNT - 1);
    return http_server_send_json_response(req, resp);
}

/* ── Registration ──────────────────────────────────────────────────────── */

esp_err_t http_server_register_mpx_studio_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/v1/studio/mode",    .method = HTTP_GET, .handler = studio_mode_handler },
        /* The same URL again for POST. ServoStudio leaves studio mode with
         * navigator.sendBeacon(), which is the one way a page can still be
         * heard from as it unloads -- and sendBeacon always POSTs. Without
         * this entry that beacon 405s and the robot is left with its servos
         * in studio mode after the tab closes. */
        { .uri = "/v1/studio/mode",    .method = HTTP_POST, .handler = studio_mode_handler },
        { .uri = "/v1/studio/status",  .method = HTTP_GET, .handler = studio_status_handler },
        { .uri = "/v1/studio/temps",   .method = HTTP_GET, .handler = studio_temps_handler },
        { .uri = "/v1/studio/scan",    .method = HTTP_GET, .handler = studio_scan_handler },
        { .uri = "/v1/studio/dump",    .method = HTTP_GET, .handler = studio_dump_handler },
        { .uri = "/v1/studio/set",     .method = HTTP_GET, .handler = studio_set_handler },
        { .uri = "/v1/studio/setall",  .method = HTTP_GET, .handler = studio_setall_handler },
        { .uri = "/v1/studio/save",    .method = HTTP_GET, .handler = studio_save_handler },
        { .uri = "/v1/studio/restore", .method = HTTP_GET, .handler = studio_restore_handler },
        { .uri = "/v1/studio/direct",  .method = HTTP_GET, .handler = studio_direct_handler },
        { .uri = "/v1/studio/live",    .method = HTTP_GET, .handler = studio_live_handler },
    };

    ESP_LOGI(TAG, "registering %u studio routes",
             (unsigned)(sizeof(handlers) / sizeof(handlers[0])));
    return http_server_register_uri_table(server, handlers,
                                          sizeof(handlers) / sizeof(handlers[0]),
                                          "studio");
}
