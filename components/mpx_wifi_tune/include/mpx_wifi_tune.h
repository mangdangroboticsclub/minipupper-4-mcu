/*
 * mpx_wifi_tune -- make the station side of wifi_manager stick to a real home
 * network.
 *
 * wifi_manager (esp-claw submodule, not edited) configures the station with
 * IDF's defaults: WIFI_FAST_SCAN (connect to the first AP that answers with
 * the right SSID), no driver-level retries, and a 10 s reconnect timer. On a
 * mesh or a Wi-Fi 6 router that is exactly the wrong set of defaults:
 *
 *   - fast scan picks whichever mesh node beacons first, not the strongest;
 *   - a router doing band steering or an SA-Query after the robot rebooted
 *     answers "association refused temporarily" (status 30). The driver
 *     tries once more after the comeback time, gives up, and reports reason
 *     203 (ASSOC_FAIL). Then nothing happens for 10 s.
 *
 * This component listens for WIFI_EVENT_STA_DISCONNECTED and, from its own
 * small task (never from the 2.3 KB sys_evt task), rewrites the station
 * config that wifi_manager installed: all-channel scan, strongest AP first,
 * three driver retries per attempt, PMF capable-but-optional. wifi_manager's
 * own reconnect timer then reconnects with the tuned config. The tuning is
 * re-applied whenever wifi_manager resets the config (a `wifi --set --apply`
 * from the console or the web UI) and the next attempt fails.
 *
 * The retry interval itself is CONFIG_APP_WIFI_RETRY_MS, declared in
 * main/Kconfig.projbuild and picked up by wifi_manager.c's #ifndef fallback.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the disconnect listener and start the worker. Call once, after
 * wifi_manager_start(). Idempotent. */
esp_err_t mpx_wifi_tune_start(void);

/* Hold the station config still while someone else changes it.
 *
 * The tuner rewrites the station config read-modify-write (the driver has no
 * call that sets only the scan method). A phone leaving the robot's hotspot
 * wakes it, and changing networks restarts the hotspot -- so without this a
 * tune that had read the OLD network wrote it straight back over the NEW one,
 * and the robot kept looking for the network it had just been told to leave.
 * Take the lock around wifi_manager_apply_sta_config(); the tuner takes it
 * around its read and write. A no-op before mpx_wifi_tune_start(). */
void mpx_wifi_tune_config_lock(void);
void mpx_wifi_tune_config_unlock(void);

/* Number of station disconnects seen since the last successful connection.
 * Zero once the station has an IP. For `wifi` status lines and tests. */
unsigned mpx_wifi_tune_failures(void);

#ifdef __cplusplus
}
#endif
