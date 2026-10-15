/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * The /v1/display routes -- the app's Display page: what the robot's screen
 * shows, its eyes or a GIF animation from <storage>/display/.
 *
 *   GET    /v1/display            state + the animations on the robot
 *   POST   /v1/display            {"on":true,"id":"smile","loop":true} / {"on":false}
 *   GET    /v1/display/gif?id=x   the file itself (the app's previews)
 *   POST   /v1/display/gif?id=x   upload a GIF (raw body)
 *   DELETE /v1/display/gif?id=x   remove one you added
 *
 * The three that ship with the firmware (blink, smile, sleepy) are written
 * into the storage image at build time; they can be played but not
 * overwritten or deleted from here.
 */

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"

#include "cap_display.h"
#include "http_server_priv.h"

static const char *TAG = "http_mpx_display";

#define DISPLAY_UPLOAD_MAX   (600 * 1024)
#define DISPLAY_MAX_W        320
#define DISPLAY_MAX_H        240
#define DISPLAY_FREE_MARGIN  (64 * 1024)   /* never fill storage to the brim */

static const struct { const char *id; const char *name; } s_builtin[] = {
    { "blink",  "Blink"  },
    { "smile",  "Smile"  },
    { "sleepy", "Sleepy" },
};
#define BUILTIN_COUNT (sizeof(s_builtin) / sizeof(s_builtin[0]))

static int builtin_index(const char *id)
{
    for (size_t i = 0; i < BUILTIN_COUNT; i++) {
        if (strcmp(id, s_builtin[i].id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void gif_path(const char *id, char *out, size_t out_size)
{
    snprintf(out, out_size, "%s/%s.gif", cap_display_anim_dir(), id);
}

/* Width and height from the GIF header: "GIF8?a", then two LE uint16. */
static bool gif_header_size(const uint8_t *b, size_t n, unsigned *w, unsigned *h)
{
    if (n < 10 || memcmp(b, "GIF8", 4) != 0 || (b[4] != '7' && b[4] != '9') || b[5] != 'a') {
        return false;
    }
    *w = (unsigned)b[6] | ((unsigned)b[7] << 8);
    *h = (unsigned)b[8] | ((unsigned)b[9] << 8);
    return true;
}

static bool gif_file_size(const char *path, unsigned *w, unsigned *h, long *bytes)
{
    uint8_t head[10];
    struct stat st;
    FILE *f;
    bool ok;

    if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) {
        return false;
    }
    f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    ok = fread(head, 1, sizeof(head), f) == sizeof(head) && gif_header_size(head, sizeof(head), w, h);
    fclose(f);
    *bytes = (long)st.st_size;
    return ok;
}

static void add_anim(cJSON *arr, const char *id, const char *name, bool builtin)
{
    char path[HTTP_SERVER_PATH_MAX];
    unsigned w = 0, h = 0;
    long bytes = 0;
    cJSON *o;

    gif_path(id, path, sizeof(path));
    if (!gif_file_size(path, &w, &h, &bytes)) {
        return;
    }
    o = cJSON_CreateObject();
    if (!o) {
        return;
    }
    http_server_json_add_string(o, "id", id);
    http_server_json_add_string(o, "name", name);
    cJSON_AddNumberToObject(o, "bytes", (double)bytes);
    cJSON_AddNumberToObject(o, "w", w);
    cJSON_AddNumberToObject(o, "h", h);
    cJSON_AddBoolToObject(o, "builtin", builtin);
    cJSON_AddItemToArray(arr, o);
}

/* "my_cat-2" -> "My cat 2" */
static void pretty_name(const char *id, char *out, size_t out_size)
{
    size_t i = 0;

    for (; id[i] && i + 1 < out_size; i++) {
        char c = id[i];
        out[i] = (c == '_' || c == '-') ? ' ' : (i == 0 && c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    out[i] = '\0';
}

static esp_err_t display_reply(httpd_req_t *req)
{
    cap_display_anim_state_t st;
    cJSON *root = cJSON_CreateObject();
    cJSON *anims;

    if (!root) {
        return httpd_resp_send_500(req);
    }
    cap_display_anim_get(&st);
    cJSON_AddBoolToObject(root, "on", st.on);
    cJSON_AddBoolToObject(root, "loop", st.loop);
    http_server_json_add_string(root, "id", st.id);
    cJSON_AddNumberToObject(root, "max_bytes", DISPLAY_UPLOAD_MAX);
    cJSON_AddNumberToObject(root, "max_w", DISPLAY_MAX_W);
    cJSON_AddNumberToObject(root, "max_h", DISPLAY_MAX_H);

    anims = cJSON_AddArrayToObject(root, "anims");
    if (anims) {
        DIR *dir;

        for (size_t i = 0; i < BUILTIN_COUNT; i++) {
            add_anim(anims, s_builtin[i].id, s_builtin[i].name, true);
        }
        dir = opendir(cap_display_anim_dir());
        if (dir) {
            struct dirent *e;

            while ((e = readdir(dir)) != NULL) {
                char id[32], name[32];
                const char *dot = strrchr(e->d_name, '.');
                size_t len;

                if (!dot || strcasecmp(dot, ".gif") != 0) {
                    continue;
                }
                len = (size_t)(dot - e->d_name);
                if (len == 0 || len >= sizeof(id)) {
                    continue;
                }
                memcpy(id, e->d_name, len);
                id[len] = '\0';
                if (!cap_display_anim_id_valid(id) || builtin_index(id) >= 0) {
                    continue;
                }
                pretty_name(id, name, sizeof(name));
                add_anim(anims, id, name, false);
            }
            closedir(dir);
        }
    }

    {
        uint64_t total = 0, free_b = 0;
        const char *base = http_server_ctx()->storage_base_path;

        if (esp_vfs_fat_info(base, &total, &free_b) == ESP_OK) {
            cJSON *s = cJSON_AddObjectToObject(root, "storage");
            if (s) {
                cJSON_AddNumberToObject(s, "total", (double)total);
                cJSON_AddNumberToObject(s, "used", (double)(total - free_b));
            }
        }
    }
    return http_server_send_json_response(req, root);
}

static esp_err_t display_get_handler(httpd_req_t *req)
{
    return display_reply(req);
}

static esp_err_t display_post_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    cJSON *on, *loop;
    char id[32] = { 0 };
    esp_err_t err;

    if (http_server_parse_json_body(req, &body) != ESP_OK || !body) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }
    on = cJSON_GetObjectItem(body, "on");
    loop = cJSON_GetObjectItem(body, "loop");
    http_server_json_read_string(body, "id", id, sizeof(id));
    {
        const bool want_on = cJSON_IsBool(on) ? cJSON_IsTrue(on) : true;
        const bool want_loop = cJSON_IsBool(loop) ? cJSON_IsTrue(loop) : true;
        cJSON_Delete(body);

        if (!want_on) {
            cap_display_anim_stop();
            return display_reply(req);
        }
        if (!id[0]) {
            cap_display_anim_state_t st;
            cap_display_anim_get(&st);
            strlcpy(id, st.id[0] ? st.id : s_builtin[0].id, sizeof(id));
        }
        err = cap_display_anim_play(id, want_loop);
    }
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_ARG) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No such animation");
    }
    if (err == ESP_ERR_NO_MEM) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Not enough memory to play it right now");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not play that GIF");
    }
    return display_reply(req);
}

