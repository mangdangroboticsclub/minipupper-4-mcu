#include "sdk/wasm_host_functions.h"

#include <cinttypes>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "wasm_export.h"

extern "C" {
#include "robot/driver_board.h"
}

#include "display/cap_display.h"
#include "fs/littlefs_manager.h"
#include "network/chat_ws.h"
#include "robot/robot.h"
#include "util/trace_ring.h"
#include "wasm/wasm_sandbox.h"

#if CONFIG_APP_VOICE_ENABLE
#include "voice/cap_music.h"
#endif

static const char *TAG = "wasm_sdk";

namespace sdk {

int32_t host_print(wasm_exec_env_t exec_env,
				   int32_t text_ptr, int32_t len)
{
	// Check for watchdog cancellation — if the sandbox timeout has
	// fired, bail out so the WASM thread can terminate promptly.
	if (wasm::was_cancelled()) {
		return -1;
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
		return -1;
	}

	// With "$" signature, name_ptr is already a native pointer
	const char *name = reinterpret_cast<const char *>(
		static_cast<uintptr_t>(name_ptr));

	if (!name || !*name) {
		ESP_LOGW(TAG, "robot_gait: empty name");
		return -1;
	}

	ESP_LOGI(TAG, "robot_gait: \"%s\"", name);

	// One table, in robot.cc, shared with the HTTP layer and the gait task's
	// logging switch. It used to be three hand-maintained copies - three
	// chances for a name to mean different things depending on who asked - and
	// they had already drifted, so the SDK accepted names the HTTP layer
	// rejected.
	robot::GaitCmd cmd = robot::GaitCmd::None;
	if (!robot::gait_from_name(name, cmd)) {
		ESP_LOGW(TAG, "robot_gait: unknown gait \"%s\"", name);
		return MPX_ERR_ARG;
	}

	// A skill that claimed foot placement or direct joints should not be
	// steering the gait underneath itself.
	if (!control_allows(MPX_CTRL_GAIT)) return MPX_ERR_BUSY;

	robot::send_gait_cmd(cmd);
	return MPX_OK;
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
	if (wasm::was_cancelled()) return -1;

	robot::set_attitude_speed(static_cast<float>(dps));
	ESP_LOGI(TAG, "robot_set_attitude_speed: %" PRId32 " dps", dps);
	return 0;
}

int32_t host_robot_set_attitude_speed_xyz(wasm_exec_env_t exec_env,
                                          int32_t roll_dps, int32_t pitch_dps,
                                          int32_t yaw_dps)
{
	if (wasm::was_cancelled()) return -1;

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
	if (id < 1 || id > 12) {
		ESP_LOGW(TAG, "set_servo_angle: invalid id %" PRId32, id);
		return -1;
	}
	// A skill that claimed feet or the gait should not be writing joints
	// underneath its own claim.
	if (!control_allows(MPX_CTRL_JOINTS)) return MPX_ERR_BUSY;
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

int32_t host_robot_set_servo_current(wasm_exec_env_t exec_env,
									 int32_t id, int32_t milliamps)
{
	if (id < 1 || id > 12) {
		ESP_LOGW(TAG, "set_servo_current: invalid id %" PRId32, id);
		return -1;
	}
	// Negative is meaningless; robot::set_servo_current clamps the top end.
	if (milliamps < 0) milliamps = 0;
	robot::set_servo_current(static_cast<int>(id),
							 static_cast<uint16_t>(milliamps));
	ESP_LOGD(TAG, "robot_set_servo_current: id=%" PRId32 " mA=%" PRId32, id, milliamps);
	return 0;
}

int32_t host_robot_get_servo_current(wasm_exec_env_t exec_env,
									 int32_t id)
{
	if (id < 1 || id > 12) {
		ESP_LOGW(TAG, "get_servo_current: invalid id %" PRId32, id);
		return 0;
	}
	return static_cast<int32_t>(robot::get_servo_current(static_cast<int>(id)));
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
	if (id < 1 || id > 12) {
		ESP_LOGW(TAG, "read_angle_cdeg: invalid id %" PRId32, id);
		return INT32_MIN;
	}
	// No log line: this is meant to be polled in a control loop, and one line
	// per iteration is how a log becomes the reason the loop misses its period.
	return robot::read_angle_cdeg(static_cast<int>(id));
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
	return MPX_OK;
}

int32_t host_robot_ik_fl(wasm_exec_env_t exec_env,
						 float x, float th0, float z)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	if (!control_allows(MPX_CTRL_FEET)) return MPX_ERR_BUSY;
	ESP_LOGD(TAG, "ik_fl: x=%.1f th0=%.1f z=%.1f", x, th0, z);
	robot::front_left_ik(x, th0, z);
	return MPX_OK;
}

int32_t host_robot_ik_rr(wasm_exec_env_t exec_env,
						 float x, float th0, float z)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	if (!control_allows(MPX_CTRL_FEET)) return MPX_ERR_BUSY;
	ESP_LOGD(TAG, "ik_rr: x=%.1f th0=%.1f z=%.1f", x, th0, z);
	robot::rear_right_ik(x, th0, z);
	return MPX_OK;
}

int32_t host_robot_ik_rl(wasm_exec_env_t exec_env,
						 float x, float th0, float z)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	if (!control_allows(MPX_CTRL_FEET)) return MPX_ERR_BUSY;
	ESP_LOGD(TAG, "ik_rl: x=%.1f th0=%.1f z=%.1f", x, th0, z);
	robot::rear_left_ik(x, th0, z);
	return MPX_OK;
}

// ═══════════════════════════════════════════════════════════════
//  IMU host functions
// ═══════════════════════════════════════════════════════════════

