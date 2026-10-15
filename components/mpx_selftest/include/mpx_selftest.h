/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Register the `selftest` console command.
 *
 *  Checks the board against what the firmware believes is wired: I2C
 *  addresses, the IMU's scaling, the servo chip selects, the panel and the
 *  buttons. Nothing it does moves a servo, so it is safe to run with the
 *  servo rail unpowered -- which is how it should be run first.
 */
void register_selftest_command(void);

#ifdef __cplusplus
}
#endif
