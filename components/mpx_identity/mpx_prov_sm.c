#include "mpx_prov_sm.h"

mpx_prov_action_t mpx_prov_decide(bool have_key, bool have_record,
                                  bool id_matches, bool request_valid)
{
    /* A request that does not parse cannot be acted on in any state: there is
     * no ID to burn and nothing to compare against. */
    if (!request_valid) {
        return MPX_PROV_REFUSE_BAD_STATE;
    }

    if (!have_key && !have_record) {
        return MPX_PROV_BURN_ALL;
    }
    if (have_key && !have_record) {
        /* Power was lost between the key and the record. Keep the key: a fresh
         * S would leave the first one in an eFuse block forever, and the
         * already-burned key is no worse than a new one. */
        return MPX_PROV_BURN_RECORD;
    }
    if (!have_key && have_record) {
        return MPX_PROV_BURN_KEY;
    }
    return id_matches ? MPX_PROV_SEAL : MPX_PROV_REFUSE_ID_MISMATCH;
}

const char *mpx_prov_state_name(bool have_key, bool have_record)
{
    if (have_key && have_record) {
        return "sealed";
    }
    if (have_key || have_record) {
        return "partial";
    }
    return "unprovisioned";
}

const char *mpx_prov_error_name(mpx_prov_action_t action)
{
    switch (action) {
    case MPX_PROV_REFUSE_ID_MISMATCH:
        return "id_mismatch";
    case MPX_PROV_REFUSE_BAD_STATE:
        return "bad_request";
    default:
        return "ok";
    }
}
