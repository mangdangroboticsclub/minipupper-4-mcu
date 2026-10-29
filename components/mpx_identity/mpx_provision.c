#include "mpx_provision.h"

#include <string.h>

#include "bootloader_random.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "mpx_efuse.h"
#include "mpx_efuse_record.h"
#include "mpx_prov_sm.h"

static const char *TAG = "mpx_prov";

#if CONFIG_APP_PROVISIONING_ENABLE

#include <stdio.h>

#include "driver/uart.h"
#include "esp_app_desc.h"
#include "esp_random.h"

#include "mpx_derive.h"
#include "mpx_identity.h"
#include "mpx_seal.h"
#include "mpx_seal_device.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mpx_derive.h"
#include "mpx_id.h"
#include "mpx_prov_json.h"
#include "mpx_seal.h"
#include "mpx_seal_device.h"

/* One command line is short (an ID and a version); the buffer is this wide so
 * that a line arriving in pieces cannot be truncated into a valid-looking
 * command. A longer line is discarded whole. */
#define MPX_PROV_LINE_MAX 512

/* The reply carries the sealed blob as hex, which is 784 characters on its own.
 * Hex rather than base64 so the station needs no decoder and so a human reading
 * the log can see the magic. */
#define REPLY_MAX (900 + 2 * MPX_SEAL_BLOB_LEN)

/* D9: the hardware RNG is only a true entropy source once Wi-Fi/Bluetooth has
 * initialised. A board provisioned before its radio is up would otherwise
 * generate S from a predictable source, and S is the one value in this system
 * that nothing can ever replace. `main` sets this flag immediately before
 * bringing up the radio; until then the bootloader's entropy source is enabled
 * for the few hundred milliseconds of a provisioning run and disabled again
 * (leaving it enabled across radio init is what breaks RF).
 */
static bool s_rf_ready;

void mpx_provision_set_rf_ready(void)
{
    s_rf_ready = true;
}

/* NVS: the identity component owns the kv and the activation flag; the blob is
 * here so a lost upload can be retried without re-deriving anything. */
static const char *const kNvsNamespace = "mpx_id";
static const char *const kNvsKv = "kv";
static const char *const kNvsActivated = "activated";
static const char *const kNvsBlob = "seal";

static void reply(const char *json)
{
    /* The station reads this port. One line, always prefixed, so it is
     * distinguishable from log output sharing the UART. */
    printf("@MPXPROVR %s\n", json);
    fflush(stdout);
}

static void reply_error(const char *cmd, const char *error, const char *state)
{
    char out[160];

    snprintf(out, sizeof out,
             "{\"ok\":false,\"cmd\":\"%s\",\"error\":\"%s\",\"state\":\"%s\"}",
             cmd, error, state);
    reply(out);
}

static void hex_encode(char *out, const uint8_t *in, size_t len)
{
    static const char kHex[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < len; i++) {
        out[2 * i] = kHex[in[i] >> 4];
        out[2 * i + 1] = kHex[in[i] & 0x0f];
    }
    out[2 * len] = '\0';
}

static bool save_u32(const char *key, uint32_t value)
{
    nvs_handle_t handle;
    bool ok = false;

    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s) failed", kNvsNamespace);
        return false;
    }
    if (nvs_set_u32(handle, key, value) == ESP_OK && nvs_commit(handle) == ESP_OK) {
        ok = true;
    }
    nvs_close(handle);
    return ok;
}

bool mpx_provision_activated(void)
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

esp_err_t mpx_provision_set_activated(bool activated)
{
    return save_u32(kNvsActivated, activated ? 1u : 0u) ? ESP_OK : ESP_FAIL;
}

const char *mpx_provision_state(void)
{
    mpx_efuse_record_t rec;
    uint8_t block = 0;
    const bool have_record = mpx_efuse_read_record(&rec) == ESP_OK;
    const bool have_key = mpx_efuse_find_hmac_key(&block);

    return mpx_prov_state_name(have_key, have_record);
}

/* Everything a reply needs to say about a board, read fresh from the fuses. */
static void handle_status(void)
{
    mpx_efuse_record_t rec;
    uint8_t block = 0;
    const bool have_record = mpx_efuse_read_record(&rec) == ESP_OK;
    const bool have_key = mpx_efuse_find_hmac_key(&block);
    char out[320];

    snprintf(out, sizeof out,
             "{\"ok\":true,\"cmd\":\"status\",\"state\":\"%s\",\"id\":\"%s\","
             "\"kv\":%u,\"key_block\":%d,\"seal_key\":%d,\"activated\":%s}",
             mpx_prov_state_name(have_key, have_record),
             have_record ? rec.robot_id : "", have_record ? rec.kv_base : 0u,
             have_key ? (int)block : -1, CONFIG_APP_PROV_SEAL_KEY_ID,
             mpx_provision_activated() ? "true" : "false");
    reply(out);
}