int32_t host_robot_imu_read(wasm_exec_env_t exec_env,
							int32_t buffer_ptr)
{
	if (wasm::was_cancelled()) return -1;
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
	//
	// SIX, and it has to stay six. The guest allocates this buffer, so writing
	// a seventh float would run off the end of whatever it reserved. The IMU on
	// the new board also has a magnetometer (BMM150) and this function could
	// carry it, but that is an ABI change: it needs a new symbol and a version
	// bump, not a wider write here. Lua has no such constraint — see
	// robot.imu_read() in lua_bindings.cc, which returns the mag fields.
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
	if (wasm::was_cancelled()) return -1;
	ESP_LOGV(TAG, "robot_imu_print");
	robot::imu_print();
	return 0;
}

// ═══════════════════════════════════════════════════════════════
//  ABI v2-v5
// ═══════════════════════════════════════════════════════════════

int32_t host_mpx_abi_version(wasm_exec_env_t exec_env)
{
	// Deliberately does NOT check was_cancelled(): a skill asking what it is
	// talking to should always get an answer.
	(void)exec_env;
	return MPX_ABI_VERSION;
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
//     ignores them - see servo_stage()'s note.
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

/* Resolve a WASM linear-memory offset to a native pointer, checking that the
 * whole span is inside the sandbox's memory. A skill must not be able to hand
 * us an offset that walks off the end of its own heap - robot_imu_read() does
 * this by hand without the bounds check, which is the pattern NOT to copy. */
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

/* ── The SDK frame and the AT32 frame, and the two things between them ────
 *
 * The SDK says a joint position is degrees with centre at 135 and positive in
 * the direction the gait calls positive. The driver boards say something else,
 * and robot.h spells out exactly what: the AT32 frame runs BACKWARDS from the
 * gait frame (at32_raw == 1024 - gait_raw), and its full travel is
 * range_position_deg, which the boards hold in their own flash and which is
 * 310 on a stock board, less if a range trim has taken some - not 270.
 *
 * Handing q_deg to driver_board_stage() untouched and reading it back as
 * raw * 270/1024 is two silent errors:
 *
 *   - no flip, so every joint a skill commanded moved the mirror image of what
 *     it asked for. A pose came out reflected; a backflip came out inverted.
 *   - 270 asserted against boards set to 310, so both the centre (135 vs the
 *     real 155) and the degrees-per-tick were wrong.
 *
 * The gait path has always done this conversion properly, in
 * driver_board_sync_write(). Converting here rather than inside
 * driver_board_stage() leaves that function's documented AT32-frame contract
 * alone - Servo Studio and the diagnostics still speak it directly - and puts
 * the translation at the boundary where the SDK frame is actually defined. */
constexpr float SDK_CENTRE_DEG = 135.0f;

inline float sdk_deg_to_at32_deg(float q_deg)
{
	const float centre = driver_board_get_scale_dd() / 20.0f;   /* deg */
	return centre - (q_deg - SDK_CENTRE_DEG);
}

/* The current CAP that goes out with a position, in mA.
 *
 * The boards take a position and a current limit; there is no speed field. The
 * SDK documents 0 as "just a position", but 0 that reaches the board does not
 * mean "unspecified" - it means zero milliamps, and a servo told to hold a
 * position with no current available does not move.
 *
 * That is what a backflip looks like when it runs perfectly and the robot
 * stands still: every frame arrives, on time, with no power behind it.
 *
 * So 0 or negative means "the cap the gait itself uses", and a positive value
 * is honoured as the deliberate limit it is. */
inline int16_t stage_current_ma(float tau_ma)
{
	if (tau_ma > 0.0f) return static_cast<int16_t>(tau_ma);
	return static_cast<int16_t>(robot::SERVO_CURRENT_MAX_MA);
}

inline float at32_raw_to_sdk_deg(uint16_t at32_raw)
{
	/* present_position() is already normalised to 0..1024 AT32 ticks. */
	const float gait = 1024.0f - static_cast<float>(at32_raw);
	return SDK_CENTRE_DEG
		 + (gait - 511.0f) * driver_board_get_scale_dd() / 10240.0f;
}

/* Parameters that define the joint's physical angle mapping rather than its
 * control response. A skill must never write these:
 *
 *   1 MIN_POSITION_ADC   2 MAX_POSITION_ADC   3 RANGE_POSITION_DEG
 *
 * They are the board's own calibration. Change one and every angle command
 * afterwards means something different - including the gait's, including the
 * NVS offsets calibrated against the old mapping - and servo_save_config()
 * burns the new value into the driver board's flash where a reboot will not
 * clear it.
 *
 * The two REVERSE_* slots are here for a stronger reason than the ADC ones:
 * they flip a direction, so a skill that writes one leaves a single joint
 * driving opposite to the other eleven. That is not a robot behaving oddly,
 * it is a robot tearing at its own legs, and it survives the skill that caused
 * it with nothing on screen to explain why.
 *
 * All of them are still settable from Servo Studio, where a human is watching
 * the joint move. */
inline bool param_is_read_only(int32_t param)
{
	return param == DB_PARAM_MIN_POSITION_ADC
		|| param == DB_PARAM_MAX_POSITION_ADC
		|| param == DB_PARAM_RANGE_POSITION_DEG
		|| param == DB_PARAM_REVERSE_MOTOR
		|| param == DB_PARAM_REVERSE_POSITION_SENSOR;
}

/* ── Undoing what a skill did to the gains ────────────────────────────────
 *
 * Gains live on the AT32 boards, not in the module, so they survive the skill
 * that wrote them - the bus lock being released, the run returning, even the
 * skill crashing. That is deliberate and useful: it is how a skill tunes the
 * motors and then lets the firmware's own gait walk with that tuning.
 *
 * It is also a trap. A skill that leaves Kp at 95 makes every built-in gait
 * afterwards walk slightly wrong with nothing on screen to explain why - the
 * same shape of bug the sandbox already closes for overlays and for the bus
 * lock.
 *
 * So remember what was there BEFORE the skill's first write to each slot and
 * put it back on the way out. Not "restore stock": that would throw away a
 * tuning a human set from Servo Studio and has every reason to expect to still
 * be theirs.
 *
 * TO KEEP A TUNING ON PURPOSE the escape hatch already exists:
 * servo_save_config() burns it into the driver board's own flash, which is
 * what "permanent" means here. Restoring the RAM value afterwards does not
 * touch that.
 *
 * Cost: one extra config read the first time a skill touches a given slot, paid
 * once and not per write. 12 x DB_PARAM_COUNT of bookkeeping, under a kilobyte,
 * in BSS. */
float s_gain_before[12][DB_PARAM_COUNT];
bool  s_gain_saved [12][DB_PARAM_COUNT];
/* Counted, not narrated. One line per unreadable slot meant 45 warnings for a
 * bench robot with one board plugged in - enough to bury the run's actual
 * result. The fact is worth reporting once, with a number. */
int   s_gain_unreadable;

void remember_gain(int id, int param)
{
	const int i = id - 1;
	if (i < 0 || i >= 12 || param < 0 || param >= DB_PARAM_COUNT) return;
	if (s_gain_saved[i][param]) return;          /* first write only */

	float before = 0.0f;
	if (!driver_board_get_param(id, param, &before)) {
		/* Could not read it, so we must not pretend we can restore it.
		 * Leaving `saved` false means the value is left alone at the end
		 * rather than being overwritten with a guess. */
		s_gain_unreadable++;
		ESP_LOGD(TAG, "could not read servo %d param %d before writing; it "
					  "will not be restored", id, param);
		return;
	}
	s_gain_before[i][param] = before;
	s_gain_saved [i][param] = true;
}

/* id 0 means "every board"; 1..12 means "the board that servo lives on".
 *
 * Computing `servo_id_ok(id) ? (id-1)/3 : -1` instead makes -1 mean ALL BOARDS,
 * so id 13, id -5 and id 9999 quietly mean "every board" too - and for
 * servo_restore_config that is a factory reset of all twelve servos'
 * calibration, returned as success. Every other servo_* entry point rejects a
 * bad id; these two must as well. */
inline bool config_scope_ok(int32_t id, int *board_out)
{
	if (id == 0) { *board_out = -1; return true; }            // all boards
	if (!servo_id_ok(id)) return false;                        // reject, do not widen
	*board_out = (static_cast<int>(id) - 1) / 3;
	return true;
}

}  // namespace

