/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * The /v1/skills routes, plus /v1/logs and /v1/trace.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "http_server_priv.h"
#include "mpx_rings.h"
#include "mpx_wasm.h"

static const char *TAG = "http_mpx_skills";

#define MPX_UPLOAD_MAX_BYTES  (256 * 1024)
#define MPX_INSTALLED_PATH    "installed.json"
#define MPX_INSTALLED_MAX     4096
#define MPX_RING_MAX_LINES    499
#define MPX_RING_DEF_LINES    200

/*
 * Filename validator, ported verbatim in behaviour from the MPX-Dog firmware.
 *
 * This is the gate on a byte stream that arrives from a marketplace, so it is
 * an allowlist and not a blocklist: a fixed character set, a length cap, no
 * separators of either kind, no leading dot, no "..", and one of exactly two
 * extensions. Everything else is refused without interpretation.
 */
static bool skill_filename_ok(const char *name)
{
    size_t len;

    if (!name) {
        return false;
    }
    len = strlen(name);
    if (len == 0 || len > 64) {
        return false;
    }
    if (strchr(name, '/') || strchr(name, '\\') || strstr(name, "..")) {
        return false;
    }
    if (name[0] == '.') {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        const char c = name[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    if (len <= 5) {
        return false;
    }
    return strcmp(name + len - 5, ".wasm") == 0 || strcmp(name + len - 5, ".mpxe") == 0;
}

/* ── /v1/skills/list ───────────────────────────────────────────────────── */

static esp_err_t skills_list_handler(httpd_req_t *req)
{
    cJSON *arr = cJSON_CreateArray();

    if (!arr) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    for (int i = 0; i < mpx_wasm_skill_count(); ++i) {
        const char *file = NULL;
        cJSON *item = NULL;
        if (!mpx_wasm_skill_at(i, &file, NULL, NULL, NULL, NULL, NULL) || !file) {
            continue;
        }
        item = cJSON_CreateObject();
        if (!item) {
            continue;
        }
        /* The registry stores "/name"; the PWA expects a bare name. */
        http_server_json_add_string(item, "name", file[0] == '/' ? file + 1 : file);
        cJSON_AddItemToArray(arr, item);
    }
    return http_server_send_json_response(req, arr);
}

/* ── /v1/skills/registry ───────────────────────────────────────────────── */

static esp_err_t skills_registry_handler(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON *arr = NULL;

    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    arr = cJSON_AddArrayToObject(resp, "skills");
    for (int i = 0; arr && i < mpx_wasm_skill_count(); ++i) {
        const char *file = NULL, *slug = NULL, *gait = NULL;
        int abi = 0;
        bool autorun = false, behaviour = false;
        cJSON *item = NULL;

        if (!mpx_wasm_skill_at(i, &file, &slug, &gait, &abi, &autorun, &behaviour)) {
            continue;
        }
        item = cJSON_CreateObject();
        if (!item) {
            continue;
        }
        http_server_json_add_string(item, "slug", slug ? slug : "");
        http_server_json_add_string(item, "file", file ? file : "");
        cJSON_AddNumberToObject(item, "abi", abi);
        http_server_json_add_string(item, "provides_gait", gait ? gait : "");
        cJSON_AddBoolToObject(item, "autorun", autorun);
        cJSON_AddBoolToObject(item, "behaviour", behaviour);
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddBoolToObject(resp, "safe_mode", mpx_wasm_safe_mode());
    return http_server_send_json_response(req, resp);
}

/* ── /v1/skills/run · stop · status ────────────────────────────────────── */

static esp_err_t skills_run_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    cJSON *resp = NULL;
    char skill[80] = {0};
    char params[192] = {0};

    if (http_server_parse_json_body(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
    }
    http_server_json_read_string(body, "skill", skill, sizeof(skill));
    http_server_json_read_string(body, "params", params, sizeof(params));
    cJSON_Delete(body);

    if (skill[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing skill name");
    }

    resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    if (mpx_wasm_skill_running()) {
        /* 409, which is what SkillsView branches on to show "already running"
         * rather than a generic failure. */
        http_server_json_add_string(resp, "output", "a skill is already running");
        httpd_resp_set_status(req, "409 Conflict");
        return http_server_send_json_response(req, resp);
    }

    http_server_json_add_string(resp, "output",
                                mpx_wasm_run_skill(skill, params, "api")
                                    ? "started" : "could not start");
    return http_server_send_json_response(req, resp);
}

static esp_err_t skills_stop_handler(httpd_req_t *req)
{
    const bool was = mpx_wasm_skill_running();
    cJSON *resp = cJSON_CreateObject();

    mpx_wasm_stop_skill();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "was_running", was);
    return http_server_send_json_response(req, resp);
}

static esp_err_t skills_status_handler(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    const char *name;

    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "running", mpx_wasm_skill_running());
    name = mpx_wasm_running_skill_name();
    if (name) {
        http_server_json_add_string(resp, "name", name);
    }
    cJSON_AddBoolToObject(resp, "safe_mode", mpx_wasm_safe_mode());
    return http_server_send_json_response(req, resp);
}

static esp_err_t skills_safe_mode_clear_handler(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();

    mpx_wasm_clear_safe_mode();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    return http_server_send_json_response(req, resp);
}

/* ── Provenance: which marketplace skill owns which file ───────────────────
 *
 * Kept in installed.json beside the modules rather than in the browser.
 * Client-owned tracking produced three real bugs in the MPX-Dog firmware:
 * installing from a phone then a laptop offered to install twice; uninstalling
 * from a second device deleted nothing while reporting success; and clearing
 * site data orphaned every installed file permanently.
 *
 * Rewritten whole on every change. The file is a few hundred bytes, FATFS has
 * no atomic rename to lean on here, and correctness beats cleverness at this
 * size.
 */

static cJSON *installed_read(void)
{
    char *buf = calloc(1, MPX_INSTALLED_MAX + 1);
    cJSON *root = NULL;

    if (!buf) {
        return NULL;
    }
    if (mpx_wasm_skill_file_read(MPX_INSTALLED_PATH, buf, MPX_INSTALLED_MAX) > 0) {
        root = cJSON_Parse(buf);
    }
    free(buf);

    if (!root) {
        root = cJSON_CreateObject();
        if (root) {
            cJSON_AddItemToObject(root, "skills", cJSON_CreateArray());
        }
    } else if (!cJSON_GetObjectItem(root, "skills")) {
        cJSON_AddItemToObject(root, "skills", cJSON_CreateArray());
    }
    return root;
}

static bool installed_write(cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    bool ok;

    if (!text) {
        return false;
    }
    ok = mpx_wasm_skill_file_write(MPX_INSTALLED_PATH, text, strlen(text));
    cJSON_free(text);
    return ok;
}

static esp_err_t skills_installed_handler(httpd_req_t *req)
{
    cJSON *root = installed_read();

    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    return http_server_send_json_response(req, root);
}

static esp_err_t skills_record_handler(httpd_req_t *req)
{
    cJSON *body = NULL, *root = NULL, *arr = NULL, *entry = NULL;
    char skill_id[64] = {0}, file[80] = {0}, title[96] = {0}, version[32] = {0};

    if (http_server_parse_json_body(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }
    http_server_json_read_string(body, "skill_id", skill_id, sizeof(skill_id));
    http_server_json_read_string(body, "file", file, sizeof(file));
    http_server_json_read_string(body, "title", title, sizeof(title));
    http_server_json_read_string(body, "version", version, sizeof(version));
    cJSON_Delete(body);

    if (skill_id[0] == '\0' || file[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing skill_id or file");
    }
    if (!skill_filename_ok(file)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad file name");
    }

    root = installed_read();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    arr = cJSON_GetObjectItem(root, "skills");

    /* Replace any existing record for this id rather than appending, or a
     * reinstall leaves two rows claiming the same skill. */
    for (int i = cJSON_GetArraySize(arr) - 1; i >= 0; --i) {
        const cJSON *it = cJSON_GetArrayItem(arr, i);
        const cJSON *id = cJSON_GetObjectItem(it, "skill_id");
        if (cJSON_IsString(id) && strcmp(id->valuestring, skill_id) == 0) {
            cJSON_DeleteItemFromArray(arr, i);
        }
    }

    entry = cJSON_CreateObject();
    if (entry) {
        http_server_json_add_string(entry, "skill_id", skill_id);
        http_server_json_add_string(entry, "file", file);
        http_server_json_add_string(entry, "title", title);
        http_server_json_add_string(entry, "version", version);
        cJSON_AddItemToArray(arr, entry);
    }

    {
        const bool ok = installed_write(root);
        cJSON *resp;
        cJSON_Delete(root);
        resp = cJSON_CreateObject();
        if (!resp) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        }
        cJSON_AddBoolToObject(resp, "ok", ok);
        return http_server_send_json_response(req, resp);
    }
}

static esp_err_t skills_uninstall_handler(httpd_req_t *req)
{
    cJSON *body = NULL, *root = NULL, *arr = NULL, *resp = NULL;
    char skill_id[64] = {0};
    char file[80] = {0};
    bool deleted = false;

    if (http_server_parse_json_body(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }
    http_server_json_read_string(body, "skill_id", skill_id, sizeof(skill_id));
    cJSON_Delete(body);

    if (skill_id[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing skill_id");
    }

    root = installed_read();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    arr = cJSON_GetObjectItem(root, "skills");

    for (int i = cJSON_GetArraySize(arr) - 1; i >= 0; --i) {
        const cJSON *it = cJSON_GetArrayItem(arr, i);
        const cJSON *id = cJSON_GetObjectItem(it, "skill_id");
        const cJSON *f  = cJSON_GetObjectItem(it, "file");
        if (cJSON_IsString(id) && strcmp(id->valuestring, skill_id) == 0) {
            if (cJSON_IsString(f)) {
                snprintf(file, sizeof(file), "%s", f->valuestring);
            }
            cJSON_DeleteItemFromArray(arr, i);
        }
    }

    /* Remove the file AND the record. Doing only one produced a refunded skill
     * that still ran, or a phantom install with no file behind it. */
    if (file[0] != '\0' && skill_filename_ok(file)) {
        deleted = mpx_wasm_skill_file_delete(file);
    }
    installed_write(root);
    cJSON_Delete(root);
    mpx_wasm_rescan();

    resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    http_server_json_add_string(resp, "file", file);
    cJSON_AddBoolToObject(resp, "deleted", deleted);
    return http_server_send_json_response(req, resp);
}

/* ── /v1/skills/upload ─────────────────────────────────────────────────── */

static esp_err_t skills_upload_handler(httpd_req_t *req)
{
    char name[80] = {0};
    uint8_t *buf = NULL;
    int remaining = req->content_len;
    int at = 0;
    cJSON *resp = NULL;
    bool ok;

    if (http_server_query_get(req, "name", name, sizeof(name)) != ESP_OK || name[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing name");
    }
    http_server_url_decode_inplace(name);
    if (!skill_filename_ok(name)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid filename");
    }
    if (remaining <= 0 || remaining > MPX_UPLOAD_MAX_BYTES) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid size");
    }

    /* PSRAM first: a 256 KB module would be a quarter of internal RAM. */
    buf = heap_caps_malloc((size_t)remaining, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc((size_t)remaining);
    }
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    while (remaining > 0) {
        const int got = httpd_req_recv(req, (char *)buf + at, remaining);
        if (got <= 0) {
            free(buf);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read error");
        }
        at += got;
        remaining -= got;
    }

    ok = mpx_wasm_skill_file_write(name, buf, (size_t)at);
    free(buf);

    if (!ok) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
    }
    mpx_wasm_rescan();

    resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    http_server_json_add_string(resp, "path", name);
    return http_server_send_json_response(req, resp);
}

/* ── /v1/logs and /v1/trace ────────────────────────────────────────────────
 *
 * Polled over plain HTTP, deliberately, and not streamed. httpd runs with
 * max_open_sockets = 12 and lru_purge_enable, so a permanently-held log socket
 * is a candidate for eviction -- it could get itself, or the chat socket,
 * dropped.
 */

static int ring_query_int(httpd_req_t *req, const char *key, int fallback, int lo, int hi)
{
    char raw[16] = {0};
    long v;

    if (http_server_query_get(req, key, raw, sizeof(raw)) != ESP_OK || raw[0] == '\0') {
        return fallback;
    }
    v = strtol(raw, NULL, 10);
    if (v < lo) { return lo; }
    if (v > hi) { return hi; }
    return (int)v;
}

static esp_err_t logs_handler(httpd_req_t *req)
{
    const int since = ring_query_int(req, "since", 0, 0, INT32_MAX);
    const int max   = ring_query_int(req, "max", MPX_RING_DEF_LINES, 1, MPX_RING_MAX_LINES);
    uint32_t next = 0;
    char *json = mpx_log_ring_json_alloc((uint32_t)since, (size_t)max, &next);

    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, json);
    mpx_ring_json_free(json);
    return ESP_OK;
}

static esp_err_t trace_handler(httpd_req_t *req)
{
    const int since = ring_query_int(req, "since", 0, 0, INT32_MAX);
    const int max   = ring_query_int(req, "max", MPX_RING_DEF_LINES, 1, MPX_RING_MAX_LINES);
    uint32_t next = 0;
    char *json = mpx_trace_ring_json_alloc((uint32_t)since, (size_t)max, &next);

    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, json);
    mpx_ring_json_free(json);
    return ESP_OK;
}

/* ── Registration ──────────────────────────────────────────────────────── */

esp_err_t http_server_register_mpx_skills_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/v1/skills/list",             .method = HTTP_GET,  .handler = skills_list_handler },
        { .uri = "/v1/skills/registry",         .method = HTTP_GET,  .handler = skills_registry_handler },
        { .uri = "/v1/skills/status",           .method = HTTP_GET,  .handler = skills_status_handler },
        { .uri = "/v1/skills/installed",        .method = HTTP_GET,  .handler = skills_installed_handler },
        { .uri = "/v1/skills/run",              .method = HTTP_POST, .handler = skills_run_handler },
        { .uri = "/v1/skills/stop",             .method = HTTP_POST, .handler = skills_stop_handler },
        { .uri = "/v1/skills/safe-mode/clear",  .method = HTTP_POST, .handler = skills_safe_mode_clear_handler },
        { .uri = "/v1/skills/record",           .method = HTTP_POST, .handler = skills_record_handler },
        { .uri = "/v1/skills/uninstall",        .method = HTTP_POST, .handler = skills_uninstall_handler },
        { .uri = "/v1/skills/upload",           .method = HTTP_POST, .handler = skills_upload_handler },
        { .uri = "/v1/logs",                    .method = HTTP_GET,  .handler = logs_handler },
        { .uri = "/v1/trace",                   .method = HTTP_GET,  .handler = trace_handler },
    };

    ESP_LOGI(TAG, "registering %u skill routes",
             (unsigned)(sizeof(handlers) / sizeof(handlers[0])));
    return http_server_register_uri_table(server, handlers,
                                          sizeof(handlers) / sizeof(handlers[0]),
                                          "skills");
}
