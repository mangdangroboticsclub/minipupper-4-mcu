/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * The policy on the robot: omniRL's runner.py (Runner, ObservationBuilder,
 * the run sequence and its stop rules), on a 200 Hz tick here instead of the
 * Pi's STATE stream. The arithmetic is policy_core.c; this file is the timing,
 * the servos and the safety.
 *
 * The network runs in its own task (infer_task, same core, one priority
 * below the tick): a step boundary hands it the observation, the ticks go on
 * (servos, IMU, guards every 5 ms) while it computes, and the first tick after
 * it is done turns its action into servo targets, before that tick's exchange.
 * A network under one tick (today's 31k) lands exactly where it did when it
 * ran inside the tick; a bigger one has most of the 20 ms step, and lands one
 * tick (5 ms) later for each tick it takes. Still running at the next step
 * boundary: that step is late (no new observation), and two in a row stop the
 * run, as before.
 *
 * Servos go through the raw path the Pi's RL link used (robot::host::begin(),
 * driver_board_sync_raw()): the gait is parked, the AT32 boards get absolute
 * targets in their own units (dd) every tick, and robot::host::end() puts the
 * walk's servo gains back and glides the robot into its stand afterwards.
 *
 * Stops, as the runner's:
 *   immediate (limp now): tilt over the policy's limit (low-passed, 0.1 s),
 *     a servo over the current or temperature guard, a board silent;
 *   graceful (ramp to the stand, hold, limp): a stop from the app, the run's time
 *     up, the IMU invalid, a servo at the policy's stop temperature, a
 *     non-finite action, two late steps in a row.
 *
 * The get-up policy (policies/getup.mpxp, omniRL getup.py; built in when the
 * file is there) is a second network with the walk's inputs and outputs and a
 * wider range about the stand. It drives the joints in GETUP: on its own
 * (the app's Get up, mpx_policy_getup(): from wherever the robot lies to the
 * stand, then the robot is given back), or inside a walk with "never stays
 * down" on (mpx_policy_set_auto_getup()): a tilt over the walk's limit starts
 * it instead of going limp, and the walk goes on once the robot stands. GETUP
 * has no tilt stop (lying on the back is where it starts), a higher current
 * guard, and a time limit; the servo, temperature and board guards stay.
 */
#include "mpx_policy.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "policy_core.h"
#include "robot/driver_board.h"
#include "robot/robot.h"