// ── Control arbitration ──────────────────────────────────────
//
// Four things can move a joint: the gait generator, the built-in IK, a skill's
// direct joint writes, and the servo bus. Without arbitration they all write
// the same goal buffer and "what happens if I call robot_ik_fr() while a gait
// is running" has an answer nobody can look up - last writer wins, at 15 ms
// granularity.
//
// This does not change that default. It adds a claim: a skill that calls
// mpx_control_take(MPX_CTRL_FEET) is telling the firmware it owns foot
// placement, and any *other* domain's write is then refused with MPX_ERR_BUSY
// instead of silently interleaving. A skill that never calls take() sees
// byte-identical pre-v3 behaviour.

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
// functions - so it could refuse the skill's own calls from another layer and
// nothing else. Only one skill runs at a time, so it protected the skill from
// itself and from no one, while the goal buffer was still overwritten
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

// ── Gain undo bookkeeping, called by the sandbox at run boundaries ──

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
				 s_gain_unreadable ? " - and some could not be read beforehand,"
									 " so they were left alone" : "");
		if (s_gain_unreadable) {
			ESP_LOGI(TAG, "  %d slot(s) unreadable - a board that is not "
						  "answering", s_gain_unreadable);
		}
	}
	s_gain_unreadable = 0;
	return restored;
}

// ── Bus ownership ────────────────────────────────────────────

int32_t host_servo_lock(wasm_exec_env_t exec_env)
{
	(void)exec_env;
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	return robot::servo_lock() ? MPX_OK : MPX_ERR_BUSY;
}

int32_t host_servo_unlock(wasm_exec_env_t exec_env)
{
	(void)exec_env;
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	robot::servo_unlock();
	return MPX_OK;
}

int32_t host_servo_is_locked(wasm_exec_env_t exec_env)
{
	// Answers "do *I* hold the bus", which is the only useful question for a
	// skill. Answering "is the bus held by anyone" lets a skill see 1 while
	// Servo Studio owns it, conclude it is safe to write, and be wrong.
	(void)exec_env;
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	return robot::servo_owned_by_skill() ? 1 : 0;
}

// ── Config path: gains and board flash ───────────────────────

int32_t host_servo_set_gain(wasm_exec_env_t exec_env,
							int32_t id, int32_t param, float value)
{
	(void)exec_env;
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	if (!servo_id_ok(id) || param < 0 || param >= DB_PARAM_COUNT) return MPX_ERR_ARG;
	if (param_is_read_only(param)) {
		ESP_LOGW(TAG, "servo_set_gain: param %" PRId32 " is calibration, not a "
					  "gain - read-only from a skill", param);
		return MPX_ERR_READONLY;
	}
	if (!robot::servo_owned_by_skill()) {
		ESP_LOGW(TAG, "servo_set_gain: bus not locked - call servo_lock() first");
		return MPX_ERR_NOT_LOCKED;
	}
	remember_gain(static_cast<int>(id), static_cast<int>(param));
	if (!driver_board_set_param(id, param, value)) return MPX_ERR_NO_REPLY;
	ESP_LOGD(TAG, "servo_set_gain: id=%" PRId32 " p=%" PRId32 " v=%.4f",
			 id, param, value);
	return MPX_OK;
}

