/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * The policy on the robot: omniRL's runner.py (Runner, ObservationBuilder,
 * the run sequence and its stop rules), on a 200 Hz tick here instead of the
 * Pi's STATE stream. The arithmetic is policy_core.c; this file is the timing,
 * the servos and the safety.
 *
 * Servos go through the raw path the Pi's RL link used (robot::host::begin(),
 * driver_board_sync_raw()): the gait is parked, the AT32 boards get absolute
 * targets in their own units (dd) every tick, and robot::host::end() puts the
 * walk's servo gains back and glides the robot into its stand afterwards.
 *
 * Stops, as the runner's:
 *   immediate (limp now): tilt over the policy's limit (low-passed, 0.1 s),
 *     a servo over the current or temperature guard, a board silent;
 *   graceful (ramp to the stand, hold, limp): `policy stop`, the run's time
 *     up, the IMU invalid, a servo at the policy's stop temperature, a
 *     non-finite action, two late steps in a row.
 *
 * The get-up policy (policies/getup.mpxp, omniRL getup.py; built in when the
 * file is there) is a second network with the walk's inputs and outputs and a
 * wider range about the stand. It drives the joints in GETUP: on its own
 * (`policy getup`, mpx_policy_getup(): from wherever the robot lies to the
 * stand, then the robot is given back), or inside a walk with "never stays
 * down" on (mpx_policy_set_auto_getup()): a tilt over the walk's limit starts
 * it instead of going limp, and the walk goes on once the robot stands. GETUP
 * has no tilt stop (lying on the back is where it starts), a higher current
 * guard, and a time limit; the servo, temperature and board guards stay.
 */
#include "mpx_policy.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "policy_core.h"
#include "policy_priv.h"
#include "robot/driver_board.h"
#include "robot/robot.h"

extern "C" {
extern const uint8_t default_mpxp_start[] asm("_binary_default_mpxp_start");
extern const uint8_t default_mpxp_end[] asm("_binary_default_mpxp_end");
#ifdef MPX_POLICY_HAVE_GETUP
extern const uint8_t getup_mpxp_start[] asm("_binary_getup_mpxp_start");
extern const uint8_t getup_mpxp_end[] asm("_binary_getup_mpxp_end");
#endif
}

