/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * The C side's window onto robot.h (which is C++).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float    acc_g[3];      /* chip axes, g     */
    float    gyro_dps[3];   /* chip axes, deg/s */
    uint32_t seq;           /* +1 per new sample, 0 = never sampled */
    int64_t  t_us;          /* esp_timer time of the sample */
} uros_imu_t;

/** Latest IMU sample. False until the IMU has produced one. */
bool uros_robot_imu(uros_imu_t *out);

/* ── Phase 1: ROS drives the robot ─────────────────────────────────────────
 *
 * cmd_vel goes to the robot's own gait (robot::master, as the Pi bridge
 * does over J-PI): the first command takes the robot, unless the RL walk, a
 * raw host or another master holds it. While ROS has it the app's motion
 * controls are fenced. A zero command for UROS_STAND_AFTER_US stands the robot
 * still (instead of stepping in place); no command at all for
 * UROS_RELEASE_AFTER_US, or the agent gone, hands it back to the app. Inside
 * that, robot.cc's own PI_VEL_TIMEOUT_MS (0.5 s) already turns a stale
 * command into stepping in place. All from the micro-ROS task. */
#define UROS_STAND_AFTER_US    1000000
#define UROS_RELEASE_AFTER_US  2000000

/** A cmd_vel, REP-103 (m/s, m/s, rad/s; +y and +wz to the left). False if
 *  refused: not finite, or someone else holds the robot. */
bool uros_robot_cmd_vel(float vx, float vy, float wz, int64_t now_us);

/** Every tick: stand after a while at zero, hand back after silence, or at
 *  once with `release_now` (the session is ending). */
void uros_robot_control_tick(int64_t now_us, bool release_now);

/** True while ROS holds the robot. */
bool uros_robot_in_control(void);

/** The body velocity the gait executes (the command after clamping, zero
 *  when it is not walking), REP-103. Whoever drives: ROS or the app. */
void uros_robot_executed_velocity(float *vx, float *vy, float *wz);

/**
 * All twelve servo angles in degrees (index 0 = servo 1), firmware terms:
 * from the stand pose, calibration offset removed. Measured where the driver
 * board answered, the commanded angle otherwise.
 * @return bit i set = servo i+1 is measured (not the commanded fallback)
 */
uint16_t uros_robot_servo_deg(float deg[12]);

#ifdef __cplusplus
}
#endif
