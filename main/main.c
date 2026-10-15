/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_claw.h"
#include "app_fs.h"
#include "claw_version.h"
#include "claw_paths.h"
#include "edge_agent_version.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "wifi_manager.h"
#include "time.h"
#include "nvs_flash.h"
#include "http_server.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_board_manager_includes.h"
#include "mpx_captive_dns.h"
#include "cmd_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
#include "cap_im_wechat.h"
#endif
#include "app_config.h"
#include "esp_console.h"
#include "mpx_wifi_tune.h"
#if CONFIG_MP4_ROBOT_ENABLE
#include "mpx_robot.h"
#include "mpx_wasm.h"
#include "mpx_rings.h"
#include "app_capabilities.h"
#include "cap_robot.h"
#include "cap_mpx_skill.h"
#include "mpx_selftest.h"
#include "mpx_audio.h"
#include "mpx_voice_link.h"
#include "mpx_mcp_ws.h"
#if CONFIG_MP4_PI_LINK_ENABLE
#include "mpx_pi_link.h"
#endif
#if CONFIG_MP4_ROBOT_ENABLE && CONFIG_MP4_UROS_ENABLE
#include "mpx_uros.h"
#endif
#if CONFIG_MP4_ROBOT_ENABLE && CONFIG_MP4_POLICY_ENABLE
#include "mpx_policy.h"
#endif
#if CONFIG_MP4_VOICE_ENABLE
#include "mpx_voice_wake.h"
#include "cap_music.h"
#endif
/* cap_display is built for every MP4_ROBOT_ENABLE configuration, panel or no
 * panel -- see main/idf_component.yml. Do NOT put this include behind
 * CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT: the face calls below are guarded
 * by MP4_ROBOT_ENABLE alone, and a narrower guard on the declaration is how
 * you get an implicit-declaration error on a display-less board.
 * cap_display_face_start() already treats "no panel" as success. */
#include "cap_display.h"
#endif

#define APP_ENABLE_MEM_LOG        (0)

#if CONFIG_MP4_ROBOT_ENABLE
/* The alive eyes' IMU feed (cap_display_face.c). Runs on the LVGL task ~30
 * times a second: imu_read is a cached copy under a short mutex, and the gait
 * name a table lookup. "Busy" = the robot is making its own bumps -- a
 * continuous gait, or the Pi driving it -- so a footfall is not mistaken for
 * someone picking it up. */
static bool main_face_motion(cap_display_motion_t *out)
{
    mpx_robot_imu_t imu;

    if (!mpx_robot_ready()) {
        return false;
    }
    mpx_robot_imu_read(&imu);
    out->ax = imu.ax; out->ay = imu.ay; out->az = imu.az;
    out->gx = imu.gx; out->gy = imu.gy; out->gz = imu.gz;
    out->ok = imu.ax != 0.0f || imu.ay != 0.0f || imu.az != 0.0f;
    out->busy = mpx_robot_pi_in_control() ||
                mpx_robot_gait_is_continuous(mpx_robot_current_gait_name());
    out->move = mpx_robot_playing_move(&out->move_seq);   /* the move's own face */
    return true;
}
#endif


/* The robot's own access point. Declared up here rather than beside
 * main_apply_ap_ip() because main_get_wifi_status(), which reports this
 * address, is defined earlier in the file. */
#define MP4_AP_IP_ADDR            "192.168.2.1"

static const char *TAG = "app";

#if CONFIG_MP4_ROBOT_ENABLE && CONFIG_MP4_PI_LINK_ENABLE
/* ── The Raspberry Pi (ROS) link: what changes while the Pi drives ─────────
 *
 * mpx_pi_link is mpv2's Pi link. When the ROS host takes control
 * (robot::master::begin()) mpv2 turned voice off and put a ROS badge on the
 * face; this does the same here, and also stops music, because the Pi may
 * play its own sound through the speaker (AUDIO_DATA). When the Pi lets go,
 * hands-free listening comes back if it was on.
 *
 * Both callbacks run on the Pi link task, which also carries the failsafe,
 * so they only post an event; this small task does the work that can block
 * (the display lock, stopping the microphone). */
enum { MAIN_PI_MASTER_ON = 1, MAIN_PI_MASTER_OFF, MAIN_PI_IP };
typedef struct {
    uint8_t kind;
    char    ip[16];
} main_pi_event_t;

static QueueHandle_t s_pi_events;
static volatile bool s_voice_resume;   /* hands-free was on when the Pi took over */

static void main_pi_post(uint8_t kind, const char *ip)
{
    main_pi_event_t ev = { .kind = kind };
    if (ip) {
        snprintf(ev.ip, sizeof(ev.ip), "%s", ip);
    }
    if (s_pi_events) {
        (void)xQueueSend(s_pi_events, &ev, 0);
    }
}

static void main_on_master_change(bool active)
{
    main_pi_post(active ? MAIN_PI_MASTER_ON : MAIN_PI_MASTER_OFF, NULL);
}

static void main_on_pi_ip(const char *ip)
{
    main_pi_post(MAIN_PI_IP, ip);
}

