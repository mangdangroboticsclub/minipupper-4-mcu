/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * mpx_policy: an RL walk policy running on the ESP32-S3 itself.
 *
 * What the Pi runner (omniRL mp3_walk_policy/runner.py) did over the J-PI
 * link, here, on the robot: the same observation, network and action
 * decoding (src/policy_core.c, checked against the Python), driven by a 200 Hz
 * tick with a policy step every 4th tick (50 Hz), and the same sequence:
 * servo configuration -> gyro bias (1 s) -> ramp to the stand (2 s) -> hold
 * -> walk -> ramp to the stand (1 s) -> hold -> limp -> the robot's own gait
 * takes over again. No Wi-Fi in the loop.
 *
 * The policy is a .mpxp file (tools/policy/export_mpxp.py): one is built in,
 * others load from /fatfs/policies/<name>.mpxp.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MPX_POLICY_IDLE = 0,
    MPX_POLICY_PREP,       /* taking the robot, servo configuration, gyro bias */
    MPX_POLICY_RAMP_IN,    /* to the stand */
    MPX_POLICY_HOLD_IN,
    MPX_POLICY_RUN,        /* the policy drives the joints */
    MPX_POLICY_RAMP_OUT,
    MPX_POLICY_HOLD_OUT,
    MPX_POLICY_GETUP,      /* the get-up policy drives the joints (after a fall, or `policy getup`) */
} mpx_policy_state_t;

typedef struct {
    mpx_policy_state_t state;
    char     name[48];          /* the loaded policy */
    char     robot[16];         /* the robot its contract describes */
    bool     omni;
    bool     robot_offsets;     /* calibration: the robot's own (true) or the contract's */
    bool     dry;               /* the current (or last) run touches no servo */
    float    cmd[3];            /* vx, vy (m/s), wz (rad/s), after clamping */
    uint32_t steps;             /* policy steps in this (or the last) run */
    uint32_t late_steps;
    uint32_t forward_us_max;    /* slowest network evaluation */
    uint32_t exchange_us_max;   /* slowest servo exchange (all four boards) */
    uint32_t tick_us_max;       /* slowest tick, start to end: over 5000 the next one is late */
    float    tilt_deg;
    float    temp_max_c;
    float    gyro_bias[3];      /* rad/s, sensor frame */
    char     last_stop[64];     /* why the last run ended */
    /* The get-up policy (policies/getup.mpxp, built in when present). */
    bool     getup_ready;       /* loaded and self-tested */
    char     getup_name[48];
    bool     auto_getup;        /* runs started now get up after a fall instead of going limp */
    uint32_t getups;            /* get-ups in this (or the last) run */
    uint32_t getups_ok;         /* ... that reached the stand */
} mpx_policy_status_t;

/** Load the built-in policy (or the one saved with `policy load`). */
esp_err_t mpx_policy_init(void);

/**
 * Start a run: `seconds` of policy (0 = until stopped), after holding the
 * stand `hold_s` seconds. Refused (with a reason in `why`) if a run is on, the
 * robot is held by ROS, the policy is for another robot (unless `force`), ...
 *
 * `dry` (runner --dry): no servo is touched. The gyro bias, the IMU, the
 * network and the timing are real; the joints are taken to be at the stand.
 * For checking a robot (or a bare board) before anything moves.
 */
esp_err_t mpx_policy_start(float seconds, float hold_s, bool force, bool dry, char *why, int why_len);

/**
 * "Never stays down": with `on`, a walk started from now on that falls (tilt
 * over the walk policy's limit) runs the get-up policy instead of going limp,
 * and walks on once it stands (at most MPX_POLICY_MAX_GETUPS times a run). Off
 * by default and without a get-up policy; false if none is loaded.
 */
#define MPX_POLICY_MAX_GETUPS 3
bool mpx_policy_set_auto_getup(bool on);

/**
 * Run the get-up policy alone: from wherever the robot lies (on its back, a
 * side, ...) to the stand, then the robot is given back standing. Refused like
 * mpx_policy_start(), and without a get-up policy.
 */
esp_err_t mpx_policy_getup(bool force, char *why, int why_len);

/** A graceful stop: ramp to the stand, hold, limp, give the robot back. */
void mpx_policy_stop(const char *reason);

/** The command the policy follows (clamped to the policy's training range). */
void mpx_policy_set_cmd(float vx, float vy, float wz);

/**
 * A dead-man for remote control (the web app): with `ms` > 0, a run that hears
 * no mpx_policy_keepalive() for that long stops gracefully (ramp to the
 * stand, hold, limp), so a locked phone, a closed tab or lost Wi-Fi cannot
 * leave the robot walking. 0 (the console's choice) turns it off. Set it
 * before mpx_policy_start(); it holds until changed.
 */
void mpx_policy_set_deadman_ms(uint32_t ms);
void mpx_policy_keepalive(void);

/** `policy test`: the self-test against training's outputs and the time per
 * step on the policy's core. False with no policy or while a run is on. */
bool mpx_policy_selftest(float *max_err, float *us_avg, float *us_max);

/** The loaded policy's command limits: vx, vy (m/s), wz (rad/s). */
bool mpx_policy_cmd_max(float out[3]);

/** Which calibration the next run uses: the robot's own (true) or the one
 * saved in the policy file (false). The default after loading a policy: the
 * robot's own for a policy trained for this robot, the policy's otherwise.
 * Refused (false) while a run is on. */
bool mpx_policy_set_own_calibration(bool own);

/**
 * A per-step log of the next run (every 20 ms step, all phases but PREP):
 * timing, servo exchange, network time, tracking error, tilt, temperature,
 * current, command. Up to MPX_POLICY_LOG_MAX steps (the rest is dropped and
 * counted). Read it after the run with mpx_policy_log_count()/_row().
 */
#define MPX_POLICY_LOG_MAX 6000
void mpx_policy_set_log(bool on);
size_t mpx_policy_log_count(size_t *dropped);
const char *mpx_policy_log_header(void);
/** Row i as one CSV line (no newline). False past the end or while a run is on. */
bool mpx_policy_log_row(size_t i, char *buf, size_t len);

bool mpx_policy_active(void);
void mpx_policy_get_status(mpx_policy_status_t *out);
const char *mpx_policy_state_name(mpx_policy_state_t s);

void mpx_policy_register_console(void);

#ifdef __cplusplus
}
#endif
