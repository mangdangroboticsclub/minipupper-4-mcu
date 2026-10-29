/* The provisioning line parser and the §6.6 decision table.
 *
 * Both are readable only on a board with a cable plugged into it, so these are
 * the tests that stand in for the bench: the parser has to see a station line
 * exactly as it arrives, and the table has to refuse the states that would burn
 * a wrong identity.
 */
#include "test_harness.h"

#include "mpx_prov_json.h"
#include "mpx_prov_sm.h"

static const char kProvisionLine[] =
    "@MPXPROV {\"cmd\":\"provision\",\"id\":\"MPX-AJZ7J8Y0YTDS\",\"kv\":1}";

TEST_CASE(prov_line_prefix_is_required)
{
    CHECK(mpx_prov_is_command(kProvisionLine));
    CHECK(!mpx_prov_is_command("{\"cmd\":\"provision\"}"));
    CHECK(!mpx_prov_is_command("I (123) boot: @MPXPROV late"));
    CHECK(mpx_prov_command_body("{\"cmd\":\"status\"}") == NULL);
}

TEST_CASE(prov_body_skips_space_after_prefix)
{
    const char *body = mpx_prov_command_body(kProvisionLine);

    CHECK(body != NULL);
    CHECK_EQ_STR(body, "{\"cmd\":\"provision\",\"id\":\"MPX-AJZ7J8Y0YTDS\",\"kv\":1}");
    /* No space at all is also a line, and so is a bare ping. */
    CHECK_EQ_STR(mpx_prov_command_body("@MPXPROV{\"cmd\":\"status\"}"), "{\"cmd\":\"status\"}");
    CHECK_EQ_STR(mpx_prov_command_body("@MPXPROV"), "");
}

TEST_CASE(prov_parse_command_fields)
{
    const char *body = mpx_prov_command_body(kProvisionLine);
    char cmd[16];
    char id[17];
    uint32_t kv = 0;

    CHECK_EQ_INT(mpx_json_get_string(body, "cmd", cmd, sizeof cmd), MPX_JSON_STRING);
    CHECK_EQ_STR(cmd, "provision");
    CHECK_EQ_INT(mpx_json_get_string(body, "id", id, sizeof id), MPX_JSON_STRING);
    CHECK_EQ_STR(id, "MPX-AJZ7J8Y0YTDS");
    CHECK(mpx_json_get_u32(body, "kv", &kv));
    CHECK_EQ_INT(kv, 1);
}

TEST_CASE(prov_parse_rejects_wrong_shapes)
{
    const char *json = "{\"a\":1,\"b\":\"x\",\"c\":true,\"d\":-2,\"e\":1.5}";
    char out[8];
    uint32_t num = 0;
    bool flag = false;

    CHECK_EQ_INT(mpx_json_get_string(json, "a", out, sizeof out), MPX_JSON_NONE);
    CHECK_EQ_INT(mpx_json_get_string(json, "missing", out, sizeof out), MPX_JSON_NONE);
    CHECK_EQ_STR(out, "");
    CHECK(!mpx_json_get_u32(json, "d", &num));
    CHECK(!mpx_json_get_u32(json, "e", &num));
    CHECK(!mpx_json_get_u32(json, "b", &num));
    CHECK(mpx_json_get_bool(json, "c", &flag));
    CHECK(flag);
    CHECK(!mpx_json_get_bool(json, "a", &flag));
}

TEST_CASE(prov_parse_refuses_truncation)
{
    /* A short buffer must not yield a prefix of the ID: the station would then
     * provision under an ID nobody allocated. */
    const char *json = "{\"id\":\"MPX-AJZ7J8Y0YTDS\"}";
    char small[8];

    CHECK_EQ_INT(mpx_json_get_string(json, "id", small, sizeof small), MPX_JSON_NONE);
    CHECK_EQ_STR(small, "");
}

TEST_CASE(prov_parse_rejects_escapes)
{
    /* Escapes would need real unescaping; refusing is the safe read. */
    const char *json = "{\"id\":\"MPX-MPX\\u0041\"}";
    char out[17];

    CHECK_EQ_INT(mpx_json_get_string(json, "id", out, sizeof out), MPX_JSON_NONE);
}

TEST_CASE(prov_parse_kv_bounds)
{
    uint32_t kv = 0;

    CHECK(mpx_json_get_u32("{\"kv\":65535}", "kv", &kv));
    CHECK_EQ_INT(kv, 65535);
    CHECK(mpx_json_get_u32("{\"kv\":4294967295}", "kv", &kv));
    /* 2^32 and above must be refused rather than wrapped into a valid kv. */
    CHECK(!mpx_json_get_u32("{\"kv\":4294967296}", "kv", &kv));
    CHECK(!mpx_json_get_u32("{\"kv\":x}", "kv", &kv));
}

TEST_CASE(prov_table_burns_a_fresh_board)
{
    CHECK_EQ_INT(mpx_prov_decide(false, false, false, true), MPX_PROV_BURN_ALL);
    CHECK_EQ_STR(mpx_prov_state_name(false, false), "unprovisioned");
}

TEST_CASE(prov_table_recovers_partial_states)
{
    /* A reset between the two burns must be repairable, and it is repaired by
     * burning whatever is missing rather than by starting over. */
    CHECK_EQ_INT(mpx_prov_decide(true, false, false, true), MPX_PROV_BURN_RECORD);
    CHECK_EQ_INT(mpx_prov_decide(false, true, true, true), MPX_PROV_BURN_KEY);
    CHECK_EQ_STR(mpx_prov_state_name(true, false), "partial");
    CHECK_EQ_STR(mpx_prov_state_name(false, true), "partial");
}

TEST_CASE(prov_table_seals_only_a_matching_id)
{
    CHECK_EQ_INT(mpx_prov_decide(true, true, true, true), MPX_PROV_SEAL);
    CHECK_EQ_STR(mpx_prov_state_name(true, true), "sealed");

    /* Different ID: the fuses are already committed to one, and re-reserving is
     * the cloud's decision, not the firmware's. */
    CHECK_EQ_INT(mpx_prov_decide(true, true, false, true), MPX_PROV_REFUSE_ID_MISMATCH);
    CHECK_EQ_STR(mpx_prov_error_name(MPX_PROV_REFUSE_ID_MISMATCH), "id_mismatch");
}

TEST_CASE(prov_table_refuses_invalid_requests_in_every_state)
{
    CHECK_EQ_INT(mpx_prov_decide(false, false, false, false), MPX_PROV_REFUSE_BAD_STATE);
    CHECK_EQ_INT(mpx_prov_decide(true, true, true, false), MPX_PROV_REFUSE_BAD_STATE);
    CHECK_EQ_STR(mpx_prov_error_name(MPX_PROV_REFUSE_BAD_STATE), "bad_request");
    CHECK_EQ_STR(mpx_prov_error_name(MPX_PROV_SEAL), "ok");
}