static void main_pi_events_task(void *arg)
{
    main_pi_event_t ev;
    (void)arg;

    for (;;) {
        if (xQueueReceive(s_pi_events, &ev, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (ev.kind) {
        case MAIN_PI_MASTER_ON:
            ESP_LOGW(TAG, "Raspberry Pi (ROS) in control: voice and music off, "
                          "app/agent/skills cannot move the robot");
            cap_display_set_ros_mode(true);
#if CONFIG_MP4_VOICE_ENABLE
            if (cap_music_active()) {
                cap_music_stop();
            }
            if (mpx_voice_wake_auto_active()) {
                s_voice_resume = true;
                mpx_voice_wake_auto_stop();
            }
#endif
            break;
        case MAIN_PI_MASTER_OFF:
            ESP_LOGW(TAG, "Raspberry Pi released the robot: back to normal");
            cap_display_set_ros_mode(false);
#if CONFIG_MP4_VOICE_ENABLE
            if (s_voice_resume) {
                s_voice_resume = false;
                if (mpx_voice_wake_auto_start() != ESP_OK) {
                    ESP_LOGW(TAG, "hands-free listening did not restart -- `wake auto on`");
                }
            }
#endif
            break;
        case MAIN_PI_IP:
            cap_display_face_set_pi(ev.ip);
            break;
        default:
            break;
        }
    }
}

/* Straight after the robot, as mpv2 does: the failsafe has to exist before
 * anything later in boot can stall, because a Pi that took control must
 * always be able to lose it again. */
static void main_pi_link_start(void)
{
    s_pi_events = xQueueCreate(8, sizeof(main_pi_event_t));
    if (!s_pi_events ||
        xTaskCreate(main_pi_events_task, "pi_events", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Pi link event task did not start");
    }
    mpx_robot_set_master_observer(main_on_master_change);
    mpx_pi_link_set_ip_observer(main_on_pi_ip);
    mpx_pi_link_set_video_sink(cap_display_show_jpeg);   /* the Pi's camera picture */
    if (mpx_pi_link_init() != ESP_OK) {
        ESP_LOGE(TAG, "Pi link did not start -- ROS host control unavailable");
    }
}
#endif

static app_config_t *s_config;
static app_claw_config_t *s_claw_config;

/* What the Wi-Fi endpoints under /v1/wifi/ still need after boot.
 *
 * s_config is freed at the end of app_main -- deliberately, because the full
 * app_config_t is about 6.5 KB and holds the LLM API key, the Telegram bot
 * token and every other secret. Keeping it resident just so the Wi-Fi
 * endpoints can read four fields would leave all of that in RAM for the life
 * of the device, which is a poor trade for a struct that is written once at
 * boot.
 *
 * So the Wi-Fi services keep their own copy of exactly what they need and
 * nothing else. The STA password is deliberately NOT here: /v1/wifi/connect
 * receives it as an argument, and no other caller needs it. Anything that has
 * to change what is stored loads a fresh app_config_t from NVS, edits it,
 * saves it and frees it -- see main_wifi_store_credentials(). */
typedef struct {
    char sta_ssid[APP_CONFIG_STR_LEN];
    char ap_ssid[APP_CONFIG_STR_LEN];
    char ap_password[APP_CONFIG_STR_LEN];
    char ap_behavior[16];
} main_wifi_state_t;

static main_wifi_state_t s_wifi;

static void main_wifi_state_capture(const app_config_t *config)
{
    if (!config) {
        return;
    }
    strlcpy(s_wifi.sta_ssid, config->wifi_ssid, sizeof(s_wifi.sta_ssid));
    strlcpy(s_wifi.ap_ssid, config->ap_ssid, sizeof(s_wifi.ap_ssid));
    strlcpy(s_wifi.ap_password, config->ap_password, sizeof(s_wifi.ap_password));
    strlcpy(s_wifi.ap_behavior, config->ap_behavior, sizeof(s_wifi.ap_behavior));
}

static esp_err_t app_allocate_runtime_state(void)
{
    if (!s_config) {
        s_config = calloc(1, sizeof(*s_config));
    }
    if (!s_claw_config) {
        s_claw_config = calloc(1, sizeof(*s_claw_config));
    }

    ESP_RETURN_ON_FALSE(s_config && s_claw_config, ESP_ERR_NO_MEM, TAG,
                        "Failed to allocate runtime state");

    return ESP_OK;
}

static void app_free_runtime_state(void)
{
    free(s_claw_config);
    s_claw_config = NULL;

    free(s_config);
    s_config = NULL;
}

static void log_wifi_startup_config(const app_config_t *config)
{
    ESP_LOGI(TAG,
             "Wi-Fi startup STA: ssid=%s pwd_len=%u",
             config->wifi_ssid[0] ? config->wifi_ssid : "(empty)",
             (unsigned)strlen(config->wifi_password));

    ESP_LOGI(TAG,
             "Wi-Fi startup AP: ssid=%s pwd_len=%u behavior=%s",
             config->ap_ssid[0] ? config->ap_ssid : "(auto:mac-suffix)",
             (unsigned)strlen(config->ap_password),
             config->ap_behavior[0] ? config->ap_behavior : "keep");
}

/* ── Why the station link failed ───────────────────────────────────────────
 *
 * wifi_manager logs `reason=15` and retries. Nothing turns that number into
 * something a person can act on, and 15 is by far the most common one:
 * WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT, which in practice means the password is
 * wrong. The radio associates regardless of the password -- association does
 * not check it -- so the log shows a clean `auth -> assoc -> run` and only
 * fails three seconds later at the handshake. Read quickly, that looks like a
 * connection that keeps dropping rather than one that was never accepted.
 *
 * This handler is the cheapest thing that could work: store the code, and log
 * a decoded line only when it changes, so a ten-second retry loop does not
 * become a ten-second log loop. It runs on the system event task, so it must
 * stay this small.
 */
static volatile int s_sta_reason;
static const char *s_sta_error = "";

static const char *main_wifi_reason_text(int reason)
{
    switch (reason) {
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return "wrong password";
        case WIFI_REASON_NO_AP_FOUND:
            return "network not found";
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_802_1X_AUTH_FAILED:
            return "authentication rejected";
        case WIFI_REASON_ASSOC_FAIL:
        case WIFI_REASON_ASSOC_EXPIRE:
        case WIFI_REASON_ASSOC_TOOMANY:
            return "access point refused the connection";
        case WIFI_REASON_BEACON_TIMEOUT:
        case WIFI_REASON_AUTH_EXPIRE:
            return "signal lost";
        case WIFI_REASON_CONNECTION_FAIL:
            return "could not connect";
        default:
            return "disconnected";
    }
}

static void on_wifi_disconnected(void *arg, esp_event_base_t base,
                                 int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    (void)event_id;

    const wifi_event_sta_disconnected_t *ev =
        (const wifi_event_sta_disconnected_t *)event_data;
    if (!ev) {
        return;
    }

    const int reason = ev->reason;
    if (reason == s_sta_reason) {
        return;
    }
    s_sta_reason = reason;
    s_sta_error = main_wifi_reason_text(reason);
    ESP_LOGW(TAG, "Wi-Fi station failed: %s (reason=%d)", s_sta_error, reason);
}

/* ── Network status, off the event task ────────────────────────────────────
 *
 * wifi_manager invokes the state callback from IDF's system event task
 * ("sys_evt"), whose stack is CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE -- 2304
 * bytes by default, sized for IDF's own handlers and nothing more.
 *
 * Doing the work there overflowed it:
 *
 *   ***ERROR*** A stack overflow in task sys_evt has been detected.
 *
 * Three things add up. The log line goes through the capture hook in
 * mpx_util/log_ring.cc, which formats into its own buffer before chaining to
 * the UART sink. app_claw_set_network_status() updates system_ui. And
 * cap_display_face_set_network() takes the display lock and writes an LVGL
 * label -- LVGL is the expensive one, and on a board with no panel it also
 * logs an error from inside the lock, which re-enters the same capture hook.
 *
 * Raising the event task stack alone would be treating the symptom. Blocking
 * sys_evt is the real problem: every Wi-Fi and IP event in the system queues
 * behind it, and on the MP4 board this contends for the display lock with the
 * face's own animation timer. So the callback now does the cheap part only --
 * read the status, copy the strings it needs, post -- and a worker task does
 * the rest.
 *
 * The queue holds one item and is written with xQueueOverwrite, so the
 * callback never blocks and never fails. Coalescing two transitions into the
 * latest one is correct here: this drives a status line, not a state machine. */
typedef struct {
    bool connected;
    bool ap_active;
    char mode[16];
    char ap_ssid[33];
    char sta_ip[16];
} main_net_status_t;

static QueueHandle_t s_net_status_queue;

static void main_apply_net_status(const main_net_status_t *st)
{
    const char *ap_ssid = st->ap_active && st->ap_ssid[0] ? st->ap_ssid : NULL;

    /* The station IP is in this line because it is the single most-wanted
     * value on the console -- it is what you type into a browser and what
     * mpx-cli --ip wants -- and without it "sta_connected=1" reads as an
     * unfinished connection. IDF prints it once, on a line from
     * esp_netif_handlers that is easy to miss in a busy log. */
    ESP_LOGI(TAG, "Wi-Fi state: sta_connected=%d ip=%s ap_active=%d mode=%s ap_ssid=%s",
             st->connected,
             st->connected && st->sta_ip[0] ? st->sta_ip : "(none)",
             st->ap_active,
             st->mode[0] ? st->mode : "off",
             ap_ssid ? ap_ssid : "(none)");

    if (st->connected && st->sta_ip[0]) {
        ESP_LOGW(TAG, "*** Robot on the network: http://%s/  (mpx-cli --ip %s) ***",
                 st->sta_ip, st->sta_ip);
    }

    esp_err_t err = app_claw_set_network_status(st->connected, ap_ssid);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to update network UI: %s", esp_err_to_name(err));
    }

#if CONFIG_MP4_ROBOT_ENABLE
    /* The face carries this line itself: system_ui's status bar is on a home
     * screen this board cannot show. */
    cap_display_face_set_network(st->connected, ap_ssid, st->sta_ip);
#endif
}

static void main_net_status_task(void *arg)
{
    (void)arg;
    main_net_status_t st;

    for (;;) {
        if (xQueueReceive(s_net_status_queue, &st, portMAX_DELAY) == pdTRUE) {
            main_apply_net_status(&st);
        }
    }
}

static esp_err_t main_net_status_start(void)
{
    s_net_status_queue = xQueueCreate(1, sizeof(main_net_status_t));
    ESP_RETURN_ON_FALSE(s_net_status_queue, ESP_ERR_NO_MEM, TAG,
                        "Failed to create network status queue");

    /* 4 KB because LVGL is on the far end of this. Low priority: nothing
     * waits on it, and it must never preempt the gait task. */
    BaseType_t ok = xTaskCreate(main_net_status_task, "net_status", 4096,
                                NULL, 3, NULL);
    if (ok != pdPASS) {
        vQueueDelete(s_net_status_queue);
        s_net_status_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void on_wifi_state_changed(bool connected, void *user_ctx)
{
    (void)user_ctx;

    wifi_manager_status_t status = {0};
    wifi_manager_get_status(&status);

    main_net_status_t st = {0};
    st.connected = connected;
    st.ap_active = status.ap_active;
    if (status.mode) {
        strlcpy(st.mode, status.mode, sizeof(st.mode));
    }
    if (status.ap_active && status.ap_ssid) {
        strlcpy(st.ap_ssid, status.ap_ssid, sizeof(st.ap_ssid));
    }
    if (status.sta_ip) {
        strlcpy(st.sta_ip, status.sta_ip, sizeof(st.sta_ip));
    }

    if (s_net_status_queue) {
        xQueueOverwrite(s_net_status_queue, &st);
    } else {
        /* Before the worker exists -- the first callback can arrive from
         * inside wifi_manager_start(). That one runs on the caller's stack,
         * which is app_main's, and has room. */
        main_apply_net_status(&st);
    }
}

/* One line saying how much room is left, and -- more usefully -- the largest
 * single block, because an allocation fails on contiguous space long before
 * the total runs out. Called at the points where the boot sequence takes its
 * big bites, so an ESP_ERR_NO_MEM has a number next to it instead of being a
 * guess. Cheap enough to leave in unconditionally. */
static void main_log_heap(const char *stage)
{
    const size_t internal_free =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internal_block =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    if (psram_free) {
        ESP_LOGI(TAG, "heap [%s]: internal %u free / %u largest, PSRAM %u free",
                 stage, (unsigned)internal_free, (unsigned)internal_block,
                 (unsigned)psram_free);
    } else {
        ESP_LOGI(TAG, "heap [%s]: internal %u free / %u largest, no PSRAM",
                 stage, (unsigned)internal_free, (unsigned)internal_block);
    }
}

static esp_err_t main_load_config(app_config_t *config)
{
    return app_config_load(config);
}

static esp_err_t main_save_config(const app_config_t *config)
{
    esp_err_t err;
    app_claw_config_t *claw_config = NULL;

    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_ERROR(app_config_validate_wifi(config, NULL), TAG, "Invalid Wi-Fi config");

    err = app_config_save(config);
    if (err != ESP_OK) {
        return err;
    }

    claw_config = calloc(1, sizeof(*claw_config));
    if (!claw_config) {
        ESP_LOGW(TAG, "Failed to allocate Claw config for runtime update");
        return ESP_OK;
    }
    app_config_to_claw(config, claw_config);
    err = app_claw_update_config(claw_config);
    free(claw_config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Failed to update running Claw config: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

static void main_copy_claw_to_app_config(const app_claw_config_t *src, app_config_t *dst)
{
    strlcpy(dst->llm_api_key, src->llm_api_key, sizeof(dst->llm_api_key));
    strlcpy(dst->llm_backend_type, src->llm_backend_type, sizeof(dst->llm_backend_type));
    strlcpy(dst->llm_model, src->llm_model, sizeof(dst->llm_model));
    strlcpy(dst->llm_base_url, src->llm_base_url, sizeof(dst->llm_base_url));
    strlcpy(dst->llm_auth_type, src->llm_auth_type, sizeof(dst->llm_auth_type));
    strlcpy(dst->llm_timeout_ms, src->llm_timeout_ms, sizeof(dst->llm_timeout_ms));
    strlcpy(dst->llm_max_tokens, src->llm_max_tokens, sizeof(dst->llm_max_tokens));
    strlcpy(dst->llm_default_image_max_bytes,
            src->llm_default_image_max_bytes,
            sizeof(dst->llm_default_image_max_bytes));
    strlcpy(dst->llm_max_tokens_field, src->llm_max_tokens_field, sizeof(dst->llm_max_tokens_field));
    strlcpy(dst->llm_supports_tools, src->llm_supports_tools, sizeof(dst->llm_supports_tools));
    strlcpy(dst->llm_supports_vision, src->llm_supports_vision, sizeof(dst->llm_supports_vision));
    strlcpy(dst->llm_image_remote_url_only,
            src->llm_image_remote_url_only,
            sizeof(dst->llm_image_remote_url_only));
}

static esp_err_t main_save_claw_config(const app_claw_config_t *config, void *user_ctx)
{
    esp_err_t err;
    app_config_t *app_config = NULL;

    (void)user_ctx;
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");

    app_config = calloc(1, sizeof(*app_config));
    ESP_RETURN_ON_FALSE(app_config, ESP_ERR_NO_MEM, TAG, "Failed to allocate app config for Claw save");

    err = app_config_load(app_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load config for Claw save: %s", esp_err_to_name(err));
        free(app_config);
        return err;
    }
    main_copy_claw_to_app_config(config, app_config);
    err = app_config_save(app_config);
    free(app_config);
    return err;
}

static esp_err_t main_get_wifi_status(http_server_wifi_status_t *status)
{
    ESP_RETURN_ON_FALSE(status, ESP_ERR_INVALID_ARG, TAG, "status is NULL");

    wifi_manager_status_t wifi_status = {0};
    wifi_manager_get_status(&wifi_status);
    status->wifi_connected = wifi_status.sta_connected;
    status->ip = wifi_status.sta_ip;
    status->ap_active = wifi_status.ap_active;
    status->ap_ssid = wifi_status.ap_ssid;
    /* Not wifi_status.ap_ip: wifi_manager snapshots that string when the AP
     * comes up and never refreshes it, so it still holds the address the
     * interface had before main_apply_ap_ip() renumbered it. Everything that
     * tells a user where to point -- /v1/wifi/status, the captive portal
     * redirect -- reads it through here, so this is the one place to correct. */
    status->ap_ip = wifi_status.ap_active ? MP4_AP_IP_ADDR : wifi_status.ap_ip;
    status->wifi_mode = wifi_status.mode;
    status->sta_configured = wifi_status.sta_configured;
    /* The SSID comes from the saved config rather than the radio, which does
     * not expose it -- but only while the radio is actually carrying it.
     * Reporting a name that /v1/wifi/disconnect just took off the air would
     * have the setup screen showing a network the robot is not on. */
    status->sta_ssid = (wifi_status.sta_configured || wifi_status.sta_connected)
                       ? s_wifi.sta_ssid : "";
    status->sta_error = wifi_status.sta_connected ? "" : s_sta_error;
    return ESP_OK;
}

static void main_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t main_restart_device(void);

/* ── `ai` console command ──────────────────────────────────────────────────
 *
 *   ai                 show the mode (and the one this boot is running)
 *   ai mangdang|off    switch, then restart
 *   ai own             switch to the owner's own LLM -- only if one is saved;
 *                      the key itself is entered in the web app (Settings >
 *                      AI & Voice), never typed into a serial log. */
static int main_ai_cmd(int argc, char **argv)
{
    app_config_t *config = calloc(1, sizeof(*config));
    app_ai_mode_t want;
    int rc = 0;

    if (!config || app_config_load_stored(config) != ESP_OK) {
        printf("ai: could not load settings\n");
        free(config);
        return 1;
    }
    if (argc < 2) {
        printf("AI mode   : %s (running: %s)\n",
               app_config_ai_mode_name(app_config_ai_mode(config)),
               app_config_ai_mode_name(app_config_running_ai_mode()));
        printf("own LLM   : %s %s model=%s key=%s\n",
               config->llm_backend_type[0] ? config->llm_backend_type : "-",
               config->llm_base_url[0] ? config->llm_base_url : "-",
               config->llm_model[0] ? config->llm_model : "-",
               config->llm_api_key[0] ? "saved" : "none");
        printf("usage: ai [mangdang|own|off]   (own needs a key saved in the web app)\n");
    } else if (!app_config_parse_ai_mode(argv[1], &want)) {
        printf("ai: mode must be mangdang, own or off\n");
        rc = 1;
    } else if (want == APP_AI_OWN && (!config->llm_api_key[0] || !config->llm_base_url[0])) {
        printf("ai: no own LLM saved yet -- set it in the web app: Settings > AI & Voice\n");
        rc = 1;
    } else {
        strlcpy(config->ai_mode, app_config_ai_mode_name(want), sizeof(config->ai_mode));
        if (app_config_save(config) != ESP_OK) {
            printf("ai: save failed\n");
            rc = 1;
        } else {
            printf("ai: mode %s saved -- restarting\n", config->ai_mode);
            (void)main_restart_device();
        }
    }
    memset(config->llm_api_key, 0, sizeof(config->llm_api_key));
    free(config);
    return rc;
}

#if CONFIG_MP4_ROBOT_ENABLE
/* `servo` -- servo power: soft start at boot, rest when unused (robot.cc). */
static int main_servo_cmd(int argc, char **argv)
{
    const char *op = argc >= 2 ? argv[1] : "";

    if (strcmp(op, "rest") == 0 && argc >= 3) {
        const uint32_t s = strcmp(argv[2], "off") == 0 ? 0 : (uint32_t)strtoul(argv[2], NULL, 10);
        mpx_robot_set_rest_after(s);
    } else if (strcmp(op, "sleep") == 0) {
        mpx_robot_rest_now();
    } else if (strcmp(op, "wake") == 0) {
        mpx_robot_wake();
    } else if (op[0]) {
        printf("usage: servo [rest <seconds>|rest off|sleep|wake]\n"
               "  rest 300   lie down and switch the servos off after 5 min unused (default)\n"
               "  rest off   never\n"
               "  sleep      lie down and switch off now\n"
               "  wake       stand back up (any movement command also wakes it)\n");
        return 0;
    }
    const uint32_t s = mpx_robot_rest_after();
    printf("servos: %s; rest after %s", mpx_robot_power_state(), s ? "" : "never\n");
    if (s) {
        printf("%u min %u s unused\n", (unsigned)(s / 60), (unsigned)(s % 60));
    }
    return 0;
}

static void main_register_servo_command(void)
{
    const esp_console_cmd_t cmd = {
        .command = "servo",
        .help = "Servo power: servo [rest <seconds>|rest off|sleep|wake]",
        .hint = NULL,
        .func = &main_servo_cmd,
    };
    if (esp_console_cmd_register(&cmd) != ESP_OK) {
        ESP_LOGW(TAG, "`servo` console command not registered");
    }
}
#endif

static void main_register_ai_command(void)
{
    const esp_console_cmd_t cmd = {
        .command = "ai",
        .help = "Where the AI comes from: ai [mangdang|own|off]",
        .func = &main_ai_cmd,
    };
    if (esp_console_cmd_register(&cmd) != ESP_OK) {
        ESP_LOGW(TAG, "`ai` console command not registered");
    }
}

static esp_err_t main_restart_device(void)
{
    BaseType_t ok = xTaskCreate(main_restart_task, "http_restart", 2048, NULL, 5, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "Failed to create restart task");
    return ESP_OK;
}

/* ── The robot's own access point ──────────────────────────────────────────
 *
 * ESP-Claw's wifi_manager leaves the AP on the ESP-IDF default, 192.168.4.1.
 * The MPX toolchain does not: mpx-cli defaults to 192.168.2.1, its
 * connection-failure hint names that address, and so does every note anyone
 * has written down about these robots. Moving the firmware is a dozen lines;
 * moving the toolchain means a per-machine .env on every clone forever, and a
 * built-in error hint that confidently tells you the wrong address.
 */
static esp_err_t main_apply_ap_ip(void)
{
    esp_netif_t *ap = wifi_manager_get_ap_netif();
    esp_netif_ip_info_t ip = {0};

    ESP_RETURN_ON_FALSE(ap, ESP_ERR_INVALID_STATE, TAG, "no AP netif");

    ip.ip.addr      = esp_ip4addr_aton(MP4_AP_IP_ADDR);
    ip.gw.addr      = ip.ip.addr;
    ip.netmask.addr = esp_ip4addr_aton("255.255.255.0");

    /* The DHCP server has to be down to renumber the interface, and it hands
     * out the gateway from this same ip_info once it is back up. Do this
     * before mpx_captive_dns_start(), which writes the DNS option into the very
     * DHCP configuration a restart here would otherwise discard. */
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_stop(ap), TAG, "AP dhcps stop failed");
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(ap, &ip), TAG, "AP set ip failed");
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(ap), TAG, "AP dhcps start failed");

    ESP_LOGI(TAG, "AP address set to %s", MP4_AP_IP_ADDR);
    return ESP_OK;
}

/* ── Live Wi-Fi control for the PWA's setup screen ─────────────────────────
 *
 * ESP-Claw's /api/config saves credentials and asks for a reboot. The PWA does
 * not reboot: the phone running it is normally joined to the robot's own AP,
 * so a restart mid-setup drops it off the network and leaves the user on a
 * page that cannot reload. These three apply the change to the radio as well,
 * and the setup screen polls /v1/wifi/status for the outcome.
 *
 * Credentials still travel through main_save_config, so validation and the
 * running Claw config see them exactly as they would from the settings page.
 */
static esp_err_t main_wifi_apply(const char *sta_ssid, const char *sta_password)
{
    /* The AP half is passed every time because apply_sta_config replaces the
     * whole configuration: omitting it would silently rename the robot's own
     * access point back to the default while joining a network. */
    /* Under the tuner's lock: it rewrites the station config read-modify-
     * write, and must never write the old network back over this one. */
    mpx_wifi_tune_config_lock();
    esp_err_t err = wifi_manager_apply_sta_config(&(wifi_manager_config_t) {
        .sta_ssid = sta_ssid,
        .sta_password = sta_password,
        .ap_ssid = s_wifi.ap_ssid[0] ? s_wifi.ap_ssid : NULL,
        .ap_password = s_wifi.ap_password[0] ? s_wifi.ap_password : NULL,
        .ap_behavior = s_wifi.ap_behavior,
    });
    mpx_wifi_tune_config_unlock();
    return err;
}

/* Persist a change to the stored STA credentials.
 *
 * Loads the whole config from NVS, edits the two fields, saves, frees. The
 * temporary is about 6.5 KB, which is why it is heap rather than stack -- the
 * httpd task's stack is 8 KB and this runs on it. Passing NULL for ssid
 * clears both fields, which is what /v1/wifi/forget wants. */
static esp_err_t main_wifi_store_credentials(const char *ssid, const char *password)
{
    app_config_t *config = calloc(1, sizeof(*config));
    ESP_RETURN_ON_FALSE(config, ESP_ERR_NO_MEM, TAG, "Out of memory for config");

    esp_err_t err = app_config_load(config);
    if (err == ESP_OK) {
        strlcpy(config->wifi_ssid, ssid ? ssid : "", sizeof(config->wifi_ssid));
        strlcpy(config->wifi_password, password ? password : "",
                sizeof(config->wifi_password));
        err = main_save_config(config);
        if (err == ESP_OK) {
            strlcpy(s_wifi.sta_ssid, config->wifi_ssid, sizeof(s_wifi.sta_ssid));
        }
    }

    free(config);
    return err;
}

static esp_err_t main_wifi_connect(const char *ssid, const char *password)
{
    ESP_RETURN_ON_FALSE(ssid && ssid[0], ESP_ERR_INVALID_ARG, TAG, "ssid is empty");

    ESP_RETURN_ON_ERROR(main_wifi_store_credentials(ssid, password), TAG,
                        "Failed to save Wi-Fi credentials");

    ESP_LOGI(TAG, "Joining Wi-Fi network %s", ssid);
    return main_wifi_apply(ssid, password);
}

/* Drop the station link and stay dropped.
 *
 * esp_wifi_disconnect() on its own would not do it: wifi_manager arms a
 * reconnect timer from the disconnect event, so the robot would rejoin within
 * seconds and the button would look broken. Reapplying with no station SSID is
 * what actually takes the radio back to AP-only. NVS is untouched, so the next
 * boot -- or the next /v1/wifi/connect -- reconnects. */
static esp_err_t main_wifi_disconnect(void)
{
    ESP_LOGI(TAG, "Dropping Wi-Fi station link (credentials kept)");
    return main_wifi_apply(NULL, NULL);
}

static esp_err_t main_wifi_forget(void)
{
    ESP_LOGW(TAG, "Forgetting Wi-Fi credentials for %s",
             s_wifi.sta_ssid[0] ? s_wifi.sta_ssid : "(none)");

    ESP_RETURN_ON_ERROR(main_wifi_store_credentials(NULL, NULL), TAG,
                        "Failed to clear Wi-Fi credentials");

    return main_wifi_apply(NULL, NULL);
}

#if CONFIG_APP_CLAW_CAP_IM_WECHAT
static esp_err_t main_wechat_login_start(const char *account_id, bool force)
{
    return cap_im_wechat_qr_login_start(account_id, force);
}

static esp_err_t main_wechat_login_get_status(http_server_wechat_login_status_t *status)
{
    esp_err_t ret = ESP_OK;
    cap_im_wechat_qr_login_status_t *raw = NULL;

    ESP_RETURN_ON_FALSE(status, ESP_ERR_INVALID_ARG, TAG, "status is NULL");

    raw = calloc(1, sizeof(*raw));
    ESP_RETURN_ON_FALSE(raw, ESP_ERR_NO_MEM, TAG, "Failed to allocate login status");

    ESP_GOTO_ON_ERROR(cap_im_wechat_qr_login_get_status(raw), cleanup, TAG,
                      "Failed to query WeChat login status");

    memset(status, 0, sizeof(*status));
    status->active = raw->active;
    status->configured = raw->configured;
    status->completed = raw->completed;
    status->persisted = raw->persisted;
    strlcpy(status->session_key, raw->session_key, sizeof(status->session_key));
    strlcpy(status->status, raw->status, sizeof(status->status));
    strlcpy(status->message, raw->message, sizeof(status->message));
    strlcpy(status->qr_data_url, raw->qr_data_url, sizeof(status->qr_data_url));
    strlcpy(status->account_id, raw->account_id, sizeof(status->account_id));
    strlcpy(status->user_id, raw->user_id, sizeof(status->user_id));
    strlcpy(status->token, raw->token, sizeof(status->token));
    strlcpy(status->base_url, raw->base_url, sizeof(status->base_url));

cleanup:
    free(raw);
    return ret;
}

static esp_err_t main_wechat_login_cancel(void)
{
    return cap_im_wechat_qr_login_cancel();
}

static esp_err_t main_wechat_login_mark_persisted(void)
{
    return cap_im_wechat_qr_login_mark_persisted();
}
#endif

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t init_timezone(const char *timezone)
{
    esp_err_t ret = ESP_OK;

    ESP_GOTO_ON_FALSE(timezone && timezone[0] != '\0', ESP_ERR_INVALID_ARG, tz_default, TAG,
                      "Timezone is empty.");
    ESP_GOTO_ON_FALSE(setenv("TZ", timezone, 1) == 0, ESP_FAIL, tz_default, TAG,
                      "Failed to set TZ env");
    tzset();
    ESP_LOGI(TAG, "Timezone set to %s", timezone);
    return ESP_OK;

tz_default:
    assert(setenv("TZ", "CST-8", 1) == 0);
    tzset();
    ESP_LOGI(TAG, "Timezone set to default: CST-8");
    return ret;
}

#if APP_ENABLE_MEM_LOG

static void print_task_stack_info(void)
{
#ifdef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    static TaskStatus_t s_task_status_snapshot[24];
    UBaseType_t count = uxTaskGetSystemState(s_task_status_snapshot,
                                             sizeof(s_task_status_snapshot) / sizeof(s_task_status_snapshot[0]),
                                             NULL);

    for (UBaseType_t i = 0; i < count; i++) {
        ESP_LOGI(TAG,
                 "Task %s  %u",
                 s_task_status_snapshot[i].pcTaskName,
                 s_task_status_snapshot[i].usStackHighWaterMark);
    }
#endif
}

/* Periodic task: print internal free, minimum free, and PSRAM free every 20s */
static void memory_monitor_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t internal_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "Memory: internal_free=%u bytes, internal_min_free=%u bytes, psram_free=%u bytes",
                 (unsigned)internal_free, (unsigned)internal_min, (unsigned)psram_free);
        print_task_stack_info();
    }
}

