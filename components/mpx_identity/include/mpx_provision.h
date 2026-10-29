/* Factory provisioning over UART0 (§6.6). Device-only.
 *
 * A board that has never been provisioned has no identity, so it cannot be
 * reached over the network to be given one: the only party that can address it
 * is whoever is holding the cable. This module is therefore a deliberately
 * narrow, build-gated listener that accepts four commands on the console UART:
 *
 *     provision  burn the ID record and S, derive K_kv, return the sealed blob
 *     reseal     re-derive and re-seal for the ID already burned (lost blob)
 *     status     report what the fuses say, without touching them
 *     lock       write-protect everything that `provision` left writable
 *
 * Every command is a single line prefixed `@MPXPROV`, so it is identifiable in
 * the boot log that shares the UART. It is compiled in only when
 * CONFIG_APP_PROVISIONING_ENABLE is set, which is a factory build: the commands
 * are irreversible and there is no reason for a shipped image to carry them
 * (plan doc D18/D19).
 */
#ifndef MPX_PROVISION_H
#define MPX_PROVISION_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the listener. Safe to call on every build: without
 * CONFIG_APP_PROVISIONING_ENABLE it returns ESP_ERR_NOT_SUPPORTED and starts
 * nothing. Call after `init_nvs()` and `mpx_identity_init()`.
 *
 * Not an error to call twice; the second call returns ESP_OK. */
esp_err_t mpx_provision_init(void);

/* Call immediately before bringing the radio up. Until this is called, a
 * provisioning run enables the bootloader's entropy source around the secret
 * generation (docs/ROBOT_IDENTITY_AND_KEYS.md D9); afterwards the hardware RNG
 * is a true source and nothing is enabled. Harmless on a build without the
 * listener. */
void mpx_provision_set_rf_ready(void);

/* What `status` reports, sampled from the fuses: "sealed", "partial" or
 * "unprovisioned". Usable on any build, so the HTTP status page can show the
 * same string the bench does. */
const char *mpx_provision_state(void);

/* NVS flag set once a sealed blob has been produced and uploaded. It records
 * "the cloud knows this unit", not "the fuses are burned" -- a reseal or a lost
 * blob must be recoverable, so this never gates sealing. */
/* Replaced by mpx_enroll_activated() in mpx_enroll.h: the activation flag is
 * owned by the enrolment path, which also exists in builds without the
 * listener. */



#ifdef __cplusplus
}
#endif

#endif /* MPX_PROVISION_H */