namespace {

const char *TAG = "policy";

constexpr int64_t TICK_US        = 5000;    // the servo tick (runner STATE_DT)
constexpr int     TICKS_PER_STEP = 4;       // 50 Hz policy steps
constexpr double  DEG            = M_PI / 180.0;
constexpr float   BIAS_S = 1.0f, RAMP_IN_S = 2.0f, RAMP_OUT_S = 1.0f, HOLD_OUT_S = 2.0f;
constexpr int64_t IMU_MAX_AGE_US = 30000;   // a sample older than this is a dropout
constexpr double  ACC_DROPOUT_G  = 0.001;   // runner ACC_DROPOUT_MG
// The firmware guard mp4-link and the Pi's RL link apply (rl_link.cc).
constexpr int     GUARD_CUR_MA   = 500;
constexpr int64_t GUARD_CUR_US   = 500000;
constexpr int     GUARD_TEMP_DC  = 600;
constexpr int64_t GUARD_TEMP_US  = 1000000;
constexpr int     BOARD_LOST_TICKS = 20;   // ramps and holds: as rl_link
constexpr int     BOARD_LOST_TICKS_RUN = 2; // the policy: its observation would be stale
// GETUP: pushing the body over loads the servos for longer than a walk does.
constexpr int     GETUP_CUR_MA   = 1000;
constexpr int64_t GETUP_CUR_US   = 1000000;
constexpr float   GETUP_MAX_S    = 10.0f;   // then it gives up (ramp to the stand, limp)
// "Standing": as omniRL getup.py's at_stand, held 0.5 s.
constexpr double  GETUP_OK_TILT_DEG = 12.0;
constexpr double  GETUP_OK_GYRO     = 0.8;  // rad/s
constexpr double  GETUP_OK_RMS_DEG  = 10.0; // joints from the stand, rms
constexpr int     GETUP_OK_STEPS    = 25;

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// The loaded policy (PSRAM). Replaced only while idle.
uint8_t  *s_file = nullptr;
size_t    s_file_len = 0;
pol_net_t s_net;
float    *s_work = nullptr;
bool      s_builtin = true;
// Layers copied into internal RAM (the file itself sits in PSRAM).
float    *s_int_blk[MPXP_MAX_LAYERS] = {};
size_t    s_int_bytes = 0, s_weight_bytes = 0;
// The get-up policy (PSRAM; loaded once at init when built in).
pol_net_t s_gu_net;
float    *s_gu_work = nullptr;
bool      s_gu_ok = false;
bool      s_auto_getup = false;

// Requests from other tasks (under s_mux).
bool  s_req_start = false, s_req_stop = false, s_req_dry = false, s_req_getup = false;
float s_req_seconds = 0.0f, s_req_hold = 0.0f;
// The last policy step, for `policy obs` (under s_mux).
float s_last_obs[POL_OBS], s_last_raw[POL_ACT];
int   s_last_dd[POL_ACT];
bool  s_last_valid = false;
char  s_req_why[64];
// The per-step log (mpx_policy_set_log). Written by the tick task during a
// run only; read by others only while idle.
struct LogRec {
    uint32_t t_ms;          // since the run began
    uint32_t dt_us;         // since the previous step
    uint16_t tick_us;       // this step's tick, start to end
    uint16_t ex_us;         // its servo exchange
    uint16_t fwd_us;        // the network (RUN only)
    uint8_t  phase;         // mpx_policy_state_t
    uint8_t  flags;         // bit 0: a tick of this step came late; bits 4..7: fresh legs
    int16_t  tilt_cdeg;
    int16_t  temp_dc;
    int16_t  cur_ma;        // the largest |current| this tick
    int16_t  trk_mean_cdeg; // mean |target - measured| over the joints, 0.01 deg
    int16_t  trk_max_cdeg;
    int16_t  vx_mm, vy_mm;  // command, mm/s
    int16_t  wz_mrad;
};
LogRec  *s_log = nullptr;
size_t   s_log_n = 0, s_log_dropped = 0;
bool     s_log_req = false;   // log the next run
bool     s_log_on = false;    // this run is logged

// The remote dead-man (mpx_policy_set_deadman_ms), under s_mux.
int64_t s_deadman_us = 0, s_keepalive_us = 0;
float s_cmd[3];
bool  s_use_robot_offsets = false;

mpx_policy_status_t s_st;   // under s_mux
volatile mpx_policy_state_t s_state = MPX_POLICY_IDLE;

TaskHandle_t       s_task  = nullptr;
esp_timer_handle_t s_timer = nullptr;

// ── run state (tick task only) ──────────────────────────────────────────
struct Run {
    const pol_net_t *net;             // the network driving the joints: s_net (walk) or s_gu_net (GETUP)
    float    *work;
    bool      getup_only;             // `policy getup`: GETUP, then give the robot back
    bool      auto_up;                // never stays down: a fall starts GETUP
    uint32_t  getups;
    int       gu_steps, gu_ok;        // GETUP: steps so far, consecutive steps standing
    float     seconds, hold_s;
    double    off[POL_N];
    int       stand_dd[POL_N];
    int       target[POL_N];          // what each tick sends
    int       target_sent[POL_N];     // what the last exchange sent (the log's tracking error)
    int       from[POL_N];            // a ramp's start
    int       ramp_k, ramp_n;         // policy-rate steps of a ramp or a hold
    int       sub;                    // ticks into the current 50 Hz step
    uint32_t  steps_left;             // RUN: 0 = until stopped
    bool      unlimited;
    bool      armed;
    // PREP
    int       prep_ticks, syncs;
    double    bias_sum[3];
    int       bias_n;
    double    bias[3];
    // feedback
    uint16_t  pos_dd[POL_N];
    bool      pos_ok[POL_N];          // ever had a fresh reading this run
    int64_t   over_cur_us[POL_N], over_temp_us[POL_N];
    uint16_t  lost[4];
    // observation
    pol_velfilt_t vf;
    pol_decoder_t dec;
    pol_tilt_t    tilt;
    double    acc[3], gyr[3];         // latest valid sample, sensor frame (g, rad/s)
    bool      imu_ok;
    int       late_run;
    bool      late_in_step;
    int64_t   t_prev;
    bool      have_host;              // robot::host::begin() succeeded for this run
    bool      dry;                    // no servo is touched (runner --dry)
    bool      dry_tilt_noted;         // dry: "would disarm" said once
    // the log
    int64_t   t0_us, last_step_us;
    uint32_t  log_fwd_us;
    bool      log_late;
};
Run R;

void set_state(mpx_policy_state_t s)
{
    s_state = s;
    portENTER_CRITICAL(&s_mux);
    s_st.state = s;
    portEXIT_CRITICAL(&s_mux);
}

void note_stop(const char *why)
{
    portENTER_CRITICAL(&s_mux);
    std::snprintf(s_st.last_stop, sizeof(s_st.last_stop), "%s", why);
    portEXIT_CRITICAL(&s_mux);
}

// ── servos ──────────────────────────────────────────────────────────────

/* One exchange with the boards: every servo POSITION at its target (armed),
 * or HOLD (keep what they were last told). Then read what came back. */
void exchange(bool armed, uint8_t *answered, uint8_t *fresh)
{
    uint16_t mode[POL_N], pos[POL_N];
    int16_t  cur[POL_N];
    const int16_t cap = static_cast<int16_t>(s_net.h->cap_ma);
    for (int i = 0; i < POL_N; ++i) {
        mode[i] = armed ? DB_MODE_POSITION : DB_MODE_HOLD;
        int t = R.target[i];
        if (t < 0) t = 0;
        if (t > 3100) t = 3100;
        pos[i] = static_cast<uint16_t>(t);
        cur[i] = armed ? cap : 0;
        R.target_sent[i] = t;
    }
    driver_board_sync_raw(mode, pos, cur, answered, fresh);
    // Only a leg whose board sent a FRESH reply this exchange: raw_feedback
    // keeps returning the previous values for one that answered stale.
    for (int i = 0; i < POL_N; ++i) {
        uint16_t p = 0;
        if ((*fresh >> (i / 3) & 1u) && driver_board_raw_feedback(i + 1, &p, nullptr, nullptr, nullptr)) {
            R.pos_dd[i] = p;
            R.pos_ok[i] = true;
        }
    }
}

/* Limp every servo where it is (runner Disarm). */
void limp()
{
    uint16_t mode[POL_N], pos[POL_N];
    int16_t  cur[POL_N] = {};
    for (int i = 0; i < POL_N; ++i) {
        uint16_t p = 0;
        mode[i] = DB_MODE_IDLE;
        pos[i] = driver_board_raw_feedback(i + 1, &p, nullptr, nullptr, nullptr) ? p : 1550;
    }
    driver_board_sync_raw(mode, pos, cur, nullptr, nullptr);
}

/* The configuration the policy was identified with, written and read back
 * (runner.preflight refuses to start without it). Returns false if any servo
 * did not take it. */
bool write_servo_config()
{
    const mpxp_header_t *h = s_net.h;
    int bad = 0;
    for (int id = 1; id <= POL_N; ++id) {
        for (uint32_t k = 0; k < h->n_cfg; ++k) {
            const int pid = static_cast<int>(h->cfg[k].param_id);
            const float want = h->cfg[k].value;
            float got = NAN;
            if (!driver_board_set_param(id, pid, want) || !driver_board_get_param(id, pid, &got) ||
                !(std::fabs(got - want) <= 1e-6f + 1e-3f * std::fabs(want))) {
                if (bad++ == 0) ESP_LOGE(TAG, "servo %d parameter %d: wanted %g, reads %g", id, pid, want, got);
            }
        }
    }
    return bad == 0;
}

/* Back to the robot: stop the tick, put the gains back, glide into the stand. */
void finish(const char *why, bool limp_first)
{
    if (limp_first) limp();
    R.armed = false;
    if (R.have_host) robot::host::end();
    R.have_host = false;
    note_stop(why);
    set_state(MPX_POLICY_IDLE);
    ESP_LOGW(TAG, "run ended: %s", why);
}

// ── stops ───────────────────────────────────────────────────────────────

/* A ramp (to the stand) or a hold, counted in policy steps. */
void begin_ramp(mpx_policy_state_t st, float seconds)
{
    std::memcpy(R.from, R.target, sizeof(R.from));
    R.ramp_k = 0;
    R.ramp_n = std::max(1, static_cast<int>(std::lround(seconds / s_net.h->ctrl_dt)));
    R.sub = 0;
    set_state(st);
}

void graceful_stop(const char *why)
{
    const mpx_policy_state_t s = s_state;
    if (s == MPX_POLICY_RAMP_OUT || s == MPX_POLICY_HOLD_OUT) return;   // already on the way down
    note_stop(why);
    ESP_LOGW(TAG, "stopping: %s", why);
    if (s == MPX_POLICY_PREP || !R.armed || R.dry) {
        finish(why, false);
        return;
    }
    begin_ramp(MPX_POLICY_RAMP_OUT, RAMP_OUT_S);
}

void immediate_stop(const char *why)
{
    ESP_LOGE(TAG, "DISARM: %s", why);
    finish(why, true);
}

// ── the IMU ─────────────────────────────────────────────────────────────

void read_imu(int64_t now)
{
    const robot::ImuData d = robot::imu_read();
    const double a[3] = { d.ax, d.ay, d.az };
    const double n = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    R.imu_ok = d.seq != 0 && now - d.t_us <= IMU_MAX_AGE_US && n > ACC_DROPOUT_G;
    if (R.imu_ok) {
        std::memcpy(R.acc, a, sizeof(R.acc));
        R.gyr[0] = d.gx * DEG;
        R.gyr[1] = d.gy * DEG;
        R.gyr[2] = d.gz * DEG;
    }
}

// ── policy coordinates of what the boards report ────────────────────────
void pos_rel(double rel[POL_N])
{
    const mpxp_header_t *h = s_net.h;
    for (int i = 0; i < POL_N; ++i) rel[i] = pol_rel_from_dd(R.pos_dd[i], h->signs[i], R.off[i]);
}

// ── the phases ──────────────────────────────────────────────────────────

void start_run()
{
    float seconds, hold;
    portENTER_CRITICAL(&s_mux);
    if (!s_req_start) {               // cancelled by `policy stop` in the meantime
        portEXIT_CRITICAL(&s_mux);
        return;
    }
    s_req_start = false;
    seconds = s_req_seconds;
    hold = s_req_hold;
    const bool dry = s_req_dry;
    const bool getup_only = s_req_getup;
    s_req_getup = false;
    s_st.dry = dry;
    s_st.getups = 0;
    s_st.getups_ok = 0;
    s_last_valid = false;
    s_st.steps = 0;
    s_st.late_steps = 0;
    s_st.forward_us_max = 0;
    s_st.exchange_us_max = 0;
    s_st.tick_us_max = 0;
    s_st.last_stop[0] = '\0';
    portEXIT_CRITICAL(&s_mux);

    std::memset(&R, 0, sizeof(R));
    R.net = &s_net;
    R.work = s_work;
    R.getup_only = getup_only && s_gu_ok;
    R.auto_up = s_auto_getup && s_gu_ok && !getup_only;
    R.seconds = seconds;
    R.hold_s = hold;
    R.dry = dry;
    R.t0_us = R.last_step_us = esp_timer_get_time();
    s_log_on = s_log_req;
    s_log_n = s_log_dropped = 0;
    if (s_log_on && !s_log) {
        s_log = static_cast<LogRec *>(heap_caps_calloc(MPX_POLICY_LOG_MAX, sizeof(LogRec), MALLOC_CAP_SPIRAM));
        if (!s_log) {
            s_log_on = false;
            ESP_LOGW(TAG, "no memory for the run log");
        }
    }
    R.unlimited = seconds <= 0.0f;
    R.steps_left = R.unlimited ? 0
                               : static_cast<uint32_t>(std::max(1L, std::lround(seconds / s_net.h->ctrl_dt)));
    const mpxp_header_t *h = s_net.h;
    for (int i = 0; i < POL_N; ++i) {
        R.off[i] = s_use_robot_offsets ? robot::get_offset(i + 1) : h->offsets_deg[i];
        R.stand_dd[i] = s_use_robot_offsets ? pol_dd_from_rel(0.0, h->signs[i], R.off[i]) : h->stand_dd[i];
    }
    pol_tilt_init(&R.tilt, h->tilt_tau_s, static_cast<double>(TICK_US) * 1e-6);
    set_state(MPX_POLICY_PREP);

    if (R.dry) {
        R.t_prev = esp_timer_get_time();
        ESP_LOGW(TAG, "DRY run: %s, no servo is touched; measuring the gyro bias (%.0f s, keep still)",
                 h->name, BIAS_S);
        return;
    }
    // Someone else may have taken the robot since `policy run` was accepted.
    if (robot::master_active() || robot::host::active() || !robot::host::begin()) {
        finish("another controller (ROS, a host link) holds the robot", false);
        return;
    }
    R.have_host = true;
    driver_board_raw_invalidate();
    if (!write_servo_config()) {
        finish("the servos did not take the policy's configuration", false);
        return;
    }
    R.t_prev = esp_timer_get_time();
    ESP_LOGW(TAG, "run: %s, servos configured; measuring the gyro bias (%.0f s, keep still)",
             h->name, BIAS_S);
}

void enter_run()
{
    R.net = &s_net;
    R.work = s_work;
    // runner.loop: decoder and observation reset at the stand.
    pol_decoder_reset(&R.dec);
    pol_vel_reset(&R.vf);
    double rel[POL_N];
    pos_rel(rel);
    pol_vel_push(&R.vf, rel);
    R.late_run = 0;
    R.late_in_step = false;
    R.sub = 0;
    set_state(MPX_POLICY_RUN);
    ESP_LOGW(TAG, "policy on%s", R.dry ? " (dry: servos untouched)" : "");
}

/* The get-up policy takes over, from wherever the joints are (its decoder
 * starts at the stand, as training's does after a reset). */
void enter_getup()
{
    R.net = &s_gu_net;
    R.work = s_gu_work;
    pol_decoder_reset(&R.dec);
    pol_vel_reset(&R.vf);
    double rel[POL_N];
    pos_rel(rel);
    pol_vel_push(&R.vf, rel);
    R.late_run = 0;
    R.late_in_step = false;
    R.sub = 0;
    R.gu_steps = 0;
    R.gu_ok = 0;
    ++R.getups;
    portENTER_CRITICAL(&s_mux);
    s_st.getups = R.getups;
    portEXIT_CRITICAL(&s_mux);
    set_state(MPX_POLICY_GETUP);
    ESP_LOGW(TAG, "getting up (%u)%s", static_cast<unsigned>(R.getups), R.dry ? " (dry: servos untouched)" : "");
}

void tick_prep()
{
    uint8_t answered = 0, fresh = 0;
    if (!R.dry) exchange(false, &answered, &fresh);
    if (++R.syncs < 3) return;   // the first replies can still be configuration echoes
    if (R.imu_ok) {
        for (int k = 0; k < 3; ++k) R.bias_sum[k] += R.gyr[k];
        ++R.bias_n;
    }
    if (++R.prep_ticks < static_cast<int>(BIAS_S * 1e6 / TICK_US)) return;

    const mpxp_header_t *h = s_net.h;
    if (R.dry) {
        if (R.bias_n < static_cast<int>(0.5 * BIAS_S * 1e6 / TICK_US)) {
            finish("too few valid IMU samples for the gyro bias", false);
            return;
        }
        for (int k = 0; k < 3; ++k) R.bias[k] = R.bias_sum[k] / R.bias_n;
        portENTER_CRITICAL(&s_mux);
        for (int k = 0; k < 3; ++k) s_st.gyro_bias[k] = static_cast<float>(R.bias[k]);
        portEXIT_CRITICAL(&s_mux);
        // The joints are taken to be at the stand.
        for (int i = 0; i < POL_N; ++i) {
            R.pos_dd[i] = static_cast<uint16_t>(R.stand_dd[i]);
            R.target[i] = R.stand_dd[i];
        }
        R.t_prev = esp_timer_get_time();
        ESP_LOGW(TAG, "gyro bias %.4f %.4f %.4f rad/s", R.bias[0], R.bias[1], R.bias[2]);
        if (R.getup_only) enter_getup();
        else enter_run();
        return;
    }
    if (answered != 0x0F || fresh != 0x0F) {
        finish("a driver board does not answer", false);
        return;
    }
    for (int i = 0; i < POL_N; ++i) {
        if (!R.pos_ok[i]) {
            finish("a servo reports no position", false);
            return;
        }
        int16_t t = 0;
        driver_board_raw_feedback(i + 1, nullptr, nullptr, &t, nullptr);
        if (t >= static_cast<int16_t>(h->temp_start_c * 10.0f) && t < 1500) {
            char why[64];
            std::snprintf(why, sizeof(why), "servo %d at %.1f degC: wait below %.0f", i + 1, t / 10.0,
                          h->temp_start_c);
            finish(why, false);
            return;
        }
    }
    if (R.bias_n < static_cast<int>(0.5 * BIAS_S * 1e6 / TICK_US)) {
        finish("too few valid IMU samples for the gyro bias", false);
        return;
    }
    for (int k = 0; k < 3; ++k) R.bias[k] = R.bias_sum[k] / R.bias_n;
    portENTER_CRITICAL(&s_mux);
    for (int k = 0; k < 3; ++k) s_st.gyro_bias[k] = static_cast<float>(R.bias[k]);
    portEXIT_CRITICAL(&s_mux);

    // The ramp starts where the legs are.
    for (int i = 0; i < POL_N; ++i) R.target[i] = R.pos_dd[i];
    R.armed = true;
    R.t_prev = esp_timer_get_time();   // the guards count from the first armed tick
    if (R.getup_only) {
        ESP_LOGW(TAG, "gyro bias %.4f %.4f %.4f rad/s; getting up", R.bias[0], R.bias[1], R.bias[2]);
        enter_getup();
        return;
    }
    begin_ramp(MPX_POLICY_RAMP_IN, RAMP_IN_S);
    ESP_LOGW(TAG, "gyro bias %.4f %.4f %.4f rad/s; ramping to the stand", R.bias[0], R.bias[1], R.bias[2]);
}

/* A policy step (runner.loop body). */
void policy_step()
{
    const pol_net_t *net = R.net;
    const mpxp_header_t *h = net->h;
    float cmdf[3];
    portENTER_CRITICAL(&s_mux);
    std::memcpy(cmdf, s_cmd, sizeof(cmdf));
    portEXIT_CRITICAL(&s_mux);
    double cmd[3] = { cmdf[0], cmdf[1], cmdf[2] };
    if (!(h->flags & MPXP_FLAG_OMNI)) cmd[1] = cmd[2] = 0.0;
    if (h->flags & MPXP_FLAG_GETUP) cmd[0] = cmd[1] = cmd[2] = 0.0;   // trained with no command

    double gb[3], grav[3], rel[POL_N], vel[POL_N];
    pol_gyro_body(h, R.gyr, R.bias, gb);
    pol_gravity_body(h, R.acc, grav);
    pos_rel(rel);
    pol_vel_get(&R.vf, vel);
    float obs[POL_OBS], raw[POL_ACT];
    pol_observe(gb, grav, cmd, rel, vel, R.dec.prev_action, obs);
    const int64_t t0 = esp_timer_get_time();
    pol_forward(net, obs, raw, R.work);
    const uint32_t us = static_cast<uint32_t>(esp_timer_get_time() - t0);
    R.log_fwd_us = us;
    for (int i = 0; i < POL_ACT; ++i) {
        if (!std::isfinite(raw[i])) {
            graceful_stop("non-finite action");
            return;
        }
    }
    pol_decode(&R.dec, h, R.off, raw, R.target);
    portENTER_CRITICAL(&s_mux);
    std::memcpy(s_last_obs, obs, sizeof(s_last_obs));
    std::memcpy(s_last_raw, raw, sizeof(s_last_raw));
    std::memcpy(s_last_dd, R.target, sizeof(s_last_dd));
    s_last_valid = true;
    portEXIT_CRITICAL(&s_mux);

    R.late_run = R.late_in_step ? R.late_run + 1 : 0;
    portENTER_CRITICAL(&s_mux);
    s_st.steps++;
    if (R.late_in_step) s_st.late_steps++;
    if (us > s_st.forward_us_max) s_st.forward_us_max = us;
    portEXIT_CRITICAL(&s_mux);
    R.late_in_step = false;
    if (R.late_run >= 2) graceful_stop("two late steps in a row");
}

/* A GETUP step: the get-up network, then is the robot standing (upright,
 * still, the joints near the stand, for GETUP_OK_STEPS steps)? Then back to
 * the walk, or (`policy getup`) to the stand and the end of the run. */
void getup_step(double tilt)
{
    policy_step();
    if (s_state != MPX_POLICY_GETUP) return;
    ++R.gu_steps;
    double g2 = 0.0, r2 = 0.0, rel[POL_N];
    for (int k = 0; k < 3; ++k) g2 += (R.gyr[k] - R.bias[k]) * (R.gyr[k] - R.bias[k]);
    pos_rel(rel);
    for (int i = 0; i < POL_N; ++i) r2 += rel[i] * rel[i];
    const bool ok = std::isfinite(tilt) && tilt < GETUP_OK_TILT_DEG && g2 < GETUP_OK_GYRO * GETUP_OK_GYRO &&
                    std::sqrt(r2 / POL_N) < GETUP_OK_RMS_DEG * DEG;
    R.gu_ok = ok ? R.gu_ok + 1 : 0;
    if (R.gu_ok >= GETUP_OK_STEPS) {
        portENTER_CRITICAL(&s_mux);
        s_st.getups_ok++;
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGW(TAG, "standing again after %.1f s", R.gu_steps * s_gu_net.h->ctrl_dt);
        if (R.getup_only) {
            graceful_stop("stood up");
        } else {
            enter_run();
            ESP_LOGW(TAG, "walking on");
        }
        return;
    }
    if (R.gu_steps * s_gu_net.h->ctrl_dt > GETUP_MAX_S) {
        char why[64];
        std::snprintf(why, sizeof(why), "could not get up in %.0f s", static_cast<double>(GETUP_MAX_S));
        graceful_stop(why);
    }
}

int16_t clamp16(double v)
{
    return static_cast<int16_t>(v > 32767 ? 32767 : v < -32768 ? -32768 : std::lround(v));
}

/* One log record per step (every 4th tick), at the end of the step's tick. */
void log_step(int64_t tick_start, mpx_policy_state_t phase, uint32_t ex_us, uint8_t fresh,
              double tilt, float tmax, int cur_max)
{
    if (!s_log_on) return;
    const int64_t now = esp_timer_get_time();
    if (s_log_n >= MPX_POLICY_LOG_MAX) {
        ++s_log_dropped;
        return;
    }
    LogRec &r = s_log[s_log_n++];
    r.t_ms = static_cast<uint32_t>((now - R.t0_us) / 1000);
    r.dt_us = static_cast<uint32_t>(tick_start - R.last_step_us);
    R.last_step_us = tick_start;
    r.tick_us = static_cast<uint16_t>(std::min<int64_t>(now - tick_start, 65535));
    r.ex_us = static_cast<uint16_t>(std::min<uint32_t>(ex_us, 65535));
    r.fwd_us = static_cast<uint16_t>(phase == MPX_POLICY_RUN || phase == MPX_POLICY_GETUP
                                         ? std::min<uint32_t>(R.log_fwd_us, 65535) : 0);
    r.phase = static_cast<uint8_t>(phase);
    r.flags = static_cast<uint8_t>((R.log_late ? 1u : 0u) | ((fresh & 0x0Fu) << 4));
    R.log_late = false;
    r.tilt_cdeg = std::isfinite(tilt) ? clamp16(tilt * 100.0) : INT16_MIN;
    r.temp_dc = clamp16(tmax * 10.0f);
    r.cur_ma = clamp16(cur_max);
    // How far the servos are from what they were last told (dd: 10 per degree).
    double sum = 0.0, mx = 0.0;
    int n = 0;
    if (!R.dry) {
        for (int i = 0; i < POL_N; ++i) {
            if (!R.pos_ok[i]) continue;
            const double e = std::fabs(static_cast<double>(R.target_sent[i]) - R.pos_dd[i]) / 10.0;
            sum += e;
            mx = std::max(mx, e);
            ++n;
        }
    }
    r.trk_mean_cdeg = clamp16(n ? sum / n * 100.0 : 0.0);
    r.trk_max_cdeg = clamp16(mx * 100.0);
    float c[3];
    portENTER_CRITICAL(&s_mux);
    std::memcpy(c, s_cmd, sizeof(c));
    portEXIT_CRITICAL(&s_mux);
    r.vx_mm = clamp16(c[0] * 1000.0f);
    r.vy_mm = clamp16(c[1] * 1000.0f);
    r.wz_mrad = clamp16(c[2] * 1000.0f);
}

/* A dry tick: the IMU, the tilt (reported, not acted on), the policy on its
 * 50 Hz steps; no servo, so no servo guard. An IMU dropout and late steps
 * still end the run, as they would a real one. */
void tick_dry(int64_t now)
{
    const mpxp_header_t *h = s_net.h;
    const int64_t dt = now - R.t_prev;
    R.t_prev = now;
    const uint32_t missed = dt > TICK_US + TICK_US / 2 ? static_cast<uint32_t>((dt + TICK_US / 2) / TICK_US - 1) : 0;
    double tilt = NAN;
    if (R.imu_ok) {
        double b[3];
        for (int r = 0; r < 3; ++r)
            b[r] = h->imu_R[3 * r] * R.acc[0] + h->imu_R[3 * r + 1] * R.acc[1] + h->imu_R[3 * r + 2] * R.acc[2];
        const double as_sensor[3] = { b[0], b[1], -b[2] };
        tilt = pol_tilt_push(&R.tilt, as_sensor);
    }
    portENTER_CRITICAL(&s_mux);
    s_st.tilt_deg = static_cast<float>(tilt);
    portEXIT_CRITICAL(&s_mux);
    if (std::isfinite(tilt) && tilt > h->tilt_limit_deg && !R.dry_tilt_noted) {
        R.dry_tilt_noted = true;
        ESP_LOGW(TAG, "tilt %.0f deg: a real run would disarm here (dry run goes on)", tilt);
    }
    const mpx_policy_state_t st = s_state;
    if (st != MPX_POLICY_RUN && st != MPX_POLICY_GETUP) return;
    if (!R.imu_ok) {
        graceful_stop("IMU sample invalid");
        return;
    }
    double rel[POL_N];
    pos_rel(rel);
    for (uint32_t k = 0; k < missed && k < 6; ++k) pol_vel_push(&R.vf, rel);
    if (missed) R.late_in_step = true;
    R.log_late |= missed > 0;
    pol_vel_push(&R.vf, rel);
    if (++R.sub < TICKS_PER_STEP) return;
    R.sub = 0;
    if (st == MPX_POLICY_GETUP) {
        getup_step(tilt);
    } else {
        policy_step();
        if (s_state == MPX_POLICY_RUN && !R.unlimited && R.steps_left > 0 && --R.steps_left == 0) {
            graceful_stop("completed");
        }
    }
    log_step(now, st, 0, 0, tilt, 0.0f, 0);
}

/* An armed tick: send, read back, the guards; at a step boundary, the next
 * ramp/hold target or the policy. */
void tick_armed(int64_t now)
{
    if (R.dry) {
        tick_dry(now);
        return;
    }
    const mpxp_header_t *h = s_net.h;
    uint8_t answered = 0, fresh = 0;
    const int64_t tx0 = esp_timer_get_time();
    exchange(true, &answered, &fresh);
    const uint32_t ex_us = static_cast<uint32_t>(esp_timer_get_time() - tx0);
    portENTER_CRITICAL(&s_mux);
    if (ex_us > s_st.exchange_us_max) s_st.exchange_us_max = ex_us;
    portEXIT_CRITICAL(&s_mux);
    const int64_t dt = now - R.t_prev;
    R.t_prev = now;
    // Ticks this one came late by (the timer skips, it does not queue).
    const uint32_t missed = dt > TICK_US + TICK_US / 2 ? static_cast<uint32_t>((dt + TICK_US / 2) / TICK_US - 1) : 0;

    R.log_late |= missed > 0;

    // Guards (rl_link.cc's): current, temperature, a silent board.
    const bool up_now = s_state == MPX_POLICY_GETUP;
    const int cur_ma = up_now ? GETUP_CUR_MA : GUARD_CUR_MA;
    const int64_t cur_us = up_now ? GETUP_CUR_US : GUARD_CUR_US;
    float tmax = 0.0f;
    int cur_max = 0;
    for (int i = 0; i < POL_N; ++i) {
        int16_t c = 0, t = 0;
        const bool valid = driver_board_raw_feedback(i + 1, nullptr, &c, &t, nullptr) && (fresh >> (i / 3) & 1u);
        if (!valid) continue;
        const int ac = c < 0 ? -c : c;
        if (ac > cur_max) cur_max = ac;
        R.over_cur_us[i] = ac > cur_ma ? R.over_cur_us[i] + dt : 0;
        const bool real_t = t < 1500;
        R.over_temp_us[i] = (real_t && t > GUARD_TEMP_DC) ? R.over_temp_us[i] + dt : 0;
        if (real_t && t / 10.0f > tmax) tmax = t / 10.0f;
        char why[64];
        if (R.over_cur_us[i] > cur_us) {
            std::snprintf(why, sizeof(why), "servo %d over %d mA for %lld ms", i + 1, cur_ma,
                          static_cast<long long>(cur_us / 1000));
            immediate_stop(why);
            return;
        }
        if (R.over_temp_us[i] > GUARD_TEMP_US) {
            std::snprintf(why, sizeof(why), "servo %d over %.0f degC", i + 1, GUARD_TEMP_DC / 10.0);
            immediate_stop(why);
            return;
        }
    }
    const int lost_max = s_state == MPX_POLICY_RUN || up_now ? BOARD_LOST_TICKS_RUN : BOARD_LOST_TICKS;
    for (int b = 0; b < 4; ++b) {
        R.lost[b] = (fresh >> b & 1u) ? 0 : R.lost[b] + 1;
        if (R.lost[b] > lost_max) {
            char why[64];
            std::snprintf(why, sizeof(why), "leg %d: no fresh servo reading for %d ticks", b, R.lost[b]);
            immediate_stop(why);
            return;
        }
    }

    // The IMU and the tilt, every tick.
    // runner.tilt_deg takes the sensor frame and relies on the mount
    // flipping z; rotate first, so any mount reads 0 upright.
    double tilt = NAN;
    if (R.imu_ok) {
        double b[3];
        for (int r = 0; r < 3; ++r)
            b[r] = h->imu_R[3 * r] * R.acc[0] + h->imu_R[3 * r + 1] * R.acc[1] + h->imu_R[3 * r + 2] * R.acc[2];
        const double as_sensor[3] = { b[0], b[1], -b[2] };
        tilt = pol_tilt_push(&R.tilt, as_sensor);
    }
    portENTER_CRITICAL(&s_mux);
    s_st.tilt_deg = static_cast<float>(tilt);
    s_st.temp_max_c = tmax;
    portEXIT_CRITICAL(&s_mux);
    if (std::isfinite(tilt) && tilt > h->tilt_limit_deg && !up_now) {
        const mpx_policy_state_t s0 = s_state;
        const bool walking = s0 == MPX_POLICY_RUN || s0 == MPX_POLICY_RAMP_IN || s0 == MPX_POLICY_HOLD_IN;
        if (R.auto_up && walking && R.getups < MPX_POLICY_MAX_GETUPS) {
            ESP_LOGW(TAG, "fell (tilt %.0f deg)", tilt);
            enter_getup();
        } else {
            char why[64];
            if (R.auto_up && walking)
                std::snprintf(why, sizeof(why), "fell %u times", static_cast<unsigned>(R.getups + 1));
            else
                std::snprintf(why, sizeof(why), "tilt %.0f deg", tilt);
            immediate_stop(why);
            return;
        }
    }
    const mpx_policy_state_t st = s_state;
    const bool stopping = st == MPX_POLICY_RAMP_OUT || st == MPX_POLICY_HOLD_OUT;
    if (!R.imu_ok && !stopping) {
        graceful_stop("IMU sample invalid");
        return;
    }
    if (tmax >= h->temp_stop_c && !stopping) {
        char why[64];
        std::snprintf(why, sizeof(why), "servo at %.1f degC", tmax);
        graceful_stop(why);
        return;
    }

    // The velocity filter sees every tick; a missed tick repeats the last
    // sample (runner ObservationBuilder.push).
    if (st == MPX_POLICY_RUN || st == MPX_POLICY_GETUP) {
        double rel[POL_N];
        pos_rel(rel);
        for (uint32_t k = 0; k < missed && k < 6; ++k) {
            double last[POL_N];
            std::memcpy(last, R.vf.hist[5], sizeof(last));
            pol_vel_push(&R.vf, last);
        }
        if (missed) R.late_in_step = true;
        pol_vel_push(&R.vf, rel);
    }

    if (++R.sub < TICKS_PER_STEP) return;
    R.sub = 0;

    switch (st) {
    case MPX_POLICY_RAMP_IN:
    case MPX_POLICY_RAMP_OUT: {
        ++R.ramp_k;
        const double s = pol_smoothstep(static_cast<double>(R.ramp_k) / R.ramp_n);
        for (int i = 0; i < POL_N; ++i)
            R.target[i] = static_cast<int>(std::lround(R.from[i] + (R.stand_dd[i] - R.from[i]) * s));
        if (R.ramp_k >= R.ramp_n) {
            const bool in = st == MPX_POLICY_RAMP_IN;
            begin_ramp(in ? MPX_POLICY_HOLD_IN : MPX_POLICY_HOLD_OUT, in ? R.hold_s : HOLD_OUT_S);
            if (in) ESP_LOGW(TAG, "holding the stand %.1f s", R.hold_s);
        }
        break;
    }
    case MPX_POLICY_HOLD_IN:
    case MPX_POLICY_HOLD_OUT:
        std::memcpy(R.target, R.stand_dd, sizeof(R.target));
        if (++R.ramp_k >= R.ramp_n) {
            if (st == MPX_POLICY_HOLD_OUT) {
                char why[64];
                portENTER_CRITICAL(&s_mux);
                std::snprintf(why, sizeof(why), "%s", s_st.last_stop[0] ? s_st.last_stop : "stopped");
                portEXIT_CRITICAL(&s_mux);
                finish(why, true);
                return;
            }
            enter_run();
        }
        break;
    case MPX_POLICY_RUN:
        policy_step();
        if (s_state == MPX_POLICY_RUN && !R.unlimited && R.steps_left > 0 && --R.steps_left == 0) {
            graceful_stop("completed");
        }
        break;
    case MPX_POLICY_GETUP:
        getup_step(tilt);
        break;
    default:
        break;
    }
    log_step(now, st, ex_us, fresh, tilt, tmax, cur_max);
}

void tick_cb(void *)
{
    if (s_task) xTaskNotifyGive(s_task);
}

void tick_task(void *)
{
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const int64_t now = esp_timer_get_time();
        bool start, stop;
        char why[64];
        portENTER_CRITICAL(&s_mux);
        start = s_req_start;
        stop = s_req_stop;
        s_req_stop = false;
        std::memcpy(why, s_req_why, sizeof(why));
        const bool deadman = s_deadman_us > 0 && now - s_keepalive_us > s_deadman_us;
        portEXIT_CRITICAL(&s_mux);

        // The timer always runs (an idle tick is these two tests): stopping
        // and restarting it from two tasks could lose a start.
        if (start) {
            start_run();
            continue;
        }
        if (s_state == MPX_POLICY_IDLE) continue;
        read_imu(now);
        if (stop) {
            graceful_stop(why[0] ? why : "stop");
            if (s_state == MPX_POLICY_IDLE) continue;
        }
        if (deadman && s_state != MPX_POLICY_RAMP_OUT && s_state != MPX_POLICY_HOLD_OUT) {
            graceful_stop("the app stopped answering");
            if (s_state == MPX_POLICY_IDLE) continue;
        }
        if (s_state == MPX_POLICY_PREP) {
            tick_prep();
        } else {
            tick_armed(now);
            const uint32_t us = static_cast<uint32_t>(esp_timer_get_time() - now);
            portENTER_CRITICAL(&s_mux);
            if (us > s_st.tick_us_max) s_st.tick_us_max = us;
            portEXIT_CRITICAL(&s_mux);
        }
    }
}

