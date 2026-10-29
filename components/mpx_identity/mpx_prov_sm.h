/* The `provision` state table (§6.6), as a pure function.
 *
 * This is the part that decides whether a board gets burned, re-sealed, or
 * refused, and it is the part that is expensive to get wrong: the wrong answer
 * either bricks a board's identity or, worse, seals a key for an ID the fuses
 * do not carry. So the table is ESP-free and covered by host tests; the device
 * file only supplies the three facts it needs and executes the answer.
 */
#ifndef MPX_PROV_SM_H
#define MPX_PROV_SM_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /* Nothing burned: generate S, burn HMAC_UP, burn the record, derive, seal. */
    MPX_PROV_BURN_ALL = 0,
    /* S already there (power was lost mid-provision): burn the record only. */
    MPX_PROV_BURN_RECORD,
    /* A record but no usable key: burn S for the ID already in the fuses. */
    MPX_PROV_BURN_KEY,
    /* Both present and consistent: derive again and seal (a `reseal`). */
    MPX_PROV_SEAL,
    /* Both present but for a different ID: refuse; re-reserving is the cloud's
     * decision, not the firmware's (§6.6, D15). */
    MPX_PROV_REFUSE_ID_MISMATCH,
    /* A record exists but does not decode, or the requested ID is malformed.
     * Burning anything here would make the board's identity a guess. */
    MPX_PROV_REFUSE_BAD_STATE,
} mpx_prov_action_t;

/* Decides what `provision` does.
 *
 * `have_key`    -- an HMAC_UP block exists in eFuse
 * `have_record` -- a decodable ID record exists in USR_DATA
 * `id_matches`  -- the requested ID equals the burned one (only meaningful when
 *                  `have_record` is true)
 * `request_valid` -- the requested ID parses as a production ID (or a dev ID
 *                  when `allow_dev_ids`) and kv is in range
 */
mpx_prov_action_t mpx_prov_decide(bool have_key, bool have_record,
                                  bool id_matches, bool request_valid);

/* The state string reported by `status`: what a station prints and what the
 * operator reads off the bench. "partial" means the board is half-burned. */
const char *mpx_prov_state_name(bool have_key, bool have_record);

/* The error string a refusal reports, for the JSON reply. */
const char *mpx_prov_error_name(mpx_prov_action_t action);

#ifdef __cplusplus
}
#endif

#endif /* MPX_PROV_SM_H */