extern "C" {
extern const uint8_t default_mpxp_start[] asm("_binary_default_mpxp_start");
extern const uint8_t default_mpxp_end[] asm("_binary_default_mpxp_end");
#ifdef MPX_POLICY_HAVE_GETUP
extern const uint8_t getup_mpxp_start[] asm("_binary_getup_mpxp_start");
extern const uint8_t getup_mpxp_end[] asm("_binary_getup_mpxp_end");
#endif
#ifdef MPX_POLICY_HAVE_TERRAIN
extern const uint8_t terrain_mpxp_start[] asm("_binary_terrain_mpxp_start");
extern const uint8_t terrain_mpxp_end[] asm("_binary_terrain_mpxp_end");
#endif
#ifdef MPX_POLICY_HAVE_SKU1
extern const uint8_t sku1_mpxp_start[] asm("_binary_sku1_mpxp_start");
extern const uint8_t sku1_mpxp_end[] asm("_binary_sku1_mpxp_end");
#endif
#ifdef MPX_POLICY_HAVE_SKU1_TERRAIN
extern const uint8_t sku1_terrain_mpxp_start[] asm("_binary_sku1_terrain_mpxp_start");
extern const uint8_t sku1_terrain_mpxp_end[] asm("_binary_sku1_terrain_mpxp_end");
#endif
#ifdef MPX_POLICY_HAVE_SKU1_TERR11K
extern const uint8_t sku1_terr11k_mpxp_start[] asm("_binary_sku1_terr11k_mpxp_start");
extern const uint8_t sku1_terr11k_mpxp_end[] asm("_binary_sku1_terr11k_mpxp_end");
#endif
#ifdef MPX_POLICY_HAVE_SKU1_TERR15K
extern const uint8_t sku1_terr15k_mpxp_start[] asm("_binary_sku1_terr15k_mpxp_start");
extern const uint8_t sku1_terr15k_mpxp_end[] asm("_binary_sku1_terr15k_mpxp_end");
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
// The temperature checks (the start's "wait below", the policy's stop
// temperature, this guard): off unless APP_POLICY_TEMP_GUARD. The temperature
// is still read, shown and logged.
#ifdef CONFIG_APP_POLICY_TEMP_GUARD
constexpr bool    TEMP_GUARD     = true;
#else
constexpr bool    TEMP_GUARD     = false;
#endif
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
constexpr double  GETUP_PHASE_LYING_DEG = 60.0;   // a tracked get-up motion starts from its beginning beyond this tilt

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// The loaded policy (PSRAM). Replaced only while idle.
uint8_t  *s_file = nullptr;
size_t    s_file_len = 0;
pol_net_t s_net;
float    *s_work = nullptr;
bool      s_builtin = true;
// The layers' own copies (place_layers(): 16- or 8-bit blocks, in internal
// RAM or PSRAM, or float copies in internal RAM; the file itself sits in PSRAM).
void     *s_int_blk[MPXP_MAX_LAYERS] = {};
size_t    s_int_bytes = 0, s_weight_bytes = 0;
// Weights as the network runs them: 32 (the file's floats), 16 or 8 bits.
#ifndef CONFIG_APP_POLICY_WEIGHT_BITS
#define CONFIG_APP_POLICY_WEIGHT_BITS 16
#endif
int       s_bits = CONFIG_APP_POLICY_WEIGHT_BITS;
// The get-up policy (PSRAM; loaded once at init when built in).
pol_net_t s_gu_net;
float    *s_gu_work = nullptr;
bool      s_gu_ok = false;
bool      s_auto_getup = false;

// Requests from other tasks (under s_mux).
bool  s_req_start = false, s_req_stop = false, s_req_dry = false, s_req_getup = false;
bool  s_bench_busy = false;   // the size test (or the Test button) has the network
float s_req_seconds = 0.0f, s_req_hold = 0.0f;
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
// After a logged run (after_task): its summary on the serial port, and the
// log saved to flash so it outlives a reboot (or a run without Wi-Fi).
constexpr const char *LOG_FILE = "/fs/rl_last_run.csv";
TaskHandle_t s_after = nullptr;
volatile bool s_saving = false;

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
    bool      getup_only;             // Get up alone: GETUP, then give the robot back
    bool      auto_up;                // never stays down: a fall starts GETUP
    uint32_t  getups;
    int       gu_steps, gu_ok;        // GETUP: steps so far, consecutive steps standing
    int       gu_pstep, gu_back;      // GETUP_PHASE: steps since the motion (re)started, steps still on the back
    double    gu_phase0;              //   the phase it started at
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

// ── the network's task (infer_task) ─────────────────────────────────────
enum : int { JOB_IDLE = 0, JOB_RUNNING = 1, JOB_DONE = 2 };
struct Job {
    const pol_net_t *net;
    float    *work;
    float     obs[POL_OBS];
    float     raw[POL_ACT];
    uint32_t  us;                     // the evaluation, start to end (ticks included)
    uint32_t  gen;                    // s_gen when handed over
    std::atomic<int> state{ JOB_IDLE };
};
Job s_job;
TaskHandle_t s_infer = nullptr;
// Bumped on every state change: a result from before one is dropped (a ramp,
// a stop, the walk handing over to the get-up policy and back).
uint32_t s_gen = 0;

void set_state(mpx_policy_state_t s)
{
    ++s_gen;
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
    // Busy until the log is summarised and saved (after_task), so nothing
    // (a new run, the app's Wi-Fi restore) sees an idle robot before that.
    const bool save = s_log_on && s_after;
    if (save) s_saving = true;
    set_state(MPX_POLICY_IDLE);
    ESP_LOGW(TAG, "run ended: %s", why);
    if (save) xTaskNotifyGive(s_after);
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
    if (!s_req_start) {               // cancelled by a stop in the meantime
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
    // Someone else may have taken the robot since the run was accepted.
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
    R.gu_pstep = 0;
    R.gu_back = 0;
    R.gu_phase0 = 0.0;
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
        if (TEMP_GUARD && t >= static_cast<int16_t>(h->temp_start_c * 10.0f) && t < 1500) {
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

void policy_apply();

/* A policy step (runner.loop body). */
void policy_step()
{
    // An answer that came in during this tick (while its exchange waited on
    // the servo bus) goes in first: the observation carries the last action.
    if (s_job.state.load() == JOB_DONE) {
        policy_apply();
        if (s_state != MPX_POLICY_RUN && s_state != MPX_POLICY_GETUP) return;
    }
    const pol_net_t *net = R.net;
    const mpxp_header_t *h = net->h;
    float cmdf[3];
    portENTER_CRITICAL(&s_mux);
    std::memcpy(cmdf, s_cmd, sizeof(cmdf));
    portEXIT_CRITICAL(&s_mux);
    double cmd[3] = { cmdf[0], cmdf[1], cmdf[2] };
    if (!(h->flags & MPXP_FLAG_OMNI)) cmd[1] = cmd[2] = 0.0;
    if (h->flags & MPXP_FLAG_GETUP) {
        cmd[0] = cmd[1] = cmd[2] = 0.0;                                // trained with no command ...
        if ((h->flags & MPXP_FLAG_GETUP_PHASE) && h->cmd_max[0] > 0.0f) {
            // ... or with the motion's phase in its place (getup_track.py obs_phase)
            double ph = R.gu_phase0 + R.gu_pstep * h->ctrl_dt / h->cmd_max[0];
            if (ph > 1.0) ph = 1.0;
            cmd[0] = ph;
            cmd[1] = std::sin(2.0 * M_PI * ph);
            cmd[2] = std::cos(2.0 * M_PI * ph);
        }
    }

    double gb[3], grav[3], rel[POL_N], vel[POL_N];
    pol_gyro_body(h, R.gyr, R.bias, gb);
    pol_gravity_body(h, R.acc, grav);
    pos_rel(rel);
    pol_vel_get(&R.vf, vel);
    if (s_job.state.load() == JOB_RUNNING) {
        // The last step's network is not done: no new observation this step,
        // the joints keep the last targets.
        R.late_in_step = true;
        R.log_late = true;
    } else {
        pol_observe(gb, grav, cmd, rel, vel, R.dec.prev_action, s_job.obs);
        s_job.net = net;
        s_job.work = R.work;
        s_job.gen = s_gen;
        s_job.state.store(JOB_RUNNING);
        xTaskNotifyGive(s_infer);
    }

    R.late_run = R.late_in_step ? R.late_run + 1 : 0;
    portENTER_CRITICAL(&s_mux);
    s_st.steps++;
    if (R.late_in_step) s_st.late_steps++;
    portEXIT_CRITICAL(&s_mux);
    R.late_in_step = false;
    if (R.late_run >= 2) graceful_stop("two late steps in a row");
}

/* The network's answer, at the start of the first tick after it is done (so
 * that tick's exchange sends it): the action decoded into servo targets. */
void policy_apply()
{
    if (s_job.state.load() != JOB_DONE) return;
    const bool ours = s_job.gen == s_gen && (s_state == MPX_POLICY_RUN || s_state == MPX_POLICY_GETUP);
    const uint32_t us = s_job.us;
    float raw[POL_ACT];
    std::memcpy(raw, s_job.raw, sizeof(raw));
    const pol_net_t *net = s_job.net;
    s_job.state.store(JOB_IDLE);
    if (!ours) return;
    R.log_fwd_us = us;
    portENTER_CRITICAL(&s_mux);
    if (us > s_st.forward_us_max) s_st.forward_us_max = us;
    portEXIT_CRITICAL(&s_mux);
    for (int i = 0; i < POL_ACT; ++i) {
        if (!std::isfinite(raw[i])) {
            graceful_stop("non-finite action");
            return;
        }
    }
    pol_decode(&R.dec, net->h, R.off, raw, R.target);
}

void infer_task(void *)
{
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_job.state.load() != JOB_RUNNING) continue;
        const int64_t t0 = esp_timer_get_time();
        pol_forward(s_job.net, s_job.obs, s_job.raw, s_job.work);
        s_job.us = static_cast<uint32_t>(esp_timer_get_time() - t0);
        s_job.state.store(JOB_DONE);
    }
}

/* A GETUP step: the get-up network, then is the robot standing (upright,
 * still, the joints near the stand, for GETUP_OK_STEPS steps)? Then back to
 * the walk, or (Get up alone) to the stand and the end of the run. */
void getup_step(double tilt)
{
    const mpxp_header_t *gh = s_gu_net.h;
    const bool phased = (gh->flags & MPXP_FLAG_GETUP_PHASE) != 0;
    if (phased && R.gu_steps == 0) {
        // where the motion starts, as in training: from its beginning when the robot lies tilted more than
        // 60 deg (on the back or a side), at its upright part when it is roughly upright already
        const bool lying = !std::isfinite(tilt) || tilt > GETUP_PHASE_LYING_DEG;
        R.gu_phase0 = lying ? 0.0 : static_cast<double>(gh->cmd_max[1]);
        ESP_LOGW(TAG, "get-up motion from phase %.2f (tilt %.0f deg)", R.gu_phase0, tilt);
    }
    policy_step();
    if (s_state != MPX_POLICY_GETUP) return;
    ++R.gu_steps;
    ++R.gu_pstep;
    if (phased && R.gu_pstep * gh->ctrl_dt >= gh->cmd_max[0]) {
        // the motion is over: still on the back for a second, start it again
        R.gu_back = (std::isfinite(tilt) && tilt > GETUP_PHASE_LYING_DEG) ? R.gu_back + 1 : 0;
        if (R.gu_back * gh->ctrl_dt >= 1.0) {
            ESP_LOGW(TAG, "still lying (tilt %.0f deg): the get-up motion again", tilt);
            R.gu_phase0 = 0.0;
            R.gu_pstep = 0;
            R.gu_back = 0;
        }
    }
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
    policy_apply();
    if (s_state == MPX_POLICY_IDLE) return;   // a non-finite action ended the run
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
        if (TEMP_GUARD && R.over_temp_us[i] > GUARD_TEMP_US) {
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
    if (TEMP_GUARD && tmax >= h->temp_stop_c && !stopping) {
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

bool place_layers(pol_net_t *net, int bits, void *blk[MPXP_MAX_LAYERS], size_t *bytes_int, size_t *bytes_all);
void free_blocks(void *blk[MPXP_MAX_LAYERS]);

// What the self-test accepts for each weight format: floats match training to
// float rounding; 16-bit to a few thousandths of a degree; 8-bit is a real
// rounding of the weights (the bam_t5ub06 walk: 2.3 deg at worst, 0.36 mean).
bool selftest_ok(int bits, float raw, float worst_deg)
{
    if (bits == 32) return raw < 1e-4f;
    if (bits == 16) return worst_deg < 0.5f;
    return worst_deg < 5.0f;
}

esp_err_t install(uint8_t *file, size_t len, bool builtin, int bits, char *why, int why_len)
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
    void *blk[MPXP_MAX_LAYERS];
    size_t b_int = 0, b_all = 0;
    if (!place_layers(&net, bits, blk, &b_int, &b_all)) {
        heap_caps_free(work);
        if (why) std::snprintf(why, why_len, "no memory for the %d-bit weights", bits);
        return ESP_ERR_NO_MEM;
    }
    float mean_deg = 0.0f;
    const float e = pol_selftest(&net, work);   // on the copies, as it will run
    const float worst_deg = pol_selftest_deg(&net, work, &mean_deg);
    if (!selftest_ok(bits, e, worst_deg)) {
        free_blocks(blk);
        heap_caps_free(work);
        if (why) std::snprintf(why, why_len, "%d-bit self-test failed (%.2f deg off training)", bits,
                               static_cast<double>(worst_deg));
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t *old_file = s_file;
    float *old_work = s_work;
    void *old_blk[MPXP_MAX_LAYERS];
    std::memcpy(old_blk, s_int_blk, sizeof(old_blk));
    for (uint32_t l = 0; l < MPXP_MAX_LAYERS; ++l) s_int_blk[l] = l < net.h->n_layers ? blk[l] : nullptr;
    s_int_bytes = b_int;
    s_weight_bytes = b_all;
    s_bits = bits;
    s_file = file;
    s_file_len = len;
    s_net = net;
    s_work = work;
    s_builtin = builtin;
    // A policy trained for this robot runs on this robot's own calibration; one
    // for another robot on its contract's (as omniRL's Pi runner does).
    // mpx_policy_set_own_calibration() changes it until the next load.
    s_use_robot_offsets = std::strcmp(net.h->robot, CONFIG_APP_POLICY_ROBOT) == 0;
    portENTER_CRITICAL(&s_mux);
    s_st.robot_offsets = s_use_robot_offsets;
    std::snprintf(s_st.name, sizeof(s_st.name), "%s", net.h->name);
    std::snprintf(s_st.robot, sizeof(s_st.robot), "%s", net.h->robot);
    s_st.omni = (net.h->flags & MPXP_FLAG_OMNI) != 0;
    s_st.weight_bits = static_cast<uint8_t>(bits);
    s_st.weights_bytes = static_cast<uint32_t>(b_all);
    s_st.weights_internal = static_cast<uint32_t>(b_int);
    s_st.weights_worst_deg = worst_deg;
    s_st.weights_mean_deg = mean_deg;
    portEXIT_CRITICAL(&s_mux);
    if (old_file) heap_caps_free(old_file);
    if (old_work) heap_caps_free(old_work);
    free_blocks(old_blk);
    ESP_LOGI(TAG, "policy %s (%s, for %s): %u layers, %d-bit weights, %u of %u KB in internal RAM, "
                  "self-test %.3f deg worst, %.4f mean", net.h->name, builtin ? "built in" : "from flash",
             net.h->robot, static_cast<unsigned>(net.h->n_layers), bits, static_cast<unsigned>(b_int / 1024),
             static_cast<unsigned>(b_all / 1024), static_cast<double>(worst_deg), static_cast<double>(mean_deg));
    return ESP_OK;
}

// Until the build reruns menuconfig's step, a new Kconfig option is missing
// from sdkconfig.h: use its default.
#ifndef CONFIG_APP_POLICY_INTERNAL_RESERVE_KB
#define CONFIG_APP_POLICY_INTERNAL_RESERVE_KB 96
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
 * step's time. The layers, biggest first, get their own blocks: a 16- or
 * 8-bit copy (always: in internal RAM while at least
 * APP_POLICY_INTERNAL_RESERVE_KB of it stays free for Wi-Fi, TLS and the
 * tasks, else in PSRAM), or for float weights a copy in internal RAM under
 * the same rule (else the layer reads the file). Separate blocks fit a
 * fragmented heap better than one. False: a 16-/8-bit block found no room
 * anywhere (nothing kept). */
bool place_layers(pol_net_t *net, int bits, void *blk[MPXP_MAX_LAYERS], size_t *bytes_int, size_t *bytes_all)
{
    constexpr uint32_t caps_int = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t reserve = static_cast<size_t>(CONFIG_APP_POLICY_INTERNAL_RESERVE_KB) * 1024u;
    const uint32_t nl = net->h->n_layers;
    size_t sz[MPXP_MAX_LAYERS];
    bool done[MPXP_MAX_LAYERS] = {};
    *bytes_int = 0;
    *bytes_all = 0;
    for (uint32_t l = 0; l < MPXP_MAX_LAYERS; ++l) blk[l] = nullptr;
    for (uint32_t l = 0; l < nl; ++l) {
        const int in = static_cast<int>(net->h->dims[l]), o = static_cast<int>(net->h->dims[l + 1]);
        sz[l] = bits == 32 ? sizeof(float) * (static_cast<size_t>(in) + 1u) * static_cast<size_t>(o)
                           : pol_quant_layer_size(in, o, bits);
        *bytes_all += sz[l];
    }
    for (uint32_t k = 0; k < nl; ++k) {
        int best = -1;
        for (uint32_t l = 0; l < nl; ++l)
            if (!done[l] && (best < 0 || sz[l] > sz[best])) best = static_cast<int>(l);
        done[best] = true;
        void *p = nullptr;
        if (reserve > 0 && heap_caps_get_free_size(caps_int) >= sz[best] + reserve &&
            heap_caps_get_largest_free_block(caps_int) >= sz[best] + 16) {
            p = heap_caps_aligned_alloc(4, sz[best], caps_int);
            if (p) *bytes_int += sz[best];
        }
        if (!p && bits != 32) p = heap_caps_aligned_alloc(4, sz[best], MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) {
            if (bits == 32) continue;   // reads the file's floats
            free_blocks(blk);
            *bytes_int = 0;
            return false;
        }
        blk[best] = p;
        const int in = static_cast<int>(net->h->dims[best]), o = static_cast<int>(net->h->dims[best + 1]);
        if (bits == 32) {
            float *f = static_cast<float *>(p);
            const size_t nw = static_cast<size_t>(in) * o;
            std::memcpy(f, net->W[best], sizeof(float) * nw);
            std::memcpy(f + nw, net->b[best], sizeof(float) * o);
            net->W[best] = f;
            net->b[best] = f + nw;
        } else {
            pol_quantize_layer(net->W[best], net->b[best], in, o, bits, p);
            pol_use_quant_layer(net, static_cast<uint32_t>(best), bits, p);
        }
    }
    return true;
}

void free_blocks(void *blk[MPXP_MAX_LAYERS])
{
    for (uint32_t l = 0; l < MPXP_MAX_LAYERS; ++l) {
        if (blk[l]) heap_caps_free(blk[l]);
        blk[l] = nullptr;
    }
}

}  // namespace

// ── the built-in policy, and timing the network (mpx_policy_selftest) ──

namespace {

// The walk policies built in (main/rl/policies/): the page switches between them.
struct Builtin { const char *id; const uint8_t *start; const uint8_t *end; };
const Builtin BUILTINS[] = {
    { "walk", default_mpxp_start, default_mpxp_end },
#ifdef MPX_POLICY_HAVE_TERRAIN
    { "terrain", terrain_mpxp_start, terrain_mpxp_end },
#endif
#ifdef MPX_POLICY_HAVE_SKU1
    { "sku1", sku1_mpxp_start, sku1_mpxp_end },
#endif
#ifdef MPX_POLICY_HAVE_SKU1_TERRAIN
    { "sku1_terrain", sku1_terrain_mpxp_start, sku1_terrain_mpxp_end },
#endif
#ifdef MPX_POLICY_HAVE_SKU1_TERR11K
    { "sku1_terr11k", sku1_terr11k_mpxp_start, sku1_terr11k_mpxp_end },
#endif
#ifdef MPX_POLICY_HAVE_SKU1_TERR15K
    { "sku1_terr15k", sku1_terr15k_mpxp_start, sku1_terr15k_mpxp_end },
#endif
};
constexpr int N_BUILTINS = static_cast<int>(sizeof(BUILTINS) / sizeof(BUILTINS[0]));
int s_builtin_sel = 0;

esp_err_t policy_load_builtin(int bits, char *why, int why_len)
{
    if (s_state != MPX_POLICY_IDLE) {
        std::snprintf(why, why_len, "a run is on");
        return ESP_ERR_INVALID_STATE;
    }
    const Builtin &bi = BUILTINS[s_builtin_sel];
    const size_t len = static_cast<size_t>(bi.end - bi.start);
    uint8_t *f = alloc_file(len);
    if (!f) {
        std::snprintf(why, why_len, "out of memory");
        return ESP_ERR_NO_MEM;
    }
    std::memcpy(f, bi.start, len);
    const esp_err_t e = install(f, len, true, bits, why, why_len);
    if (e != ESP_OK) heap_caps_free(f);
    return e;
}

/* The built-in policy in `bits`, or in floats when that fails (memory, or an
 * 8-bit rounding the self-test does not accept). */
esp_err_t policy_load_builtin_or_float(int bits, char *why, int why_len)
{
    esp_err_t e = policy_load_builtin(bits, why, why_len);
    if (e != ESP_OK && bits != 32) {
        ESP_LOGW(TAG, "%d-bit weights refused (%s): the float weights instead", bits, why);
        e = policy_load_builtin(32, why, why_len);
    }
    return e;
}

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

bool policy_time_forward(int n, float *avg_us, float *max_us)
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

// ── the size test (mpx_policy_bench_start) ─────────────────────────────

constexpr uint16_t BENCH_H[MPX_POLICY_BENCH_MAX] = { 128, 160, 192, 224, 256, 320, 384, 512, 640 };
constexpr int      BENCH_RUNS  = 50;
constexpr size_t   BENCH_SCRUB = 64 * 1024;    // the S3's largest data cache (this build: 32 KB)

mpx_policy_bench_t s_bench;   // under s_mux

/* The loaded walk goes back to its file's floats during the test (nothing
 * runs it then), so its own blocks -- internal RAM above all -- go back to the
 * heap a bigger network would get; restore_internal() places them again. */
void release_internal()
{
    pol_net_t f;
    if (!s_file || pol_parse(s_file, s_file_len, &f)) return;
    free_blocks(s_int_blk);
    s_net = f;
    s_int_bytes = 0;
}

void restore_internal()
{
    if (!s_file || !s_net.h) return;
    if (!place_layers(&s_net, s_bits, s_int_blk, &s_int_bytes, &s_weight_bytes) && s_bits != 32) {
        ESP_LOGW(TAG, "no room for the %d-bit weights after the size test: the float weights", s_bits);
        pol_net_t f;
        if (pol_parse(s_file, s_file_len, &f)) return;
        s_net = f;
        s_bits = 32;
        (void)place_layers(&s_net, 32, s_int_blk, &s_int_bytes, &s_weight_bytes);
    }
    portENTER_CRITICAL(&s_mux);
    s_st.weight_bits = static_cast<uint8_t>(s_bits);
    s_st.weights_bytes = static_cast<uint32_t>(s_weight_bytes);
    s_st.weights_internal = static_cast<uint32_t>(s_int_bytes);
    portEXIT_CRITICAL(&s_mux);
}

/* xorshift32: weights only need to be finite and of a sensible size. */
float bench_rand(uint32_t *x)
{
    *x ^= *x << 13;
    *x ^= *x >> 17;
    *x ^= *x << 5;
    return static_cast<float>(*x >> 8) * (2.0f / 16777216.0f) - 1.0f;   // [-1, 1)
}

/* One network of the walk's shape, placed as layers_to_internal() would, timed. */
bool bench_one(uint16_t hsz, int bits, mpx_policy_bench_row_t *row)
{
    constexpr uint32_t caps_int = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t reserve = static_cast<size_t>(CONFIG_APP_POLICY_INTERNAL_RESERVE_KB) * 1024u;
    mpxp_header_t *h = static_cast<mpxp_header_t *>(heap_caps_calloc(1, sizeof(mpxp_header_t), caps_int));
    if (!h) return false;
    h->n_layers = 4;
    const uint32_t dims[5] = { POL_OBS, hsz, hsz, static_cast<uint32_t>(hsz / 2), POL_ACT };
    std::memcpy(h->dims, dims, sizeof(dims));
    h->activation = MPXP_ACT_ELU;
    pol_net_t net = {};
    net.h = h;
    size_t sz[4];
    void *blk[4] = {};
    bool done[4] = {};
    row->hidden = hsz;
    row->params = 0;
    row->bytes = 0;
    row->bytes_internal = 0;
    for (int l = 0; l < 4; ++l) {
        const int in = static_cast<int>(dims[l]), o = static_cast<int>(dims[l + 1]);
        sz[l] = bits == 32 ? sizeof(float) * (static_cast<size_t>(in) + 1u) * static_cast<size_t>(o)
                           : pol_quant_layer_size(in, o, bits);
        row->bytes += static_cast<uint32_t>(sz[l]);
        row->params += static_cast<uint32_t>((dims[l] + 1u) * dims[l + 1]);
        net.max_width = std::max(net.max_width, static_cast<int>(std::max(dims[l], dims[l + 1])));
    }
    bool ok = true;
    for (int k = 0; k < 4 && ok; ++k) {
        int best = -1;
        for (int l = 0; l < 4; ++l)
            if (!done[l] && (best < 0 || sz[l] > sz[best])) best = l;
        done[best] = true;
        if (reserve > 0 && heap_caps_get_free_size(caps_int) >= sz[best] + reserve &&
            heap_caps_get_largest_free_block(caps_int) >= sz[best] + 16) {
            blk[best] = heap_caps_aligned_alloc(4, sz[best], caps_int);
            if (blk[best]) row->bytes_internal += static_cast<uint32_t>(sz[best]);
        }
        if (!blk[best]) blk[best] = heap_caps_aligned_alloc(4, sz[best], MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ok = blk[best] != nullptr;
        if (!ok) row->fail_bytes = static_cast<uint32_t>(sz[best]);
    }
    float *work = ok ? static_cast<float *>(heap_caps_malloc(sizeof(float) * 2 * net.max_width, caps_int)) : nullptr;
    if (ok && !work) row->fail_bytes = static_cast<uint32_t>(sizeof(float) * 2 * net.max_width);
    ok = ok && work;
    // Taken after the network: the test's own buffer must not be what fails it.
    uint8_t *scrub = ok ? static_cast<uint8_t *>(heap_caps_malloc(BENCH_SCRUB, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))
                        : nullptr;
    row->flushed = scrub != nullptr;
    if (scrub) std::memset(scrub, 1, BENCH_SCRUB);
    if (ok) {
        uint32_t x = esp_random() | 1u;
        for (int l = 0; l < 4; ++l) {
            const size_t nw = static_cast<size_t>(dims[l]) * dims[l + 1];
            const float g = 1.0f / std::sqrt(static_cast<float>(dims[l]));
            if (bits == 32) {
                float *f = static_cast<float *>(blk[l]);
                for (size_t i = 0; i < nw; ++i) f[i] = g * bench_rand(&x);
                for (size_t i = 0; i < dims[l + 1]; ++i) f[nw + i] = 0.0f;
                net.W[l] = f;
                net.b[l] = f + nw;
                continue;
            }
            // integer weights spread over their range, scaled to the same size
            pol_use_quant_layer(&net, static_cast<uint32_t>(l), bits, blk[l]);
            const float M = bits == 16 ? 32767.0f : 127.0f;
            for (size_t i = 0; i < nw; ++i) {
                const float v = bench_rand(&x) * M;
                if (bits == 16) static_cast<int16_t *>(blk[l])[i] = static_cast<int16_t>(v);
                else            static_cast<int8_t *>(blk[l])[i] = static_cast<int8_t>(v);
            }
            float *sc = const_cast<float *>(net.sq[l]);
            for (size_t j = 0; j < dims[l + 1]; ++j) {
                sc[j] = g / M;
                sc[dims[l + 1] + j] = 0.0f;   // the bias, right after the scales
            }
        }
        float obs[POL_OBS], act[POL_ACT];
        for (float &o : obs) o = bench_rand(&x);
        int64_t total = 0, worst = 0;
        for (int k = 0; k < BENCH_RUNS; ++k) {
            // A real step finds the cache full of the other tasks' data.
            if (scrub) {
                const volatile uint8_t *v = scrub;
                uint32_t sum = 0;
                for (size_t i = 0; i < BENCH_SCRUB; i += 32) sum += v[i];
                (void)sum;
            }
            const int64_t t0 = esp_timer_get_time();
            pol_forward(&net, obs, act, work);
            const int64_t us = esp_timer_get_time() - t0;
            total += us;
            worst = std::max(worst, us);
            vTaskDelay(1);   // the idle task on this core, and the watchdog
        }
        row->us_avg = static_cast<float>(total) / BENCH_RUNS;
        row->us_max = static_cast<float>(worst);
        if (!std::isfinite(act[0])) ok = false;
    }
    row->ok = ok;
    if (scrub) heap_caps_free(scrub);
    if (work) heap_caps_free(work);
    for (void *p : blk)
        if (p) heap_caps_free(p);
    heap_caps_free(h);
    return ok;
}

void bench_task(void *)
{
    release_internal();
    portENTER_CRITICAL(&s_mux);
    s_bench.internal_free = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    s_bench.internal_largest = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    s_bench.internal_reserve = static_cast<uint32_t>(CONFIG_APP_POLICY_INTERNAL_RESERVE_KB) * 1024u;
    s_bench.psram_free = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    s_bench.psram_largest = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "size test: internal %lu KB free (largest block %lu), PSRAM %lu KB free (largest block %lu)",
             static_cast<unsigned long>(s_bench.internal_free / 1024),
             static_cast<unsigned long>(s_bench.internal_largest / 1024),
             static_cast<unsigned long>(s_bench.psram_free / 1024),
             static_cast<unsigned long>(s_bench.psram_largest / 1024));
    bool all_flushed = true;
    for (int i = 0; i < MPX_POLICY_BENCH_MAX; ++i) {
        mpx_policy_bench_row_t row = {};
        const bool ok = bench_one(BENCH_H[i], s_bench.bits, &row);
        if (ok && !row.flushed) all_flushed = false;
        portENTER_CRITICAL(&s_mux);
        s_bench.rows[i] = row;
        s_bench.n_rows = i + 1;
        portEXIT_CRITICAL(&s_mux);
        if (ok) {
            ESP_LOGI(TAG, "size test h=%u: %lu weights (%lu of %lu KB internal) avg %.0f us max %.0f us",
                     row.hidden, static_cast<unsigned long>(row.params),
                     static_cast<unsigned long>(row.bytes_internal / 1024),
                     static_cast<unsigned long>(row.bytes / 1024), static_cast<double>(row.us_avg),
                     static_cast<double>(row.us_max));
        } else {
            ESP_LOGW(TAG, "size test h=%u: %lu weights (%lu KB): no room for a %lu KB block",
                     row.hidden, static_cast<unsigned long>(row.params), static_cast<unsigned long>(row.bytes / 1024),
                     static_cast<unsigned long>(row.fail_bytes / 1024));
            break;
        }
    }
    restore_internal();
    portENTER_CRITICAL(&s_mux);
    if (!all_flushed) std::snprintf(s_bench.error, sizeof(s_bench.error), "some sizes ran without the cache flush");
    s_bench.running = false;
    s_bench.done = true;
    s_bench_busy = false;
    portEXIT_CRITICAL(&s_mux);
    vTaskDelete(nullptr);
}

}  // namespace

// ── after a logged run: the serial summary and the saved log ───────────

namespace {

struct Stat { float mean, p99, max; int n; };

Stat stat_of(float *v, int n)
{
    Stat s = { NAN, NAN, NAN, n };
    if (n <= 0) return s;
    double sum = 0.0;
    for (int i = 0; i < n; ++i) sum += v[i];
    std::sort(v, v + n);
    s.mean = static_cast<float>(sum / n);
    s.p99 = v[std::min(n - 1, static_cast<int>(n * 0.99f))];
    s.max = v[n - 1];
    return s;
}

/* The page's run summary, on the serial port: what a run without Wi-Fi (or
 * any run, with a cable) shows without the app. */
void log_summary()
{
    const size_t n = s_log_n;
    float *v = static_cast<float *>(heap_caps_malloc(sizeof(float) * (n + 1), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!v || n == 0) {
        if (v) heap_caps_free(v);
        return;
    }
    auto collect = [&](auto get, bool run_only, bool skip_first) {
        int k = 0;
        bool first = true;
        for (size_t i = 0; i < n; ++i) {
            const LogRec &r = s_log[i];
            if (run_only && r.phase != MPX_POLICY_RUN) continue;
            if (skip_first && first) {
                first = false;
                continue;
            }
            const float x = get(r);
            if (std::isfinite(x)) v[k++] = x;
        }
        return stat_of(v, k);
    };
    int walking = 0, late = 0, late_walk = 0;
    for (size_t i = 0; i < n; ++i) {
        const bool w = s_log[i].phase == MPX_POLICY_RUN;
        walking += w;
        late += s_log[i].flags & 1u;
        late_walk += w && (s_log[i].flags & 1u);
    }
    const Stat dt = collect([](const LogRec &r) { return r.dt_us / 1000.0f; }, true, true);
    const Stat tick = collect([](const LogRec &r) { return r.tick_us / 1000.0f; }, false, false);
    const Stat net = collect([](const LogRec &r) { return r.fwd_us / 1000.0f; }, true, false);
    const Stat ex = collect([](const LogRec &r) { return r.ex_us / 1000.0f; }, false, false);
    const Stat tilt = collect([](const LogRec &r) {
        return r.tilt_cdeg == INT16_MIN ? NAN : r.tilt_cdeg / 100.0f; }, true, false);
    const Stat temp = collect([](const LogRec &r) { return r.temp_dc / 10.0f; }, false, false);
    const Stat cur = collect([](const LogRec &r) { return static_cast<float>(r.cur_ma); }, false, false);
    const Stat trk = collect([](const LogRec &r) { return r.trk_max_cdeg / 100.0f; }, true, false);
    heap_caps_free(v);
    char stop[64];
    portENTER_CRITICAL(&s_mux);
    std::snprintf(stop, sizeof(stop), "%s", s_st.last_stop);
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "RL run log: %s, ended: %s", s_st.name, stop);
    ESP_LOGI(TAG, "  steps %u (walking %d), late ticks %d (walking %d)", static_cast<unsigned>(n), walking, late,
             late_walk);
    ESP_LOGI(TAG, "  step period walking: mean %.1f ms, p99 %.1f, max %.1f [20 ms]", dt.mean, dt.p99, dt.max);
    ESP_LOGI(TAG, "  tick: mean %.2f ms, p99 %.2f, max %.2f [5 ms] | network: mean %.2f ms, max %.2f | "
                  "servo exchange: mean %.2f ms, max %.2f", tick.mean, tick.p99, tick.max, net.mean, net.max,
             ex.mean, ex.max);
    ESP_LOGI(TAG, "  tilt walking: mean %.1f deg, max %.1f | tracking max p99 %.1f deg | hottest servo %.1f C | "
                  "max current %.0f mA", tilt.mean, tilt.max, trk.p99, temp.max, cur.max);
}

void save_log_file()
{
    FILE *f = std::fopen(LOG_FILE, "w");
    if (!f) {
        ESP_LOGW(TAG, "could not save the run log to %s", LOG_FILE);
        return;
    }
    std::fputs(mpx_policy_log_header(), f);
    std::fputc('\n', f);
    char line[200];
    size_t i = 0;
    for (; i < s_log_n; ++i) {
        if (!mpx_policy_log_row(i, line, sizeof(line))) break;
        std::fputs(line, f);
        std::fputc('\n', f);
    }
    std::fclose(f);
    ESP_LOGI(TAG, "run log saved: %s (%u steps)", LOG_FILE, static_cast<unsigned>(i));
}

void after_task(void *)
{
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_log && s_log_n > 0 && s_state == MPX_POLICY_IDLE) {
            log_summary();
            save_log_file();
        }
        s_saving = false;
    }
}

}  // namespace

extern "C" const char *mpx_policy_log_file(void)
{
    FILE *f = std::fopen(LOG_FILE, "r");
    if (!f) return nullptr;
    std::fclose(f);
    return LOG_FILE;
}

extern "C" bool mpx_policy_log_saving(void)
{
    return s_saving;
}

// ── public API ──────────────────────────────────────────────────────────

extern "C" esp_err_t mpx_policy_init(void)
{
    if (s_task) return ESP_OK;
    char why[64] = "";
    // Start with the first built-in walk trained for this robot (the header's
    // robot name), else the first one.
    for (int i = 0; i < N_BUILTINS; ++i) {
        const size_t len = static_cast<size_t>(BUILTINS[i].end - BUILTINS[i].start);
        if (len < sizeof(mpxp_header_t)) continue;
        mpxp_header_t hd;
        std::memcpy(&hd, BUILTINS[i].start, sizeof(hd));
        if (std::strncmp(hd.robot, CONFIG_APP_POLICY_ROBOT, sizeof(hd.robot)) == 0) {
            s_builtin_sel = i;
            break;
        }
    }
    const int bits = (CONFIG_APP_POLICY_WEIGHT_BITS == 8 || CONFIG_APP_POLICY_WEIGHT_BITS == 16) ?
                     CONFIG_APP_POLICY_WEIGHT_BITS : 32;
    if (policy_load_builtin_or_float(bits, why, sizeof(why)) != ESP_OK) {
        ESP_LOGE(TAG, "built-in policy refused: %s", why);
    }
    install_getup();

    const esp_timer_create_args_t ta = { .callback = tick_cb, .arg = nullptr,
                                         .dispatch_method = ESP_TIMER_TASK, .name = "policy",
                                         .skip_unhandled_events = true };
    if (esp_timer_create(&ta, &s_timer) != ESP_OK) return ESP_FAIL;
    // Core 1 with the gait (parked while a run is on), above everything but
    // the servo bus itself; the network has its own task just below.
    if (xTaskCreatePinnedToCore(tick_task, "policy", 6144, nullptr, 22, &s_task, 1) != pdPASS) {
        s_task = nullptr;
        return ESP_ERR_NO_MEM;
    }
    // The network: same core, just below the tick, so the 5 ms tick always
    // gets its turn and the network has the time between.
    if (xTaskCreatePinnedToCore(infer_task, "pol_net", 4096, nullptr, 21, &s_infer, 1) != pdPASS) {
        s_infer = nullptr;
        return ESP_ERR_NO_MEM;
    }
    // After a run: low priority, core 0 (the flash write must not hold up a tick).
    if (xTaskCreatePinnedToCore(after_task, "pol_after", 4096, nullptr, 3, &s_after, 0) != pdPASS) {
        s_after = nullptr;
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
    if (!force && !dry && std::strcmp(s_net.h->robot, CONFIG_APP_POLICY_ROBOT) != 0) {
        std::snprintf(why, why_len, "this policy is for %s, the robot is %s (add `force` to run it anyway)",
                      s_net.h->robot, CONFIG_APP_POLICY_ROBOT);
        return ESP_ERR_INVALID_ARG;
    }
    if (!std::isfinite(seconds) || !(seconds >= 0.0f) || seconds > 3600.0f || !std::isfinite(hold_s) ||
        !(hold_s >= 0.0f) || hold_s > 60.0f) {
        std::snprintf(why, why_len, "bad duration");
        return ESP_ERR_INVALID_ARG;
    }
    bool busy;
    portENTER_CRITICAL(&s_mux);
    busy = s_state != MPX_POLICY_IDLE || s_req_start || s_bench_busy || s_job.state.load() == JOB_RUNNING || s_saving;
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
        std::snprintf(why, why_len, "a run or the size test is on");
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
    if (!force && std::strcmp(s_gu_net.h->robot, CONFIG_APP_POLICY_ROBOT) != 0) {
        std::snprintf(why, why_len, "the get-up policy is for %s, the robot is %s (add `force` to run it anyway)",
                      s_gu_net.h->robot, CONFIG_APP_POLICY_ROBOT);
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_mux);
    const bool busy = s_state != MPX_POLICY_IDLE || s_req_start || s_bench_busy || s_job.state.load() == JOB_RUNNING;
    if (!busy) s_req_getup = true;
    portEXIT_CRITICAL(&s_mux);
    if (busy) {
        std::snprintf(why, why_len, "a run or the size test is on");
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
    return mpx_policy_selftest_full(max_err, nullptr, nullptr, nullptr, us_avg, us_max);
}

extern "C" bool mpx_policy_selftest_full(float *max_err, float *worst_deg, int *bits, bool *ok, float *us_avg,
                                         float *us_max)
{
    if (!s_net.h || !s_work) return false;
    portENTER_CRITICAL(&s_mux);
    const bool busy = s_state != MPX_POLICY_IDLE || s_req_start || s_bench_busy || s_job.state.load() == JOB_RUNNING;
    if (!busy) s_bench_busy = true;
    portEXIT_CRITICAL(&s_mux);
    if (busy) return false;
    const float e = pol_selftest(&s_net, s_work);
    float mean_deg = 0.0f;
    const float wdeg = pol_selftest_deg(&s_net, s_work, &mean_deg);
    float avg = -1.0f, worst = -1.0f;
    (void)policy_time_forward(200, &avg, &worst);
    portENTER_CRITICAL(&s_mux);
    s_bench_busy = false;
    portEXIT_CRITICAL(&s_mux);
    if (max_err) *max_err = e;
    if (worst_deg) *worst_deg = wdeg;
    if (bits) *bits = s_bits;
    if (ok) *ok = selftest_ok(s_bits, e, wdeg);
    if (us_avg) *us_avg = avg;
    if (us_max) *us_max = worst;
    return true;
}

extern "C" esp_err_t mpx_policy_bench_start(int bits, char *why, int why_len)
{
    if (bits != 32 && bits != 16 && bits != 8) {
        std::snprintf(why, why_len, "bits: 32, 16 or 8");
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_mux);
    const bool busy = s_state != MPX_POLICY_IDLE || s_req_start || s_bench_busy || s_job.state.load() == JOB_RUNNING;
    if (!busy) {
        s_bench_busy = true;
        s_bench = {};
        s_bench.running = true;
        s_bench.n_total = MPX_POLICY_BENCH_MAX;
        s_bench.bits = static_cast<uint8_t>(bits);
    }
    portEXIT_CRITICAL(&s_mux);
    if (busy) {
        std::snprintf(why, why_len, "a run or a test is on");
        return ESP_ERR_INVALID_STATE;
    }
    // The policy's core and priority: what a run would see.
    if (xTaskCreatePinnedToCore(bench_task, "pol_bench", 4096, nullptr, 22, nullptr, 1) != pdPASS) {
        portENTER_CRITICAL(&s_mux);
        s_bench.running = false;
        s_bench_busy = false;
        portEXIT_CRITICAL(&s_mux);
        std::snprintf(why, why_len, "out of memory");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "size test started (%d-bit weights)", bits);
    return ESP_OK;
}

extern "C" int mpx_policy_builtin_count(void)
{
    return N_BUILTINS;
}

extern "C" const char *mpx_policy_builtin_id(int i)
{
    return (i >= 0 && i < N_BUILTINS) ? BUILTINS[i].id : nullptr;
}

extern "C" const char *mpx_policy_builtin_current(void)
{
    return BUILTINS[s_builtin_sel].id;
}

extern "C" esp_err_t mpx_policy_select(const char *id, char *why, int why_len)
{
    int k = -1;
    for (int i = 0; i < N_BUILTINS; ++i)
        if (id && std::strcmp(id, BUILTINS[i].id) == 0) k = i;
    if (k < 0) {
        std::snprintf(why, why_len, "no built-in policy '%s'", id ? id : "");
        return ESP_ERR_NOT_FOUND;
    }
    portENTER_CRITICAL(&s_mux);
    const bool busy = s_state != MPX_POLICY_IDLE || s_req_start || s_bench_busy || s_job.state.load() == JOB_RUNNING;
    if (!busy) s_bench_busy = true;
    portEXIT_CRITICAL(&s_mux);
    if (busy) {
        std::snprintf(why, why_len, "a run or a test is on");
        return ESP_ERR_INVALID_STATE;
    }
    const int old = s_builtin_sel;
    s_builtin_sel = k;
    // install() keeps the old policy until the new one is placed and self-tested.
    const esp_err_t e = policy_load_builtin(s_bits, why, why_len);
    if (e != ESP_OK) s_builtin_sel = old;
    else ESP_LOGI(TAG, "built-in policy now '%s'", id);
    portENTER_CRITICAL(&s_mux);
    s_bench_busy = false;
    portEXIT_CRITICAL(&s_mux);
    return e;
}

extern "C" esp_err_t mpx_policy_set_weight_bits(int bits, char *why, int why_len)
{
    if (bits != 32 && bits != 16 && bits != 8) {
        std::snprintf(why, why_len, "bits: 32, 16 or 8");
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_mux);
    const bool busy = s_state != MPX_POLICY_IDLE || s_req_start || s_bench_busy || s_job.state.load() == JOB_RUNNING;
    if (!busy) s_bench_busy = true;
    portEXIT_CRITICAL(&s_mux);
    if (busy) {
        std::snprintf(why, why_len, "a run or a test is on");
        return ESP_ERR_INVALID_STATE;
    }
    // The old copy stays until the new one is placed and self-tested
    // (install()), so both fit or the old one stays.
    esp_err_t e = policy_load_builtin(bits, why, why_len);
    if (e == ESP_OK) ESP_LOGI(TAG, "weights now %d-bit", bits);
    portENTER_CRITICAL(&s_mux);
    s_bench_busy = false;
    portEXIT_CRITICAL(&s_mux);
    return e;
}

extern "C" void mpx_policy_bench_get(mpx_policy_bench_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_bench;
    portEXIT_CRITICAL(&s_mux);
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
    s_use_robot_offsets = own;
    portENTER_CRITICAL(&s_mux);
    s_st.robot_offsets = own;
    portEXIT_CRITICAL(&s_mux);
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
    constexpr uint32_t caps_int = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    out->ram_free = static_cast<uint32_t>(heap_caps_get_free_size(caps_int));
    out->ram_min_free = static_cast<uint32_t>(heap_caps_get_minimum_free_size(caps_int));
    out->ram_largest = static_cast<uint32_t>(heap_caps_get_largest_free_block(caps_int));
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
