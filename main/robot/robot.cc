#include "robot/robot.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"   /* esp_rom_delay_us - sub-tick wait in flush() */
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"

extern "C" {
#include "robot/driver_board.h"
}

#include "robot/board_config.h"
#include "robot/stanford_gait.h"
#include "robot/stanford_kinematics.h"
#include "wasm/wasm_sandbox.h"

static const char *TAG = "robot";

// Does a running skill currently own the twelve goal positions? Defined in
// wasm_host_functions.cc, inside the GLOBAL namespace sdk.
//
// Declared out here on purpose: written inside `namespace robot`, this would
// have declared robot::sdk::control_owner_is_pose() - a different function
// that nothing defines, and the link fails with a message that points at the
// call site rather than at the declaration.
namespace sdk { bool control_owner_is_pose(); }

/* Ticks per degree, from the boards' OWN range - not a constant.
 *
 * This file used SERVO_DEG_TO_RAW = 1023/180, compiled in, because the Feetech
 * SCSCL servos it drove were 0-180 deg over the raw range 0..1023.
 *
 * The AT32 driver boards are not. They have a range_position_deg of their own,
 * defaulting to 310 deg and trimable by the reference firmware's `guard`
 * command, and they keep it in THEIR flash, where it outlives any amount of
 * ESP reflashing. So the range is not 180 by decree and it is not 270 either:
 * it is whatever the boards say, which driver_board_probe_scale() asks at boot.
 *
 * 10240/scale_dd rather than 1023/deg: the same formula the reference
 * firmware uses, so the two agree to the tick and not merely to the nearest
 * 0.1%. Getting this wrong does not stop the robot walking - it overdrives
 * every joint by the ratio, which is exactly the kind of fault that reads as
 * "the new servos are weaker" instead of "the scale is wrong". */
static inline float servo_ticks_per_deg()
{
    return 10240.0f / driver_board_get_scale_dd();
}

