#include "mpx_wifi_tune.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "mpx_wifi_tune";

/* Driver-level association/authentication retries per connect attempt. Only
 * honoured with WIFI_ALL_CHANNEL_SCAN, which is why both are set together.
 * Three covers the "refused temporarily, come back in ~1 s" dance twice over
 * without holding the radio for more than a few seconds. */
#define MPX_WIFI_TUNE_DRIVER_RETRIES  3

/* After this many failures in a row with the same "the router said no"
 * reason, log the one hint that usually fixes it. Once per outage. */
#define MPX_WIFI_TUNE_HINT_AFTER      4

static TaskHandle_t s_task;
static SemaphoreHandle_t s_cfg_lock;   /* see mpx_wifi_tune_config_lock() */
static volatile unsigned s_failures;
static volatile int s_last_reason;
static volatile int s_ap_clients;
static bool s_hinted;
static bool s_quiet;
static char s_last_ssid[33];

/* ── Quiet mode: someone is on the robot's own access point ────────────────
 *
 * The robot has one radio. While its station side cannot reach the saved
 * network (wrong password, router off, moved house), every reconnect attempt
 * is an all-channel scan with retries -- a few seconds every 10 s with the
 * radio away from the access point's channel. To a phone on that access
 * point, trying to open the setup page to FIX the network, that is a page
 * that crawls and requests that time out.
 *
 * So while a phone is connected to the AP and the station has failed twice,
 * the search goes gentle: a light scan (no driver retries), shorter visits to
 * each other channel (60 ms instead of 120), and 150 ms back home on the AP's
 * channel between every two of them instead of 30 -- the radio is mostly
 * where the phone is. Back to the full search as soon as the phone leaves,
 * the network connects, or new credentials arrive. */
#define MPX_WIFI_TUNE_QUIET_AFTER  2

static volatile bool s_sta_up;

static void set_scan_gentle(bool gentle)
{
    wifi_scan_default_params_t p = WIFI_SCAN_PARAMS_DEFAULT_CONFIG();
    if (gentle) {
        p.scan_time.active.min = 30;
        p.scan_time.active.max = 60;
        p.home_chan_dwell_time = 150;
    }
    const esp_err_t err = esp_wifi_set_scan_parameters(&p);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan timing not changed (%s) -- next attempt", esp_err_to_name(err));
    }
}

static void apply_tuning_locked(void);

void mpx_wifi_tune_config_lock(void)
{
    if (s_cfg_lock) {
        xSemaphoreTake(s_cfg_lock, portMAX_DELAY);
    }
}

void mpx_wifi_tune_config_unlock(void)
{
    if (s_cfg_lock) {
        xSemaphoreGive(s_cfg_lock);
    }
}

/* The read and the write happen under the lock, so a network change can
 * only land before the read (and gets tuned) or after the write (and wins). */
static void apply_tuning(void)
{
    mpx_wifi_tune_config_lock();
    apply_tuning_locked();
    mpx_wifi_tune_config_unlock();
}

static void apply_tuning_locked(void)
{
    wifi_config_t cfg;
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t err;
    bool quiet;

    /* AP-only (no network set up yet): there is no station to tune, and the
     * driver answers ESP_ERR_WIFI_MODE -- which is all the old warning on
     * every phone joining the setup hotspot was saying. */
    if (esp_wifi_get_mode(&mode) != ESP_OK || (mode != WIFI_MODE_STA && mode != WIFI_MODE_APSTA)) {
        return;
    }
    err = esp_wifi_get_config(WIFI_IF_STA, &cfg);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "get station config: %s", esp_err_to_name(err));
        return;
    }
    if (cfg.sta.ssid[0] == '\0' || s_sta_up) {
        return;     /* nothing to find, or found: never touch a live link */
    }
    if (strncmp((const char *)cfg.sta.ssid, s_last_ssid, sizeof(s_last_ssid)) != 0) {
        /* New credentials (the setup page, or the console): a fresh start. */
        strlcpy(s_last_ssid, (const char *)cfg.sta.ssid, sizeof(s_last_ssid));
        s_failures = 0;
        s_hinted = false;
    }
    quiet = s_ap_clients > 0 && s_failures >= MPX_WIFI_TUNE_QUIET_AFTER;
    if (quiet != s_quiet) {
        set_scan_gentle(quiet);
    }

    const wifi_scan_method_t scan = quiet ? WIFI_FAST_SCAN : WIFI_ALL_CHANNEL_SCAN;
    const uint8_t retries = quiet ? 0 : MPX_WIFI_TUNE_DRIVER_RETRIES;
    if (cfg.sta.scan_method == scan && cfg.sta.sort_method == WIFI_CONNECT_AP_BY_SIGNAL &&
            cfg.sta.failure_retry_cnt == retries) {
        s_quiet = quiet;
        return;
    }

    cfg.sta.scan_method = scan;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    cfg.sta.failure_retry_cnt = retries;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;
    /* Threshold 0 = accept any signal; the sort still prefers the best. */
    cfg.sta.threshold.rssi = 0;

    err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set station config: %s", esp_err_to_name(err));
        return;
    }
    if (quiet != s_quiet) {
        if (quiet) {
            ESP_LOGI(TAG, "a phone is on the robot's access point: searching for \"%s\" "
                          "gently, so the setup page stays fast",
                     (const char *)cfg.sta.ssid);
        } else if (s_quiet) {
            ESP_LOGI(TAG, "full search for \"%s\" again", (const char *)cfg.sta.ssid);
        }
        s_quiet = quiet;
    }
    if (!quiet) {
        ESP_LOGI(TAG, "station tuned for \"%s\": all-channel scan, strongest AP first, "
                      "%d driver retries per attempt",
                 (const char *)cfg.sta.ssid, MPX_WIFI_TUNE_DRIVER_RETRIES);
    }
}

