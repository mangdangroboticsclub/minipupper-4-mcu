/* Algorithm tests for components/mpx_identity (Stage 1).
 *
 * test_vectors.c checks that the golden vectors arrive intact. This file checks
 * the C implementation against them: mpx_id.c for the ID format, and
 * mpx_derive.c for the key derivation. The point of keeping both ESP-free is
 * here -- the algorithm runs on a laptop, so an edit that would change every
 * derived key fails in CI instead of on a robot.
 *
 * The HMAC is injected (see mpx_derive.h) because the device hashes in the
 * HMAC peripheral and neither laptop nor CI has that. With OpenSSL present the
 * injected callback is a real HMAC-SHA256 and the derived keys are compared to
 * the vectors; without it only the 37-byte message is checked, which is still
 * where a layout change would show up.
 */
#include "mpx_vectors.h"
#include "test_harness.h"

#include "mpx_derive.h"
#include "mpx_id.h"

#include <string.h>

#ifdef MPX_HAVE_OPENSSL
#include <openssl/evp.h>
#include <openssl/hmac.h>
#endif

/* ── ID format (§6.3) ──────────────────────────────────────── */

TEST_CASE(id_alphabet_is_crockford_base32_no_i_l_o_u)
{
    /* 32 symbols, uppercase, and the four ambiguous letters are gone. The
     * alphabet is a one-way door: changing it invalidates every printed label
     * and every ID already burned into an eFuse, so assert it literally. */
    CHECK_EQ_INT(strlen(mpx_id_alphabet), 32);
    CHECK_EQ_STR(mpx_id_alphabet, MPX_VEC_ALPHABET);
    CHECK(strchr(mpx_id_alphabet, 'I') == NULL);
    CHECK(strchr(mpx_id_alphabet, 'L') == NULL);
    CHECK(strchr(mpx_id_alphabet, 'O') == NULL);
    CHECK(strchr(mpx_id_alphabet, 'U') == NULL);
}

TEST_CASE(id_accepts_the_golden_ids)
{
    for (size_t i = 0; i < (size_t)MPX_VALID_ID_COUNT; i++) {
        CHECK_MSG(mpx_id_valid(mpx_valid_ids[i]), "rejected %s",
                  mpx_valid_ids[i]);
    }
}

TEST_CASE(id_rejects_the_golden_bad_ids)
{
    for (size_t i = 0; i < (size_t)MPX_INVALID_ID_COUNT; i++) {
        CHECK_MSG(!mpx_id_valid(mpx_invalid_ids[i]), "accepted %s",
                  mpx_invalid_ids[i]);
    }
}

TEST_CASE(id_accepts_the_dev_prefix)
{
    /* tools/mpx_dev_identity.py hands every bench board a DEV- id so two robots
     * on one gateway stop being the same robot. Both prefixes go through the
     * same check symbol, so DEV- must validate exactly like MPX-. */
    CHECK(mpx_id_valid("DEV-5CC9CWABV4QC"));
    CHECK(!mpx_id_valid("DEV-5CC9CWABV4QD"));
    CHECK(!mpx_id_valid("DEW-5CC9CWABV4QC"));
}

TEST_CASE(id_rejects_null_and_short_and_long)
{
    CHECK(!mpx_id_valid(NULL));
    CHECK(!mpx_id_valid(""));
    /* One byte short, one byte long: length is part of the format, and 16 is
     * exactly the frame header field it has to fit. */
    CHECK(!mpx_id_valid("MPX-AJZ7J8Y0YTD"));
    CHECK(!mpx_id_valid("MPX-AJZ7J8Y0YTDSX"));
    CHECK(!mpx_id_valid("MPX-"));
}

TEST_CASE(id_check_symbol_matches_the_last_character)
{
    /* mpx_id_check_symbol() is what a human-readable error message uses to say
     * "you typed X, it should be Y". It must agree with the trailing character
     * of every valid ID, and it must disagree for a corrupt one. */
    for (size_t i = 0; i < (size_t)MPX_VALID_ID_COUNT; i++) {
        const char *id = mpx_valid_ids[i];
        const int expected = (int)id[MPX_ID_STR_LEN - 1];
        CHECK_EQ_INT(mpx_id_check_symbol(id), expected);
    }

    char broken[MPX_ID_STR_LEN + 1];
    memcpy(broken, mpx_valid_ids[0], sizeof broken);
    broken[MPX_ID_BODY_OFF] =
        broken[MPX_ID_BODY_OFF] == '0' ? '1' : '0';
    CHECK(mpx_id_check_symbol(broken) != (int)broken[MPX_ID_STR_LEN - 1]);
}

