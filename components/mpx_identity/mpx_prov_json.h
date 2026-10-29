/* Field reader for the `@MPXPROV` lines (§6.6).
 *
 * The robot parses three fixed commands, so this is not a JSON library and must
 * not become one: it finds `"key"` and reads the value that follows, with no
 * escape handling, no nesting and no allocation. The station tool is the only
 * writer, and the commands are constants, so a full parser would be code with
 * no way to be exercised. It is ESP-free so the host tests can run it.
 */
#ifndef MPX_PROV_JSON_H
#define MPX_PROV_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MPX_JSON_NONE = 0,  /* key absent, or its value has the wrong shape */
    MPX_JSON_STRING,
    MPX_JSON_NUMBER,
    MPX_JSON_BOOL,
} mpx_json_type_t;

/* Copies the string value of `key` into `out` (NUL-terminated). Returns
 * MPX_JSON_STRING on success, MPX_JSON_NONE otherwise; `out` is then "". A
 * string longer than `out_len - 1` counts as absent rather than being
 * truncated: a half-copied robot ID or nonce would be worse than a refusal. */
mpx_json_type_t mpx_json_get_string(const char *json, const char *key,
                                    char *out, size_t out_len);

/* Reads an unsigned decimal integer. Returns true on success. Refuses
 * negatives, fractions and anything that would overflow. */
bool mpx_json_get_u32(const char *json, const char *key, uint32_t *out);

/* Reads `true`/`false`. Returns true when the key was present. */
bool mpx_json_get_bool(const char *json, const char *key, bool *out);

/* True when the line starts with the protocol prefix, i.e. it is addressed to
 * us rather than being a normal log line on the same console. */
bool mpx_prov_is_command(const char *line);

/* The text after the prefix, skipping spaces. NULL when the line carries no
 * prefix. */
const char *mpx_prov_command_body(const char *line);

#ifdef __cplusplus
}
#endif

#endif /* MPX_PROV_JSON_H */