#endif


#if CONFIG_MP4_ROBOT_ENABLE
/* ── Robot capability groups ──────────────────────────────────────────────
 *
 * ESP-Claw takes application-provided capability groups through a registry
 * that app_capabilities_init() merges with its own compiled table, so none of
 * this requires editing anything inside the submodule. Registration has to
 * happen BEFORE app_claw_start(), which is where that merge runs.
 *
 * The register_fn signature carries the config and storage paths because some
 * built-in groups need them (cap_lua wants the script root, the IM groups want
 * credentials). These three do not: everything they touch is already
 * initialised by the time app_claw_start() is reached. */
static esp_err_t app_register_cap_robot(const app_claw_config_t *config,
                                        const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    return cap_robot_register_group();
}

static esp_err_t app_register_cap_mpx_skill(const app_claw_config_t *config,
                                            const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    return cap_mpx_skill_register_group();
}

#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT
static esp_err_t app_register_cap_display(const app_claw_config_t *config,
                                          const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    return cap_display_register_group();
}
#endif

#if CONFIG_MP4_VOICE_ENABLE
static esp_err_t app_register_cap_music(const app_claw_config_t *config,
                                        const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    return cap_music_register_group();
}
#endif

static void app_register_robot_capabilities(void)
{
    static const app_capability_external_group_t groups[] = {
#if CONFIG_MP4_VOICE_ENABLE
        {
            .group_id = "cap_music",
            .display_name = "Music",
            .llm_visible_by_default = true,
            .reg = app_register_cap_music,
        },
#endif
        {
            .group_id = "cap_robot",
            .display_name = "Robot",
            .llm_visible_by_default = true,
            .reg = app_register_cap_robot,
        },
        {
            .group_id = "cap_mpx_skill",
            .display_name = "Robot skills",
            .llm_visible_by_default = true,
            .reg = app_register_cap_mpx_skill,
        },
#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT
        {
            .group_id = "cap_display",
            .display_name = "Robot display",
            .llm_visible_by_default = true,
            .reg = app_register_cap_display,
        },
#endif
    };

    for (size_t i = 0; i < sizeof(groups) / sizeof(groups[0]); i++) {
        const esp_err_t err = app_capabilities_register_external_group(&groups[i]);
        if (err != ESP_OK) {
            /* Not fatal. A device that cannot walk is still worth booting into
             * the web UI so it can be told why. */
            ESP_LOGE(TAG, "Could not register %s: %s",
                     groups[i].group_id, esp_err_to_name(err));
        }
    }
}
#endif  /* CONFIG_MP4_ROBOT_ENABLE */

