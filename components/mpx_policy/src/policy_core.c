/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * See policy_core.h. Plain C, no ESP headers: tools/policy/host_test builds
 * this file on a PC and compares it with omniRL's numpy.
 */
#include "policy_core.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

_Static_assert(offsetof(mpxp_header_t, name) == 16, "header layout");
_Static_assert(offsetof(mpxp_header_t, n_layers) == 80, "header layout");
_Static_assert(offsetof(mpxp_header_t, activation) == 120, "header layout");
_Static_assert(offsetof(mpxp_header_t, ctrl_dt) == 136, "header layout");
_Static_assert(offsetof(mpxp_header_t, cmd_max) == 168, "header layout");
_Static_assert(offsetof(mpxp_header_t, imu_R) == 180, "header layout");
_Static_assert(offsetof(mpxp_header_t, signs) == 216, "header layout");
_Static_assert(offsetof(mpxp_header_t, stand_dd) == 408, "header layout");
_Static_assert(offsetof(mpxp_header_t, cfg) == 460, "header layout");
_Static_assert(sizeof(mpxp_header_t) <= MPXP_HEADER_SIZE, "header layout");

#define DEG2RAD (M_PI / 180.0)   /* np.radians */
#define RAD2DEG (180.0 / M_PI)   /* np.degrees */
#define DD0 (3100.0 - 511.0 * 3100.0 / 1024.0)

/* ── the file ────────────────────────────────────────────────────────── */

uint32_t pol_crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;   /* zlib.crc32 */
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return ~c;
}

const char *pol_parse(const void *data, size_t len, pol_net_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!data || len < MPXP_HEADER_SIZE) return "too short for a policy file";
    if (((uintptr_t)data & 3u) != 0) return "not 4-byte aligned";
    const mpxp_header_t *h = (const mpxp_header_t *)data;
    if (h->magic != MPXP_MAGIC) return "not a policy file (no MPXP magic)";
    if (h->version != MPXP_VERSION) return "unsupported policy file version";
    if (h->total_size != len) return "size does not match the header (truncated?)";
    if (pol_crc32((const uint8_t *)data + 16, len - 16) != h->crc32) return "checksum mismatch";
    if (h->n_layers < 1 || h->n_layers > MPXP_MAX_LAYERS) return "bad layer count";
    if (h->dims[0] != POL_OBS || h->dims[h->n_layers] != POL_ACT) return "network is not 45 -> 12";
    if (h->activation != MPXP_ACT_ELU) return "unsupported activation";
    if (h->n_cfg > MPXP_MAX_CFG) return "bad servo configuration";
    if (!(h->ctrl_dt > 0.0f) || !(h->state_dt > 0.0f)) return "bad control period";

    const float *p = (const float *)((const uint8_t *)data + MPXP_HEADER_SIZE);
    size_t floats = 0;
    int width = 0;
    for (uint32_t l = 0; l < h->n_layers; ++l) {
        const uint32_t in = h->dims[l], o = h->dims[l + 1];
        if (in == 0 || o == 0 || in > 4096 || o > 4096) return "bad layer size";
        out->W[l] = p + floats;
        floats += (size_t)in * o;
        out->b[l] = p + floats;
        floats += o;
        if ((int)o > width) width = (int)o;
        if ((int)in > width) width = (int)in;
    }
    out->fix_obs = p + floats;
    floats += (size_t)h->n_fixtures * POL_OBS;
    out->fix_act = p + floats;
    floats += (size_t)h->n_fixtures * POL_ACT;
    if (MPXP_HEADER_SIZE + floats * sizeof(float) != len) return "layer sizes do not add up to the file";
    out->h = h;
    out->max_width = width;
    return NULL;
}

/* ── the network ─────────────────────────────────────────────────────── */