int32_t host_servo_get_gain(wasm_exec_env_t exec_env,
							int32_t id, int32_t param, int32_t out_ptr)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	if (!servo_id_ok(id) || param < 0 || param >= DB_PARAM_COUNT) return MPX_ERR_ARG;
	if (!robot::servo_owned_by_skill()) return MPX_ERR_NOT_LOCKED;

	float *out = static_cast<float *>(wasm_ptr(exec_env, out_ptr, sizeof(float)));
	if (!out) return MPX_ERR_ARG;

	float v = 0.0f;
	if (!driver_board_get_param(id, param, &v)) return MPX_ERR_NO_REPLY;
	*out = v;
	return MPX_OK;
}

int32_t host_servo_save_config(wasm_exec_env_t exec_env, int32_t id)
{
	(void)exec_env;
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	int board;
	if (!config_scope_ok(id, &board)) return MPX_ERR_ARG;
	if (!robot::servo_owned_by_skill()) return MPX_ERR_NOT_LOCKED;
	return driver_board_save_config(board) ? MPX_OK : MPX_ERR_NO_REPLY;
}

int32_t host_servo_restore_config(wasm_exec_env_t exec_env, int32_t id)
{
	(void)exec_env;
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	int board;
	if (!config_scope_ok(id, &board)) return MPX_ERR_ARG;
	if (!robot::servo_owned_by_skill()) return MPX_ERR_NOT_LOCKED;
	return driver_board_factory_restore(board) ? MPX_OK : MPX_ERR_NO_REPLY;
}

// ── Command path: stage / commit ─────────────────────────────

int32_t host_servo_stage(wasm_exec_env_t exec_env,
						 int32_t id, float q_deg, float tau_ma,
						 float kp, float kd)
{
	(void)exec_env;
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	if (!servo_id_ok(id)) return MPX_ERR_ARG;
	if (!robot::servo_owned_by_skill()) return MPX_ERR_NOT_LOCKED;

	driver_board_stage(id, DB_MODE_POSITION, sdk_deg_to_at32_deg(q_deg),
					   stage_current_ma(tau_ma),
					   static_cast<uint16_t>(kp < 0 ? 0 : kp),
					   static_cast<uint16_t>(kd < 0 ? 0 : kd));
	return MPX_OK;
}

int32_t host_servo_commit(wasm_exec_env_t exec_env)
{
	(void)exec_env;
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	if (!robot::servo_owned_by_skill()) return MPX_ERR_NOT_LOCKED;
	return driver_board_commit() ? MPX_OK : MPX_ERR_NO_REPLY;
}

// servo_write_all: the closest thing to a Unitree LowCmd - an array of
// {q_deg, tau_ma, kp, kd} indexed by servo 1..12, staged and committed in one
// call, so a whole-robot update costs four SPI frames.
int32_t host_servo_write_all(wasm_exec_env_t exec_env,
							 int32_t cmd_ptr, int32_t count)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	if (count < 1 || count > 12) return MPX_ERR_ARG;
	if (!robot::servo_owned_by_skill()) return MPX_ERR_NOT_LOCKED;

	const uint32_t bytes = static_cast<uint32_t>(count) * 4u * sizeof(float);
	const float *cmd = static_cast<const float *>(wasm_ptr(exec_env, cmd_ptr, bytes));
	if (!cmd) return MPX_ERR_ARG;

	for (int32_t i = 0; i < count; ++i) {
		const float *c = cmd + i * 4;
		driver_board_stage(static_cast<int>(i) + 1, DB_MODE_POSITION,
						   sdk_deg_to_at32_deg(c[0]),
						   stage_current_ma(c[1]),
						   static_cast<uint16_t>(c[2] < 0 ? 0 : c[2]),
						   static_cast<uint16_t>(c[3] < 0 ? 0 : c[3]));
	}
	return driver_board_commit() ? MPX_OK : MPX_ERR_NO_REPLY;
}

// ── Feedback ─────────────────────────────────────────────────

// servo_read_all / servo_read: four floats per servo:
//   q_deg   present position, SDK degrees (centre 135, gait direction)
//   tau_ma  present motor current, mA (signed)
//   temp_c  NTC temperature, degC - NaN if that servo never answered
//   q_raw   the same position on the raw 0..1023 scale
//
// There is deliberately no velocity field: the boards do not measure one, and
// a hardcoded zero in a struct named "dq" is worse than its absence.
int32_t host_servo_read_all(wasm_exec_env_t exec_env, int32_t out_ptr)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;

	float *out = static_cast<float *>(
		wasm_ptr(exec_env, out_ptr, 12u * 4u * sizeof(float)));
	if (!out) return MPX_ERR_ARG;

	for (int id = 1; id <= 12; ++id) {
		const uint16_t raw = driver_board_present_position(id);
		const float    t   = driver_board_present_temperature(id);
		float *o = out + (id - 1) * 4;
		o[0] = at32_raw_to_sdk_deg(raw);
		o[1] = static_cast<float>(driver_board_present_current(id));
		o[2] = (t > DB_TEMP_INVALID) ? t : NAN;
		o[3] = static_cast<float>(raw);
	}
	return MPX_OK;
}

int32_t host_servo_read(wasm_exec_env_t exec_env, int32_t id, int32_t out_ptr)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	if (!servo_id_ok(id)) return MPX_ERR_ARG;

	float *out = static_cast<float *>(
		wasm_ptr(exec_env, out_ptr, 4u * sizeof(float)));
	if (!out) return MPX_ERR_ARG;

	const uint16_t raw = driver_board_present_position(id);
	const float    t   = driver_board_present_temperature(id);
	out[0] = at32_raw_to_sdk_deg(raw);
	out[1] = static_cast<float>(driver_board_present_current(id));
	out[2] = (t > DB_TEMP_INVALID) ? t : NAN;
	out[3] = static_cast<float>(raw);
	return MPX_OK;
}