/* Derives K_kv and seals it. `secret` is used instead of the HMAC peripheral
 * when the key was burned in this boot, because the peripheral only sees a key
 * after a reset. */
static esp_err_t derive_and_seal(const char *robot_id, uint32_t kv,
                                 const uint8_t *secret, size_t secret_len,
                                 uint8_t out_blob[MPX_SEAL_BLOB_LEN])
{
    mpx_seal_plain_t plain;
    uint8_t key[MPX_KEY_LEN];
    char key_fp[MPX_KEY_FP_LEN + 1];
    const esp_app_desc_t *desc = esp_app_get_description();
    esp_err_t err;

    memset(&plain, 0, sizeof plain);
    memset(key, 0, sizeof key);

    if (secret != NULL) {
        if (mpx_derive_key(mpx_hmac_soft, secret, secret_len, robot_id, kv, key) != 0) {
            ESP_LOGE(TAG, "derive from the supplied secret failed");
            return ESP_FAIL;
        }
    } else if (mpx_derive_key(mpx_efuse_hmac_sha256, NULL, 0, robot_id, kv, key) != 0) {
        ESP_LOGE(TAG, "derive through the HMAC peripheral failed (key block not usable?)");
        return ESP_FAIL;
    }

    if (mpx_derive_key_fp(mpx_hmac_soft, key, key_fp) != 0) {
        ESP_LOGE(TAG, "key fingerprint failed");
        memset(key, 0, sizeof key);
        return ESP_FAIL;
    }

    plain.key_version = kv;
    snprintf(plain.robot_id, sizeof plain.robot_id, "%s", robot_id);
    esp_fill_random(plain.nonce, sizeof plain.nonce);
    memcpy(plain.key, key, sizeof plain.key);
    memset(key, 0, sizeof key);
    if (mpx_efuse_mac(plain.mac) != ESP_OK) {
        memset(plain.mac, 0, sizeof plain.mac);
    }
    plain.sec_flags = mpx_efuse_sec_flags();
    plain.secure_version = mpx_efuse_secure_version();
    if (desc != NULL) {
        snprintf(plain.app_version, sizeof plain.app_version, "%s", desc->version);
    }
    /* ds_pub_sha256 stays zero until Secure Boot is enabled: there is no DS key
     * to hash yet, and a fabricated hash would be worse than an absent one --
     * the cloud reads zeros as "not enforced" and refuses that for prod (D12). */

    err = mpx_seal_plaintext(&plain, CONFIG_APP_PROV_SEAL_KEY_ID, out_blob);
    memset(&plain, 0, sizeof plain);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "seal failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "sealed %s kv=%u key_fp=%s (secure_version=%u)",
             robot_id, (unsigned)kv, key_fp, (unsigned)mpx_efuse_secure_version());
    return ESP_OK;
}

static esp_err_t store_blob(const uint8_t blob[MPX_SEAL_BLOB_LEN])
{
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, kNvsBlob, blob, MPX_SEAL_BLOB_LEN);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

/* Exactly 2*MPX_KEY_LEN hex characters, or a refusal. A short S would burn a
 * key whose upper half is zeros, which no later check could detect. */
