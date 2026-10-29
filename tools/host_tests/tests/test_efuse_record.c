/* The eFuse identity record layout (§6.2).
 *
 * These bytes are written once, are not rewritable, and are what the robot later
 * parses back to find out who it is. A field moving by one byte would burn a
 * fleet's worth of boards with the wrong identity, so the layout is pinned two
 * ways: against the frozen JSON vectors (shared with tools/mpx_seal.py) and
 * against the offsets in the header.
 */
#include "mpx_efuse_record.h"

#include "mpx_seal_vectors.h"
#include "test_harness.h"

#include <string.h>

static void hex_to_bytes(uint8_t *out, size_t out_len, const char *hex)
{
    for (size_t i = 0; i < out_len; i++) {
        const char hi = hex[i * 2];
        const char lo = hex[i * 2 + 1];
        const uint8_t h = (uint8_t)((hi >= 'a') ? (hi - 'a' + 10) : (hi - '0'));
        const uint8_t l = (uint8_t)((lo >= 'a') ? (lo - 'a' + 10) : (lo - '0'));
        out[i] = (uint8_t)((h << 4) | l);
    }
}

TEST_CASE(efuse_record_matches_frozen_vectors)
{
    for (size_t i = 0; i < (size_t)MPX_SEAL_VEC_RECORD_COUNT; i++) {
        uint8_t expected[MPX_EFUSE_RECORD_LEN];
        uint8_t encoded[MPX_EFUSE_RECORD_LEN];

        hex_to_bytes(expected, sizeof expected, MPX_SEAL_RECORDS[i].raw_hex);

        mpx_efuse_record_t rec;
        CHECK_EQ_INT(mpx_efuse_record_decode(&rec, expected), 0);

        CHECK_EQ_INT(mpx_efuse_record_encode(encoded, &rec), 0);
        CHECK_EQ_MEM(encoded, expected, sizeof expected);
    }
}

TEST_CASE(efuse_record_field_offsets)
{
    mpx_efuse_record_t rec;
    uint8_t raw[MPX_EFUSE_RECORD_LEN];

    memset(&rec, 0, sizeof rec);
    strcpy(rec.robot_id, "MPX-AJZ7J8Y0YTDS");
    rec.format  = MPX_EFUSE_FORMAT_V1;
    rec.flags   = MPX_EFUSE_FLAG_DEV | MPX_EFUSE_FLAG_REPLACEMENT;
    rec.kv_base = 0x1234;

    CHECK_EQ_INT(mpx_efuse_record_encode(raw, &rec), 0);

    /* 16 bytes of ID, then format, flags and a big-endian kv_base, in that
     * order, filling the 20-byte record exactly. */
    CHECK_EQ_INT(raw[MPX_EFUSE_ID_OFF], 'M');
    CHECK_EQ_INT(raw[MPX_EFUSE_ID_OFF + 15], 'S');
    CHECK_EQ_INT(raw[MPX_EFUSE_FORMAT_OFF], MPX_EFUSE_FORMAT_V1);
    CHECK_EQ_INT(raw[MPX_EFUSE_FLAGS_OFF], 0x03);
    CHECK_EQ_INT(raw[MPX_EFUSE_KVBASE_OFF], 0x12);
    CHECK_EQ_INT(raw[MPX_EFUSE_KVBASE_OFF + 1], 0x34);
    CHECK_EQ_INT(MPX_EFUSE_KVBASE_OFF + 1, MPX_EFUSE_RECORD_LEN - 1);
}

TEST_CASE(efuse_record_round_trip_short_id)
{
    mpx_efuse_record_t rec;
    mpx_efuse_record_t back;
    uint8_t raw[MPX_EFUSE_RECORD_LEN];

    /* A dev board may carry a short legacy ID; the rest of the field is zeros
     * so the record stays a fixed 20 bytes. */
    memset(&rec, 0, sizeof rec);
    strcpy(rec.robot_id, "MPX-DOG-01");
    rec.format  = MPX_EFUSE_FORMAT_V1;
    rec.flags   = MPX_EFUSE_FLAG_DEV;
    rec.kv_base = 1;

    CHECK_EQ_INT(mpx_efuse_record_encode(raw, &rec), 0);
    CHECK_EQ_INT(raw[10], 0x00);

    CHECK_EQ_INT(mpx_efuse_record_decode(&back, raw), 0);
    CHECK_EQ_STR(back.robot_id, "MPX-DOG-01");
    CHECK_EQ_INT(back.flags, MPX_EFUSE_FLAG_DEV);
    CHECK_EQ_INT(back.kv_base, 1);
}

