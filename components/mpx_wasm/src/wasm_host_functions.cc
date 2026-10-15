#include "wasm_host_functions.h"

#include <cinttypes>
#include <cmath>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wasm_export.h"

#include "robot.h"
extern "C" {
#include "driver_board.h"
}
#include "wasm_sandbox.h"
#include "movement.h"
#include "trace_ring.h"

static const char *TAG = "wasm_sdk";

namespace sdk {

int32_t host_mpx_abi_version(wasm_exec_env_t exec_env)
{
	// Deliberately does NOT check was_cancelled(): a skill asking what it is
	// talking to should always get an answer.
	(void)exec_env;
	return MPX_ABI_VERSION;
}

int32_t host_print(wasm_exec_env_t exec_env,
				   int32_t text_ptr, int32_t len)
{
	// Check for watchdog cancellation — if the sandbox timeout has
	// fired, bail out so the WASM thread can terminate promptly.
	if (wasm::was_cancelled()) {
		return MPX_ERR_CANCELLED;
	}

	if (len <= 0 || text_ptr == 0) {
		ESP_LOGW(TAG, "print called with invalid args (ptr=%" PRId32 ", len=%" PRId32 ")",
				 text_ptr, len);
		return -1;
	}

	// Clamp length to a sane maximum to prevent runaway reads
	if (len > 4096) {
		ESP_LOGW(TAG, "print text too long (%" PRId32 "), truncating to 4096", len);
		len = 4096;
	}

	// With the "$" signature in our NativeSymbol, WAMR already converts
	// the wasm linear-memory offset to a native pointer before calling
	// this function.  So text_ptr IS a native pointer — do NOT call
	// wasm_runtime_addr_app_to_native again.
	const char *wasm_ptr = reinterpret_cast<const char *>(
		static_cast<uintptr_t>(text_ptr));

	if (!wasm_ptr) {
		ESP_LOGE(TAG, "print: invalid pointer %p", wasm_ptr);
		return -1;
	}

	// Copy into a stack buffer + null-terminate
	char buf[4096];
	const int copy_len = (len < 4096) ? len : 4095;
	std::memcpy(buf, wasm_ptr, static_cast<std::size_t>(copy_len));
	buf[copy_len] = '\0';

	ESP_LOGI(TAG, "print: \"%s\"", buf);

	return 0;
}

// ═══════════════════════════════════════════════════════════════
//  Robot host functions
// ═══════════════════════════════════════════════════════════════

int32_t host_robot_gait(wasm_exec_env_t exec_env,
						int32_t name_ptr)
{
	// Check for watchdog cancellation
	if (wasm::was_cancelled()) {
	if (!control_allows(MPX_CTRL_GAIT)) return MPX_ERR_BUSY;
		return MPX_ERR_CANCELLED;
	}

	// With "$" signature, name_ptr is already a native pointer
	const char *name = reinterpret_cast<const char *>(
		static_cast<uintptr_t>(name_ptr));

	if (!name || !*name) {
		ESP_LOGW(TAG, "robot_gait: empty name");
		return -1;
	}

	ESP_LOGI(TAG, "robot_gait: \"%s\"", name);

	// One table, in robot.cc. This chain used to be a second copy of it.
	//
	// Routing through skills::movement means a skill saying mpx_gait("x") sees
	// the same name space the web UI does -- including movements provided by
	// other skills, which are refused here rather than silently doing nothing.
	const skills::MovementResult r = skills::run(name, /*from_skill=*/true);
	switch (r) {
		case skills::MovementResult::Started:
			return MPX_OK;
		case skills::MovementResult::NotPermitted:
			ESP_LOGW(TAG, "robot_gait(\"%s\"): provided by another skill; "
			              "only one skill runs at a time", name);
			return MPX_ERR_STATE;
		case skills::MovementResult::Busy:
			return MPX_ERR_STATE;
		case skills::MovementResult::Unknown:
		default:
			ESP_LOGW(TAG, "robot_gait: unknown movement \"%s\"", name);
			return MPX_ERR_ARG;
	}
}

int32_t host_robot_get_mode(wasm_exec_env_t exec_env)
{
	int32_t mode = static_cast<int32_t>(robot::current_gait_cmd());
	ESP_LOGI(TAG, "robot_get_mode: %" PRId32, mode);
	return mode;
}

int32_t host_robot_set_body_pose(wasm_exec_env_t exec_env,
                                 float roll_deg, float pitch_deg,
                                 float yaw_deg)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	if (!control_allows(MPX_CTRL_GAIT)) return MPX_ERR_BUSY;

	robot::set_body_attitude(roll_deg, pitch_deg, yaw_deg);
	ESP_LOGI(TAG, "robot_set_body_pose: roll=%.1f pitch=%.1f yaw=%.1f",
			 roll_deg, pitch_deg, yaw_deg);
	return 0;
}

int32_t host_robot_set_attitude_speed(wasm_exec_env_t exec_env,
                                      int32_t dps)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;

	robot::set_attitude_speed(static_cast<float>(dps));
	ESP_LOGI(TAG, "robot_set_attitude_speed: %" PRId32 " dps", dps);
	return 0;
}

int32_t host_robot_set_attitude_speed_xyz(wasm_exec_env_t exec_env,
                                          int32_t roll_dps, int32_t pitch_dps,
                                          int32_t yaw_dps)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;

	robot::set_attitude_speed_xyz(static_cast<float>(roll_dps),
	                              static_cast<float>(pitch_dps),
	                              static_cast<float>(yaw_dps));
	ESP_LOGI(TAG, "robot_set_attitude_speed_xyz: r=%" PRId32 " p=%" PRId32
	              " y=%" PRId32 " dps", roll_dps, pitch_dps, yaw_dps);
	return 0;
}

