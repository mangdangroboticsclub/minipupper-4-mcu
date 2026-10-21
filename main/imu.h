/* imu.h -- the board's BMI270 (U11) on I2C.
 *
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef IMU_H
#define IMU_H

#include <stdbool.h>
#include <stdint.h>

/* Probe and configure the BMI270: 400 Hz, +-4 g, +-1000 deg/s. Returns false
 * (and imu_latest returns zeros) if it does not answer. */
bool imu_init(void);

/* Start the sampling task (every 2 ms; the sensor's output data rate is
 * 400 Hz). The driver's I2C calls wait without a
 * timeout, so they must never run in the servo tick. */
void imu_start(void);

/* The latest sample: acceleration in milli-g, angular rate in 0.1 deg/s,
 * sensor axes, and the ESP32 time it was read. Returns false (zeros) if the
 * IMU is absent or its last good sample is older than max_age_us. */
bool imu_latest(int16_t acc_mg[3], int16_t gyr_ddps[3], uint32_t *t_us, int64_t max_age_us);

#endif /* IMU_H */
