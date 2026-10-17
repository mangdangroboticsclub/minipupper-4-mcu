/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * An LDROBOT LD06 lidar on the J-PI header, for the micro-ROS node's `scan`.
 *
 *   LD06 P5V -> J-PI pin 6 (VBUS 5 V)    LD06 Tx  -> J-PI pin 3 (IO8, UART1 RX)
 *   LD06 GND -> J-PI pin 1               LD06 PWM -> not connected (10 Hz)
 *
 * The LD06 talks by itself: 230400 8N1, a 47-byte packet of 12 points about
 * 375 times a second, one turn about 10 times a second. A task on core 0
 * parses the packets (CRC-8, polynomial 0x4D) and bins every turn into 360
 * one-degree beams, counter-clockwise from the LD06's zero angle as
 * ldlidar_stl_ros2 does with laser_scan_dir (angle = 360 - LD06 angle), so
 * the ros2jazzympv2 URDF's lidar_joint (yaw +90 deg) fits as it is. A beam
 * keeps its nearest point; a beam with no point is NaN, a point with no echo
 * (distance 0) +inf.
 *
 * Needs the Pi link off (CONFIG_APP_PI_LINK_ENABLE): they share the port.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UROS_SCAN_BEAMS 360

typedef struct {
    float    range_m[UROS_SCAN_BEAMS];
    int64_t  t_us;        /* esp_timer time of the turn's first packet */
    float    scan_s;      /* one turn, s (from the LD06's own speed) */
    uint16_t points;      /* points that fell into this turn */
    uint32_t seq;         /* +1 per finished turn */
} uros_scan_t;

typedef struct {
    bool     running;
    uint32_t packets, crc_errors, turns;
    float    hz;          /* turns per second, last turn */
    uint16_t points;      /* points in the last turn */
} uros_lidar_stats_t;

/** Start the UART and the parser task. Once; ESP_OK if already running. */
esp_err_t uros_lidar_start(void);

/** The last finished turn, if newer than `after_seq`. */
bool uros_lidar_take(uros_scan_t *out, uint32_t after_seq);

void uros_lidar_get_stats(uros_lidar_stats_t *out);

#ifdef __cplusplus
}
#endif
