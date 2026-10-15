#include "mpx_crypto.h"

#include <cstring>

#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/gcm.h"

static const char *TAG = "crypto";

namespace crypto {

/* ── Public API ─────────────────────────────────────────────── */

bool gcm_encrypt(const uint8_t key[KEY_SIZE],
                 const uint8_t iv[IV_SIZE],
                 const uint8_t *aad, size_t aad_len,
                 const uint8_t *plaintext, size_t pt_len,
                 uint8_t *ciphertext,
                 uint8_t tag[TAG_SIZE])
{
    if (!key || !iv || !plaintext || !ciphertext || !tag) {
        ESP_LOGE(TAG, "gcm_encrypt: null argument");
        return false;
    }

    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);

    int ret = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, KEY_SIZE * 8);
    if (ret != 0) {
        ESP_LOGE(TAG, "gcm_setkey failed: %d", ret);
        mbedtls_gcm_free(&ctx);
        return false;
    }

    // The ESP32-S3 hardware AES accelerator is used transparently
    // by mbedtls when CONFIG_MBEDTLS_HARDWARE_AES is enabled (default).
    ret = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT,
                                     pt_len,
                                     iv, IV_SIZE,
                                     aad, aad_len,
                                     plaintext,
                                     ciphertext,
                                     TAG_SIZE, tag);
    if (ret != 0) {
        ESP_LOGE(TAG, "gcm_crypt_and_tag failed: %d", ret);
        mbedtls_gcm_free(&ctx);
        return false;
    }

    mbedtls_gcm_free(&ctx);
    return true;
}

bool gcm_decrypt(const uint8_t key[KEY_SIZE],
                 const uint8_t iv[IV_SIZE],
                 const uint8_t *aad, size_t aad_len,
                 const uint8_t *ciphertext, size_t ct_len,
                 const uint8_t tag[TAG_SIZE],
                 uint8_t *plaintext)
{
    if (!key || !iv || !ciphertext || !tag || !plaintext) {
        ESP_LOGE(TAG, "gcm_decrypt: null argument");
        return false;
    }

    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);

    int ret = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, KEY_SIZE * 8);
    if (ret != 0) {
        ESP_LOGE(TAG, "gcm_setkey failed: %d", ret);
        mbedtls_gcm_free(&ctx);
        return false;
    }

    ret = mbedtls_gcm_auth_decrypt(&ctx,
                                    ct_len,
                                    iv, IV_SIZE,
                                    aad, aad_len,
                                    tag, TAG_SIZE,
                                    ciphertext,
                                    plaintext);
    if (ret != 0) {
        ESP_LOGW(TAG, "gcm_auth_decrypt failed (integrity check): %d", ret);
        mbedtls_gcm_free(&ctx);
        return false;
    }

    mbedtls_gcm_free(&ctx);
    return true;
}

void random_bytes(uint8_t *buf, size_t len)
{
    if (!buf || len == 0) return;
    esp_fill_random(buf, len);
}

bool hex_to_bytes(const char *hex, uint8_t *buf, size_t buflen)
{
    if (!hex || !buf) return false;

    size_t hex_len = std::strlen(hex);
    if (hex_len != buflen * 2) return false;

    for (size_t i = 0; i < buflen; ++i) {
        char hi = hex[i * 2];
        char lo = hex[i * 2 + 1];

        auto nybble = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
            if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
            return 0xFF;
        };

        uint8_t hn = nybble(hi);
        uint8_t ln = nybble(lo);
        if (hn == 0xFF || ln == 0xFF) return false;

        buf[i] = (hn << 4) | ln;
    }

    return true;
}

}  // namespace crypto
