/* The sealed-blob layout (§6.4).
 *
 * The cloud parses these bytes with tools/mpx_seal.py; the firmware builds them
 * in mpx_seal.c and mpx_provision.c. Neither side can catch a field moving by
 * one byte at compile time, so both are pinned against tools/vectors/seal.json.
 */
#include "mpx_seal.h"

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

static void fill_sample(mpx_seal_plain_t *p)
{
    memset(p, 0, sizeof *p);
    strcpy(p->robot_id, "MPX-AJZ7J8Y0YTDS");
    p->key_version = 1;
    for (size_t i = 0; i < MPX_SEAL_NONCE_LEN; i++) {
        p->nonce[i] = (uint8_t)i;
    }
    for (size_t i = 0; i < MPX_SEAL_KEY_LEN; i++) {
        p->key[i] = (uint8_t)i;
    }
    p->mac[0] = 0x24;
    p->mac[5] = 0xcc;
    p->sec_flags = MPX_SEC_FLAG_SECURE_BOOT | MPX_SEC_FLAG_JTAG_DISABLED;
    p->secure_version = 0;
    strcpy(p->app_version, "1.2.3");
    for (size_t i = 0; i < MPX_SEAL_DSHASH_LEN; i++) {
        p->ds_pub_sha256[i] = (uint8_t)i;
    }
}

TEST_CASE(seal_lengths_are_the_contract)
{
    /* 8-byte envelope header plus RSA-3072, and a plaintext that fits OAEP's
     * 342-byte SHA-256 limit with room to spare. */
    CHECK_EQ_INT(MPX_SEAL_BLOB_LEN, 392);
    CHECK_EQ_INT(MPX_SEAL_BLOB_HEADER + MPX_SEAL_RSA_BYTES, MPX_SEAL_BLOB_LEN);
    CHECK_EQ_INT(MPX_SEAL_PLAIN_LEN, 149);
    CHECK(MPX_SEAL_PLAIN_LEN < 342);

    /* The offsets in the header must tile the plaintext exactly, with no gap
     * and no overlap -- a hand-maintained offset table is where this breaks. */
    CHECK_EQ_INT(MPX_SEAL_PLAIN_KV_OFF, 5);
    CHECK_EQ_INT(MPX_SEAL_PLAIN_ID_OFF, 9);
    CHECK_EQ_INT(MPX_SEAL_PLAIN_NONCE_OFF, 25);
    CHECK_EQ_INT(MPX_SEAL_PLAIN_KEY_OFF, 41);
    CHECK_EQ_INT(MPX_SEAL_PLAIN_MAC_OFF, 73);
    CHECK_EQ_INT(MPX_SEAL_PLAIN_FLAGS_OFF, 79);
    CHECK_EQ_INT(MPX_SEAL_PLAIN_SVER_OFF, 81);
    CHECK_EQ_INT(MPX_SEAL_PLAIN_APPVER_OFF, 85);
    CHECK_EQ_INT(MPX_SEAL_PLAIN_DSHASH_OFF, 117);
    CHECK_EQ_INT(MPX_SEAL_PLAIN_DSHASH_OFF + MPX_SEAL_DSHASH_LEN,
                 MPX_SEAL_PLAIN_LEN);
}

TEST_CASE(seal_plain_round_trip)
{
    mpx_seal_plain_t in;
    mpx_seal_plain_t out;
    uint8_t buf[MPX_SEAL_PLAIN_LEN];

    fill_sample(&in);
    CHECK_EQ_INT(mpx_seal_plain_encode(buf, &in), 0);
    CHECK_EQ_INT(mpx_seal_plain_decode(&out, buf), 0);

    CHECK_EQ_STR(out.robot_id, in.robot_id);
    CHECK_EQ_INT(out.key_version, in.key_version);
    CHECK_EQ_MEM(out.nonce, in.nonce, MPX_SEAL_NONCE_LEN);
    CHECK_EQ_MEM(out.key, in.key, MPX_SEAL_KEY_LEN);
    CHECK_EQ_MEM(out.mac, in.mac, MPX_SEAL_MAC_LEN);
    CHECK_EQ_INT(out.sec_flags, in.sec_flags);
    CHECK_EQ_INT(out.secure_version, in.secure_version);
    CHECK_EQ_STR(out.app_version, in.app_version);
    CHECK_EQ_MEM(out.ds_pub_sha256, in.ds_pub_sha256, MPX_SEAL_DSHASH_LEN);
}