int32_t host_robot_set_config(wasm_exec_env_t exec_env,
							  int32_t period, int32_t height,
							  int32_t up_height, int32_t stride,
							  int32_t tilt)
{
	// Start from the CURRENT config so fields not exposed to WASM
	// (e.g. sg_speed, the Stanford walk speed) keep their values.
	robot::Config cfg = robot::get_config();
	cfg.period    = static_cast<int>(period);
	cfg.height    = static_cast<int>(height);
	cfg.up_height = static_cast<int>(up_height);
	cfg.stride    = static_cast<int>(stride);
	cfg.tilt      = static_cast<int>(tilt);
	robot::set_config(cfg);

	ESP_LOGI(TAG, "robot_set_config: p=%d h=%d uh=%d s=%d t=%d",
			 cfg.period, cfg.height, cfg.up_height, cfg.stride, cfg.tilt);
	return 0;
}

int32_t host_robot_get_period(wasm_exec_env_t exec_env)
{
	int32_t val = static_cast<int32_t>(robot::get_config().period);
	ESP_LOGD(TAG, "robot_get_period: %" PRId32, val);
	return val;
}

int32_t host_robot_get_height(wasm_exec_env_t exec_env)
{
	int32_t val = static_cast<int32_t>(robot::get_config().height);
	ESP_LOGD(TAG, "robot_get_height: %" PRId32, val);
	return val;
}

int32_t host_robot_get_up_height(wasm_exec_env_t exec_env)
{
	int32_t val = static_cast<int32_t>(robot::get_config().up_height);
	ESP_LOGD(TAG, "robot_get_up_height: %" PRId32, val);
	return val;
}

int32_t host_robot_get_stride(wasm_exec_env_t exec_env)
{
	int32_t val = static_cast<int32_t>(robot::get_config().stride);
	ESP_LOGD(TAG, "robot_get_stride: %" PRId32, val);
	return val;
}

int32_t host_robot_get_tilt(wasm_exec_env_t exec_env)
{
	int32_t val = static_cast<int32_t>(robot::get_config().tilt);
	ESP_LOGD(TAG, "robot_get_tilt: %" PRId32, val);
	return val;
}

int32_t host_robot_set_servo_angle(wasm_exec_env_t exec_env,
								   int32_t id, int32_t centideg)
{
	if (!control_allows(MPX_CTRL_JOINTS)) return MPX_ERR_BUSY;
	if (id < 1 || id > 12) {
		ESP_LOGW(TAG, "set_servo_angle: invalid id %" PRId32, id);
		return -1;
	}
	robot::set_servo_angle(static_cast<int>(id),
						   static_cast<float>(centideg) / 100.0f);
	ESP_LOGV(TAG, "robot_set_servo_angle: id=%" PRId32 " deg=%.2f",
			 id, static_cast<float>(centideg) / 100.0f);
	return 0;
}

int32_t host_robot_flush(wasm_exec_env_t exec_env)
{
	robot::flush();
	ESP_LOGV(TAG, "robot_flush");
	return 0;
}

int32_t host_robot_set_servo_speed(wasm_exec_env_t exec_env,
								   int32_t id, int32_t speed)
{
	if (id < 1 || id > 12) {
		ESP_LOGW(TAG, "set_servo_speed: invalid id %" PRId32, id);
		return -1;
	}
	robot::set_servo_speed(static_cast<int>(id),
						   static_cast<uint16_t>(speed));
	ESP_LOGV(TAG, "robot_set_servo_speed: id=%" PRId32 " speed=%" PRId32, id, speed);
	return 0;
}

int32_t host_robot_read_position(wasm_exec_env_t exec_env,
								 int32_t id)
{
	if (id < 1 || id > 12) {
		ESP_LOGW(TAG, "read_position: invalid id %" PRId32, id);
		return -1;
	}
	int32_t val = static_cast<int32_t>(robot::read_position(static_cast<int>(id)));
	ESP_LOGD(TAG, "read_position: id=%" PRId32 " pos=%" PRId32, id, val);
	return val;
}

int32_t host_robot_read_angle_cdeg(wasm_exec_env_t exec_env,
								   int32_t id)
{
	// Same frame as robot_set_servo_angle(), so read -> compare -> correct
	// actually converges. robot_read_position() is the OPPOSITE frame; see
	// the frame note at the top of robot.h.
	if (id < 1 || id > 12) {
		ESP_LOGW(TAG, "read_angle_cdeg: invalid id %" PRId32, id);
		return INT32_MIN;
	}
	int32_t val = static_cast<int32_t>(robot::read_angle_cdeg(static_cast<int>(id)));
	ESP_LOGD(TAG, "read_angle_cdeg: id=%" PRId32 " cdeg=%" PRId32, id, val);
	return val;
}

int32_t host_robot_read_speed(wasm_exec_env_t exec_env,
							  int32_t id)
{
	if (id < 1 || id > 12) {
		ESP_LOGW(TAG, "read_speed: invalid id %" PRId32, id);
		return -1;
	}
	int32_t val = static_cast<int32_t>(robot::read_speed(static_cast<int>(id)));
	ESP_LOGD(TAG, "read_speed: id=%" PRId32 " speed=%" PRId32, id, val);
	return val;
}

int32_t host_robot_read_load(wasm_exec_env_t exec_env,
							 int32_t id)
{
	if (id < 1 || id > 12) {
		ESP_LOGW(TAG, "read_load: invalid id %" PRId32, id);
		return -1;
	}
	int32_t val = static_cast<int32_t>(robot::read_load(static_cast<int>(id)));
	ESP_LOGD(TAG, "read_load: id=%" PRId32 " load=%" PRId32, id, val);
	return val;
}

int32_t host_robot_read_voltage(wasm_exec_env_t exec_env,
								int32_t id)
{
	if (id < 1 || id > 12) {
		ESP_LOGW(TAG, "read_voltage: invalid id %" PRId32, id);
		return -1;
	}
	int32_t val = static_cast<int32_t>(robot::read_voltage(static_cast<int>(id)));
	ESP_LOGD(TAG, "read_voltage: id=%" PRId32 " mV=%" PRId32, id, val);
	return val;
}

