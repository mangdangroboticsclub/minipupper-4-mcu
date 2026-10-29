/* First-connect enrolment: the robot announces its own key.
 *
 * The station does not have to carry the key anywhere. At first boot, the unit
 * derives K_v from its own fuse, seals it to the KMS public key compiled into
 * the image, and posts that envelope to the cloud. The envelope is useless to
 * anyone who intercepts it -- only the KMS private key opens it -- so the
 * factory's remaining job is burning fuses and printing a label.
 *
 * The blob is stable across boots (K_v is derived, not random) and is kept in
 * NVS so a retry re-sends the same envelope instead of making a new one; the
 * cloud can then dedupe on robot_uuid without ambiguity.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the enrolment task when this build owns an eFuse identity, has both
 * halves burned, and has not been activated yet. Otherwise does nothing and the
 * robot behaves exactly as before. */
esp_err_t mpx_enroll_init(void);

/* One synchronous attempt: derive (or reuse) the sealed blob and post it.
 * Returns ESP_OK only when the cloud accepted it. */
esp_err_t mpx_enroll_now(void);

/* True while the robot has a usable identity that the cloud has not confirmed. */
bool mpx_enroll_pending(void);

/* The activation flag lives in NVS under the identity namespace so a unit that
 * has been enrolled stops asking. Losing NVS is not fatal: the blob is stored
 * next to it, so re-enrolment re-sends the same envelope. */
bool mpx_enroll_activated(void);
esp_err_t mpx_enroll_set_activated(bool activated);

#ifdef __cplusplus
}
#endif