TEST_CASE(id_every_single_symbol_substitution_is_caught)
{
    /* §11 test 1, in C. The Luhn-32 fold is what makes this true: a plain
     * sum-mod-32 would miss a substituted symbol whose value differs by 16 at a
     * position where the factor is 1. */
    char variant[MPX_ID_STR_LEN + 1];
    for (size_t i = 0; i < (size_t)MPX_VALID_ID_COUNT; i++) {
        const char *id = mpx_valid_ids[i];
        for (size_t pos = MPX_ID_BODY_OFF; pos < MPX_ID_STR_LEN; pos++) {
            for (size_t s = 0; s < 32; s++) {
                if (mpx_id_alphabet[s] == id[pos]) continue;
                memcpy(variant, id, sizeof variant);
                variant[pos] = mpx_id_alphabet[s];
                CHECK_MSG(!mpx_id_valid(variant),
                          "missed substitution %s at %zu -> %s", id, pos,
                          variant);
            }
        }
    }
}

TEST_CASE(id_to_frame_pads_with_zeros_and_never_overflows)
{
    /* The frame header field is uint8_t[16] and the cloud reads all 16 as AAD.
     * Padding must be deterministic zeros, not whatever was on the stack. */
    uint8_t frame[16];
    memset(frame, 0xa5, sizeof frame);
    CHECK(mpx_id_to_frame(frame, mpx_valid_ids[0]));
    CHECK_EQ_MEM(frame, mpx_valid_ids[0], MPX_ID_STR_LEN);

    /* A short legacy ID occupies the leading bytes and the rest is zero. */
    CHECK(mpx_id_to_frame(frame, "MPX-DOG-01"));
    CHECK_EQ_MEM(frame, "MPX-DOG-01", 10);
    for (size_t i = 10; i < sizeof frame; i++) {
        CHECK_EQ_INT(frame[i], 0);
    }

    /* Nothing to write or too much to fit: refuse rather than truncate. */
    CHECK(!mpx_id_to_frame(frame, ""));
    CHECK(!mpx_id_to_frame(frame, NULL));
    CHECK(!mpx_id_to_frame(frame, "MPX-AJZ7J8Y0YTDSX"));
}

TEST_CASE(legacy_ids_stay_valid_only_for_the_legacy_path)
{
    /* MPX-DOG-01 is what every dev board has in Kconfig today, and it is 10
     * bytes of printable ASCII. The strict checker must reject it (it is not
     * the Crockford format) while the transitional one accepts it, so the
     * Kconfig source keeps working until the eFuse source lands. */
    CHECK(!mpx_id_valid("MPX-DOG-01"));
    CHECK(mpx_id_legacy_valid("MPX-DOG-01"));
    CHECK(mpx_id_valid_or_legacy("MPX-DOG-01"));
    CHECK(mpx_id_valid_or_legacy(mpx_valid_ids[0]));

    CHECK(!mpx_id_legacy_valid(""));
    CHECK(!mpx_id_legacy_valid(NULL));
    CHECK(!mpx_id_legacy_valid("MPX DOG 01"));   /* space is not printable-ID */
    CHECK(!mpx_id_legacy_valid("MPX-DOG-01-AND-MORE")); /* > 16 bytes */
}

/* ── Key derivation (§6.4) ─────────────────────────────────── */

TEST_CASE(derive_msg_matches_the_vectors)
{
    /* 16-byte label, NUL, 16-byte id, big-endian key version. Frozen by §12.1
     * #3: any change here silently re-keys the fleet. */
    uint8_t msg[MPX_DERIVE_MSG_LEN];
    for (size_t i = 0; i < (size_t)MPX_VEC_COUNT; i++) {
        const mpx_vector_t *v = mpx_vectors[i];
        memset(msg, 0, sizeof msg);
        /* Returns 0/-1 like the rest of the ESP-free API, not a length. */
        CHECK_EQ_INT(mpx_derive_msg(msg, v->robot_id, v->kv), 0);
        CHECK_EQ_MEM(msg, v->msg, v->msg_len);
    }
}

TEST_CASE(derive_msg_rejects_bad_arguments)
{
    uint8_t msg[MPX_DERIVE_MSG_LEN];
    CHECK_EQ_INT(mpx_derive_msg(msg, NULL, 1), -1);
    CHECK_EQ_INT(mpx_derive_msg(NULL, mpx_valid_ids[0], 1), -1);
    /* An id longer than the field must be refused, not truncated: a truncated
     * id derives a different key than the one the cloud can reproduce. */
    CHECK_EQ_INT(mpx_derive_msg(msg, "MPX-AJZ7J8Y0YTDSX", 1), -1);
}