void app_main(void)
{
#if CONFIG_MP4_ROBOT_ENABLE
    /* First, before anything logs. The ring installs a vprintf hook that
     * chains to the existing sink, so serial output is unchanged -- but
     * anything logged before this point is not in it, and the boot sequence
     * is exactly the part you want when a device will not come up. */
    mpx_log_ring_init();
#endif

    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);
    esp_log_level_set("http_reuse", ESP_LOG_WARN);

    ESP_LOGI(TAG, "Starting app");
    ESP_LOGI(TAG, "ESP-Claw version: %s", claw_get_version());
    ESP_LOGI(TAG, "ESP-Claw git version: %s", claw_get_git_version());
    ESP_LOGI(TAG, "Edge Agent version: %s", edge_agent_get_version());
    ESP_ERROR_CHECK(app_allocate_runtime_state());
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(app_config_init());
    ESP_ERROR_CHECK(app_config_load(s_config));
    app_config_to_claw(s_config, s_claw_config);
    init_timezone(app_config_get_timezone(s_config)); // no need to check error

    /* Take the Wi-Fi fields now, while the config is still in memory. Every
     * request to the endpoints under /v1/wifi/ reads them from here after
     * boot, because s_config itself is freed at the end of app_main. */
    main_wifi_state_capture(s_config);

    /* Deliberately not ESP_ERROR_CHECK'd.
     *
     * This initialises every chip the selected board file declares. On the
     * MP4 that is the ST7789, the ES7210 and the BMI270; if one of them does
     * not answer -- an unseated FPC, a board revision with a different part --
     * ESP_ERROR_CHECK aborts inside app_main, the panic handler reboots, and
     * it does it again on the next boot. The visible symptom is a silent boot
     * loop with no way in, which is exactly the situation you most need a
     * console and a web UI for.
     *
     * Everything downstream already treats a missing device as a normal
     * configuration: cap_display returns ESP_ERR_NOT_SUPPORTED with no panel,
     * mpx_robot_init() and mpx_wasm_init() warn and continue, and `selftest`
     * reports SKIP. So log loudly and carry on -- a half-initialised board
     * that boots into the agent can be diagnosed; one that reboots cannot. */
    {
        const esp_err_t bm_err = esp_board_manager_init();
        if (bm_err != ESP_OK) {
            ESP_LOGE(TAG, "Board '%s' did not initialise: %s",
                     CONFIG_ESP_BOARD_NAME, esp_err_to_name(bm_err));
            ESP_LOGE(TAG, "Continuing without board peripherals. Run `selftest`"
                          " on the console to see which ones are missing.");
        }
    }