static esp_err_t id_from_query(httpd_req_t *req, char *id, size_t id_size)
{
    if (http_server_query_get(req, "id", id, id_size) != ESP_OK || !cap_display_anim_id_valid(id)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t gif_get_handler(httpd_req_t *req)
{
    char id[32] = { 0 };
    char path[HTTP_SERVER_PATH_MAX];
    char *scratch;
    FILE *f;

    if (id_from_query(req, id, sizeof(id)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad id");
    }
    gif_path(id, path, sizeof(path));
    f = fopen(path, "rb");
    if (!f) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No such animation");
    }
    scratch = http_server_alloc_scratch_buffer();
    if (!scratch) {
        fclose(f);
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "image/gif");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=300");
    for (;;) {
        const size_t n = fread(scratch, 1, HTTP_SERVER_SCRATCH_SIZE, f);
        if (n == 0) {
            if (ferror(f)) {
                free(scratch);
                fclose(f);
                return ESP_FAIL;
            }
            break;
        }
        if (httpd_resp_send_chunk(req, scratch, n) != ESP_OK) {
            free(scratch);
            fclose(f);
            return ESP_FAIL;
        }
    }
    free(scratch);
    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t gif_post_handler(httpd_req_t *req)
{
    char id[32] = { 0 };
    char path[HTTP_SERVER_PATH_MAX], tmp[HTTP_SERVER_PATH_MAX + 4];
    int remaining = req->content_len;
    bool first = true;
    char *scratch;
    FILE *f;

    if (id_from_query(req, id, sizeof(id)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Name it with letters, digits, - or _");
    }
    if (builtin_index(id) >= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "That name belongs to a built-in animation");
    }
    if (remaining <= 0 || remaining > DISPLAY_UPLOAD_MAX) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "GIF must be under 600 KB");
    }
    {
        uint64_t total = 0, free_b = 0;
        if (esp_vfs_fat_info(http_server_ctx()->storage_base_path, &total, &free_b) == ESP_OK &&
            free_b < (uint64_t)remaining + DISPLAY_FREE_MARGIN) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not enough room on the robot");
        }
    }
    mkdir(cap_display_anim_dir(), 0775);
    gif_path(id, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not create the file");
    }
    scratch = http_server_alloc_scratch_buffer();
    if (!scratch) {
        fclose(f);
        unlink(tmp);
        return httpd_resp_send_500(req);
    }
    while (remaining > 0) {
        const int want = remaining < HTTP_SERVER_SCRATCH_SIZE ? remaining : HTTP_SERVER_SCRATCH_SIZE;
        const int got = httpd_req_recv(req, scratch, want);

        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (got <= 0) {
            free(scratch);
            fclose(f);
            unlink(tmp);
            return ESP_FAIL;
        }
        if (first) {
            unsigned w, h;
            first = false;
            if (!gif_header_size((const uint8_t *)scratch, (size_t)got, &w, &h)) {
                free(scratch);
                fclose(f);
                unlink(tmp);
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "That is not a GIF");
            }
            if (w > DISPLAY_MAX_W || h > DISPLAY_MAX_H) {
                free(scratch);
                fclose(f);
                unlink(tmp);
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "GIF must be 320 x 240 or smaller");
            }
        }
        if (fwrite(scratch, 1, (size_t)got, f) != (size_t)got) {
            free(scratch);
            fclose(f);
            unlink(tmp);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Storage write failed");
        }
        remaining -= got;
    }
    free(scratch);
    fclose(f);
    unlink(path);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save the file");
    }
    ESP_LOGI(TAG, "animation '%s' added (%d bytes)", id, req->content_len);
    return display_reply(req);
}

