#pragma once

#include "sdkconfig.h"
#include <cstdint>

#include "robot/board_config.h"

namespace robot {

// ── Forward declarations ─────────────────────────────────────
struct ImuData;

}  // namespace robot

#include "robot/imu.h"

namespace robot {

// ── Robot constants ──────────────────────────────────────────
constexpr float L1 = 50.0f;   // Upper leg length (mm)
constexpr float L2 = 56.0f;   // Lower leg length (mm)
constexpr float PI = 3.14159265358979f;

// ── Servo signal range ───────────────────────────────────────
//
// There is no fixed range here any more, and so no SERVO_DEG_TO_RAW.
//
// The twelve servos are driven by four AT32F413 boards over SPI (see
// robot/driver_board.h), and those boards hold their OWN range_position_deg in
// their flash, where it survives any amount of ESP reflashing. robot.cc asks
// them what it is at boot (driver_board_probe_scale()) and derives
// ticks-per-degree from the answer:
//
//     ticks_per_deg = 10240 / driver_board_get_scale_dd()
//
// The Mini Pupper 2 build this replaced could hardcode 1023/180, because a
// Feetech SCSCL servo's range is a physical fact of the servo. Here it is a
// configuration value on another chip, and assuming one overdrives every joint
// by the ratio - which reads as "the new servos are weaker", not as "the
// scale is wrong".
//
// IK angles are commanded RELATIVE to centre either way, so the usable command
// range is a little over +/-150 deg at the stock 310 deg, minus whatever a
// range trim has already removed.

// ── The two raw position frames, and how to move between them ────
//
// There are TWO raw position frames and they are mirror images of each other.
// Mixing them is not a compile error, so name the conversion instead of
// writing it out:
//
//   GAIT frame - what s_goal_pos[] and set_servo_angle() speak.
//                0..1023, 511 = centre, positive `deg` increases the value.
//
//   AT32 frame - what the driver boards, driver_board_present_position() and
//                Servo Studio speak. 0..1023, 511 = centre.
//
// driver_board_sync_write() converts gait -> AT32 with a direction flip
// (scale_dd - pos * scale_dd / 1024), and the feedback path deliberately does
// NOT flip on the way back, which is what makes the round trip exact. The net
// effect is simply that the two frames run in opposite directions:
//
//     at32_raw == 1024 - gait_raw
//
// Anything comparing a commanded position against a measured one MUST put both
// in the same frame first.
constexpr int gait_raw_to_at32_raw(int gait_raw) { return 1024 - gait_raw; }
constexpr int at32_raw_to_gait_raw(int at32_raw) { return 1024 - at32_raw; }

// ── Neutral stand calibration (like the reference minipupperesp) ─
// The robot is physically calibrated so ALL SERVOS CENTRED == the
// standing pose at this height.  The leg IK subtracts the neutral
// joint angles of this stance so that front_right_ik(0,0,NEUTRAL_Z)
// == centred servos == the power-on pose.  This makes the stand
// phase after start-up identical to the reference repo: the robot
// simply holds/settles into the stand with no repositioning jump.
constexpr float NEUTRAL_Z = 70.0f;

// ── Stanford walk parameters ─────────────────────────────────
// The walk speed itself is RUNTIME-ADJUSTABLE: Config::sg_speed
// (mm/s), set from the web UI "Walk speed" slider and persisted in
// NVS.  Full-stick maximum is 200 mm/s per the Mini Pupper BSP
// Config.py; the reference web default was 100 which is too fast
// for this robot, so the default here is 50.
constexpr float SG_SPEED_MAX_MM_S = 200.0f;   // full joystick stick
// The reference robot's leg roles are mirrored left<->right vs the
// physical legs, so lateral/yaw commands are flipped there.  Keep
// the same default; set to false if strafe/turn comes out mirrored.
constexpr bool  SG_MIRROR_LR = true;

// ── Web joystick (mini_pupper_web_controller style) ──────────
// Left pad: forward/strafe, right pad: turn.  Values -1..1, scaled
// by Config::sg_speed.  Fresh input drives the Stanford walk; when
// the pads are released (zeros / timeout) the robot steps in place.
constexpr float JOY_VY_MAX     = 200.0f;   // mm/s strafe at full stick
constexpr float JOY_WZ_MAX     = 2.0f;     // rad/s yaw at full stick
constexpr uint32_t JOY_TIMEOUT_MS = 600;

// ── Body geometry for attitude (look) poses ──────────────────
// NOTE: superseded — the look/imported-move poses now use the exact
// Stanford geometry (SG_ORIGIN_X/SG_ORIGIN_Y in stanford_gait.h).
// Kept only for reference.
constexpr float BODY_LX = 50.0f;   // half fore-aft hip spacing (mm)
constexpr float BODY_LY = 35.0f;   // half left-right hip spacing (mm)

// Direction sign flips for the look poses.  If the robot looks the
// OPPOSITE way to what a command says, flip that axis sign.
// (Verified in simulation: PITCH_SIGN = -1 makes LookUp raise the nose.)
constexpr float PITCH_SIGN = -1.0f; // LookUp raises the nose
constexpr float YAW_SIGN   =  1.0f; // LookRight aims head right (flip if reversed)
constexpr float ROLL_SIGN  =  1.0f; // positive roll leans right (flip if reversed)

// Default look amplitudes (degrees), matching StanfordQuadruped.
constexpr float LOOK_PITCH_DEG = 20.0f;
constexpr float LOOK_YAW_DEG   = 30.0f;

// ── Gait command enum ────────────────────────────────────────
enum class GaitCmd : uint8_t {
    None,
    Init,        // Initial pose (zero offsets)
    Step,        // In-place stepping (trot)
    Roll,        // Body roll
    Pitch,       // Body pitch
    Stretch,     // Body stretch (up/down)
    Advance,     // Walk forward
    Back,        // Walk backward
    Left,        // Strafe left
    Right,       // Strafe right
    TurnL,       // Turn left (spin)
    TurnR,       // Turn right (spin)
    Twerk,       // Twerk / wiggle
    Jump,        // Vertical jump
    JumpFwd,     // Jump forward
    TestSpeed,   // Servo speed test

