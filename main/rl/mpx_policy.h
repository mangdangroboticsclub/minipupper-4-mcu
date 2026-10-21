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
 * The policy is a .mpxp file (tools/policy/export_mpxp.py) built into the
 * firmware (rl/policies/default.mpxp). The app drives it through /v1/policy
 * (network/http_policy.cc, the RL Walk page). Ported from mpv2-uros.
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
    MPX_POLICY_GETUP,      /* the get-up policy drives the joints (after a fall, or the app's Get up) */
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
    /* The walk network's weights as it runs them (APP_POLICY_WEIGHT_BITS,
     * mpx_policy_set_weight_bits()). */
    uint8_t  weight_bits;       /* 32 (the file's floats), 16 or 8 */
    uint32_t weights_bytes;     /* the weights, biases and scales */
    uint32_t weights_internal;  /* ... of which in internal RAM */
    float    weights_worst_deg; /* the self-test against training: largest joint error, deg */
    float    weights_mean_deg;  /* ... and the mean */
    /* Internal RAM (Wi-Fi, the tasks, DMA): now, the lowest since boot, the largest block. */
    uint32_t ram_free, ram_min_free, ram_largest;
} mpx_policy_status_t;

/** Load the built-in policy and start the policy task (core 1). */
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
 * leave the robot walking. 0 turns it off. Set it
 * before mpx_policy_start(); it holds until changed.
 */
void mpx_policy_set_deadman_ms(uint32_t ms);
void mpx_policy_keepalive(void);

/** The self-test against training's outputs and the time per
 * step on the policy's core. False with no policy or while a run is on. */
bool mpx_policy_selftest(float *max_err, float *us_avg, float *us_max);
/** The same check as at load: `ok` judged for the weights as they run (32-bit:
 * the raw outputs within 1e-4; 16-/8-bit: the joint targets within 0.5 / 5 deg
 * of training's, `worst_deg`), and the weight bits. */
bool mpx_policy_selftest_full(float *max_err, float *worst_deg, int *bits, bool *ok, float *us_avg, float *us_max);

/*
 * The size test: how big a network this robot can run. Networks of the walk's
 * shape (45, h, h, h/2, 12) for growing h, random weights, are placed in
 * memory as a real policy would be (layers, biggest first, in internal RAM
 * while APP_POLICY_INTERNAL_RESERVE_KB stays free, the rest in PSRAM; the
 * loaded walk's own internal copies are released for the test and restored
 * after) and timed on the policy's core, with the data cache flushed before
 * each evaluation as a real run's other tasks would (a 64 KB buffer, taken
 * after the network so it never decides whether one fits). Nothing moves.
 */
#define MPX_POLICY_BENCH_MAX 9
typedef struct {
    uint16_t hidden;            /* h */
    uint32_t params;            /* weights and biases */
    uint32_t bytes;             /* params * 4 */
    uint32_t bytes_internal;    /* of which in internal RAM */
    float    us_avg, us_max;    /* one network evaluation */
    uint32_t fail_bytes;        /* not ok: the block (one layer) that found no room */
    bool     ok;                /* false: out of memory (the sweep stops there) */
    bool     flushed;           /* the cache was flushed before each evaluation */
} mpx_policy_bench_row_t;

typedef struct {
    bool     running;
    bool     done;
    int      n_rows;            /* finished so far */
    int      n_total;
    uint32_t internal_free;     /* bytes, with the walk's internal copies released */
    uint32_t internal_reserve;  /* bytes kept free (APP_POLICY_INTERNAL_RESERVE_KB) */
    uint32_t internal_largest;  /* the largest single free block */
    uint32_t psram_free;
    uint32_t psram_largest;
    uint8_t  bits;              /* the weights tested: 32, 16 or 8 */
    char     error[48];
    mpx_policy_bench_row_t rows[MPX_POLICY_BENCH_MAX];
} mpx_policy_bench_t;

/** Start the size test with `bits` weights (32, 16 or 8; a few seconds, in the
 *  background). Refused while a run is on. */
esp_err_t mpx_policy_bench_start(int bits, char *why, int why_len);

/**
 * Run the walk network with 32-bit (the file's floats), 16- or 8-bit weights
 * from now on (until the next boot: APP_POLICY_WEIGHT_BITS is the default).
 * The built-in policy is placed again and self-tested against training; on a
 * failure (memory, too much rounding) the current weights stay. Idle only.
 */
esp_err_t mpx_policy_set_weight_bits(int bits, char *why, int why_len);

/**
 * The walk policies built into the firmware (main/rl/policies/: "walk" is
 * default.mpxp; "terrain" terrain.mpxp, "sku1" sku1.mpxp and "sku1_terrain"
 * sku1_terrain.mpxp when they are there). Select one (idle only; until the next boot, which starts with the
 * first one trained for APP_POLICY_ROBOT); it is self-tested first and the
 * current one stays on a failure.
 */
int mpx_policy_builtin_count(void);
const char *mpx_policy_builtin_id(int i);
const char *mpx_policy_builtin_current(void);
esp_err_t mpx_policy_select(const char *id, char *why, int why_len);
void mpx_policy_bench_get(mpx_policy_bench_t *out);

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

/**
 * After every logged run the robot prints its summary on the serial port and
 * saves the log as a CSV file on its flash (it outlives a reboot, and a run
 * without Wi-Fi). The file's path when there is one, else NULL; saving: the
 * file is being written now (a run cannot start meanwhile).
 */
const char *mpx_policy_log_file(void);
bool mpx_policy_log_saving(void);

bool mpx_policy_active(void);
void mpx_policy_get_status(mpx_policy_status_t *out);
const char *mpx_policy_state_name(mpx_policy_state_t s);

#ifdef __cplusplus
}
#endif