// Refresh the feedback cache while the gait is parked. With the gait running
// the cache is already refreshed every tick and this is unnecessary.
int32_t host_servo_poll(wasm_exec_env_t exec_env)
{
	(void)exec_env;
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	if (!robot::servo_owned_by_skill()) return MPX_ERR_NOT_LOCKED;
	bool ok = true;
	for (int b = 0; b < 4; ++b) ok &= driver_board_poll_board(b);
	return ok ? MPX_OK : MPX_ERR_NO_REPLY;
}

// mode: 0 = idle (motor off), 1 = position hold, 2 = torque
int32_t host_servo_direct(wasm_exec_env_t exec_env,
						  int32_t id, int32_t mode, float q_deg, float tau_ma)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	if (!servo_id_ok(id)) return MPX_ERR_ARG;
	if (!robot::servo_owned_by_skill()) return MPX_ERR_NOT_LOCKED;

	const uint16_t m = (mode == 1) ? DB_MODE_POSITION
					 : (mode == 2) ? DB_MODE_TORQUE
								   : DB_MODE_IDLE;
	/* IDLE and TORQUE do not carry a position; only POSITION needs the frame
	 * conversion, and converting the other two would send a pose the caller
	 * never asked for. */
	const float at32 = (m == DB_MODE_POSITION) ? sdk_deg_to_at32_deg(q_deg) : q_deg;
	/* TORQUE mode means the caller IS commanding a current, so pass it through;
	 * only a position needs a default cap standing in for 0. */
	const int16_t ma = (m == DB_MODE_POSITION) ? stage_current_ma(tau_ma)
											   : static_cast<int16_t>(tau_ma);
	return driver_board_direct(id, m, at32, ma) ? MPX_OK : MPX_ERR_NO_REPLY;
}

// Bitmask of servos that answered a parameter read: bit 0 = servo 1.
int32_t host_servo_scan(wasm_exec_env_t exec_env)
{
	(void)exec_env;
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	if (!robot::servo_owned_by_skill()) return MPX_ERR_NOT_LOCKED;
	int32_t mask = 0;
	for (int id = 1; id <= 12; ++id) {
		float v;
		if (driver_board_get_param(id, DB_PARAM_KP_POSITION, &v)) mask |= (1 << (id - 1));
	}
	return mask;
}

// ── Clock ────────────────────────────────────────────────────
//
// Before v3 there was no time source at all: robot_delay_ms was the only
// timing primitive, so a skill could not measure a frame, hold a rate, or run
// for a wall-clock duration. Everything time-shaped had to be counted in
// frames and hoped for.

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

// ── Continuous drive ─────────────────────────────────────────
//
// robot::joy_input() is what the phone UI's thumbsticks call. It was never in
// the ABI, so a skill could pick one of 46 discrete gaits but could not ask for
// "forward at a third of speed while turning gently" - the one thing the
// hardware was already doing for the web client.

