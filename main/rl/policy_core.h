/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * The policy's arithmetic, with nothing ESP-specific in it, so the same file
 * is compiled on a PC and checked against omniRL's numpy runner
 * (mp3_walk_policy/contract.py and runner.py). Keep the two in step: every
 * function names the Python it mirrors.
 *
 * The .mpxp file (tools/policy/export_mpxp.py writes it, little-endian):
 *   mpxp_header_t (PC side: HEADER_SIZE), then for each layer W[out][in] and
 *   b[out] as float32, then n_fixtures observations and n_fixtures actions.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MPXP_MAGIC        0x5058504Du   /* "MPXP" */
#define MPXP_VERSION      1
#define MPXP_HEADER_SIZE  640
#define MPXP_MAX_LAYERS   8
#define MPXP_MAX_CFG      8
#define MPXP_ACT_ELU      1
#define MPXP_FLAG_OMNI    1u
#define MPXP_FLAG_GETUP   2u   /* a get-up policy (omniRL getup.py): runs after a fall, not a walk */
#define MPXP_FLAG_GETUP_PHASE 4u   /* ... that tracks a motion (getup_track.py): its command inputs carry the
                                     phase phi, sin 2 pi phi, cos 2 pi phi; cmd_max[0] is the motion's length (s),
                                     cmd_max[1] the phase at which it is upright */

#define POL_OBS  45
#define POL_ACT  12
#define POL_N    12   /* servos, id 1..12 at index 0..11 */

typedef struct {
    uint32_t magic, version, total_size, crc32;
    char     name[48];
    char     robot[16];
    uint32_t n_layers;
    uint32_t dims[MPXP_MAX_LAYERS + 1];
    uint32_t activation, flags, n_fixtures, reserved0;
    float    ctrl_dt, state_dt, step_limit_deg, tilt_limit_deg;   /* degrees: the C side
                                                                    converts as np.radians */
    float    tilt_tau_s, temp_stop_c, temp_start_c, cap_ma;
    float    cmd_max[3];
    float    imu_R[9];             /* sensor -> body, row-major */
    float    signs[POL_N];
    float    scale[POL_N];
    float    band_deg[POL_N];
    float    offsets_deg[POL_N];   /* the contract's calibration */
    int32_t  stand_dd[POL_N];
    uint32_t n_cfg;
    struct { uint32_t param_id; float value; } cfg[MPXP_MAX_CFG];
} mpxp_header_t;

/* A parsed policy: pointers into the file's bytes (which must stay alive).
 * A layer with qbits[l] = 8 or 16 runs from Wq[l] (int8_t or int16_t,
 * row-major) and sq[l] (one float scale per output row) instead of W[l]:
 * pol_quantize_layer(). qbits[l] = 0: the file's float weights. */
typedef struct {
    const mpxp_header_t *h;
    const float *W[MPXP_MAX_LAYERS];
    const float *b[MPXP_MAX_LAYERS];
    const void  *Wq[MPXP_MAX_LAYERS];
    const float *sq[MPXP_MAX_LAYERS];
    uint8_t      qbits[MPXP_MAX_LAYERS];
    const float *fix_obs, *fix_act;
    int max_width;
} pol_net_t;

/** Check and index a file. Returns NULL, or a reason it was refused. */
const char *pol_parse(const void *data, size_t len, pol_net_t *out);

uint32_t pol_crc32(const uint8_t *p, size_t n);

/** The network (export.py NumpyPolicy.__call__). `work` needs 2 * max_width floats. */
void pol_forward(const pol_net_t *net, const float obs[POL_OBS], float act[POL_ACT], float *work);

/** Largest |forward(fixture_obs) - fixture_act| over the file's fixtures. */
float pol_selftest(const pol_net_t *net, float *work);

/** The same check in joint degrees (|error| x the action scale): the largest,
 *  and the mean in *mean_deg (may be NULL). */
float pol_selftest_deg(const pol_net_t *net, float *work, float *mean_deg);

/* ── 8- and 16-bit weights ───────────────────────────────────────────────
 * Each output row j of a layer: w[j][i] ~= s[j] * q[j][i], q an integer in
 * [-M, M] (M = 127 or 32767), s[j] = max_i |w[j][i]| / M (a row of zeros:
 * s 0). The activations stay float: y[j] = s[j] * sum_i q[j][i] * x[i] + b[j].
 * Half (16-bit) or a quarter (8-bit) of the bytes to read per step, at a
 * rounding of the weights pol_selftest_deg() measures against training. */

/** Bytes of one quantized layer's block: q (in*o integers, padded to 4), s[o], b[o]. */
size_t pol_quant_layer_size(int in, int o, int bits);

/** Fill `blk` (pol_quant_layer_size bytes, 4-byte aligned) from float
 *  weights W[o][in] and biases b[o]. bits: 8 or 16. */
void pol_quantize_layer(const float *W, const float *b, int in, int o, int bits, void *blk);

/** Point layer l of `net` at a quantized block. */
void pol_use_quant_layer(pol_net_t *net, uint32_t l, int bits, const void *blk);

/* ── the contract (contract.py Contract) ─────────────────────────────── */

/* A measured AT32 angle (dd) as a policy position: rel_from_dd(). */
double pol_rel_from_dd(double dd, double sign, double offset_deg);

/* The dd the firmware path would send for a policy target: quantise_rel()'s
 * dd (float32 arithmetic and C truncation, exactly as contract.py). */
int pol_dd_from_rel(double rel, double sign, double offset_deg);

/* ── ActionDecoder ───────────────────────────────────────────────────── */
typedef struct {
    double prev_rel[POL_ACT];
    double prev_action[POL_ACT];
} pol_decoder_t;

void pol_decoder_reset(pol_decoder_t *d);

/** raw action -> dd targets; updates prev_rel / prev_action (ActionDecoder.__call__). */
void pol_decode(pol_decoder_t *d, const mpxp_header_t *h, const double offsets_deg[POL_N],
                const float raw[POL_ACT], int dd_out[POL_ACT]);

/* ── VelocityFilter ──────────────────────────────────────────────────── */
typedef struct {
    double hist[6][POL_N];
    bool   primed;
} pol_velfilt_t;

void pol_vel_reset(pol_velfilt_t *f);
void pol_vel_push(pol_velfilt_t *f, const double x[POL_N]);
void pol_vel_get(const pol_velfilt_t *f, double v[POL_N]);

/* ── the IMU (ObservationBuilder.gyro_body / gravity_body, TiltFilter) ── */
void pol_gyro_body(const mpxp_header_t *h, const double gyro_rad_s_sensor[3],
                   const double bias_sensor[3], double out[3]);
/* false if |acc| is 0 */
bool pol_gravity_body(const mpxp_header_t *h, const double acc_sensor[3], double out[3]);

/** The body's tilt from upright (runner.tilt_deg); NAN for a zero vector. */
double pol_tilt_deg(const double acc_sensor[3]);

typedef struct {
    double acc[3];
    bool   primed;
    double k;
} pol_tilt_t;

void   pol_tilt_init(pol_tilt_t *t, double tau_s, double dt);
double pol_tilt_push(pol_tilt_t *t, const double acc_sensor[3]);

/** assemble_observation(): command is (vx, vy, wz); a forward-only policy gets (vx, 0, 0). */
void pol_observe(const double gyro_body[3], const double gravity_body[3], const double cmd[3],
                 const double pos_rel[POL_N], const double vel[POL_N], const double prev_action[POL_ACT],
                 float obs[POL_OBS]);

/** mp3link cli.smoothstep */
double pol_smoothstep(double x);

#ifdef __cplusplus
}
#endif
