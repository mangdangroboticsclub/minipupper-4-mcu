/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * The /v1/lua routes -- the Lua script store and runner the PWA's Skills and
 * FileViewer screens speak to.
 *
 * The old firmware carried its own Lua interpreter behind these URLs. Here the
 * URLs stay and the interpreter underneath them is ESP-Claw's cap_lua, so a
 * script saved from the PWA is the same object the agent can run with
 * lua_run_script, and vice versa. That is the whole reason to rewire rather
 * than port: one script store, two front doors.
 *
 * Scripts live in <DATA>/scripts -- app_claw's own writable Lua root
 * (app_claw.c composes it with claw_paths_join(CLAW_PATH_DATA, "scripts")),
 * not a private /lua directory. cap_lua's package.path already searches it.
 *
 * On ordering: cap_lua's async runner runs jobs concurrently and rejects a
 * second job in the same exclusive group rather than queuing it. The PWA's
 * deploy flow posts a skill's scripts to /v1/lua/enqueue in a loop and expects
 * them to run in order, one after another -- that is what the old single
 * worker task gave it. So enqueue keeps a worker of its own: a queue of paths
 * and one task that runs them through cap_lua_run_script back to back. The
 * concurrency lives in cap_lua; the ordering lives here.
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "cap_lua.h"
#include "claw_paths.h"
#include "esp_err.h"
#include "esp_log.h"

#include "http_server_priv.h"

static const char *TAG = "http_mpx_lua";

/* Where scripts live, relative to CLAW_PATH_DATA. Must match app_claw's
 * lua_root_dir, or the PWA and the agent would be looking at different sets of
 * files. */
#define MPX_LUA_DIR             "scripts"

#define MPX_LUA_RUN_TIMEOUT_MS  5000
#define MPX_LUA_OUTPUT_MAX      2048
#define MPX_LUA_SCRIPT_MAX      (32 * 1024)
#define MPX_LUA_BODY_MAX        (64 * 1024)
#define MPX_LUA_QUEUE_DEPTH     8
#define MPX_LUA_DEPLOY_SLOTS    8   /* _deploy_0..7.lua, reused round-robin */

/* ── Path helpers ──────────────────────────────────────────────────────────
 *
 * A name arrives from the browser, so it is checked rather than normalised:
 * anything with a separator or a dot-dot in it is refused outright. The store
 * is flat by design -- the PWA has no notion of a subdirectory here, so a name
 * that implies one is a mistake or an attack, and neither deserves a guess. */

static bool lua_name_ok(const char *name)
{
    size_t len;

    if (!name || name[0] == '\0' || name[0] == '.') {
        return false;
    }
    if (strchr(name, '/') || strchr(name, '\\') || strstr(name, "..")) {
        return false;
    }
    len = strlen(name);
    if (len < 5 || len > 64) {
        return false;
    }
    return strcmp(name + len - 4, ".lua") == 0;
}

static esp_err_t lua_dir_path(char *out, size_t out_size)
{
    return claw_paths_join(CLAW_PATH_DATA, MPX_LUA_DIR, out, out_size);
}

static esp_err_t lua_name_path(const char *name, char *out, size_t out_size)
{
    char rel[HTTP_SERVER_PATH_MAX];
    int written = snprintf(rel, sizeof(rel), "%s/%s", MPX_LUA_DIR, name);

    if (written < 0 || (size_t)written >= sizeof(rel)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return claw_paths_join(CLAW_PATH_DATA, rel, out, out_size);
}

static bool lua_write_file(const char *full_path, const char *data, size_t len)
{
    char dir[HTTP_SERVER_PATH_MAX];
    FILE *f;
    size_t written;

    if (lua_dir_path(dir, sizeof(dir)) == ESP_OK) {
        mkdir(dir, 0777);   /* already-exists is the common case, and fine */
    }
    f = fopen(full_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "open for write failed: %s (errno=%d)", full_path, errno);
        return false;
    }
    written = len ? fwrite(data, 1, len, f) : 0;
    fclose(f);
    if (written != len) {
        ESP_LOGE(TAG, "short write: %s (%zu of %zu)", full_path, written, len);
        return false;
    }
    return true;
}

/* ── Body reader ───────────────────────────────────────────────────────────
 *
 * Not http_server_parse_json_body(): that caps a body at 8 KB, which is right
 * for a settings form and wrong here. A marketplace skill's deploy script is
 * the body of a /v1/lua/enqueue POST, and one of those can be tens of
 * kilobytes -- the old firmware allowed 256 KB. Truncating it would hand cJSON
 * a half-object and report "invalid JSON" for a request that was fine.
 */
