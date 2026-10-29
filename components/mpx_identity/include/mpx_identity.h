/* Robot identity: the 16-byte ID and the 32-byte AES key every other module
 * asks for.
 *
 * Why this exists at all: before it, `CONFIG_APP_ROBOT_UUID` and
 * `CONFIG_APP_CHAT_AES_KEY_HEX` were read directly in six files, in three
 * different encodings (a `constexpr char *`, a hex string parsed on use, and a
 * `#define` fallback to "MPX-DOG-01"). The key is also derivable on a
 * production unit, where neither Kconfig symbol exists. Both facts live behind
 * this one API so the source of the identity becomes a build- and runtime
 * decision instead of something every call site re-decides.
 *
 * Two sources, chosen by Kconfig (`choice APP_IDENTITY_SOURCE`):
 *
 *   KCONFIG  development. The ID and key are the Kconfig strings. Everything
 *            works as it did before this component existed.
 *   EFUSE    production. The ID is in `BLOCK_USR_DATA` and the key is derived
 *            from an `HMAC_UP` block that the CPU can use but never read.
 *
 * See docs/ROBOT_IDENTITY_AND_KEYS.md §6 and §8.1.
 */
#ifndef MPX_IDENTITY_H
#define MPX_IDENTITY_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The ID is exactly this wide because `robot_uuid` in the upstream binary
 * frame is 16 bytes (main/network/chat_ws.cc). The cloud uses those bytes as
 * AES-GCM AAD, so they are compared verbatim -- never normalised, never
 * NUL-stripped on the wire. */
#define MPX_ID_LEN      16
#define MPX_KEY_LEN     32
#define MPX_KEY_FP_LEN  16   /* hex characters, no NUL */

typedef enum {
    MPX_ID_UNPROVISIONED = 0,  /* no usable ID/key: cloud features are off  */
    MPX_ID_KCONFIG,            /* development build                         */
    MPX_ID_EFUSE,              /* provisioned: USR_DATA record + HMAC_UP key */
    MPX_ID_INCONSISTENT,       /* a record exists but does not add up       */
} mpx_id_state_t;

/* Reads the identity once and caches it. Call after `init_nvs()` and before
 * anything that needs an ID or a key (chat, agent, WASM, HTTP, pi_link).
 *
 * Returns ESP_OK when an identity was loaded, ESP_ERR_INVALID_STATE when the
 * build is unprovisioned (the robot still boots and still walks), and
 * ESP_ERR_NOT_SUPPORTED if the configured source is not implemented. */
esp_err_t mpx_identity_init(void);

mpx_id_state_t mpx_identity_state(void);

/* "" unless provisioned. NUL-terminated, never longer than 16 characters. */
const char *mpx_identity_robot_id(void);

/* Copies the 32-byte AES-256 key. The caller owns the buffer and must zero it
 * when done (`secure_zero`), because this is the robot's root secret. Returns
 * false when the identity is not usable, in which case `out` is untouched. */
bool mpx_identity_robot_key(uint8_t out[MPX_KEY_LEN]);

/* The ID as the 16 bytes the frame header wants: copied verbatim and
 * zero-padded, so a short legacy dev ID becomes the same AAD the old
 * `strncpy` produced. False when unprovisioned. */
bool mpx_identity_frame_id(uint8_t out[MPX_ID_LEN]);

/* Key version. 1 until a unit has been re-provisioned or rotated (§6.9 C3);
 * it is part of the derivation message, so it must be the same number the
 * cloud used. */
uint32_t mpx_identity_key_version(void);

/* 16 hex characters identifying the key without revealing it. Two robots that
 * log the same fingerprint hold the same key, which is the only thing support
 * ever needs to know. Always 16 characters; "" before init. */
const char *mpx_identity_key_fp(void);

/* Security-relevant eFuse bits recorded at sealing time (§6.4), 0 in KCONFIG
 * builds. */
uint16_t mpx_identity_sec_flags(void);

#ifdef __cplusplus
}
#endif

#endif /* MPX_IDENTITY_H */