int32_t host_robot_read_temperature(wasm_exec_env_t exec_env,
									int32_t id)
{
	if (id < 1 || id > 12) {
		ESP_LOGW(TAG, "read_temperature: invalid id %" PRId32, id);
		return -1;
	}
	int32_t val = static_cast<int32_t>(robot::read_temperature(static_cast<int>(id)));
	ESP_LOGD(TAG, "read_temperature: id=%" PRId32 " temp=%" PRId32, id, val);
	return val;
}

int32_t host_robot_read_moving(wasm_exec_env_t exec_env,
							   int32_t id)
{
	if (id < 1 || id > 12) {
		ESP_LOGW(TAG, "read_moving: invalid id %" PRId32, id);
		return -1;
	}
	int32_t val = static_cast<int32_t>(robot::read_moving(static_cast<int>(id)));
	ESP_LOGD(TAG, "read_moving: id=%" PRId32 " moving=%" PRId32, id, val);
	return val;
}

int32_t host_robot_read_current(wasm_exec_env_t exec_env,
								int32_t id)
{
	if (id < 1 || id > 12) {
		ESP_LOGW(TAG, "read_current: invalid id %" PRId32, id);
		return -1;
	}
	int32_t val = static_cast<int32_t>(robot::read_current(static_cast<int>(id)));
	ESP_LOGD(TAG, "read_current: id=%" PRId32 " mA=%" PRId32, id, val);
	return val;
}

int32_t host_robot_set_offset(wasm_exec_env_t exec_env,
							  int32_t id, int32_t centideg)
{
	if (id < 1 || id > 12) {
		ESP_LOGW(TAG, "set_offset: invalid id %" PRId32, id);
		return -1;
	}
	robot::set_offset(static_cast<int>(id),
					  static_cast<float>(centideg) / 100.0f);
	ESP_LOGV(TAG, "set_offset: id=%" PRId32 " deg=%.2f",
			 id, static_cast<float>(centideg) / 100.0f);
	return 0;
}

int32_t host_robot_get_offset(wasm_exec_env_t exec_env,
							  int32_t id)
{
	if (id < 1 || id > 12) {
		ESP_LOGW(TAG, "get_offset: invalid id %" PRId32, id);
		return 0;
	}
	float deg = robot::get_offset(static_cast<int>(id));
	ESP_LOGV(TAG, "get_offset: id=%" PRId32 " deg=%.2f", id, deg);
	return static_cast<int32_t>(deg * 100.0f);
}

int32_t host_robot_ping_servo(wasm_exec_env_t exec_env,
							  int32_t id)
{
	if (id < 1 || id > 12) {
		ESP_LOGW(TAG, "ping_servo: invalid id %" PRId32, id);
		return -1;
	}
	int32_t result = static_cast<int32_t>(
		robot::ping_servo(static_cast<int>(id)));
	ESP_LOGV(TAG, "ping_servo: id=%" PRId32 " result=%" PRId32, id, result);
	return result;
}

int32_t host_robot_delay_ms(wasm_exec_env_t exec_env,
							int32_t ms)
{
	if (ms <= 0) return 0;

	// Break the delay into small chunks so we can check for
	// watchdog cancellation.  This lets the WASM thread terminate
	// promptly when the sandbox timeout fires.
	constexpr TickType_t CHUNK_MS = 50;
	int32_t remaining = ms;

	ESP_LOGV(TAG, "robot_delay_ms: %" PRId32 " ms", ms);

	while (remaining > 0) {
		if (wasm::was_cancelled()) {
			ESP_LOGW(TAG, "robot_delay_ms: cancelled after %" PRId32 " ms",
					 ms - remaining);
			return -1;
		}

		TickType_t delay = pdMS_TO_TICKS(
			(remaining > static_cast<int32_t>(CHUNK_MS))
				? CHUNK_MS
				: static_cast<TickType_t>(remaining));
		vTaskDelay(delay);
		remaining -= static_cast<int32_t>(CHUNK_MS);
	}

	return 0;
}

// ═══════════════════════════════════════════════════════════════
//  Inverse Kinematics host functions
// ═══════════════════════════════════════════════════════════════

int32_t host_robot_ik_fr(wasm_exec_env_t exec_env,
						 float x, float th0, float z)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	if (!control_allows(MPX_CTRL_FEET)) return MPX_ERR_BUSY;
	ESP_LOGD(TAG, "ik_fr: x=%.1f th0=%.1f z=%.1f", x, th0, z);
	robot::front_right_ik(x, th0, z);
	return 0;
}

int32_t host_robot_ik_fl(wasm_exec_env_t exec_env,
						 float x, float th0, float z)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	if (!control_allows(MPX_CTRL_FEET)) return MPX_ERR_BUSY;
	ESP_LOGD(TAG, "ik_fl: x=%.1f th0=%.1f z=%.1f", x, th0, z);
	robot::front_left_ik(x, th0, z);
	return 0;
}

int32_t host_robot_ik_rr(wasm_exec_env_t exec_env,
						 float x, float th0, float z)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	if (!control_allows(MPX_CTRL_FEET)) return MPX_ERR_BUSY;
	ESP_LOGD(TAG, "ik_rr: x=%.1f th0=%.1f z=%.1f", x, th0, z);
	robot::rear_right_ik(x, th0, z);
	return 0;
}

int32_t host_robot_ik_rl(wasm_exec_env_t exec_env,
						 float x, float th0, float z)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	if (!control_allows(MPX_CTRL_FEET)) return MPX_ERR_BUSY;
	ESP_LOGD(TAG, "ik_rl: x=%.1f th0=%.1f z=%.1f", x, th0, z);
	robot::rear_left_ik(x, th0, z);
	return 0;
}

// ═══════════════════════════════════════════════════════════════
//  IMU host functions
// ═══════════════════════════════════════════════════════════════

