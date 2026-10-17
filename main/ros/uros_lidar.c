/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * LD06 on J-PI: see uros_lidar.h.
 */
#include "uros_lidar.h"

#include <math.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "robot/board_config.h"

static const char *TAG = "ld06";

#define LD_BAUD        230400
#define LD_PKT_LEN     47
#define LD_HEADER      0x54
#define LD_VER_LEN     0x2C      /* one packet, 12 points */
#define LD_POINTS      12
#define LD_RX_BUF      2048

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;
static uint8_t s_crc_table[256];

/* The turn being filled (task only) and the last finished one (locked). */
static float    s_build[UROS_SCAN_BEAMS];
static int64_t  s_build_t_us;
static uint16_t s_build_points;
static uros_scan_t s_ready;
static uros_lidar_stats_t s_st;

static void crc_init(void)
{
    for (int i = 0; i < 256; ++i) {
        uint8_t c = (uint8_t)i;
        for (int b = 0; b < 8; ++b) {
            c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x4D) : (uint8_t)(c << 1);
        }
        s_crc_table[i] = c;
    }
}

static uint8_t crc8(const uint8_t *p, int n)
{
    uint8_t c = 0;
    for (int i = 0; i < n; ++i) {
        c = s_crc_table[(c ^ p[i]) & 0xFF];
    }
    return c;
}

static void build_reset(int64_t t_us)
{
    for (int i = 0; i < UROS_SCAN_BEAMS; ++i) {
        s_build[i] = NAN;          /* no point in this beam (yet) */
    }
    s_build_points = 0;
    s_build_t_us = t_us;
}

static void turn_done(float speed_dps, int64_t now_us)
{
    const float scan_s = speed_dps > 1.0f ? 360.0f / speed_dps : 0.1f;
    portENTER_CRITICAL(&s_lock);
    memcpy(s_ready.range_m, s_build, sizeof(s_build));
    s_ready.t_us = s_build_t_us;
    s_ready.scan_s = scan_s;
    s_ready.points = s_build_points;
    s_ready.seq++;
    s_st.turns++;
    s_st.points = s_build_points;
    s_st.hz = 1.0f / scan_s;
    portEXIT_CRITICAL(&s_lock);
    build_reset(now_us);
}

static inline uint16_t u16le(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void packet(const uint8_t *p, int64_t now_us)
{
    static int32_t last_start = -1;
    const float speed = (float)u16le(p + 2);              /* deg/s */
    const int32_t start = u16le(p + 4);                   /* 0.01 deg */
    int32_t end = u16le(p + 42);
    if (start >= 36000 || end >= 36000) {
        return;
    }
    /* A new turn starts when the start angle wraps past 360 deg. */
    if (last_start >= 0 && start < last_start) {
        turn_done(speed, now_us);
    }
    last_start = start;
    if (end < start) {
        end += 36000;
    }
    const float step = (float)(end - start) / (LD_POINTS - 1);
    for (int i = 0; i < LD_POINTS; ++i) {
        const uint8_t *q = p + 6 + 3 * i;
        const uint16_t mm = u16le(q);
        const int32_t a = ((int32_t)lroundf(start + step * i)) % 36000;   /* LD06: clockwise */
        int beam = (int)lroundf((float)((36000 - a) % 36000) / 100.0f);   /* ROS: counter-clockwise */
        if (beam >= UROS_SCAN_BEAMS) {
            beam -= UROS_SCAN_BEAMS;
        }
        const float r = mm ? mm * 0.001f : INFINITY;   /* 0 = no echo */
        float *b = &s_build[beam];
        if (isnan(*b) || r < *b) {
            *b = r;                /* the nearest point wins */
        }
        s_build_points++;
    }
}

static void lidar_task(void *arg)
{
    (void)arg;
    uint8_t rx[128];
    uint8_t pkt[LD_PKT_LEN];
    int have = 0;
    build_reset(esp_timer_get_time());
    for (;;) {
        const int n = uart_read_bytes(PI_UART_PORT, rx, sizeof(rx), pdMS_TO_TICKS(20));
        const int64_t now = esp_timer_get_time();
        for (int i = 0; i < n; ++i) {
            const uint8_t b = rx[i];
            if (have == 0 && b != LD_HEADER) continue;
            if (have == 1 && b != LD_VER_LEN) {
                have = (b == LD_HEADER) ? 1 : 0;
                continue;
            }
            pkt[have++] = b;
            if (have < LD_PKT_LEN) continue;
            have = 0;
            if (crc8(pkt, LD_PKT_LEN - 1) != pkt[LD_PKT_LEN - 1]) {
                portENTER_CRITICAL(&s_lock);
                s_st.crc_errors++;
                portEXIT_CRITICAL(&s_lock);
                continue;
            }
            portENTER_CRITICAL(&s_lock);
            s_st.packets++;
            portEXIT_CRITICAL(&s_lock);
            packet(pkt, now);
        }
    }
}

esp_err_t uros_lidar_start(void)
{
    if (s_task) {
        return ESP_OK;
    }
    crc_init();
    uart_config_t cfg = {
        .baud_rate = LD_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(PI_UART_PORT, LD_RX_BUF, 0, 0, NULL, 0);
    if (err == ESP_OK) err = uart_param_config(PI_UART_PORT, &cfg);
    /* Receive only: IO18 (J-PI pin 2) stays untouched. */
    if (err == ESP_OK) err = uart_set_pin(PI_UART_PORT, UART_PIN_NO_CHANGE, PI_UART_RX_PIN,
                                          UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART: %s", esp_err_to_name(err));
        return err;
    }
    if (xTaskCreatePinnedToCore(lidar_task, "ld06", 3072, NULL, 6, &s_task, 0) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_lock);
    s_st.running = true;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "LD06 on J-PI (IO8, %d baud): publishing scan once the agent is connected", LD_BAUD);
    return ESP_OK;
}

bool uros_lidar_take(uros_scan_t *out, uint32_t after_seq)
{
    bool fresh;
    portENTER_CRITICAL(&s_lock);
    fresh = s_ready.seq != after_seq && s_ready.seq != 0;
    if (fresh) {
        *out = s_ready;
    }
    portEXIT_CRITICAL(&s_lock);
    return fresh;
}

void uros_lidar_get_stats(uros_lidar_stats_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_st;
    portEXIT_CRITICAL(&s_lock);
}