void layers_to_internal(pol_net_t *net, float *blk[MPXP_MAX_LAYERS], size_t *bytes_int, size_t *bytes_all);

esp_err_t install(uint8_t *file, size_t len, bool builtin, char *why, int why_len)
{
    pol_net_t net;
    const char *err = pol_parse(file, len, &net);
    if (err) {
        if (why) std::snprintf(why, why_len, "%s", err);
        return ESP_ERR_INVALID_ARG;
    }
    // What this runtime hard-codes (the 5 ms tick, a step every 4 ticks)
    // and what would turn into NaN or overflow further down.
    const mpxp_header_t *hh = net.h;
    const char *bad = nullptr;
    if (std::fabs(hh->ctrl_dt - 0.020f) > 1e-6f || std::fabs(hh->state_dt - 0.005f) > 1e-7f)
        bad = "timing other than 50 Hz policy / 200 Hz tick";
    else if (!(hh->tilt_tau_s > 0.0f) || !(hh->tilt_limit_deg > 0.0f) || !(hh->step_limit_deg > 0.0f))
        bad = "bad tilt or step limit";
    else if (!(hh->cap_ma > 0.0f) || hh->cap_ma > 32767.0f)
        bad = "bad current cap";
    for (int i = 0; i < POL_N && !bad; ++i) {
        if (!(hh->scale[i] > 0.0f) || !(hh->band_deg[i] > 0.0f) || (hh->signs[i] != 1.0f && hh->signs[i] != -1.0f))
            bad = "bad action scale, band or sign";
    }
    if (bad) {
        if (why) std::snprintf(why, why_len, "%s", bad);
        return ESP_ERR_INVALID_ARG;
    }
    float *work = static_cast<float *>(heap_caps_malloc(sizeof(float) * 2 * net.max_width, MALLOC_CAP_8BIT));
    if (!work) {
        if (why) std::snprintf(why, why_len, "out of memory");
        return ESP_ERR_NO_MEM;
    }
    float *blk[MPXP_MAX_LAYERS];
    size_t b_int = 0, b_all = 0;
    layers_to_internal(&net, blk, &b_int, &b_all);
    const float e = pol_selftest(&net, work);   // on the copies, as it will run
    if (!(e < 1e-4f)) {
        for (uint32_t l = 0; l < MPXP_MAX_LAYERS && l < net.h->n_layers; ++l)
            if (blk[l]) heap_caps_free(blk[l]);
        heap_caps_free(work);
        if (why) std::snprintf(why, why_len, "self-test failed (error %g)", static_cast<double>(e));
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t *old_file = s_file;
    float *old_work = s_work;
    float *old_blk[MPXP_MAX_LAYERS];
    std::memcpy(old_blk, s_int_blk, sizeof(old_blk));
    for (uint32_t l = 0; l < MPXP_MAX_LAYERS; ++l) s_int_blk[l] = l < net.h->n_layers ? blk[l] : nullptr;
    s_int_bytes = b_int;
    s_weight_bytes = b_all;
    s_file = file;
    s_file_len = len;
    s_net = net;
    s_work = work;
    s_builtin = builtin;
    // A policy trained for this robot runs on this robot's own calibration; one
    // for another robot on its contract's (as omniRL's Pi runner does).
    // `policy offsets` changes it until the next load.
    s_use_robot_offsets = std::strcmp(net.h->robot, CONFIG_MP4_POLICY_ROBOT) == 0;
    portENTER_CRITICAL(&s_mux);
    s_st.robot_offsets = s_use_robot_offsets;
    std::snprintf(s_st.name, sizeof(s_st.name), "%s", net.h->name);
    std::snprintf(s_st.robot, sizeof(s_st.robot), "%s", net.h->robot);
    s_st.omni = (net.h->flags & MPXP_FLAG_OMNI) != 0;
    portEXIT_CRITICAL(&s_mux);
    if (old_file) heap_caps_free(old_file);
    if (old_work) heap_caps_free(old_work);
    for (float *p : old_blk)
        if (p) heap_caps_free(p);
    ESP_LOGI(TAG, "policy %s (%s, for %s): %u layers, %u of %u KB weights in internal RAM, "
                  "self-test max error %.2g", net.h->name, builtin ? "built in" : "from flash", net.h->robot,
             static_cast<unsigned>(net.h->n_layers), static_cast<unsigned>(b_int / 1024),
             static_cast<unsigned>(b_all / 1024), static_cast<double>(e));
    return ESP_OK;
}

// Until the build reruns menuconfig's step, a new Kconfig option is missing
// from sdkconfig.h: use its default.
#ifndef CONFIG_MP4_POLICY_INTERNAL_RESERVE_KB
#define CONFIG_MP4_POLICY_INTERNAL_RESERVE_KB 96
#endif

uint8_t *alloc_file(size_t len)
{
    uint8_t *p = static_cast<uint8_t *>(heap_caps_aligned_alloc(4, len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!p) p = static_cast<uint8_t *>(heap_caps_aligned_alloc(4, len, MALLOC_CAP_8BIT));
    return p;
}

/* The built-in get-up policy, once at init (it stays in PSRAM: GETUP is
 * short, and internal RAM goes to the walk first). It must be a get-up
 * policy, run on the walk's timing and servo directions, and pass its own
 * self-test. */
void install_getup()
{
#ifdef MPX_POLICY_HAVE_GETUP
    const size_t len = static_cast<size_t>(getup_mpxp_end - getup_mpxp_start);
    uint8_t *f = alloc_file(len);
    if (!f) return;
    std::memcpy(f, getup_mpxp_start, len);
    pol_net_t net;
    const char *err = pol_parse(f, len, &net);
    const mpxp_header_t *h = err ? nullptr : net.h;
    if (!err && !(h->flags & MPXP_FLAG_GETUP)) err = "not a get-up policy";
    if (!err && (std::fabs(h->ctrl_dt - 0.020f) > 1e-6f || std::fabs(h->state_dt - 0.005f) > 1e-7f))
        err = "timing other than 50 Hz policy / 200 Hz tick";
    if (!err && (!(h->step_limit_deg > 0.0f) || !(h->cap_ma > 0.0f) || h->cap_ma > 32767.0f))
        err = "bad step limit or current cap";
    for (int i = 0; i < POL_N && !err; ++i) {
        if (!(h->scale[i] > 0.0f) || !(h->band_deg[i] > 0.0f) || h->band_deg[i] > 150.0f)
            err = "bad action scale or band";
        else if (s_net.h && h->signs[i] != s_net.h->signs[i])
            err = "servo directions differ from the walk policy's";
    }
    float *work = err ? nullptr
                      : static_cast<float *>(heap_caps_malloc(sizeof(float) * 2 * net.max_width, MALLOC_CAP_8BIT));
    if (!err && !work) err = "out of memory";
    if (!err) {
        const float e = pol_selftest(&net, work);
        if (!(e < 1e-4f)) err = "self-test failed";
    }
    if (err) {
        ESP_LOGE(TAG, "built-in get-up policy refused: %s", err);
        if (work) heap_caps_free(work);
        heap_caps_free(f);
        return;
    }
    s_gu_net = net;
    s_gu_work = work;
    s_gu_ok = true;
    portENTER_CRITICAL(&s_mux);
    s_st.getup_ready = true;
    std::snprintf(s_st.getup_name, sizeof(s_st.getup_name), "%s", h->name);
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "get-up policy %s (for %s), band %.0f deg", h->name, h->robot, static_cast<double>(h->band_deg[0]));
#endif
}

/* Each step reads every weight once, and from PSRAM that is most of the
 * step's time. Copy layers (biggest first) into internal RAM while at least
 * MP4_POLICY_INTERNAL_RESERVE_KB of it stays free for Wi-Fi, TLS and the
 * tasks. Separate blocks fit a fragmented heap better than one. */
void layers_to_internal(pol_net_t *net, float *blk[MPXP_MAX_LAYERS], size_t *bytes_int, size_t *bytes_all)
{
    constexpr uint32_t caps_int = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t reserve = static_cast<size_t>(CONFIG_MP4_POLICY_INTERNAL_RESERVE_KB) * 1024u;
    const uint32_t nl = net->h->n_layers;
    size_t sz[MPXP_MAX_LAYERS];
    bool done[MPXP_MAX_LAYERS] = {};
    *bytes_int = 0;
    *bytes_all = 0;
    for (uint32_t l = 0; l < nl; ++l) {
        sz[l] = sizeof(float) * (static_cast<size_t>(net->h->dims[l]) + 1u) * net->h->dims[l + 1];
        *bytes_all += sz[l];
        blk[l] = nullptr;
    }
    if (reserve == 0) return;
    for (uint32_t k = 0; k < nl; ++k) {
        int best = -1;
        for (uint32_t l = 0; l < nl; ++l)
            if (!done[l] && (best < 0 || sz[l] > sz[best])) best = static_cast<int>(l);
        done[best] = true;
        if (heap_caps_get_free_size(caps_int) < sz[best] + reserve ||
            heap_caps_get_largest_free_block(caps_int) < sz[best] + 16)
            continue;
        float *p = static_cast<float *>(heap_caps_malloc(sz[best], caps_int));
        if (!p) continue;
        const size_t nw = static_cast<size_t>(net->h->dims[best]) * net->h->dims[best + 1];
        std::memcpy(p, net->W[best], sizeof(float) * nw);
        std::memcpy(p + nw, net->b[best], sizeof(float) * net->h->dims[best + 1]);
        net->W[best] = p;
        net->b[best] = p + nw;
        blk[best] = p;
        *bytes_int += sz[best];
    }
}

}  // namespace