#if CONFIG_MP4_ROBOT_ENABLE
    /* Servos first. The robot needs NVS and the board manager and nothing
     * else -- no files -- so it starts before the file systems mount (about
     * a second), and the legs begin their soft start ~2 s after power-on
     * instead of ~4.6 s. */
    const bool robot_ok = mpx_robot_init();
#if CONFIG_MP4_PI_LINK_ENABLE
    main_pi_link_start();
#endif
#endif

    ESP_ERROR_CHECK(app_fs_init());

    /* Publish the resolved storage roots so any component can compose paths
     * without knowing whether data lives on flash or an SD card. */
    ESP_ERROR_CHECK(claw_paths_set(CLAW_PATH_DATA, app_fs_storage_base_path()));
    ESP_ERROR_CHECK(claw_paths_set(CLAW_PATH_SYSTEM, app_fs_system_base_path()));

#if CONFIG_MP4_ROBOT_ENABLE
    /* Robot HAL: SPI3, the four AT32F413 driver boards, the BMI270, the
     * persisted servo offsets, and the gait task on core 1.
     *
     * Here rather than later because it needs NVS and the board manager and
     * nothing else, and because a robot that cannot stand is worth knowing
     * about before we spend thirty seconds waiting for Wi-Fi. Deliberately
     * NOT fatal: a board with no servo harness attached should still boot
     * into the agent and the web UI so it can be diagnosed.
     *
     * The WASM skill runtime attaches to this later, after app_claw_start(),
     * so that a misbehaving autorun skill cannot keep the web UI from
     * coming up. That ordering is the difference between a bad skill and a
     * brick, and it is why the two are not initialised together. */
    if (!robot_ok) {
        ESP_LOGW(TAG, "Robot init failed - continuing without servos");
    }

    /* Microphone and speaker. Only looks up the board's codec handles, so it
     * is cheap and cannot fail in a way worth reacting to: a board with no
     * audio devices simply reports both as absent. */
    mpx_audio_init();

    /* WAMR, the 74 host functions, and the hook table mpx_robot asks through.
     * Needs mpx_robot up first. Not fatal either: a device that cannot run
     * skills is still a usable robot. */
    if (!mpx_wasm_init()) {
        ESP_LOGW(TAG, "WASM runtime init failed - skills unavailable");
    }

