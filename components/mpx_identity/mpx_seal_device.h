/* RSA-OAEP encryption of the sealed blob (§6.4). Device-only.
 *
 * The public key is compiled in: a robot has exactly one thing to talk to at
 * provisioning time, and shipping the key in the image means a station cannot
 * be talked into sealing to a key it chose. The key id in the blob tells the
 * cloud which KMS key pair to unwrap with, so rotating it is a firmware release
 * rather than a protocol change (see the two slots in the key table below).
 *
 * mbedTLS is used rather than the PSA API: PSA needs extra config in this
 * IDF's mbedTLS build, whereas RSA-OAEP with a public key is a one-shot
 * operation that the classic API already provides.
 */
#ifndef MPX_SEAL_DEVICE_H
#define MPX_SEAL_DEVICE_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "mpx_seal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Seals `plain` to the key with `seal_key_id` and writes the 392-byte upload
 * blob. ESP_ERR_NOT_FOUND when this image has no key with that id, so a station
 * that asks for the wrong slot fails loudly instead of sealing to nothing. */
esp_err_t mpx_seal_plaintext(const mpx_seal_plain_t *plain, uint8_t seal_key_id,
                             uint8_t out[MPX_SEAL_BLOB_LEN]);

/* True when this image embeds a key with that id. Lets the station reject a
 * request before it does any work. */
bool mpx_seal_has_key(uint8_t seal_key_id);

/* HMAC-SHA256 with a RAM key, matching `mpx_hmac_sha256_fn`. Provisioning only:
 * a key burned in this boot cannot be used by the HMAC peripheral yet, and the
 * seal cannot wait for a reboot. */
int mpx_hmac_soft(const uint8_t *key, size_t key_len, const uint8_t *msg,
                  size_t msg_len, uint8_t out[32]);

#ifdef __cplusplus
}
#endif

#endif /* MPX_SEAL_DEVICE_H */
