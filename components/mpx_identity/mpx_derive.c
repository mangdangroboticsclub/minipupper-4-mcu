#include "mpx_derive.h"

#include "mpx_id.h"

#include <string.h>

static void secure_zero(void *buf, size_t len)
{
    /* volatile, so the compiler cannot drop the stores as dead: this wipes a
     * derived key, which is exactly the kind of write that gets optimised. */
    volatile uint8_t *p = (volatile uint8_t *)buf;

    for (size_t i = 0; i < len; i++) {
        p[i] = 0;
    }
}

int mpx_derive_msg(uint8_t out[MPX_DERIVE_MSG_LEN], const char *robot_id,
                   uint32_t key_version)
{
    size_t i;

    if (out == NULL || robot_id == NULL) {
        return -1;
    }

    i = 0;
    memcpy(out + i, MPX_DERIVE_LABEL, MPX_DERIVE_LABEL_LEN);
    i += MPX_DERIVE_LABEL_LEN;
    out[i++] = 0x00;
    if (!mpx_id_to_frame(out + i, robot_id)) {
        return -1;
    }
    i += MPX_ID_STR_LEN;
    out[i++] = (uint8_t)((key_version >> 24) & 0xff);
    out[i++] = (uint8_t)((key_version >> 16) & 0xff);
    out[i++] = (uint8_t)((key_version >> 8) & 0xff);
    out[i++] = (uint8_t)(key_version & 0xff);

    return (i == (size_t)MPX_DERIVE_MSG_LEN) ? 0 : -1;
}

int mpx_derive_key(mpx_hmac_sha256_fn hmac,
                   const uint8_t *secret, size_t secret_len,
                   const char *robot_id, uint32_t key_version,
                   uint8_t out[MPX_KEY_LEN])
{
    uint8_t msg[MPX_DERIVE_MSG_LEN];
    int rc;

    /* `secret` may legitimately be NULL: the eFuse adapter (mpx_efuse_hmac_sha256)
     * takes the secret from its HMAC_UP key slot and ignores the argument, which
     * is the only way it can work -- that block is RD_DIS, so there are no bytes
     * to pass. Requiring a secret here made every provisioned unit fail to load
     * its identity at boot (no K_v -> no chat pipeline -> the installer's
     * /v1/lua/enqueue answers 503 forever). A NULL `secret` with a non-zero
     * length is still a caller bug: no adapter can honour it. */
    if (hmac == NULL || out == NULL || (secret == NULL && secret_len != 0)) {
        return -1;
    }
    if (mpx_derive_msg(msg, robot_id, key_version) != 0) {
        return -1;
    }

    rc = hmac(secret, secret_len, msg, sizeof(msg), out);
    secure_zero(msg, sizeof(msg));
    return (rc == 0) ? 0 : -1;
}

int mpx_derive_key_fp(mpx_hmac_sha256_fn hmac, const uint8_t key[MPX_KEY_LEN],
                      char out_hex[17])
{
    static const char hex[] = "0123456789abcdef";
    uint8_t mac[MPX_KEY_LEN];
    int rc;

    if (hmac == NULL || key == NULL || out_hex == NULL) {
        return -1;
    }

    rc = hmac(key, MPX_KEY_LEN,
              (const uint8_t *)MPX_KEYFP_LABEL, MPX_KEYFP_LABEL_LEN, mac);
    if (rc != 0) {
        return -1;
    }

    for (size_t i = 0; i < 8; i++) {
        out_hex[2 * i]     = hex[(mac[i] >> 4) & 0x0f];
        out_hex[2 * i + 1] = hex[mac[i] & 0x0f];
    }
    out_hex[16] = '\0';

    secure_zero(mac, sizeof(mac));
    return 0;
}