TEST_CASE(seal_plain_matches_frozen_vectors)
{
    for (size_t i = 0; i < (size_t)MPX_SEAL_VEC_SEAL_COUNT; i++) {
        uint8_t expected[MPX_SEAL_VEC_PLAIN_LEN];
        uint8_t decoded_raw[MPX_SEAL_VEC_PLAIN_LEN];
        hex_to_bytes(expected, sizeof expected, MPX_SEAL_VECS[i].plain_hex);
        hex_to_bytes(decoded_raw, sizeof decoded_raw, MPX_SEAL_VECS[i].plain_hex);

        mpx_seal_plain_t rec;
        CHECK_EQ_INT(mpx_seal_plain_decode(&rec, decoded_raw), 0);

        /* Re-encoding what we just parsed must reproduce the golden bytes: this
         * is what catches an offset that changed on only one side. */
        uint8_t encoded[MPX_SEAL_VEC_PLAIN_LEN];
        CHECK_EQ_INT(mpx_seal_plain_encode(encoded, &rec), 0);
        CHECK_EQ_MEM(encoded, expected, sizeof expected);
    }
}

TEST_CASE(seal_blob_matches_frozen_vectors)
{
    for (size_t i = 0; i < (size_t)MPX_SEAL_VEC_SEAL_COUNT; i++) {
        uint8_t expected[MPX_SEAL_VEC_BLOB_LEN];
        hex_to_bytes(expected, sizeof expected, MPX_SEAL_VECS[i].blob_hex);

        /* The vectors use 0xA5 as a stand-in ciphertext, because real RSA-OAEP
         * output is random and could never be a golden value. */
        uint8_t ct[MPX_SEAL_RSA_BYTES];
        memset(ct, 0xA5, sizeof ct);
        memcpy(expected + MPX_SEAL_BLOB_HEADER, ct, sizeof ct);

        uint8_t blob[MPX_SEAL_BLOB_LEN];
        CHECK_EQ_INT(mpx_seal_blob_encode(blob, 0, ct), 0);
        CHECK_EQ_MEM(blob, expected, sizeof expected);

        CHECK(mpx_seal_blob_looks_valid(blob, sizeof blob));
    }
}

TEST_CASE(seal_blob_rejects_wrong_length)
{
    uint8_t ct[MPX_SEAL_RSA_BYTES];
    uint8_t blob[MPX_SEAL_BLOB_LEN];

    memset(ct, 0x5A, sizeof ct);
    CHECK_EQ_INT(mpx_seal_blob_encode(blob, 0, ct), 0);

    CHECK(!mpx_seal_blob_looks_valid(blob, MPX_SEAL_BLOB_LEN - 1));
    CHECK(!mpx_seal_blob_looks_valid(blob, MPX_SEAL_BLOB_LEN + 1));
    CHECK(!mpx_seal_blob_looks_valid(NULL, MPX_SEAL_BLOB_LEN));

    /* A truncated upload is the likely field failure (a station that wrote a
     * partial line), so the length check has to come first. */
    CHECK(mpx_seal_blob_looks_valid(blob, MPX_SEAL_BLOB_LEN));
}

TEST_CASE(seal_blob_rejects_bad_magic)
{
    uint8_t ct[MPX_SEAL_RSA_BYTES];
    uint8_t blob[MPX_SEAL_BLOB_LEN];

    memset(ct, 0x5A, sizeof ct);
    CHECK_EQ_INT(mpx_seal_blob_encode(blob, 0, ct), 0);

    blob[0] = 'X';
    CHECK(!mpx_seal_blob_looks_valid(blob, MPX_SEAL_BLOB_LEN));

    blob[0] = 'M';
    blob[4] = 0x02; /* unknown format, written by a newer station */
    CHECK(!mpx_seal_blob_looks_valid(blob, MPX_SEAL_BLOB_LEN));
}

