/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * Settings (NVS namespace "uros"), status and the `uros` console command.
 * The micro-ROS session itself is uros_node.c.
 */
#include "mpx_uros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "uros_priv.h"

static const char *TAG = "uros";

#define NVS_NS "uros"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uros_cfg_t s_cfg;
static uint32_t s_cfg_gen = 1;
static mpx_uros_status_t s_st;
static int64_t s_session_start_us;
static TaskHandle_t s_task;

/* ── settings ─────────────────────────────────────────────────────────── */

static void cfg_defaults(uros_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->enabled = true;
    snprintf(c->agent_ip, sizeof(c->agent_ip), "%s", CONFIG_MP4_UROS_AGENT_IP);
    c->agent_port = CONFIG_MP4_UROS_AGENT_PORT;
    c->domain_id = CONFIG_MP4_UROS_DOMAIN_ID;
    c->imu_hz = CONFIG_MP4_UROS_IMU_HZ;
    c->joint_hz = CONFIG_MP4_UROS_JOINT_HZ;
}

static void cfg_load(uros_cfg_t *c)
{
    cfg_defaults(c);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t u8;
    uint16_t u16;
    size_t len = sizeof(c->agent_ip);
    if (nvs_get_u8(h, "enabled", &u8) == ESP_OK)   c->enabled = u8 != 0;
    (void)nvs_get_str(h, "agent_ip", c->agent_ip, &len);
    if (nvs_get_u16(h, "agent_port", &u16) == ESP_OK && u16) c->agent_port = u16;
    if (nvs_get_u8(h, "domain", &u8) == ESP_OK)    c->domain_id = u8;
    if (nvs_get_u16(h, "imu_hz", &u16) == ESP_OK)  c->imu_hz = u16;
    if (nvs_get_u16(h, "joint_hz", &u16) == ESP_OK) c->joint_hz = u16;
    nvs_close(h);
}

