#pragma once

#include <cstdint>
#include <string>

namespace network {

/**
 * @brief Wi-Fi AP configuration defaults (from Kconfig).
 *
 * These are compile-time fallbacks used when no user-customised values
 * have been saved to NVS.  Change them via `idf.py menuconfig` under
 * "MPX Dog App Configuration".
 */
#ifndef CONFIG_APP_AP_SSID
#error "CONFIG_APP_AP_SSID not defined — include sdkconfig.h or run idf.py menuconfig"
#endif

constexpr const char *WIFI_AP_SSID       = CONFIG_APP_AP_SSID;
constexpr const char *WIFI_AP_PASSWORD   = CONFIG_APP_AP_PASSWORD;
constexpr uint8_t     WIFI_AP_MAX_CONN   = CONFIG_APP_AP_MAX_CONN;
constexpr uint8_t     WIFI_AP_CHANNEL    = CONFIG_APP_AP_CHANNEL;

/**
 * @brief Static IP for the AP interface.
 */
constexpr const char *AP_IP_ADDR  = "192.168.2.1";
constexpr const char *AP_NETMASK  = "255.255.255.0";
constexpr const char *AP_GATEWAY  = "192.168.2.1";

/**
 * @brief Initialize Wi-Fi in Soft-AP mode with a static IP.
 *
 * Sets up the ESP32 as an access point at 192.168.2.1.
 * Checks NVS for user-customised SSID/password first, falling back to
 * the compile-time defaults (WIFI_AP_SSID / WIFI_AP_PASSWORD).
 * Must be called after nvs_flash_init() and before the HTTP server starts.
 *
 * @return true on success.
 */
bool init_wifi_ap();

/**
 * @brief Stop Wi-Fi AP and clean up.
 */
void deinit_wifi_ap();

/**
 * @brief Persist AP SSID + password to NVS.
 *
 * On the next boot init_wifi_ap() will use these values instead of the
 * compile-time defaults.  The caller should restart the ESP after saving.
 *
 * @param ssid     New AP SSID (max 31 chars).
 * @param password New AP password (max 63 chars, empty for open network).
 * @return true on success.
 */
bool wifi_ap_save_config(const char *ssid, const char *password);

/**
 * @brief Load the saved AP config from NVS.
 *
 * @param[out] ssid     Set to the saved SSID (or empty if none).
 * @param[out] password Set to the saved password (or empty if none).
 * @return true if saved config was found in NVS.
 */
bool wifi_ap_load_config(std::string &ssid, std::string &password);

/**
 * @brief Erase saved AP config from NVS (reverts to compile-time defaults).
 */
void wifi_ap_erase_config();

/**
 * @brief Return the currently-active AP SSID.
 *
 * This is the SSID actually in use (NVS-customised or compile-time default).
 */
const char *wifi_ap_get_ssid();

/**
 * @brief Return the currently-active AP password.
 */
const char *wifi_ap_get_password();

}  // namespace network
