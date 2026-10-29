/* Derive the robot's key and seal it to the embedded KMS public key.
 *
 * Shared by the two entry points that need a sealed blob: the bench listener
 * (which can hand in a just-burned secret, because the HMAC peripheral only
 * sees a key after a reset) and first-boot enrolment (which derives through the
 * peripheral like every later boot does).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "mpx_seal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* `secret` is the per-chip secret S when the caller holds it in RAM (a burn in
 * this boot), otherwise NULL to derive through the HMAC peripheral.
 *
 * `key_source` is set to "supplied" or "efuse" so the caller can tell the
 * station which happened: a factory that supplied S needs to know the unit did
 * not silently generate its own. Either pointer may be NULL. */
esp_err_t mpx_seal_key(const char *robot_id, uint32_t kv,
                       const uint8_t *secret, size_t secret_len,
                       uint8_t out_blob[MPX_SEAL_BLOB_LEN],
                       const char **key_source);

#ifdef __cplusplus
}
#endif
