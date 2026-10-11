/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * The /v1/display routes -- the app's Display page: what the robot's screen
 * shows, its eyes or a GIF animation from <fs>/display/.
 *
 *   GET    /v1/display            state + the animations on the robot
 *   POST   /v1/display            {"on":true,"id":"smile","loop":true} / {"on":false}
 *                                 also {"flip180":b} {"mirror":b} {"shift_x":n,"shift_y":n}
 *                                 {"brightness":n} {"emotion":"happy"} {"text":"hi"}
 *                                 {"background":"#000000","eyes":"yellow"}
 *   GET    /v1/display/gif?id=x   the file itself (the app's previews)
 *   POST   /v1/display/gif?id=x   upload a GIF (raw body)
 *   DELETE /v1/display/gif?id=x   remove one you added
 *
 * Ported from mpv2-claw's http_server_mpx_display_api.c. Same JSON shapes
 * (the app is the same), with two differences: storage is LittleFS, not
 * FAT, and the orientation / brightness / emotion knobs that mpv2-claw
 * reaches through its console command and agent tools ride on POST
 * /v1/display here, since mpx-dog has neither.
 *
 * The three that ship with the firmware (blink, smile, sleepy) are written
 * to <fs>/display at boot (display_assets.cc); they can be played but not
 * overwritten or deleted from here.
 *
 * Nothing is registered when the display is compiled out: the app treats a
 * 404 on GET /v1/display as "no screen" and hides the page.
 */

#include "network/http_display.h"

#include <dirent.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "display/cap_display.h"
#include "fs/littlefs_manager.h"

static const char *TAG = "http_display";

#if CONFIG_APP_DISPLAY_ENABLE

namespace {

constexpr int    DISPLAY_UPLOAD_MAX  = 600 * 1024;
constexpr int    DISPLAY_MAX_W       = 320;
constexpr int    DISPLAY_MAX_H       = 240;
constexpr size_t DISPLAY_FREE_MARGIN = 64 * 1024;   /* never fill storage to the brim */
constexpr size_t SCRATCH_SIZE        = 4096;
constexpr size_t PATH_MAX_LEN        = 160;
constexpr size_t ID_MAX              = 32;

const struct { const char *id; const char *name; } BUILTIN[] = {
    { "blink",  "Blink"  },
    { "smile",  "Smile"  },
    { "sleepy", "Sleepy" },
};
constexpr size_t BUILTIN_COUNT = sizeof(BUILTIN) / sizeof(BUILTIN[0]);

int builtin_index(const char *id)
{
    for (size_t i = 0; i < BUILTIN_COUNT; i++) {
        if (strcmp(id, BUILTIN[i].id) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void gif_path(const char *id, char *out, size_t out_size)
{
    snprintf(out, out_size, "%s/%s.gif", cap_display_anim_dir(), id);
}

/* Width and height from the GIF header: "GIF8?a", then two LE uint16. */
bool gif_header_size(const uint8_t *b, size_t n, unsigned *w, unsigned *h)
{
    if (n < 10 || memcmp(b, "GIF8", 4) != 0 || (b[4] != '7' && b[4] != '9') || b[5] != 'a') {
        return false;
    }
    *w = static_cast<unsigned>(b[6]) | (static_cast<unsigned>(b[7]) << 8);
    *h = static_cast<unsigned>(b[8]) | (static_cast<unsigned>(b[9]) << 8);
    return true;
}

bool gif_file_size(const char *path, unsigned *w, unsigned *h, long *bytes)
{
    uint8_t head[10];
    struct stat st;

    if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) {
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    const bool ok = fread(head, 1, sizeof(head), f) == sizeof(head) &&
                    gif_header_size(head, sizeof(head), w, h);
    fclose(f);
    *bytes = static_cast<long>(st.st_size);
    return ok;
}

void add_anim(cJSON *arr, const char *id, const char *name, bool builtin)
{
    char path[PATH_MAX_LEN];
    unsigned w = 0, h = 0;
    long bytes = 0;

    gif_path(id, path, sizeof(path));
    if (!gif_file_size(path, &w, &h, &bytes)) {
        return;
    }
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return;
    }
    cJSON_AddStringToObject(o, "id", id);
    cJSON_AddStringToObject(o, "name", name);
    cJSON_AddNumberToObject(o, "bytes", static_cast<double>(bytes));
    cJSON_AddNumberToObject(o, "w", w);
    cJSON_AddNumberToObject(o, "h", h);
    cJSON_AddBoolToObject(o, "builtin", builtin);
    cJSON_AddItemToArray(arr, o);
}

/* "my_cat-2" -> "My cat 2" */
void pretty_name(const char *id, char *out, size_t out_size)
{
    size_t i = 0;
    for (; id[i] && i + 1 < out_size; i++) {
        const char c = id[i];
        out[i] = (c == '_' || c == '-') ? ' '
               : (i == 0 && c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
    }
    out[i] = '\0';
}

esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    const esp_err_t err = httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
    cJSON_free(text);
    return err;
}

esp_err_t display_reply(httpd_req_t *req)
{
    cap_display_anim_state_t st;
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_500(req);
    }
    cap_display_anim_get(&st);
    cJSON_AddBoolToObject(root, "on", st.on);
    cJSON_AddBoolToObject(root, "loop", st.loop);
    cJSON_AddStringToObject(root, "id", st.id);
    cJSON_AddNumberToObject(root, "max_bytes", DISPLAY_UPLOAD_MAX);
    cJSON_AddNumberToObject(root, "max_w", DISPLAY_MAX_W);
    cJSON_AddNumberToObject(root, "max_h", DISPLAY_MAX_H);

    /* mpx-dog additions: the knobs mpv2-claw keeps on its console. */
    {
        bool flip = false, mirror = false;
        int sx = 0, sy = 0;
        cap_display_get_orientation(&flip, &mirror);
        cap_display_get_shift(&sx, &sy);
        cJSON_AddBoolToObject(root, "flip180", flip);
        cJSON_AddBoolToObject(root, "mirror", mirror);
        cJSON_AddNumberToObject(root, "shift_x", sx);
        cJSON_AddNumberToObject(root, "shift_y", sy);
        cJSON_AddNumberToObject(root, "brightness", cap_display_brightness());
        cJSON_AddBoolToObject(root, "eyes_alive", cap_display_face_alive());
    }

    cJSON *anims = cJSON_AddArrayToObject(root, "anims");
    if (anims) {
        for (size_t i = 0; i < BUILTIN_COUNT; i++) {
            add_anim(anims, BUILTIN[i].id, BUILTIN[i].name, true);
        }
        DIR *dir = opendir(cap_display_anim_dir());
        if (dir) {
            struct dirent *e;
            while ((e = readdir(dir)) != nullptr) {
                char id[ID_MAX], name[ID_MAX];
                const char *dot = strrchr(e->d_name, '.');
                if (!dot || strcasecmp(dot, ".gif") != 0) {
                    continue;
                }
                const size_t len = static_cast<size_t>(dot - e->d_name);
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
        size_t total = 0, used = 0;
        if (fs::stats(total, used)) {
            cJSON *s = cJSON_AddObjectToObject(root, "storage");
            if (s) {
                cJSON_AddNumberToObject(s, "total", static_cast<double>(total));
                cJSON_AddNumberToObject(s, "used", static_cast<double>(used));
            }
        }
    }
    return send_json(req, root);
}

esp_err_t display_get_handler(httpd_req_t *req)
{
    return display_reply(req);
}

/* The whole body, NUL-terminated, malloc'd; nullptr if empty or too big. */
char *read_body(httpd_req_t *req, int max_len)
{
    const int len = req->content_len;
    if (len <= 0 || len > max_len) {
        return nullptr;
    }
    char *buf = static_cast<char *>(malloc(static_cast<size_t>(len) + 1));
    if (!buf) {
        return nullptr;
    }
    int total = 0;
    while (total < len) {
        const int got = httpd_req_recv(req, buf + total, static_cast<size_t>(len - total));
        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (got <= 0) {
            free(buf);
            return nullptr;
        }
        total += got;
    }
    buf[total] = '\0';
    return buf;
}

/* The settings a POST may carry besides the animation. Each is applied if
 * present; a bad value is a 400 with the reason. Returns nullptr when fine. */
const char *apply_settings(const cJSON *body)
{
    const cJSON *flip = cJSON_GetObjectItem(body, "flip180");
    const cJSON *mirror = cJSON_GetObjectItem(body, "mirror");
    if (cJSON_IsBool(flip) || cJSON_IsBool(mirror)) {
        bool f = false, m = false;
        cap_display_get_orientation(&f, &m);
        if (cJSON_IsBool(flip))   { f = cJSON_IsTrue(flip); }
        if (cJSON_IsBool(mirror)) { m = cJSON_IsTrue(mirror); }
        cap_display_set_orientation(f, m);
    }

    const cJSON *sx = cJSON_GetObjectItem(body, "shift_x");
    const cJSON *sy = cJSON_GetObjectItem(body, "shift_y");
    if (cJSON_IsNumber(sx) || cJSON_IsNumber(sy)) {
        int x = 0, y = 0;
        cap_display_get_shift(&x, &y);
        if (cJSON_IsNumber(sx)) { x = static_cast<int>(sx->valuedouble); }
        if (cJSON_IsNumber(sy)) { y = static_cast<int>(sy->valuedouble); }
        if (cap_display_set_shift(x, y) == ESP_ERR_INVALID_ARG) {
            return "shift_x / shift_y: -40 to 40 pixels";
        }
    }

    const cJSON *bright = cJSON_GetObjectItem(body, "brightness");
    if (cJSON_IsNumber(bright)) {
        const int pct = static_cast<int>(bright->valuedouble);
        if (pct < 0 || pct > 100) {
            return "brightness: 0 to 100";
        }
        cap_display_set_brightness(pct);
    }

    const cJSON *alive = cJSON_GetObjectItem(body, "eyes_alive");
    if (cJSON_IsBool(alive)) {
        cap_display_set_eyes_alive(cJSON_IsTrue(alive));
    }

    const cJSON *emotion = cJSON_GetObjectItem(body, "emotion");
    if (cJSON_IsString(emotion) && emotion->valuestring[0]) {
        if (cap_display_show_emotion(emotion->valuestring) == ESP_ERR_NOT_FOUND) {
            return "emotion: not one the face knows";
        }
    }

    const cJSON *text = cJSON_GetObjectItem(body, "text");
    if (cJSON_IsString(text)) {
        if (text->valuestring[0]) {
            cap_display_show_text(text->valuestring);
        } else {
            cap_display_clear();
        }
    }

    const cJSON *bg = cJSON_GetObjectItem(body, "background");
    const cJSON *eyes = cJSON_GetObjectItem(body, "eyes");
    if (cJSON_IsString(bg) || cJSON_IsString(eyes)) {
        const int32_t bg_rgb = cJSON_IsString(bg) ? cap_display_parse_color(bg->valuestring) : -1;
        const int32_t eye_rgb = cJSON_IsString(eyes) ? cap_display_parse_color(eyes->valuestring) : -1;
        if ((cJSON_IsString(bg) && bg->valuestring[0] && bg_rgb < 0) ||
            (cJSON_IsString(eyes) && eyes->valuestring[0] && eye_rgb < 0)) {
            return "colours are a name (black, white, red, green, blue, yellow, orange, "
                   "purple, pink, cyan, grey) or hex like #00ff00";
        }
        if (bg_rgb >= 0 || eye_rgb >= 0) {
            cap_display_set_colors(bg_rgb, eye_rgb);
        }
    }
    return nullptr;
}

esp_err_t display_post_handler(httpd_req_t *req)
{
    char *raw = read_body(req, 2048);
    if (!raw) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }
    cJSON *body = cJSON_Parse(raw);
    free(raw);
    if (!body) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }

    const char *bad = apply_settings(body);
    if (bad) {
        cJSON_Delete(body);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, bad);
    }

    const cJSON *on = cJSON_GetObjectItem(body, "on");
    const cJSON *loop = cJSON_GetObjectItem(body, "loop");
    const cJSON *id_item = cJSON_GetObjectItem(body, "id");
    char id[ID_MAX] = { 0 };
    if (cJSON_IsString(id_item)) {
        strlcpy(id, id_item->valuestring, sizeof(id));
    }
    /* A settings-only POST (no "on", no "id") leaves the animation alone. */
    const bool touches_anim = cJSON_IsBool(on) || id[0];
    const bool want_on = cJSON_IsBool(on) ? cJSON_IsTrue(on) : true;
    const bool want_loop = cJSON_IsBool(loop) ? cJSON_IsTrue(loop) : true;
    cJSON_Delete(body);

    if (!touches_anim) {
        return display_reply(req);
    }
    if (!want_on) {
        cap_display_anim_stop();
        return display_reply(req);
    }
    if (!id[0]) {
        cap_display_anim_state_t st;
        cap_display_anim_get(&st);
        strlcpy(id, st.id[0] ? st.id : BUILTIN[0].id, sizeof(id));
    }
    const esp_err_t err = cap_display_anim_play(id, want_loop);
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

esp_err_t id_from_query(httpd_req_t *req, char *id, size_t id_size)
{
    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "id", id, id_size) != ESP_OK ||
        !cap_display_anim_id_valid(id)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t gif_get_handler(httpd_req_t *req)
{
    char id[ID_MAX] = { 0 };
    char path[PATH_MAX_LEN];

    if (id_from_query(req, id, sizeof(id)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad id");
    }
    gif_path(id, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No such animation");
    }
    char *scratch = static_cast<char *>(malloc(SCRATCH_SIZE));
    if (!scratch) {
        fclose(f);
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "image/gif");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=300");
    for (;;) {
        const size_t n = fread(scratch, 1, SCRATCH_SIZE, f);
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
    return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t gif_post_handler(httpd_req_t *req)
{
    char id[ID_MAX] = { 0 };
    char path[PATH_MAX_LEN], tmp[PATH_MAX_LEN + 4];
    int remaining = req->content_len;
    bool first = true;

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
        size_t total = 0, used = 0;
        if (fs::stats(total, used) && total - used < static_cast<size_t>(remaining) + DISPLAY_FREE_MARGIN) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not enough room on the robot");
        }
    }
    mkdir(cap_display_anim_dir(), 0775);
    gif_path(id, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not create the file");
    }
    char *scratch = static_cast<char *>(malloc(SCRATCH_SIZE));
    if (!scratch) {
        fclose(f);
        unlink(tmp);
        return httpd_resp_send_500(req);
    }
    auto fail = [&](const char *msg, httpd_err_code_t code) -> esp_err_t {
        free(scratch);
        fclose(f);
        unlink(tmp);
        return msg ? httpd_resp_send_err(req, code, msg) : ESP_FAIL;
    };
    while (remaining > 0) {
        const int want = remaining < static_cast<int>(SCRATCH_SIZE) ? remaining : static_cast<int>(SCRATCH_SIZE);
        const int got = httpd_req_recv(req, scratch, static_cast<size_t>(want));

        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (got <= 0) {
            return fail(nullptr, HTTPD_500_INTERNAL_SERVER_ERROR);
        }
        if (first) {
            unsigned w, h;
            first = false;
            if (!gif_header_size(reinterpret_cast<const uint8_t *>(scratch), static_cast<size_t>(got), &w, &h)) {
                return fail("That is not a GIF", HTTPD_400_BAD_REQUEST);
            }
            if (w > DISPLAY_MAX_W || h > DISPLAY_MAX_H) {
                return fail("GIF must be 320 x 240 or smaller", HTTPD_400_BAD_REQUEST);
            }
        }
        if (fwrite(scratch, 1, static_cast<size_t>(got), f) != static_cast<size_t>(got)) {
            return fail("Storage write failed", HTTPD_500_INTERNAL_SERVER_ERROR);
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

esp_err_t gif_delete_handler(httpd_req_t *req)
{
    char id[ID_MAX] = { 0 };
    char path[PATH_MAX_LEN];
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

}  // namespace

int network::register_display_routes(httpd_handle_t server,
                                     bool (*reg)(httpd_handle_t, const httpd_uri_t *, const char *))
{
    const struct { const char *uri; httpd_method_t method; const char *name;
                   esp_err_t (*handler)(httpd_req_t *); } table[] = {
        { "/v1/display",     HTTP_GET,    "GET",    display_get_handler  },
        { "/v1/display",     HTTP_POST,   "POST",   display_post_handler },
        { "/v1/display/gif", HTTP_GET,    "GET",    gif_get_handler      },
        { "/v1/display/gif", HTTP_POST,   "POST",   gif_post_handler     },
        { "/v1/display/gif", HTTP_DELETE, "DELETE", gif_delete_handler   },
    };
    int ok = 0;
    for (const auto &t : table) {
        httpd_uri_t h = {};
        h.uri = t.uri;
        h.method = t.method;
        h.handler = t.handler;
        if (reg(server, &h, t.name)) {
            ESP_LOGI(TAG, "  API: %s %s", t.name, t.uri);
            ok++;
        }
    }
    return ok;
}

#else /* !CONFIG_APP_DISPLAY_ENABLE */

int network::register_display_routes(httpd_handle_t,
                                     bool (*)(httpd_handle_t, const httpd_uri_t *, const char *))
{
    ESP_LOGI(TAG, "display compiled out; /v1/display not registered");
    return 0;
}

#endif /* CONFIG_APP_DISPLAY_ENABLE */
