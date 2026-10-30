/* Key derivation (§6.3). No ESP headers, and no HMAC implementation either:
 * the caller passes the primitive in.
 *
 * That indirection is the whole point. On a provisioned robot the secret `S`
 * lives in an `HMAC_UP` eFuse block that the CPU can use but no code can read,
 * so the HMAC has to be `esp_hmac_calculate`. On a laptop there is no such
 * block, so the same function takes `mbedtls_md_hmac` or OpenSSL. Both run this
 * byte-layout code, which is the part that must never drift from the cloud's
 * Python.
 */
#ifndef MPX_DERIVE_H
#define MPX_DERIVE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Frozen labels (§12.1 #3). Changing either silently invalidates every key
 * ever derived for every robot already in the field, so they are literals and
 * the host tests assert them. */
#define MPX_DERIVE_LABEL   "mpx/v1/robot-key"   /* 16 bytes */
#define MPX_DERIVE_LABEL_LEN 16
#define MPX_KEYFP_LABEL    "mpx/v1/key-fp"      /* 13 bytes */
#define MPX_KEYFP_LABEL_LEN 13

#define MPX_KEY_LEN      32
#define MPX_DERIVE_MSG_LEN 37   /* 16 label + 1 NUL + 16 id + 4 kv */

/* HMAC-SHA256: `out` is 32 bytes. Returns 0 on success. */
typedef int (*mpx_hmac_sha256_fn)(const uint8_t *key, size_t key_len,
                                  const uint8_t *msg, size_t msg_len,
                                  uint8_t out[MPX_KEY_LEN]);

/* Builds the derivation message:
 *
 *     "mpx/v1/robot-key" 0x00 <robot_id as 16 bytes> uint32_be(kv)
 *
 * Returns 0 on success, -1 if the ID is missing or does not fit. A short
 * legacy dev ID is zero-padded, which is why the same 37 bytes come out of
 * "MPX-DOG-01" as the device computed for it. */
int mpx_derive_msg(uint8_t out[MPX_DERIVE_MSG_LEN], const char *robot_id,
                   uint32_t key_version);

/* K_kv = HMAC-SHA256(S, msg). `secret` is the 32-byte HMAC_UP block on a
 * provisioned unit; on a dev bench it is a test vector.
 *
 * `secret` may be NULL (with secret_len 0) for an adapter that holds the secret
 * itself -- mpx_efuse_hmac_sha256 is exactly that: the block is RD_DIS, so the
 * key never enters RAM and the adapter derives through the HMAC peripheral.
 * NULL with a non-zero length is rejected. */
int mpx_derive_key(mpx_hmac_sha256_fn hmac,
                   const uint8_t *secret, size_t secret_len,
                   const char *robot_id, uint32_t key_version,
                   uint8_t out[MPX_KEY_LEN]);

/* The 16-hex-character fingerprint: HMAC-SHA256(K_kv, "mpx/v1/key-fp")[:8].
 * Safe to log and to compare on a provisioning station. */
int mpx_derive_key_fp(mpx_hmac_sha256_fn hmac, const uint8_t key[MPX_KEY_LEN],
                      char out_hex[17]);

#ifdef __cplusplus
}
#endif

#endif /* MPX_DERIVE_H */