    // ── Imported from StanfordQuadruped (body-attitude poses) ────
    // These hold a static "head aiming" pose by rotating the body
    // while the feet stay planted (Stanford's roll/pitch/yaw model
    // converted to this robot's per-leg IK).  All static-hold.
    LookUp,         // Pitch nose up
    LookDown,       // Pitch nose down
    LookLeft,       // Yaw head left
    LookRight,      // Yaw head right
    LookUpperLeft,  // Up + left
    LookUpperRight, // Up + right
    LookLowerLeft,  // Down + left
    LookLowerRight, // Down + right

    // ── Single-leg lifts (raise one paw while standing) ──────────
    ForelegLiftL,   // Lift front-left paw
    ForelegLiftR,   // Lift front-right paw
    BacklegLiftL,   // Lift rear-left paw
    BacklegLiftR,   // Lift rear-right paw

    // ── Static body height ───────────────────────────────────────
    HeightUp,       // Raise body
    HeightDown,     // Lower body

    // ── Static combined roll+pitch hold ──────────────────────────
    Balance,        // Hold a roll+pitch attitude

    // ── Level-3 choreography (dynamic, auto-return) ──────────────
    BowBack,        // Bow head down and shuffle backward
    BodyCycle,      // Body centre draws a circle (orientation fixed)
    HeadEllipse,    // Head draws an ellipse via body attitude

    // ── Diagonal trot walks ──────────────────────────────────────
    MoveLeftFront,  // Forward + strafe left
    MoveRightFront, // Forward + strafe right
    MoveLeftBack,   // Backward + strafe left
    MoveRightBack,  // Backward + strafe right

    // ── Stanford Pupper trot gait (exact StanfordQuadruped port) ─
    StanfordWalk,   // Continuous forward trot, Raibert swing/stance

