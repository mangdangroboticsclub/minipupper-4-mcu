/* eFuse access for a provisioned robot (§6.2, §6.5). Device-only.
 *
 * The two halves are deliberately separate functions because they are used at
 * different times and by different callers:
 *
 *   read  -- every boot, by mpx_identity_init(), through mpx_efuse_read_record()
 *            and the HMAC adapter.
 *   burn  -- once, by the provisioning station, through mpx_provision.c.
 *
 * Nothing here may be called before the eFuse driver is usable, and the burn
 * path must only run from a factory build: it is irreversible.
 */
#ifndef MPX_EFUSE_H
#define MPX_EFUSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "mpx_efuse_record.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reads BLOCK_USR_DATA into `rec`. ESP_ERR_NOT_FOUND when the block holds no
 * record this firmware understands (a blank or a dev board). */
esp_err_t mpx_efuse_read_record(mpx_efuse_record_t *rec);

/* Finds the HMAC_UP key block. True and the block index when one exists, false
 * when the key has not been burned. */
bool mpx_efuse_find_hmac_key(uint8_t *block_out);

/* Burns the record and `secret` in one batch, then write-protects both: the
 * record's WR_DIS bit so the ID can never be rewritten, the key's RD_DIS so
 * that no code (or the ROM loader, or a debugger) can read `S` back out.
 *
 * This is irreversible and must succeed or fail as a unit -- a half-burned
 * board has an ID but no key and cannot be re-provisioned without a new block.
 * On success `key_block_out` receives the key block that now holds `secret`.
 * A reboot is needed before the new key is usable by the HMAC peripheral. */
esp_err_t mpx_efuse_burn_record(const mpx_efuse_record_t *rec,
                                const uint8_t secret[32],
                                uint8_t *key_block_out);

/* HMAC-SHA256 adapter for mpx_derive_key(). `key` and `key_len` are ignored on
 * purpose: the key is in eFuse and no code may hold it, which is the entire
 * reason this project uses HMAC_UP. A caller that passes a different secret
 * gets the eFuse one anyway, so the parameter is kept only to match the typedef
 * the host tests use. */
int mpx_efuse_hmac_sha256(const uint8_t *key, size_t key_len,
                          const uint8_t *msg, size_t msg_len,
                          uint8_t out[32]);

/* Write-protects what `provision` left writable: the record, the key's purpose
 * and the RD_DIS bits themselves. Idempotent, and never writes an ID or a key,
 * so it is safe to run on a unit that was provisioned long ago (§6.6 `lock`). */
esp_err_t mpx_efuse_lock_all(void);

/* Base MAC of this board, the physical identity the cloud records next to the
 * sealed blob (§6.4). */
esp_err_t mpx_efuse_mac(uint8_t out[6]);

/* The security state a sealed blob reports. On a pilot unit this is what
 * distinguishes "skipped `lock` deliberately" from "someone shipped it
 * unlocked": the bits are read from the fuses, never from a Kconfig option. */
uint16_t mpx_efuse_sec_flags(void);

/* esp_app_desc.secure_version, the anti-rollback counter that travels in the
 * sealed plaintext. */
uint32_t mpx_efuse_secure_version(void);

#ifdef __cplusplus
}
#endif

#endif /* MPX_EFUSE_H */