int32_t host_mpx_drive(wasm_exec_env_t exec_env, float fwd, float strafe, float turn)
{
	(void)exec_env;
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
	(void)exec_env;
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

// ── Foot placement ───────────────────────────────────────────
//
// One call instead of four differently-named ones, so a leg index can be a
// loop variable. Same maths as robot_ik_*; this is purely about the shape of
// the call site.
//
// Z CONVENTION, SO NOBODY HAS TO GUESS AGAIN: `z` arriving here is the
// FIRMWARE's - distance DOWN from the hip, POSITIVE, standing at about +70, the
// same number robot::calculate_ik() and the idle loop's
// front_right_ik(0, 0, s_cfg.height) use. The C SDK declares z as UP-positive
// and negates it before the call; do not negate again here.
//
// It was not always converted. A skill asking for the standing height as -70
// resolved to servo2 = servo3 = -180 (atan2(x, zd) flips by pi once zd goes
// negative), clamped at the joint limit, and moved the legs somewhere nobody
// asked for - silently, with no error and no warning. If this ever needs
// changing, change it in ONE of the two places, never both.
int32_t host_mpx_foot(wasm_exec_env_t exec_env, int32_t leg, float x, float th0, float z)
{
	(void)exec_env;
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
	(void)exec_env;
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	if (speed < 0 || speed > 2047) return MPX_ERR_ARG;
	robot::set_all_servo_speed(static_cast<uint16_t>(speed));
	return MPX_OK;
}

int32_t host_mpx_reset_offsets(wasm_exec_env_t exec_env)
{
	(void)exec_env;
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

float host_mpx_board_scale_dd(wasm_exec_env_t exec_env)
{
	/* No was_cancelled() check, and no ownership check: a skill asking how wide
	 * its joints are should always get an answer, exactly like
	 * mpx_abi_version().
	 *
	 * Returns 0 when no board answered the range query, which is the honest
	 * answer - the alternative is handing back the built-in 3100 default and
	 * letting a skill compute angles from a number nothing on the robot
	 * confirmed. */
	(void)exec_env;
	return driver_board_probe_answered() ? driver_board_get_scale_dd() : 0.0f;
}

// ── Skill parameters ─────────────────────────────────────────
//
// on_start() takes no arguments, so "the same wave, but three times and faster"
// meant an edit and a recompile, and the web UI had no knob to offer.
// Parameters are supplied per run and read here by name, with a fallback the
// skill chooses - so a skill run with no parameters at all still behaves
// exactly as it was written.

float host_mpx_param_f(wasm_exec_env_t exec_env, int32_t name_ptr, float fallback)
{
	(void)exec_env;
	if (name_ptr == 0) return fallback;
	// "$" in the signature means WAMR has already bounds-checked and converted
	// the pointer, so this is a native pointer into the sandbox's memory.
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

// ── v4: overlay, tick, trace ─────────────────────────────────

int32_t host_mpx_overlay(wasm_exec_env_t exec_env, int32_t id, float deg)
{
	(void)exec_env;
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	if (id < 1 || id > 12) return MPX_ERR_ARG;

	// Deliberately NOT gated on control arbitration. The point of an overlay is
	// to ride on top of something else that owns the joints - usually the gait
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

	const char *name = reinterpret_cast<const char *>(
		static_cast<uintptr_t>(name_ptr));
	if (!name) return MPX_ERR_ARG;

	util::trace_ring_put(name, value, wasm::skill_millis());
	return MPX_OK;
}

// ═══════════════════════════════════════════════════════════════
//  ABI v6 — display, files, music, robot state
//
//  Declared in wasm_host_functions.h; each body calls the same cap_display /
//  fs / cap_music / robot entry point the Lua VM (lua/lua_bindings.cc,
//  lua/lua_display.cc) and the local agent's tool table (agent/robot_tools.cc)
//  call, so the three surfaces cannot drift apart.
// ═══════════════════════════════════════════════════════════════

namespace {

/* esp_err_t -> the one error convention. ESP_ERR_INVALID_STATE is "this robot
 * has no screen / no speaker", which a skill sees as MPX_ERR_STATE -- the same
 * condition Lua's display module raises "this robot has no screen" for. */
int32_t from_esp_err(esp_err_t err)
{
	switch (err) {
	case ESP_OK:              return MPX_OK;
	case ESP_ERR_INVALID_ARG: return MPX_ERR_ARG;
	case ESP_ERR_NOT_FOUND:   return MPX_ERR_ARG;
	default:                  return MPX_ERR_STATE;
	}
}

/* A '$' in the signature hands us a native pointer WAMR has already
 * bounds-checked; the older host functions carry it as an int32_t. */
const char *guest_str(int32_t ptr)
{
	if (ptr == 0) return nullptr;
	return reinterpret_cast<const char *>(static_cast<uintptr_t>(ptr));
}

/* Copy a string into a guest buffer. Returns the length copied (a terminating
 * NUL is written when there is room; it is not counted), or MPX_ERR_ARG when
 * the buffer is too small. Every "return a string" host call uses this, so the
 * capacity rule is stated once. */
int32_t return_string(wasm_exec_env_t exec_env, int32_t buf_ptr, int32_t cap,
					  const char *text)
{
	if (!text) return MPX_ERR_ARG;
	const int32_t need = static_cast<int32_t>(std::strlen(text));
	if (buf_ptr == 0 || cap < need) return MPX_ERR_ARG;
	char *out = static_cast<char *>(
		wasm_ptr(exec_env, buf_ptr, static_cast<uint32_t>(cap)));
	if (!out) return MPX_ERR_ARG;
	std::memcpy(out, text, static_cast<std::size_t>(need));
	if (cap > need) out[need] = '\0';
	return need;
}

}  // namespace

// ── display ──────────────────────────────────────────────────

int32_t host_display_width(wasm_exec_env_t exec_env)
{
	(void)exec_env;
	return cap_display_draw_width();
}

int32_t host_display_height(wasm_exec_env_t exec_env)
{
	(void)exec_env;
	return cap_display_draw_height();
}

int32_t host_display_rgb(wasm_exec_env_t exec_env,
						 int32_t r, int32_t g, int32_t b)
{
	(void)exec_env;
	return ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
}

int32_t host_display_clear(wasm_exec_env_t exec_env, int32_t color)
{
	(void)exec_env;
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	return from_esp_err(cap_display_draw_fill(static_cast<uint32_t>(color) & 0xFFFFFFu));
}

int32_t host_display_pixel(wasm_exec_env_t exec_env,
						   int32_t x, int32_t y, int32_t color)
{
	(void)exec_env;
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	return from_esp_err(cap_display_draw_pixel(x, y,
							static_cast<uint32_t>(color) & 0xFFFFFFu));
}

int32_t host_display_rect(wasm_exec_env_t exec_env, int32_t x, int32_t y,
						  int32_t w, int32_t h, int32_t color,
						  int32_t radius, int32_t border)
{
	(void)exec_env;
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	return from_esp_err(cap_display_draw_rect(
		x, y, w, h, static_cast<uint32_t>(color) & 0xFFFFFFu, radius, border));
}

int32_t host_display_circle(wasm_exec_env_t exec_env, int32_t cx, int32_t cy,
							int32_t r, int32_t color, int32_t border)
{
	(void)exec_env;
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	return from_esp_err(cap_display_draw_circle(
		cx, cy, r, static_cast<uint32_t>(color) & 0xFFFFFFu, border));
}

int32_t host_display_line(wasm_exec_env_t exec_env, int32_t x1, int32_t y1,
						  int32_t x2, int32_t y2, int32_t color, int32_t width)
{
	(void)exec_env;
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	return from_esp_err(cap_display_draw_line(
		x1, y1, x2, y2, static_cast<uint32_t>(color) & 0xFFFFFFu, width));
}

int32_t host_display_arc(wasm_exec_env_t exec_env, int32_t cx, int32_t cy,
						 int32_t r, int32_t start_deg, int32_t end_deg,
						 int32_t color, int32_t width)
{
	(void)exec_env;
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	return from_esp_err(cap_display_draw_arc(
		cx, cy, r, start_deg, end_deg,
		static_cast<uint32_t>(color) & 0xFFFFFFu, width));
}

int32_t host_display_text(wasm_exec_env_t exec_env, int32_t x, int32_t y,
						  int32_t text_ptr, int32_t color, int32_t size,
						  int32_t align, int32_t max_w)
{
	(void)exec_env;
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	const char *text = guest_str(text_ptr);
	if (!text) return MPX_ERR_ARG;
	return from_esp_err(cap_display_draw_text(
		x, y, text, static_cast<uint32_t>(color) & 0xFFFFFFu,
		size, align, max_w));
}

int32_t host_display_blit(wasm_exec_env_t exec_env, int32_t x, int32_t y,
						  int32_t w, int32_t h, int32_t data_ptr, int32_t len)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	/* The canvas is 320x240; anything this large is a guest mistake, and the
	 * bounds check below must not be handed a size that overflows int32. */
	if (w < 1 || h < 1 || w > 4096 || h > 4096) return MPX_ERR_ARG;
	const int32_t need = w * h * 2;
	if (len < need) return MPX_ERR_ARG;
	const void *data = wasm_ptr(exec_env, data_ptr, static_cast<uint32_t>(need));
	if (!data) return MPX_ERR_ARG;
	return from_esp_err(cap_display_draw_blit(
		x, y, w, h, static_cast<const uint8_t *>(data),
		static_cast<std::size_t>(need)));
}

int32_t host_display_show(wasm_exec_env_t exec_env, int32_t hold_ms)
{
	(void)exec_env;
	return from_esp_err(cap_display_draw_show(
		hold_ms < 0 ? 0u : static_cast<uint32_t>(hold_ms)));
}

int32_t host_display_face(wasm_exec_env_t exec_env)
{
	(void)exec_env;
	return from_esp_err(cap_display_clear());
}

int32_t host_display_show_text(wasm_exec_env_t exec_env, int32_t text_ptr)
{
	(void)exec_env;
	const char *text = guest_str(text_ptr);
	if (!text) return MPX_ERR_ARG;
	return from_esp_err(cap_display_show_text(text));
}

int32_t host_display_show_emotion(wasm_exec_env_t exec_env, int32_t name_ptr)
{
	(void)exec_env;
	const char *name = guest_str(name_ptr);
	if (!name) return MPX_ERR_ARG;
	return from_esp_err(cap_display_show_emotion(name));
}

int32_t host_display_set_colors(wasm_exec_env_t exec_env,
								int32_t background_rgb, int32_t eyes_rgb)
{
	(void)exec_env;
	return from_esp_err(cap_display_set_colors(background_rgb, eyes_rgb));
}

int32_t host_display_set_brightness(wasm_exec_env_t exec_env, int32_t percent)
{
	(void)exec_env;
	if (percent < 0) percent = 0;
	if (percent > 100) percent = 100;
	return from_esp_err(cap_display_set_brightness(static_cast<int>(percent)));
}

// ── fs ───────────────────────────────────────────────────────

int32_t host_fs_read(wasm_exec_env_t exec_env, int32_t path_ptr,
					 int32_t buf_ptr, int32_t cap)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	const char *path = guest_str(path_ptr);
	if (!path || !*path || cap < 0) return MPX_ERR_ARG;

	/* read_file() returns an empty vector for a missing file AND for an empty
	 * one, so ask existence first -- a skill reading a file it just truncated
	 * to zero must not be told "no such file". */
	if (!fs::file_exists(path)) return MPX_ERR_ARG;
	const std::vector<uint8_t> data = fs::read_file(path);
	const int32_t size = static_cast<int32_t>(data.size());
	if (size > 0 && cap > 0) {
		const int32_t n = size < cap ? size : cap;
		void *buf = wasm_ptr(exec_env, buf_ptr, static_cast<uint32_t>(n));
		if (!buf) return MPX_ERR_ARG;
		std::memcpy(buf, data.data(), static_cast<std::size_t>(n));
	}
	/* The whole size, not what fit: a return greater than cap says the file
	 * was truncated, so the caller can retry with a bigger buffer. */
	return size;
}

int32_t host_fs_write(wasm_exec_env_t exec_env, int32_t path_ptr,
					  int32_t data_ptr, int32_t len)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	const char *path = guest_str(path_ptr);
	if (!path || !*path || len < 0) return MPX_ERR_ARG;

	static const uint8_t kEmpty = 0;
	const void *data = &kEmpty;
	if (len > 0) {
		data = wasm_ptr(exec_env, data_ptr, static_cast<uint32_t>(len));
		if (!data) return MPX_ERR_ARG;
	}

	/* The same permission prompt Lua's fs.write raises, and the wait is
	 * reported to the watchdog so the user deciding does not look like the
	 * skill running. */
	char desc[256];
	std::snprintf(desc, sizeof(desc), "Write file '%s' (%" PRId32 " bytes)",
				  path, len);
	const int64_t t0 = esp_timer_get_time();
	const bool ok = network::request_permission("file_write", desc, 60000);
	wasm::extend_watchdog(static_cast<uint32_t>((esp_timer_get_time() - t0) / 1000));
	if (!ok) {
		ESP_LOGW(TAG, "fs_write: user denied write to '%s'", path);
		return MPX_ERR_ARG;
	}
	return fs::write_file(path, data, static_cast<std::size_t>(len)) ? MPX_OK
																	: MPX_ERR_ARG;
}