#endif


    main_log_heap("boot");
    ESP_ERROR_CHECK(wifi_manager_init());

#if CONFIG_MP4_ROBOT_ENABLE
    /* Before app_claw_ui_start(), not after.
     *
     * system_ui opens a display session with a NULL config, and whichever
     * caller starts the display service first fixes the LVGL draw-buffer
     * height for every later one. Starting it here is the only way to choose
     * that size without editing the submodule. See
     * CONFIG_MP4_DISPLAY_BUFFER_LINES for why the default is not good enough
     * on a board with PSRAM. */
    (void)cap_display_service_start();
#endif

    /* Also non-fatal, for the same reason: on a board whose panel failed to
     * initialise above, this is where a display-less build would otherwise
     * abort. Without a UI the agent still runs and the web UI still serves. */
    {
        const esp_err_t ui_err = app_claw_ui_start();
        if (ui_err != ESP_OK) {
            ESP_LOGE(TAG, "UI did not start: %s -- continuing headless",
                     esp_err_to_name(ui_err));
        }
    }

#if CONFIG_MP4_ROBOT_ENABLE
    /* Straight after the display service is up, so the eyes are the first
     * thing on the panel -- before Wi-Fi, before the agent. On a robot that
     * takes a few seconds to find a network, a face that is already blinking
     * is the difference between "booting" and "broken". */
    {
        cap_display_face_set_motion_source(main_face_motion);
        const esp_err_t face_err = cap_display_face_start();
        if (face_err != ESP_OK) {
            ESP_LOGW(TAG, "Face not started: %s", esp_err_to_name(face_err));
        }
        /* The Display page's GIFs live in <storage>/display. If a looping
         * one was chosen, it takes the screen again now, instead of the eyes. */
        char anim_dir[96];
        snprintf(anim_dir, sizeof(anim_dir), "%s/display", app_fs_storage_base_path());
        cap_display_anim_set_dir(anim_dir);
        if (face_err == ESP_OK && cap_display_anim_restore() == ESP_OK) {
            ESP_LOGI(TAG, "Display: animation restored");
        }
    }
