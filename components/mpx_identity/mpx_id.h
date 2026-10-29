/* Robot ID format checks (§6.1). No ESP headers: this file builds on a laptop
 * so tools/host_tests can hold the format to the same vectors the cloud uses.
 *
 * The ID is `MPX-` or `DEV-`, 11 Crockford-base32 symbols of entropy, and one
 * check symbol: 16 ASCII bytes total, which is what lets it drop into the
 * existing frame header with no wire change.
 */
#ifndef MPX_ID_H
#define MPX_ID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MPX_ID_STR_LEN   16   /* prefix + '-' + 11 body + 1 check */
#define MPX_ID_BODY_LEN  11
#define MPX_ID_BODY_OFF  4    /* "MPX-" */
#define MPX_ID_CHECK_OFF 15

/* The alphabet Crockford base32 uses: no I, L, O or U, so the symbols that a
 * human misreads and mistypes are simply not part of the space. */
extern const char mpx_id_alphabet[];

/* Full §6.1 validation: 16 bytes, a known prefix, alphabet symbols only, and a
 * check symbol that matches the body. Case-sensitive on purpose -- the gateway
 * compares the wire bytes exactly, and the O->0 / I,L->1 repairs belong to
 * human input, not to the firmware. */
bool mpx_id_valid(const char *id);

/* A development ID. `CONFIG_APP_ROBOT_UUID` defaults to "MPX-DOG-01", which is
 * 10 characters and predates the format, so KCONFIG builds accept any 1..16
 * printable ASCII bytes. Production never takes this path: the eFuse reader
 * demands `mpx_id_valid`. */
bool mpx_id_legacy_valid(const char *id);

/* `mpx_id_valid` or `mpx_id_legacy_valid`. This is what the identity loader
 * calls in KCONFIG builds so an existing dev board keeps booting. */
bool mpx_id_valid_or_legacy(const char *id);

/* Copies the ID into the 16-byte frame header field: verbatim, zero-padded,
 * never truncated. Returns false (and writes nothing) when the ID does not fit
 * or is NULL, because a silently truncated ID is a frame the cloud cannot
 * authenticate. */
bool mpx_id_to_frame(uint8_t out[MPX_ID_STR_LEN], const char *id);

/* The check symbol the body of `id` should end with, or -1 if `id` is not a
 * shape this format can check. Exposed for tests. */
int mpx_id_check_symbol(const char *id);

#ifdef __cplusplus
}
#endif

#endif /* MPX_ID_H */