int32_t host_fs_delete(wasm_exec_env_t exec_env, int32_t path_ptr)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	const char *path = guest_str(path_ptr);
	if (!path || !*path) return MPX_ERR_ARG;

	char desc[256];
	std::snprintf(desc, sizeof(desc), "Delete file '%s'", path);
	const int64_t t0 = esp_timer_get_time();
	const bool ok = network::request_permission("file_delete", desc, 60000);
	wasm::extend_watchdog(static_cast<uint32_t>((esp_timer_get_time() - t0) / 1000));
	if (!ok) {
		ESP_LOGW(TAG, "fs_delete: user denied delete of '%s'", path);
		return MPX_ERR_ARG;
	}
	return fs::delete_file(path) ? MPX_OK : MPX_ERR_ARG;
}

int32_t host_fs_exists(wasm_exec_env_t exec_env, int32_t path_ptr)
{
	(void)exec_env;
	const char *path = guest_str(path_ptr);
	if (!path || !*path) return 0;
	return fs::file_exists(path) ? 1 : 0;
}

int32_t host_fs_list(wasm_exec_env_t exec_env, int32_t dir_ptr,
					 int32_t buf_ptr, int32_t cap)
{
	if (wasm::was_cancelled()) return MPX_ERR_CANCELLED;
	const char *dir_path = guest_str(dir_ptr);
	if (!dir_path || !*dir_path) dir_path = "/";
	if (buf_ptr == 0 || cap <= 0) return MPX_ERR_ARG;

	/* Lua's fs.list builds the VFS path by hand and reads the directory
	 * directly (fs::list_files() drops subdirectories), so a skill sees the
	 * same entries the Lua module and the web file browser do. */
	char vfs_path[256];
	if (dir_path[0] != '/') {
		std::snprintf(vfs_path, sizeof(vfs_path), "/fs/%s", dir_path);
	} else if (std::strncmp(dir_path, "/fs/", 4) != 0) {
		std::snprintf(vfs_path, sizeof(vfs_path), "/fs%s", dir_path);
	} else {
		std::strncpy(vfs_path, dir_path, sizeof(vfs_path) - 1);
		vfs_path[sizeof(vfs_path) - 1] = '\0';
	}

	DIR *dir = opendir(vfs_path);
	if (!dir) return MPX_ERR_ARG;

	char *out = static_cast<char *>(
		wasm_ptr(exec_env, buf_ptr, static_cast<uint32_t>(cap)));
	int32_t used = 0;
	if (out) {
		struct dirent *entry;
		while ((entry = readdir(dir)) != nullptr) {
			const char *name = entry->d_name;
			if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) {
				continue;
			}
			const bool is_dir = (entry->d_type == DT_DIR);
			char full_path[320];
			std::snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, name);
			const std::size_t sz = is_dir ? 0 : fs::file_size(full_path);

			char line[400];
			const int n = std::snprintf(line, sizeof(line), "%s\t%lu\t%c\n",
										name, static_cast<unsigned long>(sz),
										is_dir ? 'd' : 'f');
			if (n <= 0) continue;
			if (used + n > cap) {
				closedir(dir);
				return MPX_ERR_ARG;
			}
			std::memcpy(out + used, line, static_cast<std::size_t>(n));
			used += n;
		}
		if (used < cap) out[used] = '\0';
	}
	closedir(dir);
	return out ? used : MPX_ERR_ARG;
}