int32_t host_robot_imu_read(wasm_exec_env_t exec_env,
							int32_t buffer_ptr)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	if (buffer_ptr == 0) {
		ESP_LOGW(TAG, "robot_imu_read: null buffer pointer");
		return -1;
	}

	// Convert WASM linear-memory offset to a native pointer
	wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
	if (!module_inst) {
		ESP_LOGE(TAG, "robot_imu_read: failed to get module instance");
		return -1;
	}

	void *native_ptr = wasm_runtime_addr_app_to_native(
		module_inst, static_cast<uint32_t>(buffer_ptr));
	if (!native_ptr) {
		ESP_LOGE(TAG, "robot_imu_read: invalid WASM pointer %" PRId32, buffer_ptr);
		return -1;
	}

	// Read the latest IMU data
	robot::ImuData data = robot::imu_read();

	// Write 6 floats into the WASM buffer (ax, ay, az, gx, gy, gz)
	float *buf = static_cast<float *>(native_ptr);
	buf[0] = data.ax;
	buf[1] = data.ay;
	buf[2] = data.az;
	buf[3] = data.gx;
	buf[4] = data.gy;
	buf[5] = data.gz;

	ESP_LOGD(TAG, "robot_imu_read: ax=%.2f ay=%.2f az=%.2f gx=%.2f gy=%.2f gz=%.2f",
			 data.ax, data.ay, data.az, data.gx, data.gy, data.gz);
	return 0;
}

int32_t host_robot_imu_print(wasm_exec_env_t exec_env)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	ESP_LOGV(TAG, "robot_imu_print");
	robot::imu_print();
	return 0;
}


// ═══════════════════════════════════════════════════════════════
//  Low-level servo host functions  (Unitree-style)
//
//  These bypass the gait and the IK layer and talk to the AT32 driver boards
//  directly, so a skill can stream joint commands and tune the control gains.
//
//  Two paths, because the hardware has two:
//
//   - COMMAND path (fast): position + current limit, staged per servo and
//     pushed to all four boards in one commit. This is the per-tick path.
//     Its kp/kd fields exist in the wire frame but the stock AT32 firmware
//     ignores them — see servo_stage()'s note.
//
//   - CONFIG path (slow): kp_position, kd_position, kp_current, kff_current,
//     max_pwm_duty. Each write is a request/reply pair over SPI, roughly a
//     millisecond, and it must not interleave with gait traffic. Set these
//     once when the skill starts, not every tick.
//
//  Everything here requires the skill to hold the bus (servo_lock), which
//  parks the gait. The sandbox force-releases the lock when the skill ends.
// ═══════════════════════════════════════════════════════════════

namespace {

// Resolve a WASM linear-memory offset to a native pointer, checking that the
// whole span is inside the sandbox's memory — a skill must not be able to
// hand us an offset that walks off the end of its heap.
void *wasm_ptr(wasm_exec_env_t exec_env, int32_t offset, uint32_t bytes)
{
	if (offset == 0) return nullptr;
	wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
	if (!inst) return nullptr;
	if (!wasm_runtime_validate_app_addr(inst, static_cast<uint32_t>(offset), bytes)) {
		ESP_LOGW(TAG, "servo: rejected out-of-bounds pointer %" PRId32 " (+%" PRIu32 ")",
				 offset, bytes);
		return nullptr;
	}
	return wasm_runtime_addr_app_to_native(inst, static_cast<uint32_t>(offset));
}

inline bool servo_id_ok(int32_t id) { return id >= 1 && id <= 12; }

}  // namespace

int32_t host_servo_lock(wasm_exec_env_t exec_env)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	return robot::servo_lock() ? 0 : -1;
}

int32_t host_servo_unlock(wasm_exec_env_t exec_env)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	robot::servo_unlock();
	return 0;
}

int32_t host_servo_is_locked(wasm_exec_env_t exec_env)
{
	// Answers "do *I* hold the bus", which is the only useful question for a
	// skill. It used to answer "is the bus held by anyone", so a skill could
	// see 1 while Servo Studio owned it, conclude it was safe to write, and
	// be wrong.
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	return robot::servo_owned_by_skill() ? 1 : 0;
}

// ── Config path: the tunable gains ──────────────────────────────

/* Parameters that define the joint's physical angle mapping rather than its
 * control response. A skill must never write these:
 *
 *   1 MIN_POSITION_ADC    2 MAX_POSITION_ADC    3 RANGE_POSITION_DEG
 *
 * They are the board's own calibration. Change one and every angle command
 * afterwards means something different — including the gait's, including the
 * NVS offsets calibrated against the old mapping — and servo_save_config()
 * burns the new value into the driver board's flash where a reboot will not
 * clear it. A typo in a skill could silently mis-calibrate a joint for good.
 *
 * Reads stay available through servo_get_gain(), which is how a skill should
 * discover the mapping it is working in. Genuine recalibration goes through
 * Servo Studio, where a human is watching the joint move.
 */
static inline bool param_is_read_only(int32_t param)
{
	/* Calibration, not control gains. Each of these changes what every angle
	 * command MEANS afterwards -- including the built-in gaits' -- and
	 * servo_save_config() can burn the mistake into the driver board's own
	 * flash, where a reboot will not clear it.
	 *
	 * The two REVERSE_* slots are here for a stronger reason than the ADC
	 * ones. They flip a direction, so a skill that writes one leaves a single
	 * joint driving opposite to the other eleven. That is not a robot that
	 * behaves oddly; it is a robot tearing at its own legs, and it survives
	 * the skill that caused it with nothing on screen to explain why. No
	 * downloaded skill has a legitimate reason to reverse a motor.
	 *
	 * All of them are still settable from Servo Studio, where a human is
	 * watching the joint move. */
	return param == DB_PARAM_MIN_POSITION_ADC
		|| param == DB_PARAM_MAX_POSITION_ADC
		|| param == DB_PARAM_RANGE_POSITION_DEG
		|| param == DB_PARAM_REVERSE_MOTOR
		|| param == DB_PARAM_REVERSE_POSITION_SENSOR;
}