static void maybe_hint(int reason, unsigned failures)
{
    if (s_hinted || failures < MPX_WIFI_TUNE_HINT_AFTER) {
        return;
    }
    switch (reason) {
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_ASSOC_TOOMANY:
    case WIFI_REASON_ASSOC_EXPIRE:
    case WIFI_REASON_AUTH_EXPIRE:
        ESP_LOGW(TAG, "the router keeps refusing association (reason=%d, %u tries). "
                      "This is the router, not the robot: band steering / \"smart "
                      "connect\" pushing a 2.4 GHz-only client to 5 GHz, or PMF set to "
                      "\"required\". Give the 2.4 GHz band its own SSID, or set PMF to "
                      "\"capable/optional\", and point the robot at that.",
                 reason, failures);
        s_hinted = true;
        break;
    case WIFI_REASON_NO_AP_FOUND:
        ESP_LOGW(TAG, "SSID not found after %u scans. The ESP32-S3 only sees 2.4 GHz: "
                      "if this network is 5 GHz-only, or hidden, it will never appear.",
                 failures);
        s_hinted = true;
        break;
    default:
        break;
    }
}

static void tune_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        apply_tuning();
        maybe_hint(s_last_reason, s_failures);
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_AP_STACONNECTED || id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (id == WIFI_EVENT_AP_STACONNECTED) {
            s_ap_clients++;
        } else if (s_ap_clients > 0) {
            s_ap_clients--;
        }
        if (s_task) {
            xTaskNotifyGive(s_task);
        }
        return;
    }
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_up = false;
        /* Only what fits on sys_evt: record and wake the worker. */
        const wifi_event_sta_disconnected_t *ev = data;
        int reason = ev ? ev->reason : 0;
        if (reason == WIFI_REASON_ASSOC_LEAVE) {
            /* We hung up (wifi_manager re-applying a config). Not a failure,
             * and the new attempt is already under way: leave it alone. */
            return;
        }
        s_last_reason = reason;
        s_failures++;
        if (s_task) {
            xTaskNotifyGive(s_task);
        }
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == IP_EVENT_STA_GOT_IP) {
        s_sta_up = true;
        if (s_failures) {
            ESP_LOGI(TAG, "connected after %u failed attempt(s)", s_failures);
        }
        s_failures = 0;
        s_hinted = false;
    }
}

esp_err_t mpx_wifi_tune_start(void)
{
    if (s_task) {
        return ESP_OK;
    }
    if (!s_cfg_lock) {
        s_cfg_lock = xSemaphoreCreateMutex();
        if (!s_cfg_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    /* 4 KB: esp_wifi_set_config stores the config in NVS underneath, which
     * is the deepest thing this task does. Internal RAM, on purpose: the
     * Wi-Fi driver is not happy being called with a PSRAM stack. */
    if (xTaskCreate(tune_task, "wifi_tune", 4096, NULL, 4, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                               on_wifi_event, NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED,
                                         on_wifi_event, NULL);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED,
                                         on_wifi_event, NULL);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "event registration failed: %s", esp_err_to_name(err));
    }
    return err;
}

unsigned mpx_wifi_tune_failures(void)
{
    return s_failures;
}
