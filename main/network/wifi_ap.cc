#include "network/wifi_ap.h"

#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/ip4_addr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_ap";

/* ── NVS namespace for AP config ────────────────────────────── */
static constexpr const char *NVS_NS      = "wifi_ap";
static constexpr const char *NVS_KEY_SSID = "ssid";
static constexpr const char *NVS_KEY_PASS = "password";

/* ── Helpers in the network namespace ──────────────────────── */
namespace network {

static esp_netif_t *s_ap_netif = nullptr;
static bool s_initialized = false;

/* ── Active AP config (NVS-customised or compile-time default) ── */
static std::string s_active_ssid     = WIFI_AP_SSID;
static std::string s_active_password = WIFI_AP_PASSWORD;

namespace {

void wifi_event_handler(void *arg, esp_event_base_t base,
                        int32_t id, void *data)
{
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        auto *event = static_cast<wifi_event_ap_staconnected_t *>(data);
        ESP_LOGI(TAG, "Station connected: " MACSTR " (aid=%d)",
                 MAC2STR(event->mac), event->aid);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        auto *event = static_cast<wifi_event_ap_stadisconnected_t *>(data);
        ESP_LOGI(TAG, "Station disconnected: " MACSTR " (aid=%d)",
                 MAC2STR(event->mac), event->aid);
    }
}

bool set_static_ip()
{
    esp_netif_dns_info_t dns;
    esp_netif_ip_info_t ip{};

    ip.ip.addr       = esp_ip4addr_aton(AP_IP_ADDR);
    ip.netmask.addr  = esp_ip4addr_aton(AP_NETMASK);
    ip.gw.addr       = esp_ip4addr_aton(AP_GATEWAY);

    if (esp_netif_dhcps_stop(s_ap_netif) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop DHCP server");
        return false;
    }

    if (esp_netif_set_ip_info(s_ap_netif, &ip) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set static IP");
        return false;
    }

    // Set DNS to the AP IP itself
    dns.ip.u_addr.ip4.addr = ip.ip.addr;
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns);

    // Restart DHCP server with the new range
    esp_netif_dhcps_start(s_ap_netif);

    ESP_LOGI(TAG, "Static IP set to %s", AP_IP_ADDR);
    return true;
}

}  // anonymous namespace

/* ── NVS persistence ────────────────────────────────────────── */

bool wifi_ap_save_config(const char *ssid, const char *password)
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
    ESP_LOGI(TAG, "Saved AP config to NVS — SSID: \"%s\"", ssid);
    return true;
}

bool wifi_ap_load_config(std::string &ssid, std::string &password)
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

void wifi_ap_erase_config()
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) return;

    nvs_erase_key(nvs, NVS_KEY_SSID);
    nvs_erase_key(nvs, NVS_KEY_PASS);
    nvs_commit(nvs);
    nvs_close(nvs);

    ESP_LOGI(TAG, "Erased AP config from NVS");
}

const char *wifi_ap_get_ssid()
{
    return s_active_ssid.c_str();
}

const char *wifi_ap_get_password()
{
    return s_active_password.c_str();
}

/* ── Public API ─────────────────────────────────────────────── */

bool init_wifi_ap()
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Wi-Fi AP already initialised");
        return true;
    }

    // Initialise netif layer (idempotent)
    ESP_ERROR_CHECK(esp_netif_init());

    // Create default event loop if not already created
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Register event handler
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                &wifi_event_handler, nullptr));

    // Create the AP netif
    s_ap_netif = esp_netif_create_default_wifi_ap();
    assert(s_ap_netif);

    // Init Wi-Fi with default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Set Wi-Fi to AP+STA dual mode (STA netif created by init_wifi_sta)
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    // ── Load custom config from NVS, fall back to compile-time defaults ──
    {
        std::string nvs_ssid, nvs_password;
        if (wifi_ap_load_config(nvs_ssid, nvs_password) && !nvs_ssid.empty()) {
            s_active_ssid     = nvs_ssid;
            s_active_password = nvs_password;
            ESP_LOGI(TAG, "Using NVS-customised AP config — SSID: \"%s\"",
                     s_active_ssid.c_str());
        } else {
            s_active_ssid     = WIFI_AP_SSID;
            s_active_password = WIFI_AP_PASSWORD;
            ESP_LOGI(TAG, "Using compile-time default AP config — SSID: \"%s\"",
                     s_active_ssid.c_str());
        }
    }

    // Configure AP
    wifi_config_t ap_config = {};
    std::strncpy(reinterpret_cast<char *>(ap_config.ap.ssid),
                 s_active_ssid.c_str(), sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = static_cast<uint8_t>(s_active_ssid.size());
    ap_config.ap.max_connection = WIFI_AP_MAX_CONN;
    ap_config.ap.channel = WIFI_AP_CHANNEL;

    if (s_active_password.empty()) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    } else {
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
        std::strncpy(reinterpret_cast<char *>(ap_config.ap.password),
                     s_active_password.c_str(), sizeof(ap_config.ap.password) - 1);
    }

    ap_config.ap.beacon_interval = 100;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    // Set static IP before starting
    if (!set_static_ip()) {
        ESP_LOGE(TAG, "Failed to configure static IP");
        return false;
    }

    // Start Wi-Fi
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi AP started — SSID: \"%s\", IP: %s, auth=%s",
             s_active_ssid.c_str(), AP_IP_ADDR,
             s_active_password.empty() ? "OPEN" : "WPA2");

    s_initialized = true;
    return true;
}

void deinit_wifi_ap()
{
    if (!s_initialized) return;

    esp_wifi_stop();
    esp_wifi_deinit();
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler);
    esp_netif_destroy_default_wifi(s_ap_netif);
    s_ap_netif = nullptr;
    s_initialized = false;

    ESP_LOGI(TAG, "Wi-Fi AP deinitialised");
}

}  // namespace network
