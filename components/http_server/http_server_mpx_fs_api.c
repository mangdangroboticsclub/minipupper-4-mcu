/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * The /v1/fs routes -- the file browser the PWA's FileViewer and AddActionView use.
 *
 * These are a thin re-spelling of ESP-Claw's own /api/files endpoints: same
 * DATA root, same path-safety rule, different JSON field names because the
 * PWA already speaks the older shape. Rather than teach the PWA a second
 * vocabulary, the two spellings coexist.
 *
 * "The filesystem" here is the DATA root, so the skills directory shows up as
 * /mpx_skills the same way sessions and memory do -- which is the honest view,
 * and means the PWA browser and cap_files agree about what exists.
 */

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"

#include "http_server_priv.h"

static const char *TAG = "http_mpx_fs";

#define MPX_FS_READ_MAX (64 * 1024)

/* ── GET /v1/fs/list?path= ─────────────────────────────────────────────── */

static esp_err_t fs_list_handler(httpd_req_t *req)
{
    char rel[HTTP_SERVER_PATH_MAX] = "/";
    char full[HTTP_SERVER_PATH_MAX];
    cJSON *resp = NULL, *files = NULL, *dirs = NULL;
    DIR *dir;
    struct dirent *ent;

    if (http_server_query_get(req, "path", rel, sizeof(rel)) != ESP_OK || rel[0] == '\0') {
        snprintf(rel, sizeof(rel), "/");
    }
    http_server_url_decode_inplace(rel);
    if (!http_server_path_is_safe(rel)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    if (http_server_resolve_storage_path(rel, full, sizeof(full)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }

    resp  = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    files = cJSON_AddArrayToObject(resp, "f");
    dirs  = cJSON_AddArrayToObject(resp, "d");

    dir = opendir(full);
    if (!dir) {
        /* An empty listing rather than a 404: the PWA renders "nothing here"
         * gracefully and a missing directory is not an error worth a dialog. */
        return http_server_send_json_response(req, resp);
    }

    while ((ent = readdir(dir)) != NULL) {
        char child_rel[HTTP_SERVER_PATH_MAX];
        char child_full[HTTP_SERVER_PATH_MAX];
        struct stat st;

        if (ent->d_name[0] == '.') {
            continue;
        }
        /* Build the child path with ESP-Claw's own helpers rather than
         * snprintf. Both bounds-check and fail rather than truncate, so an
         * entry whose path will not fit is skipped -- which is correct, since
         * a path we cannot compose is one we could not have opened either. */
        if (!http_server_build_child_relative_path(rel, ent->d_name,
                                                   child_rel, sizeof(child_rel)) ||
                http_server_resolve_storage_path(child_rel, child_full,
                                                 sizeof(child_full)) != ESP_OK) {
            continue;
        }
        if (stat(child_full, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (dirs) {
                cJSON_AddItemToArray(dirs, cJSON_CreateString(ent->d_name));
            }
        } else if (files) {
            cJSON *item = cJSON_CreateObject();
            if (!item) {
                continue;
            }
            http_server_json_add_string(item, "n", ent->d_name);
            cJSON_AddNumberToObject(item, "s", (double)st.st_size);
            cJSON_AddBoolToObject(item, "r", false);
            cJSON_AddItemToArray(files, item);
        }
    }
    closedir(dir);
    return http_server_send_json_response(req, resp);
}

/* ── GET /v1/fs/info ───────────────────────────────────────────────────── */

static esp_err_t fs_info_handler(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    const http_server_ctx_t *ctx = http_server_ctx();
    uint64_t total = 0;
    uint64_t free_bytes = 0;

    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    /* esp_vfs_fat_info(), not statvfs() -- ESP-IDF's newlib has no
     * <sys/statvfs.h>, and this is what the rest of the firmware uses
     * (app_fs.c and lua_module_storage both call it).
     *
     * It reads the FAT allocation table rather than walking every block, so it
     * is safe on the HTTP request path. The MPX-Dog firmware had to cache the
     * equivalent because LittleFS's version traversed a 13 MB partition and
     * stalled whatever request was queued behind it; that hazard is gone. */
    if (ctx && esp_vfs_fat_info(ctx->storage_base_path, &total, &free_bytes) == ESP_OK) {
        cJSON_AddNumberToObject(resp, "total", (double)total);
        cJSON_AddNumberToObject(resp, "used", (double)(total - free_bytes));
    } else {
        cJSON_AddNumberToObject(resp, "total", 0);
        cJSON_AddNumberToObject(resp, "used", 0);
    }
    return http_server_send_json_response(req, resp);
}

/* ── GET /v1/fs/read?path= ─────────────────────────────────────────────── */

static esp_err_t fs_read_handler(httpd_req_t *req)
{
    char rel[HTTP_SERVER_PATH_MAX] = {0};
    char full[HTTP_SERVER_PATH_MAX];
    cJSON *resp = NULL;
    char *buf = NULL;
    FILE *f;
    size_t got;
    struct stat st;

    if (http_server_query_get(req, "path", rel, sizeof(rel)) != ESP_OK || rel[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing path");
    }
    http_server_url_decode_inplace(rel);
    if (!http_server_path_is_safe(rel) ||
            http_server_resolve_storage_path(rel, full, sizeof(full)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "file not found");
    }
    if (st.st_size > MPX_FS_READ_MAX) {
        /* This endpoint exists to look at scripts and manifests. Anything
         * larger is a download, and /files/[*] already streams those. */
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "file too large to view; download it instead");
    }

    buf = calloc(1, (size_t)st.st_size + 1);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    f = fopen(full, "rb");
    if (!f) {
        free(buf);
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "file not found");
    }
    got = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    buf[got] = '\0';

    resp = cJSON_CreateObject();
    if (!resp) {
        free(buf);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    http_server_json_add_string(resp, "p", rel);
    http_server_json_add_string(resp, "c", buf);
    free(buf);
    return http_server_send_json_response(req, resp);
}

/* ── POST /v1/fs/delete ────────────────────────────────────────────────── */

static esp_err_t fs_delete_handler(httpd_req_t *req)
{
    cJSON *body = NULL, *resp = NULL;
    char rel[HTTP_SERVER_PATH_MAX] = {0};
    char full[HTTP_SERVER_PATH_MAX];
    const cJSON *sudo;
    bool allow_sudo;
    size_t len;

    if (http_server_parse_json_body(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }
    http_server_json_read_string(body, "path", rel, sizeof(rel));
    sudo = cJSON_GetObjectItem(body, "sudo");
    allow_sudo = cJSON_IsBool(sudo) && cJSON_IsTrue(sudo);
    cJSON_Delete(body);

    if (rel[0] == '\0' || !http_server_path_is_safe(rel) ||
            http_server_resolve_storage_path(rel, full, sizeof(full)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }

    /* Without sudo, only the file types a user could plausibly have created:
     * skills and scripts. Everything else -- sessions, memory, router rules --
     * is agent state that a file browser should not casually destroy. */
    len = strlen(rel);
    if (!allow_sudo) {
        const bool deletable =
            (len > 5 && (strcmp(rel + len - 5, ".wasm") == 0 ||
                         strcmp(rel + len - 5, ".mpxe") == 0)) ||
            (len > 4 && strcmp(rel + len - 4, ".lua") == 0);
        if (!deletable) {
            httpd_resp_set_status(req, "403 Forbidden");
            return httpd_resp_sendstr(req, "sudo required to delete this file");
        }
    }

    if (unlink(full) != 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "delete failed");
    }

    resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    return http_server_send_json_response(req, resp);
}

/* ── Registration ──────────────────────────────────────────────────────── */

esp_err_t http_server_register_mpx_fs_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/v1/fs/list",   .method = HTTP_GET,  .handler = fs_list_handler },
        { .uri = "/v1/fs/info",   .method = HTTP_GET,  .handler = fs_info_handler },
        { .uri = "/v1/fs/read",   .method = HTTP_GET,  .handler = fs_read_handler },
        { .uri = "/v1/fs/delete", .method = HTTP_POST, .handler = fs_delete_handler },
    };

    ESP_LOGI(TAG, "registering %u fs routes",
             (unsigned)(sizeof(handlers) / sizeof(handlers[0])));
    return http_server_register_uri_table(server, handlers,
                                          sizeof(handlers) / sizeof(handlers[0]),
                                          "fs");
}
