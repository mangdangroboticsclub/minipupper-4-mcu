#include "network/wifi_sta.h"

#include <cstring>
#include <string>
#include <vector>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_sta";

namespace network {
namespace {

/* ── NVS namespace for STA credentials ──────────────────────── */
constexpr const char *NVS_NS = "wifi_sta";
constexpr const char *NVS_KEY_SSID = "ssid";
constexpr const char *NVS_KEY_PASS = "password";

/* ── Event bits ─────────────────────────────────────────────── */
EventGroupHandle_t s_evt_group = nullptr;
constexpr int BIT_CONNECTED   = BIT0;
constexpr int BIT_DISCONNECTED = BIT1;
constexpr int BIT_FAILED      = BIT2;

/* ── State ──────────────────────────────────────────────────── */
esp_netif_t *s_sta_netif = nullptr;
bool s_initialized = false;
StaState s_state = StaState::Disconnected;
std::string s_current_ssid;
std::string s_current_ip;

void wifi_event_handler(void *arg, esp_event_base_t base,
                        int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA started");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "STA connected to AP");
        s_state = StaState::Connecting;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        auto *event = static_cast<wifi_event_sta_disconnected_t *>(data);
        ESP_LOGW(TAG, "STA disconnected (reason=%d)", event->reason);

        s_state = StaState::Disconnected;
        s_current_ip.clear();

        if (event->reason == WIFI_REASON_AUTH_EXPIRE ||
            event->reason == WIFI_REASON_AUTH_FAIL ||
            event->reason == WIFI_REASON_HANDSHAKE_TIMEOUT ||
            event->reason == WIFI_REASON_NO_AP_FOUND) {
            // Credential or availability issue — mark failed
            s_state = StaState::Failed;
            if (s_evt_group) xEventGroupSetBits(s_evt_group, BIT_FAILED);
        } else {
            // Transient — auto-reconnect will be attempted by IDF
            if (s_evt_group) xEventGroupSetBits(s_evt_group, BIT_DISCONNECTED);
        }
    }
}

void ip_event_handler(void *arg, esp_event_base_t base,
                      int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        auto *event = static_cast<ip_event_got_ip_t *>(data);
        char ip_str[16];
        esp_ip4addr_ntoa(&event->ip_info.ip, ip_str, sizeof(ip_str));
        s_current_ip = ip_str;
        s_state = StaState::Connected;

        ESP_LOGI(TAG, "STA got IP: %s", s_current_ip.c_str());
        if (s_evt_group) xEventGroupSetBits(s_evt_group, BIT_CONNECTED);
    } else if (id == IP_EVENT_STA_LOST_IP) {
        ESP_LOGW(TAG, "STA lost IP");
        s_current_ip.clear();
        s_state = StaState::Disconnected;
    }
}

/* ── NVS persistence ────────────────────────────────────────── */
static bool save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s'", NVS_NS);
        return false;
    }

    esp_err_t ret;
    ret = nvs_set_str(nvs, NVS_KEY_SSID, ssid);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS write ssid failed: %s", esp_err_to_name(ret));
        nvs_close(nvs);
        return false;
    }

    ret = nvs_set_str(nvs, NVS_KEY_PASS, password);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS write password failed: %s", esp_err_to_name(ret));
        nvs_close(nvs);
        return false;
    }

    ret = nvs_commit(nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(ret));
        nvs_close(nvs);
        return false;
    }

    nvs_close(nvs);
    ESP_LOGI(TAG, "Saved STA credentials to NVS");
    return true;
}

static bool load_credentials(std::string &ssid, std::string &password)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }

    char buf[64];
    size_t len;

    len = sizeof(buf);
    if (nvs_get_str(nvs, NVS_KEY_SSID, buf, &len) == ESP_OK) {
        ssid = buf;
    } else {
        nvs_close(nvs);
        return false;
    }

    len = sizeof(buf);
    if (nvs_get_str(nvs, NVS_KEY_PASS, buf, &len) == ESP_OK) {
        password = buf;
    } else {
        password.clear();
    }

    nvs_close(nvs);
    return true;
}

static void erase_credentials()
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) return;

    nvs_erase_key(nvs, NVS_KEY_SSID);
    nvs_erase_key(nvs, NVS_KEY_PASS);
    nvs_commit(nvs);
    nvs_close(nvs);

    ESP_LOGI(TAG, "Erased STA credentials from NVS");
}

}  // anonymous namespace

/* ── Public API ─────────────────────────────────────────────── */

bool init_wifi_sta()
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Wi-Fi STA already initialised");
        return true;
    }

    // Create event group
    s_evt_group = xEventGroupCreate();
    if (!s_evt_group) {
        ESP_LOGE(TAG, "Failed to create event group");
        return false;
    }

    // Register event handlers for STA events
    esp_event_handler_instance_t wifi_handler;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, &wifi_handler));

    esp_event_handler_instance_t ip_handler;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, nullptr, &ip_handler));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_LOST_IP, &ip_event_handler, nullptr, &ip_handler));

    // Create the STA netif
    s_sta_netif = esp_netif_create_default_wifi_sta();
    assert(s_sta_netif);

    s_initialized = true;

    // Try to load saved credentials and auto-connect
    std::string saved_ssid, saved_password;
    if (load_credentials(saved_ssid, saved_password) && !saved_ssid.empty()) {
        ESP_LOGI(TAG, "Found saved STA credentials for '%s' — auto-connecting",
                 saved_ssid.c_str());
        wifi_sta_connect(saved_ssid.c_str(), saved_password.c_str());
    } else {
        ESP_LOGI(TAG, "No saved STA credentials — staying in AP-only mode");
    }

    return true;
}

