#include "mpx_seal.h"

#include <string.h>

int mpx_seal_plain_encode(uint8_t out[MPX_SEAL_PLAIN_LEN],
                          const mpx_seal_plain_t *p)
{
    size_t id_len;

    if (out == NULL || p == NULL) {
        return -1;
    }
    if (!mpx_id_valid(p->robot_id)) {
        return -1;
    }
    if (p->key_version == 0) {
        return -1; /* §6.4: version 1 is the first robot key */
    }

    id_len = strlen(p->robot_id);
    if (id_len != (size_t)MPX_ID_STR_LEN) {
        return -1;
    }

    memset(out, 0, MPX_SEAL_PLAIN_LEN);
    memcpy(out, MPX_SEAL_PLAIN_MAGIC, 4);
    out[4] = MPX_SEAL_FORMAT_V1;

    out[MPX_SEAL_PLAIN_KV_OFF]     = (uint8_t)((p->key_version >> 24) & 0xff);
    out[MPX_SEAL_PLAIN_KV_OFF + 1] = (uint8_t)((p->key_version >> 16) & 0xff);
    out[MPX_SEAL_PLAIN_KV_OFF + 2] = (uint8_t)((p->key_version >> 8) & 0xff);
    out[MPX_SEAL_PLAIN_KV_OFF + 3] = (uint8_t)(p->key_version & 0xff);

    memcpy(out + MPX_SEAL_PLAIN_ID_OFF, p->robot_id, id_len);
    memcpy(out + MPX_SEAL_PLAIN_NONCE_OFF, p->nonce, MPX_SEAL_NONCE_LEN);
    memcpy(out + MPX_SEAL_PLAIN_KEY_OFF, p->key, MPX_SEAL_KEY_LEN);
    memcpy(out + MPX_SEAL_PLAIN_MAC_OFF, p->mac, MPX_SEAL_MAC_LEN);
    out[MPX_SEAL_PLAIN_FLAGS_OFF]     = (uint8_t)((p->sec_flags >> 8) & 0xff);
    out[MPX_SEAL_PLAIN_FLAGS_OFF + 1] = (uint8_t)(p->sec_flags & 0xff);
    out[MPX_SEAL_PLAIN_SVER_OFF]     = (uint8_t)((p->secure_version >> 24) & 0xff);
    out[MPX_SEAL_PLAIN_SVER_OFF + 1] = (uint8_t)((p->secure_version >> 16) & 0xff);
    out[MPX_SEAL_PLAIN_SVER_OFF + 2] = (uint8_t)((p->secure_version >> 8) & 0xff);
    out[MPX_SEAL_PLAIN_SVER_OFF + 3] = (uint8_t)(p->secure_version & 0xff);

    if (p->app_version[0] != '\0') {
        size_t app_len = strlen(p->app_version);
        if (app_len > (size_t)MPX_SEAL_APPVER_LEN) {
            return -1;
        }
        /* NUL-padded, not truncated: a version string cut in half would look
         * like a different firmware to whoever is debugging a mismatch. */
        memcpy(out + MPX_SEAL_PLAIN_APPVER_OFF, p->app_version, app_len);
    }
    memcpy(out + MPX_SEAL_PLAIN_DSHASH_OFF, p->ds_pub_sha256,
           MPX_SEAL_DSHASH_LEN);

    return 0;
}

int mpx_seal_plain_decode(mpx_seal_plain_t *p,
                          const uint8_t in[MPX_SEAL_PLAIN_LEN])
{
    size_t id_len = 0;

    if (p == NULL || in == NULL) {
        return -1;
    }
    if (memcmp(in, MPX_SEAL_PLAIN_MAGIC, 4) != 0 || in[4] != MPX_SEAL_FORMAT_V1) {
        return -1;
    }

    memset(p, 0, sizeof *p);
    p->key_version = ((uint32_t)in[MPX_SEAL_PLAIN_KV_OFF] << 24) |
                     ((uint32_t)in[MPX_SEAL_PLAIN_KV_OFF + 1] << 16) |
                     ((uint32_t)in[MPX_SEAL_PLAIN_KV_OFF + 2] << 8) |
                     (uint32_t)in[MPX_SEAL_PLAIN_KV_OFF + 3];

    while (id_len < (size_t)MPX_ID_STR_LEN && in[MPX_SEAL_PLAIN_ID_OFF + id_len] != 0) {
        id_len++;
    }
    memcpy(p->robot_id, in + MPX_SEAL_PLAIN_ID_OFF, id_len);
    p->robot_id[id_len] = '\0';

    memcpy(p->nonce, in + MPX_SEAL_PLAIN_NONCE_OFF, MPX_SEAL_NONCE_LEN);
    memcpy(p->key, in + MPX_SEAL_PLAIN_KEY_OFF, MPX_SEAL_KEY_LEN);
    memcpy(p->mac, in + MPX_SEAL_PLAIN_MAC_OFF, MPX_SEAL_MAC_LEN);
    p->sec_flags = (uint16_t)(((uint16_t)in[MPX_SEAL_PLAIN_FLAGS_OFF] << 8) |
                              (uint16_t)in[MPX_SEAL_PLAIN_FLAGS_OFF + 1]);
    p->secure_version = ((uint32_t)in[MPX_SEAL_PLAIN_SVER_OFF] << 24) |
                        ((uint32_t)in[MPX_SEAL_PLAIN_SVER_OFF + 1] << 16) |
                        ((uint32_t)in[MPX_SEAL_PLAIN_SVER_OFF + 2] << 8) |
                        (uint32_t)in[MPX_SEAL_PLAIN_SVER_OFF + 3];

    /* The app version is NUL-padded; stop at the first NUL. */
    for (size_t i = 0; i < (size_t)MPX_SEAL_APPVER_LEN; i++) {
        const char c = (char)in[MPX_SEAL_PLAIN_APPVER_OFF + i];
        p->app_version[i] = c;
        if (c == '\0') {
            break;
        }
    }
    p->app_version[MPX_SEAL_APPVER_LEN] = '\0';

    memcpy(p->ds_pub_sha256, in + MPX_SEAL_PLAIN_DSHASH_OFF, MPX_SEAL_DSHASH_LEN);

    return (p->key_version == 0) ? -1 : 0;
}

int mpx_seal_blob_encode(uint8_t out[MPX_SEAL_BLOB_LEN], uint8_t seal_key_id,
                         const uint8_t rsa_ciphertext[MPX_SEAL_RSA_BYTES])
{
    if (out == NULL || rsa_ciphertext == NULL) {
        return -1;
    }

    memset(out, 0, MPX_SEAL_BLOB_LEN);
    memcpy(out, MPX_SEAL_BLOB_MAGIC, 4);
    out[4] = MPX_SEAL_FORMAT_V1;
    out[5] = seal_key_id;
    /* out[6..7] reserved, already zero */
    memcpy(out + MPX_SEAL_BLOB_HEADER, rsa_ciphertext, MPX_SEAL_RSA_BYTES);
    return 0;
}

bool mpx_seal_blob_looks_valid(const uint8_t *blob, size_t len)
{
    if (blob == NULL || len != (size_t)MPX_SEAL_BLOB_LEN) {
        return false;
    }
    return memcmp(blob, MPX_SEAL_BLOB_MAGIC, 4) == 0 &&
           blob[4] == MPX_SEAL_FORMAT_V1;
}
