/*
 * SPDX-FileCopyrightText: 2026 MangDang
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The C facade declared in mpx_robot.h.
 *
 * Thin by design: every function here is a name translation onto namespace
 * robot, with no policy of its own. Anything that needs to make a decision
 * belongs either in robot.cc (if it is about the robot) or in the caller (if
 * it is about the agent, the web API or a skill).
 *
 * mpx_robot_set_skill_hooks() is the one exception and it lives in robot.cc,
 * next to the gait loop that reads it.
 */

#include "mpx_robot.h"

#include <cmath>

#include "robot/robot.h"
#include "robot/imu.h"

namespace {
bool s_ready = false;
}

extern "C" {

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

bool mpx_robot_init(void)
{
    if (s_ready) {
        return true;
    }
    s_ready = robot::init();
    return s_ready;
}

bool mpx_robot_ready(void)
{
    return s_ready;
}

/* ── Movement ──────────────────────────────────────────────────────────── */

esp_err_t mpx_robot_gait_by_name(const char *name)
{
    if (name == nullptr || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    robot::GaitCmd cmd;
    if (!robot::gait_from_name(name, cmd)) {
        return ESP_ERR_NOT_FOUND;
    }
    robot::send_gait_cmd(cmd);
    return ESP_OK;
}

const char *mpx_robot_current_gait_name(void)
{
    const char *name = robot::gait_to_name(robot::current_gait_cmd());
    return name != nullptr ? name : "none";
}

const char *mpx_robot_playing_move(uint32_t *seq)
{
    return robot::playing_move(seq);
}

int mpx_robot_gait_name_count(void)
{
    return robot::gait_name_count();
}

const char *mpx_robot_gait_name_at(int index)
{
    if (index < 0 || index >= robot::gait_name_count()) {
        return nullptr;
    }
    return robot::gait_name_at(index);
}

void mpx_robot_drive(float forward, float strafe, float turn)
{
    robot::joy_input(forward, strafe, turn);
}

void mpx_robot_set_body_attitude(float roll_deg, float pitch_deg, float yaw_deg)
{
    robot::set_body_attitude(roll_deg, pitch_deg, yaw_deg);
}

void mpx_robot_set_attitude_speed(float dps)
{
    robot::set_attitude_speed(dps);
}

void mpx_robot_look(float yaw_deg, float pitch_deg)
{
    robot::look_input(yaw_deg, pitch_deg);
}

const char *mpx_robot_gait_note(const char *name)
{
    const char *note = name != nullptr ? robot::gait_note(name) : nullptr;
    return note != nullptr ? note : "";
}

bool mpx_robot_gait_is_continuous(const char *name)
{
    robot::GaitCmd cmd;
    return name != nullptr && robot::gait_from_name(name, cmd) &&
           robot::gait_is_continuous(cmd);
}

bool mpx_robot_pi_in_control(void)
{
    return robot::master_active();
}

void mpx_robot_set_master_observer(void (*cb)(bool active))
{
    robot::set_master_observer(cb);
}

/* ── Servo power ───────────────────────────────────────────────────────── */

void        mpx_robot_set_rest_after(uint32_t s) { robot::set_rest_after(s); }
uint32_t    mpx_robot_rest_after(void)           { return robot::rest_after(); }
const char *mpx_robot_power_state(void)          { return robot::power_state_name(); }
bool        mpx_robot_servos_resting(void)       { return robot::servos_resting(); }
void        mpx_robot_rest_now(void)             { robot::rest_now(); }
void        mpx_robot_wake(void)                 { robot::wake_servos(); }

/* ── Configuration ─────────────────────────────────────────────────────── */

void mpx_robot_get_config(mpx_robot_config_t *out)
{
    if (out == nullptr) {
        return;
    }
    const robot::Config cfg = robot::get_config();
    out->period    = cfg.period;
    out->height    = cfg.height;
    out->up_height = cfg.up_height;
    out->stride    = cfg.stride;
    out->tilt      = cfg.tilt;
    out->sg_speed  = cfg.sg_speed;
}

void mpx_robot_set_config(const mpx_robot_config_t *in)
{
    if (in == nullptr) {
        return;
    }
    robot::Config cfg;
    cfg.period    = in->period;
    cfg.height    = in->height;
    cfg.up_height = in->up_height;
    cfg.stride    = in->stride;
    cfg.tilt      = in->tilt;
    cfg.sg_speed  = in->sg_speed;
    robot::set_config(cfg);
}

/* ── Calibration ───────────────────────────────────────────────────────── */

float mpx_robot_get_offset(int servo_id)          { return robot::get_offset(servo_id); }
void  mpx_robot_set_offset(int servo_id, float d) { robot::set_offset(servo_id, d); }
void  mpx_robot_reset_offsets(void)               { robot::reset_offsets(); }

/* ── Telemetry ─────────────────────────────────────────────────────────── */

void mpx_robot_imu_read(mpx_robot_imu_t *out)
{
    if (out == nullptr) {
        return;
    }
    const robot::ImuData d = robot::imu_read();
    out->ax = d.ax; out->ay = d.ay; out->az = d.az;
    out->gx = d.gx; out->gy = d.gy; out->gz = d.gz;
    out->mx = d.mx; out->my = d.my; out->mz = d.mz;
    out->mag_valid = d.mag_valid;
}

bool mpx_robot_mag_ready(void) { return robot::imu_mag_ready(); }

int   mpx_robot_read_angle_cdeg(int s)   { return robot::read_angle_cdeg(s); }
int   mpx_robot_read_position(int s)     { return robot::read_position(s); }
int   mpx_robot_read_current(int s)      { return robot::read_current(s); }
float mpx_robot_read_temperature_c(int s){ return robot::read_temperature_c(s); }
int   mpx_robot_ping_servo(int s)        { return robot::ping_servo(s); }

/* ── Servo Studio ──────────────────────────────────────────────────────── */

void mpx_robot_set_studio_mode(bool on) { robot::set_studio_mode(on); }
bool mpx_robot_studio_mode(void)        { return robot::studio_mode(); }

/* ── Low-level joint control ───────────────────────────────────────────── */

void  mpx_robot_set_servo_angle(int s, float deg) { robot::set_servo_angle(s, deg); }
void  mpx_robot_flush(void)                       { robot::flush(); }
void  mpx_robot_set_overlay(int s, float deg)     { robot::set_overlay(s, deg); }
float mpx_robot_get_overlay(int s)                { return robot::get_overlay(s); }
void  mpx_robot_clear_overlay(void)               { robot::clear_overlay(); }

}  // extern "C"
