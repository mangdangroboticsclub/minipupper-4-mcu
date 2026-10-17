/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * Firmware servo angles -> mini_pupper_mp4 URDF joint angles. A straight port
 * of ros2jazzympv2/mini_pupper_esp_bridge/servo_map.py with the values of its
 * config/servo_map.yaml, so joint_states match what the Pi bridge published.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define UROS_JOINTS 12

/** URDF joint names, in joint_states order (FR, FL, RR, RL; hip, upper, lower). */
extern const char *const uros_joint_names[UROS_JOINTS];

/** servo_deg[i] = servo i+1 (degrees) -> q[j] radians in uros_joint_names order. */
void uros_joint_angles(const float servo_deg[12], double q[UROS_JOINTS]);

#ifdef __cplusplus
}
#endif