// ── internal API (policy_console.c) ─────────────────────────────────────

extern "C" esp_err_t policy_load_builtin(char *why, int why_len)
{
    if (s_state != MPX_POLICY_IDLE) {
        std::snprintf(why, why_len, "a run is on");
        return ESP_ERR_INVALID_STATE;
    }
    const size_t len = static_cast<size_t>(default_mpxp_end - default_mpxp_start);
    uint8_t *f = alloc_file(len);
    if (!f) {
        std::snprintf(why, why_len, "out of memory");
        return ESP_ERR_NO_MEM;
    }
    std::memcpy(f, default_mpxp_start, len);
    const esp_err_t e = install(f, len, true, why, why_len);
    if (e != ESP_OK) heap_caps_free(f);
    return e;
}

extern "C" esp_err_t policy_load_file(const char *path, char *why, int why_len)
{
    if (s_state != MPX_POLICY_IDLE) {
        std::snprintf(why, why_len, "a run is on");
        return ESP_ERR_INVALID_STATE;
    }
    FILE *fp = std::fopen(path, "rb");
    if (!fp) {
        std::snprintf(why, why_len, "cannot open %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    std::fseek(fp, 0, SEEK_END);
    const long len = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (len <= 0 || len > 4 * 1024 * 1024) {
        std::fclose(fp);
        std::snprintf(why, why_len, "bad file size %ld", len);
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *f = alloc_file(static_cast<size_t>(len));
    if (!f) {
        std::fclose(fp);
        std::snprintf(why, why_len, "out of memory");
        return ESP_ERR_NO_MEM;
    }
    const size_t got = std::fread(f, 1, static_cast<size_t>(len), fp);
    std::fclose(fp);
    if (got != static_cast<size_t>(len)) {
        heap_caps_free(f);
        std::snprintf(why, why_len, "read %u of %ld bytes", static_cast<unsigned>(got), len);
        return ESP_FAIL;
    }
    const esp_err_t e = install(f, static_cast<size_t>(len), false, why, why_len);
    if (e != ESP_OK) heap_caps_free(f);
    return e;
}

extern "C" const pol_net_t *policy_net(void)
{
    return s_net.h ? &s_net : nullptr;
}

extern "C" float *policy_work(void)
{
    return s_work;
}

extern "C" bool policy_is_builtin(void)
{
    return s_builtin;
}

extern "C" void policy_weights_ram(size_t *internal, size_t *total)
{
    *internal = s_int_bytes;
    *total = s_weight_bytes;
}

extern "C" void policy_set_robot_offsets(bool on)
{
    s_use_robot_offsets = on;
    portENTER_CRITICAL(&s_mux);
    s_st.robot_offsets = on;
    portEXIT_CRITICAL(&s_mux);
}

extern "C" void policy_stand_dd(int out[12])
{
    const mpxp_header_t *h = s_net.h;
    for (int i = 0; i < POL_N; ++i) {
        out[i] = s_use_robot_offsets ? pol_dd_from_rel(0.0, h->signs[i], robot::get_offset(i + 1))
                                     : h->stand_dd[i];
    }
}

extern "C" bool policy_imu_now(double gravity[3], double gyro[3], double *tilt_deg)
{
    if (!s_net.h) return false;
    const robot::ImuData d = robot::imu_read();
    if (d.seq == 0) return false;
    const double a[3] = { d.ax, d.ay, d.az };
    const double g[3] = { d.gx * DEG, d.gy * DEG, d.gz * DEG };
    const double zero[3] = { 0.0, 0.0, 0.0 };
    if (!pol_gravity_body(s_net.h, a, gravity)) return false;
    pol_gyro_body(s_net.h, g, zero, gyro);
    // gravity_body is -a/|a| in the body frame: upright (0, 0, -1)
    *tilt_deg = std::acos(std::max(-1.0, std::min(1.0, -gravity[2]))) / DEG;
    return true;
}

extern "C" bool policy_last_step(float obs[45], float raw[12], int dd[12])
{
    bool ok;
    portENTER_CRITICAL(&s_mux);
    ok = s_last_valid;
    if (ok) {
        std::memcpy(obs, s_last_obs, sizeof(s_last_obs));
        std::memcpy(raw, s_last_raw, sizeof(s_last_raw));
        std::memcpy(dd, s_last_dd, sizeof(s_last_dd));
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

namespace {
struct TimeJob {
    int n;
    float avg_us, max_us;
    SemaphoreHandle_t done;
};

void time_task(void *arg)
{
    TimeJob *j = static_cast<TimeJob *>(arg);
    float act[POL_ACT];
    float *work = static_cast<float *>(heap_caps_malloc(sizeof(float) * 2 * s_net.max_width, MALLOC_CAP_8BIT));
    if (work) {
        int64_t total = 0, worst = 0;
        for (int k = 0; k < j->n; ++k) {
            const int64_t t0 = esp_timer_get_time();
            pol_forward(&s_net, s_net.fix_obs + (size_t)(k % std::max<uint32_t>(1, s_net.h->n_fixtures)) * POL_OBS,
                        act, work);
            const int64_t us = esp_timer_get_time() - t0;
            total += us;
            worst = std::max(worst, us);
        }
        j->avg_us = static_cast<float>(total) / j->n;
        j->max_us = static_cast<float>(worst);
        heap_caps_free(work);
    }
    xSemaphoreGive(j->done);
    vTaskDelete(nullptr);
}
}  // namespace

extern "C" bool policy_time_forward(int n, float *avg_us, float *max_us)
{
    if (!s_net.h || s_state != MPX_POLICY_IDLE || n <= 0) return false;
    TimeJob j = { n, -1.0f, -1.0f, xSemaphoreCreateBinary() };
    if (!j.done) return false;
    // The policy task's core and priority: what a run would see.
    if (xTaskCreatePinnedToCore(time_task, "pol_time", 4096, &j, 22, nullptr, 1) != pdPASS) {
        vSemaphoreDelete(j.done);
        return false;
    }
    xSemaphoreTake(j.done, portMAX_DELAY);
    vSemaphoreDelete(j.done);
    *avg_us = j.avg_us;
    *max_us = j.max_us;
    return j.avg_us >= 0.0f;
}

// ── public API ──────────────────────────────────────────────────────────

extern "C" esp_err_t mpx_policy_init(void)
{
    if (s_task) return ESP_OK;
    char why[64] = "";
    esp_err_t e = ESP_FAIL;
    char path[96] = "";
    nvs_handle_t h;
    if (nvs_open("policy", NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(path);
        if (nvs_get_str(h, "file", path, &n) != ESP_OK) path[0] = '\0';
        nvs_close(h);
    }
    if (path[0]) {
        e = policy_load_file(path, why, sizeof(why));
        if (e != ESP_OK) ESP_LOGW(TAG, "%s: %s - using the built-in policy", path, why);
    }
    if (e != ESP_OK) {
        e = policy_load_builtin(why, sizeof(why));
        if (e != ESP_OK) ESP_LOGE(TAG, "built-in policy refused: %s", why);
    }
    install_getup();

    const esp_timer_create_args_t ta = { .callback = tick_cb, .arg = nullptr,
                                         .dispatch_method = ESP_TIMER_TASK, .name = "policy",
                                         .skip_unhandled_events = true };
    if (esp_timer_create(&ta, &s_timer) != ESP_OK) return ESP_FAIL;
    // Core 1 with the gait (parked while a run is on), above everything but
    // the servo bus itself; the network is ~1 ms of every 20.
    if (xTaskCreatePinnedToCore(tick_task, "policy", 6144, nullptr, 22, &s_task, 1) != pdPASS) {
        s_task = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return esp_timer_start_periodic(s_timer, TICK_US);
}

extern "C" esp_err_t mpx_policy_start(float seconds, float hold_s, bool force, bool dry, char *why, int why_len)
{
    if (!s_net.h) {
        std::snprintf(why, why_len, "no policy loaded");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state != MPX_POLICY_IDLE) {
        std::snprintf(why, why_len, "a run is already on (%s)", mpx_policy_state_name(s_state));
        return ESP_ERR_INVALID_STATE;
    }
    if (!dry && (robot::master_active() || robot::host::active())) {
        std::snprintf(why, why_len, "the robot is held by ROS or another host");
        return ESP_ERR_INVALID_STATE;
    }
    // The contract converts dd with the AT32 range at 310 deg (3100 dd).
    if (!dry && std::fabs(driver_board_get_scale_dd() - 3100.0f) > 0.5f) {
        std::snprintf(why, why_len, "the servo boards' range is %.0f dd, the policy needs 3100",
                      static_cast<double>(driver_board_get_scale_dd()));
        return ESP_ERR_INVALID_STATE;
    }
    if (!force && !dry && std::strcmp(s_net.h->robot, CONFIG_MP4_POLICY_ROBOT) != 0) {
        std::snprintf(why, why_len, "this policy is for %s, the robot is %s (add `force` to run it anyway)",
                      s_net.h->robot, CONFIG_MP4_POLICY_ROBOT);
        return ESP_ERR_INVALID_ARG;
    }
    if (!std::isfinite(seconds) || !(seconds >= 0.0f) || seconds > 3600.0f || !std::isfinite(hold_s) ||
        !(hold_s >= 0.0f) || hold_s > 60.0f) {
        std::snprintf(why, why_len, "bad duration");
        return ESP_ERR_INVALID_ARG;
    }
    bool busy;
    portENTER_CRITICAL(&s_mux);
    busy = s_state != MPX_POLICY_IDLE || s_req_start;
    if (!busy) {
        s_req_start = true;
        s_req_stop = false;
        s_keepalive_us = esp_timer_get_time();
        s_req_seconds = seconds;
        s_req_hold = hold_s;
        s_req_dry = dry;
        // PREP from here: a load or a second start is refused until it ends.
        s_state = MPX_POLICY_PREP;
        s_st.state = MPX_POLICY_PREP;
    }
    portEXIT_CRITICAL(&s_mux);
    if (busy) {
        std::snprintf(why, why_len, "a run is already on");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

extern "C" bool mpx_policy_set_auto_getup(bool on)
{
    if (on && !s_gu_ok) return false;
    s_auto_getup = on;
    portENTER_CRITICAL(&s_mux);
    s_st.auto_getup = on;
    portEXIT_CRITICAL(&s_mux);
    return true;
}

extern "C" esp_err_t mpx_policy_getup(bool force, char *why, int why_len)
{
    if (!s_gu_ok) {
        std::snprintf(why, why_len, "no get-up policy (policies/getup.mpxp) is built in");
        return ESP_ERR_NOT_FOUND;
    }
    if (!force && std::strcmp(s_gu_net.h->robot, CONFIG_MP4_POLICY_ROBOT) != 0) {
        std::snprintf(why, why_len, "the get-up policy is for %s, the robot is %s (add `force` to run it anyway)",
                      s_gu_net.h->robot, CONFIG_MP4_POLICY_ROBOT);
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_mux);
    const bool busy = s_state != MPX_POLICY_IDLE || s_req_start;
    if (!busy) s_req_getup = true;
    portEXIT_CRITICAL(&s_mux);
    if (busy) {
        std::snprintf(why, why_len, "a run is already on");
        return ESP_ERR_INVALID_STATE;
    }
    // The walk's checks and start (its servo configuration, calibration and
    // gyro bias); `force` also covers the walk policy's robot.
    const esp_err_t e = mpx_policy_start(GETUP_MAX_S + 5.0f, 0.0f, true, false, why, why_len);
    if (e != ESP_OK) {
        portENTER_CRITICAL(&s_mux);
        s_req_getup = false;
        portEXIT_CRITICAL(&s_mux);
    }
    return e;
}

extern "C" void mpx_policy_set_deadman_ms(uint32_t ms)
{
    portENTER_CRITICAL(&s_mux);
    s_deadman_us = static_cast<int64_t>(ms) * 1000;
    s_keepalive_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_mux);
}

extern "C" void mpx_policy_keepalive(void)
{
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    s_keepalive_us = now;
    portEXIT_CRITICAL(&s_mux);
}

extern "C" bool mpx_policy_selftest(float *max_err, float *us_avg, float *us_max)
{
    if (!s_net.h || s_state != MPX_POLICY_IDLE || !s_work) return false;
    const float e = pol_selftest(&s_net, s_work);
    float avg = -1.0f, worst = -1.0f;
    (void)policy_time_forward(200, &avg, &worst);
    if (max_err) *max_err = e;
    if (us_avg) *us_avg = avg;
    if (us_max) *us_max = worst;
    return true;
}

extern "C" bool mpx_policy_cmd_max(float out[3])
{
    if (!s_net.h) return false;
    std::memcpy(out, s_net.h->cmd_max, sizeof(float) * 3);
    return true;
}

extern "C" bool mpx_policy_set_own_calibration(bool own)
{
    if (s_state != MPX_POLICY_IDLE) return false;
    policy_set_robot_offsets(own);
    return true;
}

extern "C" void mpx_policy_set_log(bool on)
{
    s_log_req = on;
}

extern "C" size_t mpx_policy_log_count(size_t *dropped)
{
    if (dropped) *dropped = s_log_dropped;
    return s_log ? s_log_n : 0;
}

extern "C" const char *mpx_policy_log_header(void)
{
    return "t_ms,phase,dt_us,tick_us,exchange_us,network_us,late,fresh_legs,tilt_deg,temp_c,"
           "current_ma,track_mean_deg,track_max_deg,vx,vy,wz";
}

extern "C" bool mpx_policy_log_row(size_t i, char *buf, size_t len)
{
    if (!s_log || i >= s_log_n || s_state != MPX_POLICY_IDLE) return false;
    static const char *const ph[] = { "idle", "prep", "ramp_in", "hold_in", "run", "ramp_out", "hold_out",
                                      "getup" };
    const LogRec &r = s_log[i];
    char tilt[12];
    tilt[0] = '\0';
    if (r.tilt_cdeg != INT16_MIN) std::snprintf(tilt, sizeof(tilt), "%.2f", r.tilt_cdeg / 100.0);
    std::snprintf(buf, len, "%lu,%s,%lu,%u,%u,%u,%u,%u,%s,%.1f,%d,%.2f,%.2f,%.3f,%.3f,%.3f",
                  static_cast<unsigned long>(r.t_ms), r.phase < 8 ? ph[r.phase] : "?",
                  static_cast<unsigned long>(r.dt_us), r.tick_us, r.ex_us, r.fwd_us, r.flags & 1u,
                  __builtin_popcount(r.flags >> 4), tilt, r.temp_dc / 10.0, r.cur_ma,
                  r.trk_mean_cdeg / 100.0, r.trk_max_cdeg / 100.0, r.vx_mm / 1000.0, r.vy_mm / 1000.0,
                  r.wz_mrad / 1000.0);
    return true;
}

extern "C" void mpx_policy_stop(const char *reason)
{
    portENTER_CRITICAL(&s_mux);
    if (s_req_start) {
        // Accepted but not begun: cancel it; nothing has moved.
        s_req_start = false;
        s_state = MPX_POLICY_IDLE;
        s_st.state = MPX_POLICY_IDLE;
        std::snprintf(s_st.last_stop, sizeof(s_st.last_stop), "%s (before it began)", reason ? reason : "stop");
    } else if (s_state != MPX_POLICY_IDLE) {
        s_req_stop = true;
        std::snprintf(s_req_why, sizeof(s_req_why), "%s", reason ? reason : "stop");
    }
    portEXIT_CRITICAL(&s_mux);
}

extern "C" void mpx_policy_set_cmd(float vx, float vy, float wz)
{
    float m[3] = { 0.1f, 0.1f, 0.5f };
    if (s_net.h) std::memcpy(m, s_net.h->cmd_max, sizeof(m));
    const float in[3] = { vx, vy, wz };
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < 3; ++i) {
        float v = std::isfinite(in[i]) ? in[i] : 0.0f;
        if (v > m[i]) v = m[i];
        if (v < -m[i]) v = -m[i];
        s_cmd[i] = v;
        s_st.cmd[i] = v;
    }
    portEXIT_CRITICAL(&s_mux);
}

extern "C" bool mpx_policy_active(void)
{
    return s_state != MPX_POLICY_IDLE;
}

extern "C" void mpx_policy_get_status(mpx_policy_status_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
    out->state = s_state;
}

extern "C" const char *mpx_policy_state_name(mpx_policy_state_t s)
{
    switch (s) {
    case MPX_POLICY_IDLE:     return "idle";
    case MPX_POLICY_PREP:     return "preparing (servo config, gyro bias)";
    case MPX_POLICY_RAMP_IN:  return "ramping to the stand";
    case MPX_POLICY_HOLD_IN:  return "holding the stand";
    case MPX_POLICY_RUN:      return "RUNNING the policy";
    case MPX_POLICY_RAMP_OUT: return "stopping: ramping to the stand";
    case MPX_POLICY_HOLD_OUT: return "stopping: holding the stand";
    case MPX_POLICY_GETUP:    return "GETTING UP";
    }
    return "?";
}
