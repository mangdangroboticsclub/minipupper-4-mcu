/* The eFuse identity record, as bytes (docs/ROBOT_IDENTITY_AND_KEYS.md §6.2).
 *
 * No ESP headers: the layout is the part that must never drift between the
 * device that burns it, the station that reads it back and the host tests that
 * pin it down, so it lives in a file a laptop can compile.
 *
 *     offset size field
 *     0      16   robot_id, ASCII, zero-padded (legacy short IDs allowed)
 *     16     1    format, 0x01
 *     17     1    flags, bit0 = dev unit, bit1 = replacement board
 *     18     2    kv_base, big-endian
 *
 * The bytes here are what mpx_efuse.c packs into the four eFuse fields; the
 * field boundaries are in main/esp_efuse_custom_table.csv.
 */
#ifndef MPX_EFUSE_RECORD_H
#define MPX_EFUSE_RECORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mpx_id.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MPX_EFUSE_FORMAT_V1        0x01
#define MPX_EFUSE_FLAG_DEV         0x01  /* bit0: DEV- unit, never a customer */
#define MPX_EFUSE_FLAG_REPLACEMENT 0x02  /* bit1: this board replaced another */

#define MPX_EFUSE_ID_OFF     0
#define MPX_EFUSE_FORMAT_OFF 16
#define MPX_EFUSE_FLAGS_OFF  17
#define MPX_EFUSE_KVBASE_OFF 18
#define MPX_EFUSE_RECORD_LEN 20

typedef struct {
    char     robot_id[MPX_ID_STR_LEN + 1]; /* NUL-terminated, may be short */
    uint8_t  format;                       /* MPX_EFUSE_FORMAT_V1 when written */
    uint8_t  flags;
    uint16_t kv_base;
} mpx_efuse_record_t;

/* Serialises `rec`. Returns 0, or -1 when the record cannot be a valid one:
 * empty or oversized ID, wrong format, kv_base 0 (a robot always has a version
 * 1 or higher), or unknown flag bits (so a newer firmware's record is not
 * silently reinterpreted). */
int mpx_efuse_record_encode(uint8_t out[MPX_EFUSE_RECORD_LEN],
                            const mpx_efuse_record_t *rec);

/* Parses `in` into `rec`. Returns 0 only when the bytes describe a record this
 * firmware understands; anything else is -1. In particular an all-zero block
 * is not valid -- that is what an unburned eFuse reads as, and pretending
 * otherwise would mean deriving a key from an empty ID. */
int mpx_efuse_record_decode(mpx_efuse_record_t *rec,
                            const uint8_t in[MPX_EFUSE_RECORD_LEN]);

/* True when the block holds a record (§6.2 read path): format is 0x01 and the
 * ID is a valid production ID. Deliberately stricter than decode(), which also
 * accepts the legacy dev IDs. */
bool mpx_efuse_record_is_provisioned(const uint8_t in[MPX_EFUSE_RECORD_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* MPX_EFUSE_RECORD_H */
