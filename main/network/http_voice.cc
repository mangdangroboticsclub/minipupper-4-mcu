/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * /v1/ai -- where the robot's AI comes from, for the app's "AI & Voice"
 * screen (mpv2-claw's http_server_mpx_ai_api.c, two modes instead of three):
 *
 *   GET  /v1/ai   {"mode":"mangdang"|"off","running_mode":...,
 *                  "modes":["mangdang","off"],"available":bool}
 *   POST /v1/ai   {"mode":...}  ->  {"ok":true,"mode":...,"restarting":false}
 *
 * "modes" is what this firmware offers; the app shows only those. mpv2-claw
 * has a third, "own" (the robot's own agent with the owner's LLM key); that
 * is phase 5 here, so a POST asking for it is a 400. Unlike mpv2-claw the
 * change applies live -- there is no agent to restart around.
 *
 * /v1/voice -- the voice link itself, for the app and for bench checks in
 * place of mpv2-claw's `voice`, `wake` and `music` console commands:
 *
 *   GET  /v1/voice        status (voice_glue::status_json)
 *   POST /v1/voice        any of: {"volume":0..100} {"hands_free":bool}
 *                         {"provision":true} {"music":"jazz"|"https://..."|"stop"}
 *                         {"music_level":5..100}  ->  status
 *   GET  /v1/voice/tools  the tools offered to the voice server
 *   POST /v1/voice/tools  {"name":"robot_move","arguments":{...}} -> the
 *                         tool's reply, as the server would get it
 */

#include "network/http_voice.h"

#include <cstdlib>
#include <cstring>
#include <string>

#include "cJSON.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "voice/voice_glue.h"
#if CONFIG_APP_VOICE_ENABLE
#include "audio/mpx_audio.h"
#include "voice/cap_music.h"
#include "voice/voice_tools.h"
#endif

static const char *TAG = "http_voice";

namespace {

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

esp_err_t send_text(httpd_req_t *req, const std::string &json)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json.c_str(), json.size());
}

esp_err_t send_error(httpd_req_t *req, const char *status, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", message);
    httpd_resp_set_status(req, status);
    return send_json(req, root);
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

cJSON *parse_body(httpd_req_t *req, int max_len)
{
    char *raw = read_body(req, max_len);
    if (!raw) {
        return nullptr;
    }
    cJSON *body = cJSON_Parse(raw);
    free(raw);
    return body;
}

/* ── /v1/ai ─────────────────────────────────────────────────────────── */

cJSON *modes_array()
{
    cJSON *arr = cJSON_CreateArray();
    if (voice_glue::available()) {
        cJSON_AddItemToArray(arr, cJSON_CreateString("mangdang"));
    }
    cJSON_AddItemToArray(arr, cJSON_CreateString("off"));
    return arr;
}

esp_err_t ai_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_500(req);
    }
    const char *mode = voice_glue::mode_name(voice_glue::mode());
    cJSON_AddStringToObject(root, "mode", mode);
    cJSON_AddStringToObject(root, "running_mode", mode);     /* applied live: the same */
    cJSON_AddItemToObject(root, "modes", modes_array());
    cJSON_AddBoolToObject(root, "available", voice_glue::available());
    return send_json(req, root);
}

esp_err_t ai_post_handler(httpd_req_t *req)
{
    cJSON *body = parse_body(req, 2048);
    if (!body) {
        return send_error(req, "400 Bad Request", "invalid JSON body");
    }
    const cJSON *mode_item = cJSON_GetObjectItem(body, "mode");
    voice_glue::Mode mode;
    if (!cJSON_IsString(mode_item) || !voice_glue::parse_mode(mode_item->valuestring, &mode)) {
        const bool own = cJSON_IsString(mode_item) && strcmp(mode_item->valuestring, "own") == 0;
        cJSON_Delete(body);
        return send_error(req, "400 Bad Request",
                          own ? "own-AI mode is not in this firmware yet; choose mangdang or off"
                              : "mode must be mangdang or off");
    }
    cJSON_Delete(body);

    const esp_err_t err = voice_glue::set_mode(mode);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return send_error(req, "409 Conflict", "this robot has no microphone, so voice cannot be on");
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "mode", voice_glue::mode_name(mode));
    cJSON_AddBoolToObject(root, "restarting", false);
    if (err != ESP_OK) {
        /* Saved, but listening did not start (no front end, mic busy...). */
        cJSON_AddStringToObject(root, "warning", esp_err_to_name(err));
    }
    return send_json(req, root);
}

#if CONFIG_APP_VOICE_ENABLE

/* ── /v1/voice ──────────────────────────────────────────────────────── */

esp_err_t voice_get_handler(httpd_req_t *req)
{
    return send_text(req, voice_glue::status_json());
}

