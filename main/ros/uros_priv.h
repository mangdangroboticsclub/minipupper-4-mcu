/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared between mpx_uros.c (settings, status) and uros_node.c (the
 * micro-ROS session).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "mpx_uros.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The publish tick. Each topic keeps its own schedule on it (time-based, not
 * a divider): the IMU samples at 200 Hz on its own task, so a 200 Hz tick
 * would beat against it and drop samples. At 1 kHz a new sample goes out
 * within 1 ms of being taken. */
#define UROS_TICK_HZ      1000
#define UROS_IMU_HZ_MAX   200
#define UROS_JOINT_HZ_MAX 100
#define UROS_ODOM_HZ      50     /* odom/raw */

typedef struct {
    bool     enabled;
    char     agent_ip[16];
    uint16_t agent_port;
    uint8_t  domain_id;
    uint16_t imu_hz;
    uint16_t joint_hz;
} uros_cfg_t;

/** Current settings; returns a generation number that changes on every edit. */
uint32_t uros_cfg_snapshot(uros_cfg_t *out);
uint32_t uros_cfg_generation(void);

/* Status, written by the node task, read by anyone (under a spinlock). */
typedef enum {
    UROS_CNT_IMU_SENT, UROS_CNT_IMU_FAIL,
    UROS_CNT_JOINT_SENT, UROS_CNT_JOINT_FAIL,
    UROS_CNT_PINGS, UROS_CNT_SESSIONS, UROS_CNT_AGENT_LOST,
    UROS_CNT_OVERRUNS, UROS_CNT_JOINT_FALLBACK,
    UROS_CNT_CMD, UROS_CNT_CMD_REFUSED, UROS_CNT_ODOM_SENT, UROS_CNT_ODOM_FAIL,
    UROS_CNT_SCAN_SENT, UROS_CNT_SCAN_FAIL,
} uros_counter_t;

void uros_status_set_state(mpx_uros_state_t s);
void uros_status_count(uros_counter_t c);
void uros_status_session(bool up, bool time_synced);
void uros_status_session_clock(bool time_synced);   /* a later sync succeeded */
void uros_status_ping_ms(int32_t ms);

/** True once the station interface has an IPv4 address. */
bool uros_wifi_ready(void);

/** The node task (uros_node.c). */
void uros_node_task(void *arg);

#ifdef __cplusplus
}
#endif