void pol_forward(const pol_net_t *net, const float obs[POL_OBS], float act[POL_ACT], float *work)
{
    const mpxp_header_t *h = net->h;
    float *x = work, *y = work + net->max_width;
    memcpy(x, obs, sizeof(float) * POL_OBS);
    for (uint32_t l = 0; l < h->n_layers; ++l) {
        const int in = (int)h->dims[l], o = (int)h->dims[l + 1];
        const float *W = net->W[l], *b = net->b[l];
        for (int j = 0; j < o; ++j) {
            const float *w = W + (size_t)j * in;
            /* Four independent sums: the S3's FPU pipelines them, where one
             * running sum waits for each add (minipupper_rl's dense()). */
            float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
            int i = 0;
            for (; i + 4 <= in; i += 4) {
                a0 += w[i] * x[i];
                a1 += w[i + 1] * x[i + 1];
                a2 += w[i + 2] * x[i + 2];
                a3 += w[i + 3] * x[i + 3];
            }
            for (; i < in; ++i) a0 += w[i] * x[i];
            float s = (a0 + a1) + (a2 + a3);
            s += b[j];
            if (l + 1 < h->n_layers) s = s > 0.0f ? s : expm1f(s);   /* ELU */
            y[j] = s;
        }
        float *t = x; x = y; y = t;
    }
    memcpy(act, x, sizeof(float) * POL_ACT);
}

float pol_selftest(const pol_net_t *net, float *work)
{
    float worst = 0.0f, act[POL_ACT];
    for (uint32_t k = 0; k < net->h->n_fixtures; ++k) {
        pol_forward(net, net->fix_obs + (size_t)k * POL_OBS, act, work);
        for (int j = 0; j < POL_ACT; ++j) {
            const float e = fabsf(act[j] - net->fix_act[(size_t)k * POL_ACT + j]);
            if (!(e <= worst)) worst = e;   /* a NaN wins */
        }
    }
    return worst;
}

/* ── the contract ────────────────────────────────────────────────────── */

double pol_rel_from_dd(double dd, double sign, double offset_deg)
{
    const double servo_deg = (DD0 - dd) / 10.0 - offset_deg;   /* servo_deg_from_dd */
    return sign * (servo_deg * DEG2RAD);                        /* rel_from_servo_deg */
}

int pol_dd_from_rel(double rel, double sign, double offset_deg)
{
    const double servo_deg = rel * RAD2DEG * sign;              /* servo_deg_from_rel */
    /* ticks_from_servo_deg: float32 throughout, as numpy does it */
    volatile float ang = (float)servo_deg + (float)offset_deg;
    volatile float q = ang * 10240.0f;
    q = q / 3100.0f;
    long t = 511 + (long)truncf(q);
    if (t < 0) t = 0;
    if (t > 1023) t = 1023;
    /* dd_from_ticks */
    volatile float tf = (float)t;
    volatile float r = tf * 3100.0f;
    r = r / 1024.0f;
    r = 3100.0f - r;
    return (int)truncf(r);
}

void pol_decoder_reset(pol_decoder_t *d)
{
    memset(d, 0, sizeof(*d));
}

static double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);   /* np.clip */
}

void pol_decode(pol_decoder_t *d, const mpxp_header_t *h, const double offsets_deg[POL_N],
                const float raw[POL_ACT], int dd_out[POL_ACT])
{
    const double limit = (double)h->step_limit_deg * DEG2RAD;
    for (int i = 0; i < POL_ACT; ++i) {
        const double scale = (double)h->scale[i];
        const double band = (double)h->band_deg[i] * DEG2RAD;
        const double a = clampd((double)raw[i], -1.0, 1.0);
        double rel = clampd(scale * a, -band, band);
        rel = clampd(rel, d->prev_rel[i] - limit, d->prev_rel[i] + limit);
        const int dd = pol_dd_from_rel(rel, (double)h->signs[i], offsets_deg[i]);
        rel = pol_rel_from_dd((double)dd, (double)h->signs[i], offsets_deg[i]);
        dd_out[i] = dd;
        d->prev_rel[i] = rel;
        d->prev_action[i] = rel / scale;
    }
}

/* ── VelocityFilter ──────────────────────────────────────────────────── */

void pol_vel_reset(pol_velfilt_t *f)
{
    memset(f, 0, sizeof(*f));
}

