/*
 * SPDX-FileCopyrightText: 2026 MangDang
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * mpx_robot -- the C facade over the MangDang quadruped.
 *
 * The robot layer itself is C++ (namespace robot, in robot.h) and stays that
 * way: it is a direct port of proven gait and kinematics code and rewriting it
 * would trade a lot of risk for a little style. But everything that talks to
 * it from outside this component -- the ESP-Claw capability groups, the Lua
 * module, the /v1 HTTP handlers, the WASM host functions -- is C. This header
 * is the seam between the two, and it is the only mpx_robot header those
 * layers should include.
 *
 * Angles are DEGREES relative to the centred pose unless a function says
 * otherwise. Servo ids are 1..12:
 *
 *      leg:   FR   FL   RR   RL
 *      abd:    1    4    7   10   (hip yaw)
 *      hip:    2    5    8   11   (shoulder)
 *      knee:   3    6    9   12
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

/**
 * @brief Bring up the robot: SPI3, the four AT32F413 driver boards, the
 *        BMI270, persisted offsets and config, and the gait task on core 1.
 *
 * Safe to call once. Returns false if the bus or the boards could not be
 * brought up; the caller may continue without servos.
 */
bool mpx_robot_init(void);

/** @brief Whether mpx_robot_init() has completed successfully. */
bool mpx_robot_ready(void);

/* ── The seam to the WASM skill runtime ────────────────────────────────── */

/**
 * @brief Predicates the gait loop asks about a running WASM skill.
 *
 * mpx_wasm fills this in at init. It exists so that mpx_robot does not have to
 * depend on mpx_wasm: the dependency already runs the other way -- mpx_wasm's
 * host functions drive this layer -- and making it mutual would put the two
 * components in a requirement cycle.
 *
 * With no runtime registered, both questions answer "no", which is the correct
 * behaviour for a build with no WASM support compiled in.
 */
typedef struct {
    /** Is a skill executing right now? */
    bool (*skill_is_running)(void);
    /** Does that skill currently own the twelve goal positions? */
    bool (*skill_owns_pose)(void);
    /** Ask the running skill to stop (the Pi is taking control). May be NULL. */
    void (*request_stop)(void);
} mpx_robot_skill_hooks_t;

/**
 * @brief Register the skill predicates. Pass NULL to unregister.
 *
 * The struct must outlive the registration -- pass a pointer to a static.
 */
void mpx_robot_set_skill_hooks(const mpx_robot_skill_hooks_t *hooks);

/* ── Movement ──────────────────────────────────────────────────────────── */

/**
 * @brief Start a named built-in gait ("advance", "stanford", "lookup", ...).
 *
 * Returns ESP_ERR_NOT_FOUND if there is no such gait. This is the built-in
 * table only; skill-provided movement names are resolved one layer up, in
 * mpx_wasm's movement.c, which falls through to here.
 */
esp_err_t mpx_robot_gait_by_name(const char *name);

/** @brief The wire name of the gait running now, or "none". Never NULL. */
const char *mpx_robot_current_gait_name(void);

/**
 * @brief The move the legs are playing right now -- the gait's name, or the
 *        Character move playing inside the show -- or NULL when idle. For the
 *        face's per-move expressions. `seq` (may be NULL) changes every time
 *        a move starts.
 */
const char *mpx_robot_playing_move(uint32_t *seq);

/** @brief How many built-in gait names there are. */
int mpx_robot_gait_name_count(void);

/** @brief The i-th built-in gait name, or NULL if `index` is out of range. */
const char *mpx_robot_gait_name_at(int index);

/**
 * @brief Analog drive, the same path the web joystick uses.
 *
 * @param forward  -1..1, positive walks forward
 * @param strafe   -1..1, positive strafes left
 * @param turn     -1..1, positive turns left
 *
 * Any input past the dead zone auto-starts the Stanford trot; zeros make the
 * robot step in place; leaving it alone parks it standing.
 */
void mpx_robot_drive(float forward, float strafe, float turn);

/**
 * @brief Hold a body attitude in degrees. Clamped to roll +/-25, pitch +/-20,
 *        yaw +/-30 -- the same limits as the reference movement API.
 */
void mpx_robot_set_body_attitude(float roll_deg, float pitch_deg, float yaw_deg);

/** @brief Attitude slew rate in degrees/second. 0 snaps instantly. */
void mpx_robot_set_attitude_speed(float dps);

/**
 * @brief Analog look pad: glide the head toward yaw/pitch, in degrees.
 *
 * yaw -30 (left) .. +30 (right), pitch -20 (down) .. +20 (up); clamped. 0/0
 * glides back to the stand and ends the look. Send it repeatedly while the pad
 * is held -- this is GET /v1/robot/look, the mpv2 / minipupperesp look pad.
 */
void mpx_robot_look(float yaw_deg, float pitch_deg);

/** @brief One-line description of a built-in gait, "" if it has none. */
const char *mpx_robot_gait_note(const char *name);

/**
 * @brief Does this built-in gait keep running until told to stop (walks,
 *        holds), as opposed to ending by itself (jumps, tricks)? Unknown names
 *        answer false.
 */
bool mpx_robot_gait_is_continuous(const char *name);