#endif

    ESP_ERROR_CHECK(http_server_init(&(http_server_config_t) {
        .storage_base_path = app_fs_storage_base_path(),
        .services = {
            .load_config = main_load_config,
            .save_config = main_save_config,
            .get_wifi_status = main_get_wifi_status,
            .restart_device = main_restart_device,
            .wifi_connect = main_wifi_connect,
            .wifi_disconnect = main_wifi_disconnect,
            .wifi_forget = main_wifi_forget,
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
            .wechat_login_start = main_wechat_login_start,
            .wechat_login_get_status = main_wechat_login_get_status,
            .wechat_login_cancel = main_wechat_login_cancel,
            .wechat_login_mark_persisted = main_wechat_login_mark_persisted,
#endif
        },
    }));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                              on_wifi_disconnected, NULL));

    /* Before the callback is registered, so no event can find a half-built
     * worker. */
    if (main_net_status_start() != ESP_OK) {
        ESP_LOGW(TAG, "Network status worker unavailable; updates run inline");
    }
    ESP_ERROR_CHECK(wifi_manager_register_state_callback(on_wifi_state_changed, NULL));

    log_wifi_startup_config(s_config);

    esp_err_t wifi_err = wifi_manager_start(&(wifi_manager_config_t) {
        .sta_ssid = s_config->wifi_ssid,
        .sta_password = s_config->wifi_password,
        .ap_ssid = s_config->ap_ssid[0] ? s_config->ap_ssid : NULL,
        .ap_password = s_config->ap_password[0] ? s_config->ap_password : NULL,
        .ap_behavior = s_config->ap_behavior,
    });
    if (wifi_err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi start failed: %s", esp_err_to_name(wifi_err));
    } else {
        /* Station-side tuning wifi_manager does not do (all-channel scan,
         * strongest AP first, driver retries) applied after the first failed
         * attempt, from its own task. See components/mpx_wifi_tune. */
        if (mpx_wifi_tune_start() != ESP_OK) {
            ESP_LOGW(TAG, "Wi-Fi tuning unavailable; using wifi_manager defaults");
        }
        esp_err_t ap_ip_err = main_apply_ap_ip();
        if (ap_ip_err != ESP_OK) {
            ESP_LOGW(TAG, "Could not move the AP to %s: %s -- mpx-cli will need "
                          "MPX_HOST set", MP4_AP_IP_ADDR, esp_err_to_name(ap_ip_err));
        }
        /* Non-fatal, like the board and UI init above. A route that fails to
         * register costs one endpoint; aborting here costs the console, the
         * agent and any chance of reading why. The error names the route. */
        const esp_err_t http_err = http_server_start();
        if (http_err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP server did not start: %s -- web UI unavailable, "
                          "console still works", esp_err_to_name(http_err));
        }
        /* Our own responder, not ESP-Claw's captive_dns: that one garbles
         * every reply to a phone's EDNS query, and the setup page took ages
         * to come up (see mpx_captive_dns.c). */
        if (mpx_captive_dns_start(wifi_manager_get_ap_netif()) != ESP_OK) {
            ESP_LOGW(TAG, "Captive DNS could not start, portal pop-up disabled");
        }

        if (s_config->wifi_ssid[0] != '\0') {
            esp_err_t wait_err = wifi_manager_wait_connected(30000);
            if (wait_err == ESP_OK) {
                wifi_manager_status_t status = {0};
                wifi_manager_get_status(&status);
                ESP_LOGI(TAG, "Wi-Fi STA ready: %s", status.sta_ip);
            } else if (wait_err == ESP_ERR_TIMEOUT) {
                wifi_manager_status_t status = {0};
                wifi_manager_get_status(&status);
                ESP_LOGW(TAG,
                         "Wi-Fi STA not connected within wait window; retrying in background: mode=%s ap_active=%d ap_ip=%s",
                         status.mode ? status.mode : "off",
                         status.ap_active,
                         status.ap_ip ? status.ap_ip : "0.0.0.0");
            } else {
                ESP_LOGW(TAG, "Wi-Fi STA wait returned error: %s", esp_err_to_name(wait_err));
            }
        }

        wifi_manager_status_t status = {0};
        wifi_manager_get_status(&status);
        if (status.ap_active) {
            const char *portal_auth = s_config->ap_password[0] ? "wpa2" : "open";
            ESP_LOGW(TAG,
                     "*** Provisioning portal: SSID=\"%s\" (auth=%s) IP=%s URL=http://%s/ ***",
                     status.ap_ssid,
                     portal_auth,
                     status.ap_ip,
                     status.ap_ip);
        }
    }

    ESP_ERROR_CHECK(app_claw_set_save_config_callback(main_save_claw_config, NULL));
#if CONFIG_MP4_ROBOT_ENABLE
    /* Must precede app_claw_start(): that is where the external groups are
     * merged with the built-in table and the LLM-visible set is computed. */
    app_register_robot_capabilities();
#endif

    main_log_heap("before agent");

    /* Non-fatal, like every other subsystem above.
     *
     * ESP_ERR_NO_MEM here is the expected outcome on a devkit with no PSRAM:
     * the agent, the scheduler, the event router and the Lua VM all expect
     * external RAM to spill into. Aborting turns that into a boot loop with
     * no console; continuing leaves Wi-Fi, the web server and the console
     * commands up, which is enough to read the heap numbers above and decide
     * what to turn off. */
    const esp_err_t claw_err = app_claw_start(s_claw_config);
    if (claw_err != ESP_OK) {
        ESP_LOGE(TAG, "Agent did not start: %s", esp_err_to_name(claw_err));
        if (claw_err == ESP_ERR_NO_MEM) {
            ESP_LOGE(TAG, "Out of internal RAM. This build expects PSRAM. "
                          "Enable CONFIG_SPIRAM for your module, or set "
                          "MP4_ROBOT_ENABLE=n to drop the robot stack.");
        }
    } else {
        main_log_heap("after agent");
#if CONFIG_MP4_ROBOT_ENABLE
        /* The eyes again, as the home screen. app_claw_start() reloads
         * ESP-Claw's clock/launcher once the skill registry is ready and makes
         * IT the display's default screen -- which is how the robot ended up
         * showing a clock instead of its face (it only kept the face while the
         * agent was failing to start). The face with the IP under it is this
         * robot's home screen, as on mpv2; the launcher needs a touch panel
         * this board does not have. */
        (void)cap_display_face_start();
#endif
    }
