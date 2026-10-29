#include "mpx_seal_key.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_random.h"

#include "mpx_derive.h"
#include "mpx_efuse.h"
#include "mpx_identity.h"
#include "mpx_seal_device.h"

static const char *TAG = "mpx_seal";

/* Only the eFuse build seals anything in practice; the Kconfig-source build
 * compiles this file too, and there the symbol is not defined at all. */
#if CONFIG_APP_IDENTITY_SOURCE_EFUSE
#define SEAL_KEY_ID CONFIG_APP_PROV_SEAL_KEY_ID
#else
#define SEAL_KEY_ID 0
#endif

esp_err_t mpx_seal_key(const char *robot_id, uint32_t kv,
                       const uint8_t *secret, size_t secret_len,
                       uint8_t out_blob[MPX_SEAL_BLOB_LEN],
                       const char **key_source)
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
        if (key_source != NULL) {
            *key_source = "supplied";
        }
    } else {
        if (mpx_derive_key(mpx_efuse_hmac_sha256, NULL, 0, robot_id, kv, key) != 0) {
            ESP_LOGE(TAG, "derive through the HMAC peripheral failed (key block not usable?)");
            return ESP_FAIL;
        }
        if (key_source != NULL) {
            *key_source = "efuse";
        }
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

    err = mpx_seal_plaintext(&plain, SEAL_KEY_ID, out_blob);
    memset(&plain, 0, sizeof plain);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "seal failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "sealed %s kv=%u key_fp=%s (secure_version=%u)",
             robot_id, (unsigned)kv, key_fp, (unsigned)mpx_efuse_secure_version());
    return ESP_OK;
}