int32_t host_fs_info(wasm_exec_env_t exec_env, int32_t out_ptr)
{
	std::size_t total = 0, used = 0;
	if (!fs::stats(total, used)) return MPX_ERR_ARG;
	int32_t *out = static_cast<int32_t *>(
		wasm_ptr(exec_env, out_ptr, 2u * sizeof(int32_t)));
	if (!out) return MPX_ERR_ARG;
	out[0] = static_cast<int32_t>(total);
	out[1] = static_cast<int32_t>(used);
	return MPX_OK;
}

// ── music ────────────────────────────────────────────────────

int32_t host_music_play(wasm_exec_env_t exec_env, int32_t what_ptr,
						int32_t title_ptr, int32_t cap)
{
#if CONFIG_APP_VOICE_ENABLE
	(void)exec_env;
	const char *what = guest_str(what_ptr);
	if (!what || !*what) return MPX_ERR_ARG;

	char title[64] = {0};
	const esp_err_t err = cap_music_play(what, title, sizeof(title));
	if (err != ESP_OK) return from_esp_err(err);

	if (title_ptr != 0 && cap > 0) {
		char *out = static_cast<char *>(
			wasm_ptr(exec_env, title_ptr, static_cast<uint32_t>(cap)));
		if (out) {
			std::strncpy(out, title, static_cast<std::size_t>(cap) - 1);
			out[cap - 1] = '\0';
		}
	}
	return MPX_OK;
#else
	/* A build with no voice stack: the symbol is registered so the ABI does
	 * not change shape, and answers the same "no speaker" the agent tool's
	 * esp_err_to_name(ESP_ERR_NOT_SUPPORTED) path reports. */
	(void)exec_env; (void)what_ptr; (void)title_ptr; (void)cap;
	return MPX_ERR_STATE;
#endif
}

int32_t host_music_stop(wasm_exec_env_t exec_env)
{
	(void)exec_env;
#if CONFIG_APP_VOICE_ENABLE
	cap_music_stop();
	return MPX_OK;
#else
	return MPX_ERR_STATE;
#endif
}

// ── robot state ──────────────────────────────────────────────

int32_t host_robot_get_config(wasm_exec_env_t exec_env, int32_t out_ptr)
{
	(void)exec_env;
	int32_t *out = static_cast<int32_t *>(
		wasm_ptr(exec_env, out_ptr, 6u * sizeof(int32_t)));
	if (!out) return MPX_ERR_ARG;

	const robot::Config cfg = robot::get_config();
	out[0] = cfg.period;
	out[1] = cfg.height;
	out[2] = cfg.up_height;
	out[3] = cfg.stride;
	out[4] = cfg.tilt;
	out[5] = cfg.sg_speed;
	return MPX_OK;
}

int32_t host_robot_get_gait(wasm_exec_env_t exec_env, int32_t buf_ptr,
							int32_t cap)
{
	const char *name = robot::gait_to_name(robot::current_gait_cmd());
	return return_string(exec_env, buf_ptr, cap, name ? name : "none");
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