void deinit_wifi_sta()
{
    if (!s_initialized) return;

    wifi_sta_disconnect();

    if (s_evt_group) {
        vEventGroupDelete(s_evt_group);
        s_evt_group = nullptr;
    }

    esp_netif_destroy_default_wifi(s_sta_netif);
    s_sta_netif = nullptr;
    s_initialized = false;
    s_state = StaState::Disconnected;
    s_current_ssid.clear();
    s_current_ip.clear();

    ESP_LOGI(TAG, "Wi-Fi STA deinitialised");
}

bool wifi_sta_connect(const char *ssid, const char *password)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "STA not initialised");
        return false;
    }

    if (!ssid || std::strlen(ssid) == 0) {
        ESP_LOGE(TAG, "SSID cannot be empty");
        return false;
    }

    // Validate lengths
    if (std::strlen(ssid) > 31) {
        ESP_LOGE(TAG, "SSID too long (%zu chars, max 31)", std::strlen(ssid));
        return false;
    }
    if (std::strlen(password) > 63) {
        ESP_LOGE(TAG, "Password too long (%zu chars, max 63)", std::strlen(password));
        return false;
    }

    // Stop Wi-Fi so we can reconfigure (already started by AP init)
    esp_wifi_stop();

    // Set Wi-Fi mode to AP+STA (dual mode)
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    // Configure STA
    wifi_config_t sta_config = {};
    std::strncpy(reinterpret_cast<char *>(sta_config.sta.ssid),
                 ssid, sizeof(sta_config.sta.ssid) - 1);
    if (std::strlen(password) > 0) {
        std::strncpy(reinterpret_cast<char *>(sta_config.sta.password),
                     password, sizeof(sta_config.sta.password) - 1);
    }
    sta_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta_config.sta.threshold.rssi = -127;
    sta_config.sta.pmf_cfg.capable = true;
    sta_config.sta.pmf_cfg.required = false;

    s_current_ssid = ssid;
    s_state = StaState::Connecting;

    // Persist to NVS
    save_credentials(ssid, password);

    // Restart Wi-Fi (AP + STA), then connect
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    (void)esp_wifi_set_ps(WIFI_PS_NONE);      // as wifi_ap.cc: no lag on the AP
    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(ret));
        s_state = StaState::Failed;
        return false;
    }

    ESP_LOGI(TAG, "Connecting to STA network '%s'", ssid);
    return true;
}

void wifi_sta_disconnect()
{
    if (s_state != StaState::Disconnected) {
        esp_wifi_disconnect();
        s_state = StaState::Disconnected;
        s_current_ip.clear();
        ESP_LOGI(TAG, "STA disconnected");
    }
}

void wifi_sta_forget()
{
    wifi_sta_disconnect();
    erase_credentials();
    s_current_ssid.clear();
}

StaState wifi_sta_get_state()
{
    return s_state;
}

std::string wifi_sta_get_ip()
{
    return s_current_ip;
}

std::string wifi_sta_get_ssid()
{
    return s_current_ssid;
}

/* ── Scan (ported from mpv2-claw http_server_mpx_wifi_api.c) ── */

namespace {
constexpr uint16_t SCAN_MAX = 24;

int scan_bars(int rssi)
{
    return rssi >= -60 ? 3 : rssi >= -72 ? 2 : 1;
}
}  // namespace

esp_err_t wifi_sta_scan(std::vector<ScanResult> &out)
{
    out.clear();

    // An AP-only robot has no station interface to scan with. Switching the
    // mode on a running driver brings STA up without dropping the AP.
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_AP) {
        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "scan: APSTA switch failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    wifi_scan_config_t scan = {};
    scan.show_hidden = false;
    scan.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan.scan_time.active.min = 30;
    scan.scan_time.active.max = 80;
    scan.home_chan_dwell_time = 60;

    esp_err_t err = esp_wifi_scan_start(&scan, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan refused: %s", esp_err_to_name(err));
        return err;
    }

    std::vector<wifi_ap_record_t> recs(SCAN_MAX);
    uint16_t n = SCAN_MAX;
    if (esp_wifi_scan_get_ap_records(&n, recs.data()) != ESP_OK) {
        n = 0;
    }

    // The driver hands records back strongest first, so the first time a
    // name appears is its best access point; later repeats are skipped.
    for (uint16_t i = 0; i < n; ++i) {
        const char *ssid = reinterpret_cast<const char *>(recs[i].ssid);
        if (!ssid[0]) continue;
        bool seen = false;
        for (const auto &r : out) {
            if (r.ssid == ssid) { seen = true; break; }
        }
        if (seen) continue;
        out.push_back(ScanResult{ ssid, recs[i].rssi, scan_bars(recs[i].rssi),
                                  recs[i].authmode != WIFI_AUTH_OPEN });
    }
    return ESP_OK;
}

}  // namespace network
