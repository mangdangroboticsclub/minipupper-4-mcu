#pragma once

#include <cstddef>
#include <cstdint>

#include "wasm_export.h"

namespace sdk {

// ═══════════════════════════════════════════════════════════════
//  ABI version and the one error convention
// ═══════════════════════════════════════════════════════════════

/* MPX_ABI_VERSION 2 — a BREAKING change from version 1.
 *
 * Seventeen host functions used to be registered with signatures that declare
 * no result ("()", "(ii)", "($)" ...) even though every one of them computed
 * an error code. WAMR discards a result the signature does not mention, so
 * those codes were physically unreachable from a skill: a misspelled gait
 * name, an out-of-range servo id, a null IMU pointer and a watchdog
 * cancellation all looked identical from inside a skill — like nothing
 * happening — while the real reason went to a serial console nobody was
 * watching.
 *
 * They now return int32_t. This is not backwards compatible and cannot be
 * made so: WAMR's check_symbol_signature() requires the signature string to
 * be fully consumed after ')' when the calling module declares no result, so
 * a v1 module trying to call one of these fails to link and traps on first
 * call. Skills built against SDK v1 must be rebuilt — one `mpx-cli deploy`.
 * Better to spend that now than after the marketplace fills with binaries
 * nobody can recompile.
 */
/* v4 adds three capabilities that previously required patching firmware:
 *
 *   on_tick   a skill can run inside the control loop instead of being a
 *             script that plays once (mpx_tick_every / mpx_tick_stop)
 *   overlay   a skill can add to a frame the gait generator produced,
 *             instead of having to replace it (mpx_overlay*)
 *   trace     a skill can emit named numbers, not just text (mpx_trace)
 *
 * Purely additive: every v3 symbol keeps its name, signature and meaning, so
 * a v3 module runs unchanged. The version still moves because a v4 module
 * will not run on v3 firmware, and finding that out as a trap on the first
 * host call is exactly what the version check exists to prevent.
 */
/* v5 adds one symbol, mpx_board_scale_dd(), and it exists because three
 * places in this firmware asserted that a servo's travel is 270 degrees. It
 * is not: the AT32 boards hold their own range_position_deg — 310 from the
 * factory, less once 'guard' has trimmed it — in flash that survives any
 * reflash of the ESP. A skill replaying recorded SCS ticks needs that number
 * to know what a tick is worth, and could not ask for it.
 *
 * Additive like v4: every v4 symbol is unchanged, so a v4 module still runs.
 * The version moves because a v5 module will NOT run on v4 firmware — the
 * import is missing and instantiation fails — and saying so through the
 * version check is better than a trap on the first call. */
/* v6 brings the ABI closer to parity with the other two ways to drive this
 * robot: the Lua VM (lua/lua_bindings.cc, lua_display.cc) and the local
 * agent's tool table (agent/robot_tools.cc). Until now a skill could move the
 * robot but not draw on its screen, read or write a file or play music, while
 * a Lua script and the agent could do all of it.
 *
 * What v6 adds, each mirroring the Lua module or agent tool named:
 *
 *   display_*          the screen: Lua's display.* canvas primitives plus the
 *                      agent's higher-level show_text / show_emotion /
 *                      set_colors / set_brightness
 *   fs_*               Lua's fs.read/write/delete/exists/list/info; the two
 *                      write paths keep the same user-permission prompt
 *   music_play/stop    the agent's music tools
 *   robot_get_config, robot_get_gait
 *                      the composite state Lua's robot.get_config()/get_mode()
 *                      and the agent's robot_get_state answer
 *
 * Deliberately NOT here: Lua's crypto.* and the agent's skill-management
 * tools (mpx_list/run/stop_skill). A sandboxed skill does not get to decode
 * blobs, nor to list, start or stop other skills — those stay the agent's
 * (mpx_run_skill) — and a skill asks for a movement with robot_gait().
 *
 * Every symbol is additive: a v5 module keeps running unchanged. The version
 * moves because a v6 module will NOT run on v5 firmware — the imports are
 * missing and instantiation fails — which is exactly what the version check
 * exists to say before the first host call traps. */
/* v7 adds the microphone: mpx_mic_start / mpx_mic_read / mpx_mic_stop.
 *
 * A skill runs with voice held back (voice_glue::hold, taken by the skill task
 * around the run), so the capture is free — no front end, no voice turn — and
 * this is the skill's to open. What it gets is MONO at the sample rate it asks
 * for (0 = 16 kHz, the voice rate), read straight from the codec with the
 * microphone gain already applied, which is the same stream ASR transcribes:
 * good enough to clap at, to listen for a loud noise, or to record a clip;
 * not an echo-cancelled or beam-formed signal (that needs the AFE, and the
 * AFE is exactly what the hold stopped).
 *
 * Additive like v6: every v6 symbol is unchanged, so a v6 module still runs.
 * A v7 module will NOT run on v6 firmware (missing imports), hence the bump. */
constexpr int32_t MPX_ABI_VERSION = 7;

/* Every host function returns one of these, or a value >= 0 where it is
 * documented to return data. Before v2 there were four incompatible
 * conventions in one 48-symbol table; this is the only one now.
 *
 * NOTE: large parts of the older code still return bare literals (-1/-2/-3/-4)
 * that happen to line up numerically with these. Keep the numbers exactly as
 * they are — a skill switches on them. */
enum : int32_t {
	MPX_OK             =  0,   /**< Success.                                */
	MPX_ERR_ARG        = -1,   /**< Bad argument: id, index or pointer.     */
	MPX_ERR_NOT_LOCKED = -2,   /**< YOU do not hold the servo bus.          */
	MPX_ERR_NO_REPLY   = -3,   /**< The driver board did not answer.        */
	MPX_ERR_READONLY   = -4,   /**< Calibration parameter; read-only.       */
	MPX_ERR_CANCELLED  = -5,   /**< The skill was stopped mid-call.         */
	MPX_ERR_STATE      = -6,   /**< Right call, wrong time.                 */
	MPX_ERR_BUSY       = -7,   /**< Another control domain holds the joints. */
};

// ═══════════════════════════════════════════════════════════════
//  ABI v3 additions
//
//  v3 is additive over v2 in behaviour: every v2 symbol keeps its name,
//  signature and semantics. What v3 adds is (a) the capabilities the board
//  always had but a skill could not reach, and (b) the arbitration that makes
//  the control layers composable instead of merely coexistent.
//
//  Control arbitration is OPT-IN. Until a skill calls mpx_control_take(),
//  s_control_owner is MPX_CTRL_NONE and every write path behaves exactly as it
//  did in v2. A skill that takes a domain gets MPX_ERR_BUSY instead of a silent
//  fight, which is the whole point.
// ═══════════════════════════════════════════════════════════════

enum : int32_t {
	MPX_CTRL_NONE   = 0,   /**< Nobody has claimed the joints (v2 behaviour). */
	MPX_CTRL_GAIT   = 1,   /**< The gait generator drives; you steer it.      */
	MPX_CTRL_FEET   = 2,   /**< You place feet; firmware solves the legs.     */
	MPX_CTRL_JOINTS = 3,   /**< You write joint angles directly.              */
	MPX_CTRL_BUS    = 4,   /**< You own the servo bus (implies servo_lock).   */
};

/** Legs, for mpx_foot(). Matches the robot_ik_* ordering. */
enum : int32_t {
	MPX_LEG_FR = 0, MPX_LEG_FL = 1, MPX_LEG_RR = 2, MPX_LEG_RL = 3,
};

/**
 * @brief May a write to @p domain proceed?
 *
 * True when nobody has claimed anything, or when the claimant IS this domain.
 * This is the whole of the arbitration: it is checked on the write paths and
 * nowhere else.
 */
bool control_allows(int32_t domain);

/**
 * @brief Does a skill currently own the twelve goal positions?
 *
 * Asked by the gait task's idle branch before it rebuilds the neutral stand.
 * Until this existed a claim was checked only inside the host functions, so it
 * could refuse the skill's own calls and nothing else - while the buffer the
 * skill had claimed was rewritten underneath it every tick.
 *
 * Deliberately narrow: FEET and JOINTS only. Claiming GAIT or BUS is not a
 * claim on the goal buffer.
 */
bool control_owner_is_pose();

/** Drop any claim. Called at the start and end of every skill run. */
void control_reset();

/** Forget which gains a skill touched. Called at the start of every run. */
void forget_skill_gains();

/**
 * @brief Put back every gain the skill changed, to what it was beforehand.
 *
 * Must be called while the servo bus is still held: a config write is a
 * request/reply pair and is refused without the lock.
 *
 * @return How many slots were restored.
 */
int restore_skill_gains();

// ═══════════════════════════════════════════════════════════════
//  print — SDK host function
// ═══════════════════════════════════════════════════════════════

/**
 * @brief Wasm-side: extern void print(const char *text, int len);
 *
 * Signature "($i)": auto-converted string pointer + length.
 */
int32_t host_print(wasm_exec_env_t exec_env,
				   int32_t text_offset, int32_t len);

// ═══════════════════════════════════════════════════════════════
//  Robot host functions
// ═══════════════════════════════════════════════════════════════

/**
 * @brief Wasm-side: extern void robot_gait(const char *name, int len);
 *
 * Signature "($)": auto-converted string pointer (name_len deduced by WAMR).
 * Sends a gait command by name (e.g. "advance", "jump", "none").
 */
int32_t host_robot_gait(wasm_exec_env_t exec_env,
						int32_t name_ptr);

/**
 * @brief Wasm-side: extern int robot_get_mode(void);
 * Signature "()i". Returns current gait mode as int (see GaitCmd enum).
 */
int32_t host_robot_get_mode(wasm_exec_env_t exec_env);

/**
 * @brief Wasm-side: extern void robot_set_body_pose(float roll_deg,
 *                       float pitch_deg, float yaw_deg);
 * Signature "(fff)". Holds a Stanford-IK body attitude in degrees.
 */
int32_t host_robot_set_body_pose(wasm_exec_env_t exec_env,
                                 float roll_deg, float pitch_deg,
                                 float yaw_deg);

/**
 * @brief Wasm-side: extern void robot_set_attitude_speed(int dps);
 * Signature "(i)". Sets the roll/pitch/yaw slew speed in degrees/second
 * (0 = instant snap; >0 eases toward the target at that speed).
 */
int32_t host_robot_set_attitude_speed(wasm_exec_env_t exec_env,
                                      int32_t dps);

/**
 * @brief Wasm-side: extern void robot_set_attitude_speed_xyz(int roll_dps,
 *                       int pitch_dps, int yaw_dps);
 * Signature "(iii)". Per-axis roll/pitch/yaw slew speed in degrees/second
 * (0 on an axis = instant snap; >0 = eases at that speed).
 */
int32_t host_robot_set_attitude_speed_xyz(wasm_exec_env_t exec_env,
                                          int32_t roll_dps, int32_t pitch_dps,
                                          int32_t yaw_dps);

/**
 * @brief Wasm-side: extern void robot_set_config(int period, int height,
 *                       int up_height, int stride, int tilt);
 * Signature "(iiiii)".
 */
int32_t host_robot_set_config(wasm_exec_env_t exec_env,
							  int32_t period, int32_t height,
							  int32_t up_height, int32_t stride,
							  int32_t tilt);

// ── Individual config getters ────────────────────────────────
int32_t host_robot_get_period(wasm_exec_env_t exec_env);
int32_t host_robot_get_height(wasm_exec_env_t exec_env);
int32_t host_robot_get_up_height(wasm_exec_env_t exec_env);
int32_t host_robot_get_stride(wasm_exec_env_t exec_env);
int32_t host_robot_get_tilt(wasm_exec_env_t exec_env);

// ── Low-level servo control ──────────────────────────────────

/**
 * @brief Wasm-side: extern void robot_set_servo_angle(int id, int centideg);
 * Signature "(ii)". Angle in centidegrees (e.g. 4500 = 45.00°).
 */
int32_t host_robot_set_servo_angle(wasm_exec_env_t exec_env,
								   int32_t id, int32_t centideg);

/**
 * @brief Wasm-side: extern void robot_flush(void);
 * Signature "()". Sends all buffered servo commands.
 */
int32_t host_robot_flush(wasm_exec_env_t exec_env);

/**
 * @brief Wasm-side: extern void robot_set_servo_speed(int id, int speed);
 * Signature "(ii)". 0 = max speed, higher = slower.
 *
 * STORED, NEVER SENT, from the MP4-ESP32-CORE board onwards. The servo driver
 * boards have no speed field: the command frame's third slot is a max current
 * limit in milliamps and the boards run their own position loop underneath it.
 * There is nowhere for a speed to travel.
 *
 * The symbol is kept so existing skills keep loading and keep their behaviour
 * (which was never a real speed either). New code wants
 * robot_set_servo_current().
 */
int32_t host_robot_set_servo_speed(wasm_exec_env_t exec_env,
								   int32_t id, int32_t speed);

/**
 * @brief Wasm-side: extern void robot_set_servo_current(int id, int mA);
 * Signature "(ii)".
 *
 * The torque control on this hardware: a ceiling on the current the servo's
 * driver board may source while holding its position. Clamped to 0..1500.
 * Persisted across reboots.
 */
int32_t host_robot_set_servo_current(wasm_exec_env_t exec_env,
								     int32_t id, int32_t milliamps);

/**
 * @brief Wasm-side: extern int robot_get_servo_current(int id);
 * Signature "(i)i". Returns the current limit in mA, or 0 on a bad id.
 */
int32_t host_robot_get_servo_current(wasm_exec_env_t exec_env,
								     int32_t id);

/**
 * @brief Wasm-side: extern int robot_read_position(int id);
 * Signature "(i)i". Returns raw position (0-1023) or -1 on error.
 *
 * This is the AT32 frame. It runs OPPOSITE to the frame robot_set_servo_angle()
 * takes, so a read -> compare -> correct loop built on it diverges instead of
 * converging - silently, and at speed. Use robot_read_angle_cdeg() for that.
 */
int32_t host_robot_read_position(wasm_exec_env_t exec_env,
								 int32_t id);

/**
 * @brief Wasm-side: extern int robot_read_angle_cdeg(int id);
 * Signature "(i)i".
 *
 * Measured angle in SIGNED CENTIDEGREES relative to centre, in the SAME frame
 * robot_set_servo_angle() takes - so read, compare, correct actually
 * converges. Returns INT32_MIN on a bad id.
 */
int32_t host_robot_read_angle_cdeg(wasm_exec_env_t exec_env,
								   int32_t id);

/**
 * @brief Wasm-side: extern int robot_read_speed(int id);
 * Signature "(i)i". Returns signed speed or -1 on error.
 */
int32_t host_robot_read_speed(wasm_exec_env_t exec_env,
							  int32_t id);

/**
 * @brief Wasm-side: extern int robot_read_load(int id);
 * Signature "(i)i". Returns signed load or -1 on error.
 */
int32_t host_robot_read_load(wasm_exec_env_t exec_env,
							 int32_t id);

/**
 * @brief Wasm-side: extern int robot_read_voltage(int id);
 * Signature "(i)i". Returns voltage (0.1V) or -1 on error.
 */
int32_t host_robot_read_voltage(wasm_exec_env_t exec_env,
								int32_t id);

/**
 * @brief Wasm-side: extern int robot_read_temperature(int id);
 * Signature "(i)i". Returns temperature (°C) or -1 on error.
 */
int32_t host_robot_read_temperature(wasm_exec_env_t exec_env,
									int32_t id);

/**
 * @brief Wasm-side: extern int robot_read_moving(int id);
 * Signature "(i)i". Returns 0=stopped, 1=moving or -1 on error.
 */
int32_t host_robot_read_moving(wasm_exec_env_t exec_env,
							   int32_t id);

/**
 * @brief Wasm-side: extern int robot_read_current(int id);
 * Signature "(i)i". Returns signed current (mA) or -1 on error.
 */
int32_t host_robot_read_current(wasm_exec_env_t exec_env,
								int32_t id);

// ── Calibration ──────────────────────────────────────────────

/**
 * @brief Wasm-side: extern void robot_set_offset(int id, int centideg);
 * Signature "(ii)". Offset in centidegrees (e.g. 150 = 1.50°).
 */
int32_t host_robot_set_offset(wasm_exec_env_t exec_env,
							  int32_t id, int32_t centideg);

/**
 * @brief Wasm-side: extern int robot_get_offset(int id);
 * Signature "(i)i". Returns offset in centidegrees.
 */
int32_t host_robot_get_offset(wasm_exec_env_t exec_env,
							  int32_t id);

/**
 * @brief Wasm-side: extern int robot_ping_servo(int id);
 * Signature "(i)i". Returns model number or <= 0 on failure.
 */
int32_t host_robot_ping_servo(wasm_exec_env_t exec_env,
							  int32_t id);

/**
 * @brief Wasm-side: extern void robot_delay_ms(int ms);
 * Signature "(i)". Blocks the WASM calling thread for ms milliseconds.
 *
 * This is the ONLY reliable way to pause between gait commands from
 * a WASM skill -- pure-WASM busy-loops run at near-zero wall time
 * inside the interpreter.
 */
int32_t host_robot_delay_ms(wasm_exec_env_t exec_env,
							int32_t ms);

// ═══════════════════════════════════════════════════════════════
//  Inverse Kinematics (per-leg)
// ═══════════════════════════════════════════════════════════════

/**
 * @brief Wasm-side: extern void robot_ik_fr(float x, float th0, float z);
 * Signature "(fff)".
 * Front-right leg IK: x = fwd/back (mm), th0 = hip rotation (deg), z = height (mm).
 */
int32_t host_robot_ik_fr(wasm_exec_env_t exec_env,
						 float x, float th0, float z);

/**
 * @brief Wasm-side: extern void robot_ik_fl(float x, float th0, float z);
 * Signature "(fff)".
 * Front-left leg IK.
 */
int32_t host_robot_ik_fl(wasm_exec_env_t exec_env,
						 float x, float th0, float z);

/**
 * @brief Wasm-side: extern void robot_ik_rr(float x, float th0, float z);
 * Signature "(fff)".
 * Rear-right leg IK.
 */
int32_t host_robot_ik_rr(wasm_exec_env_t exec_env,
						 float x, float th0, float z);

/**
 * @brief Wasm-side: extern void robot_ik_rl(float x, float th0, float z);
 * Signature "(fff)".
 * Rear-left leg IK.
 */
int32_t host_robot_ik_rl(wasm_exec_env_t exec_env,
						 float x, float th0, float z);

// ═══════════════════════════════════════════════════════════════
//  IMU
// ═══════════════════════════════════════════════════════════════

/**
 * @brief Wasm-side: extern void robot_imu_read(int buffer_ptr);
 * Signature "(i)".
 *
 * Reads the latest IMU 6-DOF sample into a WASM buffer. The buffer must
 * be at least 6 × 4 = 24 bytes. The layout written is:
 *   float[0] = ax (accel X, g)
 *   float[1] = ay (accel Y, g)
 *   float[2] = az (accel Z, g)
 *   float[3] = gx (gyro X, dps)
 *   float[4] = gy (gyro Y, dps)
 *   float[5] = gz (gyro Z, dps)
 */
int32_t host_robot_imu_read(wasm_exec_env_t exec_env,
							int32_t buffer_ptr);

/**
 * @brief Wasm-side: extern void robot_imu_print(void);
 * Signature "()". Prints the latest IMU data to the ESP log.
 */
int32_t host_robot_imu_print(wasm_exec_env_t exec_env);

// ═══════════════════════════════════════════════════════════════
//  NativeSymbol table
// ═══════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════
//  ABI v2-v5 host functions
//
//  Signatures are the contract: a skill that declares one of these and finds
//  a different shape here fails to link, which is the intended, loud failure.
//  WAMR's signature grammar: 'i' int32, 'f' float, '$' a length-prefixed
//  pointer pair, and a trailing type for the result.
// ═══════════════════════════════════════════════════════════════

/** Wasm-side: extern int mpx_abi_version(void); Signature "()i". */
int32_t host_mpx_abi_version(wasm_exec_env_t exec_env);

// ── Direct servo bus (requires servo_lock) ───────────────────

/** extern int servo_lock(void); Signature "()i". Parks the gait. */
int32_t host_servo_lock(wasm_exec_env_t exec_env);

/** extern int servo_unlock(void); Signature "()i". */
int32_t host_servo_unlock(wasm_exec_env_t exec_env);

/** extern int servo_is_locked(void); Signature "()i".
 *  Answers "do *I* hold the bus", not "is it held by anyone". */
int32_t host_servo_is_locked(wasm_exec_env_t exec_env);

/** extern int servo_set_gain(int id, int param, float value); "(iif)i".
 *  Calibration parameters are refused with -4. */
int32_t host_servo_set_gain(wasm_exec_env_t exec_env,
							int32_t id, int32_t param, float value);

/** extern int servo_get_gain(int id, int param, float *out); "(iii)i". */
int32_t host_servo_get_gain(wasm_exec_env_t exec_env,
							int32_t id, int32_t param, int32_t out_ptr);

/** extern int servo_save_config(int id); "(i)i". id 0 = all boards. */
int32_t host_servo_save_config(wasm_exec_env_t exec_env, int32_t id);

/** extern int servo_restore_config(int id); "(i)i". RAM only. */
int32_t host_servo_restore_config(wasm_exec_env_t exec_env, int32_t id);

/** extern int servo_stage(int id, float q_deg, float tau_ma,
 *                         float kp, float kd); "(iffff)i".
 *  Updates one servo's shadow without touching the bus. */
int32_t host_servo_stage(wasm_exec_env_t exec_env,
						 int32_t id, float q_deg, float tau_ma,
						 float kp, float kd);

/** extern int servo_commit(void); "()i". Pushes all four boards. */
int32_t host_servo_commit(wasm_exec_env_t exec_env);

/** extern int servo_write_all(const float *cmd, int count); "(ii)i".
 *  count x {q_deg, tau_ma, kp, kd}, staged then committed in one call. */
int32_t host_servo_write_all(wasm_exec_env_t exec_env,
							 int32_t cmd_ptr, int32_t count);

/** extern int servo_read(int id, float *out); "(ii)i".
 *  Four floats: {q_deg, tau_ma, temp_c, q_raw}. */
int32_t host_servo_read(wasm_exec_env_t exec_env,
						int32_t id, int32_t out_ptr);

/** extern int servo_read_all(float *out); "(i)i". 12 x 4 floats, same shape. */
int32_t host_servo_read_all(wasm_exec_env_t exec_env, int32_t out_ptr);

/** extern int servo_poll(void); "()i". Refreshes the feedback cache. */
int32_t host_servo_poll(wasm_exec_env_t exec_env);

/** extern int servo_direct(int id, int mode, float q_deg, float tau_ma);
 *  "(iiff)i". mode 0 idle, 1 position, 2 torque. */
int32_t host_servo_direct(wasm_exec_env_t exec_env,
						  int32_t id, int32_t mode, float q_deg, float tau_ma);

/** extern int servo_scan(void); "()i". Bitmask, bit 0 = servo 1. */
int32_t host_servo_scan(wasm_exec_env_t exec_env);

// ── Control arbitration (v3) ─────────────────────────────────

/** extern int mpx_control_take(int domain); "(i)i". MPX_CTRL_*. */
int32_t host_mpx_control_take(wasm_exec_env_t exec_env, int32_t domain);

/** extern int mpx_control_release(void); "()i". */
int32_t host_mpx_control_release(wasm_exec_env_t exec_env);

/** extern int mpx_control_owner(void); "()i". */
int32_t host_mpx_control_owner(wasm_exec_env_t exec_env);

// ── Clock (v3) ───────────────────────────────────────────────

/** extern int mpx_millis(void); "()i". Per-run, not since boot. */
int32_t host_mpx_millis(wasm_exec_env_t exec_env);

/** extern int mpx_sleep_until(int t_ms); "(i)i".
 *  Absolute deadline, so per-frame overhead does not accumulate into drift. */
int32_t host_mpx_sleep_until(wasm_exec_env_t exec_env, int32_t t_ms);

// ── Continuous drive (v3) ────────────────────────────────────

/** extern int mpx_drive(float fwd, float strafe, float turn); "(fff)i". */
int32_t host_mpx_drive(wasm_exec_env_t exec_env,
					   float fwd, float strafe, float turn);

/** extern int mpx_drive_stop(void); "()i". */
int32_t host_mpx_drive_stop(wasm_exec_env_t exec_env);

/** extern int mpx_set_walk_speed(int mm_s); "(i)i". Clamped 10..200. */
int32_t host_mpx_set_walk_speed(wasm_exec_env_t exec_env, int32_t mm_s);

/** extern int mpx_get_walk_speed(void); "()i". */
int32_t host_mpx_get_walk_speed(wasm_exec_env_t exec_env);

/** extern int mpx_foot(int leg, float x, float th0, float z); "(ifff)i".
 *  z is DOWN-positive from the hip (standing is about +70). */
int32_t host_mpx_foot(wasm_exec_env_t exec_env,
					  int32_t leg, float x, float th0, float z);

/** extern int mpx_set_all_servo_speed(int speed); "(i)i". Stored, not sent. */
int32_t host_mpx_set_all_servo_speed(wasm_exec_env_t exec_env, int32_t speed);

/** extern int mpx_reset_offsets(void); "()i". Zeroes all 12 in NVS. */
int32_t host_mpx_reset_offsets(wasm_exec_env_t exec_env);

/** extern float mpx_read_temperature_c(int id); "(i)f".
 *  Fractional, unlike robot_read_temperature() which truncates to a degree. */
float host_mpx_read_temperature_c(wasm_exec_env_t exec_env, int32_t id);

/** extern float mpx_board_scale_dd(void); "()f".
 *  The boards' travel x10 in deci-degrees, or 0 if no board answered. */
float host_mpx_board_scale_dd(wasm_exec_env_t exec_env);

/** extern float mpx_param_f(const char *name, float fallback); "($f)f". */
float host_mpx_param_f(wasm_exec_env_t exec_env,
					   int32_t name_ptr, float fallback);

/** extern int mpx_param_i(const char *name, int fallback); "($i)i". */
int32_t host_mpx_param_i(wasm_exec_env_t exec_env,
						 int32_t name_ptr, int32_t fallback);

// ── v4: overlay, tick, trace ─────────────────────────────────

/** extern int mpx_overlay(int id, float deg); "(if)i". Additive, clamped. */
int32_t host_mpx_overlay(wasm_exec_env_t exec_env, int32_t id, float deg);

/** extern float mpx_overlay_get(int id); "(i)f". */
float host_mpx_overlay_get(wasm_exec_env_t exec_env, int32_t id);

/** extern int mpx_overlay_clear(void); "()i". */
int32_t host_mpx_overlay_clear(wasm_exec_env_t exec_env);

/** extern int mpx_tick_every(int period_ms); "(i)i". Clamped 10..1000. */
int32_t host_mpx_tick_every(wasm_exec_env_t exec_env, int32_t period_ms);

/** extern int mpx_tick_stop(void); "()i". */
int32_t host_mpx_tick_stop(wasm_exec_env_t exec_env);

/** extern int mpx_trace(const char *name, float value); "($f)i". */
int32_t host_mpx_trace(wasm_exec_env_t exec_env,
					   int32_t name_ptr, float value);

// ═══════════════════════════════════════════════════════════════
//  ABI v6 — display, files, music, robot state
//
//  Every group here mirrors a surface that already existed for Lua
//  (lua/lua_bindings.cc, lua/lua_display.cc) or for the local agent's tool
//  table (agent/robot_tools.cc); each comment names its counterpart, so the
//  two stay comparable. Nothing here re-implements a code path — the bodies
//  call the same cap_display / fs / cap_music / robot entry points those
//  callers do.
// ═══════════════════════════════════════════════════════════════

// ── display: the drawing canvas, Lua's display.* ─────────────
//
// Paint, then display_show(); the face returns when the hold runs out. This
// is the same canvas the agent's display_draw tool and the Lua module use, so
// a skill can leave something on the screen after it exits. Colours are
// 0xRRGGBB (display_rgb() packs one) and coordinates are pixels from the top
// left, clipped at the edges. A build with no panel answers every call with
// MPX_ERR_STATE, exactly as Lua raises "this robot has no screen".

/** extern int display_width(void); "()i". Canvas width, 0 with no screen. */
int32_t host_display_width(wasm_exec_env_t exec_env);

/** extern int display_height(void); "()i". Canvas height, 0 with no screen. */
int32_t host_display_height(wasm_exec_env_t exec_env);

/** extern int display_rgb(int r, int g, int b); "(iii)i". Packs 0xRRGGBB. */
int32_t host_display_rgb(wasm_exec_env_t exec_env,
						 int32_t r, int32_t g, int32_t b);

/** extern int display_clear(int color); "(i)i". Fills the canvas. */
int32_t host_display_clear(wasm_exec_env_t exec_env, int32_t color);

/** extern int display_pixel(int x, int y, int color); "(iii)i". */
int32_t host_display_pixel(wasm_exec_env_t exec_env,
						   int32_t x, int32_t y, int32_t color);

/** extern int display_rect(int x, int y, int w, int h, int color,
 *                          int radius, int border); "(iiiiiii)i".
 *  radius -1 = pill; border 1 draws the outline only. */
int32_t host_display_rect(wasm_exec_env_t exec_env, int32_t x, int32_t y,
						  int32_t w, int32_t h, int32_t color,
						  int32_t radius, int32_t border);

/** extern int display_circle(int cx, int cy, int r, int color, int border);
 *  "(iiiii)i". */
int32_t host_display_circle(wasm_exec_env_t exec_env, int32_t cx, int32_t cy,
							int32_t r, int32_t color, int32_t border);

/** extern int display_line(int x1, int y1, int x2, int y2, int color,
 *                          int width); "(iiiiii)i". */
int32_t host_display_line(wasm_exec_env_t exec_env, int32_t x1, int32_t y1,
						  int32_t x2, int32_t y2, int32_t color, int32_t width);

/** extern int display_arc(int cx, int cy, int r, int start_deg, int end_deg,
 *                         int color, int width); "(iiiiiii)i".
 *  0 degrees is 3 o'clock, clockwise. */
int32_t host_display_arc(wasm_exec_env_t exec_env, int32_t cx, int32_t cy,
						 int32_t r, int32_t start_deg, int32_t end_deg,
						 int32_t color, int32_t width);

/** extern int display_text(int x, int y, const char *text, int color,
 *                         int size, int align, int max_w); "(ii$iiii)i".
 *  size is 14, 28 or 48 (0 = the default 14); align is -1 left, 0 centre,
 *  1 right; max_w 0 = until the text ends. */
int32_t host_display_text(wasm_exec_env_t exec_env, int32_t x, int32_t y,
						  int32_t text_ptr, int32_t color, int32_t size,
						  int32_t align, int32_t max_w);

/** extern int display_blit(int x, int y, int w, int h, const void *data,
 *                         int len); "(iiiiii)i".
 *  data is w*h*2 bytes of RGB565, little-endian — as Lua's display.blit. */
int32_t host_display_blit(wasm_exec_env_t exec_env, int32_t x, int32_t y,
						  int32_t w, int32_t h, int32_t data_ptr, int32_t len);

/** extern int display_show(int hold_ms); "(i)i". Puts the canvas on the
 *  panel, as display.draw does; 0 = until something else takes the screen. */
int32_t host_display_show(wasm_exec_env_t exec_env, int32_t hold_ms);

/** extern int display_face(void); "()i". Back to the eyes now. */
int32_t host_display_face(wasm_exec_env_t exec_env);

/** extern int display_show_text(const char *text); "($)i".
 *  The agent's display_show_text: one line of text, centred. */
int32_t host_display_show_text(wasm_exec_env_t exec_env, int32_t text_ptr);

/** extern int display_show_emotion(const char *name); "($)i".
 *  The agent's display_show_emotion. MPX_ERR_ARG when there is no such face,
 *  which is the agent tool's "no such emotion". */
int32_t host_display_show_emotion(wasm_exec_env_t exec_env, int32_t name_ptr);

/** extern int display_set_colors(int background_rgb, int eyes_rgb); "(ii)i".
 *  The agent's display_set_colors; -1 leaves that colour alone. */
int32_t host_display_set_colors(wasm_exec_env_t exec_env,
								int32_t background_rgb, int32_t eyes_rgb);

/** extern int display_set_brightness(int percent); "(i)i". 0..100, clamped.
 *  The agent's display_set_brightness. */
int32_t host_display_set_brightness(wasm_exec_env_t exec_env, int32_t percent);

// ── fs: LittleFS, Lua's fs.* ─────────────────────────────────
//
// Paths are absolute inside the LittleFS mount ("/my.lua"): the same strings
// Lua's fs module takes, with the "/fs" VFS prefix added for you.

/** extern int fs_read(const char *path, void *buf, int cap); "($ii)i".
 *  Copies the file into buf. Returns the WHOLE file size; the bytes written
 *  are min(size, cap), so a return greater than cap means it was truncated
 *  and the caller should retry with a bigger buffer. Empty file = 0.
 *  MPX_ERR_ARG when there is no such file. */
int32_t host_fs_read(wasm_exec_env_t exec_env, int32_t path_ptr,
					 int32_t buf_ptr, int32_t cap);

/** extern int fs_write(const char *path, const void *data, int len);
 *  "($ii)i". Asks the user on the phone app first, exactly as Lua's fs.write
 *  does, and blocks until they answer (up to 60 s — a wait that does not
 *  count against the skill's watchdog). MPX_OK, or MPX_ERR_ARG if they
 *  declined or the write failed. */
int32_t host_fs_write(wasm_exec_env_t exec_env, int32_t path_ptr,
					  int32_t data_ptr, int32_t len);

/** extern int fs_delete(const char *path); "($)i". Asks the user first,
 *  exactly as Lua's fs.delete does. */
int32_t host_fs_delete(wasm_exec_env_t exec_env, int32_t path_ptr);

/** extern int fs_exists(const char *path); "($)i". 1 or 0. */
int32_t host_fs_exists(wasm_exec_env_t exec_env, int32_t path_ptr);

/** extern int fs_list(const char *dir, void *buf, int cap); "($ii)i".
 *  One entry per line, "name<TAB>size<TAB>d|f\n" (d = directory, f = file),
 *  which is Lua's fs.list {name, size, is_dir} flattened for a C caller.
 *  Returns the byte count, or MPX_ERR_ARG if it does not fit in cap bytes. */
int32_t host_fs_list(wasm_exec_env_t exec_env, int32_t dir_ptr,
					 int32_t buf_ptr, int32_t cap);

/** extern int fs_info(int *out); "(i)i". Writes out[0] = total bytes and
 *  out[1] = used bytes on the partition; the buffer must be an int[2].
 *  Lua's fs.info() the same two numbers. */
int32_t host_fs_info(wasm_exec_env_t exec_env, int32_t out_ptr);

// ── music: the agent's music tools ───────────────────────────
//
// Registered in every build so the ABI does not change shape; in a build
// without the voice stack they answer MPX_ERR_STATE, the same "this robot has
// no speaker" the agent tool reports.

/** extern int music_play(const char *what, char *title, int cap); "($ii)i".
 *  A genre or mood ("chill", "jazz") picks an internet radio station; an
 *  http(s) URL plays directly. A NUL/0 title_ptr skips the title. Returns
 *  MPX_OK or MPX_ERR_STATE. */
int32_t host_music_play(wasm_exec_env_t exec_env, int32_t what_ptr,
						int32_t title_ptr, int32_t cap);

/** extern int music_stop(void); "()i". Stops the speaker. */
int32_t host_music_stop(wasm_exec_env_t exec_env);

// ── robot state: the composite getters ───────────────────────

/** extern int robot_get_config(int *out); "(i)i". Writes six ints — period,
 *  height, up_height, stride, tilt, sg_speed — the fields Lua's
 *  robot.get_config() returns as a table and robot_get_state reports. The
 *  buffer must be an int[6]. */
int32_t host_robot_get_config(wasm_exec_env_t exec_env, int32_t out_ptr);

/** extern int robot_get_gait(char *buf, int cap); "(ii)i".
 *  The current movement name ("none" when idle): the string Lua's
 *  robot.get_mode() returns. MPX_ERR_ARG if it does not fit in cap bytes. */
int32_t host_robot_get_gait(wasm_exec_env_t exec_env, int32_t buf_ptr,
							int32_t cap);

// ── the microphone (ABI v7) ──────────────────────────────────

/** extern int mpx_mic_start(int sample_rate); "(i)i".
 *
 *  Opens the microphone for this skill, mono, at @p sample_rate (0 = 16 kHz).
 *  Safe to call twice (the second call is a no-op). Returns MPX_OK;
 *  MPX_ERR_STATE when the board has no microphone; MPX_ERR_BUSY when the
 *  capture is already open (a voice turn or the wake front end got there
 *  first -- a skill run holds voice back, so this normally means the skill
 *  itself opened it), or when the DMA rings cannot be widened (no internal
 *  RAM); MPX_ERR_ARG when the codec refuses the rate. */
int32_t host_mpx_mic_start(wasm_exec_env_t exec_env, int32_t sample_rate);

/** extern int mpx_mic_read(short *pcm, int frames); "(ii)i".
 *
 *  Reads up to @p frames mono int16 samples into @p pcm, blocking until the
 *  codec has them (a little under the audio duration at 16 kHz). @p frames is
 *  clamped to MPX_AUDIO_MAX_FRAMES (1920, 120 ms at 16 kHz). Returns how many
 *  frames were written, or a negative MPX_ERR_*: MPX_ERR_STATE if the
 *  microphone is not open, MPX_ERR_ARG for a null/out-of-range buffer,
 *  MPX_ERR_CANCELLED once the run's watchdog or mpx_stop_skill() has fired
 *  (check, and return, so the skill unwinds promptly). */
int32_t host_mpx_mic_read(wasm_exec_env_t exec_env, int32_t pcm_ptr,
					  int32_t frames);

/** extern int mpx_mic_stop(void); "()i". Releases the microphone. Idempotent;
 *  the sandbox also closes it at the end of the run if the skill forgot. */
int32_t host_mpx_mic_stop(wasm_exec_env_t exec_env);

// NOTE: intentionally NOT const. WAMR's wasm_runtime_register_natives()
// sorts this table in place with qsort() so it can binary-search symbol
// names later. If the array is const it is placed in flash (.rodata) and
// the in-place sort triggers a "Dbus write to cache rejected" cache-error
// panic on the ESP32-S3. Keeping it non-const puts it in writable RAM.
static NativeSymbol NATIVE_SYMBOLS[] = {
	// ABI
	{ "mpx_abi_version",        (void *)host_mpx_abi_version,        "()i",  nullptr },

	// SDK
	{ "print", (void *)host_print, "($i)i", nullptr },

	// Robot — high-level gait
	{ "robot_gait",         (void *)host_robot_gait,         "($)i",   nullptr },
	{ "robot_get_mode",     (void *)host_robot_get_mode,     "()i",   nullptr },
	{ "robot_set_body_pose",(void *)host_robot_set_body_pose,"(fff)i", nullptr },
	{ "robot_set_attitude_speed",(void *)host_robot_set_attitude_speed,"(i)i", nullptr },
	{ "robot_set_attitude_speed_xyz",(void *)host_robot_set_attitude_speed_xyz,"(iii)i", nullptr },

	// Robot — configuration
	{ "robot_set_config",   (void *)host_robot_set_config,   "(iiiii)i", nullptr },
	{ "robot_get_period",   (void *)host_robot_get_period,   "()i",   nullptr },
	{ "robot_get_height",   (void *)host_robot_get_height,   "()i",   nullptr },
	{ "robot_get_up_height",(void *)host_robot_get_up_height,"()i",   nullptr },
	{ "robot_get_stride",   (void *)host_robot_get_stride,   "()i",   nullptr },
	{ "robot_get_tilt",     (void *)host_robot_get_tilt,     "()i",   nullptr },

	// Robot — low-level servo
	{ "robot_set_servo_angle",    (void *)host_robot_set_servo_angle,    "(ii)i", nullptr },
	{ "robot_flush",              (void *)host_robot_flush,              "()i",   nullptr },
	{ "robot_set_servo_speed",    (void *)host_robot_set_servo_speed,    "(ii)i", nullptr },
	{ "robot_set_servo_current",  (void *)host_robot_set_servo_current,  "(ii)i", nullptr },
	{ "robot_get_servo_current",  (void *)host_robot_get_servo_current,  "(i)i", nullptr },
	{ "robot_read_position",      (void *)host_robot_read_position,      "(i)i", nullptr },
	{ "robot_read_angle_cdeg",    (void *)host_robot_read_angle_cdeg,    "(i)i", nullptr },
	{ "robot_read_speed",         (void *)host_robot_read_speed,         "(i)i", nullptr },
	{ "robot_read_load",          (void *)host_robot_read_load,          "(i)i", nullptr },
	{ "robot_read_voltage",       (void *)host_robot_read_voltage,       "(i)i", nullptr },
	{ "robot_read_temperature",   (void *)host_robot_read_temperature,   "(i)i", nullptr },
	{ "robot_read_moving",        (void *)host_robot_read_moving,        "(i)i", nullptr },
	{ "robot_read_current",       (void *)host_robot_read_current,       "(i)i", nullptr },

	// Robot — calibration
	{ "robot_set_offset",   (void *)host_robot_set_offset,   "(ii)i", nullptr },
	{ "robot_get_offset",   (void *)host_robot_get_offset,   "(i)i", nullptr },
	{ "robot_ping_servo",   (void *)host_robot_ping_servo,   "(i)i", nullptr },

	// Robot — utility
	{ "robot_delay_ms",     (void *)host_robot_delay_ms,     "(i)i",  nullptr },

	// Robot — IK (per-leg)
	{ "robot_ik_fr",        (void *)host_robot_ik_fr,        "(fff)i", nullptr },
	{ "robot_ik_fl",        (void *)host_robot_ik_fl,        "(fff)i", nullptr },
	{ "robot_ik_rr",        (void *)host_robot_ik_rr,        "(fff)i", nullptr },
	{ "robot_ik_rl",        (void *)host_robot_ik_rl,        "(fff)i", nullptr },

	// Robot — IMU
	{ "robot_imu_read",     (void *)host_robot_imu_read,     "(i)i",  nullptr },
	{ "robot_imu_print",    (void *)host_robot_imu_print,    "()i",   nullptr },

	// Robot — low-level servo (Unitree-style, SDK v2)
	{ "servo_lock",           (void *)host_servo_lock,           "()i",     nullptr },
	{ "servo_unlock",         (void *)host_servo_unlock,         "()i",     nullptr },
	{ "servo_is_locked",      (void *)host_servo_is_locked,      "()i",     nullptr },
	{ "servo_set_gain",       (void *)host_servo_set_gain,       "(iif)i",  nullptr },
	{ "servo_get_gain",       (void *)host_servo_get_gain,       "(iii)i",  nullptr },
	{ "servo_save_config",    (void *)host_servo_save_config,    "(i)i",    nullptr },
	{ "servo_restore_config", (void *)host_servo_restore_config, "(i)i",    nullptr },
	{ "servo_stage",          (void *)host_servo_stage,          "(iffff)i",nullptr },
	{ "servo_commit",         (void *)host_servo_commit,         "()i",     nullptr },
	{ "servo_write_all",      (void *)host_servo_write_all,      "(ii)i",   nullptr },
	{ "servo_read",           (void *)host_servo_read,           "(ii)i",   nullptr },
	{ "servo_read_all",       (void *)host_servo_read_all,       "(i)i",    nullptr },
	{ "servo_poll",           (void *)host_servo_poll,           "()i",     nullptr },
	{ "servo_direct",         (void *)host_servo_direct,         "(iiff)i", nullptr },
	{ "servo_scan",           (void *)host_servo_scan,           "()i",     nullptr },

	// ── ABI v3 ──────────────────────────────────────────────────────────────
	// Control arbitration (opt-in; no effect until a skill calls take())
	{ "mpx_control_take",     (void *)host_mpx_control_take,     "(i)i",    nullptr },
	{ "mpx_control_release",  (void *)host_mpx_control_release,  "()i",     nullptr },
	{ "mpx_control_owner",    (void *)host_mpx_control_owner,    "()i",     nullptr },
	// Clock
	{ "mpx_millis",           (void *)host_mpx_millis,           "()i",     nullptr },
	{ "mpx_sleep_until",      (void *)host_mpx_sleep_until,      "(i)i",    nullptr },
	// Continuous drive
	{ "mpx_drive",            (void *)host_mpx_drive,            "(fff)i",  nullptr },
	{ "mpx_drive_stop",       (void *)host_mpx_drive_stop,       "()i",     nullptr },
	{ "mpx_set_walk_speed",   (void *)host_mpx_set_walk_speed,   "(i)i",    nullptr },
	{ "mpx_get_walk_speed",   (void *)host_mpx_get_walk_speed,   "()i",     nullptr },
	// Foot placement
	{ "mpx_foot",             (void *)host_mpx_foot,             "(ifff)i", nullptr },
	// Previously unreachable capabilities
	{ "mpx_set_all_servo_speed",(void *)host_mpx_set_all_servo_speed,"(i)i",nullptr },
	{ "mpx_reset_offsets",    (void *)host_mpx_reset_offsets,    "()i",     nullptr },
	{ "mpx_read_temperature_c",(void *)host_mpx_read_temperature_c,"(i)f",  nullptr },
	{ "mpx_board_scale_dd",   (void *)host_mpx_board_scale_dd,   "()f",     nullptr },
	// Skill parameters
	{ "mpx_param_f",          (void *)host_mpx_param_f,          "($f)f",   nullptr },
	{ "mpx_param_i",          (void *)host_mpx_param_i,          "($i)i",   nullptr },

	// ── ABI v4 ──────────────────────────────────────────────────────────────
	// Composing with the gait rather than replacing it
	{ "mpx_overlay",          (void *)host_mpx_overlay,          "(if)i",   nullptr },
	{ "mpx_overlay_get",      (void *)host_mpx_overlay_get,      "(i)f",    nullptr },
	{ "mpx_overlay_clear",    (void *)host_mpx_overlay_clear,    "()i",     nullptr },
	// Running inside the control loop
	{ "mpx_tick_every",       (void *)host_mpx_tick_every,       "(i)i",    nullptr },
	{ "mpx_tick_stop",        (void *)host_mpx_tick_stop,        "()i",     nullptr },
	// Being able to see what a control loop is doing
	{ "mpx_trace",            (void *)host_mpx_trace,            "($f)i",   nullptr },

	// ── ABI v6 ──────────────────────────────────────────────────────────────
	// Display: Lua's display.* canvas, then the agent's high-level tools
	{ "display_width",        (void *)host_display_width,        "()i",     nullptr },
	{ "display_height",       (void *)host_display_height,       "()i",     nullptr },
	{ "display_rgb",          (void *)host_display_rgb,          "(iii)i",  nullptr },
	{ "display_clear",        (void *)host_display_clear,        "(i)i",    nullptr },
	{ "display_pixel",        (void *)host_display_pixel,        "(iii)i",  nullptr },
	{ "display_rect",         (void *)host_display_rect,         "(iiiiiii)i", nullptr },
	{ "display_circle",       (void *)host_display_circle,       "(iiiii)i",nullptr },
	{ "display_line",         (void *)host_display_line,         "(iiiiii)i",nullptr },
	{ "display_arc",          (void *)host_display_arc,          "(iiiiiii)i", nullptr },
	{ "display_text",         (void *)host_display_text,         "(ii$iiii)i", nullptr },
	{ "display_blit",         (void *)host_display_blit,         "(iiiiii)i", nullptr },
	{ "display_show",         (void *)host_display_show,         "(i)i",    nullptr },
	{ "display_face",         (void *)host_display_face,         "()i",     nullptr },
	{ "display_show_text",    (void *)host_display_show_text,    "($)i",    nullptr },
	{ "display_show_emotion", (void *)host_display_show_emotion, "($)i",    nullptr },
	{ "display_set_colors",   (void *)host_display_set_colors,   "(ii)i",   nullptr },
	{ "display_set_brightness",(void *)host_display_set_brightness,"(i)i",  nullptr },
	// Filesystem: Lua's fs.*
	{ "fs_read",              (void *)host_fs_read,              "($ii)i",  nullptr },
	{ "fs_write",             (void *)host_fs_write,             "($ii)i",  nullptr },
	{ "fs_delete",            (void *)host_fs_delete,            "($)i",    nullptr },
	{ "fs_exists",            (void *)host_fs_exists,            "($)i",    nullptr },
	{ "fs_list",              (void *)host_fs_list,              "($ii)i",  nullptr },
	{ "fs_info",              (void *)host_fs_info,              "(i)i",    nullptr },
	// Music: the agent's music tools
	{ "music_play",           (void *)host_music_play,           "($ii)i",  nullptr },
	{ "music_stop",           (void *)host_music_stop,           "()i",     nullptr },
	// Robot state: the composite getters
	{ "robot_get_config",     (void *)host_robot_get_config,     "(i)i",    nullptr },
	{ "robot_get_gait",       (void *)host_robot_get_gait,       "(ii)i",   nullptr },

	// ── ABI v7 ──────────────────────────────────────────────────────────────
	// The microphone: a skill's own ears, while voice is held back
	{ "mpx_mic_start",        (void *)host_mpx_mic_start,        "(i)i",    nullptr },
	{ "mpx_mic_read",         (void *)host_mpx_mic_read,         "(ii)i",   nullptr },
	{ "mpx_mic_stop",         (void *)host_mpx_mic_stop,         "()i",     nullptr },
};

static constexpr uint32_t NUM_NATIVE_SYMBOLS =
	sizeof(NATIVE_SYMBOLS) / sizeof(NATIVE_SYMBOLS[0]);

}  // namespace sdk
