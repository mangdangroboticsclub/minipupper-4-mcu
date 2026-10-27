// ═══════════════════════════════════════════════════════════════════════
//  minipupperesp moves
//
//  Included into robot.cc (inside namespace robot) so these can use the same
//  goal buffer, offsets, config and gait-command word as every other move.
//
//  minipupperesp (C:\esp6\projects\minipupperesp, main/main.c) is the build
//  whose movement is verified on this robot. Everything here is a port of its
//  code, kept as close to line-for-line as the different plumbing allows:
//
//      minipupperesp            here
//      fRIK/fLIK/rRIK/rLIK      front_right_ik / front_left_ik / rear_*_ik
//      servo_write(ch, deg)     set_servo_angle(ch, deg)
//      servo_flush()            flush()   (both pace the bus to 5 ms)
//      offset[], height, ...    s_offset[], s_cfg.height, ...
//      int flag (Jump, ...)     s_gait_cmd == cmd  (still(cmd))
//      look_write()             sg_attitude() + sg_write() (same rotation)
//
//  The moves that already existed here (advance, back, left, right, turns,
//  step, roll, pitch, stretch, jumpfwd, testspeed) were compared call by call
//  against main.c and are identical, so they were left alone. Twerk and Jump
//  were older versions and are replaced below.
// ═══════════════════════════════════════════════════════════════════════

