/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 */
#include "uros_robot.h"

#include "robot/robot.h"

namespace {
constexpr int16_t kCdegInvalid = -32768;   // servo_snapshot(): never answered
}

extern "C" bool uros_robot_imu(uros_imu_t *out)
{
    const robot::ImuData d = robot::imu_read();
    if (d.seq == 0) {
        return false;
    }
    out->acc_g[0] = d.ax;     out->acc_g[1] = d.ay;     out->acc_g[2] = d.az;
    out->gyro_dps[0] = d.gx;  out->gyro_dps[1] = d.gy;  out->gyro_dps[2] = d.gz;
    out->seq  = d.seq;
    out->t_us = d.t_us;
    return true;
}

extern "C" uint16_t uros_robot_servo_deg(float deg[12])
{
    int16_t cmd[12], meas[12];
    robot::servo_snapshot(cmd, meas);
    uint16_t measured = 0;
    for (int i = 0; i < 12; ++i) {
        if (meas[i] != kCdegInvalid) {
            deg[i] = meas[i] * 0.01f;
            measured |= static_cast<uint16_t>(1u << i);
        } else {
            deg[i] = cmd[i] * 0.01f;
        }
    }
    return measured;
}