/* ── Undoing what a skill did to the gains ──────────────────────────────────
 *
 * Gains live on the AT32 driver boards, not in the module, so they survive the
 * skill that wrote them — the bus lock being released, the skill returning,
 * even the skill crashing. That is deliberate and useful: it is how a skill
 * tunes the motors and then lets the firmware's own gait walk with that
 * tuning. It is also a trap, and the same trap the sandbox already closes
 * twice over: an overlay outliving its skill silently biases every later
 * movement, so the sandbox clears it; a held bus lock leaves the gait parked,
 * so the sandbox force-releases it. A skill that leaves Kp at 95 makes every
 * built-in gait afterwards walk slightly wrong with nothing on screen to
 * explain why. Same shape of bug, and it was the one left standing.
 *
 * So: remember the value that was there BEFORE the skill's first write to each
 * slot, and put it back on the way out. Not "restore stock" — restore what was
 * actually there, which may be a tuning a human set from Servo Studio and has
 * every reason to expect to still be theirs afterwards.
 *
 * TO KEEP A TUNING ON PURPOSE, there is already a way: mpx_gain_save() burns
 * it into the driver board's own flash, which is what "permanent" means here.
 * Restoring the RAM value afterwards does not touch that, and a reboot brings
 * the saved values back. So this needs no opt-out flag — the escape hatch is
 * the one that was always the right tool for the job.
 *
 * Cost: one extra config read the first time a skill touches a given slot,
 * paid once, not per write. 12 x DB_PARAM_COUNT of bookkeeping, under a
 * kilobyte, in BSS.
 */
static float s_gain_before[12][DB_PARAM_COUNT];
static bool  s_gain_saved [12][DB_PARAM_COUNT];
/* Counted, not narrated. One line per unreadable slot meant 45 warnings for a
 * bench robot with one board plugged in — enough to bury the run's actual
 * result. The fact is worth reporting once, with a number. */
static int   s_gain_unreadable;

static void remember_gain(int id, int param)
{
	const int i = id - 1;
	if (i < 0 || i >= 12 || param < 0 || param >= DB_PARAM_COUNT) return;
	if (s_gain_saved[i][param]) return;          /* first write only */

	float before = 0.0f;
	if (!driver_board_get_param(id, param, &before)) {
		/* Could not read it, so we must not pretend we can restore it. Leaving
		 * `saved` false means the value is left alone at the end rather than
		 * being overwritten with a guess. Tallied and reported once by
		 * restore_skill_gains(); at DEBUG if you want the individual slots. */
		s_gain_unreadable++;
		ESP_LOGD(TAG, "could not read servo %d param %d before writing; it "
					  "will not be restored", id, param);
		return;
	}
	s_gain_before[i][param] = before;
	s_gain_saved [i][param] = true;
}

void forget_skill_gains()
{
	std::memset(s_gain_saved, 0, sizeof(s_gain_saved));
	s_gain_unreadable = 0;
}

int restore_skill_gains()
{
	int restored = 0, failed = 0;
	for (int i = 0; i < 12; ++i) {
		for (int p = 0; p < DB_PARAM_COUNT; ++p) {
			if (!s_gain_saved[i][p]) continue;
			if (driver_board_set_param(i + 1, p, s_gain_before[i][p])) restored++;
			else                                                      failed++;
			s_gain_saved[i][p] = false;
		}
	}
	if (restored || failed || s_gain_unreadable) {
		ESP_LOGI(TAG, "restored %d gain(s) the skill changed%s%s",
				 restored,
				 failed ? " (some failed)" : "",
				 s_gain_unreadable ? " — and some could not be read beforehand,"
									 " so they were left alone" : "");
		if (s_gain_unreadable) {
			ESP_LOGI(TAG, "  %d slot(s) unreadable — a board that is not "
						  "answering; enable DEBUG on this tag to list them",
					 s_gain_unreadable);
		}
	}
	s_gain_unreadable = 0;
	return restored;
}

int32_t host_servo_set_gain(wasm_exec_env_t exec_env,
							int32_t id, int32_t param, float value)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	if (!servo_id_ok(id) || param < 0 || param >= DB_PARAM_COUNT) return -1;
	if (param_is_read_only(param)) {
		ESP_LOGW(TAG, "servo_set_gain: param %" PRId32 " is calibration, not a gain "
					  "— read-only from a skill", param);
		return -4;
	}
	if (!robot::servo_owned_by_skill()) {
		ESP_LOGW(TAG, "servo_set_gain: bus not locked — call servo_lock() first");
		return -2;
	}
	remember_gain(id, param);   /* so the sandbox can put it back afterwards */
	if (!driver_board_set_param(id, param, value)) return -3;
	ESP_LOGD(TAG, "servo_set_gain: id=%" PRId32 " p=%" PRId32 " v=%.4f", id, param, value);
	return 0;
}

int32_t host_servo_get_gain(wasm_exec_env_t exec_env,
							int32_t id, int32_t param, int32_t out_ptr)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	if (!servo_id_ok(id) || param < 0 || param >= DB_PARAM_COUNT) return -1;
	if (!robot::servo_owned_by_skill()) return -2;

	float *out = static_cast<float *>(wasm_ptr(exec_env, out_ptr, sizeof(float)));
	if (!out) return -1;

	float v = 0.0f;
	if (!driver_board_get_param(id, param, &v)) return -3;
	*out = v;
	return 0;
}

/* id 0 means "every board"; 1..12 means "the board that servo lives on".
 *
 * These used to compute `servo_id_ok(id) ? (id-1)/3 : -1`, and -1 is
 * driver_board's code for ALL BOARDS. So id 13, id -5 and id 9999 all quietly
 * meant "every board" — and for servo_restore_config that is a factory reset
 * of all twelve servos' calibration, returned as 0 for success. Every other
 * servo_* entry point rejects a bad id with -1; these two now do too.
 */