namespace {

inline bool still(GaitCmd cmd) { return s_gait_cmd == cmd; }

// End a one-shot move - unless something else was commanded meanwhile.
inline void finish(GaitCmd cmd)
{
    if (s_gait_cmd == cmd) s_gait_cmd = GaitCmd::None;
}

inline void legs_all(float x, float z)
{
    front_right_ik(x, 0, z); front_left_ik(x, 0, z);
    rear_right_ik(x, 0, z);  rear_left_ik(x, 0, z);
}

// A servo's measured position in the GAIT frame (what s_goal_pos holds) -
// minipupperesp's teach_cmd_from_feedback(). See at32_raw_to_gait_raw().
inline int fb_gait_raw(int id)
{
    return at32_raw_to_gait_raw(static_cast<int>(driver_board_present_position(id)));
}

// Ease from wherever the servos are now to the IK stand at s_cfg.height.
// Table moves end on the calibrated Ini pose (all servos at their offset),
// which equals the IK stand only at NEUTRAL_Z; this hides the difference.
void ramp_to_stand(int ms)
{
    uint16_t from[13], to[13];
    std::memcpy(from, s_goal_pos, sizeof(from));
    legs_all(0.0f, static_cast<float>(s_cfg.height));
    std::memcpy(to, s_goal_pos, sizeof(to));
    if (ms < 1) ms = 1;
    const uint32_t t0 = millis();
    for (;;) {
        uint32_t el = millis() - t0;
        if (el > static_cast<uint32_t>(ms)) el = ms;
        const float u = 0.5f - 0.5f * std::cos(PI * static_cast<float>(el) / static_cast<float>(ms));
        for (int i = 1; i <= 12; ++i) {
            s_goal_pos[i] = static_cast<uint16_t>(std::lrintf(from[i] + (to[i] - from[i]) * u));
        }
        flush();
        if (el >= static_cast<uint32_t>(ms)) break;
    }
}

// ── Servo gain presets (main.c tune_presets / gains_push / gains_pop) ──────
//
// The spin jumps, the running man / moonwalk and the backflips were planned
// on the "Strong" tuning; they switch to it for the move and put the gains
// the servos had back afterwards. Needs AT32 firmware that understands the
// gain parameters; an older board just refuses the writes and the move runs
// on whatever gains it has.
struct TunePreset {
    const char *name;
    float kp_position, kd_position, kp_current, kff_current, max_pwm;
    float ki_position, control_mode, pwm_scale;
};
constexpr TunePreset TUNE_PRESETS[] = {
    { "Strong (backflip)",
      120.0f, 1400.0f, 0.00006f, 0.0011f,  0.99f, 0.0f, DB_CONTROL_MODE_CURRENT, 0.001f },
    { "Normal (walk / demo)",
       80.0f,  800.0f, 0.0006f,  0.00035f, 0.99f, 0.0f, DB_CONTROL_MODE_CURRENT, 0.001f },
    { "PWM (silent)",
      100.0f,  600.0f, 0.0006f,  0.00035f, 0.99f, 0.0f, DB_CONTROL_MODE_PWM,     0.001f },
};
constexpr int TUNE_NPRESETS = sizeof(TUNE_PRESETS) / sizeof(TUNE_PRESETS[0]);
constexpr int TUNE_NP = 8;
constexpr int TUNE_PIDS[TUNE_NP] = {
    DB_PARAM_KP_POSITION, DB_PARAM_KD_POSITION, DB_PARAM_KP_CURRENT, DB_PARAM_KFF_CURRENT,
    DB_PARAM_MAX_PWM_DUTY_CYCLE, DB_PARAM_KI_POSITION, DB_PARAM_CONTROL_MODE, DB_PARAM_PWM_SCALE,
};
volatile int s_tune_active = -1;          // -1 = the boards' own (power-on) gains
float s_gp_saved[13][TUNE_NP];
int   s_gp_depth = 0, s_gp_prev = -1, s_gp_hw = 0;

void tune_read_all(float g[13][TUNE_NP])
{
    for (int id = 1; id <= 12; ++id)
        for (int p = 0; p < TUNE_NP; ++p) {
            g[id][p] = NAN;
            driver_board_get_param(id, TUNE_PIDS[p], &g[id][p]);
        }
}

void tune_write_all(float g[13][TUNE_NP])
{
    for (int id = 1; id <= 12; ++id)
        for (int p = 0; p < TUNE_NP; ++p)
            if (!std::isnan(g[id][p])) driver_board_set_param(id, TUNE_PIDS[p], g[id][p]);
}

void tune_write_preset(int k)
{
    const TunePreset &t = TUNE_PRESETS[k];
    const float val[TUNE_NP] = { t.kp_position, t.kd_position, t.kp_current, t.kff_current,
                                 t.max_pwm, t.ki_position, t.control_mode, t.pwm_scale };
    for (int id = 1; id <= 12; ++id)
        for (int p = 0; p < TUNE_NP; ++p)
            driver_board_set_param(id, TUNE_PIDS[p], val[p]);
}

// Preset k for one move. Not nested: a second push is ignored. Gait task only.
// (minipupperesp also accepts k = -1, "the power-on gains read at boot"; no
// table here asks for that, so it is not kept.)
bool gains_push(int k)
{
    if (s_gp_depth || k < 0 || k >= TUNE_NPRESETS) return false;
    s_gp_depth = 1;
    s_gp_prev  = s_tune_active;
    s_gp_hw    = 0;
    if (s_tune_active != k) {
        tune_read_all(s_gp_saved);
        tune_write_preset(k);
        s_gp_hw = 1;
    }
    s_tune_active = k;
    ESP_LOGI(TAG, "servos on \"%s\" for this move", TUNE_PRESETS[k].name);
    return true;
}

void gains_pop()
{
    if (!s_gp_depth) return;
    s_gp_depth = 0;
    if (s_gp_hw) tune_write_all(s_gp_saved);
    s_tune_active = s_gp_prev;
    ESP_LOGI(TAG, "servo gains restored");
}

// ── Jump (main.c, "crouch -> push -> snap back to the crouch") ─────────────
constexpr float JUMP_CROUCH_Z   = 40.0f;   // first (loaded) crouch AND the in-air pose
constexpr float JUMP_PUSH_Z     = 105.0f;  // full extension
constexpr uint32_t JUMP_EXT_MIN_MS = 40;   // always push at least this long
constexpr uint32_t JUMP_EXT_MAX_MS = 180;  // give up waiting for full extension here
constexpr int   JUMP_EXT_TOL    = 25;      // raw units: "close enough" to full extension
constexpr uint32_t JUMP_AIR_MS  = 250;     // hold the crouch through flight + landing

// True when every calf (3, 6, 9, 12) is within tol of its goal. Feedback is
// refreshed by every flush().
bool jump_legs_extended(int tol)
{
    static const int calves[4] = { 3, 6, 9, 12 };
    for (int k = 0; k < 4; ++k) {
        const int id = calves[k];
        int err = fb_gait_raw(id) - static_cast<int>(s_goal_pos[id]);
        if (err < 0) err = -err;
        if (err > tol) return false;
    }
    return true;
}

// ── Long Jump / Far Jump (lj_play) ─────────────────────────────────────────
int s_lj_speed = 70;   // % for the slow parts; NVS "ljspeed"

void lj_clamp_to(float &x, float &z, float reach)
{
    const float ld = std::sqrt(x * x + z * z);
    if (ld > reach) { const float k = reach / ld; x *= k; z *= k; }
    if (ld < 12.0f) z = 12.0f;
}
void lj_front_r(float x, float z, float r) { lj_clamp_to(x, z, r); front_right_ik(x, 0, z); front_left_ik(x, 0, z); }
void lj_rear_r (float x, float z, float r) { lj_clamp_to(x, z, r); rear_right_ik(x, 0, z);  rear_left_ik(x, 0, z); }

void lj_track_at(const lj_seg_t *s, int n, float x0, float z0, float t, float &x, float &z)
{
    float X = x0, Z = z0;
    for (int i = 0; i < n; ++i) {
        if (t < static_cast<float>(s[i].t0)) break;
        float u = s[i].dur <= 0 ? 1.0f : (t - static_cast<float>(s[i].t0)) / static_cast<float>(s[i].dur);
        if (u > 1.0f) u = 1.0f;
        if (s[i].ease) u = 0.5f - 0.5f * std::cos(PI * u);
        X += (s[i].x - X) * u;
        Z += (s[i].z - Z) * u;
        if (t < static_cast<float>(s[i].t0 + s[i].dur)) break;
    }
    x = X; z = Z;
}

// real ms since the start -> table ms. The speed scales the slow parts only;
// fast0..fast1 (the jump itself) always runs at the speed the sim tuned.
float lj_table_time(float t, const lj_jump_t *j, float sp)
{
    const float a = static_cast<float>(j->fast0) / sp;
    if (t < a) return t * sp;
    const float b = a + static_cast<float>(j->fast1 - j->fast0);
    if (t < b) return static_cast<float>(j->fast0) + (t - a);
    return static_cast<float>(j->fast1) + (t - b) * sp;
}

void lj_play(const lj_jump_t *j, GaitCmd cmd)
{
    const float sp = static_cast<float>(s_lj_speed > 0 ? s_lj_speed : 100) / 100.0f;
    const float h = static_cast<float>(s_cfg.height);
    const float r = (j->reach > 12.0f && j->reach < 105.6f) ? j->reach : 103.0f;
    float fx, fz, rx, rz;
    set_all_servo_speed(0);
    uint32_t t0 = millis();
    for (;;) {
        const float tt = lj_table_time(static_cast<float>(millis() - t0), j, sp);
        if (tt >= static_cast<float>(j->end) || !still(cmd)) break;
        lj_track_at(j->front, j->nfront, 0.0f, h, tt, fx, fz);
        lj_track_at(j->rear,  j->nrear,  0.0f, h, tt, rx, rz);
        lj_front_r(fx, fz, r); lj_rear_r(rx, rz, r);
        flush();
    }
    if (!still(cmd)) return;
    lj_track_at(j->front, j->nfront, 0.0f, h, static_cast<float>(j->end), fx, fz);
    lj_track_at(j->rear,  j->nrear,  0.0f, h, static_cast<float>(j->end), rx, rz);
    int t_rise = static_cast<int>(static_cast<float>(j->rise) / sp);
    if (t_rise < 1) t_rise = 1;
    t0 = millis();
    for (;;) {
        uint32_t el = millis() - t0;
        if (el > static_cast<uint32_t>(t_rise)) el = t_rise;
        const float u = 0.5f - 0.5f * std::cos(PI * static_cast<float>(el) / static_cast<float>(t_rise));
        lj_front_r(fx * (1.0f - u), fz + (h - fz) * u, r);
        lj_rear_r (rx * (1.0f - u), rz + (h - rz) * u, r);
        flush();
        if (el >= static_cast<uint32_t>(t_rise) || !still(cmd)) break;
    }
}

// ── TrajOpt tables + the Character moves (tj_play) ─────────────────────────
//
// One row of 12 servo angles x10 (deg, 0 = the stand, calibration NOT
// included) per frame_ms; the player blends between rows and picks the row
// by the clock, so a late flush does not stretch the move.
constexpr float TJ_GUARD_DEG = 45.0f;   // tip-over guard: stop past this much pitch / roll

void tj_play(int k, GaitCmd cmd)
{
    if (k < 0 || k >= TJ_NTABLES) return;
    const tj_table_t *T = &TJ_TABLES[k];
    const int fms = T->frame_ms > 0 ? T->frame_ms : TJ_FRAME_MS;
    ESP_LOGI(TAG, "trajopt '%s': %d frames x %d ms", T->name, T->frames, fms);
    // Inside the Character show the command stays "charshow"; name each
    // move so the face plays that move's track.
    if (cmd == GaitCmd::CharShow) play_mark(T->name);

    // tune_preset: 0.. = TUNE_PRESETS, <= -1 = leave the gains alone
    const bool pushed = (T->tune_preset >= 0) && gains_push(T->tune_preset);
    set_all_servo_speed(0);

    // TIP-OVER GUARD (minipupperesp: "after the gallop reared up and flipped
    // over backwards on the real robot"). The jumps only - the Character moves
    // sit, roll and rear up on purpose. Integrate the gyro's x and y rates;
    // past TJ_GUARD_DEG either way, stop and go to the stand. There it was on
    // only while a recording ran; here it is always on for the jumps.
    const bool guard = !T->character;
    float g_ang[2] = { 0.0f, 0.0f };
    uint32_t g_seq = 0;
    int64_t  g_t = 0;
    bool aborted = false;

    const uint32_t t0 = millis();
    uint32_t last = t0, worst = 0;
    int sent = 0;
    while (still(cmd)) {
        if (guard) {
            const ImuData d = imu_read();
            if (d.seq != 0 && d.seq != g_seq) {
                if (g_seq != 0) {
                    float dt = static_cast<float>(d.t_us - g_t) * 1e-6f;
                    if (dt > 0.05f) dt = 0.05f;
                    if (dt < 0.0f)  dt = 0.0f;
                    g_ang[0] += d.gx * dt;
                    g_ang[1] += d.gy * dt;
                }
                g_seq = d.seq;
                g_t = d.t_us;
                if (std::fabs(g_ang[0]) > TJ_GUARD_DEG || std::fabs(g_ang[1]) > TJ_GUARD_DEG) {
                    aborted = true;
                    ESP_LOGW(TAG, "trajopt: TIPPING (%.0f / %.0f deg) at %lu ms - stopped, back to the stand",
                             g_ang[0], g_ang[1], static_cast<unsigned long>(millis() - t0));
                    break;
                }
            }
        }
        const uint32_t el = millis() - t0;
        const int f = static_cast<int>(el / fms);
        if (f >= T->frames) break;
        const int f2 = (f + 1 < T->frames) ? f + 1 : f;
        const float u = static_cast<float>(el % fms) / static_cast<float>(fms);
        for (int ch = 1; ch <= 12; ++ch) {
            const float a = T->deg10[f][ch - 1], b = T->deg10[f2][ch - 1];
            set_servo_angle(ch, (a + u * (b - a)) / 10.0f + s_offset[ch]);
        }
        flush();
        const uint32_t now = millis();
        if (now - last > worst) worst = now - last;
        last = now;
        ++sent;
    }
    for (int ch = 1; ch <= 12; ++ch)   // end exactly on the last row (the stand)
        set_servo_angle(ch, static_cast<float>(T->deg10[T->frames - 1][ch - 1]) / 10.0f + s_offset[ch]);
    flush();
    if (aborted) vTaskDelay(pdMS_TO_TICKS(600));   // let it land before the gains go back
    ESP_LOGI(TAG, "trajopt '%s': %d of %d rows in %lu ms, longest gap %lu ms",
             T->name, sent, T->frames, static_cast<unsigned long>(millis() - t0),
             static_cast<unsigned long>(worst));
    vTaskDelay(pdMS_TO_TICKS(200));
    if (pushed) gains_pop();
    if (cmd == GaitCmd::CharShow) play_mark("charshow");
}

// The Character section's show: every dog move in its .show order.
void char_show(GaitCmd cmd)
{
    for (int s = 0; s < TJ_NTABLES && still(cmd); ++s) {
        for (int k = 0; k < TJ_NTABLES && still(cmd); ++k) {
            if (!TJ_TABLES[k].character || TJ_TABLES[k].show != s) continue;
            tj_play(k, cmd);
            for (int w = 0; w < 30 && still(cmd); ++w) vTaskDelay(pdMS_TO_TICKS(20));   // 0.6 s breath
        }
    }
}

// ── Backflip / frame presets (main.c bf_presets + the backflip player) ─────
//
// Each preset is a reference pose plus per-frame deltas in raw servo ticks,
// with a move / delay / blend time per frame - the format minipupperesp's
// teach mode records.
//
// The reference pose was that robot's calibrated stand at the time. Here the
// frames are RE-BASED onto THIS robot's stand (511 + offset), which is what
// minipupperesp's "Re-anchor to the live pose" button does: the motion is the
// deltas, so it survives a re-calibration. Played on the Strong gains, like
// minipupperesp's bf_auto_strong.
//
// Backflips are hard on the robot. Run them on a soft floor, with a charged
// battery, after the robot walks correctly - and expect to tune.
/* Trial 1 - first sequence that completed 360 deg and landed on its feet.
 * 3 rocks at ~3.6 Hz to wind up, a 190 ms launch (full 206 deg extension),
 * then a 70 deg front-calf tuck while the rear stays extended. */
static const uint16_t BF_T1_REF[13] =
    { 0, 424, 514, 658, 495, 505, 366, 502, 591, 453, 500, 510, 606 };
static const int16_t BF_T1_DELTA[10][13] = {
    {0,   0,  104,  -14,   0,  -99,   29,   0, -126, -164,   0,  100,  152},
    {0,   0,   -9,    7,   0,   11,  -37,   0, -143, -162,   0,   98,  150},
    {0,   0,  104,  -14,   0,  -99,   29,   0, -126, -164,   0,  100,  152},
    {0,   0,  -25,   56,   0,   48,  -65,   0, -137, -171,   0,   98,  155},
    {0,   0,  104,  -14,   0,  -99,   29,   0, -126, -164,   0,  100,  152},
    {0,   0, -151,  114,   0,  132, -134,   0, -126, -164,   0,  100,  152},
    {0,   0, -151,  114,   0,  132, -134,   0, -142,   84,   0,  121, -135},
    {0,   0, -411, -568,   0,  377,  548,   0,  195,  391,   0, -189, -435},
    {0,   0, -398, -336,   0,  390,  321,   0,  195,  391,   0, -189, -435},
    {0,   0,    0,    0,   0,    0,    0,   0,    0,    0,   0,    0,    0},
};
static const uint16_t BF_T1_MOVE [11] = { 1000, 600, 100, 40, 130, 40, 40, 30,  50,  80, 600 };
static const uint16_t BF_T1_DELAY[11] = {  500, 300,  60, 80,  70,110, 10, 40, 140, 100, 300 };
static const uint8_t  BF_T1_BLEND[11] = {  100, 100, 100,100,  80, 10, 80,100, 100, 100, 100 };

/* Trial 2 - shorter and more repeatable than trial 1: no triple rock, one
 * load frame, a 190 ms launch, then the REAR tucks while the front stays
 * extended (the mirror of trial 1, and it holds better).
 * Left/right SYMMETRISED: every pair's deltas are exactly equal and opposite,
 * so the flip stays in the sagittal plane instead of yawing. The reference
 * pose is untouched - that is the robot's calibrated stance.
 * Every frame also completes at 1200 deg/s, including the 175 deg rear tuck
 * (170 ms allowed, 146 needed). */
static const uint16_t BF_T2_REF[13] =
    { 0, 425, 514, 658, 495, 507, 367, 504, 590, 455, 499, 512, 603 };
static const int16_t BF_T2_DELTA[7][13] = {
    {0,   0, -101, -244,   0,  101,  244,   0, -152,  -38,   0,  152,   38},
    {0,   0, -214,   16,   0,  214,  -16,   0, -152,  -38,   0,  152,   38},
    {0,   0, -214,   16,   0,  214,  -16,   0, -172,   83,   0,  172,  -83},
    {0,   0, -318, -474,   0,  318,  474,   0,  192,  431,   0, -192, -431},
    {0,   0, -322, -470,   0,  322,  470,   0,    0, -148,   0,    0,  148},
    {0,   0, -331, -388,   0,  331,  388,   0,   -3, -140,   0,    3,  140},
    {0,   0,    0,    0,   0,    0,    0,   0,    0,    0,   0,    0,    0},
};
static const uint16_t BF_T2_MOVE [8] = { 1000, 600, 30, 30,  50,  60,  50, 600 };
static const uint16_t BF_T2_DELAY[8] = {  500, 300, 45, 30, 140, 110, 300, 300 };
static const uint8_t  BF_T2_BLEND[8] = {  100, 100,100,100, 100, 100, 100, 100 };

/* The version before symmetrising, kept because it is the one with a proven
 * landing. Delete this block and its bf_presets[] row once the symmetric one
 * has as many good runs. */
static const int16_t BF_T3_DELTA[7][13] = {
    {0,   0, -110, -243,   0,   92,  246,   0, -150,  -47,   0,  154,   29},
    {0,   0, -239,   -2,   0,  190,  -35,   0, -150,  -47,   0,  154,   30},
    {0,   0, -239,   -2,   0,  190,  -34,   0, -190,   65,   0,  155, -101},
    {0,   0, -328, -490,   0,  309,  457,   0,  195,  423,   0, -189, -439},
    {0,   0, -330, -486,   0,  313,  453,   0,   -9, -142,   0,  -10,  153},
    {0,   0, -349, -403,   0,  313,  374,   0,  -16, -133,   0,  -10,  148},
    {0,   0,    0,    0,   0,    0,    0,   0,    0,    0,   0,    0,    0},
};
static const uint16_t BF_T3_MOVE [8] = { 1000, 600, 30, 30,  50, 60, 600, 600 };
static const uint16_t BF_T3_DELAY[8] = {  500, 300, 40, 30, 140, 30, 300, 300 };
static const uint8_t  BF_T3_BLEND[8] = {  100, 100, 60,100, 100,100, 100, 100 };

/* Trial 4 "clean" - 7 frames, one frame shorter than "stable" and 345 ms
 * quicker. Left/right symmetrised the same way: every pair's deltas are
 * exactly equal and opposite, reference pose untouched. Every frame
 * completes at 1200 deg/s, the tightest being the 166 deg rear tuck
 * (170 ms allowed, 139 needed). */
static const uint16_t BF_T4_REF[13] =
    { 0, 426, 493, 661, 492, 510, 364, 503, 586, 458, 502, 513, 600 };
static const int16_t BF_T4_DELTA[6][13] = {
    {0,   0,  -18, -209,   0,   18,  209,   0, -178,  -75,   0,  178,   75},
    {0,   0, -152,   66,   0,  152,  -66,   0, -177,  -78,   0,  177,   78},
    {0,   0, -152,   66,   0,  152,  -66,   0, -191,   41,   0,  191,  -41},
    {0,   0, -283, -462,   0,  283,  462,   0,  234,  452,   0, -234, -452},
    {0,   0, -286, -459,   0,  286,  459,   0,   60,  -97,   0,  -60,   97},
    {0,   0,    0,    0,   0,    0,    0,   0,    0,    0,   0,    0,    0},
};
static const uint16_t BF_T4_MOVE [7] = { 1000, 600, 30, 30,  50,  60, 600 };
static const uint16_t BF_T4_DELAY[7] = {  500, 300, 60, 30, 140, 110, 300 };
static const uint8_t  BF_T4_BLEND[7] = {  100, 100,100,100, 100, 100, 100 };

/* Backflip clean OPT - made by sim/bf_opt.py from "Backflip clean": same wind-up,
 * the load / launch / tuck frames re-tuned with CMA-ES in the MuJoCo sim to
 * turn further before touching down. sim: lands at 264 deg (was 250)
 * Left/right mirrored, hips untouched, each joint within +-70 ticks of the
 * original. */
static const uint16_t BF_T5_REF[13] =
    {    0,  426,  493,  661,  492,  510,  364,  503,  586,  458,  502,  513,  600 };
static const int16_t BF_T5_DELTA[6][13] = {
    {   0,    0,  -18, -209,    0,   18,  209,    0, -178,  -75,    0,  178,   75},
    {   0,    0, -154,   54,    0,  154,  -54,    0, -172,  -68,    0,  172,   68},
    {   0,    0, -141,   61,    0,  141,  -61,    0, -235,   62,    0,  235,  -62},
    {   0,    0, -290, -460,    0,  290,  460,    0,  289,  418,    0, -289, -418},
    {   0,    0, -250, -451,    0,  250,  451,    0,   37,  -97,    0,  -37,   97},
    {   0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0},
};
static const uint16_t BF_T5_MOVE [7] = { 1000,  600,   31,   25,   72,   68,  600 };
static const uint16_t BF_T5_DELAY[7] = {  500,  300,   45,   28,  184,  107,  300 };
static const uint8_t  BF_T5_BLEND[7] = {  100,  100,  100,  100,  100,  100,  100 };

/* Backflip stable OPT - made by sim/bf_opt.py from "Backflip stable": same wind-up,
 * the load / launch / tuck frames re-tuned with CMA-ES in the MuJoCo sim to
 * turn further before touching down. sim: lands at 282 deg (was 265)
 * Left/right mirrored, hips untouched, each joint within +-70 ticks of the
 * original. */
static const uint16_t BF_T6_REF[13] =
    {    0,  425,  514,  658,  495,  507,  367,  504,  590,  455,  499,  512,  603 };
static const int16_t BF_T6_DELTA[7][13] = {
    {   0,    0, -101, -244,    0,  101,  244,    0, -152,  -38,    0,  152,   38},
    {   0,    0, -230,   26,    0,  230,  -26,    0, -185,  -94,    0,  185,   94},
    {   0,    0, -223,   17,    0,  223,  -17,    0, -239,  126,    0,  239, -126},
    {   0,    0, -323, -492,    0,  323,  492,    0,  188,  475,    0, -188, -475},
    {   0,    0, -350, -430,    0,  350,  430,    0,   23, -147,    0,  -23,  147},
    {   0,    0, -329, -377,    0,  329,  377,    0,  -24, -145,    0,   24,  145},
    {   0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0},
};
static const uint16_t BF_T6_MOVE [8] = { 1000,  600,   20,   22,   57,   66,   68,  600 };
static const uint16_t BF_T6_DELAY[8] = {  500,  300,   77,   23,  215,  129,  431,  300 };
static const uint8_t  BF_T6_BLEND[8] = {  100,  100,  100,  100,  100,  100,  100,  100 };

/* Long Jump 1-hop (Lite 3) - sim/clean_opt.py + lj2bf3.py + bf3_tune.py.
 * The Lite 3 jump (your bow, front lifts first, rear leg pushes out straight)
 * re-optimized so it lands ONCE: the old table landed at ~10 cm and bounced
 * into a second hop. Hips fixed, L/R mirrored. REF = the stand of your
 * 2026-09-27 teach export. sim: one flight ~100 ms, 16-20 cm, lands upright
 * on 5/5 test robots (weak servos, slippery floor, heavier, late servos). */
static const uint16_t BF_T7_REF[13] =
    {    0,  531,  531,  652,  493,  529,  327,  487,  474,  387,  474,  556,  581 };
static const int16_t BF_T7_DELTA[19][13] = {
    {    0,    0,   42,  -59,    0,  -42,   59,    0,  -92,  -63,    0,   92,   63},
    {    0,    0,   55, -131,    0,  -55,  131,    0, -199,  -79,    0,  199,   79},
    {    0,    0,   55, -131,    0,  -55,  131,    0, -199,  -79,    0,  199,   79},
    {    0,    0,   48,  -87,    0,  -48,   87,    0,  -85, -109,    0,   85,  109},
    {    0,    0,   29,  -51,    0,  -29,   51,    0,   46,  -70,    0,  -46,   70},
    {    0,    0,   63,   79,    0,  -63,  -79,    0,   46,  -70,    0,  -46,   70},
    {    0,    0,   63,   79,    0,  -63,  -79,    0,  118,   40,    0, -118,  -40},
    {    0,    0,  -23,   31,    0,   23,  -31,    0,  129,  131,    0, -129, -131},
    {    0,    0, -125,   25,    0,  125,  -25,    0,   92,  219,    0,  -92, -219},
    {    0,    0, -125,   25,    0,  125,  -25,    0,   21,  305,    0,  -21, -305},
    {    0,    0, -125,   25,    0,  125,  -25,    0,   21,  305,    0,  -21, -305},
    {    0,    0, -125,   25,    0,  125,  -25,    0,   68,  164,    0,  -68, -164},
    {    0,    0, -125,   25,    0,  125,  -25,    0,   23,   71,    0,  -23,  -71},
    {    0,    0, -125,   25,    0,  125,  -25,    0,   23,   71,    0,  -23,  -71},
    {    0,    0, -101,   53,    0,  101,  -53,    0,  -25,   23,    0,   25,  -23},
    {    0,    0,  -85,   88,    0,   85,  -88,    0,  -85,  -10,    0,   85,   10},
    {    0,    0,  -22,   17,    0,   22,  -17,    0,  -20,  -18,    0,   20,   18},
    {    0,    0,   15,  -39,    0,  -15,   39,    0,   48,   -6,    0,  -48,    6},
    {    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0},
};
static const uint16_t BF_T7_MOVE [20] = { 1000,  175,  175,  800,  128,  155,   92,   51,   43,   34,   16,  113,   46,   51,   70,   63,   41,  123,  115,  700 };
static const uint16_t BF_T7_DELAY[20] = {  500,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,  500,    0 };
static const uint8_t  BF_T7_BLEND[20] = {  100,  100,  100,  100,  100,  100,  100,  100,  100,  100,  100,  100,  100,  100,  100,  100,  100,  100,  100,  100 };

/* Long Jump teach OPT - your taught Lite 3 frames (2026-09-27) made to jump
 * by sim/teach_opt.py: hips fixed, L/R mirrored, move/delay/blend AND the
 * angles re-tuned (timing alone cannot make it fly: frame 3 lifts the rear
 * and tips it 45 deg nose-down). sim: one hop, 5-12 cm. */
static const uint16_t BF_T8_REF[13] =
    {    0,  531,  531,  652,  493,  529,  327,  487,  474,  387,  474,  556,  581 };
static const int16_t BF_T8_DELTA[6][13] = {
    {    0,    0,   83,    0,    0,  -83,   -1,    0,   84,    3,    0,  -84,   -3},
    {    0,    0,   66, -138,    0,  -66,  139,    0,  120,  106,    0, -120, -106},
    {    0,    0,   39, -148,    0,  -39,  147,    0, -112, -181,    0,  112,  181},
    {    0,    0,  -33,  -14,    0,   33,   14,    0,   71,  -66,    0,  -71,   66},
    {    0,    0,   22,  -40,    0,  -22,   40,    0,   22,  284,    0,  -22, -284},
    {    0,    0,   85, -128,    0,  -85,  128,    0,   73,    7,    0,  -73,   -7},
};
static const uint16_t BF_T8_MOVE [7] = { 1000,  315,   74,  320,  368,   77,  216 };
static const uint16_t BF_T8_DELAY[7] = {  500,  329,  631,  113,  613,  523,  311 };
static const uint8_t  BF_T8_BLEND[7] = {  100,   64,   52,   42,   79,   21,   50 };

/* Far Jump as a frame table - sim/lj2bf3.py + bf3_tune.py from the Far Jump
 * (jumps/far_best.json): so it can be loaded, edited and re-taught in the
 * studio. Frames: 1-2 crouch (slow), 3 hold, 4-6 the push (rear legs first,
 * front legs ~57 ms later), 7-10 tuck in the air = the landing pose (held
 * 300 ms), 11 stand up.
 * Hips fixed, L/R mirrored, REF = the stand of the 2026-09-27 teach export.
 * sim: one hop, 18-23 cm, upright on 5/5 test robots. */
static const uint16_t BF_T9_REF[13] =
    {    0,  531,  531,  652,  493,  529,  327,  487,  474,  387,  474,  556,  581 };
static const int16_t BF_T9_DELTA[11][13] = {
    {    0,    0,   56,  -10,    0,  -56,   10,    0,   73,   21,    0,  -73,  -21},
    {    0,    0,  121,   -1,    0, -121,    1,    0,  151,   68,    0, -151,  -68},
    {    0,    0,  121,   -1,    0, -121,    1,    0,  151,   68,    0, -151,  -68},
    {    0,    0,  121,   -1,    0, -121,    1,    0,   -3,  100,    0,    3, -100},
    {    0,    0,   76,   67,    0,  -76,  -67,    0,  -80,  151,    0,   80, -151},
    {    0,    0,  -42,  190,    0,   42, -190,    0,  -80,  151,    0,   80, -151},
    {    0,    0,  -42,  190,    0,   42, -190,    0,  -80,  151,    0,   80, -151},
    {    0,    0,   17,   56,    0,  -17,  -56,    0,  -78,  149,    0,   78, -149},
    {    0,    0,  -15,  -70,    0,   15,   70,    0,    1,   61,    0,   -1,  -61},
    {    0,    0,  -15,  -70,    0,   15,   70,    0,   26,   26,    0,  -26,  -26},
    {    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0},
};
static const uint16_t BF_T9_MOVE [12] = { 1000,  350,  350,  135,   96,   16,   29,   15,   22,   57,   34,  700 };
static const uint16_t BF_T9_DELAY[12] = {  500,    0,    0,    0,    0,    0,    0,    0,    0,    0,  300,    0 };
static const uint8_t  BF_T9_BLEND[12] = {  100,  100,  100,  100,  100,  100,  100,  100,  100,  100,  100,  100 };

// Backflip K7 (minipupperesp BF_T30, "Backflip K7: K5 + soft catch"): K5's flip, then the paws reach for the
// floor with straighter legs (66 mm) and the legs give and sink into the crouch (48 mm) over ~80 ms.
static const uint16_t BF_T10_REF[13] =
    {    0,  425,  514,  658,  495,  507,  367,  504,  590,  455,  499,  512,  603 };
static const int16_t BF_T10_DELTA[8][13] = {
    {   0,    0, -101, -244,    0,  101,  244,    0, -152,  -38,    0,  152,   38},
    {   0,    0, -177,   60,    0,  177,  -60,    0, -141,  -71,    0,  141,   71},
    {   0,    0, -201,  -11,    0,  201,   11,    0, -241,   43,    0,  241,  -43},
    {   0,    0, -387, -469,    0,  387,  469,    0,  142,  402,    0, -142, -402},
    {   0,    0, -260, -528,    0,  260,  528,    0,  -48, -177,    0,   48,  177},
    {   0,    0,   12,   -9,    0,  -12,    9,    0,   12,   -9,    0,  -12,    9},
    {   0,    0,   58,  -40,    0,  -58,   40,    0,   58,  -40,    0,  -58,   40},
    {   0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0},
};
static const uint16_t BF_T10_MOVE [9] = { 1000,  600,   20,   23,   57,   53,   29,   82,  600 };
static const uint16_t BF_T10_DELAY[9] = {  500,  300,   31,   28,   87,   52,   15,  181,  300 };
static const uint8_t  BF_T10_BLEND[9] = {  100,  100,  100,  100,  100,  100,  100,  100,  100 };
// Backflip K7H (minipupperesp BF_T35, "Backflip K7H: K7 + hip damper"): K7 until the tuck, then the catch and
// the sink with the legs splaying sideways (hips ~24 deg). Sim: 10/10, ~18% softer hit than K7.
static const uint16_t BF_T11_REF[13] =
    {    0,  425,  514,  658,  495,  507,  367,  504,  590,  455,  499,  512,  603 };
static const int16_t BF_T11_DELTA[8][13] = {
    {   0,    0, -101, -244,    0,  101,  244,    0, -152,  -38,    0,  152,   38},
    {   0,    0, -177,   60,    0,  177,  -60,    0, -141,  -71,    0,  141,   71},
    {   0,    0, -201,  -11,    0,  201,   11,    0, -241,   43,    0,  241,  -43},
    {   0,    0, -387, -469,    0,  387,  469,    0,  142,  402,    0, -142, -402},
    {   0,    0, -260, -528,    0,  260,  528,    0,  -48, -177,    0,   48,  177},
    {   0,   -9,    0,    0,    9,    0,    0,    9,    0,    0,   -9,    0,    0},
    {   0,  -83,   75,  -49,   83,  -75,   49,   83,   75,  -49,  -83,  -75,   49},
    {   0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0},
};
static const uint16_t BF_T11_MOVE [9] = { 1000,  600,   20,   23,   57,   42,   30,   43,  600 };
static const uint16_t BF_T11_DELAY[9] = {  500,  300,   31,   28,   87,   51,   35,  123,  300 };
static const uint8_t  BF_T11_BLEND[9] = {  100,  100,  100,  100,  100,  100,  100,  100,  100 };

struct BfPreset {
    const char     *name;       // wire name
    const char     *note;
    int             n;          // frames, including the reference
    const uint16_t *ref;        // [13], index 0 unused
    const int16_t  *delta;      // [(n-1)*13], row-major
    const uint16_t *move_ms;    // [n]
    const uint16_t *delay_ms;   // [n]
    const uint8_t  *blend;      // [n], percent
    int             shift_after; // frame after which shift_ms is added (minipupperesp's fixed landing timing)
    int             shift_ms;    //   0 / 0 = none
};
const BfPreset BF_PRESETS[] = {
    { "backflip1",          "Backflip success (trial 1): 360 deg, lands on its feet", 11,
      BF_T1_REF, &BF_T1_DELTA[0][0], BF_T1_MOVE, BF_T1_DELAY, BF_T1_BLEND, 0, 0 },
    { "backflip_stable",    "Backflip stable: symmetric L/R, 8 frames, rear tuck", 8,
      BF_T2_REF, &BF_T2_DELTA[0][0], BF_T2_MOVE, BF_T2_DELAY, BF_T2_BLEND, 0, 0 },
    { "backflip_clean",     "Backflip clean: symmetric L/R, 7 frames, 3.8 s", 7,
      BF_T4_REF, &BF_T4_DELTA[0][0], BF_T4_MOVE, BF_T4_DELAY, BF_T4_BLEND, 0, 0 },
    { "backflip_stable_v0", "Backflip stable (pre-mirror): the asymmetric original, fallback", 8,
      BF_T2_REF, &BF_T3_DELTA[0][0], BF_T3_MOVE, BF_T3_DELAY, BF_T3_BLEND, 0, 0 },
    { "backflip_clean_opt", "Backflip clean OPT: sim lands at 264 deg (was 250)", 7,
      BF_T5_REF, &BF_T5_DELTA[0][0], BF_T5_MOVE, BF_T5_DELAY, BF_T5_BLEND, 0, 0 },
    { "backflip_stable_opt","Backflip stable OPT: sim lands at 282 deg (was 265)", 8,
      BF_T6_REF, &BF_T6_DELTA[0][0], BF_T6_MOVE, BF_T6_DELAY, BF_T6_BLEND, 0, 0 },
    { "longjump_1hop",      "Long Jump 1-hop (Lite 3): sim one hop 16-20 cm, hips fixed", 20,
      BF_T7_REF, &BF_T7_DELTA[0][0], BF_T7_MOVE, BF_T7_DELAY, BF_T7_BLEND, 0, 0 },
    { "longjump_teach",     "Long Jump teach OPT: 7 taught frames, re-timed, 5-12 cm", 7,
      BF_T8_REF, &BF_T8_DELTA[0][0], BF_T8_MOVE, BF_T8_DELAY, BF_T8_BLEND, 0, 0 },
    { "farjump_table",      "Far Jump (teach table): sim one hop 18-23 cm, hips fixed", 12,
      BF_T9_REF, &BF_T9_DELTA[0][0], BF_T9_MOVE, BF_T9_DELAY, BF_T9_BLEND, 0, 0 },
    // minipupperesp K7 / K7H, played the way it plays them by default: on Strong (gains_push(0)),
    // and with its fixed landing timing - the catch frame starts later by +6 ms (K7) / +14 ms (K7H),
    // added after frame 5 (bf_lt_def: the sink is frame 7, the shift goes after frame 7 - 2).
    { "backflip_k7",        "Backflip K7: K5 + soft catch (minipupperesp), Strong", 9,
      BF_T10_REF, &BF_T10_DELTA[0][0], BF_T10_MOVE, BF_T10_DELAY, BF_T10_BLEND, 5, 6 },
    { "backflip_k7h",       "Backflip K7H: K7 + hip damper (minipupperesp), Strong", 9,
      BF_T11_REF, &BF_T11_DELTA[0][0], BF_T11_MOVE, BF_T11_DELAY, BF_T11_BLEND, 5, 14 },
};
constexpr int BF_NPRESETS = sizeof(BF_PRESETS) / sizeof(BF_PRESETS[0]);
constexpr int BF_MAX_FRAMES = 32;
static_assert(BF_NPRESETS <= GAIT_BF_MAX, "raise GAIT_BF_MAX in robot.h");
static_assert(GAIT_BF_BASE + GAIT_BF_MAX <= GAIT_TJ_BASE, "BF and TJ gait ranges overlap");

constexpr int BF_DEF_MOVE = 600, BF_DEF_DELAY = 300, BF_REF_MOVE = 1000, BF_REF_DELAY = 500;

void bf_play(int k, GaitCmd cmd)
{
    if (k < 0 || k >= BF_NPRESETS) return;
    const BfPreset &p = BF_PRESETS[k];
    const int n = p.n < BF_MAX_FRAMES ? p.n : BF_MAX_FRAMES;

    // Frames: this robot's stand + the preset's deltas.
    static uint16_t frames[BF_MAX_FRAMES][13];
    const float tpd = servo_ticks_per_deg();
    for (int i = 1; i <= 12; ++i) {
        const int stand = 511 + static_cast<int>(s_offset[i] * tpd);
        for (int f = 0; f < n; ++f) {
            int v = stand + (f > 0 ? p.delta[(f - 1) * 13 + i] : 0);
            frames[f][i] = static_cast<uint16_t>(v < 0 ? 0 : (v > 1023 ? 1023 : v));
        }
    }
    auto move_ms = [&](int f) { return p.move_ms[f] > 0 ? p.move_ms[f] : (f == 0 ? BF_REF_MOVE : BF_DEF_MOVE); };
    auto span_ms = [&](int f) {
        const int blend = p.blend[f] > 0 ? p.blend[f] : 100;
        const int delay = p.delay_ms[f];   // 0 is a real value here (presets spell every one out)
        int v = move_ms(f) * blend / 100 + delay;
        if (f == p.shift_after) v += p.shift_ms;   // K7 / K7H landing timing (minipupperesp bf_lt_shift)
        return v < 1 ? 1 : v;
    };
    (void)BF_DEF_DELAY; (void)BF_REF_DELAY;

    const bool pushed = gains_push(0);   // Strong for the flip, like bf_auto_strong
    set_all_servo_speed(0);
    ESP_LOGI(TAG, "frame preset '%s': %d frames", p.name, n);

    float from[13], to[13];
    uint32_t t0 = 0, ms = 1;
    auto arm = [&](int f, uint32_t now) {
        for (int i = 1; i <= 12; ++i) {
            from[i] = static_cast<float>(s_goal_pos[i]);   // where it is NOW, mid-ramp or not
            to[i]   = static_cast<float>(frames[f][i]);
        }
        t0 = now;
        ms = static_cast<uint32_t>(move_ms(f));
    };
    uint32_t now = millis();
    int f = 0;
    arm(0, now);
    uint32_t next = now + static_cast<uint32_t>(span_ms(0));
    while (still(cmd)) {
        now = millis();
        const uint32_t dt = now - t0;
        for (int i = 1; i <= 12; ++i) {
            float v = to[i];
            if (ms && dt < ms) v = from[i] + (to[i] - from[i]) * (static_cast<float>(dt) / static_cast<float>(ms));
            s_goal_pos[i] = static_cast<uint16_t>(v + 0.5f);
        }
        flush();
        if (static_cast<int32_t>(now - next) >= 0) {
            if (++f >= n) break;               // done: the last frame is the stand
            arm(f, now);
            next = now + static_cast<uint32_t>(span_ms(f));
        }
    }
    if (pushed) gains_pop();
}

// ── Twerk + Mate (main.c) ──────────────────────────────────────────────────
// One cycle per call; the gait loop repeats it while the command stays.
void twerk_cycle(GaitCmd cmd)
{
    const float h = static_cast<float>(s_cfg.height);
    const float uh = static_cast<float>(s_cfg.up_height);
    const float p = static_cast<float>(s_cfg.period);
    const float twerkAmp = uh * 1.0f;
    const float frontAmp = uh * 0.6f;   // front legs bounce less than rear
    const float zLo = 15.0f, zHi = 100.0f;
    set_all_servo_speed(300);
    const uint32_t t0 = millis();
    float tim = 0;
    while (tim < p * 5) {
        tim = static_cast<float>(millis() - t0);
        const float tt = tim * 2.0f * PI / (p * 5);
        const float zf = std::fmax(zLo, std::fmin(zHi, h - frontAmp * std::sin(tt)));
        const float zr = std::fmax(zLo, std::fmin(zHi, h + twerkAmp * std::sin(tt)));
        front_right_ik(0, 0, zf); front_left_ik(0, 0, zf);
        rear_right_ik(0, 0, zr);  rear_left_ik(0, 0, zr);
        flush();
        if (!still(cmd)) break;
    }
}

void mate_cycle(GaitCmd cmd)
{
    const float uh = static_cast<float>(s_cfg.up_height);
    const float p = static_cast<float>(s_cfg.period);
    const float frontZ = 90.0f, rearMidZ = 50.0f, rearAmp = uh * 1.5f;
    const float zLo = 15.0f, zHi = 100.0f;
    set_all_servo_speed(400);
    front_right_ik(0, 0, frontZ); front_left_ik(0, 0, frontZ);
    flush();
    const uint32_t t0 = millis();
    float tim = 0;
    while (tim < p * 3) {
        tim = static_cast<float>(millis() - t0);
        const float tt = tim * 2.0f * PI / (p * 3);
        const float zr = std::fmax(zLo, std::fmin(zHi, rearMidZ + rearAmp * std::sin(tt)));
        front_right_ik(0, 0, frontZ); front_left_ik(0, 0, frontZ);
        rear_right_ik(0, 0, zr);      rear_left_ik(0, 0, zr);
        flush();
        if (!still(cmd)) break;
    }
}

// ── Jump (main.c) ──────────────────────────────────────────────────────────
void jump_once(GaitCmd cmd)
{
    const float h = static_cast<float>(s_cfg.height);
    const float p = static_cast<float>(s_cfg.period);
    const float crouchZ = JUMP_CROUCH_Z, pushZ = JUMP_PUSH_Z;
    uint32_t t0 = millis(), tim = 0;

    while (tim < p * 2) { tim = millis() - t0;
        const float tt = static_cast<float>(tim) * PI / 2.0f / (p * 2);
        legs_all(0, h - (h - crouchZ) * std::sin(tt));
        flush();
        if (!still(cmd)) return; }
    t0 = millis(); tim = 0;
    while (tim < 20) { tim = millis() - t0; legs_all(0, crouchZ); flush(); }

    // Explosive push; keep re-sending it (which refreshes the feedback) until
    // every calf reports full extension or JUMP_EXT_MAX_MS passes.
    set_all_servo_speed(0);
    legs_all(0, pushZ);
    flush();
    flush();
    t0 = millis(); tim = 0;
    while (tim < JUMP_EXT_MAX_MS) { tim = millis() - t0;
        legs_all(0, pushZ);
        flush();
        if (tim >= JUMP_EXT_MIN_MS && jump_legs_extended(JUMP_EXT_TOL)) break; }

    // Full extension -> yank the legs straight back to the crouch while the
    // body is still rising, and hold it through the flight and the landing.
    legs_all(0, crouchZ);
    flush();
    flush();
    t0 = millis(); tim = 0;
    while (tim < JUMP_AIR_MS) { tim = millis() - t0; legs_all(0, crouchZ); flush(); }

    t0 = millis(); tim = 0;
    while (tim < p * 3) { tim = millis() - t0;
        const float tt = static_cast<float>(tim) * PI / 2.0f / (p * 3);
        legs_all(0, crouchZ + (h - crouchZ) * std::sin(tt));
        flush();
        if (!still(cmd)) return; }
}

// ── Hip test (main.c /hiptest) ─────────────────────────────────────────────
volatile int s_hiptest_step = -1;
float        s_hiptest_deg  = 10.0f;
char         s_hiptest_msg[120] = "";

void hiptest_run(GaitCmd cmd)
{
    static const char *legn[4] = { "FR", "FL", "RR", "RL" };
    static const int   hid[4]  = { 1, 4, 7, 10 };
    set_all_servo_speed(0);
    const float amt = s_hiptest_deg;
    for (int k = 0; k < 8 && still(cmd); ++k) {
        const int l = k / 2;
        const float a = (k % 2 == 0) ? amt : -amt;
        s_hiptest_step = k;
        std::snprintf(s_hiptest_msg, sizeof(s_hiptest_msg),
                      "%d/8: %s hip (servo %d) %+.0f deg - did the %s FOOT go to the robot's LEFT or RIGHT?",
                      k + 1, legn[l], hid[l], static_cast<double>(a), legn[l]);
        ESP_LOGI(TAG, "hiptest %s", s_hiptest_msg);
        // 400 ms out, 1600 ms hold, 400 ms back, 400 ms rest
        const uint32_t t0 = millis();
        for (;;) {
            const uint32_t t = millis() - t0;
            if (t > 2800 || !still(cmd)) break;
            const float f = t < 400 ? t / 400.0f : t < 2000 ? 1.0f
                          : t < 2400 ? 1.0f - (t - 2000) / 400.0f : 0.0f;
            for (int i = 1; i <= 12; ++i)
                set_servo_angle(i, s_offset[i] + (i == hid[l] ? a * f : 0.0f));
            flush();
        }
    }
    for (int i = 1; i <= 12; ++i) set_servo_angle(i, s_offset[i]);
    flush();
    std::snprintf(s_hiptest_msg, sizeof(s_hiptest_msg), "%s",
                  still(cmd) ? "done - all 4 hips tested" : "stopped");
    s_hiptest_step = -1;
}

// ── IMU reflexes (main.c Alive + alive_write) ──────────────────────────────
// The pose alive.c returns (body x/y/z mm, roll + = left side up, pitch + =
// nose up, yaw + = nose left, per-paw offsets in the ground frame) -> the
// four feet in each hip's frame -> the exact IK. Line-for-line alive_write().
void alive_write_pose(const alive_out_t *o, float h)
{
    const float r = o->body[3] * PI / 180.0f, p = -o->body[4] * PI / 180.0f, y = o->body[5] * PI / 180.0f;
    const float cr = std::cos(r), sr = std::sin(r), cp = std::cos(p), sp = std::sin(p);
    const float cy = std::cos(y), sy = std::sin(y);
    // body rotation B = Rz(yaw) Ry(-pitch) Rx(roll); a foot in the body frame = B^T (foot - body)
    const float B[3][3] = { { cy * cp, cy * sp * sr - sy * cr, cy * sp * cr + sy * sr },
                            { sy * cp, sy * sp * sr + cy * cr, sy * sp * cr - cy * sr },
                            { -sp,     cp * sr,                cp * cr                } };
    const float ox[4] = { +SG_ORIGIN_X, +SG_ORIGIN_X, -SG_ORIGIN_X, -SG_ORIGIN_X };
    const float oy[4] = { -SG_ORIGIN_Y, +SG_ORIGIN_Y, -SG_ORIGIN_Y, +SG_ORIGIN_Y };
    sg_foot_t feet[4];
    for (int l = 0; l < 4; ++l) {
        const float w[3] = { ox[l] + o->paw[l][0] - o->body[0],
                             oy[l] + o->paw[l][1] - o->body[1],
                             -h    + o->paw[l][2] - o->body[2] };
        const float fx = B[0][0] * w[0] + B[1][0] * w[1] + B[2][0] * w[2];
        const float fy = B[0][1] * w[0] + B[1][1] * w[1] + B[2][1] * w[2];
        const float fz = B[0][2] * w[0] + B[1][2] * w[1] + B[2][2] * w[2];
        feet[l].x = fx - ox[l];
        feet[l].y = fy - oy[l];
        feet[l].z = -fz;
    }
    sg_write(feet);
}

void alive_run(GaitCmd cmd)
{
    const float h = static_cast<float>(s_cfg.height);
    static const int legj[8] = { 2, 3, 5, 6, 8, 9, 11, 12 };
    alive_reset();
    set_all_servo_speed(0);
    uint32_t seq = 0;
    while (still(cmd)) {
        // The BMI270 as mounted is exactly the frame alive.h expects (+x left,
        // +y forward, +z down; standing reads az = -1 g) - same chip, same
        // board as minipupperesp, raw axes on both sides.
        const ImuData d = imu_read();
        alive_in_t in;
        std::memset(&in, 0, sizeof(in));
        if (d.seq != 0 && d.seq != seq) {
            seq = d.seq;
            in.imu_ok = 1;
            in.acc_g[0] = d.ax; in.acc_g[1] = d.ay; in.acc_g[2] = d.az;
            in.gyro_dps[0] = d.gx; in.gyro_dps[1] = d.gy; in.gyro_dps[2] = d.gz;
        }
        for (int j = 0; j < 8; ++j) in.cur_mA += std::fabs(static_cast<float>(driver_board_present_current(legj[j])));
        alive_out_t o;
        alive_step(&in, 0.005f, &o);
        alive_write_pose(&o, h);
        flush();                                    // paces the loop to 5 ms
    }
}

// ── Look pad (main.c /look + its gait branch) ──────────────────────────────
constexpr float LOOK_YAW_MAX_DEG   = 30.0f;   // MovementGroups.yawcap
constexpr float LOOK_PITCH_MAX_DEG = 20.0f;
constexpr int   LOOK_TRANS_TICKS   = 70;      // Movements.transTic: full swing in 70 ticks
constexpr int64_t LOOK_TICK_US     = 15000;   // MovementGroups.dt
volatile float s_look_cmd_yaw = 0.0f, s_look_cmd_pitch = 0.0f;
float          s_look_yaw = 0.0f,     s_look_pitch = 0.0f;

float look_step(float now, float target, float rate)
{
    const float d = target - now;
    if (std::fabs(d) <= rate) return target;
    return now + (d > 0.0f ? rate : -rate);
}

// Glide toward the pad's yaw/pitch; when the pad is centred, or another move
// is commanded, glide back to the stand first - then the next move runs.
void lookpad_run(GaitCmd cmd)
{
    const float h = static_cast<float>(s_cfg.height);
    set_all_servo_speed(0);
    int64_t next_us = esp_timer_get_time();
    for (;;) {
        const bool on = still(cmd);
        const float ty = on ? s_look_cmd_yaw : 0.0f;
        const float tp = on ? s_look_cmd_pitch : 0.0f;
        s_look_yaw   = look_step(s_look_yaw,   ty, LOOK_YAW_MAX_DEG   / LOOK_TRANS_TICKS);
        s_look_pitch = look_step(s_look_pitch, tp, LOOK_PITCH_MAX_DEG / LOOK_TRANS_TICKS);
        sg_foot_t pose[4];
        sg_attitude(0.0f, s_look_pitch, s_look_yaw, h, pose);
        sg_write(pose);
        flush();
        if (s_look_yaw == 0.0f && s_look_pitch == 0.0f && ty == 0.0f && tp == 0.0f) break;
        next_us += LOOK_TICK_US;
        const int64_t now = esp_timer_get_time();
        if (now > next_us + 100000) next_us = now;
        while (esp_timer_get_time() < next_us) vTaskDelay(1);
    }
    s_sg_valid = false;
}

// Names / notes for the moves added here (GAIT_NAMES has the plain ones).
const char *mpe_note(GaitCmd cmd)
{
    switch (cmd) {
        case GaitCmd::Mate:     return "front legs tall, rear end thrusts up and down";
        case GaitCmd::LongJump: return "Lite 3 style long jump: bow, crouch, nose-up launch, land low";
        case GaitCmd::FarJump:  return "the sim-optimised jump that goes furthest";
        case GaitCmd::CharShow: return "every Character move, one after the other";
        case GaitCmd::Alive:    return "IMU reflexes: pick it up and it runs in the air; push it and it braces";
        case GaitCmd::HipTest:  return "each hip alone +/-10 deg, to check which way each hip moves its foot";
        case GaitCmd::Twerk:    return "fast up/down shake, rear legs more than front";
        case GaitCmd::Jump:     return "crouch, explosive push, tuck at lift-off, land in the crouch";
        default: break;
    }
    return "";
}

}  // namespace (minipupperesp moves)
