/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * cap_robot -- the quadruped, as tools the agent can call.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the "cap_robot" capability group.
 *
 * Called by app_claw during startup, via the external-group registration in
 * app_main. Idempotent.
 */
esp_err_t cap_robot_register_group(void);

#ifdef __cplusplus
}
#endif