static inline bool config_scope_ok(int32_t id, int *board_out)
{
	if (id == 0) { *board_out = -1; return true; }            // all boards
	if (!servo_id_ok(id)) return false;                        // reject, do not widen
	*board_out = (static_cast<int>(id) - 1) / 3;
	return true;
}

int32_t host_servo_save_config(wasm_exec_env_t exec_env, int32_t id)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	int board;
	if (!config_scope_ok(id, &board)) return -1;
	if (!robot::servo_owned_by_skill()) return -2;
	return driver_board_save_config(board) ? 0 : -3;
}

int32_t host_servo_restore_config(wasm_exec_env_t exec_env, int32_t id)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	int board;
	if (!config_scope_ok(id, &board)) return -1;
	if (!robot::servo_owned_by_skill()) return -2;
	return driver_board_factory_restore(board) ? 0 : -3;
}

// ── Command path: stage / commit ────────────────────────────────

int32_t host_servo_stage(wasm_exec_env_t exec_env,
						 int32_t id, float q_deg, float tau_ma,
						 float kp, float kd)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	if (!servo_id_ok(id)) return -1;
	if (!robot::servo_owned_by_skill()) return -2;

	driver_board_stage(id, DB_MODE_POSITION, q_deg,
					   static_cast<int16_t>(tau_ma),
					   static_cast<uint16_t>(kp < 0 ? 0 : kp),
					   static_cast<uint16_t>(kd < 0 ? 0 : kd));
	return 0;
}

int32_t host_servo_commit(wasm_exec_env_t exec_env)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	if (!robot::servo_owned_by_skill()) return -2;
	return driver_board_commit() ? 0 : -3;
}

// servo_write_all: the closest thing to a Unitree LowCmd — an array of
// { q_deg, tau_ma, kp, kd } indexed by servo 1..12, staged and committed in
// one call, so a whole-robot update costs four SPI frames.
int32_t host_servo_write_all(wasm_exec_env_t exec_env,
							 int32_t cmd_ptr, int32_t count)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	if (count < 1 || count > 12) return -1;
	if (!robot::servo_owned_by_skill()) return -2;

	const uint32_t bytes = static_cast<uint32_t>(count) * 4u * sizeof(float);
	const float *cmd = static_cast<const float *>(wasm_ptr(exec_env, cmd_ptr, bytes));
	if (!cmd) return -1;

	for (int32_t i = 0; i < count; ++i) {
		const float *c = cmd + i * 4;
		driver_board_stage(static_cast<int>(i) + 1, DB_MODE_POSITION, c[0],
						   static_cast<int16_t>(c[1]),
						   static_cast<uint16_t>(c[2] < 0 ? 0 : c[2]),
						   static_cast<uint16_t>(c[3] < 0 ? 0 : c[3]));
	}
	return driver_board_commit() ? 0 : -3;
}

// servo_read_all: the LowState side. Four floats per servo:
//   q_deg   present position, raw AT32 degrees 0..270
//   tau_ma  present motor current, mA (signed)
//   temp_c  NTC temperature, degC — NaN if that servo never answered
//   q_raw   the same position on the SCS 0..1023 scale
//
// There is deliberately no velocity field: the boards do not measure one, and
// a hardcoded zero in a struct named "dq" is worse than its absence.
int32_t host_servo_read_all(wasm_exec_env_t exec_env, int32_t out_ptr)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;

	float *out = static_cast<float *>(wasm_ptr(exec_env, out_ptr, 12u * 4u * sizeof(float)));
	if (!out) return -1;

	for (int id = 1; id <= 12; ++id) {
		const uint16_t raw = driver_board_present_position(id);
		const float    t   = driver_board_present_temperature(id);
		float *o = out + (id - 1) * 4;
		o[0] = static_cast<float>(raw) * 270.0f / 1024.0f;
		o[1] = static_cast<float>(driver_board_present_current(id));
		o[2] = (t > DB_TEMP_INVALID) ? t : NAN;
		o[3] = static_cast<float>(raw);
	}
	return 0;
}

int32_t host_servo_read(wasm_exec_env_t exec_env, int32_t id, int32_t out_ptr)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	if (!servo_id_ok(id)) return -1;

	float *out = static_cast<float *>(wasm_ptr(exec_env, out_ptr, 4u * sizeof(float)));
	if (!out) return -1;

	const uint16_t raw = driver_board_present_position(id);
	const float    t   = driver_board_present_temperature(id);
	out[0] = static_cast<float>(raw) * 270.0f / 1024.0f;
	out[1] = static_cast<float>(driver_board_present_current(id));
	out[2] = (t > DB_TEMP_INVALID) ? t : NAN;
	out[3] = static_cast<float>(raw);
	return 0;
}

// Refresh the feedback cache while the gait is parked. With the gait running
// the cache is already refreshed every tick and this is unnecessary.
int32_t host_servo_poll(wasm_exec_env_t exec_env)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	if (!robot::servo_owned_by_skill()) return -2;
	bool ok = true;
	for (int b = 0; b < 4; ++b) ok &= driver_board_poll_board(b);
	return ok ? 0 : -3;
}

// mode: 0 = idle (motor off), 1 = position hold, 2 = torque
int32_t host_servo_direct(wasm_exec_env_t exec_env,
						  int32_t id, int32_t mode, float q_deg, float tau_ma)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	if (!servo_id_ok(id)) return -1;
	if (!robot::servo_owned_by_skill()) return -2;

	const uint16_t m = (mode == 1) ? DB_MODE_POSITION
					 : (mode == 2) ? DB_MODE_TORQUE
								   : DB_MODE_IDLE;
	return driver_board_direct(id, m, q_deg, static_cast<int16_t>(tau_ma)) ? 0 : -3;
}

// Bitmask of servos that answered a parameter read: bit 0 = servo 1.
int32_t host_servo_scan(wasm_exec_env_t exec_env)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	if (!robot::servo_owned_by_skill()) return -2;
	int32_t mask = 0;
	for (int id = 1; id <= 12; ++id) {
		float v;
		if (driver_board_get_param(id, DB_PARAM_KP_POSITION, &v)) mask |= (1 << (id - 1));
	}
	return mask;
}