TEST_CASE(efuse_record_rejects_garbage)
{
    mpx_efuse_record_t rec;
    uint8_t raw[MPX_EFUSE_RECORD_LEN];

    /* An unburned block reads as zeros. It must never be mistaken for a record:
     * a zero ID would mean deriving a key from an empty string. */
    memset(raw, 0, sizeof raw);
    CHECK_EQ_INT(mpx_efuse_record_decode(&rec, raw), -1);
    CHECK(!mpx_efuse_record_is_provisioned(raw));

    /* Unknown format, as left by a future firmware. */
    memset(&rec, 0, sizeof rec);
    strcpy(rec.robot_id, "MPX-AJZ7J8Y0YTDS");
    rec.format  = MPX_EFUSE_FORMAT_V1;
    rec.kv_base = 1;
    CHECK_EQ_INT(mpx_efuse_record_encode(raw, &rec), 0);
    raw[MPX_EFUSE_FORMAT_OFF] = 0x02;
    CHECK_EQ_INT(mpx_efuse_record_decode(&rec, raw), -1);

    /* Unknown flag bits: refuse rather than guess what they mean. */
    memset(&rec, 0, sizeof rec);
    strcpy(rec.robot_id, "MPX-AJZ7J8Y0YTDS");
    rec.format  = MPX_EFUSE_FORMAT_V1;
    rec.kv_base = 1;
    CHECK_EQ_INT(mpx_efuse_record_encode(raw, &rec), 0);
    raw[MPX_EFUSE_FLAGS_OFF] = 0x80;
    CHECK_EQ_INT(mpx_efuse_record_decode(&rec, raw), -1);

    /* kv_base 0 is not a key version: every robot starts at 1. */
    memset(&rec, 0, sizeof rec);
    strcpy(rec.robot_id, "MPX-AJZ7J8Y0YTDS");
    rec.format  = MPX_EFUSE_FORMAT_V1;
    rec.kv_base = 1;
    CHECK_EQ_INT(mpx_efuse_record_encode(raw, &rec), 0);
    raw[MPX_EFUSE_KVBASE_OFF] = 0;
    raw[MPX_EFUSE_KVBASE_OFF + 1] = 0;
    CHECK_EQ_INT(mpx_efuse_record_decode(&rec, raw), -1);
}

TEST_CASE(efuse_record_provisioned_requires_production_id)
{
    mpx_efuse_record_t rec;
    uint8_t raw[MPX_EFUSE_RECORD_LEN];

    memset(&rec, 0, sizeof rec);
    rec.format  = MPX_EFUSE_FORMAT_V1;
    rec.kv_base = 1;

    /* A legacy dev ID is a readable record but not a provisioned unit. */
    strcpy(rec.robot_id, "MPX-DOG-01");
    CHECK_EQ_INT(mpx_efuse_record_encode(raw, &rec), 0);
    CHECK_EQ_INT(mpx_efuse_record_decode(&rec, raw), 0);
    CHECK(!mpx_efuse_record_is_provisioned(raw));

    /* A 16-byte production ID is. */
    memset(&rec, 0, sizeof rec);
    strcpy(rec.robot_id, "MPX-AJZ7J8Y0YTDS");
    rec.format  = MPX_EFUSE_FORMAT_V1;
    rec.kv_base = 1;
    CHECK_EQ_INT(mpx_efuse_record_encode(raw, &rec), 0);
    CHECK_EQ_INT(mpx_efuse_record_decode(&rec, raw), 0);
    CHECK(mpx_efuse_record_is_provisioned(raw));
}

TEST_CASE(efuse_record_encode_rejects_invalid)
{
    mpx_efuse_record_t rec;
    uint8_t raw[MPX_EFUSE_RECORD_LEN];

    memset(&rec, 0, sizeof rec);
    strcpy(rec.robot_id, "MPX-AJZ7J8Y0YTDS");
    rec.format  = MPX_EFUSE_FORMAT_V1;
    rec.kv_base = 1;

    /* Wrong format. */
    rec.format = 0x02;
    CHECK_EQ_INT(mpx_efuse_record_encode(raw, &rec), -1);

    /* Unknown flag bit. */
    rec.format = MPX_EFUSE_FORMAT_V1;
    rec.flags  = 0x80;
    CHECK_EQ_INT(mpx_efuse_record_encode(raw, &rec), -1);

    /* kv_base 0. */
    rec.flags   = 0;
    rec.kv_base = 0;
    CHECK_EQ_INT(mpx_efuse_record_encode(raw, &rec), -1);

    /* Empty ID. */
    rec.kv_base = 1;
    rec.robot_id[0] = '\0';
    CHECK_EQ_INT(mpx_efuse_record_encode(raw, &rec), -1);

    /* ID longer than the field. */
    strcpy(rec.robot_id, "MPX-AJZ7J8Y0YTDSX");
    CHECK_EQ_INT(mpx_efuse_record_encode(raw, &rec), -1);

    /* An ID with a bad check symbol, which would produce a robot the cloud can
     * never match to a registration. */
    strcpy(rec.robot_id, "MPX-A1Z7J8Y0YTDS");
    CHECK_EQ_INT(mpx_efuse_record_encode(raw, &rec), -1);
}
