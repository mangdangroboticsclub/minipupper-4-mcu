/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 */
#include "uros_robot.h"

#include <cmath>

#include "esp_log.h"
#include "robot/robot.h"
#include "sdkconfig.h"
#if CONFIG_APP_POLICY_ENABLE
#include "rl/mpx_policy.h"
#endif

namespace {
constexpr int16_t kCdegInvalid = -32768;   // servo_snapshot(): never answered
constexpr const char *TAG = "uros";

// Control (uros_robot_cmd_vel / _control_tick): the micro-ROS task only.
bool    s_ctrl = false;          // ROS holds the robot (robot::master)
bool    s_standing = true;       // gait parked in the stand
int64_t s_last_cmd_us = 0;       // last cmd_vel
int64_t s_last_move_us = 0;      // last non-zero cmd_vel
int64_t s_refused_log_us = 0;

// Someone else drives: the RL walk, a raw host, another master (Pi link).
const char *held_by()
{
#if CONFIG_APP_POLICY_ENABLE
    if (mpx_policy_active()) return "the RL walk";
#endif
    if (robot::host::active()) return "a raw host";
    if (robot::master_active()) return "another master";
    return nullptr;
}
}  // namespace

extern "C" bool uros_robot_cmd_vel(float vx, float vy, float wz, int64_t now_us)
{
    if (!std::isfinite(vx) || !std::isfinite(vy) || !std::isfinite(wz)) return false;
    if (!s_ctrl) {
        const char *who = held_by();
        if (who) {
            if (now_us - s_refused_log_us > 2000000) {
                ESP_LOGW(TAG, "cmd_vel refused: the robot is held by %s", who);
                s_refused_log_us = now_us;
            }
            return false;
        }
        robot::master::begin();      // fences the app, stands
        s_ctrl = true;
        s_standing = true;
        s_last_move_us = now_us;
        ESP_LOGW(TAG, "ROS took control (cmd_vel): the app's motion controls are off until it stops");
    }
    s_last_cmd_us = now_us;
    const bool moving = std::fabs(vx) > 0.002f || std::fabs(vy) > 0.002f || std::fabs(wz) > 0.02f;
    if (moving) {
        s_last_move_us = now_us;
        s_standing = false;          // set_velocity() starts the walk
    }
    robot::master::set_velocity(vx, vy, wz);
    return true;
}

extern "C" void uros_robot_control_tick(int64_t now_us, bool release_now)
{
    if (!s_ctrl) return;
    if (release_now || now_us - s_last_cmd_us > UROS_RELEASE_AFTER_US) {
        robot::master::end();        // stop, stand, the app gets it back
        s_ctrl = false;
        ESP_LOGW(TAG, "ROS released the robot (%s)", release_now ? "session ended" : "no cmd_vel for 2 s");
        return;
    }
    if (!s_standing && now_us - s_last_move_us > UROS_STAND_AFTER_US) {
        robot::master::send_gait_cmd(robot::GaitCmd::None);   // stand still, not step in place
        s_standing = true;
    }
}

extern "C" bool uros_robot_in_control(void)
{
    return s_ctrl;
}

extern "C" void uros_robot_executed_velocity(float *vx, float *vy, float *wz)
{
    float a = 0.0f, b = 0.0f, c = 0.0f;
    robot::executed_velocity(a, b, c);
    *vx = a;
    *vy = b;
    *wz = c;
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
