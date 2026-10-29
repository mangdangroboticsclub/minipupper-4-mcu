#include "mpx_seal_device.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"

static const char *TAG = "mpx_seal";

/* The embedded public key, from components/mpx_identity/certs/. One slot today:
 * slot 1 is reserved for a rotation, and a second EMBED_FILES entry is the whole
 * of adding it -- the blob carries the slot number, so old firmware keeps
 * sealing to slot 0 and new firmware can seal to slot 1. */
extern const uint8_t prov_seal_pub_pem_start[] asm("_binary_prov_seal_pub_pem_start");
extern const uint8_t prov_seal_pub_pem_end[]   asm("_binary_prov_seal_pub_pem_end");

static const uint8_t *const kKeyPemStart[] = { prov_seal_pub_pem_start };
static const uint8_t *const kKeyPemEnd[]   = { prov_seal_pub_pem_end };

#define MPX_SEAL_KEY_SLOTS 1

bool mpx_seal_has_key(uint8_t seal_key_id)
{
    return seal_key_id < MPX_SEAL_KEY_SLOTS &&
           kKeyPemStart[seal_key_id] != NULL;
}

/* mbedTLS wants a source of randomness for the OAEP seed. The hardware RNG is
 * enabled by the bootloader, so this cannot block and cannot fail. */
static int rng_cb(void *ctx, unsigned char *out, size_t len)
{
    (void)ctx;
    esp_fill_random(out, len);
    return 0;
}

esp_err_t mpx_seal_plaintext(const mpx_seal_plain_t *plain, uint8_t seal_key_id,
                             uint8_t out[MPX_SEAL_BLOB_LEN])
{
    uint8_t plaintext[MPX_SEAL_PLAIN_LEN];
    uint8_t ciphertext[MPX_SEAL_RSA_BYTES];
    /* The embedded PEM is not NUL-terminated, and mbedtls_pk_parse_public_key
     * wants a length; copy so the parser cannot read past the end. */
    char pem[1024];
    size_t pem_len;
    mbedtls_pk_context pk;
    int rc;
    esp_err_t err = ESP_FAIL;

    if (plain == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!mpx_seal_has_key(seal_key_id)) {
        ESP_LOGE(TAG, "no seal key %u in this image", (unsigned)seal_key_id);
        return ESP_ERR_NOT_FOUND;
    }
    if (mpx_seal_plain_encode(plaintext, plain) != 0) {
        ESP_LOGE(TAG, "sealed plaintext is not encodable (bad id or version)");
        return ESP_ERR_INVALID_ARG;
    }

    pem_len = (size_t)(kKeyPemEnd[seal_key_id] - kKeyPemStart[seal_key_id]);
    if (pem_len >= sizeof pem) {
        ESP_LOGE(TAG, "embedded seal key is implausibly large (%u bytes)",
                 (unsigned)pem_len);
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(pem, kKeyPemStart[seal_key_id], pem_len);
    pem[pem_len] = '\0';

    mbedtls_pk_init(&pk);
    rc = mbedtls_pk_parse_public_key(&pk, (const unsigned char *)pem, pem_len + 1);
    if (rc != 0) {
        ESP_LOGE(TAG, "embedded seal key does not parse: -0x%04x", -rc);
        goto cleanup;
    }
    if (!mbedtls_pk_can_do(&pk, MBEDTLS_PK_RSA)) {
        ESP_LOGE(TAG, "embedded seal key is not an RSA key");
        goto cleanup;
    }
    if (mbedtls_pk_get_bitlen(&pk) != MPX_SEAL_RSA_BYTES * 8) {
        ESP_LOGE(TAG, "embedded seal key is %u bits, expected %u",
                 (unsigned)mbedtls_pk_get_bitlen(&pk),
                 (unsigned)(MPX_SEAL_RSA_BYTES * 8));
        goto cleanup;
    }

    mbedtls_rsa_context *rsa = mbedtls_pk_rsa(pk);
    rc = mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA256);
    if (rc != 0) {
        ESP_LOGE(TAG, "could not select OAEP-SHA256: -0x%04x", -rc);
        goto cleanup;
    }
    /* RSA-3072 is a public-key operation with e = 65537, so this is ~1 ms of
     * bignum work; no need to offload it or to worry about the watchdog. */
    rc = mbedtls_rsa_rsaes_oaep_encrypt(rsa, rng_cb, NULL, NULL, 0,
                                        MPX_SEAL_PLAIN_LEN, plaintext, ciphertext);
    if (rc != 0) {
        ESP_LOGE(TAG, "OAEP encryption failed: -0x%04x", -rc);
        goto cleanup;
    }

    if (mpx_seal_blob_encode(out, seal_key_id, ciphertext) != 0) {
        goto cleanup;
    }
    err = ESP_OK;

cleanup:
    mbedtls_pk_free(&pk);
    /* The plaintext holds K_kv; do not leave it in the task stack. */
    memset(plaintext, 0, sizeof plaintext);
    memset(pem, 0, sizeof pem);
    if (err != ESP_OK) {
        memset(out, 0, MPX_SEAL_BLOB_LEN);
    }
    return err;
}

int mpx_hmac_soft(const uint8_t *key, size_t key_len, const uint8_t *msg,
                  size_t msg_len, uint8_t out[32])
{
    /* Software HMAC-SHA256 over a secret held in RAM. Only the provisioning
     * path uses it, for the one case the hardware cannot serve: a key that was
     * burned seconds ago is not usable by the HMAC peripheral until the next
     * boot, yet the seal has to be produced in the same session or the operator
     * has to power-cycle a board that is already irreversible.
     *
     * Never call this with a secret read from eFuse -- there is nothing to read
     * (RD_DIS), and using it would defeat the point of HMAC_UP. */
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    int rc;

    if (info == NULL) {
        return -1;
    }
    rc = mbedtls_md_hmac(info, key, key_len, msg, msg_len, out);
    return (rc == 0) ? 0 : -1;
}
