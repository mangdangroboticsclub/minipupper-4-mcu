/* Wiring tests for the generated golden vectors.
 *
 * These do not test the ID or derivation algorithms -- components/mpx_identity
 * does not exist yet. They test that tools/vectors/identity.json reaches the C
 * side intact and that the shapes the firmware will rely on are what §6.1 and
 * §6.3 say they are. When mpx_id.c and mpx_derive.c land, the same header
 * feeds their algorithm tests, so a stale JSON breaks the build here first.
 */
#include "mpx_vectors.h"
#include "test_harness.h"

#include <string.h>

TEST_CASE(vector_file_is_present)
{
    CHECK(MPX_VEC_COUNT > 0);
    CHECK(MPX_VALID_ID_COUNT > 0);
    CHECK(MPX_INVALID_ID_COUNT > 0);
}

TEST_CASE(id_shape_matches_the_frame_header)
{
    /* 16 ASCII bytes is the whole reason the ID format is this wide: it drops
     * into the existing uint8_t robot_uuid[16] header with no wire change. */
    CHECK_EQ_INT(MPX_VEC_ID_LEN, 16);

    for (size_t i = 0; i < (size_t)MPX_VALID_ID_COUNT; i++) {
        CHECK_EQ_INT(strlen(mpx_valid_ids[i]), MPX_VEC_ID_LEN);
    }
    for (size_t i = 0; i < (size_t)MPX_VEC_COUNT; i++) {
        CHECK_EQ_INT(strlen(mpx_vectors[i]->robot_id), MPX_VEC_ID_LEN);
    }
}

TEST_CASE(derivation_labels_are_frozen)
{
    /* §12.1 #3: the label and message layout are one-way doors. A silent change
     * here would invalidate every key ever derived, so assert the literal. */
    CHECK_EQ_STR(MPX_VEC_DERIVE_LABEL, "mpx/v1/robot-key");
    CHECK_EQ_STR(MPX_VEC_KEYFP_LABEL, "mpx/v1/key-fp");
    CHECK_EQ_STR(MPX_VEC_TOKEN_LABEL, "mpx-ai-v1:");
    CHECK_EQ_STR(MPX_VEC_ALPHABET, "0123456789ABCDEFGHJKMNPQRSTVWXYZ");
}

TEST_CASE(every_vector_has_the_expected_sizes)
{
    /* 37 = 16-byte label + 1 NUL + 16-byte id + 4-byte big-endian kv. */
    for (size_t i = 0; i < (size_t)MPX_VEC_COUNT; i++) {
        const mpx_vector_t *v = mpx_vectors[i];
        CHECK_EQ_INT(v->secret_len, 32);
        CHECK_EQ_INT(v->msg_len, 37);
        CHECK_EQ_INT(v->key_len, 32);
        CHECK_EQ_INT(strlen(v->key_fp), 16);
        CHECK(v->kv >= 1);
        /* The msg embeds the id verbatim, at a fixed offset. That is what makes
         * the derivation depend on the id at all. */
        CHECK_EQ_MEM(v->msg + 17, v->robot_id, MPX_VEC_ID_LEN);
    }
}

TEST_CASE(v1_and_v2_derive_different_keys)
{
    /* Rotating kv must change K_v, otherwise D13's rotation is a no-op. */
    for (size_t i = 0; i < (size_t)MPX_VEC_COUNT; i++) {
        for (size_t j = i + 1; j < (size_t)MPX_VEC_COUNT; j++) {
            const mpx_vector_t *a = mpx_vectors[i];
            const mpx_vector_t *b = mpx_vectors[j];
            if (strcmp(a->robot_id, b->robot_id) != 0 || a->kv == b->kv) {
                continue;
            }
            CHECK(memcmp(a->key, b->key, 32) != 0);
        }
    }
}

TEST_CASE(every_vector_matches_the_published_k1)
{
    /* The one vector published in §6.3; the rest are generated. Pinning it here
     * means a regeneration that changes the algorithm cannot quietly pass. */
    static const unsigned char k1[32] = {
        0x27, 0xc6, 0xf7, 0xd8, 0x97, 0x25, 0x76, 0x1e,
        0xbf, 0xf5, 0xad, 0x8f, 0x0a, 0x29, 0xce, 0x35,
        0x3e, 0xd5, 0xb2, 0xea, 0x28, 0x2e, 0x6c, 0xe0,
        0xab, 0xb2, 0xd0, 0x4a, 0xf5, 0xda, 0xf5, 0xae,
    };
    int found = 0;
    for (size_t i = 0; i < (size_t)MPX_VEC_COUNT; i++) {
        const mpx_vector_t *v = mpx_vectors[i];
        if (v->kv == 1 && strcmp(v->robot_id, "MPX-AJZ7J8Y0YTDS") == 0) {
            CHECK_EQ_MEM(v->key, k1, 32);
            CHECK_EQ_STR(v->key_fp, "6d5f50ad94184e23");
            CHECK_EQ_STR(v->robot_token,
                         "MPX-AJZ7J8Y0YTDS.460a4a84a2e2e36cefade46a38a89c93");
            found = 1;
        }
    }
    CHECK_MSG(found, "the §6.3 vector MPX-AJZ7J8Y0YTDS/kv=1 is missing");
}