static esp_err_t gif_delete_handler(httpd_req_t *req)
{
    char id[32] = { 0 };
    char path[HTTP_SERVER_PATH_MAX];
    cap_display_anim_state_t st;

    if (id_from_query(req, id, sizeof(id)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad id");
    }
    if (builtin_index(id) >= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Built-in animations stay");
    }
    cap_display_anim_get(&st);
    if (st.on && strcmp(st.id, id) == 0) {
        cap_display_anim_stop();
    }
    gif_path(id, path, sizeof(path));
    if (unlink(path) != 0) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No such animation");
    }
    ESP_LOGI(TAG, "animation '%s' removed", id);
    return display_reply(req);
}

esp_err_t http_server_register_mpx_display_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/v1/display",     .method = HTTP_GET,    .handler = display_get_handler },
        { .uri = "/v1/display",     .method = HTTP_POST,   .handler = display_post_handler },
        { .uri = "/v1/display/gif", .method = HTTP_GET,    .handler = gif_get_handler },
        { .uri = "/v1/display/gif", .method = HTTP_POST,   .handler = gif_post_handler },
        { .uri = "/v1/display/gif", .method = HTTP_DELETE, .handler = gif_delete_handler },
    };

    ESP_LOGI(TAG, "registering %u display routes", (unsigned)(sizeof(handlers) / sizeof(handlers[0])));
    return http_server_register_uri_table(server, handlers, sizeof(handlers) / sizeof(handlers[0]), "display");
}