#ifdef MPX_HAVE_OPENSSL
static int openssl_hmac_sha256(const uint8_t *key, size_t key_len,
                               const uint8_t *msg, size_t msg_len,
                               uint8_t out[MPX_KEY_LEN])
{
    unsigned int out_len = 0;
    unsigned char *rc = HMAC(EVP_sha256(), key, (int)key_len, msg, msg_len, out,
                             &out_len);
    return (rc != NULL && out_len == MPX_KEY_LEN) ? 0 : -1;
}
#endif

#ifdef MPX_HAVE_OPENSSL
TEST_CASE(derive_key_and_fingerprint_match_the_vectors)
{
    /* The end-to-end check: secret -> HMAC -> key, and key -> fingerprint, for
     * every vector. This is the assertion that says the firmware and the cloud
     * will agree on a robot's key without either of them storing it in clear. */
    uint8_t key[MPX_KEY_LEN];
    char fp[17];

    for (size_t i = 0; i < (size_t)MPX_VEC_COUNT; i++) {
        const mpx_vector_t *v = mpx_vectors[i];

        memset(key, 0, sizeof key);
        CHECK_EQ_INT(mpx_derive_key(openssl_hmac_sha256, v->secret,
                                    v->secret_len, v->robot_id, v->kv, key),
                     0);
        CHECK_EQ_MEM(key, v->key, v->key_len);

        memset(fp, 0, sizeof fp);
        CHECK_EQ_INT(mpx_derive_key_fp(openssl_hmac_sha256, key, fp), 0);
        CHECK_EQ_STR(fp, v->key_fp);
        CHECK_EQ_INT(strlen(fp), 16);
    }
}

TEST_CASE(derive_key_changes_with_the_key_version)
{
    /* Two of the vectors differ only in kv, under the same secret and id. If
     * the version ever dropped out of the message these would collide. */
    const mpx_vector_t *a = mpx_vectors[0];
    for (size_t i = 1; i < (size_t)MPX_VEC_COUNT; i++) {
        if (strcmp(mpx_vectors[i]->robot_id, a->robot_id) == 0 &&
            mpx_vectors[i]->kv != a->kv) {
            uint8_t ka[MPX_KEY_LEN];
            uint8_t kb[MPX_KEY_LEN];
            CHECK_EQ_INT(mpx_derive_key(openssl_hmac_sha256, a->secret,
                                        a->secret_len, a->robot_id, a->kv, ka),
                         0);
            CHECK_EQ_INT(mpx_derive_key(openssl_hmac_sha256, a->secret,
                                        a->secret_len, mpx_vectors[i]->robot_id,
                                        mpx_vectors[i]->kv, kb),
                         0);
            CHECK(mpx_test_mem_eq(ka, kb, MPX_KEY_LEN) == 0);
            return;
        }
    }
    mpx_test_fail(__FILE__, __LINE__,
                  "vectors no longer contain a same-id kv pair");
}

/* The eFuse identity path passes no secret at all: S lives in an RD_DIS block
 * and mpx_efuse_hmac_sha256 derives through the HMAC peripheral instead. A
 * guard that rejected a NULL secret made every provisioned unit boot with no
 * K_v -- which reads as "no robot identity": no chat pipeline, no Lua queue,
 * and the PWA installer gets a 503 on every script it enqueues. */
static const uint8_t *s_owned_secret;
static size_t s_owned_secret_len;

static int owning_hmac_sha256(const uint8_t *key, size_t key_len,
                              const uint8_t *msg, size_t msg_len,
                              uint8_t out[MPX_KEY_LEN])
{
    (void)key;
    (void)key_len;
    return openssl_hmac_sha256(s_owned_secret, s_owned_secret_len, msg, msg_len,
                               out);
}

TEST_CASE(derive_key_accepts_an_adapter_that_owns_the_secret)
{
    const mpx_vector_t *v = mpx_vectors[0];
    uint8_t key[MPX_KEY_LEN];

    s_owned_secret = v->secret;
    s_owned_secret_len = v->secret_len;

    memset(key, 0, sizeof key);
    CHECK_EQ_INT(mpx_derive_key(owning_hmac_sha256, NULL, 0, v->robot_id, v->kv,
                                key),
                 0);
    CHECK_EQ_MEM(key, v->key, v->key_len);

    /* NULL with a length is still a caller bug: no adapter can honour it. */
    memset(key, 0, sizeof key);
    CHECK_EQ_INT(mpx_derive_key(owning_hmac_sha256, NULL, 32, v->robot_id, v->kv,
                                key),
                 -1);
}
#endif /* MPX_HAVE_OPENSSL */