    // ── FPC choreography (MangDang MovementGroups.py, Stanford IK) ─
    FrontKick,      // Rear up like a horse: front paws reach fwd+up, auto-return
    Wiggle,         // Butt up (pitch -22°), tail-wag yaw sweep while held
    ButtShrug,      // Nose up (pitch +20°), butt-shrug yaw sweep while held
    WiggleLeft,     // Butt up, yaw held to one side (FPC wiggle_left)
    WiggleRight,    // Butt up, yaw held to the other side (FPC wiggle_right)
    ButtShrugLeft,  // Nose up, yaw held to one side (FPC butt_shrug_left)
    ButtShrugRight, // Nose up, yaw held to the other side (FPC butt_shrug_right)

    // Direct SDK-controlled body attitude. Kept last so existing numeric
    // GaitCmd values remain backward compatible.
    BodyAttitude,   // Hold caller-provided roll, pitch and yaw angles
};

// ── Robot configuration ──────────────────────────────────────
struct Config {
    int period   = 80;   // Gait period (ms per phase)
    int height   = 70;   // Body height (mm)
    int up_height = 10;  // Foot lift height (mm)
    int stride   = 10;   // Stride length (mm)
    int tilt     = 10;   // Body tilt angle (degrees)
    int sg_speed = 50;   // Stanford walk / diagonal speed (mm/s, max 200)
};

// ── Servo bus configuration ──────────────────────────────────
//
// The bus pins and the four chip selects live in robot/board_config.h; the
// only pin this layer still owns is the servo power rail, which does not exist
// on this board (SERVO_POWER_EN_PIN is GPIO_NUM_NC).
//
// This replaced the Feetech SCSCL serial bus on UART1 (TX=4, RX=5). Those pins
// and CONFIG_APP_SERVO_BAUD_RATE are no longer used by anything; the Kconfig
// symbols are kept only so an out-of-tree file referring to them still builds.
constexpr int SERVO_POWER_PIN = SERVO_POWER_EN_PIN;

// Current cap sent with every position setpoint, in mA. The driver boards do
// position control with a current LIMIT rather than a speed, so this is a
// ceiling and not a forced draw: a lightly loaded leg still only sources what
// it needs to hold station.
//
// 1500 is the reference firmware's gait figure, and it is the right value for
// a robot that is known to be calibrated and walking. Bring a new chassis up
// at something like 600 first: at a lower cap a mis-signed or overdriven joint
// stalls instead of tearing the leg, and the symptom is visible in
// GET /v1/studio/status as current pinned at the cap.
constexpr uint16_t SERVO_CURRENT_MAX_MA = 1500;

// ── Servo overlay ────────────────────────────────────────────
//
// An additive per-servo trim, in degrees, applied to the outgoing frame in
// flush() and never written back into the goal buffer.
//
// It exists so a skill can add to a frame the gait generator produced instead
// of having to replace it: a limp that favours one leg, a payload
// compensation, a slow drift. Because it is applied on the way out and not
// stored in the goal buffer, clearing it is enough to undo it and it cannot
// accumulate.
//
// Clamped, because the whole point is to ride on top of something else that
// owns the joints and is not expecting to be overridden.
constexpr float SERVO_OVERLAY_MAX_DEG = 20.0f;

/**
 * @brief Add a trim to one servo's outgoing frame. Clamped to
 *        +/-SERVO_OVERLAY_MAX_DEG, and then to the raw range.
 */
void set_overlay(int servo_id, float deg);

/**
 * @brief Read back one servo's overlay in degrees. 0 for a bad id.
 */
float get_overlay(int servo_id);

/**
 * @brief Clear every overlay. Called at the end of a skill run so a trim
 *        cannot outlive the skill that set it.
 */
void clear_overlay();

// ── Public API ───────────────────────────────────────────────

/**
 * @brief Initialise robot HAL.
 *
 * - Opens NVS handle
 * - Enables servo power (GPIO8 high)
 * - Initialises SCSCL bus on UART1 (TX=4, RX=5, 500 kbps)
 * - Restores persisted offsets and config from NVS
 * - Spawns the gait task on core 1
 *
 * @return true on success.
 */
bool init();

/**
 * @brief Send a gait command (thread-safe).
 *
 * Overwrites any previous command.  The gait task picks it up
 * on its next iteration.
 */
void send_gait_cmd(GaitCmd cmd);

/**
 * @brief Return the currently active gait command.
 */
GaitCmd current_gait_cmd();

/* ── The one gait name table ─────────────────────────────────────────────
 *
 * This mapping used to exist in three hand-maintained copies - the SDK's host
 * function, the HTTP handler, and the logging switch in gait_task() - which is
 * three chances for a name to mean different things depending on who asked.
 * They had already drifted: the SDK accepted names the HTTP layer rejected.
 *
 * They all come here now. If you add a gait, add it here and nowhere else.
 */

/**
 * @brief Look a wire name up.
 * @return false if there is no such gait; `out` is untouched then.
 */
bool gait_from_name(const char *name, GaitCmd &out);

/**
 * @brief The wire name for a gait, or "none" if it has none.
 */
const char *gait_to_name(GaitCmd cmd);

/**
 * @brief How many named gaits there are, and the i-th name.
 *        For listing them (the PWA's gait picker, mpx-cli's help).
 */
int         gait_name_count();
const char *gait_name_at(int index);

/**
 * @brief Web-joystick input (mini_pupper_web_controller style).
 *
 * f = forward/back, s = strafe (+left), t = turn (+left); each -1..1.
 * Any input beyond the dead-zone auto-starts the Stanford walk; the
 * velocities scale with Config::sg_speed.  Releasing the pads (zeros)
 * makes the robot step in place; leaving the walk parks it standing.
 */
void joy_input(float f, float s, float t);

/**
 * @brief Hold a Stanford-IK body attitude (degrees).
 *
 * Values are clamped to the same safe limits as the reference movement API:
 * roll +/-25, pitch +/-20 and yaw +/-30 degrees. Calling this again updates
 * the held pose; sending any gait command exits the pose normally.
 */
void set_body_attitude(float roll_deg, float pitch_deg, float yaw_deg);

/**
 * @brief Set the body-attitude slew speed in degrees/second.
 *
 * 0 (default) = instant: roll/pitch/yaw/attitude snap to the target.
 * >0 makes the held attitude glide toward the target at this speed, so
 * repeated pose updates ease smoothly instead of jumping. Negative values
 * are clamped to 0.
 */
void set_attitude_speed(float dps);

/**
 * @brief Set the body-attitude slew speed per axis, in degrees/second.
 *
 * Like set_attitude_speed() but with an independent speed for roll, pitch
 * and yaw. 0 on an axis = that axis snaps instantly; >0 = it glides at that
 * speed. Negative values are clamped to 0.
 */
void set_attitude_speed_xyz(float roll_dps, float pitch_dps, float yaw_dps);

/**
 * @brief Get the current robot configuration.
 */
Config get_config();

/**
 * @brief Overwrite the robot configuration and persist to NVS.
 */
void set_config(const Config &cfg);

// ── IMU API ─────────────────────────────────────────────────

/**
 * @brief Initialise the BMI270 IMU (with its BMM150 magnetometer) over I2C.
 *
 * The BMI270 is on the shared I2C bus at 0x68 (SDA=IO1, SCL=IO2 - see
 * robot/board_config.h), with the BMM150 hanging off the BMI270's own
 * auxiliary I2C pins, so the host reaches the magnetometer only through the
 * BMI270. This replaces the QMI8658C, which was on SPI2 on the Mini Pupper 2
 * board - a bus this board gives entirely to the servo driver boards.
 *
 * If the BMM150 does not answer, accel and gyro still come up and
 * mag_valid stays false: a dead magnetometer must not cost the robot its gait.
 *
 * Must be called once during boot, after NVS but before the IMU
 * background task is needed.  Spawns a low-priority polling task
 * on core 0 that reads 6-DOF data at ~20 Hz.
 *
 * @return true on success.
 */
bool imu_init();

/**
 * @brief Return a thread-safe copy of the latest IMU sample.
 */
ImuData imu_read();

/**
 * @brief Print the latest IMU data to the log.
 */
void imu_print();

/**
 * @brief Get the angular offset for a single servo (1‑12).
 */
float get_offset(int servo_id);

/**
 * @brief Set the angular offset for a single servo (1‑12) and
 *        persist it to NVS.
 */
void set_offset(int servo_id, float deg);

/**
 * @brief Reset all servo offsets to 0 and persist.
 */
void reset_offsets();

/**
 * @brief Read the current position of a servo, in the AT32 frame (0..1023).
 *
 * This is the frame the driver boards and Servo Studio speak, NOT the frame
 * set_servo_angle() accepts - the two run in opposite directions. For a closed
 * loop use read_angle_cdeg() instead.
 *
 * @return Position, or -1 on failure.
 */
int read_position(int servo_id);

/**
 * @brief Read the current speed of a servo (signed).
 *
 * Always -1 on this board. The driver boards have no speed field: position
 * control is current limited, not velocity commanded, so there is no reading
 * to return and inventing one would be worse than saying so.
 *
 * @return -1, always.
 */
int read_speed(int servo_id);

/**
 * @brief Read the current load on a servo (signed, unit‑dependent).
 *        Returns -1 on failure.
 */
int read_load(int servo_id);

/**
 * @brief Read the servo supply voltage (0.1 V increments, were it ever reported).
 *        Returns -1 on failure.
 *
 * @note Still returns -1 on this board, for a different reason: 
 *       The servo rail is Vbat+ straight through CN3..CN6, so there is no
 *       per-servo supply for the AT32 boards to measure.
 */
int read_voltage(int servo_id);

/**
 * @brief Read the servo internal temperature, in whole degrees C.
 *        Returns -1 on failure; read_temperature_c() gives NaN instead.
 *
 * @note Works on this board: each AT32 board samples one 10k NTC per servo
 *       at ~22 Hz and ships it in the feedback frame's reserved field as a
 *       signed 0.1 degC value. It rides on the sync_write the gait already
 *       does, so it costs no extra SPI traffic. When the gait is parked the
 *       cache goes stale - call driver_board_poll() for a fresh reading.
 */
int read_temperature(int servo_id);

/**
 * @brief Read the servo moving state (0 = stopped, 1 = moving).
 *        Returns -1 on failure.
 */
int read_moving(int servo_id);

/**
 * @brief Read the servo current draw (mA, signed).
 *        Returns -1 on failure.
 *
 * @note Works on this board: present motor current rides back in the feedback
 *       frame, refreshed on every sync_write the gait already does.
 */
int read_current(int servo_id);

/**
 * @brief Ping a servo to verify communication.
 *        Returns the model number on success, or <= 0 on failure.
 */
int ping_servo(int servo_id);

/**
 * @brief Read a servo's measured angle in the frame set_servo_angle() takes.
 *
 * Signed centidegrees relative to centre, positive in the direction a positive
 * `deg` command moves the joint - so read -> compare -> correct actually
 * converges. read_position() is in the opposite frame and cannot be used for
 * this.
 *
 * @return Centidegrees, or INT32_MIN on failure.
 */
int read_angle_cdeg(int servo_id);

/**
 * @brief Read a servo's NTC temperature in degrees C.
 *
 * @return Temperature, or NaN if that servo has never answered.
 */
float read_temperature_c(int servo_id);

// ── Servo bus ownership ──────────────────────────────────────
//
// Two owners may take the bus from the gait: the Servo Studio console (a human
// at a keyboard, via the HTTP handlers) and a WASM skill doing low-level
// control. While either holds it, flush() holds the last pose instead of
// writing, so nothing interleaves with a config request/NOP pair.

/** Park the gait so a human can drive the servos directly. */
void set_studio_mode(bool on);

/** Is the studio (rather than the gait) driving the servos? */
bool studio_mode();

/** Take the bus for a skill. False if Studio already holds it. */
bool servo_lock();

/** Give the bus back after servo_lock(). */
void servo_unlock();

/** True if ANYBODY holds the bus (Studio or a skill). NOT an authorisation. */
bool servo_locked();

/** True only if THIS skill holds the bus. This is the one to gate writes on. */
bool servo_owned_by_skill();

/** Release the bus if a skill still holds it. Call when a skill ends. */
void release_skill_bus_lock();

// ── Low-level IK (exposed for WASM host functions) ───────────

/**
 * @brief Set one servo angle in degrees RELATIVE to centre.
 *
 * Clamped to the raw range 0..1023, where one degree is
 * servo_ticks_per_deg() raw steps - derived from the driver boards' OWN
 * range_position_deg, not from a constant in this file. Centre (0 deg) is 511.
 *
 * This only fills a goal buffer; flush() is what puts it on the bus.
 *         
 */
void set_servo_angle(int servo_id, float deg);

/**
 * @brief Set movement speed for one servo (0 = max, larger = slower).
 *
 * STORED, NEVER SENT. The driver boards have no speed field: in position mode
 * the command frame's third field is a max CURRENT limit in milliamps, and the
 * boards run their own position loop underneath it. There is nothing for a
 * speed value to travel in.
 *
 * Kept because the symbol is part of the shipped SDK and removing it would
 * break every skill that calls it. Use the current cap instead
 * (SERVO_CURRENT_MAX_MA here, /v1/studio/set for one servo).
 */
void set_servo_speed(int servo_id, uint16_t speed);

/**
 * @brief Set speed for all 12 servos at once. Stored, not sent - see
 *        set_servo_speed().
 */
void set_all_servo_speed(uint16_t speed);

/**
 * @brief Set the per-servo current limit, in milliamps.
 *
 * THIS is the torque control on this hardware. The driver boards do position
 * control with a current LIMIT: the value rides in every command frame and
 * caps what the board's own loop may source. It is a ceiling, not a forced
 * draw, so a lightly loaded leg still only takes what it needs to hold station.
 *
 * Clamped to 0..SERVO_CURRENT_MAX_MA. 0 means "hold with no torque", which
 * for a loaded joint is indistinguishable from releasing it.
 *
 * Persisted, so a value set here survives a reboot.
 */
void set_servo_current(int servo_id, uint16_t milliamps);

/**
 * @brief Read the per-servo current limit in milliamps.
 *        Returns 0 for an out-of-range id.
 */
uint16_t get_servo_current(int servo_id);

/**
 * @brief Push the twelve buffered setpoints to the driver boards.
 *
 * One SPI pass: four 36-byte frames, one per board, each carrying three
 * servos' position and current cap. Paced to at most one pass per 5 ms.
 *
 * Does nothing at all while Servo Studio or a skill holds the bus.
 */
void flush();

// ── Per‑leg IK helpers ───────────────────────────────────────

/**
 * Each leg IK takes three parameters:
 *   x    – forward/backward displacement (mm), positive = forward
 *   th0  – hip rotation (degrees), positive = outward
 *   z    – foot height (mm), measured from hip
 *
 * Leg‑to‑servo mapping
 *   Front Right (FR): servo 1 (hip),  2 (shoulder), 3 (knee)
 *   Front Left  (FL): servo 4 (hip),  5 (shoulder), 6 (knee)
 *   Rear Right  (RR): servo 7 (hip),  8 (shoulder), 9 (knee)
 *   Rear Left   (RL): servo 10 (hip), 11 (shoulder),12 (knee)
 */
void front_right_ik(float x, float th0, float z);
void front_left_ik(float x, float th0, float z);
void rear_right_ik(float x, float th0, float z);
void rear_left_ik(float x, float th0, float z);

}  // namespace robot