static esp_err_t cfg_save(const uros_cfg_t *c)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (err == ESP_OK) err = nvs_set_u8(h, "enabled", c->enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_str(h, "agent_ip", c->agent_ip);
    if (err == ESP_OK) err = nvs_set_u16(h, "agent_port", c->agent_port);
    if (err == ESP_OK) err = nvs_set_u8(h, "domain", c->domain_id);
    if (err == ESP_OK) err = nvs_set_u16(h, "imu_hz", c->imu_hz);
    if (err == ESP_OK) err = nvs_set_u16(h, "joint_hz", c->joint_hz);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* Apply an edit: store it, bump the generation (the node reconnects). */
static esp_err_t cfg_apply(const uros_cfg_t *c)
{
    portENTER_CRITICAL(&s_lock);
    s_cfg = *c;
    s_cfg_gen++;
    portEXIT_CRITICAL(&s_lock);
    return cfg_save(c);
}

uint32_t uros_cfg_snapshot(uros_cfg_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_cfg;
    const uint32_t g = s_cfg_gen;
    portEXIT_CRITICAL(&s_lock);
    return g;
}

uint32_t uros_cfg_generation(void)
{
    portENTER_CRITICAL(&s_lock);
    const uint32_t g = s_cfg_gen;
    portEXIT_CRITICAL(&s_lock);
    return g;
}

/* ── status ───────────────────────────────────────────────────────────── */

void uros_status_set_state(mpx_uros_state_t s)
{
    portENTER_CRITICAL(&s_lock);
    s_st.state = s;
    portEXIT_CRITICAL(&s_lock);
}

void uros_status_count(uros_counter_t c)
{
    portENTER_CRITICAL(&s_lock);
    switch (c) {
    case UROS_CNT_IMU_SENT:       s_st.imu_sent++;       break;
    case UROS_CNT_IMU_FAIL:       s_st.imu_fail++;       break;
    case UROS_CNT_JOINT_SENT:     s_st.joint_sent++;     break;
    case UROS_CNT_JOINT_FAIL:     s_st.joint_fail++;     break;
    case UROS_CNT_PINGS:          s_st.pings_echoed++;   break;
    case UROS_CNT_SESSIONS:       s_st.sessions++;       break;
    case UROS_CNT_AGENT_LOST:     s_st.agent_lost++;     break;
    case UROS_CNT_OVERRUNS:       s_st.tick_overruns++;  break;
    case UROS_CNT_JOINT_FALLBACK: s_st.joint_fallback++; break;
    }
    portEXIT_CRITICAL(&s_lock);
}

void uros_status_session(bool up, bool time_synced)
{
    portENTER_CRITICAL(&s_lock);
    s_st.time_synced = up && time_synced;
    s_session_start_us = up ? esp_timer_get_time() : 0;
    if (!up) {
        s_st.last_ping_ms = -1;
    }
    portEXIT_CRITICAL(&s_lock);
}

void uros_status_session_clock(bool time_synced)
{
    portENTER_CRITICAL(&s_lock);
    if (s_session_start_us) {
        s_st.time_synced = time_synced;
    }
    portEXIT_CRITICAL(&s_lock);
}

void uros_status_ping_ms(int32_t ms)
{
    portENTER_CRITICAL(&s_lock);
    s_st.last_ping_ms = ms;
    portEXIT_CRITICAL(&s_lock);
}

void mpx_uros_get_status(mpx_uros_status_t *out)
{
    uros_cfg_t c;
    (void)uros_cfg_snapshot(&c);
    portENTER_CRITICAL(&s_lock);
    *out = s_st;
    const int64_t start = s_session_start_us;
    portEXIT_CRITICAL(&s_lock);
    snprintf(out->agent_ip, sizeof(out->agent_ip), "%s", c.agent_ip);
    out->agent_port = c.agent_port;
    out->domain_id = c.domain_id;
    out->imu_hz = c.imu_hz;
    out->joint_hz = c.joint_hz;
    out->connected_s = (out->state == MPX_UROS_CONNECTED && start)
                           ? (uint32_t)((esp_timer_get_time() - start) / 1000000)
                           : 0;
}

const char *mpx_uros_state_name(mpx_uros_state_t s)
{
    switch (s) {
    case MPX_UROS_OFF:          return "off";
    case MPX_UROS_NO_AGENT_IP:  return "no agent address";
    case MPX_UROS_NO_WIFI:      return "waiting for Wi-Fi";
    case MPX_UROS_SEARCHING:    return "looking for the agent";
    case MPX_UROS_CONNECTED:    return "connected";
    }
    return "?";
}

bool uros_wifi_ready(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    return sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0;
}

/* ── console ──────────────────────────────────────────────────────────── */

static void print_status(void)
{
    mpx_uros_status_t st;
    mpx_uros_get_status(&st);
    printf("state        %s\n", mpx_uros_state_name(st.state));
    printf("agent        %s:%u  (ROS_DOMAIN_ID %u)\n",
           st.agent_ip[0] ? st.agent_ip : "(not set: uros agent <PC IP>)",
           (unsigned)st.agent_port, (unsigned)st.domain_id);
    printf("rates        imu/data_raw %u Hz, joint_states %u Hz\n",
           (unsigned)st.imu_hz, (unsigned)st.joint_hz);
    if (st.state == MPX_UROS_CONNECTED) {
        printf("session      up %lu s, clock %s, agent ping %ld ms\n",
               (unsigned long)st.connected_s, st.time_synced ? "synced to the PC" : "NOT synced",
               (long)st.last_ping_ms);
    }
    printf("sessions     %lu started, %lu lost the agent\n",
           (unsigned long)st.sessions, (unsigned long)st.agent_lost);
    printf("imu          %lu sent, %lu failed\n",
           (unsigned long)st.imu_sent, (unsigned long)st.imu_fail);
    printf("joints       %lu sent, %lu failed, %lu with a silent servo\n",
           (unsigned long)st.joint_sent, (unsigned long)st.joint_fail,
           (unsigned long)st.joint_fallback);
    printf("pings        %lu echoed\n", (unsigned long)st.pings_echoed);
    printf("late ticks   %lu\n", (unsigned long)st.tick_overruns);
}

static int uros_cmd(int argc, char **argv)
{
    uros_cfg_t c;
    (void)uros_cfg_snapshot(&c);

    if (argc <= 1 || strcmp(argv[1], "status") == 0) {
        print_status();
        return 0;
    }
    if (strcmp(argv[1], "agent") == 0 && argc >= 3) {
        ip4_addr_t a;
        if (!ip4addr_aton(argv[2], &a)) {
            printf("not an IPv4 address: %s\n", argv[2]);
            return 1;
        }
        snprintf(c.agent_ip, sizeof(c.agent_ip), "%s", argv[2]);
        if (argc >= 4) {
            const long p = strtol(argv[3], NULL, 10);
            if (p < 1 || p > 65535) {
                printf("port must be 1..65535\n");
                return 1;
            }
            c.agent_port = (uint16_t)p;
        }
        c.enabled = true;
    } else if (strcmp(argv[1], "domain") == 0 && argc >= 3) {
        const long d = strtol(argv[2], NULL, 10);
        if (d < 0 || d > 232) {
            printf("ROS_DOMAIN_ID must be 0..232\n");
            return 1;
        }
        c.domain_id = (uint8_t)d;
    } else if (strcmp(argv[1], "rate") == 0 && argc >= 4) {
        const long imu = strtol(argv[2], NULL, 10), js = strtol(argv[3], NULL, 10);
        if (imu < 0 || imu > UROS_IMU_HZ_MAX || js < 0 || js > UROS_JOINT_HZ_MAX) {
            printf("imu 0..%d Hz, joints 0..%d Hz (0 = off)\n", UROS_IMU_HZ_MAX, UROS_JOINT_HZ_MAX);
            return 1;
        }
        c.imu_hz = (uint16_t)imu;
        c.joint_hz = (uint16_t)js;
    } else if (strcmp(argv[1], "on") == 0) {
        c.enabled = true;
    } else if (strcmp(argv[1], "off") == 0) {
        c.enabled = false;
    } else {
        printf("usage: uros [status]\n"
               "       uros agent <PC IP> [port]   where the micro-ROS agent runs (default port 8888)\n"
               "       uros domain <0..232>        ROS_DOMAIN_ID\n"
               "       uros rate <imu Hz> <joint Hz>\n"
               "       uros on | off\n");
        return 1;
    }
    const esp_err_t err = cfg_apply(&c);
    if (err != ESP_OK) {
        printf("applied, but not saved: %s\n", esp_err_to_name(err));
    } else {
        printf("saved; the node reconnects with the new settings\n");
    }
    return 0;
}

void mpx_uros_register_console(void)
{
    const esp_console_cmd_t cmd = {
        .command = "uros",
        .help = "micro-ROS node: uros [status] | agent <ip> [port] | domain <id> | "
                "rate <imu Hz> <joint Hz> | on | off",
        .hint = NULL,
        .func = &uros_cmd,
    };
    if (esp_console_cmd_register(&cmd) != ESP_OK) {
        ESP_LOGW(TAG, "`uros` command not registered");
    }
}

/* ── start ────────────────────────────────────────────────────────────── */

esp_err_t mpx_uros_init(void)
{
    if (s_task) {
        return ESP_OK;
    }
    uros_cfg_t c;
    cfg_load(&c);
    portENTER_CRITICAL(&s_lock);
    s_cfg = c;
    s_st.last_ping_ms = -1;
    portEXIT_CRITICAL(&s_lock);

    /* Core 0 with Wi-Fi and lwIP; the gait runs on core 1. The stack is for
     * rcl/rmw: entity creation goes several calls deep. */
    if (xTaskCreatePinnedToCore(uros_node_task, "uros", 12288, NULL, 5, &s_task, 0) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "micro-ROS node started (agent %s:%u, domain %u)",
             c.agent_ip[0] ? c.agent_ip : "not set", (unsigned)c.agent_port,
             (unsigned)c.domain_id);
    return ESP_OK;
}