TEST_CASE(seal_plain_encode_rejects_invalid)
{
    mpx_seal_plain_t p;
    uint8_t buf[MPX_SEAL_PLAIN_LEN];

    fill_sample(&p);
    CHECK_EQ_INT(mpx_seal_plain_encode(buf, &p), 0);

    /* key_version 0 would mean "no key generation", which no robot has. */
    fill_sample(&p);
    p.key_version = 0;
    CHECK_EQ_INT(mpx_seal_plain_encode(buf, &p), -1);

    /* A short ID has no room in a 16-byte field, and a dev ID must never be
     * sealed for a production key. */
    fill_sample(&p);
    strcpy(p.robot_id, "MPX-DOG-01");
    CHECK_EQ_INT(mpx_seal_plain_encode(buf, &p), -1);

    /* An app version that does not fit is a bug in the caller, not something to
     * truncate silently. */
    fill_sample(&p);
    memset(p.app_version, 'x', MPX_SEAL_APPVER_LEN + 1);
    p.app_version[MPX_SEAL_APPVER_LEN + 1] = '\0';
    CHECK_EQ_INT(mpx_seal_plain_encode(buf, &p), -1);

    /* An all-zero buffer is not a plaintext. */
    uint8_t zeros[MPX_SEAL_PLAIN_LEN];
    memset(zeros, 0, sizeof zeros);
    CHECK_EQ_INT(mpx_seal_plain_decode(&p, zeros), -1);
}

TEST_CASE(seal_plain_fields_are_independent)
{
    /* Each field occupies its own bytes: changing one must not touch another.
     * This is the cheap version of "no aliasing in the offset table". */
    mpx_seal_plain_t base;
    uint8_t a[MPX_SEAL_PLAIN_LEN];
    uint8_t b[MPX_SEAL_PLAIN_LEN];

    fill_sample(&base);
    CHECK_EQ_INT(mpx_seal_plain_encode(a, &base), 0);

    base.key[0] ^= 0xff;
    CHECK_EQ_INT(mpx_seal_plain_encode(b, &base), 0);
    CHECK_EQ_MEM(a, b, MPX_SEAL_PLAIN_KEY_OFF);
    CHECK(a[MPX_SEAL_PLAIN_KEY_OFF] != b[MPX_SEAL_PLAIN_KEY_OFF]);
    for (int off = MPX_SEAL_PLAIN_KEY_OFF + 1; off < MPX_SEAL_PLAIN_MAC_OFF; off++) {
        CHECK_EQ_INT(a[off], b[off]);
    }
    CHECK_EQ_MEM(a + MPX_SEAL_PLAIN_MAC_OFF, b + MPX_SEAL_PLAIN_MAC_OFF,
                 MPX_SEAL_PLAIN_LEN - MPX_SEAL_PLAIN_MAC_OFF);
}

TEST_CASE(seal_plain_app_version_is_nul_padded)
{
    mpx_seal_plain_t p;
    uint8_t buf[MPX_SEAL_PLAIN_LEN];

    /* A short version leaves the rest of the field zeroed, not garbage: the
     * cloud compares those bytes across re-seals. */
    fill_sample(&p);
    strcpy(p.app_version, "1.2.3");
    CHECK_EQ_INT(mpx_seal_plain_encode(buf, &p), 0);

    for (size_t i = 5; i < MPX_SEAL_APPVER_LEN; i++) {
        CHECK_EQ_INT(buf[MPX_SEAL_PLAIN_APPVER_OFF + i], 0);
    }
    CHECK(buf[MPX_SEAL_PLAIN_APPVER_OFF + 5] == 0);

    mpx_seal_plain_t back;
    CHECK_EQ_INT(mpx_seal_plain_decode(&back, buf), 0);
    CHECK_EQ_STR(back.app_version, "1.2.3");

    /* An empty version parses as empty, not as 32 NULs of garbage. */
    fill_sample(&p);
    p.app_version[0] = '\0';
    CHECK_EQ_INT(mpx_seal_plain_encode(buf, &p), 0);
    CHECK_EQ_INT(mpx_seal_plain_decode(&back, buf), 0);
    CHECK_EQ_STR(back.app_version, "");
}
