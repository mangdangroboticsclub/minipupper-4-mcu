/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * /v1/ai -- where the robot's AI comes from, for the web app's
 * "AI & Voice" screen.
 *
 *   GET  /v1/ai   {"mode":"mangdang"|"own"|"off", "running_mode":...,
 *                  "own":{"backend","base_url","model","auth_type",
 *                         "max_tokens_field","key_set","key_hint"}}
 *   POST /v1/ai   {"mode":..., and for "own": "backend","base_url","model",
 *                  "auth_type","max_tokens_field","api_key"}
 *
 * The API key is never sent back: "key_set" says whether there is one and
 * "key_hint" shows its last four characters, so the owner can tell which key
 * it is without the robot handing it to anyone on the LAN who asks. An empty
 * "api_key" in a POST keeps the stored one.
 *
 * Saving restarts the robot. The mode decides what starts at boot -- the
 * microphone, the voice link, the device brain -- and a restart is the one
 * way to get all of that right without tearing subsystems down live. The
 * reply goes out first, then the restart.
 */

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "http_server_priv.h"

static const char *TAG = "http_mpx_ai";

static void key_hint(const char *key, char *out, size_t out_len)
{
    const size_t n = key ? strlen(key) : 0;
    if (n == 0) {
        out[0] = '\0';
    } else if (n <= 8) {
        snprintf(out, out_len, "set");
    } else {
        snprintf(out, out_len, "...%s", key + n - 4);
    }
}

static esp_err_t ai_send_error(httpd_req_t *req, const char *status, const char *message)
{
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", false);
    http_server_json_add_string(resp, "error", message);
    httpd_resp_set_status(req, status);
    return http_server_send_json_response(req, resp);
}

