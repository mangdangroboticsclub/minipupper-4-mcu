#include "mpx_enroll.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "mpx_efuse.h"
#include "mpx_efuse_record.h"
#include "mpx_id.h"
#include "mpx_identity.h"
#include "mpx_seal.h"
#include "mpx_seal_key.h"

static const char *TAG = "mpx_enroll";

static const char *const kNvsNamespace = "mpx_id";
static const char *const kNvsActivated = "activated";
static const char *const kNvsBlob = "seal";

/* blobs are 392 bytes; hex doubles that, plus the JSON around it. */
#define ENROLL_BODY_MAX (2 * MPX_SEAL_BLOB_LEN + 256)

static bool load_blob(uint8_t blob[MPX_SEAL_BLOB_LEN])
{
    nvs_handle_t handle;
    size_t len = MPX_SEAL_BLOB_LEN;

    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    const esp_err_t err = nvs_get_blob(handle, kNvsBlob, blob, &len);
    nvs_close(handle);
    return err == ESP_OK && len == MPX_SEAL_BLOB_LEN;
}

static bool save_blob(const uint8_t blob[MPX_SEAL_BLOB_LEN])
{
    nvs_handle_t handle;
    bool ok = false;

    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s) failed", kNvsNamespace);
        return false;
    }
    if (nvs_set_blob(handle, kNvsBlob, blob, MPX_SEAL_BLOB_LEN) == ESP_OK &&
        nvs_commit(handle) == ESP_OK) {
        ok = true;
    }
    nvs_close(handle);
    return ok;
}

bool mpx_enroll_activated(void)
{
    nvs_handle_t handle;
    uint8_t value = 0;

    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    (void)nvs_get_u8(handle, kNvsActivated, &value);
    nvs_close(handle);
    return value != 0;
}

esp_err_t mpx_enroll_set_activated(bool activated)
{
    nvs_handle_t handle;

    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        return ESP_FAIL;
    }
    const esp_err_t err = nvs_set_u8(handle, kNvsActivated, activated ? 1u : 0u);
    if (err == ESP_OK) {
        (void)nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

#if CONFIG_APP_IDENTITY_SOURCE_EFUSE && CONFIG_APP_ENROLL_ENABLE

static void hex_encode(char *out, const uint8_t *in, size_t len)
{
    static const char kHex[] = "0123456789abcdef";

    for (size_t i = 0; i < len; i++) {
        out[2 * i] = kHex[in[i] >> 4];
        out[2 * i + 1] = kHex[in[i] & 0x0f];
    }
    out[2 * len] = '\0';
}

/* The seed the station never touches: an identity is enrolable only when both
 * halves are in the fuses. */
static bool identity_present(mpx_efuse_record_t *rec)
{
    uint8_t block = 0;

    /* A development ID must not enrol: it is not unique, and the cloud would
     * bind one robot's key to a name that every dev board shares. */
    return mpx_efuse_read_record(rec) == ESP_OK &&
           mpx_efuse_find_hmac_key(&block) &&
           mpx_id_valid(rec->robot_id);
}

bool mpx_enroll_pending(void)
{
    mpx_efuse_record_t rec;

    return !mpx_enroll_activated() && identity_present(&rec);
}

esp_err_t mpx_enroll_now(void)
{
    mpx_efuse_record_t rec;
    uint8_t blob[MPX_SEAL_BLOB_LEN];
    char *body = NULL;
    esp_http_client_handle_t client = NULL;
    char url[160];
    esp_err_t err = ESP_FAIL;
    int status = 0;
    const char *source = NULL;

    if (!identity_present(&rec)) {
        ESP_LOGW(TAG, "nothing to enrol: identity is not fully burned");
        return ESP_ERR_NOT_FOUND;
    }

    /* A blob stored by the bench listener is reused: the cloud should see one
     * envelope per robot, not a new one per retry. */
    if (!load_blob(blob)) {
        err = mpx_seal_key(rec.robot_id, rec.kv_base, NULL, 0, blob, &source);
        if (err != ESP_OK) {
            /* Nothing to post and nothing to invent: the key cannot be derived
             * without a working HMAC block, and a blob sealed from a wrong key
             * would be worse than a robot that keeps retrying. */
            ESP_LOGE(TAG, "cannot seal the key, enrolment stays pending");
            return err;
        }
        if (!save_blob(blob)) {
            ESP_LOGW(TAG, "sealed but not stored; the next attempt re-seals");
        }
    }

    body = malloc(ENROLL_BODY_MAX);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    char blob_hex[2 * MPX_SEAL_BLOB_LEN + 1];
    hex_encode(blob_hex, blob, sizeof blob);
    snprintf(body, ENROLL_BODY_MAX,
             "{\"robot_uuid\":\"%s\",\"kv\":%u,\"seal_key_id\":%d,\"blob\":\"%s\"}",
             rec.robot_id, (unsigned)rec.kv_base, CONFIG_APP_PROV_SEAL_KEY_ID,
             blob_hex);
    memset(blob_hex, 0, sizeof blob_hex);

    snprintf(url, sizeof url, "http://%s:%d%s", CONFIG_APP_CHAT_SERVER_IP,
             CONFIG_APP_CHAT_SERVER_PORT, CONFIG_APP_ENROLL_PATH);

    const esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
    };
    client = esp_http_client_init(&cfg);
    if (client == NULL) {
        free(body);
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        status = esp_http_client_get_status_code(client);
    }
    esp_http_client_cleanup(client);
    free(body);
    memset(blob, 0, sizeof blob);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "enrolment POST failed: %s", esp_err_to_name(err));
        return err;
    }
    /* 409 means the cloud already holds this robot: the unit is enrolled even
     * though this particular call did not create the record. */
    if (status != 200 && status != 201 && status != 204 && status != 409) {
        ESP_LOGW(TAG, "cloud refused enrolment with status %d", status);
        return ESP_FAIL;
    }
    if (mpx_enroll_set_activated(true) != ESP_OK) {
        ESP_LOGW(TAG, "enrolled but could not record it; will re-send next boot");
    }
    ESP_LOGI(TAG, "enrolled %s kv=%u (%s key)", rec.robot_id,
             (unsigned)rec.kv_base, source != NULL ? source : "stored");
    return ESP_OK;
}

/* Retries are the normal case, not an error: the robot may boot before the
 * access point is up, and a bench unit may have no network at all. The backoff
 * is slow enough to be invisible in a log and quick enough that a customer who
 * just joined Wi-Fi sees activation within a minute. */
static void enroll_task(void *arg)
{
    (void)arg;

    for (;;) {
        if (!mpx_enroll_pending()) {
            vTaskDelete(NULL);
            return;
        }
        if (mpx_enroll_now() == ESP_OK) {
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

esp_err_t mpx_enroll_init(void)
{
    static bool started = false;

    if (started) {
        return ESP_OK;
    }
    if (!mpx_enroll_pending()) {
        return ESP_OK;
    }
    started = true;
    /* 6 KB: the body is heap-allocated, this only holds TCP state. */
    if (xTaskCreate(enroll_task, "mpx_enroll", 6144, NULL, 3, NULL) != pdPASS) {
        started = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "identity not yet announced; enrolment task started");
    return ESP_OK;
}

#else

bool mpx_enroll_pending(void)
{
    return false;
}

esp_err_t mpx_enroll_now(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t mpx_enroll_init(void)
{
    return ESP_OK;
}

#endif
