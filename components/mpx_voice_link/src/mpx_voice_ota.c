/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * Device provisioning against the xiaozhi cloud.
 *
 * The websocket URL is NOT configured on the device. This was the thing I got
 * wrong for several rounds: the reference firmware has exactly one address in
 * it, CONFIG_OTA_URL, and everything else is discovered.
 *
 *   1. POST device information to the OTA endpoint.
 *   2. The reply carries `websocket: { url, token }` -- that is where to
 *      connect and what to authenticate with.
 *   3. On a device the account has never seen, the reply also carries
 *      `activation: { code, message }`: a six-digit code to type into the
 *      Xiaozhi console to bind the device to an agent.
 *
 * So `voice provision` has to run before `voice connect` can mean anything,
 * and guessing a path like /xiaozhi/v1/ was never going to work.
 */

#include "mpx_voice_link.h"

#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "mpx_voice_ota";

#define VOICE_NVS_NAMESPACE  "mpx_voice"
#define VOICE_NVS_URL        "ws_url"
#define VOICE_NVS_TOKEN      "ws_token"
#define VOICE_OTA_RESP_MAX   4096

static char s_ws_url[192];
static char s_ws_token[192];

/* ── stored settings ─────────────────────────────────────────────────────── */

static void voice_store(const char *key, const char *value)
{
    nvs_handle_t nvs;
    if (nvs_open(VOICE_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_set_str(nvs, key, value);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static bool voice_load(const char *key, char *out, size_t out_size)
{
    nvs_handle_t nvs;
    if (nvs_open(VOICE_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t len = out_size;
    const esp_err_t err = nvs_get_str(nvs, key, out, &len);
    nvs_close(nvs);
    return err == ESP_OK && out[0] != '\0';
}

const char *mpx_voice_stored_url(void)
{
    if (!s_ws_url[0]) {
        voice_load(VOICE_NVS_URL, s_ws_url, sizeof(s_ws_url));
    }
    return s_ws_url;
}

const char *mpx_voice_stored_token(void)
{
    if (!s_ws_token[0]) {
        voice_load(VOICE_NVS_TOKEN, s_ws_token, sizeof(s_ws_token));
    }
    return s_ws_token;
}

/* ── the request body ────────────────────────────────────────────────────── */

static char *voice_build_device_json(void)
{
    uint8_t mac[6] = {0};
    char mac_str[18];
    char uuid_str[37];

    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    /* A stable per-device identifier. Derived from the MAC rather than random
     * so it survives an NVS erase -- the console binds an agent to it. */
    snprintf(uuid_str, sizeof(uuid_str),
             "%02x%02x%02x%02x-%02x%02x-4000-8000-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    const esp_app_desc_t *app = esp_app_get_description();
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON_AddNumberToObject(root, "version", 2);
    cJSON_AddNumberToObject(root, "flash_size", 16 * 1024 * 1024);
    cJSON_AddNumberToObject(root, "psram_size", 8 * 1024 * 1024);
    cJSON_AddNumberToObject(root, "minimum_free_heap_size",
                            (double)esp_get_minimum_free_heap_size());
    cJSON_AddStringToObject(root, "mac_address", mac_str);
    cJSON_AddStringToObject(root, "uuid", uuid_str);
    cJSON_AddStringToObject(root, "chip_model_name", "esp32s3");

    cJSON *application = cJSON_AddObjectToObject(root, "application");
    if (application) {
        cJSON_AddStringToObject(application, "name", "mp4-claw");
        cJSON_AddStringToObject(application, "version", app ? app->version : "0.0.0");
        cJSON_AddStringToObject(application, "idf_version", app ? app->idf_ver : "");
    }

    cJSON *board = cJSON_AddObjectToObject(root, "board");
    if (board) {
        cJSON_AddStringToObject(board, "type", CONFIG_ESP_BOARD_NAME);
        cJSON_AddStringToObject(board, "name", CONFIG_ESP_BOARD_NAME);
        cJSON_AddStringToObject(board, "mac", mac_str);
    }

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return text;
}

/* ── the response ────────────────────────────────────────────────────────── */

static esp_err_t voice_parse_ota_response(const char *body)
{
    cJSON *root = cJSON_Parse(body);
    ESP_RETURN_ON_FALSE(root, ESP_ERR_INVALID_RESPONSE, TAG, "unparseable reply");

    esp_err_t ret = ESP_ERR_NOT_FOUND;

    /* Activation first: on a device the account has not bound yet, this is
     * the whole point of the exchange and the websocket block may be absent. */
    const cJSON *activation = cJSON_GetObjectItem(root, "activation");
    if (cJSON_IsObject(activation)) {
        const cJSON *code = cJSON_GetObjectItem(activation, "code");
        const cJSON *message = cJSON_GetObjectItem(activation, "message");
        ESP_LOGW(TAG, "*** THIS DEVICE IS NOT ACTIVATED ***");
        if (cJSON_IsString(code)) {
            ESP_LOGW(TAG, "*** Enter code %s in the Xiaozhi console ***",
                     code->valuestring);
        }
        if (cJSON_IsString(message)) {
            ESP_LOGW(TAG, "%s", message->valuestring);
        }
        ESP_LOGW(TAG, "Add Device in the console, type the code, then run "
                      "`voice provision` again");
    }

    const cJSON *ws = cJSON_GetObjectItem(root, "websocket");
    if (cJSON_IsObject(ws)) {
        const cJSON *url = cJSON_GetObjectItem(ws, "url");
        const cJSON *token = cJSON_GetObjectItem(ws, "token");
        if (cJSON_IsString(url) && url->valuestring[0]) {
            strlcpy(s_ws_url, url->valuestring, sizeof(s_ws_url));
            voice_store(VOICE_NVS_URL, s_ws_url);
            ret = ESP_OK;
        }
        if (cJSON_IsString(token)) {
            strlcpy(s_ws_token, token->valuestring, sizeof(s_ws_token));
            voice_store(VOICE_NVS_TOKEN, s_ws_token);
        }
        ESP_LOGI(TAG, "websocket url: %s", s_ws_url);
        ESP_LOGI(TAG, "token: %s", s_ws_token[0] ? "(received)" : "(none)");
    }

    cJSON_Delete(root);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "the reply contained no websocket url");
    }
    return ret;
}

/* ── provision ───────────────────────────────────────────────────────────── */

esp_err_t mpx_voice_provision(const char *ota_url)
{
    char *body = NULL;
    char *resp = NULL;
    esp_err_t ret = ESP_FAIL;
    uint8_t mac[6] = {0};
    char mac_str[18];
    char uuid_str[37];

    const char *url = (ota_url && ota_url[0]) ? ota_url : MPX_VOICE_DEFAULT_OTA_URL;

    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(uuid_str, sizeof(uuid_str),
             "%02x%02x%02x%02x-%02x%02x-4000-8000-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    body = voice_build_device_json();
    ESP_RETURN_ON_FALSE(body, ESP_ERR_NO_MEM, TAG, "out of memory");

    resp = calloc(1, VOICE_OTA_RESP_MAX);
    if (!resp) {
        free(body);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "provisioning against %s", url);
    ESP_LOGI(TAG, "device-id %s", mac_str);
    ESP_LOGI(TAG, "internal RAM before: %u free, %u largest",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
        /* The cloud endpoint is HTTPS. ESP-Claw already carries the IDF
         * certificate bundle, so this needs no per-server certificate. */
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(body);
        free(resp);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Device-Id", mac_str);
    esp_http_client_set_header(client, "Client-Id", uuid_str);
    /* "2" tells the console this device carries an efuse-programmed serial
     * number and will complete the HMAC challenge/response activation flow
     * (Ota::Activate() in the reference client). We have neither -- no
     * factory-programmed serial number, no /activate call -- so claiming v2
     * is what produces "Serial number is required. Please check if the
     * device is programmed" in the console: it is waiting on a step this
     * firmware never performs. Devices without a serial number use v1,
     * the plain code-in-console flow this firmware actually implements. */
    esp_http_client_set_header(client, "Activation-Version", "1");
    esp_http_client_set_header(client, "User-Agent", "mp4-claw/0.1.0");
    esp_http_client_set_header(client, "Accept-Language", "en-US");
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    ret = esp_http_client_open(client, (int)strlen(body));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "cannot reach %s: %s", url, esp_err_to_name(ret));
        goto cleanup;
    }

    if (esp_http_client_write(client, body, (int)strlen(body)) < 0) {
        ESP_LOGE(TAG, "request write failed");
        ret = ESP_FAIL;
        goto cleanup;
    }

    if (esp_http_client_fetch_headers(client) < 0) {
        ESP_LOGE(TAG, "no response headers");
        ret = ESP_FAIL;
        goto cleanup;
    }

    const int status = esp_http_client_get_status_code(client);
    const int got = esp_http_client_read_response(client, resp, VOICE_OTA_RESP_MAX - 1);
    if (got > 0) {
        resp[got] = '\0';
    }
    ESP_LOGI(TAG, "HTTP %d, %d bytes", status, got);

    if (status != 200 || got <= 0) {
        ESP_LOGE(TAG, "unexpected reply: %.200s", got > 0 ? resp : "(empty)");
        ret = ESP_FAIL;
        goto cleanup;
    }

    ret = voice_parse_ota_response(resp);

cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(body);
    free(resp);
    return ret;
}