#if CONFIG_APP_CLAW_CAP_IM_LOCAL
    ESP_ERROR_CHECK(http_server_webim_bind_im());
#endif

#if CONFIG_MP4_ROBOT_ENABLE
    /* Scan the skills directory, start the IMU event watcher, and run the
     * autorun skill -- deliberately the last thing that happens.
     *
     * An autorun skill that crashes the robot runs again on the next boot,
     * and the next. A user with no serial cable then has a brick, and the
     * thing that bricked it arrived from a marketplace. By this point the
     * web server is listening and the agent is up, so they can always
     * uninstall it. That is the difference between a bad skill and a brick,
     * and it is the whole reason this call is here and not next to
     * mpx_wasm_init(). */
    mpx_wasm_start_skills();
#endif

    register_wifi_command();
    main_register_ai_command();
#if CONFIG_MP4_ROBOT_ENABLE
    register_selftest_command();
    main_register_servo_command();
    register_audio_command();
    register_voice_command();
    cap_display_register_console();
    /* Music: the player task, the `music` command, and the hook that skips
     * the follow-up turn when a song is waiting. Works in every AI mode --
     * with AI off it is the console's. */
#if CONFIG_MP4_VOICE_ENABLE
    cap_music_init();
#endif

#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT
    /* Phase 5: the conversation on the face. mpx_voice_link reports what
     * the server heard, says and feels; cap_display turns that into a
     * subtitle under the eyes and an expression. Registered here rather
     * than inside either component so neither has to know the other exists
     * (cap_display is not even in the build on a board without a panel). */
    {
        static const mpx_voice_ui_hooks_t face_hooks = {
            .heard         = cap_display_voice_heard,
            .saying        = cap_display_voice_saying,
            .emotion       = cap_display_voice_emotion,
            .reply_done    = cap_display_voice_reply_done,
            .session_ended = cap_display_voice_session_ended,
        };
        mpx_voice_set_ui_hooks(&face_hooks);
    }
#endif

#if CONFIG_MP4_PI_LINK_ENABLE
    /* The link itself started right after the robot (main_pi_link_start);
     * this is only `pi status` on the console. */
    register_pi_command();
#endif

#if CONFIG_MP4_UROS_ENABLE
    /* mpv2-uros: the robot is a ROS 2 node itself (micro-ROS over Wi-Fi to an
     * agent on the PC). The task waits for Wi-Fi and the agent on its own,
     * so starting it here costs nothing until `uros agent <PC IP>` is set. */
    mpx_uros_register_console();
    if (mpx_uros_init() != ESP_OK) {
        ESP_LOGE(TAG, "micro-ROS node did not start");
    }
#endif

#if CONFIG_MP4_POLICY_ENABLE
    /* An RL walk policy on the robot itself (`policy` on the console). It
     * only loads and self-tests here; nothing moves until `policy run`. */
    mpx_policy_register_console();
    if (mpx_policy_init() != ESP_OK) {
        ESP_LOGE(TAG, "policy runtime did not start");
    }
#endif

    /* Phase 3 of docs/voice-plan.md: the MCP bridge. Started unconditionally
     * here rather than from `voice connect` -- claw_cap's capabilities are
     * already registered by this point (app_capabilities_register_external_group()
     * ran ahead of app_claw_start(), earlier in this function), so there is
     * nothing to wait on. Starting it now also means `mcp call ...` works
     * from the console immediately, without a socket, which is the Phase 3
     * gate that does not need STT working yet. */
    /* The owner's choice (Settings > AI & Voice, or `ai`): "off" starts no
     * voice at all -- no bridge, no wake word, the microphone never opens;
     * "own" sends voice turns to this robot's agent (the owner's own LLM
     * key) with the server doing only speech-to-text and text-to-speech. */
    const app_ai_mode_t ai_mode = app_config_running_ai_mode();
    ESP_LOGI(TAG, "AI mode: %s", app_config_ai_mode_name(ai_mode));
    if (ai_mode != APP_AI_OFF) {
        if (mpx_mcp_ws_init() != ESP_OK) {
            ESP_LOGW(TAG, "MCP bridge did not start -- `mcp` commands will fail");
        } else if (ai_mode == APP_AI_OWN && mpx_mcp_ws_enable_device_brain() != ESP_OK) {
            ESP_LOGW(TAG, "device brain not enabled -- voice will use the server's AI");
        }
    }
    register_mcp_command();

#if CONFIG_MP4_VOICE_ENABLE
    if (ai_mode == APP_AI_OFF) {
        ESP_LOGW(TAG, "AI is off: voice disabled, the microphone stays closed");
    } else {
    /* Phase 4 of docs/voice-plan.md (first cut): load esp-sr's models and
     * build the AFE instance now, same reasoning as the MCP bridge above --
     * claw_cap is already up, and doing it at boot means `wake start` works
     * immediately rather than paying the model-load cost on the first use.
     * This only builds the front end; whether it then takes the microphone
     * is CONFIG_MP4_VOICE_AUTO_LISTEN's call, just below. */
    if (mpx_voice_wake_init() != ESP_OK) {
        ESP_LOGW(TAG, "wake word init did not complete -- `wake start` will fail");
    }
    /* The AFE is the last big consumer of internal RAM to come up, and the
     * LCD's per-flush DMA bounce buffer (see cap_display_service_start) has
     * to fit in whatever is left. Print it so the margin is visible in the
     * boot log instead of showing up as "Draw bitmap failed: ESP_ERR_NO_MEM"
     * a few hundred milliseconds later. */
    main_log_heap("after wake init");
    register_wake_command();
#if CONFIG_MP4_VOICE_AUTO_LISTEN
    /* Hands-free from here on: the front end takes the microphone and
     * keeps it until `wake auto off`. Nothing network-related happens yet
     * -- the socket is opened on the first wake word, by which time Wi-Fi
     * has had every chance to come up -- so this cannot fail on a board
     * that is still on its provisioning AP. */
#if CONFIG_MP4_PI_LINK_ENABLE
    if (mpx_robot_pi_in_control()) {
        /* The Pi took control during boot: voice stays off until it lets go. */
        s_voice_resume = true;
        ESP_LOGI(TAG, "hands-free listening waits: the Raspberry Pi is in control");
    } else
#endif
    if (mpx_voice_wake_auto_start() != ESP_OK) {
        ESP_LOGW(TAG, "hands-free listening did not start -- `wake auto on` to retry");
    }
#endif
    }   /* ai_mode != APP_AI_OFF */
#endif
#endif

#if APP_ENABLE_MEM_LOG
    /* Start memory monitor: print internal free, min free, PSRAM free every 20s */
    xTaskCreate(memory_monitor_task, "mem_mon", 4096, NULL, 1, NULL);
#endif

    /* s_config and s_claw_config are boot-time scaffolding: about 6.5 KB of
     * strings, most of them secrets, that nothing needs once the subsystems
     * they configured are running.
     *
     * Anything reached from an HTTP handler or a console command runs AFTER
     * this point, so it must not hold a pointer into either of them. That is
     * exactly how /v1/wifi/connect came to fail with "config not loaded" on
     * every request: the endpoints worked from s_config, which by then was
     * NULL. See main_wifi_state_capture() for the pattern to follow -- copy
     * the few fields you need before the free, or reload from NVS on demand. */
    app_free_runtime_state();
}