static esp_err_t ai_get_handler(httpd_req_t *req)
{
    app_config_t *config = calloc(1, sizeof(*config));
    cJSON *resp = NULL;
    cJSON *own = NULL;
    char hint[16];

    if (!config) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    /* Stored values, not the gateway's overrides: the screen shows (and
     * keeps) the owner's own settings whatever mode is active. */
    if (app_config_load_stored(config) != ESP_OK) {
        free(config);
        return ai_send_error(req, "500 Internal Server Error", "failed to load settings");
    }
    resp = cJSON_CreateObject();
    if (!resp) {
        free(config);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    http_server_json_add_string(resp, "mode",
                                app_config_ai_mode_name(app_config_ai_mode(config)));
    http_server_json_add_string(resp, "running_mode",
                                app_config_ai_mode_name(app_config_running_ai_mode()));
    own = cJSON_AddObjectToObject(resp, "own");
    if (own) {
        /* In mangdang/off mode the stored LLM fields are whatever the owner
         * last saved in "own" mode (or nothing): exactly what the form needs. */
        http_server_json_add_string(own, "backend", config->llm_backend_type);
        http_server_json_add_string(own, "base_url", config->llm_base_url);
        http_server_json_add_string(own, "model", config->llm_model);
        http_server_json_add_string(own, "auth_type", config->llm_auth_type);
        http_server_json_add_string(own, "max_tokens_field", config->llm_max_tokens_field);
        key_hint(config->llm_api_key, hint, sizeof(hint));
        cJSON_AddBoolToObject(own, "key_set", config->llm_api_key[0] != '\0');
        http_server_json_add_string(own, "key_hint", hint);
    }
    /* Wipe the key from the heap copy before freeing it. */
    memset(config->llm_api_key, 0, sizeof(config->llm_api_key));
    free(config);
    return http_server_send_json_response(req, resp);
}

static bool read_field(const cJSON *body, const char *key, char *out, size_t out_len)
{
    const cJSON *item = cJSON_GetObjectItem(body, key);
    if (!cJSON_IsString(item) || !item->valuestring) {
        return false;
    }
    if (strlen(item->valuestring) >= out_len) {
        return false;
    }
    strlcpy(out, item->valuestring, out_len);
    return true;
}

static esp_err_t ai_post_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();
    cJSON *body = NULL;
    app_config_t *config = NULL;
    app_ai_mode_t mode;
    char mode_text[16] = {0};
    esp_err_t err;

    if (!ctx->services.restart_device) {
        return ai_send_error(req, "500 Internal Server Error", "restart service unavailable");
    }
    if (http_server_parse_json_body(req, &body) != ESP_OK) {
        return ai_send_error(req, "400 Bad Request", "invalid JSON body");
    }
    if (!read_field(body, "mode", mode_text, sizeof(mode_text)) ||
            !app_config_parse_ai_mode(mode_text, &mode)) {
        cJSON_Delete(body);
        return ai_send_error(req, "400 Bad Request", "mode must be mangdang, own or off");
    }

    config = calloc(1, sizeof(*config));
    if (!config) {
        cJSON_Delete(body);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    if (app_config_load_stored(config) != ESP_OK) {
        free(config);
        cJSON_Delete(body);
        return ai_send_error(req, "500 Internal Server Error", "failed to load settings");
    }

    if (mode == APP_AI_OWN) {
        char backend[32] = {0};
        char key[APP_CONFIG_STR_LEN] = {0};
        bool bad = false;

        if (!read_field(body, "backend", backend, sizeof(backend)) ||
                (strcmp(backend, "openai_compatible") != 0 &&
                 strcmp(backend, "anthropic_compatible") != 0)) {
            bad = true;
        }
        if (!bad && (!read_field(body, "base_url", config->llm_base_url,
                                 sizeof(config->llm_base_url)) ||
                     strncmp(config->llm_base_url, "http", 4) != 0)) {
            bad = true;
        }
        if (!bad && (!read_field(body, "model", config->llm_model, sizeof(config->llm_model)) ||
                     config->llm_model[0] == '\0')) {
            bad = true;
        }
        if (bad) {
            memset(config->llm_api_key, 0, sizeof(config->llm_api_key));
            free(config);
            cJSON_Delete(body);
            return ai_send_error(req, "400 Bad Request",
                                 "own mode needs a provider, an http(s) base URL and a model");
        }
        strlcpy(config->llm_backend_type, backend, sizeof(config->llm_backend_type));
        if (!read_field(body, "auth_type", config->llm_auth_type, sizeof(config->llm_auth_type))) {
            strlcpy(config->llm_auth_type,
                    strcmp(backend, "anthropic_compatible") == 0 ? "none" : "bearer",
                    sizeof(config->llm_auth_type));
        }
        if (!read_field(body, "max_tokens_field", config->llm_max_tokens_field,
                        sizeof(config->llm_max_tokens_field))) {
            strlcpy(config->llm_max_tokens_field, "max_tokens",
                    sizeof(config->llm_max_tokens_field));
        }
        strlcpy(config->llm_supports_tools, "true", sizeof(config->llm_supports_tools));
        /* Empty keeps the stored key, so the form never has to hold it. */
        if (read_field(body, "api_key", key, sizeof(key)) && key[0]) {
            strlcpy(config->llm_api_key, key, sizeof(config->llm_api_key));
        }
        memset(key, 0, sizeof(key));
        if (config->llm_api_key[0] == '\0') {
            free(config);
            cJSON_Delete(body);
            return ai_send_error(req, "400 Bad Request", "own mode needs an API key");
        }
    }
    cJSON_Delete(body);

    strlcpy(config->ai_mode, app_config_ai_mode_name(mode), sizeof(config->ai_mode));
    ESP_LOGW(TAG, "AI mode -> %s%s%s", config->ai_mode,
             mode == APP_AI_OWN ? " " : "", mode == APP_AI_OWN ? config->llm_base_url : "");
    /* app_config_save writes the LLM fields only in "own" mode, so switching
     * to mangdang or off keeps the owner's key for the next time. */
    err = app_config_save(config);
    memset(config->llm_api_key, 0, sizeof(config->llm_api_key));
    free(config);
    if (err != ESP_OK) {
        return ai_send_error(req, "500 Internal Server Error", "failed to save settings");
    }

    cJSON *resp = cJSON_CreateObject();
    if (resp) {
        cJSON_AddBoolToObject(resp, "ok", true);
        http_server_json_add_string(resp, "mode", app_config_ai_mode_name(mode));
        cJSON_AddBoolToObject(resp, "restarting", true);
        (void)http_server_send_json_response(req, resp);
    }
    ctx->services.restart_device();
    return ESP_OK;
}

esp_err_t http_server_register_mpx_ai_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/v1/ai", .method = HTTP_GET,  .handler = ai_get_handler },
        { .uri = "/v1/ai", .method = HTTP_POST, .handler = ai_post_handler },
    };
    return http_server_register_uri_table(server, handlers,
                                          sizeof(handlers) / sizeof(handlers[0]), "ai");
}
