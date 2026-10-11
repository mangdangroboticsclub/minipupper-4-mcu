/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * See display_glue.h. Ported from the display parts of mpv2-claw's main.c.
 *
 * The network line is polled once a second rather than fed from the Wi-Fi
 * event handlers. mpv2-claw's note on this: cap_display_face_set_network()
 * takes the LVGL lock and writes an LVGL object, and the Wi-Fi event task
 * is not somewhere to wait on a lock the LVGL task may be holding while it
 * pushes a frame. A one-second poll from our own low-priority task costs
 * nothing and keeps wifi_sta.cc untouched.
 */

#include "display/display_glue.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "sdkconfig.h"

#include "display/cap_display.h"
#include "display/display_assets.h"
#include "network/wifi_ap.h"
#include "network/wifi_sta.h"
#include "pi/pi_link.h"
#include "robot/mpx_robot.h"

static const char *TAG = "display_glue";

namespace {

constexpr const char *ANIM_DIR = "/fs/display";
constexpr TickType_t  NET_POLL = pdMS_TO_TICKS(1000);

enum class Kind : uint8_t { MasterOn, MasterOff, PiIp };

struct Event {
    Kind kind;
    char ip[16];
};

QueueHandle_t s_events;

/* The alive eyes' IMU feed (cap_display_face.c). Runs on the LVGL task ~30
 * times a second: imu_read is a cached copy under a short mutex, and the
 * gait name a table lookup. "Busy" = the robot is making its own bumps -- a
 * continuous gait, or the Pi driving it -- so a footfall is not mistaken for
 * someone picking it up. */
bool face_motion(cap_display_motion_t *out)
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

void post(Kind kind, const char *ip)
{
    Event ev = { kind, { 0 } };
    if (ip) {
        std::snprintf(ev.ip, sizeof(ev.ip), "%s", ip);
    }
    if (s_events) {
        (void)xQueueSend(s_events, &ev, 0);
    }
}

void on_pi_ip(const char *ip)
{
    post(Kind::PiIp, ip);
}

void poll_network()
{
    static bool last_connected = false;
    static std::string last_ip;
    static bool first = true;

    const bool connected = network::wifi_sta_get_state() == network::StaState::Connected;
    const std::string ip = connected ? network::wifi_sta_get_ip() : std::string();
    if (!first && connected == last_connected && ip == last_ip) {
        return;
    }
    first = false;
    last_connected = connected;
    last_ip = ip;
    cap_display_face_set_network(connected, network::wifi_ap_get_ssid(), ip.c_str());
}

void events_task(void *)
{
    Event ev;
    for (;;) {
        if (xQueueReceive(s_events, &ev, NET_POLL) == pdTRUE) {
            switch (ev.kind) {
            case Kind::MasterOn:
                ESP_LOGW(TAG, "Raspberry Pi (ROS) in control: app and skills cannot move the robot");
                cap_display_set_ros_mode(true);
                break;
            case Kind::MasterOff:
                ESP_LOGW(TAG, "Raspberry Pi released the robot: back to normal");
                cap_display_set_ros_mode(false);
                break;
            case Kind::PiIp:
                cap_display_face_set_pi(ev.ip);
                break;
            }
        }
        poll_network();
    }
}

}  // namespace

void display_glue::start_face()
{
    if (cap_display_service_start() != ESP_OK) {
        return;                       /* compiled out, or the panel did not answer */
    }
    cap_display_face_set_motion_source(face_motion);
    const esp_err_t err = cap_display_face_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "face not started: %s", esp_err_to_name(err));
    }
    /* The queue exists from here so the Pi link (started next, before the
     * network) can post from its first frame; the task that drains it
     * starts with the network (start_events). Posts queue up meanwhile. */
    s_events = xQueueCreate(8, sizeof(Event));
#if CONFIG_APP_PI_LINK_ENABLE
    pi_link::set_ip_observer(on_pi_ip);
    pi_link::set_video_sink(cap_display_show_jpeg);   /* the Pi's camera picture */
#endif
}

void display_glue::start_animations()
{
    cap_display_anim_set_dir(ANIM_DIR);
    (void)display::deploy_builtin_gifs(ANIM_DIR);
    if (cap_display_available() && cap_display_anim_restore() == ESP_OK) {
        ESP_LOGI(TAG, "animation restore queued");
    }
}

void display_glue::start_events()
{
    if (!cap_display_available() || !s_events) {
        return;
    }
    // PSRAM stack: it only reads cached network state and takes the LVGL lock.
    if (xTaskCreateWithCaps(events_task, "disp_events", 4096, nullptr, 3, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "display event task did not start");
    }
}

void display_glue::on_master_changed(bool active)
{
    post(active ? Kind::MasterOn : Kind::MasterOff, nullptr);
}
