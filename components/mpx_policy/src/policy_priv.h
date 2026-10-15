/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * Between policy_run.cc and policy_console.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "policy_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define POLICY_DIR "/fatfs/policies"

esp_err_t policy_load_builtin(char *why, int why_len);
esp_err_t policy_load_file(const char *path, char *why, int why_len);
const pol_net_t *policy_net(void);
float *policy_work(void);
bool policy_is_builtin(void);
/* How much of the network's weights sit in internal RAM (the rest: PSRAM, slower). */
void policy_weights_ram(size_t *internal, size_t *total);
void policy_set_robot_offsets(bool on);
void policy_stand_dd(int out[12]);

/** The IMU now, through the loaded policy's mounting (no gyro bias):
 *  gravity direction and gyro in the body frame, tilt in degrees. */
bool policy_imu_now(double gravity[3], double gyro[3], double *tilt_deg);

/** The last step's observation, raw action and servo targets (dd). */
bool policy_last_step(float obs[45], float raw[12], int dd[12]);

/** Time the network on core 1 at the policy task's priority: us per step. */
bool policy_time_forward(int n, float *avg_us, float *max_us);

#ifdef __cplusplus
}
#endif
