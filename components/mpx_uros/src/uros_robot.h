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