void pol_vel_push(pol_velfilt_t *f, const double x[POL_N])
{
    if (!f->primed) {
        for (int k = 0; k < 6; ++k) memcpy(f->hist[k], x, sizeof(double) * POL_N);
        f->primed = true;
        return;
    }
    memmove(f->hist[0], f->hist[1], sizeof(double) * POL_N * 5);
    memcpy(f->hist[5], x, sizeof(double) * POL_N);
}

static double med3(double a, double b, double c)
{
    if (a > b) { double t = a; a = b; b = t; }
    if (b > c) { b = c; }
    return a > b ? a : b;
}

void pol_vel_get(const pol_velfilt_t *f, double v[POL_N])
{
    static const double c[4] = { -1.5, -0.5, 0.5, 1.5 };
    const double den = 5.0 * 0.005;   /* 5.0 * STATE_DT */
    if (!f->primed) {
        memset(v, 0, sizeof(double) * POL_N);
        return;
    }
    for (int j = 0; j < POL_N; ++j) {
        double s = 0.0;
        for (int k = 0; k < 4; ++k) {
            s += (c[k] / den) * med3(f->hist[k][j], f->hist[k + 1][j], f->hist[k + 2][j]);
        }
        v[j] = s;
    }
}

/* ── the IMU ─────────────────────────────────────────────────────────── */

static void rot(const mpxp_header_t *h, const double in[3], double out[3])
{
    for (int r = 0; r < 3; ++r) {
        out[r] = (double)h->imu_R[3 * r + 0] * in[0] + (double)h->imu_R[3 * r + 1] * in[1] +
                 (double)h->imu_R[3 * r + 2] * in[2];
    }
}

void pol_gyro_body(const mpxp_header_t *h, const double gyro_rad_s_sensor[3],
                   const double bias_sensor[3], double out[3])
{
    const double g[3] = { gyro_rad_s_sensor[0] - bias_sensor[0], gyro_rad_s_sensor[1] - bias_sensor[1],
                          gyro_rad_s_sensor[2] - bias_sensor[2] };
    rot(h, g, out);
}

bool pol_gravity_body(const mpxp_header_t *h, const double acc_sensor[3], double out[3])
{
    double a[3];
    rot(h, acc_sensor, a);
    const double n = sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (!(n > 0.0)) {
        out[0] = out[1] = out[2] = 0.0;
        return false;
    }
    for (int i = 0; i < 3; ++i) out[i] = -a[i] / n;
    return true;
}

double pol_tilt_deg(const double a[3])
{
    const double n = sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (!(n > 0.0)) return NAN;
    return acos(clampd(-a[2] / n, -1.0, 1.0)) * RAD2DEG;
}

void pol_tilt_init(pol_tilt_t *t, double tau_s, double dt)
{
    memset(t, 0, sizeof(*t));
    t->k = dt / tau_s;
}

double pol_tilt_push(pol_tilt_t *t, const double a[3])
{
    if (!t->primed) {
        memcpy(t->acc, a, sizeof(t->acc));
        t->primed = true;
    } else {
        for (int i = 0; i < 3; ++i) t->acc[i] = t->acc[i] + t->k * (a[i] - t->acc[i]);
    }
    return pol_tilt_deg(t->acc);
}

void pol_observe(const double gyro_body[3], const double gravity_body[3], const double cmd[3],
                 const double pos_rel[POL_N], const double vel[POL_N], const double prev_action[POL_ACT],
                 float obs[POL_OBS])
{
    for (int i = 0; i < 3; ++i) {
        obs[i] = (float)gyro_body[i];
        obs[3 + i] = (float)gravity_body[i];
        obs[6 + i] = (float)cmd[i];
    }
    for (int i = 0; i < POL_N; ++i) {
        obs[9 + i] = (float)pos_rel[i];
        obs[21 + i] = (float)vel[i];
        obs[33 + i] = (float)prev_action[i];
    }
}

double pol_smoothstep(double x)
{
    x = clampd(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}
