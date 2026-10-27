#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "esp_err.h"

namespace network {

/**
 * @brief Wi-Fi STA mode configuration.
 *
 * SSID and password are persisted to NVS under the "wifi_sta" namespace
 * so the robot auto-connects on subsequent boots.
 */
struct StaConfig {
    std::string ssid;
    std::string password;
    std::string identity;   ///< WPA2/WPA3-Enterprise (eduroam, HKU, ...) login; empty = a normal password network
};

/**
 * @brief Current STA connection state.
 */
enum class StaState : uint8_t {
    Disconnected,
    Connecting,
    Connected,
    Failed,
};

/**
 * @brief Attempt to connect to a Wi-Fi network as a station.
 *
 * Credentials are persisted to NVS so they survive reboots.
 * Calling this while already connected will disconnect and reconnect.
 *
 * @param ssid     The network SSID (max 31 chars).
 * @param password The network password (max 63 chars, empty for open); for an enterprise
 *                 network, the account's password (max 127 chars).
 * @param identity Enterprise networks only (eduroam, a university or company network:
 *                 WPA2/WPA3-Enterprise, PEAP or TTLS with MSCHAPv2): the login, e.g.
 *                 "name@hku.hk". Empty (the default) = a normal password network.
 * @return true if the connection attempt was started.
 */
bool wifi_sta_connect(const char *ssid, const char *password, const char *identity = "");

/**
 * @brief Disconnect from the current Wi-Fi network.
 *
 * Credentials are NOT erased from NVS — use wifi_sta_forget() for that.
 */
void wifi_sta_disconnect();

/**
 * @brief Forget saved credentials and disconnect.
 */
void wifi_sta_forget();

/**
 * @brief Get the current STA connection state.
 */
StaState wifi_sta_get_state();

/**
 * @brief Get the IP address assigned to the STA interface.
 *
 * @return IP string (e.g. "192.168.1.42") or empty string if not connected.
 */
std::string wifi_sta_get_ip();

/**
 * @brief Get the SSID of the currently connected (or last-attempted) network.
 */
std::string wifi_sta_get_ssid();

/**
 * @brief Why the last join failed ("auth", "not_found", "reason 15", ...), empty when it did not.
 */
std::string wifi_sta_get_error();

/**
 * @brief One network heard by a scan (GET /v1/wifi/scan).
 */
struct ScanResult {
    std::string ssid;
    int         rssi;
    int         bars;     ///< 1..3 from RSSI, same thresholds as mpv2-claw
    bool        secure;   ///< anything but WIFI_AUTH_OPEN
    bool        enterprise;   ///< WPA2/WPA3-Enterprise: needs a login (identity) and a password
};

/**
 * @brief Scan for nearby networks, strongest first, one entry per SSID.
 *
 * Blocks for about a second. Short active dwell times keep the AP on its own
 * channel most of that time so a phone on the robot's hotspot stays joined.
 * Switches to AP+STA if the robot is still AP-only (the driver cannot scan
 * without a station interface).
 *
 * @return ESP_OK, ESP_ERR_WIFI_STATE while a connect is in progress ("busy"),
 *         or the driver's error.
 */
esp_err_t wifi_sta_scan(std::vector<ScanResult> &out);

/**
 * @brief Initialise the STA interface.
 *
 * Must be called after nvs_flash_init() but before the HTTP server starts.
 * If saved credentials exist in NVS, an auto-connect is triggered.
 *
 * @return true on success.
 */
bool init_wifi_sta();

/**
 * @brief De-initialise the STA interface.
 */
void deinit_wifi_sta();

}  // namespace network
