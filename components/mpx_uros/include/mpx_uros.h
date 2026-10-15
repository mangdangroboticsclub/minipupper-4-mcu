/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * mpx_uros: the robot as a ROS 2 node over Wi-Fi (micro-ROS, XRCE-DDS over
 * UDP to a micro-ROS agent on the PC). No Raspberry Pi.
 *
 * Phase 0 (link check): the node publishes
 *   imu/data_raw        sensor_msgs/Imu         best effort, body frame (REP-103)
 *   joint_states        sensor_msgs/JointState  best effort, mini_pupper_mp4 names
 * and echoes
 *   uros/ping -> uros/pong   std_msgs/Header    best effort (round-trip test)
 * so the PC can measure rate, loss and latency (tools/uros/uros_check.py).
 * It does not drive the robot yet.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MPX_UROS_OFF = 0,        /* disabled (`uros off`)                       */
    MPX_UROS_NO_AGENT_IP,    /* no agent address set yet                    */
    MPX_UROS_NO_WIFI,        /* waiting for the robot's Wi-Fi               */
    MPX_UROS_SEARCHING,      /* pinging the agent                           */
    MPX_UROS_CONNECTED,      /* session up, publishing                      */
} mpx_uros_state_t;

typedef struct {
    mpx_uros_state_t state;
    char     agent_ip[16];
    uint16_t agent_port;
    uint8_t  domain_id;
    uint16_t imu_hz;
    uint16_t joint_hz;
    bool     time_synced;
    uint32_t sessions;          /* times the session came up               */
    uint32_t connected_s;       /* seconds in the current session          */
    uint32_t imu_sent, imu_fail;
    uint32_t joint_sent, joint_fail;
    uint32_t pings_echoed;
    uint32_t agent_lost;        /* sessions ended because the agent went away */
    int32_t  last_ping_ms;      /* agent ping round trip, -1 = none        */
    uint32_t tick_overruns;     /* publish ticks more than 10 ms late      */
    uint32_t joint_fallback;    /* joints sent as commanded (servo silent) */
} mpx_uros_status_t;

/** Start the node task. Safe to call once, after NVS and Wi-Fi are up. */
esp_err_t mpx_uros_init(void);

void mpx_uros_get_status(mpx_uros_status_t *out);

const char *mpx_uros_state_name(mpx_uros_state_t s);

/** `uros` on the serial console. */
void mpx_uros_register_console(void);

#ifdef __cplusplus
}
#endif
