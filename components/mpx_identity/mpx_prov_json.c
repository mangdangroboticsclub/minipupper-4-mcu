#include "mpx_prov_json.h"

#include <string.h>

/* The line prefix. It exists so a provisioning command is obvious in the boot
 * log, which is the same UART the commands arrive on (§6.6). */
static const char kPrefix[] = "@MPXPROV";
#define PREFIX_LEN (sizeof kPrefix - 1)

bool mpx_prov_is_command(const char *line)
{
    return line != NULL && strncmp(line, kPrefix, PREFIX_LEN) == 0;
}

const char *mpx_prov_command_body(const char *line)
{
    const char *p;

    if (!mpx_prov_is_command(line)) {
        return NULL;
    }
    p = line + PREFIX_LEN;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

/* Position just past `"key"`, or NULL. The colon is skipped too, so the caller
 * lands on the value. */
static const char *find_value(const char *json, const char *key)
{
    char pattern[48];
    size_t key_len;
    const char *hit;

    if (json == NULL || key == NULL) {
        return NULL;
    }
    key_len = strlen(key);
    if (key_len + 3 > sizeof pattern) {
        return NULL;
    }
    pattern[0] = '"';
    memcpy(pattern + 1, key, key_len);
    pattern[key_len + 1] = '"';
    pattern[key_len + 2] = '\0';

    hit = strstr(json, pattern);
    if (hit == NULL) {
        return NULL;
    }
    hit += key_len + 2;
    while (*hit == ' ' || *hit == '\t') {
        hit++;
    }
    if (*hit != ':') {
        return NULL;
    }
    hit++;
    while (*hit == ' ' || *hit == '\t') {
        hit++;
    }
    return hit;
}

mpx_json_type_t mpx_json_get_string(const char *json, const char *key,
                                    char *out, size_t out_len)
{
    const char *v;
    size_t len = 0;

    if (out == NULL || out_len == 0) {
        return MPX_JSON_NONE;
    }
    out[0] = '\0';

    v = find_value(json, key);
    if (v == NULL || *v != '"') {
        return MPX_JSON_NONE;
    }
    v++;

    /* Measure first, copy second. Writing as we scan would leave a truncated
     * value in `out` when the buffer is too small, and a truncated robot ID is
     * worse than no robot ID: the station would provision under an ID that was
     * never allocated. */
    while (v[len] != '\0' && v[len] != '"') {
        /* A quote or backslash inside the value would need real escape
         * handling, which is exactly what this parser does not do. */
        if (v[len] == '\\') {
            return MPX_JSON_NONE;
        }
        len++;
    }
    if (v[len] != '"' || len + 1 > out_len) {
        return MPX_JSON_NONE;
    }
    memcpy(out, v, len);
    out[len] = '\0';
    return MPX_JSON_STRING;
}

bool mpx_json_get_u32(const char *json, const char *key, uint32_t *out)
{
    const char *v;
    uint32_t value = 0;
    bool any = false;

    if (out == NULL) {
        return false;
    }
    v = find_value(json, key);
    if (v == NULL || *v < '0' || *v > '9') {
        return false;
    }

    while (*v >= '0' && *v <= '9') {
        const uint32_t digit = (uint32_t)(*v - '0');
        if (value > (UINT32_MAX - digit) / 10u) {
            return false;
        }
        value = value * 10u + digit;
        any = true;
        v++;
    }
    if (!any) {
        return false;
    }
    /* "1.5" and "1e3" are not integers. */
    if (*v == '.' || *v == 'e' || *v == 'E') {
        return false;
    }
    *out = value;
    return true;
}

bool mpx_json_get_bool(const char *json, const char *key, bool *out)
{
    const char *v;

    if (out == NULL) {
        return false;
    }
    v = find_value(json, key);
    if (v == NULL) {
        return false;
    }
    if (strncmp(v, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(v, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}
