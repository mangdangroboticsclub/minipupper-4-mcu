#include "mpx_efuse_record.h"

#include <string.h>

int mpx_efuse_record_encode(uint8_t out[MPX_EFUSE_RECORD_LEN],
                            const mpx_efuse_record_t *rec)
{
    size_t len;

    if (out == NULL || rec == NULL) {
        return -1;
    }
    if (rec->format != MPX_EFUSE_FORMAT_V1) {
        return -1;
    }
    /* Only the two flags this firmware knows. A record with other bits set was
     * written by a newer build; burning or claiming to understand it would be
     * a guess. */
    if ((rec->flags & (uint8_t)~(MPX_EFUSE_FLAG_DEV | MPX_EFUSE_FLAG_REPLACEMENT)) != 0) {
        return -1;
    }
    if (rec->kv_base == 0) {
        return -1;
    }
    if (!mpx_id_valid_or_legacy(rec->robot_id)) {
        return -1;
    }

    len = strlen(rec->robot_id);

    memset(out, 0, MPX_EFUSE_RECORD_LEN);
    memcpy(out + MPX_EFUSE_ID_OFF, rec->robot_id, len);
    out[MPX_EFUSE_FORMAT_OFF] = rec->format;
    out[MPX_EFUSE_FLAGS_OFF]  = rec->flags;
    out[MPX_EFUSE_KVBASE_OFF]     = (uint8_t)((rec->kv_base >> 8) & 0xff);
    out[MPX_EFUSE_KVBASE_OFF + 1] = (uint8_t)(rec->kv_base & 0xff);

    return 0;
}

int mpx_efuse_record_decode(mpx_efuse_record_t *rec,
                            const uint8_t in[MPX_EFUSE_RECORD_LEN])
{
    size_t len = 0;

    if (rec == NULL || in == NULL) {
        return -1;
    }
    if (in[MPX_EFUSE_FORMAT_OFF] != MPX_EFUSE_FORMAT_V1) {
        return -1;
    }
    if ((in[MPX_EFUSE_FLAGS_OFF] &
         (uint8_t)~(MPX_EFUSE_FLAG_DEV | MPX_EFUSE_FLAG_REPLACEMENT)) != 0) {
        return -1;
    }

    /* The ID field is zero-padded, so the string ends at the first NUL. A field
     * with no NUL at all is all 16 bytes of ID, which is the production case. */
    while (len < (size_t)MPX_ID_STR_LEN && in[MPX_EFUSE_ID_OFF + len] != 0) {
        len++;
    }
    if (len == 0) {
        return -1;
    }
    memcpy(rec->robot_id, in + MPX_EFUSE_ID_OFF, len);
    rec->robot_id[len] = '\0';

    if (!mpx_id_valid_or_legacy(rec->robot_id)) {
        return -1;
    }

    rec->format  = in[MPX_EFUSE_FORMAT_OFF];
    rec->flags   = in[MPX_EFUSE_FLAGS_OFF];
    rec->kv_base = (uint16_t)(((uint16_t)in[MPX_EFUSE_KVBASE_OFF] << 8) |
                              (uint16_t)in[MPX_EFUSE_KVBASE_OFF + 1]);
    if (rec->kv_base == 0) {
        return -1;
    }

    return 0;
}

bool mpx_efuse_record_is_provisioned(const uint8_t in[MPX_EFUSE_RECORD_LEN])
{
    mpx_efuse_record_t rec;

    if (mpx_efuse_record_decode(&rec, in) != 0) {
        return false;
    }
    /* A unit burned through the station always has a 16-byte production ID. A
     * shorter one is a dev record and must not be mistaken for a provisioned
     * pilot unit. */
    return mpx_id_valid(rec.robot_id);
}