namespace robot {
namespace {

// ── NVS handle ───────────────────────────────────────────────
nvs_handle_t s_nvs = 0;

// ── Current gait command (written by send_gait_cmd, read by gait task) ──
GaitCmd s_gait_cmd = GaitCmd::None;

// ── Robot configuration (cached + persisted) ─────────────────
Config s_cfg;

// ── Servo offsets in degrees (index 1‑12, index 0 unused) ────
float s_offset[13] = {};

// ── Goal buffers (1‑based, index 0 unused) ───────────────────
uint16_t s_goal_pos[13]   = {};
uint16_t s_goal_speed[13] = {};

/* Per-servo current limit in mA, sent in every position frame. Seeded to
 * SERVO_CURRENT_MAX_MA in init() and persisted to NVS from there on.
 *
 * This is the knob that actually does something on this hardware: the driver
 * boards take a current cap, not a speed. Lowering it is how you bring a new
 * or freshly-calibrated robot up without a mis-signed joint tearing its leg. */
uint16_t s_current_ma[13] = {};

/* Additive per-servo trim in DEGREES, applied to the outgoing frame in flush()
 * and never written back into s_goal_pos - see set_overlay(). */
float s_overlay[13] = {};

/* The calibration this chassis is built to, in degrees, applied inside the IK.
 *
 * Index 1..12 is FR hip/shoulder/knee, FL, RR, RL - the same order as
 * s_offset[] below and as kFactoryOffset in the reference firmware.
 *
 * ALL ZERO, AND THAT MEANS UNCALIBRATED - not "centred". The Mini Pupper 2
 * numbers that used to live here were measured against a 180 deg servo scale;
 * the driver boards use their own range, so the same degree value is worth a
 * different number of counts and the old table would place every joint
 * somewhere new. There is nothing to carry over, so nothing is carried: put
 * the measured values here once the robot is on the bench and bump kCalSeed.
 *
 * Compiled in rather than left to NVS because every other route to getting
 * twelve numbers onto the robot has a way to silently not happen: an NVS
 * namespace that does not match, a web page whose writes never reached a
 * servo, a script against the wrong address. A table in the firmware is true
 * the moment it boots. */
constexpr float kFactoryOffset[13] = {
    0.0f,                                  /* index 0 unused */
    0.0f, 0.0f, 0.0f,                      /* FR hip, shoulder, knee */
    0.0f, 0.0f, 0.0f,                      /* FL */
    0.0f, 0.0f, 0.0f,                      /* RR */
    0.0f, 0.0f, 0.0f,                      /* RL */
};
constexpr int32_t kCalSeed = 1;

/* ── Servo bus ownership ──────────────────────────────────────
 * While the bus belongs to anything other than the gait, flush() holds the last
 * pose instead of writing, so nothing fights over the SPI transactions. Two
 * owners exist and must not steal from each other: the Servo Studio console
 * (a human) and a WASM skill doing low-level control. */
enum : int { BUS_FREE = 0, BUS_STUDIO = 1, BUS_SKILL = 2 };
volatile int s_bus_owner = BUS_FREE;

// ── Sync‑write ID list for all 12 servos ─────────────────────
/* No sync-write id list here any more. The SCSCL build needed the twelve servo
 * ids to build a SyncWritePos broadcast out of them; the driver boards are
 * selected by chip select from the board index instead, so there is no id list
 * to keep in step with the wiring - see driver_board_sync_write(). */

// ── Gait task handle ─────────────────────────────────────────
TaskHandle_t s_gait_task_handle = nullptr;

/* Wake the gait task immediately instead of letting it sit out its idle delay.
 * Every path that changes what the gait should be doing should call this; the
 * cost of forgetting is a few milliseconds of extra latency, not a hang. */
inline void wake_gait()
{
    if (s_gait_task_handle) {
        xTaskNotifyGive(s_gait_task_handle);
    }
}

/* ── The one gait name table ────────────────────────────────────────────
 *
 * GaitCmd::BodyAttitude is deliberately absent: it is set through
 * set_body_attitude(), not by name, and letting a caller send it directly
 * would put the robot into a pose-hold state that nothing asked for. */
struct GaitName { const char *name; GaitCmd cmd; };

const GaitName GAIT_NAMES[] = {
    { "none",         GaitCmd::None },
    { "init",         GaitCmd::Init },
    { "step",         GaitCmd::Step },
    { "roll",         GaitCmd::Roll },
    { "pitch",        GaitCmd::Pitch },
    { "stretch",      GaitCmd::Stretch },
    { "advance",      GaitCmd::Advance },
    { "back",         GaitCmd::Back },
    { "left",         GaitCmd::Left },
    { "right",        GaitCmd::Right },
    { "turnL",        GaitCmd::TurnL },
    { "turnR",        GaitCmd::TurnR },
    { "twerk",        GaitCmd::Twerk },
    { "jump",         GaitCmd::Jump },
    { "jumpfwd",      GaitCmd::JumpFwd },
    { "testspeed",    GaitCmd::TestSpeed },
    { "lookup",       GaitCmd::LookUp },
    { "lookdown",     GaitCmd::LookDown },
    { "lookleft",     GaitCmd::LookLeft },
    { "lookright",    GaitCmd::LookRight },
    { "lookul",       GaitCmd::LookUpperLeft },
    { "lookur",       GaitCmd::LookUpperRight },
    { "lookll",       GaitCmd::LookLowerLeft },
    { "looklr",       GaitCmd::LookLowerRight },
    { "flegL",        GaitCmd::ForelegLiftL },
    { "flegR",        GaitCmd::ForelegLiftR },
    { "blegL",        GaitCmd::BacklegLiftL },
    { "blegR",        GaitCmd::BacklegLiftR },
    { "heightup",     GaitCmd::HeightUp },
    { "heightdown",   GaitCmd::HeightDown },
    { "balance",      GaitCmd::Balance },
    { "bowback",      GaitCmd::BowBack },
    { "bodycycle",    GaitCmd::BodyCycle },
    { "headellipse",  GaitCmd::HeadEllipse },
    { "moveLF",       GaitCmd::MoveLeftFront },
    { "moveRF",       GaitCmd::MoveRightFront },
    { "moveLB",       GaitCmd::MoveLeftBack },
    { "moveRB",       GaitCmd::MoveRightBack },
    { "stanford",     GaitCmd::StanfordWalk },
    { "frontkick",    GaitCmd::FrontKick },
    { "wiggle",       GaitCmd::Wiggle },
    { "buttshrug",    GaitCmd::ButtShrug },
    { "wiggleL",      GaitCmd::WiggleLeft },
    { "wiggleR",      GaitCmd::WiggleRight },
    { "buttshrugL",   GaitCmd::ButtShrugLeft },
    { "buttshrugR",   GaitCmd::ButtShrugRight },
};
constexpr int GAIT_NAME_COUNT = sizeof(GAIT_NAMES) / sizeof(GAIT_NAMES[0]);

// ── Web joystick state (written by joy_input, read by gait task) ─
// Values -1..1 (already clamped).  s_js_active means the Stanford
// walk was started BY the joystick: when the input goes stale the
// robot then steps in place instead of walking forward.
volatile float    s_js_f = 0.0f, s_js_s = 0.0f, s_js_t = 0.0f;
volatile uint32_t s_js_last_ms = 0;
volatile bool     s_js_active  = false;

// Direct SDK body-attitude target. Each value is a single 32-bit write, so
// the gait task can safely sample it while the host task updates the pose.
volatile float s_body_roll_deg  = 0.0f;
volatile float s_body_pitch_deg = 0.0f;
volatile float s_body_yaw_deg   = 0.0f;

// Per-axis attitude slew rate in degrees/second. 0 = instant (snap straight
// to the target, the original behaviour). >0 makes that axis glide toward its
// target at this speed so it eases instead of jumping. Each axis is
// independent, so e.g. yaw can be slow while roll/pitch stay instant.
volatile float s_body_slew_roll_dps  = 0.0f;
volatile float s_body_slew_pitch_dps = 0.0f;
volatile float s_body_slew_yaw_dps   = 0.0f;

// The pose the gait task is CURRENTLY commanding while slewing toward the
// target above. Only touched by the gait task.
float s_body_roll_cur  = 0.0f;
float s_body_pitch_cur = 0.0f;
float s_body_yaw_cur   = 0.0f;

// Move `cur` toward `target` by at most `max_step` (one tick of slew).
static inline float step_toward(float cur, float target, float max_step)
{
    const float d = target - cur;
    if (d >  max_step) return cur + max_step;
    if (d < -max_step) return cur - max_step;
    return target;
}

// ── Helper: millis since boot ────────────────────────────────
static inline uint32_t millis()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

// ── NVS persistence helpers ──────────────────────────────────
static void nvs_put_float(const char *key, float v)
{
    if (s_nvs) {
        nvs_set_blob(s_nvs, key, &v, sizeof(v));
        nvs_commit(s_nvs);
    }
}

static float nvs_get_float(const char *key, float def)
{
    float v = def;
    size_t sz = sizeof(v);
    if (!s_nvs || nvs_get_blob(s_nvs, key, &v, &sz) != ESP_OK) {
        v = def;
    }
    return v;
}

static void nvs_put_i32(const char *key, int32_t v)
{
    if (s_nvs) {
        nvs_set_i32(s_nvs, key, v);
        nvs_commit(s_nvs);
    }
}

// ── Neutral-angle calibration (like NEUTRAL_ANGLE_DEGREES in the BSP,
//    identical to ik_neutral_init in the reference minipupperesp) ─────
// On this robot the physical standing pose is ALL SERVOS CENTRED (the
// Init pose).  The plain IK, however, returns big absolute angles
// (~ +52° on the shoulders) for the standing pose, so starting any
// motion used to reposition the legs first — and the boot-up stand was
// wrong.  Fix: command IK angles RELATIVE to the IK angles of the
// neutral stand (x=0, z=NEUTRAL_Z).  Then front_right_ik(0,0,NEUTRAL_Z)
// == centred servos == the power-on pose, and every gait starts right
// from the start-up stance — the same stand phase as the reference repo.
float s_th1_neutral_deg = 0.0f;
float s_th2_neutral_deg = 0.0f;

static void ik_neutral_init()
{
    const float ld  = NEUTRAL_Z;   // x=0 -> phi=0, ld=z
    const float th1 = -std::acos((L1 * L1 + ld * ld - L2 * L2) / (2.0f * L1 * ld));
    const float th2 =  std::asin((ld * ld - L1 * L1 - L2 * L2) / (2.0f * L1 * L2)) - th1;
    s_th1_neutral_deg = th1 * 180.0f / PI;
    s_th2_neutral_deg = th2 * 180.0f / PI;
}

// ── Inverse kinematics (shared math) ─────────────────────────
// Given x (forward), th0 (hip angle deg), z (height),
// compute shoulder (th1) and knee (th2) angles and write all 3 servos.
static void calculate_ik(float x, float th0_deg, float z,
                          float &th1_deg, float &th2_deg)
{
    const float th0_rad = th0_deg * PI / 180.0f;
    const float zd = z / std::cos(th0_rad);
    const float ld = std::sqrt(x * x + zd * zd);
    const float phi = std::atan2(x, zd);
    const float cos_arg = (L1 * L1 + ld * ld - L2 * L2) / (2.0f * L1 * ld);
    // Clamp to [-1, 1] to avoid domain errors
    const float clamped = (cos_arg > 1.0f) ? 1.0f : (cos_arg < -1.0f) ? -1.0f : cos_arg;
    const float th1 = phi - std::acos(clamped);
    const float th2 = std::asin((ld * ld - L1 * L1 - L2 * L2) / (2.0f * L1 * L2)) - th1;

    th1_deg = th1 * 180.0f / PI;
    th2_deg = th2 * 180.0f / PI;
}
 
// ═══════════════════════════════════════════════════════════════
//  Stanford exact-IK helpers (imported from StanfordQuadruped via
//  the reference minipupperesp firmware).
//
//  Everything below drives the 12 servos through the EXACT Stanford
//  Pupper / Mini Pupper BSP inverse kinematics (stanford_kinematics.cc)
//  instead of the approximate planar per-leg IK.  Foot targets are
//  sg_foot_t: x fwd / y left / z = body height above foot (downward-
//  positive), one per hip, leg order FR, FL, RR, RL.
// ═══════════════════════════════════════════════════════════════

sg_foot_t s_sg_feet[4] = {};   // last written Stanford foot targets
bool      s_sg_valid   = false;

// Buffer all 12 servo goals from four Stanford foot targets (does NOT flush).
static void sg_write(const sg_foot_t feet[4])
{
    float sdeg[13];
    stanford_kinematics_servo_deg(feet, sdeg);
    for (int i = 1; i <= 12; ++i) set_servo_angle(i, sdeg[i] + s_offset[i]);
    for (int l = 0; l < 4; ++l) s_sg_feet[l] = feet[l];
    s_sg_valid = true;
}

// Rest stance: all feet straight under the hips at height h.
static void sg_rest(float h, sg_foot_t feet[4])
{
    for (int l = 0; l < 4; ++l) {
        feet[l].x = 0.0f;
        feet[l].y = 0.0f;
        feet[l].z = h;
    }
}

// ── Body-attitude pose (StanfordQuadruped "head aiming") ─────
// StanfordQuadruped aims the "head" by holding the four feet planted
// while rotating the whole body (roll/pitch/yaw).  This computes each
// foot's position in the rotated body frame and returns the per-hip
// Stanford foot targets (fed to the exact IK by sg_write()).
//
//   roll  > 0  ->  lean right       (ROLL_SIGN)
//   pitch > 0  ->  nose up          (PITCH_SIGN)
//   yaw   > 0  ->  head turns right (YAW_SIGN)
//
// sg_attitude_feet() is the general form: it applies the body rotation
// on top of ARBITRARY per-hip foot targets (`base`), exactly like the
// MangDang FPC pipeline combines explicit foot locations with an
// attitude.  sg_attitude() is the common case with feet at rest.
static void sg_attitude_feet(float roll_deg, float pitch_deg, float yaw_deg,
                             const sg_foot_t base[4], sg_foot_t feet[4])
{
    const float r = roll_deg  * ROLL_SIGN  * PI / 180.0f;
    const float p = pitch_deg * PITCH_SIGN * PI / 180.0f;
    const float y = yaw_deg   * YAW_SIGN   * PI / 180.0f;

    const float cr = std::cos(r), sr = std::sin(r);
    const float cp = std::cos(p), sp = std::sin(p);
    const float cy = std::cos(y), sy = std::sin(y);

    // Body->world rotation R = Rz(yaw) * Ry(pitch) * Rx(roll).
    const float R00 = cy * cp;
    const float R01 = cy * sp * sr - sy * cr;
    const float R02 = cy * sp * cr + sy * sr;
    const float R10 = sy * cp;
    const float R11 = sy * sp * sr + cy * cr;
    const float R12 = sy * sp * cr - cy * sr;
    const float R20 = -sp;
    const float R21 = cp * sr;
    const float R22 = cp * cr;

    // Hip origins in the body frame (+x fwd, +y left) — true Stanford
    // geometry (LEG_FB, LEG_LR + ABDUCTION_OFFSET), order FR, FL, RR, RL.
    const float ox[4] = { +SG_ORIGIN_X, +SG_ORIGIN_X, -SG_ORIGIN_X, -SG_ORIGIN_X };
    const float oy[4] = { -SG_ORIGIN_Y, +SG_ORIGIN_Y, -SG_ORIGIN_Y, +SG_ORIGIN_Y };

    for (int l = 0; l < 4; ++l) {
        // Foot offset from body centre = hip origin + per-hip target.
        const float px = ox[l] + base[l].x;
        const float py = oy[l] + base[l].y;
        const float pz = -base[l].z;

        // Foot position in the rotated body frame = R^T * offset.
        const float fx = R00 * px + R10 * py + R20 * pz;
        const float fy = R01 * px + R11 * py + R21 * pz;
        const float fz = R02 * px + R12 * py + R22 * pz;

        feet[l].x = fx - ox[l];       // per-hip forward offset
        feet[l].y = fy - oy[l];       // per-hip lateral offset (+ left)
        feet[l].z = -fz;              // downward-positive body height
    }
}

// Attitude with the feet at the rest stance (the common case).
static void sg_attitude(float roll_deg, float pitch_deg, float yaw_deg,
                        float h, sg_foot_t feet[4])
{
    sg_foot_t base[4];
    sg_rest(h, base);
    sg_attitude_feet(roll_deg, pitch_deg, yaw_deg, base, feet);
}

// Pose-transition duration: scales with the web "Period" slider so the
// MOVEMENT ITSELF slows down together with the tempo (not just the time
// between moves).  Period 80 -> 250 ms ramps (snappy), 160 -> 480 ms,
// 300 -> 900 ms (slow and deliberate).
static int sg_ramp_ms()
{
    int ms = s_cfg.period * 3;
    if (ms < 250)  ms = 250;
    if (ms > 1500) ms = 1500;
    return ms;
}

// Smoothly move from the last written Stanford pose to `to` (sin-eased).
// Used both to ENTER a pose without snapping and to RETURN to the stand
// after a move ends — the StanfordQuadruped-style "goes back" behaviour.
static void sg_ramp_to(const sg_foot_t to[4], int ms)
{
    if (ms < 1) ms = 1;
    sg_foot_t from[4];
    if (s_sg_valid) {
        for (int l = 0; l < 4; ++l) from[l] = s_sg_feet[l];
    } else {
        sg_rest(static_cast<float>(s_cfg.height), from);
    }

    const uint32_t t0 = millis();
    for (;;) {
        uint32_t el = millis() - t0;
        if (el > static_cast<uint32_t>(ms)) el = ms;
        const float f = std::sin(static_cast<float>(el) * PI / 2.0f
                                 / static_cast<float>(ms));
        sg_foot_t cur[4];
        for (int l = 0; l < 4; ++l) {
            cur[l].x = from[l].x + (to[l].x - from[l].x) * f;
            cur[l].y = from[l].y + (to[l].y - from[l].y) * f;
            cur[l].z = from[l].z + (to[l].z - from[l].z) * f;
        }
        sg_write(cur);
        flush();                       // self-paced (≥5 ms per bus write)
        if (el >= static_cast<uint32_t>(ms)) break;
    }
}

// Park back in the neutral stand (Stanford "return to rest").
static void sg_park(float h)
{
    sg_foot_t rest[4];
    sg_rest(h, rest);
    sg_ramp_to(rest, (sg_ramp_ms() * 2) / 3);
    s_sg_valid = false;
}

/* Pull servo offsets out of the reference firmware's NVS namespace, once.
 *
 * Same keys, same float-blob format, same NVS partition - but that firmware
 * opens the namespace "parameter" and this one opens "robot", so a calibration
 * done there survives a reflash physically and is then invisible. Every offset
 * reads back as 0.0 and the robot stands visibly crooked, which looks like the
 * flash erased it. It did not; nobody was looking in the right place.
 *
 * Runs only when this namespace has nothing: any non-zero offset here means a
 * calibration was done on THIS firmware, and overwriting that with an older
 * robot's numbers would be worse than doing nothing.
 *
 * Note the offsets are in degrees and are applied by the IK, so they are
 * scale-relative by nature - values from a robot on a different servo range
 * will not reproduce the pose they came from. Treat an import as a starting
 * point to be re-measured, never as a calibrated stance. */
static void import_legacy_offsets()
{
    bool have_local = false;
    for (int i = 1; i <= 12 && !have_local; ++i) {
        if (s_offset[i] != 0.0f) have_local = true;
    }
    if (have_local) return;

    nvs_handle_t old = 0;
    if (nvs_open("parameter", NVS_READONLY, &old) != ESP_OK) return;

    int found = 0;
    for (int i = 1; i <= 12; ++i) {
        char key[16];
        std::snprintf(key, sizeof(key), "offset%d", i);
        float v = 0.0f;
        size_t sz = sizeof(v);
        if (nvs_get_blob(old, key, &v, &sz) == ESP_OK && sz == sizeof(v)) {
            s_offset[i] = v;
            nvs_put_float(key, v);   // into THIS namespace, so it sticks
            ++found;
        }
    }
    nvs_close(old);

    if (found) {
        ESP_LOGW(TAG, "Imported %d/12 servo offsets from the reference firmware's "
                      "NVS namespace. They were measured on a different servo "
                      "range - re-calibrate before running anything energetic.",
                 found);
    }
}

}  // anonymous namespace

// Forward declaration of gait task (defined at end of file)
void gait_task();

// ═══════════════════════════════════════════════════════════════
//  Public API
// ═══════════════════════════════════════════════════════════════

bool init()
{
    // ── Neutral-angle calibration (Ini pose == IK neutral stand) ─
    ik_neutral_init();

    // ── Open NVS ────────────────────────────────────────────────
    esp_err_t err = nvs_open("robot", NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        s_nvs = 0;
    }

    // ── Servo rail ──────────────────────────────────────────────
    // CN3..CN6 are wired straight to Vbat+, so SERVO_POWER_PIN is GPIO_NUM_NC
    // and there is nothing to raise. The delay is kept either way: on a cold
    // boot the AT32 driver boards need a moment on their own rail before they
    // will answer, whether that rail was just switched on or was live before
    // the ESP32 was.
#if SERVO_HAS_POWER_EN
    gpio_set_direction(static_cast<gpio_num_t>(SERVO_POWER_PIN), GPIO_MODE_OUTPUT);
    gpio_set_level(static_cast<gpio_num_t>(SERVO_POWER_PIN), 1);
    ESP_LOGI(TAG, "Servo power enabled (GPIO%d)", SERVO_POWER_PIN);
#else
    ESP_LOGI(TAG, "Servo rail is always on (no power-enable GPIO)");
#endif
    vTaskDelay(pdMS_TO_TICKS(1000));

    // The UART servo bus this firmware used to bring up (SCSCL on UART1,
    // TX=IO4 / RX=IO5, 500 kbaud) is gone: on this board both of those pins
    // are servo chip selects. What follows is the same job done over SPI, and
    // it is the better version of it - there is no baud rate to get wrong, and
    // each leg reports its position and current back on the same transaction
    // that commands it.
    // 
    // ── Initialise the SPI servo driver boards ──────────────────
    // Four AT32F413 boards, three servos each, on SPI2 (MOSI=IO6, MISO=IO17,
    // CLK=IO16, chip selects from robot/board_config.h). This replaces the
    // Feetech SCSCL serial bus, which used UART1 on IO4/IO5 - and on this
    // board both of those pins are servo chip selects.
    ESP_LOGI(TAG, "Initialising SPI servo driver boards...");
    if (!driver_board_init()) {
        ESP_LOGE(TAG, "Driver board init FAILED - no servo control");
    } else {
        ESP_LOGI(TAG, "Driver board init done");

        // Adopt the boards' own range_position_deg rather than assuming one.
        // A robot that was range-trimmed on the reference firmware keeps that
        // trim in the driver boards' flash, where an ESP reflash cannot reach
        // it, and assuming the wrong range puts every commanded angle out by
        // the ratio. Done here, before the gait task exists, because the
        // config path needs the SPI bus to itself.
        const float scale_dd = driver_board_probe_scale();
        if (driver_board_probe_answered()) {
            ESP_LOGI(TAG, "Driver board range: %.1f deg (%.3f ticks/deg)",
                     scale_dd / 10.0f, servo_ticks_per_deg());
        } else {
            ESP_LOGW(TAG, "No board reported its range - assuming %.1f deg. "
                          "Every commanded angle is a guess until one answers.",
                     scale_dd / 10.0f);
        }
    }

    // ── Diagnostic: probe all 12 channels ───────────────────────
    // One parameter read per servo. The old build pinged servo ID 1 alone,
    // which answers "is anything there"; this answers WHICH channels are
    // alive, and a whole silent board of three points at that board's chip
    // select or its power rather than at the servos.
    vTaskDelay(pdMS_TO_TICKS(100));
    {
        int  alive = 0;
        int  n     = 0;
        char list[64];
        list[0] = '\0';
        for (int i = 1; i <= 12; ++i) {
            float val;
            if (driver_board_get_param(i, DB_PARAM_KP_POSITION, &val)) {
                ++alive;
                n += std::snprintf(list + n, sizeof(list) - n, "%s%d", n ? "," : "", i);
            }
        }
        if (alive == 12) {
            ESP_LOGI(TAG, "Servo probe OK - all 12 channels responded");
        } else if (alive > 0) {
            ESP_LOGW(TAG, "Servo probe - %d/12 responded: [%s]", alive, list);
            ESP_LOGW(TAG, "Silent channels: check board power and chip select "
                          "wiring. SERVO_CS_MAP 1 is FR=CN4/IO7 FL=CN5/IO4 "
                          "RR=CN3/IO15 RL=CN6/IO5 - see robot/board_config.h");
        } else {
            ESP_LOGW(TAG, "Servo probe FAILED - no driver board responded");
            ESP_LOGW(TAG, "Check: servo rail on CN3..CN6, SPI wiring "
                          "(MOSI=IO6 MISO=IO17 CLK=IO16), and AT32 board firmware");
        }
    }

    // ── Restore persisted config ────────────────────────────────
    int32_t v;
    if (s_nvs) {
        if (nvs_get_i32(s_nvs, "period", &v) == ESP_OK)   s_cfg.period    = static_cast<int>(v);
        if (nvs_get_i32(s_nvs, "height", &v) == ESP_OK)   s_cfg.height    = static_cast<int>(v);
        if (nvs_get_i32(s_nvs, "upHeight", &v) == ESP_OK) s_cfg.up_height = static_cast<int>(v);
        if (nvs_get_i32(s_nvs, "stride", &v) == ESP_OK)   s_cfg.stride    = static_cast<int>(v);
        if (nvs_get_i32(s_nvs, "tilt", &v) == ESP_OK)     s_cfg.tilt      = static_cast<int>(v);
        if (nvs_get_i32(s_nvs, "sgspeed", &v) == ESP_OK)  s_cfg.sg_speed  = static_cast<int>(v);

        /* Seed the calibration from kFactoryOffset the first time this firmware
         * boots, then leave it alone. The marker is what makes it once-only:
         * without it, re-applying the table every boot would undo any
         * calibration done on the robot the moment it restarted. Bumping
         * kCalSeed forces the re-seed after the table is filled in. */
        int32_t seeded = 0;
        if (nvs_get_i32(s_nvs, "calseed", &seeded) != ESP_OK) seeded = 0;

        if (seeded != kCalSeed) {
            for (int i = 1; i <= 12; ++i) {
                s_offset[i] = kFactoryOffset[i];
                char key[16];
                std::snprintf(key, sizeof(key), "offset%d", i);
                nvs_put_float(key, s_offset[i]);
            }
            nvs_put_i32("calseed", kCalSeed);
            ESP_LOGI(TAG, "Servo calibration seeded from the built-in table (seed %d)",
                     (int)kCalSeed);
        } else {
            for (int i = 1; i <= 12; ++i) {
                char key[16];
                std::snprintf(key, sizeof(key), "offset%d", i);
                s_offset[i] = nvs_get_float(key, kFactoryOffset[i]);
            }
        }

        // One-time import from the reference firmware's namespace, for a
        // robot that was calibrated there before this firmware existed.
        import_legacy_offsets();
    }

    ESP_LOGI(TAG, "Config restored: period=%d height=%d upHeight=%d stride=%d tilt=%d sgspeed=%d",
             s_cfg.period, s_cfg.height, s_cfg.up_height, s_cfg.stride, s_cfg.tilt,
             s_cfg.sg_speed);

    /* Print the calibration, every boot, unconditionally.
     *
     * "Is this robot calibrated the same as the one the movement was taught
     * on" is not a question anybody should answer from memory, and it decides
     * whether a recorded pose lands or lands the robot on its back. The
     * reference build prints the same twelve numbers with its `offsets`
     * command, so the two can simply be read side by side.
     *
     * All twelve at 0.0 means UNCALIBRATED, not "centred": nothing has been
     * measured on this robot yet, and every pose is being played against raw
     * horn centres. */
    {
        static const char *kJoint[13] = { "",
            "FR_hip", "FR_sh", "FR_knee", "FL_hip", "FL_sh", "FL_knee",
            "RR_hip", "RR_sh", "RR_knee", "RL_hip", "RL_sh", "RL_knee" };
        int nonzero = 0;
        for (int i = 1; i <= 12; ++i) if (s_offset[i] != 0.0f) ++nonzero;
        ESP_LOGI(TAG, "Servo calibration (%d/12 non-zero):", nonzero);
        for (int i = 1; i <= 12; ++i) {
            ESP_LOGI(TAG, "   %2d %-8s %+7.2f deg", i, kJoint[i], s_offset[i]);
        }
        if (nonzero == 0) {
            ESP_LOGW(TAG, "   all zero - this robot has NOT been calibrated. "
                          "Any recorded pose will be played against raw horn "
                          "centres.");
        }
    }

    // ── Initialise goal buffers to centre ───────────────────────
    for (int i = 1; i <= 12; ++i) {
        s_goal_pos[i]   = 511;
        s_goal_speed[i] = 0;

        // Current cap: the persisted value if there is one, else the compiled
        // default. Never 0 - a robot that boots with no torque on its joints
        // simply collapses, and 0 is also what an unread NVS key returns.
        char ckey[16];
        std::snprintf(ckey, sizeof(ckey), "cur%d", i);
        int32_t cm = 0;
        if (s_nvs && nvs_get_i32(s_nvs, ckey, &cm) == ESP_OK &&
            cm > 0 && cm <= static_cast<int32_t>(SERVO_CURRENT_MAX_MA)) {
            s_current_ma[i] = static_cast<uint16_t>(cm);
        } else {
            s_current_ma[i] = SERVO_CURRENT_MAX_MA;
        }
    }

    // ── Initialise IMU (BMI270 + BMM150 over I2C) ──────────────────
    if (!imu_init()) {
        ESP_LOGW(TAG, "IMU init failed — continuing without IMU");
    } else {
        ESP_LOGI(TAG, "IMU initialised");
    }

    // ── Spawn gait task on core 1 ───────────────────────────────
    BaseType_t rv = xTaskCreatePinnedToCore(
        [](void *) { gait_task(); },
        "gait", 8192, nullptr, 22, &s_gait_task_handle, 1);

    if (rv != pdPASS) {
        ESP_LOGE(TAG, "Failed to create gait task");
        return false;
    }

    ESP_LOGI(TAG, "Robot HAL initialised");
    return true;
}

// ── Gait command ─────────────────────────────────────────────

void send_gait_cmd(GaitCmd cmd)
{
    s_gait_cmd = cmd;
}

GaitCmd current_gait_cmd()
{
    return s_gait_cmd;
}

// ── The gait name table ──────────────────────────────────────

bool gait_from_name(const char *name, GaitCmd &out)
{
    if (!name) return false;
    for (int i = 0; i < GAIT_NAME_COUNT; ++i) {
        if (std::strcmp(name, GAIT_NAMES[i].name) == 0) {
            out = GAIT_NAMES[i].cmd;
            return true;
        }
    }
    return false;
}

const char *gait_to_name(GaitCmd cmd)
{
    for (int i = 0; i < GAIT_NAME_COUNT; ++i) {
        if (GAIT_NAMES[i].cmd == cmd) return GAIT_NAMES[i].name;
    }
    return "none";
}

int gait_name_count()
{
    return GAIT_NAME_COUNT;
}

const char *gait_name_at(int index)
{
    if (index < 0 || index >= GAIT_NAME_COUNT) return nullptr;
    return GAIT_NAMES[index].name;
}

// ── Servo overlay ────────────────────────────────────────────

void set_overlay(int servo_id, float deg)
{
    if (servo_id < 1 || servo_id > 12) return;

    // Clamp rather than reject. A skill trimming a gait wants a bounded nudge,
    // and silently ignoring the request is a worse answer than giving it the
    // most it is allowed to have.
    if (deg >  SERVO_OVERLAY_MAX_DEG) deg =  SERVO_OVERLAY_MAX_DEG;
    if (deg < -SERVO_OVERLAY_MAX_DEG) deg = -SERVO_OVERLAY_MAX_DEG;

    s_overlay[servo_id] = deg;
    wake_gait();
}

float get_overlay(int servo_id)
{
    if (servo_id < 1 || servo_id > 12) return 0.0f;
    return s_overlay[servo_id];
}

void clear_overlay()
{
    for (int i = 1; i <= 12; ++i) s_overlay[i] = 0.0f;
    wake_gait();
}

void set_body_attitude(float roll_deg, float pitch_deg, float yaw_deg)
{
    // Match MovementGroups.py safety caps from the reference implementation.
    if (roll_deg > 25.0f) roll_deg = 25.0f;
    if (roll_deg < -25.0f) roll_deg = -25.0f;
    if (pitch_deg > 20.0f) pitch_deg = 20.0f;
    if (pitch_deg < -20.0f) pitch_deg = -20.0f;
    if (yaw_deg > 30.0f) yaw_deg = 30.0f;
    if (yaw_deg < -30.0f) yaw_deg = -30.0f;

    s_body_roll_deg  = roll_deg;
    s_body_pitch_deg = pitch_deg;
    s_body_yaw_deg   = yaw_deg;
    s_gait_cmd       = GaitCmd::BodyAttitude;
}

void set_attitude_speed(float dps)
{
    if (dps < 0.0f) dps = 0.0f;
    s_body_slew_roll_dps  = dps;
    s_body_slew_pitch_dps = dps;
    s_body_slew_yaw_dps   = dps;
}

void set_attitude_speed_xyz(float roll_dps, float pitch_dps, float yaw_dps)
{
    if (roll_dps  < 0.0f) roll_dps  = 0.0f;
    if (pitch_dps < 0.0f) pitch_dps = 0.0f;
    if (yaw_dps   < 0.0f) yaw_dps   = 0.0f;
    s_body_slew_roll_dps  = roll_dps;
    s_body_slew_pitch_dps = pitch_dps;
    s_body_slew_yaw_dps   = yaw_dps;
}

// ── Web joystick ─────────────────────────────────────────────
// Same behaviour as the reference minipupperesp /js handler:
// touching a pad auto-starts the Stanford trot; the gait task
// consumes the values each 15 ms tick, scaled by s_cfg.sg_speed.
void joy_input(float f, float s, float t)
{
    if (f >  1.0f) f =  1.0f;
    if (f < -1.0f) f = -1.0f;
    if (s >  1.0f) s =  1.0f;
    if (s < -1.0f) s = -1.0f;
    if (t >  1.0f) t =  1.0f;
    if (t < -1.0f) t = -1.0f;

    const bool moving = (std::fabs(f) > 0.08f ||
                         std::fabs(s) > 0.08f ||
                         std::fabs(t) > 0.08f);

    if (moving && s_gait_cmd != GaitCmd::StanfordWalk) {
        s_js_active = true;             // joystick-started walk
        s_gait_cmd  = GaitCmd::StanfordWalk;
    }
    if (moving) s_js_active = true;

    s_js_f = f;
    s_js_s = s;
    s_js_t = t;
    s_js_last_ms = millis();
}

// ── Configuration ────────────────────────────────────────────

Config get_config()
{
    return s_cfg;
}

void set_config(const Config &cfg)
{
    s_cfg = cfg;
    if (s_cfg.sg_speed < 10)  s_cfg.sg_speed = 10;
    if (s_cfg.sg_speed > static_cast<int>(SG_SPEED_MAX_MM_S))
        s_cfg.sg_speed = static_cast<int>(SG_SPEED_MAX_MM_S);
    if (s_nvs) {
        nvs_put_i32("period",   cfg.period);
        nvs_put_i32("height",   cfg.height);
        nvs_put_i32("upHeight", cfg.up_height);
        nvs_put_i32("stride",   cfg.stride);
        nvs_put_i32("tilt",     cfg.tilt);
        nvs_put_i32("sgspeed",  s_cfg.sg_speed);
    }
}

// ── Servo offsets ────────────────────────────────────────────

float get_offset(int servo_id)
{
    if (servo_id < 1 || servo_id > 12) return 0.0f;
    return s_offset[servo_id];
}

void set_offset(int servo_id, float deg)
{
    if (servo_id < 1 || servo_id > 12) return;
    s_offset[servo_id] = deg;
    char key[16];
    std::snprintf(key, sizeof(key), "offset%d", servo_id);
    nvs_put_float(key, deg);
}

void reset_offsets()
{
    for (int i = 1; i <= 12; ++i) {
        s_offset[i] = 0.0f;
        char key[16];
        std::snprintf(key, sizeof(key), "offset%d", i);
        nvs_put_float(key, 0.0f);
    }
}

// ── Servo feedback ───────────────────────────────────────────

// Every read below is served from the feedback cache the driver boards refresh
// on each sync_write, so at gait rate they cost no SPI traffic at all. When the
// gait is parked (Studio mode) that cache goes stale - poll the board first if
// you need a fresh value there.

int read_position(int servo_id)
{
    // AT32 frame, 0..1023 - the same frame Servo Studio reports. NOT the frame
    // set_servo_angle() accepts; for that, use read_angle_cdeg() below.
    if (servo_id < 1 || servo_id > 12) return -1;
    return static_cast<int>(driver_board_present_position(servo_id));
}

int read_angle_cdeg(int servo_id)
{
    // The measured angle in the SAME frame set_servo_angle() takes: signed
    // centidegrees relative to centre, positive in the direction a positive
    // `deg` command moves the joint.
    //
    // Without this a closed loop is impossible to write correctly.
    // read_position() is in the opposite frame, so the obvious
    // read -> compare -> correct loop diverges instead of converging,
    // silently and at speed - and nothing in the SDK says so.
    if (servo_id < 1 || servo_id > 12) return INT32_MIN;

    const int at32 = static_cast<int>(driver_board_present_position(servo_id));
    const int gait = at32_raw_to_gait_raw(at32);              // -> gait frame
    const float deg = (gait - 511) / servo_ticks_per_deg();   // -> deg from centre
    return static_cast<int>(deg * 100.0f);
}

int read_speed(int servo_id)
{
    // The driver boards do not report speed - position control is current
    // limited, not velocity commanded. Stays -1 rather than inventing one.
    if (servo_id < 1 || servo_id > 12) return -1;
    return -1;
}

int read_load(int servo_id)
{
    // Closest equivalent the boards report is present motor current.
    if (servo_id < 1 || servo_id > 12) return -1;
    return static_cast<int>(driver_board_present_current(servo_id));
}

int read_voltage(int servo_id)
{
    // Not reported by the AT32 boards: the rail is Vbat+ straight through, so
    // there is no per-servo supply to measure. (Called out as non-functional
    // on the Mini Pupper 2 hardware too; here it is at least the whole truth.)
    if (servo_id < 1 || servo_id > 12) return -1;
    return -1;
}

int read_temperature(int servo_id)
{
    const float c = read_temperature_c(servo_id);
    if (std::isnan(c)) return -1;
    return static_cast<int>(c);
}

float read_temperature_c(int servo_id)
{
    // A real reading on this board, unlike the Mini Pupper 2 build where
    // ReadTemper() was advertised and returned nothing. Each AT32 board
    // samples one 10k NTC per servo and ships it in the feedback frame's
    // reserved field, so this is free: it rides on the sync_write the gait is
    // already doing. Returns NaN when that servo has never answered.
    if (servo_id < 1 || servo_id > 12) return NAN;
    const float c = driver_board_present_temperature(servo_id);
    return (c > DB_TEMP_INVALID) ? c : NAN;
}

int read_moving(int servo_id)
{
    // Not reported directly. Infer it from the error against the goal: more
    // than ~2 raw counts off target counts as still moving.
    //
    // FRAME MATTERS HERE. Comparing the measured position (AT32 frame) straight
    // against s_goal_pos[] (gait frame) gives `1024 - 2*goal`, which is within
    // 2 counts only when the goal is near 511 - so every pose away from centre
    // reads as "still moving" forever and any skill polling this to sequence
    // its motions hangs. Convert the goal into the measured frame first; see
    // the frame note in robot.h.
    if (servo_id < 1 || servo_id > 12) return -1;
    const int now  = static_cast<int>(driver_board_present_position(servo_id));
    const int goal = gait_raw_to_at32_raw(static_cast<int>(s_goal_pos[servo_id]));
    return (std::abs(now - goal) > 2) ? 1 : 0;
}

int read_current(int servo_id)
{
    if (servo_id < 1 || servo_id > 12) return -1;
    return static_cast<int>(driver_board_present_current(servo_id));
}

int ping_servo(int servo_id)
{
    // There is no bus ping to send: a channel "answers" if its board replies to
    // a parameter read. One read is one request/NOP pair, so this is the same
    // cost as the SCSCL Ping() it replaces and gives the same yes/no.
    if (servo_id < 1 || servo_id > 12) return -1;
    float v;
    return driver_board_get_param(servo_id, DB_PARAM_KP_POSITION, &v) ? 1 : -1;
}

// ── Servo bus ownership ──────────────────────────────────────
//
// Two owners may take the bus away from the gait: the Servo Studio console (a
// human at a keyboard) and a WASM skill doing low-level control. While either
// holds it, flush() holds the last pose instead of writing, so nothing
// interleaves with a config request/NOP pair.

void set_studio_mode(bool on)
{
    if (on) {
        if (s_bus_owner == BUS_STUDIO) return;
        // The console outranks a skill: a human at the keyboard takes the bus.
        if (s_bus_owner == BUS_SKILL) {
            ESP_LOGW(TAG, "Studio mode taking the bus from a running skill");
        }
        s_bus_owner = BUS_STUDIO;
        // Let the gait task finish the tick it is in before the caller starts
        // issuing config frames. A tick is ~5 ms; 50 ms is generous.
        vTaskDelay(pdMS_TO_TICKS(50));
        ESP_LOGI(TAG, "Studio mode ON - gait parked, servos holding pose");
    } else {
        if (s_bus_owner != BUS_STUDIO) return;
        s_bus_owner = BUS_FREE;
        ESP_LOGI(TAG, "Studio mode OFF - gait resumed");
    }
}

bool studio_mode()
{
    return s_bus_owner == BUS_STUDIO;
}

bool servo_lock()
{
    if (s_bus_owner == BUS_SKILL) return true;      // already ours
    if (s_bus_owner == BUS_STUDIO) {
        ESP_LOGW(TAG, "servo_lock refused - Servo Studio holds the bus");
        return false;
    }
    s_bus_owner = BUS_SKILL;
    vTaskDelay(pdMS_TO_TICKS(50));   // let the in-flight gait tick finish
    ESP_LOGI(TAG, "Servo bus locked by skill - gait parked");
    return true;
}

void servo_unlock()
{
    if (s_bus_owner != BUS_SKILL) return;
    s_bus_owner = BUS_FREE;
    ESP_LOGI(TAG, "Servo bus released by skill - gait resumed");
}

bool servo_locked()
{
    // "Somebody holds the bus" - Studio OR a skill. Do NOT use this to
    // authorise a skill's servo writes; use servo_owned_by_skill() for that.
    return s_bus_owner != BUS_FREE;
}

bool servo_owned_by_skill()
{
    // "THIS skill holds the bus", which is the question every low-level servo
    // host function actually needs to ask.
    //
    // Calling servo_locked() instead would also be true while Servo Studio
    // holds the bus, so a skill that never called servo_lock() could drive all
    // twelve joints for as long as someone had Studio open in a browser - and
    // its SPI traffic would interleave with Studio's config request/reply
    // pairs. The per-frame mutex in driver_board.c keeps individual frames
    // intact but cannot stop two owners taking turns.
    return s_bus_owner == BUS_SKILL;
}

void release_skill_bus_lock()
{
    if (s_bus_owner == BUS_SKILL) {
        ESP_LOGW(TAG, "Skill ended while holding the servo bus - releasing");
        servo_unlock();
    }
}

// ── Low-level servo control ──────────────────────────────────

void set_servo_angle(int servo_id, float deg)
{
    // Convert degrees to a raw setpoint in the GAIT frame: 0..1023 across the
    // boards' full mechanical range, centre -> 511. `deg` is RELATIVE to
    // centre, and one degree is servo_ticks_per_deg() raw steps - read from
    // the boards at boot, not assumed (see the note by that function).
    // driver_board_sync_write() rescales this to the AT32's deci-degrees.
    int sig = 511 + static_cast<int>(deg * servo_ticks_per_deg());
    if (sig < 0)   sig = 0;
    if (sig > 1023) sig = 1023;
    s_goal_pos[servo_id] = static_cast<uint16_t>(sig);
    ESP_LOGD(TAG, "set_servo_angle: id=%d deg=%.1f raw=%u", servo_id, deg, s_goal_pos[servo_id]);
}

void set_servo_speed(int servo_id, uint16_t speed)
{
    s_goal_speed[servo_id] = speed;
}

void set_all_servo_speed(uint16_t speed)
{
    for (int i = 1; i <= 12; ++i) s_goal_speed[i] = speed;
}

void set_servo_current(int servo_id, uint16_t milliamps)
{
    if (servo_id < 1 || servo_id > 12) return;

    // Clamp rather than reject. A skill that asks for 5 A should get the most
    // the hardware is allowed to give, not silence, and not an error code it
    // has no way to act on.
    if (milliamps > SERVO_CURRENT_MAX_MA) milliamps = SERVO_CURRENT_MAX_MA;
    s_current_ma[servo_id] = milliamps;

    char key[16];
    std::snprintf(key, sizeof(key), "cur%d", servo_id);
    nvs_put_i32(key, static_cast<int32_t>(milliamps));

    ESP_LOGI(TAG, "Servo %d current limit = %u mA", servo_id, (unsigned)milliamps);
}

uint16_t get_servo_current(int servo_id)
{
    if (servo_id < 1 || servo_id > 12) return 0;
    return s_current_ma[servo_id];
}

void flush()
{
    // Studio or a skill owns the bus - hold the last pose rather than fight it.
    if (s_bus_owner != BUS_FREE) return;
    // Ensure at least 5 ms between bus accesses
    static int64_t last_us = 0;
    for (;;) {
        const int64_t remain_us = 5000 - (esp_timer_get_time() - last_us);
        if (remain_us <= 0) break;
        if (remain_us <= 1000) {
            esp_rom_delay_us(static_cast<uint32_t>(remain_us));
            break;
        }
        vTaskDelay(1);
    }
    last_us = esp_timer_get_time();

    // The driver boards take a position plus a CURRENT CAP and have no speed
    // field, so s_goal_speed is deliberately not sent - see set_servo_speed().
    uint16_t pos[12];
    uint16_t cur[12];
    for (int i = 0; i < 12; ++i) {
        int raw = static_cast<int>(s_goal_pos[i + 1]);

        // The overlay is added here, to the frame on its way out, so it rides
        // on top of whatever produced s_goal_pos - the gait generator, the IK
        // or a skill - without any of them knowing about it. It is never
        // written back, which is what makes clearing it sufficient to undo it
        // and stops it accumulating across ticks.
        const float ov = s_overlay[i + 1];
        if (ov != 0.0f) {
            raw += static_cast<int>(ov * servo_ticks_per_deg());
            if (raw < 0)    raw = 0;
            if (raw > 1023) raw = 1023;
        }

        pos[i] = static_cast<uint16_t>(raw);
        cur[i] = s_current_ma[i + 1];
    }
    driver_board_sync_write(pos, cur);
    ESP_LOGD(TAG, "flush: sync_write sent to the four driver boards");
}

// ── Per‑leg IK ───────────────────────────────────────────────

// All shoulder/knee angles are commanded RELATIVE to the neutral stand
// (s_th1_neutral_deg / s_th2_neutral_deg), exactly like the reference
// minipupperesp fRIK/fLIK/rRIK/rLIK: front_right_ik(0,0,NEUTRAL_Z) ==
// centred servos == the calibrated stand pose.

void front_right_ik(float x, float th0, float z)
{
    float th1, th2;
    calculate_ik(x, th0, z, th1, th2);
    set_servo_angle(1,  th0                          + s_offset[1]);
    set_servo_angle(2, -(th1 - s_th1_neutral_deg)    + s_offset[2]);
    set_servo_angle(3,  (th2 - s_th2_neutral_deg)    + s_offset[3]);
}

void front_left_ik(float x, float th0, float z)
{
    float th1, th2;
    calculate_ik(x, th0, z, th1, th2);
    set_servo_angle(4,  th0                          + s_offset[4]);
    set_servo_angle(5,  (th1 - s_th1_neutral_deg)    + s_offset[5]);
    set_servo_angle(6, -(th2 - s_th2_neutral_deg)    + s_offset[6]);
}

void rear_right_ik(float x, float th0, float z)
{
    float th1, th2;
    calculate_ik(x, th0, z, th1, th2);
    set_servo_angle(7,  th0                          + s_offset[7]);
    set_servo_angle(8, -(th1 - s_th1_neutral_deg)    + s_offset[8]);
    set_servo_angle(9,  (th2 - s_th2_neutral_deg)    + s_offset[9]);
}

void rear_left_ik(float x, float th0, float z)
{
    float th1, th2;
    calculate_ik(x, th0, z, th1, th2);
    set_servo_angle(10,  th0                         + s_offset[10]);
    set_servo_angle(11,  (th1 - s_th1_neutral_deg)   + s_offset[11]);
    set_servo_angle(12, -(th2 - s_th2_neutral_deg)   + s_offset[12]);
}

// ═══════════════════════════════════════════════════════════════
//  Gait task (runs on core 1)
// ═══════════════════════════════════════════════════════════════

void gait_task()
{
    uint32_t time_mSt;
    float tim, tt;
    const float PI = robot::PI;

    // Start centred
    for (int i = 1; i <= 12; ++i) s_goal_pos[i] = 511;
    set_all_servo_speed(0);

    GaitCmd last_logged = GaitCmd::None;

    for (;;) {
        const GaitCmd cmd = s_gait_cmd;

        // Log gait transitions (avoid spam on continuous gaits)
        if (cmd != last_logged) {
            last_logged = cmd;
            const char *name = "none";
            switch (cmd) {
                case GaitCmd::None:      name = "none";      break;
                case GaitCmd::Init:      name = "init";      break;
                case GaitCmd::Step:      name = "step";      break;
                case GaitCmd::Roll:      name = "roll";      break;
                case GaitCmd::Pitch:     name = "pitch";     break;
                case GaitCmd::Stretch:   name = "stretch";   break;
                case GaitCmd::Advance:   name = "advance";   break;
                case GaitCmd::Back:      name = "back";      break;
                case GaitCmd::Left:      name = "left";      break;
                case GaitCmd::Right:     name = "right";     break;
                case GaitCmd::TurnL:     name = "turnL";     break;
                case GaitCmd::TurnR:     name = "turnR";     break;
                case GaitCmd::Twerk:     name = "twerk";     break;
                case GaitCmd::Jump:      name = "jump";      break;
                case GaitCmd::JumpFwd:   name = "jumpfwd";   break;
                case GaitCmd::TestSpeed: name = "testspeed"; break;
                case GaitCmd::LookUp:         name = "lookup";        break;
                case GaitCmd::LookDown:       name = "lookdown";      break;
                case GaitCmd::LookLeft:       name = "lookleft";      break;
                case GaitCmd::LookRight:      name = "lookright";     break;
                case GaitCmd::LookUpperLeft:  name = "lookul";        break;
                case GaitCmd::LookUpperRight: name = "lookur";        break;
                case GaitCmd::LookLowerLeft:  name = "lookll";        break;
                case GaitCmd::LookLowerRight: name = "looklr";        break;
                case GaitCmd::ForelegLiftL:   name = "flegL";         break;
                case GaitCmd::ForelegLiftR:   name = "flegR";         break;
                case GaitCmd::BacklegLiftL:   name = "blegL";         break;
                case GaitCmd::BacklegLiftR:   name = "blegR";         break;
                case GaitCmd::HeightUp:       name = "heightup";      break;
                case GaitCmd::HeightDown:     name = "heightdown";    break;
                case GaitCmd::Balance:        name = "balance";       break;
                case GaitCmd::BowBack:        name = "bowback";       break;
                case GaitCmd::BodyCycle:      name = "bodycycle";     break;
                case GaitCmd::HeadEllipse:    name = "headellipse";   break;
                case GaitCmd::MoveLeftFront:  name = "moveLF";        break;
                case GaitCmd::MoveRightFront: name = "moveRF";        break;
                case GaitCmd::MoveLeftBack:   name = "moveLB";        break;
                case GaitCmd::MoveRightBack:  name = "moveRB";        break;
                case GaitCmd::StanfordWalk:   name = "stanford";      break;
                case GaitCmd::FrontKick:      name = "frontkick";     break;
                case GaitCmd::Wiggle:         name = "wiggle";        break;
                case GaitCmd::ButtShrug:      name = "buttshrug";     break;
                case GaitCmd::WiggleLeft:     name = "wiggleL";       break;
                case GaitCmd::WiggleRight:    name = "wiggleR";       break;
                case GaitCmd::ButtShrugLeft:  name = "buttshrugL";    break;
                case GaitCmd::ButtShrugRight: name = "buttshrugR";    break;
                case GaitCmd::BodyAttitude:   name = "attitude";      break;
            }
            ESP_LOGI(TAG, "Gait: %s", name);
        }

        // ── Idle (no command) ────────────────────────────────────
        if (cmd == GaitCmd::None) {
            // A skill that claimed the feet or the joints owns the goal buffer,
            // so do not rebuild the neutral stand over the top of it.
            //
            // Before this guard existed a control claim was checked ONLY inside
            // the host functions, so it could refuse the skill's own calls and
            // nothing else - while the buffer it had just claimed was rewritten
            // underneath it every tick. Only one skill runs at a time, so the
            // claim protected the skill from itself and from nobody.
            if (!::sdk::control_owner_is_pose()) {
                front_right_ik(0, 0, static_cast<float>(s_cfg.height));
                rear_right_ik(0, 0, static_cast<float>(s_cfg.height));
                front_left_ik(0, 0, static_cast<float>(s_cfg.height));
                rear_left_ik(0, 0, static_cast<float>(s_cfg.height));
            }
            // When a WASM skill is running, skip the flush so the
            // skill's own servo commands (sent via robot_flush from
            // host functions) are not immediately overwritten with
            // neutral positions.  The goal buffer is still updated
            // so positions are correct when WASM ends.
            if (!wasm::is_running()) {
                flush();
            }
            vTaskDelay(1);
            continue;
        }

        // ── Initial pose ─────────────────────────────────────────
        if (cmd == GaitCmd::Init) {
            set_all_servo_speed(0);
            for (int i = 1; i <= 12; ++i) set_servo_angle(i, s_offset[i]);
            flush();
            vTaskDelay(1);
            s_gait_cmd = GaitCmd::None;   // auto‑reset
            continue;
        }

        // ── Step (in‑place trot) ─────────────────────────────────
        if (cmd == GaitCmd::Step) {
            const float h  = static_cast<float>(s_cfg.height);
            const float uh = static_cast<float>(s_cfg.up_height);
            const float p  = static_cast<float>(s_cfg.period);

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(0, 0, h - uh * std::sin(tt));
                rear_left_ik(0, 0, h - uh * std::sin(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(0, 0, h - uh * std::cos(tt));
                rear_left_ik(0, 0, h - uh * std::cos(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                rear_right_ik(0, 0, h - uh * std::sin(tt));
                front_left_ik(0, 0, h - uh * std::sin(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                rear_right_ik(0, 0, h - uh * std::cos(tt));
                front_left_ik(0, 0, h - uh * std::cos(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            continue;
        }

        // ── Roll ─────────────────────────────────────────────────
        if (cmd == GaitCmd::Roll) {
            const float h = static_cast<float>(s_cfg.height);
            const float t = static_cast<float>(s_cfg.tilt);
            const float p = static_cast<float>(s_cfg.period) * 8.0f;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * 2.0f * PI / p;
                front_right_ik(0, -t * std::sin(tt), h);
                rear_left_ik(0,  t * std::sin(tt), h);
                rear_right_ik(0,  t * std::sin(tt), h);
                front_left_ik(0, -t * std::sin(tt), h);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            s_gait_cmd = GaitCmd::None;
            continue;
        }

        // ── Pitch ────────────────────────────────────────────────
        if (cmd == GaitCmd::Pitch) {
            const float h  = static_cast<float>(s_cfg.height);
            const float uh = static_cast<float>(s_cfg.up_height);
            const float p  = static_cast<float>(s_cfg.period) * 8.0f;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * 2.0f * PI / p;
                front_right_ik(0, 0, h - uh * std::sin(tt));
                rear_left_ik(0, 0, h + uh * std::sin(tt));
                rear_right_ik(0, 0, h + uh * std::sin(tt));
                front_left_ik(0, 0, h - uh * std::sin(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            s_gait_cmd = GaitCmd::None;
            continue;
        }

        // ── Stretch ──────────────────────────────────────────────
        if (cmd == GaitCmd::Stretch) {
            const float h  = static_cast<float>(s_cfg.height);
            const float uh = static_cast<float>(s_cfg.up_height);
            const float p  = static_cast<float>(s_cfg.period) * 8.0f;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * 2.0f * PI / p;
                front_right_ik(0, 0, h + uh * std::sin(tt));
                rear_left_ik(0, 0, h + uh * std::sin(tt));
                rear_right_ik(0, 0, h + uh * std::sin(tt));
                front_left_ik(0, 0, h + uh * std::sin(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            s_gait_cmd = GaitCmd::None;
            continue;
        }

        // ── Advance (forward trot) ───────────────────────────────
        if (cmd == GaitCmd::Advance) {
            const float h  = static_cast<float>(s_cfg.height);
            const float uh = static_cast<float>(s_cfg.up_height);
            const float s  = static_cast<float>(s_cfg.stride);
            const float p  = static_cast<float>(s_cfg.period);

            // Phase 1
            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(-s * std::cos(tt), 0, h - uh * std::sin(tt));
                rear_left_ik(  -s * std::cos(tt), 0, h - uh * std::sin(tt));
                rear_right_ik(  s * std::cos(tt), 0, h);
                front_left_ik(  s * std::cos(tt), 0, h);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            // Phase 2
            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik( s * std::sin(tt), 0, h - uh * std::cos(tt));
                rear_left_ik(   s * std::sin(tt), 0, h - uh * std::cos(tt));
                rear_right_ik(-s * std::sin(tt), 0, h);
                front_left_ik(-s * std::sin(tt), 0, h);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            // Phase 3
            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik( s * std::cos(tt), 0, h);
                rear_left_ik(   s * std::cos(tt), 0, h);
                rear_right_ik(-s * std::cos(tt), 0, h - uh * std::sin(tt));
                front_left_ik(-s * std::cos(tt), 0, h - uh * std::sin(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            // Phase 4
            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(-s * std::sin(tt), 0, h);
                rear_left_ik(  -s * std::sin(tt), 0, h);
                rear_right_ik( s * std::sin(tt), 0, h - uh * std::cos(tt));
                front_left_ik( s * std::sin(tt), 0, h - uh * std::cos(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            continue;
        }

        // ── Back (backward trot) ─────────────────────────────────
        if (cmd == GaitCmd::Back) {
            const float h  = static_cast<float>(s_cfg.height);
            const float uh = static_cast<float>(s_cfg.up_height);
            const float s  = static_cast<float>(s_cfg.stride);
            const float p  = static_cast<float>(s_cfg.period);

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik( s * std::cos(tt), 0, h - uh * std::sin(tt));
                rear_left_ik(   s * std::cos(tt) + 15, 0, h - uh * std::sin(tt));
                rear_right_ik(-s * std::cos(tt) + 15, 0, h);
                front_left_ik(-s * std::cos(tt), 0, h);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(-s * std::sin(tt), 0, h - uh * std::cos(tt));
                rear_left_ik(  -s * std::sin(tt) + 15, 0, h - uh * std::cos(tt));
                rear_right_ik( s * std::sin(tt) + 15, 0, h);
                front_left_ik( s * std::sin(tt), 0, h);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(-s * std::cos(tt), 0, h);
                rear_left_ik(  -s * std::cos(tt) + 15, 0, h);
                rear_right_ik( s * std::cos(tt) + 15, 0, h - uh * std::sin(tt));
                front_left_ik( s * std::cos(tt), 0, h - uh * std::sin(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik( s * std::sin(tt), 0, h);
                rear_left_ik(   s * std::sin(tt) + 15, 0, h);
                rear_right_ik(-s * std::sin(tt) + 15, 0, h - uh * std::cos(tt));
                front_left_ik(-s * std::sin(tt), 0, h - uh * std::cos(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            continue;
        }

        // ── Left (strafe) ────────────────────────────────────────
        if (cmd == GaitCmd::Left) {
            const float h  = static_cast<float>(s_cfg.height);
            const float uh = static_cast<float>(s_cfg.up_height);
            const float t  = static_cast<float>(s_cfg.tilt);
            const float p  = static_cast<float>(s_cfg.period);

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(0,  t - 2.0f * t * std::sin(tt), h - uh * std::sin(tt));
                rear_left_ik(0,  -t + 2.0f * t * std::sin(tt), h - uh * std::sin(tt));
                rear_right_ik(0,  t * std::cos(tt), h - uh * std::cos(tt));
                front_left_ik(0, -t * std::cos(tt), h - uh * std::cos(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                rear_right_ik(0, -t * std::sin(tt), h);
                front_left_ik(0,  t * std::sin(tt), h);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(0, -t * std::cos(tt), h - uh * std::cos(tt));
                rear_left_ik(0,   t * std::cos(tt), h - uh * std::cos(tt));
                rear_right_ik(0, -t + 2.0f * t * std::sin(tt), h - uh * std::sin(tt));
                front_left_ik(0,  t - 2.0f * t * std::sin(tt), h - uh * std::sin(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(0,  t * std::sin(tt), h);
                rear_left_ik(0,  -t * std::sin(tt), h);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            continue;
        }

        // ── Right (strafe) ───────────────────────────────────────
        if (cmd == GaitCmd::Right) {
            const float h  = static_cast<float>(s_cfg.height);
            const float uh = static_cast<float>(s_cfg.up_height);
            const float t  = static_cast<float>(s_cfg.tilt);
            const float p  = static_cast<float>(s_cfg.period);

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(0, -t + 2.0f * t * std::sin(tt), h - uh * std::sin(tt));
                rear_left_ik(0,   t - 2.0f * t * std::sin(tt), h - uh * std::sin(tt));
                rear_right_ik(0, -t * std::cos(tt), h - uh * std::cos(tt));
                front_left_ik(0,  t * std::cos(tt), h - uh * std::cos(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                rear_right_ik(0,  t * std::sin(tt), h);
                front_left_ik(0, -t * std::sin(tt), h);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(0,  t * std::cos(tt), h - uh * std::cos(tt));
                rear_left_ik(0,  -t * std::cos(tt), h - uh * std::cos(tt));
                rear_right_ik(0,  t - 2.0f * t * std::sin(tt), h - uh * std::sin(tt));
                front_left_ik(0, -t + 2.0f * t * std::sin(tt), h - uh * std::sin(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(0, -t * std::sin(tt), h);
                rear_left_ik(0,  t * std::sin(tt), h);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            continue;
        }

        // ── Turn Left ────────────────────────────────────────────
        if (cmd == GaitCmd::TurnL) {
            const float h  = static_cast<float>(s_cfg.height);
            const float uh = static_cast<float>(s_cfg.up_height);
            const float t  = static_cast<float>(s_cfg.tilt);
            const float p  = static_cast<float>(s_cfg.period);

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(0,  t - 2.0f * t * std::sin(tt), h - uh * std::sin(tt));
                rear_left_ik(0,    t - 2.0f * t * std::sin(tt), h - uh * std::sin(tt));
                rear_right_ik(0, -t * std::cos(tt), h - uh * std::cos(tt));
                front_left_ik(0, -t * std::cos(tt), h - uh * std::cos(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                rear_right_ik(0,  t * std::sin(tt), h);
                front_left_ik(0,  t * std::sin(tt), h);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(0, -t * std::cos(tt), h - uh * std::cos(tt));
                rear_left_ik(0,   -t * std::cos(tt), h - uh * std::cos(tt));
                rear_right_ik(0,  t - 2.0f * t * std::sin(tt), h - uh * std::sin(tt));
                front_left_ik(0,  t - 2.0f * t * std::sin(tt), h - uh * std::sin(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(0,  t * std::sin(tt), h);
                rear_left_ik(0,    t * std::sin(tt), h);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            continue;
        }

        // ── Turn Right ───────────────────────────────────────────
        if (cmd == GaitCmd::TurnR) {
            const float h  = static_cast<float>(s_cfg.height);
            const float uh = static_cast<float>(s_cfg.up_height);
            const float t  = static_cast<float>(s_cfg.tilt);
            const float p  = static_cast<float>(s_cfg.period);

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(0, -t + 2.0f * t * std::sin(tt), h - uh * std::sin(tt));
                rear_left_ik(0,   -t + 2.0f * t * std::sin(tt), h - uh * std::sin(tt));
                rear_right_ik(0,  t * std::cos(tt), h - uh * std::cos(tt));
                front_left_ik(0,  t * std::cos(tt), h - uh * std::cos(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                rear_right_ik(0, -t * std::sin(tt), h);
                front_left_ik(0, -t * std::sin(tt), h);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(0,  t * std::cos(tt), h - uh * std::cos(tt));
                rear_left_ik(0,    t * std::cos(tt), h - uh * std::cos(tt));
                rear_right_ik(0, -t + 2.0f * t * std::sin(tt), h - uh * std::sin(tt));
                front_left_ik(0, -t + 2.0f * t * std::sin(tt), h - uh * std::sin(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(0, -t * std::sin(tt), h);
                rear_left_ik(0,   -t * std::sin(tt), h);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            continue;
        }

        // ── Twerk ────────────────────────────────────────────────
        if (cmd == GaitCmd::Twerk) {
            const float h  = static_cast<float>(s_cfg.height);
            const float uh = static_cast<float>(s_cfg.up_height);
            const float p  = static_cast<float>(s_cfg.period);

            // Phase 1: crouch front
            time_mSt = millis(); tim = 0;
            while (tim < p * 4.0f) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / (p * 4.0f);
                front_right_ik(0, 0, h - uh * std::sin(tt));
                front_left_ik(0,  0, h - uh * std::sin(tt));
                rear_right_ik(0, 0, h + uh * std::sin(tt));
                rear_left_ik(0,  0, h + uh * std::sin(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            // Phase 2: wiggle
            time_mSt = millis(); tim = 0;
            while (tim < p * 6.0f) { tim = millis() - time_mSt; tt = tim * 2.0f * PI / p;
                front_right_ik(0, 0, h - uh);
                front_left_ik(0,  0, h - uh);
                rear_right_ik(0, 0, h + uh * (1.0f + 0.5f * std::sin(tt)));
                rear_left_ik(0,  0, h + uh * (1.0f + 0.5f * std::sin(tt)));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            // Phase 3: return
            time_mSt = millis(); tim = 0;
            while (tim < p * 4.0f) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / (p * 4.0f);
                front_right_ik(0, 0, h - uh * std::cos(tt));
                front_left_ik(0,  0, h - uh * std::cos(tt));
                rear_right_ik(0, 0, h + uh * std::cos(tt));
                rear_left_ik(0,  0, h + uh * std::cos(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            s_gait_cmd = GaitCmd::None;
            continue;
        }

        // ── Jump (vertical) ──────────────────────────────────────
        if (cmd == GaitCmd::Jump) {
            const float h       = static_cast<float>(s_cfg.height);
            const float p       = static_cast<float>(s_cfg.period);
            const float crouchZ = 40.0f;
            const float pushZ   = 105.0f;
            const float tuckZ   = 45.0f;

            // Crouch
            time_mSt = millis(); tim = 0;
            while (tim < p * 2.0f) { tim = millis() - time_mSt;
                tt = tim * PI / 2.0f / (p * 2.0f);
                float z = h - (h - crouchZ) * std::sin(tt);
                front_right_ik(0, 0, z); front_left_ik(0, 0, z);
                rear_right_ik(0, 0, z);  rear_left_ik(0, 0, z);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            // Hold crouch
            time_mSt = millis(); tim = 0;
            while (tim < 20.0f) { tim = millis() - time_mSt;
                front_right_ik(0, 0, crouchZ); front_left_ik(0, 0, crouchZ);
                rear_right_ik(0, 0, crouchZ);  rear_left_ik(0, 0, crouchZ);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            // Explosive extension
            set_all_servo_speed(0);
            front_right_ik(0, 0, pushZ); front_left_ik(0, 0, pushZ);
            rear_right_ik(0, 0, pushZ);  rear_left_ik(0, 0, pushZ);
            flush();
            flush();

            int air_ms = static_cast<int>(160.0f + (70.0f - crouchZ) * 1.0f);
            vTaskDelay(pdMS_TO_TICKS(air_ms));

            // Tuck in air
            time_mSt = millis(); tim = 0;
            const float tuck_ms = 50.0f;
            while (tim < tuck_ms) { tim = millis() - time_mSt;
                float frac = std::sin(tim * PI / 2.0f / tuck_ms);
                float z = pushZ - (pushZ - tuckZ) * frac;
                front_right_ik(0, 0, z); front_left_ik(0, 0, z);
                rear_right_ik(0, 0, z);  rear_left_ik(0, 0, z);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            // Soft landing
            time_mSt = millis(); tim = 0;
            while (tim < p * 3.0f) { tim = millis() - time_mSt;
                tt = tim * PI / 2.0f / (p * 3.0f);
                float z = tuckZ + (h - tuckZ) * std::sin(tt);
                front_right_ik(0, 0, z); front_left_ik(0, 0, z);
                rear_right_ik(0, 0, z);  rear_left_ik(0, 0, z);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            s_gait_cmd = GaitCmd::None;
            continue;
        }

        // ── Jump Forward ─────────────────────────────────────────
        if (cmd == GaitCmd::JumpFwd) {
            const float h             = static_cast<float>(s_cfg.height);
            const float p             = static_cast<float>(s_cfg.period);
            const float crouchZrear   = 60.0f;
            const float crouchZfront  = 40.0f;
            const float pushZ         = 105.0f;
            const float tuckZ         = 45.0f;

            // Phase 1: controlled crouch
            set_all_servo_speed(100);
            front_right_ik(0, 0, crouchZfront); front_left_ik(0, 0, crouchZfront);
            rear_right_ik(0, 0, crouchZrear);   rear_left_ik(0, 0, crouchZrear);
            flush();
            vTaskDelay(pdMS_TO_TICKS(900));

            time_mSt = millis(); tim = 0;
            while (tim < 25.0f) { tim = millis() - time_mSt;
                front_right_ik(0, 0, crouchZfront); front_left_ik(0, 0, crouchZfront);
                rear_right_ik(0, 0, crouchZrear);   rear_left_ik(0, 0, crouchZrear);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            // Phase 2: explosive extension
            set_all_servo_speed(0);
            front_right_ik(0, 0, pushZ); front_left_ik(0, 0, pushZ);
            rear_right_ik(0, 0, pushZ);  rear_left_ik(0, 0, pushZ);
            flush();
            flush();

            int air_ms = static_cast<int>(160.0f + (70.0f - crouchZrear) * 1.0f);
            vTaskDelay(pdMS_TO_TICKS(air_ms));

            // Phase 3: pounce tuck (rear first, then front)
            const float rear_tuck_ms = 45.0f;
            time_mSt = millis(); tim = 0;
            while (tim < rear_tuck_ms) { tim = millis() - time_mSt;
                float frac = std::sin(tim * PI / 2.0f / rear_tuck_ms);
                float zr = pushZ - (pushZ - tuckZ) * frac;
                rear_right_ik(0, 0, zr);  rear_left_ik(0, 0, zr);
                front_right_ik(0, 0, pushZ); front_left_ik(0, 0, pushZ);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            const float front_tuck_ms = 45.0f;
            time_mSt = millis(); tim = 0;
            while (tim < front_tuck_ms) { tim = millis() - time_mSt;
                float frac = std::sin(tim * PI / 2.0f / front_tuck_ms);
                float zf = pushZ - (pushZ - tuckZ) * frac;
                front_right_ik(0, 0, zf); front_left_ik(0, 0, zf);
                rear_right_ik(0, 0, tuckZ);  rear_left_ik(0, 0, tuckZ);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            // Phase 4: soft landing
            set_all_servo_speed(0);
            front_right_ik(0, 0, h); front_left_ik(0, 0, h);
            rear_right_ik(0, 0, h);  rear_left_ik(0, 0, h);
            flush();
            vTaskDelay(pdMS_TO_TICKS(p * 4.0f));
            set_all_servo_speed(0);
            s_gait_cmd = GaitCmd::None;
            continue;
        }

        // ── Speed Test ───────────────────────────────────────────
        if (cmd == GaitCmd::TestSpeed) {
            ESP_LOGI(TAG, "--- Speed Test START ---");
            set_all_servo_speed(2047);
            front_right_ik(0, 0, 70); front_left_ik(0, 0, 70);
            rear_right_ik(0, 0, 70);  rear_left_ik(0, 0, 70);
            flush();
            vTaskDelay(pdMS_TO_TICKS(1500));

            ESP_LOGI(TAG, "Moving SLOW (speed=30)");
            set_all_servo_speed(30);
            front_right_ik(0, 0, 100); front_left_ik(0, 0, 100);
            rear_right_ik(0, 0, 100);  rear_left_ik(0, 0, 100);
            flush();
            vTaskDelay(pdMS_TO_TICKS(3000));

            ESP_LOGI(TAG, "Moving FAST (speed=2047)");
            set_all_servo_speed(2047);
            front_right_ik(0, 0, 70); front_left_ik(0, 0, 70);
            rear_right_ik(0, 0, 70);  rear_left_ik(0, 0, 70);
            flush();
            vTaskDelay(pdMS_TO_TICKS(2000));

            ESP_LOGI(TAG, "--- Speed Test DONE ---");
            set_all_servo_speed(0);
            s_gait_cmd = GaitCmd::None;
            continue;
        }

        // ═══════════════════════════════════════════════════════════
        //  Imported from StanfordQuadruped
        // ═══════════════════════════════════════════════════════════

        // ── Look / head poses (static body-attitude holds) ───────
        // Direct roll/pitch/yaw pose requested by the SDK. Repeated calls can
        // update the target while this command is active; any gait command
        // interrupts it and the normal Stanford park transition runs.
        if (cmd == GaitCmd::BodyAttitude) {
            const float h = static_cast<float>(s_cfg.height);
            set_all_servo_speed(0);

            sg_foot_t pose[4];
            // Snapshot the per-axis speeds (deg/s). 0 on an axis = instant.
            const float rdps = s_body_slew_roll_dps;
            const float pdps = s_body_slew_pitch_dps;
            const float ydps = s_body_slew_yaw_dps;
            const bool any_slew = (rdps > 0.0f || pdps > 0.0f || ydps > 0.0f);

            if (!any_slew) {
                // All axes instant: ease in once (period-based ramp), then
                // hold, applying any live target updates immediately (snap).
                sg_attitude(s_body_roll_deg, s_body_pitch_deg,
                            s_body_yaw_deg, h, pose);
                sg_ramp_to(pose, sg_ramp_ms());

                while (s_gait_cmd == cmd) {
                    sg_attitude(s_body_roll_deg, s_body_pitch_deg,
                                s_body_yaw_deg, h, pose);
                    sg_write(pose);
                    flush();
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
                sg_park(h);
                continue;
            }

            // Per-axis speed-limited slew: glide each axis toward its (possibly
            // changing) target at its own deg/second so repeated pose updates
            // ease smoothly. An axis with speed 0 uses a huge step, i.e. it
            // snaps instantly while the others glide. Start from the rest
            // attitude we parked at, so the first move eases in too.
            s_body_roll_cur  = 0.0f;
            s_body_pitch_cur = 0.0f;
            s_body_yaw_cur   = 0.0f;
            const float BIG   = 1.0e6f;
            const float rstep = (rdps > 0.0f) ? rdps * 0.02f : BIG;  // deg/20ms
            const float pstep = (pdps > 0.0f) ? pdps * 0.02f : BIG;
            const float ystep = (ydps > 0.0f) ? ydps * 0.02f : BIG;

            while (s_gait_cmd == cmd) {
                s_body_roll_cur  = step_toward(s_body_roll_cur,
                                               s_body_roll_deg,  rstep);
                s_body_pitch_cur = step_toward(s_body_pitch_cur,
                                               s_body_pitch_deg, pstep);
                s_body_yaw_cur   = step_toward(s_body_yaw_cur,
                                               s_body_yaw_deg,   ystep);
                sg_attitude(s_body_roll_cur, s_body_pitch_cur,
                            s_body_yaw_cur, h, pose);
                sg_write(pose);
                flush();
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            sg_park(h);
            continue;
        }

        if (cmd == GaitCmd::LookUp     || cmd == GaitCmd::LookDown  ||
            cmd == GaitCmd::LookLeft   || cmd == GaitCmd::LookRight ||
            cmd == GaitCmd::LookUpperLeft  || cmd == GaitCmd::LookUpperRight ||
            cmd == GaitCmd::LookLowerLeft  || cmd == GaitCmd::LookLowerRight) {

            float pitch = 0.0f, yaw = 0.0f;
            switch (cmd) {
                case GaitCmd::LookUp:         pitch =  LOOK_PITCH_DEG; break;
                case GaitCmd::LookDown:       pitch = -LOOK_PITCH_DEG; break;
                case GaitCmd::LookLeft:       yaw   = -LOOK_YAW_DEG;   break;
                case GaitCmd::LookRight:      yaw   =  LOOK_YAW_DEG;   break;
                case GaitCmd::LookUpperLeft:  pitch =  LOOK_PITCH_DEG; yaw = -LOOK_YAW_DEG; break;
                case GaitCmd::LookUpperRight: pitch =  LOOK_PITCH_DEG; yaw =  LOOK_YAW_DEG; break;
                case GaitCmd::LookLowerLeft:  pitch = -LOOK_PITCH_DEG; yaw = -LOOK_YAW_DEG; break;
                case GaitCmd::LookLowerRight: pitch = -LOOK_PITCH_DEG; yaw =  LOOK_YAW_DEG; break;
                default: break;
            }

            // Exact StanfordQuadruped attitude via the Stanford leg IK:
            // ease into the pose, hold it, then ease back to the stand.
            const float h = static_cast<float>(s_cfg.height);
            set_all_servo_speed(0);

            sg_foot_t pose[4];
            sg_attitude(0.0f, pitch, yaw, h, pose);
            sg_ramp_to(pose, sg_ramp_ms());                  // smooth head move in

            while (s_gait_cmd == cmd) {             // hold the pose
                sg_write(pose);
                flush();
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            sg_park(h);                             // Stanford-style return
            continue;
        }

        // ── Single-leg lifts (static hold) ───────────────────────
        if (cmd == GaitCmd::ForelegLiftL || cmd == GaitCmd::ForelegLiftR ||
            cmd == GaitCmd::BacklegLiftL || cmd == GaitCmd::BacklegLiftR) {

            const float h    = static_cast<float>(s_cfg.height);
            const float lift = 30.0f;   // how far the paw rises (mm)

            // 0 = FR, 1 = FL, 2 = RR, 3 = RL
            int leg = 0;
            switch (cmd) {
                case GaitCmd::ForelegLiftR: leg = 0; break;
                case GaitCmd::ForelegLiftL: leg = 1; break;
                case GaitCmd::BacklegLiftR: leg = 2; break;
                case GaitCmd::BacklegLiftL: leg = 3; break;
                default: break;
            }
            const bool front = (leg == 0 || leg == 1);
            const bool left  = (leg == 1 || leg == 3);

            // Shift weight away from the lifted leg for balance.
            const float pitch = front ? -6.0f :  6.0f;  // lifting front -> lean back
            const float roll  = left  ?  6.0f : -6.0f;  // lifting left  -> lean right

            // Weight-shift attitude + raised paw, all through the exact
            // Stanford IK: ease in, hold, ease back to the stand.
            set_all_servo_speed(0);

            sg_foot_t pose[4];
            sg_attitude(roll, pitch, 0.0f, h, pose);
            pose[leg].z -= lift;                    // raise the chosen paw

            sg_ramp_to(pose, sg_ramp_ms());
            while (s_gait_cmd == cmd) {
                sg_write(pose);
                flush();
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            sg_park(h);
            continue;
        }

        // ── Static body height up / down (Stanford IK) ───────────
        if (cmd == GaitCmd::HeightUp || cmd == GaitCmd::HeightDown) {
            const float h = static_cast<float>(s_cfg.height);
            const float d = 20.0f;
            const float z = (cmd == GaitCmd::HeightUp) ? (h + d) : (h - d);

            set_all_servo_speed(0);
            sg_foot_t pose[4];
            sg_rest(z, pose);
            sg_ramp_to(pose, sg_ramp_ms());
            while (s_gait_cmd == cmd) {
                sg_write(pose);
                flush();
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            sg_park(h);
            continue;
        }

        // ── Balance: hold a roll + pitch attitude (Stanford IK) ──
        if (cmd == GaitCmd::Balance) {
            const float h = static_cast<float>(s_cfg.height);
            const float t = static_cast<float>(s_cfg.tilt);

            set_all_servo_speed(0);
            sg_foot_t pose[4];
            sg_attitude(t, t, 0.0f, h, pose);
            sg_ramp_to(pose, sg_ramp_ms());
            while (s_gait_cmd == cmd) {
                sg_write(pose);
                flush();
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            sg_park(h);
            continue;
        }

        // ── Bow + shuffle back, then rise (auto-return, Stanford IK) ─
        if (cmd == GaitCmd::BowBack) {
            const float h         = static_cast<float>(s_cfg.height);
            const float p         = static_cast<float>(s_cfg.period);
            const float bow       = 25.0f;   // front crouch (mm)
            const float backshift = 15.0f;   // body shifts back (mm)

            set_all_servo_speed(0);

            // Bow pose: feet forward relative to the body (body shifts
            // back), front lowered.
            sg_foot_t pose[4];
            sg_rest(h, pose);
            for (int l = 0; l < 4; ++l) pose[l].x = backshift;
            pose[0].z = h - bow;   // FR
            pose[1].z = h - bow;   // FL

            // Lower the front and shift weight backward (sin-eased)
            sg_ramp_to(pose, static_cast<int>(p * 4.0f));
            if (s_gait_cmd != cmd) { sg_park(h); continue; }

            // Hold the bow briefly
            time_mSt = millis(); tim = 0;
            while (tim < 600.0f) { tim = millis() - time_mSt;
                sg_write(pose);
                flush();
                if (s_gait_cmd != cmd) break;
                vTaskDelay(pdMS_TO_TICKS(20));
            }

            // Rise back to neutral — the Stanford-style "goes back"
            sg_foot_t rest[4];
            sg_rest(h, rest);
            sg_ramp_to(rest, static_cast<int>(p * 4.0f));
            s_sg_valid = false;
            s_gait_cmd = GaitCmd::None;
            continue;
        }

        // ── Body cycle: body centre draws a circle (auto-return,
        //    exact Stanford IK — lateral moves use the true abduction
        //    solve instead of the old hip-angle approximation) ──────
        if (cmd == GaitCmd::BodyCycle) {
            const float h   = static_cast<float>(s_cfg.height);
            const float p   = static_cast<float>(s_cfg.period) * 8.0f;
            const float Rad = 15.0f;   // circle radius (mm)
            set_all_servo_speed(0);

            // Ease to the circle start (tt = 0: feet at x = -Rad).
            sg_foot_t pose[4];
            sg_rest(h, pose);
            for (int l = 0; l < 4; ++l) pose[l].x = -Rad;
            sg_ramp_to(pose, sg_ramp_ms());

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * 2.0f * PI / p;
                // Feet move opposite to the body centre (+y = left).
                for (int l = 0; l < 4; ++l) {
                    pose[l].x = -Rad * std::cos(tt);
                    pose[l].y = -Rad * std::sin(tt);
                    pose[l].z = h;
                }
                sg_write(pose);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            sg_park(h);                 // Stanford-style return to stand
            s_gait_cmd = GaitCmd::None;
            continue;
        }

        // ── Head ellipse: head sweeps an ellipse (auto-return) ───
        // Exact StanfordQuadruped behaviour: ease into the ellipse,
        // sweep it, then GO BACK smoothly to the neutral stand.
        if (cmd == GaitCmd::HeadEllipse) {
            const float h = static_cast<float>(s_cfg.height);
            const float p = static_cast<float>(s_cfg.period) * 8.0f;
            set_all_servo_speed(0);

            // Ease to the ellipse start (tt = 0: pitch 0, yaw = max).
            sg_foot_t pose[4];
            sg_attitude(0.0f, 0.0f, LOOK_YAW_DEG, h, pose);
            sg_ramp_to(pose, sg_ramp_ms());

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * 2.0f * PI / p;
                float pitch = LOOK_PITCH_DEG * std::sin(tt);
                float yaw   = LOOK_YAW_DEG   * std::cos(tt);
                sg_attitude(0.0f, pitch, yaw, h, pose);
                sg_write(pose);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            sg_park(h);                 // after the ellipse it goes back
            s_gait_cmd = GaitCmd::None;
            continue;
        }

        // ── Diagonal trot walks (Stanford gait, vx + vy) ─────────
        // The Stanford trot natively supports lateral velocity, so the
        // diagonals are simply the walk with a combined vx/vy command —
        // exactly what StanfordQuadruped does with a diagonal stick.
        if (cmd == GaitCmd::MoveLeftFront  || cmd == GaitCmd::MoveRightFront ||
            cmd == GaitCmd::MoveLeftBack   || cmd == GaitCmd::MoveRightBack) {

            const float h  = static_cast<float>(s_cfg.height);

            // xf: +1 forward, -1 backward.  yf: +1 left, -1 right.
            const float xf = (cmd == GaitCmd::MoveLeftFront || cmd == GaitCmd::MoveRightFront) ? 1.0f : -1.0f;
            const float yf = (cmd == GaitCmd::MoveLeftFront || cmd == GaitCmd::MoveLeftBack)   ? 1.0f : -1.0f;

            // Diagonal speed comes from the SAME adjustable walk speed
            // (web "Walk speed" slider, Config::sg_speed, mm/s).
            const float v  = static_cast<float>(s_cfg.sg_speed) * 0.7071f;
            const float vx = xf * v;
            float       vy = yf * v;
            if (SG_MIRROR_LR) vy = -vy;    // reference L/R mirror (see robot.h)

            set_all_servo_speed(0);
            stanford_gait_reset(h);

            int64_t next_us = esp_timer_get_time();
            while (s_gait_cmd == cmd) {
                sg_foot_t feet[4];
                stanford_gait_step(vx, vy, 0.0f, h, SG_NATIVE_CLEARANCE_MM, feet);
                sg_write(feet);
                flush();

                // Pace to the next 15 ms gait tick; resync if far behind.
                next_us += static_cast<int64_t>(SG_DT * 1e6f);
                int64_t now = esp_timer_get_time();
                if (now > next_us + 100000) next_us = now;
                while (esp_timer_get_time() < next_us && s_gait_cmd == cmd) {
                    vTaskDelay(1);
                }
            }
            sg_park(h);    // park mid-stride feet back in the stand
            continue;
        }

        // ── Stanford Pupper trot walk (exact StanfordQuadruped port) ─
        // Phase scheduler + stance/swing controllers from Gaits.py /
        // StanceController.py / SwingLegController.py, feet solved by the
        // exact mini_pupper BSP IK.  Runs at the native 15 ms tick with
        // the native clearance.
        //
        // Speed comes from the web "Walk speed" slider (Config::sg_speed).
        // Velocity command each tick (same scheme as the reference /js):
        //   - fresh joystick input  -> full 3-axis command from the pads
        //   - joystick released     -> step in place (zeros stay fresh /
        //                              joystick-started walk went stale)
        //   - started by the button -> steady forward walk at sg_speed
        if (cmd == GaitCmd::StanfordWalk) {
            const float h = static_cast<float>(s_cfg.height);

            set_all_servo_speed(0);

            // From the stand this is already the walk stance (the gait
            // rests at feet (0,0,h)), so the walk starts directly from
            // the pose the robot is in — same as the reference repo.
            stanford_gait_reset(h);

            int64_t next_us = esp_timer_get_time();
            while (s_gait_cmd == cmd) {
                const float sg = static_cast<float>(s_cfg.sg_speed);
                float vx, vy, wz;
                if (millis() - s_js_last_ms < JOY_TIMEOUT_MS) {
                    // Fresh joystick: full stick == sg_speed; strafe and
                    // yaw scaled proportionally so turning matches.
                    const float k = sg / SG_SPEED_MAX_MM_S;
                    vx = s_js_f * sg;
                    // Invert lateral (strafe) joystick for Stanford walk —
                    // user reported left/right reversed when using the
                    // Stanford gait via the joystick pads.
                    vy = -s_js_s * JOY_VY_MAX * k;
                    wz = s_js_t * JOY_WZ_MAX * k;
                } else if (s_js_active) {
                    // Joystick-started walk, input gone stale: step in
                    // place instead of surprising the user by walking.
                    vx = vy = wz = 0.0f;
                } else {
                    // Button-started walk: steady forward trot.
                    vx = sg; vy = 0.0f; wz = 0.0f;
                }
                if (SG_MIRROR_LR) { vy = -vy; wz = -wz; }   // see robot.h

                sg_foot_t feet[4];
                stanford_gait_step(vx, vy, wz, h, SG_NATIVE_CLEARANCE_MM, feet);
                sg_write(feet);
                flush();

                // Pace to the next 15 ms gait tick; resync if far behind.
                next_us += static_cast<int64_t>(SG_DT * 1e6f);
                int64_t now = esp_timer_get_time();
                if (now > next_us + 100000) next_us = now;
                while (esp_timer_get_time() < next_us && s_gait_cmd == cmd) {
                    vTaskDelay(1);
                }
            }
            s_js_active = false;
            sg_park(h);    // park mid-stride feet back in the stand
            continue;
        }

        // ═══════════════════════════════════════════════════════════
        //  FPC choreography (MangDang MovementGroups.py), driven by the
        //  exact Stanford IK.  FPC coordinates are body-frame metres;
        //  converted here to per-hip mm (body = hip origin + local).
        // ═══════════════════════════════════════════════════════════

        // ── Front kick: rear up like a horse (auto-return) ───────
        // FPC front_kick(ht=0.04, pitch=15): front feet reach forward
        // ([0.12, ±0.06] body frame) and lift 40 mm, rear legs squat
        // 20 mm, body pitches 15° nose-up.  Hold ~1 s, then return to
        // the stand (front_kick_to_stand).
        if (cmd == GaitCmd::FrontKick) {
            const float h = static_cast<float>(s_cfg.height);
            set_all_servo_speed(0);

            sg_foot_t base[4];
            sg_rest(h, base);
            base[0].x = 61.0f;  base[0].y = -10.5f; base[0].z = h - 40.0f; // FR paw fwd+up
            base[1].x = 61.0f;  base[1].y = +10.5f; base[1].z = h - 40.0f; // FL paw fwd+up
            base[2].x = -1.0f;  base[2].y = -0.5f;  base[2].z = h + 20.0f; // RR squat
            base[3].x = -1.0f;  base[3].y = +0.5f;  base[3].z = h + 20.0f; // RL squat

            sg_foot_t pose[4];
            sg_attitude_feet(0.0f, 15.0f, 0.0f, base, pose);   // nose up 15°
            sg_ramp_to(pose, sg_ramp_ms());                             // rear up

            // Hold the kick pose ~1 s (interruptible)
            time_mSt = millis(); tim = 0;
            while (tim < 1000.0f) { tim = millis() - time_mSt;
                sg_write(pose);
                flush();
                if (s_gait_cmd != cmd) break;
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            sg_park(h);                    // front_kick_to_stand
            s_gait_cmd = GaitCmd::None;
            continue;
        }

        // ── Wiggle / butt shrug: pitched body + yaw tail-wag ─────
        // FPC wiggle_trajectory: butt up (pitch -25) with the yaw
        // sweeping side to side; butt_shrug_trajectory: nose up
        // (pitch +25) with the same sweep.  Angles softened slightly
        // (-22/+20, yaw ±15) to stay well inside this servo's range.
        // Runs continuously while held, then returns to the stand.
        if (cmd == GaitCmd::Wiggle) {
            const float h     = static_cast<float>(s_cfg.height);
            const float pitch = -22.0f;
            const float yawA  = 15.0f;
            const float p     = static_cast<float>(s_cfg.period) * 8.0f;
            set_all_servo_speed(0);

            sg_foot_t pose[4];
            sg_attitude(0.0f, pitch, 0.0f, h, pose);
            sg_ramp_to(pose, sg_ramp_ms());          // ease into the pitched pose

            time_mSt = millis();
            while (s_gait_cmd == cmd) {     // wag until released
                tim = millis() - time_mSt; tt = tim * 2.0f * PI / p;
                const float yaw = yawA * std::sin(tt);
                sg_attitude(0.0f, pitch, yaw, h, pose);
                sg_write(pose);
                flush();
                vTaskDelay(pdMS_TO_TICKS(20));   // pace the wag like every other hold loop
            }
            sg_park(h);
            continue;
        }

        // Butt shrug is a separate front-up trajectory. Keeping its own
        // branch makes its timing and amplitudes independent from wiggle.
        if (cmd == GaitCmd::ButtShrug) {
            const float h     = static_cast<float>(s_cfg.height);
            const float pitch = 20.0f;
            const float yawA  = 15.0f;
            const float p     = static_cast<float>(s_cfg.period) * 8.0f;
            set_all_servo_speed(0);

            sg_foot_t pose[4];
            sg_attitude(0.0f, pitch, 0.0f, h, pose);
            sg_ramp_to(pose, sg_ramp_ms());

            time_mSt = millis();
            while (s_gait_cmd == cmd) {
                tim = millis() - time_mSt;
                tt = tim * 2.0f * PI / p;
                const float yaw = yawA * std::sin(tt);
                sg_attitude(0.0f, pitch, yaw, h, pose);
                sg_write(pose);
                flush();
                vTaskDelay(pdMS_TO_TICKS(20));   // pace the wag like every other hold loop
            }
            sg_park(h);
            continue;
        }

        // ── One-sided wiggle / butt shrug (FPC wiggle_left/right,
        //    butt_shrug_left/right): pitched body with the yaw swept
        //    to ONE side and held there.  The FPC versions step the
        //    yaw 0→±20 through intermediate poses; the entry ramp here
        //    sweeps through the same path.  Hold while active, then
        //    return to the stand.
        if (cmd == GaitCmd::WiggleLeft    || cmd == GaitCmd::WiggleRight ||
            cmd == GaitCmd::ButtShrugLeft || cmd == GaitCmd::ButtShrugRight) {

            const float h = static_cast<float>(s_cfg.height);
            const bool wig  = (cmd == GaitCmd::WiggleLeft ||
                               cmd == GaitCmd::WiggleRight);
            const bool left = (cmd == GaitCmd::WiggleLeft ||
                               cmd == GaitCmd::ButtShrugLeft);
            // FPC: wiggle pitch -25, shrug +25, yaw to ±20; softened to
            // the verified-safe -22/+20 and ±15 for this 0–180° servo.
            const float pitch = wig  ? -22.0f : 20.0f;
            const float yaw   = left ? +15.0f : -15.0f;   // FPC sign convention

            set_all_servo_speed(0);
            sg_foot_t pose[4];
            sg_attitude(0.0f, pitch, yaw, h, pose);
            sg_ramp_to(pose, sg_ramp_ms());          // sweep pitch+yaw in like FPC
            while (s_gait_cmd == cmd) {     // hold the side pose
                sg_write(pose);
                flush();
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            sg_park(h);
            continue;
        }

        // ── Unknown / fallback ───────────────────────────────────
        s_gait_cmd = GaitCmd::None;
    }
}

}  // namespace robot