static esp_err_t lua_parse_body(httpd_req_t *req, cJSON **out_root)
{
    char *buf;
    int total = 0;

    *out_root = NULL;
    if (req->content_len <= 0 || req->content_len > MPX_LUA_BODY_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    buf = malloc(req->content_len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    while (total < (int)req->content_len) {
        int got = httpd_req_recv(req, buf + total, req->content_len - total);
        if (got <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        total += got;
    }
    buf[total] = '\0';

    *out_root = cJSON_Parse(buf);
    free(buf);
    return *out_root ? ESP_OK : ESP_ERR_INVALID_ARG;
}

/* ── Responses ─────────────────────────────────────────────────────────── */

static esp_err_t lua_send_ok(httpd_req_t *req, const char *key, const char *value)
{
    cJSON *resp = cJSON_CreateObject();

    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    if (key) {
        http_server_json_add_string(resp, key, value);
    }
    return http_server_send_json_response(req, resp);
}

/* The PWA reads d.error on failure and shows it verbatim, so the message is
 * part of the interface: it is what the user sees when a script does not run. */
static esp_err_t lua_send_err(httpd_req_t *req, const char *message)
{
    cJSON *resp = cJSON_CreateObject();

    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", false);
    http_server_json_add_string(resp, "error", message);
    return http_server_send_json_response(req, resp);
}

/* ── The enqueue worker ────────────────────────────────────────────────────
 *
 * One task, one queue of absolute paths. Started lazily on the first enqueue
 * so a robot that never deploys a skill never pays for the stack. */

typedef struct {
    char path[HTTP_SERVER_PATH_MAX];
} lua_queue_item_t;

static QueueHandle_t s_lua_queue;
static TaskHandle_t  s_lua_worker;
static int           s_deploy_seq;

static void lua_worker_task(void *arg)
{
    lua_queue_item_t item;
    char *output = malloc(MPX_LUA_OUTPUT_MAX);

    (void)arg;
    if (!output) {
        ESP_LOGE(TAG, "worker: no memory for output buffer");
        s_lua_worker = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (xQueueReceive(s_lua_queue, &item, portMAX_DELAY) == pdTRUE) {
        esp_err_t err;

        output[0] = '\0';
        err = cap_lua_run_script(item.path, NULL, MPX_LUA_RUN_TIMEOUT_MS,
                                 output, MPX_LUA_OUTPUT_MAX);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "queued script done: %s", item.path);
        } else {
            /* Nobody is waiting on this result -- the POST returned long ago --
             * so the log is the only place it can go. */
            ESP_LOGE(TAG, "queued script failed: %s err=%s output=%s",
                     item.path, esp_err_to_name(err), output);
        }
    }

    free(output);
    s_lua_worker = NULL;
    vTaskDelete(NULL);
}

static bool lua_worker_ensure(void)
{
    if (s_lua_worker) {
        return true;
    }
    if (!s_lua_queue) {
        s_lua_queue = xQueueCreate(MPX_LUA_QUEUE_DEPTH, sizeof(lua_queue_item_t));
        if (!s_lua_queue) {
            return false;
        }
    }
    /* Small stack: the interpreter itself runs in cap_lua's own runner task,
     * and this one only blocks waiting for it. */
    if (xTaskCreate(lua_worker_task, "mpx_lua_q", 3072, NULL, 4, &s_lua_worker) != pdPASS) {
        s_lua_worker = NULL;
        return false;
    }
    return true;
}

/* ── POST /v1/lua/enqueue ──────────────────────────────────────────────── */

static esp_err_t lua_enqueue_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    const cJSON *script;
    lua_queue_item_t item;
    char name[32];

    if (lua_parse_body(req, &body) != ESP_OK || !body) {
        return lua_send_err(req, "invalid JSON body");
    }
    script = cJSON_GetObjectItem(body, "script");
    if (!cJSON_IsString(script) || script->valuestring[0] == '\0') {
        cJSON_Delete(body);
        return lua_send_err(req, "need 'script' field");
    }
    if (strlen(script->valuestring) > MPX_LUA_SCRIPT_MAX) {
        cJSON_Delete(body);
        return lua_send_err(req, "script too large");
    }

    /* The deploy pipeline's scripts are throwaway, so they reuse a fixed ring
     * of filenames instead of growing the store forever. /v1/lua/list hides
     * them, and SkillsView filters them again on its side. */
    snprintf(name, sizeof(name), "_deploy_%d.lua", s_deploy_seq);
    s_deploy_seq = (s_deploy_seq + 1) % MPX_LUA_DEPLOY_SLOTS;

    if (lua_name_path(name, item.path, sizeof(item.path)) != ESP_OK) {
        cJSON_Delete(body);
        return lua_send_err(req, "path too long");
    }
    if (!lua_write_file(item.path, script->valuestring, strlen(script->valuestring))) {
        cJSON_Delete(body);
        return lua_send_err(req, "failed to save script");
    }
    cJSON_Delete(body);

    if (!lua_worker_ensure()) {
        return lua_send_err(req, "Lua worker unavailable");
    }
    if (xQueueSend(s_lua_queue, &item, 0) != pdTRUE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return lua_send_err(req, "Lua queue full");
    }

    ESP_LOGI(TAG, "POST /v1/lua/enqueue  %s", item.path);
    return lua_send_ok(req, NULL, NULL);
}

/* ── POST /v1/lua/run ──────────────────────────────────────────────────────
 *
 * Body is {"script":"..."} or {"path":"/scripts/foo.lua"}. The path form comes
 * from FileViewer, which composes it from what /v1/fs/list returned -- so it is
 * DATA-relative and goes through the same resolver those routes use. */

static esp_err_t lua_run_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    const cJSON *script, *path;
    char full[HTTP_SERVER_PATH_MAX];
    char *output;
    esp_err_t err;
    cJSON *resp;

    if (lua_parse_body(req, &body) != ESP_OK || !body) {
        return lua_send_err(req, "invalid JSON body");
    }
    script = cJSON_GetObjectItem(body, "script");
    path   = cJSON_GetObjectItem(body, "path");

    if (cJSON_IsString(path) && path->valuestring[0] != '\0') {
        if (!http_server_path_is_safe(path->valuestring) ||
                http_server_resolve_storage_path(path->valuestring, full, sizeof(full)) != ESP_OK) {
            cJSON_Delete(body);
            return lua_send_err(req, "bad path");
        }
        cJSON_Delete(body);
        body = NULL;
    } else if (cJSON_IsString(script) && script->valuestring[0] != '\0') {
        /* An ad-hoc script still has to become a file: cap_lua runs paths, not
         * strings. One fixed scratch name rather than a fresh one per request,
         * because these are debug one-shots and a growing pile of them on
         * flash would be the only lasting trace. */
        if (strlen(script->valuestring) > MPX_LUA_SCRIPT_MAX) {
            cJSON_Delete(body);
            return lua_send_err(req, "script too large");
        }
        if (lua_name_path("_scratch.lua", full, sizeof(full)) != ESP_OK ||
                !lua_write_file(full, script->valuestring, strlen(script->valuestring))) {
            cJSON_Delete(body);
            return lua_send_err(req, "failed to save script");
        }
        cJSON_Delete(body);
        body = NULL;
    } else {
        cJSON_Delete(body);
        return lua_send_err(req, "need 'script' or 'path' field");
    }

    output = malloc(MPX_LUA_OUTPUT_MAX);
    if (!output) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    output[0] = '\0';

    err = cap_lua_run_script(full, NULL, MPX_LUA_RUN_TIMEOUT_MS, output, MPX_LUA_OUTPUT_MAX);
    ESP_LOGI(TAG, "POST /v1/lua/run  %s -> %s", full, esp_err_to_name(err));

    resp = cJSON_CreateObject();
    if (!resp) {
        free(output);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
    /* cap_lua writes the failure reason into the same buffer as the result, so
     * one buffer answers both fields -- only the name changes. */
    http_server_json_add_string(resp, err == ESP_OK ? "output" : "error", output);
    free(output);
    return http_server_send_json_response(req, resp);
}

/* ── GET /v1/lua/list ──────────────────────────────────────────────────────
 *
 * Bare names, no leading slash -- the same shape /v1/skills/list returns. The
 * two disagreed once and every consumer downstream assumed the second form. */

static esp_err_t lua_list_handler(httpd_req_t *req)
{
    char dir[HTTP_SERVER_PATH_MAX];
    cJSON *resp, *files;
    DIR *d;
    struct dirent *ent;

    resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    files = cJSON_AddArrayToObject(resp, "files");

    if (lua_dir_path(dir, sizeof(dir)) != ESP_OK || !(d = opendir(dir))) {
        /* No directory yet simply means no scripts yet. */
        return http_server_send_json_response(req, resp);
    }

    while ((ent = readdir(d)) != NULL) {
        size_t len = strlen(ent->d_name);

        if (ent->d_name[0] == '.' || ent->d_name[0] == '_') {
            continue;   /* hidden files, and the deploy/scratch scratchpad */
        }
        if (len < 5 || strcmp(ent->d_name + len - 4, ".lua") != 0) {
            continue;
        }
        cJSON_AddItemToArray(files, cJSON_CreateString(ent->d_name));
    }
    closedir(d);

    return http_server_send_json_response(req, resp);
}

/* ── POST /v1/lua/save ─────────────────────────────────────────────────── */

static esp_err_t lua_save_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    const cJSON *name, *code;
    char full[HTTP_SERVER_PATH_MAX];
    const char *text;

    if (lua_parse_body(req, &body) != ESP_OK || !body) {
        return lua_send_err(req, "invalid JSON body");
    }
    name = cJSON_GetObjectItem(body, "name");
    code = cJSON_GetObjectItem(body, "code");

    if (!cJSON_IsString(name) || !lua_name_ok(name->valuestring)) {
        cJSON_Delete(body);
        return lua_send_err(req, "bad name");
    }
    text = cJSON_IsString(code) ? code->valuestring : "";
    if (strlen(text) > MPX_LUA_SCRIPT_MAX) {
        cJSON_Delete(body);
        return lua_send_err(req, "script too large");
    }
    if (lua_name_path(name->valuestring, full, sizeof(full)) != ESP_OK ||
            !lua_write_file(full, text, strlen(text))) {
        cJSON_Delete(body);
        return lua_send_err(req, "failed to save script");
    }
    ESP_LOGI(TAG, "POST /v1/lua/save  %s (%zu bytes)", full, strlen(text));
    cJSON_Delete(body);

    return lua_send_ok(req, "path", full);
}