esp_err_t voice_post_handler(httpd_req_t *req)
{
    cJSON *body = parse_body(req, 2048);
    if (!body) {
        return send_error(req, "400 Bad Request", "invalid JSON body");
    }
    const char *problem = nullptr;

    const cJSON *volume = cJSON_GetObjectItem(body, "volume");
    if (cJSON_IsNumber(volume)) {
        const int v = volume->valueint;
        if (v < 0 || v > 100) {
            problem = "volume must be 0..100";
        } else {
            mpx_audio_set_volume(v);
        }
    }

    const cJSON *level = cJSON_GetObjectItem(body, "music_level");
    if (!problem && cJSON_IsNumber(level)) {
        const int v = level->valueint;
        if (v < 5 || v > 100) {
            problem = "music_level must be 5..100";
        } else {
            cap_music_set_level(v);
        }
    }

    const cJSON *music = cJSON_GetObjectItem(body, "music");
    if (!problem && cJSON_IsString(music)) {
        if (strcmp(music->valuestring, "stop") == 0 || music->valuestring[0] == '\0') {
            cap_music_stop();
        } else {
            const esp_err_t err = cap_music_play(music->valuestring, nullptr, 0);
            if (err == ESP_ERR_NOT_SUPPORTED) {
                problem = "this robot has no speaker";
            } else if (err != ESP_OK) {
                problem = esp_err_to_name(err);
            }
        }
    }

    const cJSON *provision = cJSON_GetObjectItem(body, "provision");
    if (!problem && cJSON_IsTrue(provision)) {
        const esp_err_t err = voice_glue::provision();
        if (err != ESP_OK) {
            problem = "provisioning failed -- is the robot online?";
            ESP_LOGW(TAG, "provision: %s", esp_err_to_name(err));
        }
    }

    const cJSON *hands_free = cJSON_GetObjectItem(body, "hands_free");
    if (!problem && cJSON_IsBool(hands_free)) {
        if (cJSON_IsTrue(hands_free)) {
            const esp_err_t err = voice_glue::hands_free_start();
            if (err == ESP_ERR_INVALID_STATE) {
                problem = "AI mode is off; turn it on at /v1/ai first";
            } else if (err != ESP_OK) {
                problem = "hands-free listening did not start";
                ESP_LOGW(TAG, "hands_free: %s", esp_err_to_name(err));
            }
        } else {
            voice_glue::hands_free_stop();
        }
    }
    cJSON_Delete(body);

    if (problem) {
        return send_error(req, "400 Bad Request", problem);
    }
    return send_text(req, voice_glue::status_json());
}

/* ── /v1/voice/tools ────────────────────────────────────────────────── */

esp_err_t tools_get_handler(httpd_req_t *req)
{
    return send_text(req, std::string("{\"tools\":") + voice_tools::list_json() + "}");
}

esp_err_t tools_post_handler(httpd_req_t *req)
{
    cJSON *body = parse_body(req, 4096);
    if (!body) {
        return send_error(req, "400 Bad Request", "invalid JSON body");
    }
    const cJSON *name = cJSON_GetObjectItem(body, "name");
    if (!cJSON_IsString(name) || !name->valuestring[0]) {
        cJSON_Delete(body);
        return send_error(req, "400 Bad Request", "'name' is required");
    }
    const cJSON *arguments = cJSON_GetObjectItem(body, "arguments");
    char *args = cJSON_IsObject(arguments) ? cJSON_PrintUnformatted(arguments) : nullptr;

    bool is_error = false;
    const std::string text = voice_tools::call(name->valuestring, args ? args : "{}", &is_error);
    cJSON_free(args);
    cJSON_Delete(body);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(root, "ok", !is_error);
    cJSON_AddStringToObject(root, "text", text.c_str());
    /* When the tool answered with JSON, hand it over parsed as well. */
    if (cJSON *parsed = cJSON_Parse(text.c_str())) {
        cJSON_AddItemToObject(root, "result", parsed);
    }
    return send_json(req, root);
}

#endif  /* CONFIG_APP_VOICE_ENABLE */

}  // namespace

int network::register_voice_routes(httpd_handle_t server,
                                   bool (*reg)(httpd_handle_t, const httpd_uri_t *, const char *))
{
    const struct { const char *uri; httpd_method_t method; const char *name;
                   esp_err_t (*handler)(httpd_req_t *); } table[] = {
        { "/v1/ai",          HTTP_GET,  "GET",  ai_get_handler     },
        { "/v1/ai",          HTTP_POST, "POST", ai_post_handler    },
#if CONFIG_APP_VOICE_ENABLE
        { "/v1/voice",       HTTP_GET,  "GET",  voice_get_handler  },
        { "/v1/voice",       HTTP_POST, "POST", voice_post_handler },
        { "/v1/voice/tools", HTTP_GET,  "GET",  tools_get_handler  },
        { "/v1/voice/tools", HTTP_POST, "POST", tools_post_handler },
#endif
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