// ═══════════════════════════════════════════════════════════════
//  ABI v3
// ═══════════════════════════════════════════════════════════════

// ── Control arbitration ────────────────────────────────────────
//
// Four things in this firmware can move a joint: the gait generator, the
// built-in IK, a skill's direct joint writes, and the servo bus. In v2 they
// all wrote the same goal buffer with no arbitration, so "what happens if I
// call robot_ik_fr() while a gait is running" had an answer nobody could look
// up — last writer wins, at 15 ms granularity.
//
// v3 does not change that default. It adds a claim: a skill that calls
// mpx_control_take(MPX_CTRL_FEET) is telling the firmware it owns foot
// placement, and any *other* domain's write is then refused with
// MPX_ERR_BUSY instead of silently interleaving. A skill that never calls
// take() sees byte-identical v2 behaviour.

static int32_t s_control_owner = MPX_CTRL_NONE;

void control_reset()
{
	s_control_owner = MPX_CTRL_NONE;
}

bool control_allows(int32_t domain)
{
	return s_control_owner == MPX_CTRL_NONE || s_control_owner == domain;
}

// Does a skill currently own the twelve goal positions?
//
// The gait task's idle branch asks this before rewriting them with the neutral
// stand. Until this existed, a control claim was checked ONLY inside host
// functions -- so it could refuse the skill's own calls from another layer and
// nothing else. Only one skill runs at a time, so it protected you from
// yourself and from no one, while the goal buffer was still overwritten
// underneath the skill that had claimed it.
bool control_owner_is_pose()
{
	return s_control_owner == MPX_CTRL_FEET || s_control_owner == MPX_CTRL_JOINTS;
}

int32_t host_mpx_control_take(wasm_exec_env_t exec_env, int32_t domain)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	if (domain < MPX_CTRL_GAIT || domain > MPX_CTRL_BUS) return MPX_ERR_ARG;
	if (s_control_owner != MPX_CTRL_NONE && s_control_owner != domain) {
		ESP_LOGW(TAG, "control_take(%" PRId32 ") refused; %" PRId32 " holds it",
				 domain, s_control_owner);
		return MPX_ERR_BUSY;
	}
	s_control_owner = domain;
	return MPX_OK;
}

int32_t host_mpx_control_release(wasm_exec_env_t exec_env)
{
	(void)exec_env;
	s_control_owner = MPX_CTRL_NONE;
	return MPX_OK;
}

int32_t host_mpx_control_owner(wasm_exec_env_t exec_env)
{
	(void)exec_env;
	return s_control_owner;
}

// ── Clock ──────────────────────────────────────────────────────
//
// v2 had no time source at all: robot_delay_ms was the only timing primitive,
// so a skill could not measure a frame, hold a rate, or run for a wall-clock
// duration. Everything time-shaped had to be counted in frames and hoped for.

int32_t host_mpx_millis(wasm_exec_env_t exec_env)
{
	(void)exec_env;
	return static_cast<int32_t>(wasm::skill_millis());
}

// Absolute-deadline sleep. Sleeping to a deadline instead of for a duration is
// what stops per-frame overhead accumulating into drift: 600 frames of
// "delay(16)" run long by however much 600 frames of host calls cost, but 600
// frames of sleep_until(start + n*16) do not.
int32_t host_mpx_sleep_until(wasm_exec_env_t exec_env, int32_t t_ms)
{
	(void)exec_env;
	for (;;) {
		if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
		const int32_t now = static_cast<int32_t>(wasm::skill_millis());
		const int32_t remaining = t_ms - now;
		if (remaining <= 0) return MPX_OK;
		// Wake at least every 50 ms so cancellation is honoured promptly.
		const int32_t slice = remaining > 50 ? 50 : remaining;
		vTaskDelay(pdMS_TO_TICKS(slice));
	}
}

// ── Continuous drive ───────────────────────────────────────────
//
// robot::joy_input() is what the phone UI's thumbsticks call. It was never in
// the ABI, so a skill could pick one of 46 discrete gaits but could not ask
// for "forward at a third of speed while turning gently" — the one thing the
// hardware was already doing for the web client.

int32_t host_mpx_drive(wasm_exec_env_t exec_env, float fwd, float strafe, float turn)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	if (!control_allows(MPX_CTRL_GAIT)) return MPX_ERR_BUSY;

	auto clamp1 = [](float v) { return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v); };
	robot::joy_input(clamp1(fwd), clamp1(strafe), clamp1(turn));
	return MPX_OK;
}

int32_t host_mpx_drive_stop(wasm_exec_env_t exec_env)
{
	(void)exec_env;
	robot::joy_input(0.0f, 0.0f, 0.0f);
	return MPX_OK;
}

int32_t host_mpx_set_walk_speed(wasm_exec_env_t exec_env, int32_t mm_s)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	robot::Config cfg = robot::get_config();
	cfg.sg_speed = static_cast<int>(mm_s);
	robot::set_config(cfg);          // set_config() already clamps to 10..200
	return MPX_OK;
}

int32_t host_mpx_get_walk_speed(wasm_exec_env_t exec_env)
{
	(void)exec_env;
	return static_cast<int32_t>(robot::get_config().sg_speed);
}

// ── Foot placement ─────────────────────────────────────────────
//
// One call instead of four differently-named ones, so a leg index can be a
// loop variable. Same maths as robot_ik_*; this is purely about the shape of
// the call site.
//
// Z CONVENTION, SO NOBODY HAS TO GUESS AGAIN: `z` arriving here is the
// FIRMWARE's — distance DOWN from the hip, POSITIVE, standing at +70, the same
// number robot::calculate_ik() and the idle loop's front_right_ik(0,0,
// s_cfg.height) use. The C SDK declares z as UP-positive and negates it inside
// mpx_foot_to() before the call; do not negate again here.
//
// It was not always converted. A skill asking for the standing height as -70
// resolved to servo2 = servo3 = -180 (atan2(x, zd) flips by pi once zd goes
// negative), clamped at the joint limit, and moved the legs somewhere nobody
// asked for -- silently, with no error and no warning. If this ever needs
// changing, change it in ONE of the two places, never both.

