#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
#include "lua.h"
#ifdef __cplusplus
}
#endif

/**
 * @brief Register all robot bindings into the given Lua state.
 *        Creates the `robot` global table with all sub-modules.
 *
 * Called automatically by lua_init(). Safe to call multiple times
 * on different states (idempotent per state).
 *
 * Registered functions:
 *
 *   robot.gait(name)           — Start gait by string name
 *   robot.get_mode()           → Current gait as string
 *   robot.get_config()         → {period, height, up_height, stride, tilt}
 *   robot.set_config(p, h, u, s, t) — Set gait parameters
 *
 *   robot.set_servo_angle(id, deg) — Set servo angle (degrees)
 *   robot.set_servo_speed(id, speed) — Stored, NOT sent (no speed field on the
 *                                   driver boards; kept for compatibility)
 *   robot.set_all_servo_speed(speed) — Same, for all 12
 *   robot.set_servo_current(id, mA) — Current limit: the real torque control
 *   robot.get_servo_current(id) → mA
 *   robot.flush()              — Commit servo positions
 *
 *   robot.read_position(id)    → raw 0-1023, AT32 frame
 *   robot.read_angle_cdeg(id)  → centidegrees from centre, SAME frame as
 *                                set_servo_angle() - use this for a control loop
 *   robot.read_speed(id)       → always -1 (not reported by this hardware)
 *   robot.read_load(id)        → signed load (present motor current)
 *   robot.read_voltage(id)     → always -1
 *   robot.read_temperature(id) → °C, from the servo's NTC
 *   robot.read_moving(id)      → 0/1
 *   robot.read_current(id)     → mA
 *   robot.ping(id)             → 1 if the channel answers, else -1
 *
 *   robot.set_offset(id, deg)  — Calibration offset (degrees)
 *   robot.get_offset(id)       → offset in degrees
 *   robot.reset_offsets()      — Zero all calibration offsets
 *
 *   robot.delay_ms(ms)         — Blocking delay with cancellation
 *
 *   robot.ik_fr(x, th0, z)     — Front-right leg IK (no flush)
 *   robot.ik_fl(x, th0, z)     — Front-left  leg IK
 *   robot.ik_rr(x, th0, z)     — Rear-right  leg IK
 *
 *   crypto.base64_decode(str)  → Decode standard base64 to raw bytes
 *   crypto.base64_encode(str)  → Encode raw bytes as base64 string
 *   robot.ik_rl(x, th0, z)     — Rear-left   leg IK
 *
 *   robot.imu_read()           → {ax, ay, az, gx, gy, gz, mx, my, mz, mag_valid}
 *   robot.imu_print()          — Log IMU to console
 *
 * @param L  Lua state to register into
 */
void lua_register_robot_bindings(lua_State *L);

/**
 * @brief Register the `wasm` module into the given Lua state.
 *
 * Creates global `wasm` table with:
 *   wasm.run(path, func_name?) → bool  — Run .wasm from LittleFS
 *   wasm.run_bytes(data, func_name?) → bool — Run from a string buffer
 *
 * @param L  Lua state to register into
 */
void lua_register_wasm_bindings(lua_State *L);

/**
 * @brief Register the `fs` module into the given Lua state.
 *
 * Creates global `fs` table with:
 *   fs.read(path)          → string or nil
 *   fs.write(path, content) → bool (requests user permission)
 *   fs.delete(path)        → bool (requests user permission)
 *   fs.exists(path)        → bool
 *   fs.list(dir)           → table of {name, size, is_dir}
 *   fs.info()              → {total, used} bytes
 *
 * Write and delete operations broadcast an action telemetry to
 * the PWA and block waiting for explicit user approval.
 *
 * @param L  Lua state to register into
 */
void lua_register_fs_bindings(lua_State *L);
void lua_register_crypto_bindings(lua_State *L);

/**
 * @brief Register the `display` module (lua_display.cc): draw on the screen.
 *
 * Paint a canvas, then show it; the face comes back after the hold.
 * Colours are 0xRRGGBB or a string ("red", "#ff8800"). Coordinates are
 * pixels from the top left (320x240 on the MP4), clipped at the edges.
 *
 *   display.width(), display.height()       → canvas size (0: no screen)
 *   display.rgb(r, g, b)                    → 0xRRGGBB
 *   display.clear([color])                  — fill (default black)
 *   display.pixel(x, y, color)
 *   display.rect(x, y, w, h, color [, {radius=, outline=}])  radius -1 = pill
 *   display.circle(cx, cy, r, color [, {outline=}])
 *   display.line(x1, y1, x2, y2, color [, width=2])
 *   display.arc(cx, cy, r, start, end, color [, width=4])  0° = 3 o'clock, clockwise
 *   display.text(x, y, str, color [, {size=14|28|48, align="left|center|right", w=}])
 *   display.blit(x, y, w, h, data)          — RGB565 little-endian bytes
 *   display.show([hold_ms=15000])           — 0 = until replaced
 *   display.face()                          — back to the eyes now
 *
 * Errors (no screen, bad size) are raised as Lua errors.
 */
void lua_register_display_bindings(lua_State *L);

/**
 * @brief Register the `tools` module (lua_tools.cc): the agent's robot tools.
 *
 *   tools.call(name [, args]) → result | nil, error
 *       Same names and arguments as the agent's tools (robot_move,
 *       display_draw, mpx_run_skill, ...). result is the reply as a table.
 *   tools.list()              → { tool names }
 */
void lua_register_tools_bindings(lua_State *L);