/** @brief Is the Raspberry Pi (ROS) in control, so local motion is ignored? */
bool mpx_robot_pi_in_control(void);

/**
 * @brief Called with true when the Raspberry Pi takes the robot
 *        (robot::master::begin(), mpx_pi_link) and false when it lets go.
 *        Runs on the Pi link task, so the callback must not block: post the
 *        work somewhere (main.c turns voice off and puts ROS on the screen).
 *        One observer; a second call replaces the first.
 */
void mpx_robot_set_master_observer(void (*cb)(bool active));

/* ── Configuration ─────────────────────────────────────────────────────── */

typedef struct {
    int period;      /*!< gait period, ms per phase */
    int height;      /*!< body height, mm           */
    int up_height;   /*!< foot lift height, mm      */
    int stride;      /*!< stride length, mm         */
    int tilt;        /*!< body tilt, degrees        */
    int sg_speed;    /*!< Stanford walk speed, mm/s, max 200 */
} mpx_robot_config_t;

void mpx_robot_get_config(mpx_robot_config_t *out);
void mpx_robot_set_config(const mpx_robot_config_t *cfg);   /*!< persists to NVS */

/* ── Servo power ───────────────────────────────────────────────────────── */

/** Soft start at boot, and rest after `seconds` unused (default 300): the body
 *  lowers to a crouch and the servos switch off until something asks for a
 *  movement. 0 = never rest. Saved in NVS. */
void        mpx_robot_set_rest_after(uint32_t seconds);
uint32_t    mpx_robot_rest_after(void);
const char *mpx_robot_power_state(void);   /*!< "holding", "resting (servos off)", ... */
bool        mpx_robot_servos_resting(void);
void        mpx_robot_rest_now(void);
void        mpx_robot_wake(void);

/* ── Calibration ───────────────────────────────────────────────────────── */

/** @brief Per-servo angular offset in degrees. servo_id is 1..12. */
float mpx_robot_get_offset(int servo_id);
void  mpx_robot_set_offset(int servo_id, float deg);        /*!< persists to NVS */
void  mpx_robot_reset_offsets(void);

/* ── Telemetry ─────────────────────────────────────────────────────────── */

typedef struct {
    float ax, ay, az;   /*!< accelerometer, g   */
    float gx, gy, gz;   /*!< gyroscope, dps     */
    float mx, my, mz;   /*!< magnetometer, uT -- zero unless mag_valid */
    bool  mag_valid;    /*!< BMM150 found behind the BMI270 and sampled */
} mpx_robot_imu_t;

void mpx_robot_imu_read(mpx_robot_imu_t *out);

/** @brief True once the BMM150 magnetometer came up (see mpx_imu.cc). */
bool mpx_robot_mag_ready(void);

/**
 * @brief Measured servo angle in the same frame mpx_robot_set_servo_angle()
 *        accepts: signed centidegrees from centre.
 *
 * This is the reader to close a control loop with. mpx_robot_read_position()
 * speaks the AT32 frame instead, which runs in the opposite direction --
 * comparing that against a commanded angle diverges.
 *
 * @return centidegrees, or INT32_MIN for a bad servo id.
 */
int mpx_robot_read_angle_cdeg(int servo_id);

/** @brief Raw measured position, 0..1023 in the AT32 frame. -1 on failure. */
int mpx_robot_read_position(int servo_id);

/** @brief Motor current in mA, signed. -1 on failure. */
int mpx_robot_read_current(int servo_id);

/** @brief NTC temperature in degrees C. NAN if that servo never answered. */
float mpx_robot_read_temperature_c(int servo_id);

/** @brief Probe a channel. >0 if the driver board answered. */
int mpx_robot_ping_servo(int servo_id);

/* ── Servo Studio ──────────────────────────────────────────────────────────
 *
 * Studio mode parks the gait task so something else can own the servo bus.
 * It matters because a parameter exchange with an AT32 board is a REQUEST
 * FOLLOWED BY A REPLY, and the two must not be separated -- split them and the
 * reply is left pending in the board's tx buffer, where the next decode reads
 * it as garbage. A 31 degree reading comes back as 1540.
 *
 * While studio mode is on the servos simply hold their last commanded pose.
 * Read-only telemetry does NOT require it; only parameter access does.
 */

/** @brief Park or resume the gait task. */
void mpx_robot_set_studio_mode(bool on);

/** @brief Whether Servo Studio currently holds the bus. */
bool mpx_robot_studio_mode(void);

/* ── Low-level joint control ───────────────────────────────────────────── */

/** @brief Buffer one joint angle, in degrees from centre. */
void mpx_robot_set_servo_angle(int servo_id, float deg);

/** @brief Push all buffered setpoints to the driver boards. */
void mpx_robot_flush(void);

/**
 * @brief Add a clamped per-joint offset on top of whatever is driving the
 *        joints, applied at flush time. Lets a skill garnish a running gait
 *        instead of replacing it. Clamped to +/-20 degrees.
 */
void  mpx_robot_set_overlay(int servo_id, float deg);
float mpx_robot_get_overlay(int servo_id);
void  mpx_robot_clear_overlay(void);

#ifdef __cplusplus
}
#endif