int32_t host_mpx_foot(wasm_exec_env_t exec_env, int32_t leg, float x, float th0, float z)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	if (!control_allows(MPX_CTRL_FEET)) return MPX_ERR_BUSY;

	switch (leg) {
	case MPX_LEG_FR: robot::front_right_ik(x, th0, z); return MPX_OK;
	case MPX_LEG_FL: robot::front_left_ik (x, th0, z); return MPX_OK;
	case MPX_LEG_RR: robot::rear_right_ik (x, th0, z); return MPX_OK;
	case MPX_LEG_RL: robot::rear_left_ik  (x, th0, z); return MPX_OK;
	default:         return MPX_ERR_ARG;
	}
}

// ── Capabilities that existed in robot.h but not in the ABI ────

int32_t host_mpx_set_all_servo_speed(wasm_exec_env_t exec_env, int32_t speed)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	if (speed < 0 || speed > 2047) return MPX_ERR_ARG;
	robot::set_all_servo_speed(static_cast<uint16_t>(speed));
	return MPX_OK;
}

int32_t host_mpx_reset_offsets(wasm_exec_env_t exec_env)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	robot::reset_offsets();
	return MPX_OK;
}

// Returns degrees Celsius, or -1.0f on a bad id. robot_read_temperature()
// already existed but truncated to a whole degree, which is too coarse to
// watch a joint heat up over a long routine.
float host_mpx_read_temperature_c(wasm_exec_env_t exec_env, int32_t id)
{
	(void)exec_env;
	if (id < 1 || id > 12) return -1.0f;
	return robot::read_temperature_c(static_cast<int>(id));
}

// ── Skill parameters ───────────────────────────────────────────
//
// on_start() takes no arguments, so "the same wave, but three times and
// faster" meant an edit and a recompile, and the web UI had no way to offer a
// knob. Parameters are supplied per run (POST /v1/skills/run) and read here by
// name, with a fallback the skill chooses — so a skill run with no parameters
// at all still behaves exactly as it was written.

float host_mpx_param_f(wasm_exec_env_t exec_env, int32_t name_ptr, float fallback)
{
	(void)exec_env;
	if (name_ptr == 0) return fallback;
	const char *name = reinterpret_cast<const char *>(static_cast<uintptr_t>(name_ptr));
	float out = 0.0f;
	return wasm::param_get(name, &out) ? out : fallback;
}

int32_t host_mpx_param_i(wasm_exec_env_t exec_env, int32_t name_ptr, int32_t fallback)
{
	(void)exec_env;
	if (name_ptr == 0) return fallback;
	const char *name = reinterpret_cast<const char *>(static_cast<uintptr_t>(name_ptr));
	float out = 0.0f;
	return wasm::param_get(name, &out) ? static_cast<int32_t>(out) : fallback;
}

// ═══════════════════════════════════════════════════════════════
//  v4 — overlay, tick, trace
// ═══════════════════════════════════════════════════════════════

int32_t host_mpx_overlay(wasm_exec_env_t exec_env, int32_t id, float deg)
{
	(void)exec_env;
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	if (id < 1 || id > 12) return MPX_ERR_ARG;

	// Deliberately NOT gated on control arbitration. The point of an overlay is
	// to ride on top of something else that owns the joints — usually the gait
	// generator. Refusing it unless the caller holds the joints would refuse it
	// in exactly the case it exists for. The clamp in robot::set_overlay() is
	// what makes that safe.
	robot::set_overlay(static_cast<int>(id), deg);
	return MPX_OK;
}

float host_mpx_overlay_get(wasm_exec_env_t exec_env, int32_t id)
{
	(void)exec_env;
	if (id < 1 || id > 12) return 0.0f;
	return robot::get_overlay(static_cast<int>(id));
}

int32_t host_mpx_overlay_clear(wasm_exec_env_t exec_env)
{
	(void)exec_env;
	robot::clear_overlay();
	return MPX_OK;
}

int32_t host_mpx_tick_every(wasm_exec_env_t exec_env, int32_t period_ms)
{
	(void)exec_env;
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	if (period_ms < 0) return MPX_ERR_ARG;
	wasm::tick_every(static_cast<int>(period_ms));
	return MPX_OK;
}

int32_t host_mpx_tick_stop(wasm_exec_env_t exec_env)
{
	(void)exec_env;
	wasm::tick_stop();
	return MPX_OK;
}

int32_t host_mpx_trace(wasm_exec_env_t exec_env, int32_t name_ptr, float value)
{
	(void)exec_env;
	// No was_cancelled() check here: the trace emitted on the way out of a
	// dying skill is often the one that explains why it died.
	if (name_ptr == 0) return MPX_ERR_ARG;

	// "$" in the signature means WAMR has already bounds-checked and converted
	// the pointer, so this is a native pointer into the sandbox's memory.
	const char *name = reinterpret_cast<const char *>(
		static_cast<uintptr_t>(name_ptr));
	if (!name) return MPX_ERR_ARG;

	util::trace_ring_put(name, value, wasm::skill_millis());
	return MPX_OK;
}

}  // namespace sdk

// ============================================================================
//  Registration function called by wasm_sandbox.cc after runtime init
// ============================================================================
extern "C" void wasm_host_functions_register()
{
	wasm::register_natives(
		"env",
		const_cast<NativeSymbol *>(sdk::NATIVE_SYMBOLS),
		sdk::NUM_NATIVE_SYMBOLS);

	ESP_LOGI(TAG, "SDK + Robot host functions registered (%" PRIu32 " symbols)",
			 sdk::NUM_NATIVE_SYMBOLS);
}
