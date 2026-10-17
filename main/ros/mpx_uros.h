/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * mpx_uros: the robot as a ROS 2 node over Wi-Fi (micro-ROS, XRCE-DDS over
 * UDP to a micro-ROS agent on the PC). No Raspberry Pi.
 *
 * The node publishes
 *   imu/data_raw        sensor_msgs/Imu         best effort, body frame (REP-103)
 *   joint_states        sensor_msgs/JointState  best effort, mini_pupper_mp4 names
 *   odom/raw            nav_msgs/Odometry       best effort, 50 Hz: the velocity
 *                       the gait executes, integrated (odom -> base_link), for an EKF
 *   scan                sensor_msgs/LaserScan   reliable, ~10 Hz, with an LD06 on J-PI
 *                       (CONFIG_APP_UROS_LIDAR_LD06, uros_lidar.h)
 * subscribes
 *   cmd_vel             geometry_msgs/Twist     the robot's own gait walks it
 *                       (phase 1; uros_robot.h: who may drive, and the timeouts)
 * and echoes
 *   uros/ping -> uros/pong   std_msgs/Header    best effort (round-trip test)
 * so the PC can measure rate, loss and latency (tools/uros/uros_check.py) and
 * drive it from the keyboard (tools/uros/teleop_keys.py). Settings and status:
 * GET / POST /v1/uros (network/http_uros.cc, the RL Walk page's ROS 2 card).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MPX_UROS_OFF = 0,        /* switched off in the app                     */
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
    uint32_t cmd_vel_rx;        /* cmd_vel taken */
    uint32_t cmd_vel_refused;   /* cmd_vel while someone else held the robot */
    uint32_t odom_sent, odom_fail;
    uint32_t scan_sent, scan_fail;  /* scan (LD06), when built in */
    bool     in_control;        /* ROS holds the robot now */
} mpx_uros_status_t;

/* What the app can change; stored in NVS ("uros"). */
typedef struct {
    bool     enabled;
    char     agent_ip[16];      /* the PC running the agent; "" = not set   */
    uint16_t agent_port;
    uint8_t  domain_id;
    uint16_t imu_hz;            /* 0 = off                                  */
    uint16_t joint_hz;          /* 0 = off                                  */
} mpx_uros_settings_t;

/** Start the node task. Safe to call once, after NVS and Wi-Fi are up. */
esp_err_t mpx_uros_init(void);

void mpx_uros_get_status(mpx_uros_status_t *out);

const char *mpx_uros_state_name(mpx_uros_state_t s);

/** Check, store and apply new settings (the node reconnects). On error, `why`
 *  says what was wrong and nothing changed. */
esp_err_t mpx_uros_configure(const mpx_uros_settings_t *s, char *why, size_t why_len);

void mpx_uros_get_settings(mpx_uros_settings_t *out);

#ifdef __cplusplus
}
#endif