/* ── GET /v1/lua/read?name=foo.lua ─────────────────────────────────────── */

static esp_err_t lua_read_handler(httpd_req_t *req)
{
    char name[80];
    char full[HTTP_SERVER_PATH_MAX];
    FILE *f;
    long size;
    char *text;
    cJSON *resp;

    if (http_server_query_get(req, "name", name, sizeof(name)) != ESP_OK) {
        return lua_send_err(req, "missing 'name' query param");
    }
    http_server_url_decode_inplace(name);
    if (!lua_name_ok(name) || lua_name_path(name, full, sizeof(full)) != ESP_OK) {
        return lua_send_err(req, "bad name");
    }

    f = fopen(full, "rb");
    if (!f) {
        httpd_resp_set_status(req, "404 Not Found");
        return lua_send_err(req, "file not found");
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0 || size > MPX_LUA_SCRIPT_MAX) {
        fclose(f);
        return lua_send_err(req, "file too large");
    }
    text = malloc((size_t)size + 1);
    if (!text) {
        fclose(f);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    if (size > 0 && fread(text, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(text);
        return lua_send_err(req, "read failed");
    }
    fclose(f);
    text[size] = '\0';

    resp = cJSON_CreateObject();
    if (!resp) {
        free(text);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    http_server_json_add_string(resp, "name", name);
    http_server_json_add_string(resp, "code", text);
    free(text);
    return http_server_send_json_response(req, resp);
}

/* ── POST /v1/lua/delete ───────────────────────────────────────────────── */

static esp_err_t lua_delete_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    const cJSON *name;
    char full[HTTP_SERVER_PATH_MAX];
    bool ok;

    if (lua_parse_body(req, &body) != ESP_OK || !body) {
        return lua_send_err(req, "invalid JSON body");
    }
    name = cJSON_GetObjectItem(body, "name");
    if (!cJSON_IsString(name) || !lua_name_ok(name->valuestring)) {
        cJSON_Delete(body);
        return lua_send_err(req, "bad name");
    }
    if (lua_name_path(name->valuestring, full, sizeof(full)) != ESP_OK) {
        cJSON_Delete(body);
        return lua_send_err(req, "path too long");
    }
    cJSON_Delete(body);

    ok = (unlink(full) == 0);
    ESP_LOGI(TAG, "POST /v1/lua/delete  %s -> %s", full, ok ? "ok" : "failed");
    return ok ? lua_send_ok(req, NULL, NULL) : lua_send_err(req, "delete failed");
}

/* ── Registration ──────────────────────────────────────────────────────── */

esp_err_t http_server_register_mpx_lua_routes(httpd_handle_t server)
{
    static const httpd_uri_t handlers[] = {
        { .uri = "/v1/lua/enqueue", .method = HTTP_POST, .handler = lua_enqueue_handler },
        { .uri = "/v1/lua/run",     .method = HTTP_POST, .handler = lua_run_handler     },
        { .uri = "/v1/lua/list",    .method = HTTP_GET,  .handler = lua_list_handler    },
        { .uri = "/v1/lua/save",    .method = HTTP_POST, .handler = lua_save_handler    },
        { .uri = "/v1/lua/read",    .method = HTTP_GET,  .handler = lua_read_handler    },
        { .uri = "/v1/lua/delete",  .method = HTTP_POST, .handler = lua_delete_handler  },
    };

    return http_server_register_uri_table(server, handlers,
                                          sizeof(handlers) / sizeof(handlers[0]),
                                          "mpx lua");
}