static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool hex_to_secret(const char *hex, uint8_t out[MPX_KEY_LEN])
{
    if (strlen(hex) != 2 * MPX_KEY_LEN) {
        return false;
    }
    for (size_t i = 0; i < MPX_KEY_LEN; i++) {
        const int hi = hex_nibble(hex[2 * i]);
        const int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static void handle_provision(const char *body, bool reseal_only)
{
    const char *cmd = reseal_only ? "reseal" : "provision";
    mpx_efuse_record_t rec;
    char robot_id[MPX_ID_STR_LEN + 1];
    uint32_t kv = 0;
    uint8_t block = 0;
    uint8_t secret[MPX_KEY_LEN];
    uint8_t blob[MPX_SEAL_BLOB_LEN];
    char out[REPLY_MAX];
    char blob_hex[2 * MPX_SEAL_BLOB_LEN + 1];
    char s_hex[2 * MPX_KEY_LEN + 1];
    bool have_secret = false;

    if (mpx_json_get_string(body, "id", robot_id, sizeof robot_id) != MPX_JSON_STRING ||
        !mpx_json_get_u32(body, "kv", &kv)) {
        reply_error(cmd, "bad_request", mpx_provision_state());
        return;
    }
    if (!mpx_id_valid_or_legacy(robot_id) || kv == 0 || kv > 0xffffu) {
        reply_error(cmd, "bad_request", mpx_provision_state());
        return;
    }

    /* A factory that generates S itself hands it in here instead of letting the
     * chip generate one. That is a deliberate trade: whoever sees this line can
     * derive every key version this unit will ever have, so it is meant for an
     * HSM-driven station on a controlled line, not for a laptop. Omitted (the
     * default) means the chip generates S and only the sealed K_v ever leaves.
     * The line is never logged, and it is cleared from RAM below either way. */
    if (mpx_json_get_string(body, "s", s_hex, sizeof s_hex) == MPX_JSON_STRING &&
        hex_to_secret(s_hex, secret)) {
        have_secret = true;
    }

    const bool have_record = mpx_efuse_read_record(&rec) == ESP_OK;
    const bool have_key = mpx_efuse_find_hmac_key(&block);
    const bool id_matches = have_record && strcmp(rec.robot_id, robot_id) == 0;
    mpx_prov_action_t action = mpx_prov_decide(have_key, have_record,
                                               id_matches, true);

    if (reseal_only && action != MPX_PROV_SEAL) {
        /* Resealing is the recovery path for a board whose blob was lost; it is
         * not a second way to provision, so it never burns anything. */
        reply_error(cmd, "reseal_requires_sealed", mpx_provision_state());
        return;
    }

    if (action == MPX_PROV_REFUSE_ID_MISMATCH ||
        action == MPX_PROV_REFUSE_BAD_STATE) {
        reply_error(cmd, mpx_prov_error_name(action), mpx_provision_state());
        return;
    }

    /* A burn happens in the same session as the seal, and the newly burned key
     * is not visible to the HMAC peripheral until the next reset. Holding S in
     * RAM for the few hundred milliseconds it takes to seal avoids asking the
     * operator to power-cycle a board that has just become irreversible. */
    if (!have_secret) {
        memset(secret, 0, sizeof secret);
    }
    const bool need_bootloader_rng = !s_rf_ready;
    if (need_bootloader_rng) {
        bootloader_random_enable();
    }
    if (action != MPX_PROV_SEAL) {
        mpx_efuse_record_t next;

        memset(&next, 0, sizeof next);
        snprintf(next.robot_id, sizeof next.robot_id, "%s", robot_id);
        next.format = MPX_EFUSE_FORMAT_V1;
        next.flags = mpx_id_valid(robot_id) ? 0 : MPX_EFUSE_FLAG_DEV;
        next.kv_base = (uint16_t)kv;

        /* S is generated here rather than inside the burn so that the seal
         * below can derive with it before the reset the HMAC peripheral needs.
         * A factory-supplied S is already in the buffer and is used as-is. */
        if (!have_secret) {
            esp_fill_random(secret, sizeof secret);
        }

        /* Uses whichever blocks are free. In the `partial` states this burns a
         * second key block and lets the old one go unused: the old S cannot be
         * read back, so it cannot be reused, and a wasted 256-bit block is a
         * better outcome than a board that has to be scrapped. */
        if (mpx_efuse_burn_record(&next, secret, &block) != ESP_OK) {
            reply_error(cmd, "burn_failed", mpx_provision_state());
            return;
        }
    }

    /* RSA-OAEP draws from esp_fill_random too, so the source has to stay
     * enabled until the blob exists. */
    const esp_err_t sealed = derive_and_seal(robot_id, kv,
                                             action == MPX_PROV_SEAL ? NULL : secret,
                                             MPX_KEY_LEN, blob);
    if (need_bootloader_rng) {
        bootloader_random_disable();
    }
    memset(secret, 0, sizeof secret);
    if (sealed != ESP_OK) {
        reply_error(cmd, "seal_failed", mpx_provision_state());
        return;
    }

    (void)store_blob(blob);
    (void)save_u32(kNvsKv, kv);

    hex_encode(blob_hex, blob, sizeof blob);
    snprintf(out, sizeof out,
             "{\"ok\":true,\"cmd\":\"%s\",\"state\":\"%s\",\"id\":\"%s\","
             "\"kv\":%u,\"key_block\":%d,\"key_source\":\"%s\","
             "\"app_version\":\"%s\",\"blob\":\"%s\"}",
             cmd, mpx_provision_state(), robot_id, (unsigned)kv, (int)block,
             have_secret ? "supplied" : "generated",
             esp_app_get_description() ? esp_app_get_description()->version : "",
             blob_hex);
    reply(out);
}

static void handle_lock(const char *body)
{
    char confirm[8];

    if (mpx_json_get_string(body, "confirm", confirm, sizeof confirm) !=
            MPX_JSON_STRING ||
        strcmp(confirm, "LOCK") != 0) {
        /* A lock cannot be undone and it is the one command whose accidental
         * invocation is expensive, so it needs a word that cannot be typed by
         * reflex. */
        reply_error("lock", "confirm_required", mpx_provision_state());
        return;
    }
    if (strcmp(mpx_provision_state(), "sealed") != 0) {
        reply_error("lock", "not_sealed", mpx_provision_state());
        return;
    }
    if (mpx_efuse_lock_all() != ESP_OK) {
        reply_error("lock", "lock_failed", mpx_provision_state());
        return;
    }
    reply("{\"ok\":true,\"cmd\":\"lock\",\"state\":\"sealed\"}");
}

static void handle_line(const char *line)
{
    const char *body = mpx_prov_command_body(line);
    char cmd[16];

    if (body == NULL || body[0] != '{') {
        /* A bare `@MPXPROV` is a ping: the station uses it to find the port. */
        if (body != NULL) {
            handle_status();
        }
        return;
    }
    if (mpx_json_get_string(body, "cmd", cmd, sizeof cmd) != MPX_JSON_STRING) {
        reply_error("unknown", "bad_request", mpx_provision_state());
        return;
    }
    if (strcmp(cmd, "status") == 0) {
        handle_status();
    } else if (strcmp(cmd, "provision") == 0) {
        handle_provision(body, false);
    } else if (strcmp(cmd, "reseal") == 0) {
        handle_provision(body, true);
    } else if (strcmp(cmd, "lock") == 0) {
        handle_lock(body);
    } else {
        reply_error(cmd, "unknown_command", mpx_provision_state());
    }
}

static void prov_task(void *arg)
{
    char line[MPX_PROV_LINE_MAX];
    size_t len = 0;
    bool overflow = false;

    (void)arg;
    ESP_LOGW(TAG, "provisioning listener on UART0 (115200 8N1), prefix @MPXPROV");

    for (;;) {
        uint8_t byte;

        if (uart_read_bytes(UART_NUM_0, &byte, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (byte == '\n' || byte == '\r') {
            if (len > 0 && !overflow) {
                line[len] = '\0';
                handle_line(line);
            }
            len = 0;
            overflow = false;
            continue;
        }
        if (len + 1 >= sizeof line) {
            /* Silently truncating would let a long line end in a valid command.
             * Drop the whole line instead and say so once. */
            overflow = true;
            ESP_LOGW(TAG, "line too long, discarded");
            continue;
        }
        line[len++] = (char)byte;
    }
}

esp_err_t mpx_provision_init(void)
{
    static bool started = false;
    const uart_config_t cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    if (started) {
        return ESP_OK;
    }

    /* The console may already own this port (it usually does). Reconfiguring it
     * is unnecessary then; reading still works through the installed driver. */
    if (!uart_is_driver_installed(UART_NUM_0)) {
        esp_err_t err = uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
            return err;
        }
        err = uart_param_config(UART_NUM_0, &cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    /* 8 KiB: RSA-3072 OAEP plus an mbedTLS bignum op is the deepest stack in the
     * firmware, and a provisioning run that overflows it would fail after the
     * fuses are already burned. */
    if (xTaskCreate(prov_task, "mpx_prov", 8192, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "provisioning task could not be created");
        return ESP_ERR_NO_MEM;
    }
    started = true;
    return ESP_OK;
}

#else /* CONFIG_APP_PROVISIONING_ENABLE */

const char *mpx_provision_state(void)
{
    /* Still answer honestly on a ship build: the HTTP status page and the boot
     * log want to know whether the unit is sealed even when the listener is
     * compiled out. */
    mpx_efuse_record_t rec;
    uint8_t block = 0;
    const bool have_record = mpx_efuse_read_record(&rec) == ESP_OK;
    const bool have_key = mpx_efuse_find_hmac_key(&block);

    return mpx_prov_state_name(have_key, have_record);
}

bool mpx_provision_activated(void)
{
    nvs_handle_t handle;
    uint8_t value = 0;

    if (nvs_open("mpx_id", NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    (void)nvs_get_u8(handle, "activated", &value);
    nvs_close(handle);
    return value != 0;
}

esp_err_t mpx_provision_set_activated(bool activated)
{
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open("mpx_id", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(handle, "activated", activated ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

void mpx_provision_set_rf_ready(void)
{
}

esp_err_t mpx_provision_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_APP_PROVISIONING_ENABLE */
