/*
 * SPDX-FileCopyrightText: 2026 MPX-Dog Project Contributors
 * SPDX-License-Identifier: MIT
 *
 * Lua C bindings for the MPX-Dog robot HAL.
 *
 * Registered as the `robot` global table with sub-functions for:
 *   gait control, configuration, servo feedback, calibration,
 *   inverse kinematics, IMU, and utility.
 *
 * These bindings mirror the WASM host functions in
 * main/sdk/wasm_host_functions.cc but provide a more natural
 * Lua interface (floats instead of centidegrees, tables for
 * structured data, etc.).
 */

#include "lua_bindings.h"

#include <string.h>
#include <stdio.h>
#include <dirent.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

#include "fs/littlefs_manager.h"
#include "robot/robot.h"
#include "wasm/wasm_sandbox.h"
#include "wasm/wasm_decrypt.h"
#include "network/chat_ws.h"

static const char *TAG = "lua_bind";

/* ── Helpers ──────────────────────────────────────────────────────────── */

/** Check that a servo ID is in valid range [1, 12]; returns false + Lua error if not. */
static bool check_servo_id(lua_State *L, int arg, int *out_id)
{
    int id = (int)luaL_checkinteger(L, arg);
    if (id < 1 || id > 12) {
        lua_pushfstring(L, "invalid servo id %d (must be 1-12)", id);
        return lua_error(L), false;
    }
    *out_id = id;
    return true;
}

/* ── Gait control ─────────────────────────────────────────────────────── */

/**
 * robot.gait(name) — Start a gait by string name.
 *
 * Names: "none", "init", "step", "advance", "back", "left", "right",
 * "turnL", "turnR", "jump", "jumpfwd", "twerk",
 * "lookup", "lookdown", "lookleft", "lookright",
 * "flegL", "flegR", "blegL", "blegR",
 * "heightup", "heightdown", "balance",
 * "bowback", "bodycycle", "headellipse",
 * "moveLF", "moveRF", "moveLB", "moveRB",
 * "testspeed", "roll", "pitch", "stretch",
 * "lookul", "lookur", "lookll", "looklr",
 * "stanford" (Stanford Pupper trot walk),
 * "frontkick", "wiggle", "buttshrug",
 * "wiggleL", "wiggleR", "buttshrugL", "buttshrugR" (FPC choreography)
 */
static int l_robot_gait(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);

    robot::GaitCmd cmd = robot::GaitCmd::None;

    if      (strcmp(name, "none")      == 0) cmd = robot::GaitCmd::None;
    else if (strcmp(name, "init")      == 0) cmd = robot::GaitCmd::Init;
    else if (strcmp(name, "step")      == 0) cmd = robot::GaitCmd::Step;
    else if (strcmp(name, "roll")      == 0) cmd = robot::GaitCmd::Roll;
    else if (strcmp(name, "pitch")     == 0) cmd = robot::GaitCmd::Pitch;
    else if (strcmp(name, "stretch")   == 0) cmd = robot::GaitCmd::Stretch;
    else if (strcmp(name, "advance")   == 0) cmd = robot::GaitCmd::Advance;
    else if (strcmp(name, "back")      == 0) cmd = robot::GaitCmd::Back;
    else if (strcmp(name, "left")      == 0) cmd = robot::GaitCmd::Left;
    else if (strcmp(name, "right")     == 0) cmd = robot::GaitCmd::Right;
    else if (strcmp(name, "turnL")     == 0) cmd = robot::GaitCmd::TurnL;
    else if (strcmp(name, "turnR")     == 0) cmd = robot::GaitCmd::TurnR;
    else if (strcmp(name, "twerk")     == 0) cmd = robot::GaitCmd::Twerk;
    else if (strcmp(name, "jump")      == 0) cmd = robot::GaitCmd::Jump;
    else if (strcmp(name, "jumpfwd")   == 0) cmd = robot::GaitCmd::JumpFwd;
    else if (strcmp(name, "testspeed") == 0) cmd = robot::GaitCmd::TestSpeed;
    else if (strcmp(name, "lookup")    == 0) cmd = robot::GaitCmd::LookUp;
    else if (strcmp(name, "lookdown")  == 0) cmd = robot::GaitCmd::LookDown;
    else if (strcmp(name, "lookleft")  == 0) cmd = robot::GaitCmd::LookLeft;
    else if (strcmp(name, "lookright") == 0) cmd = robot::GaitCmd::LookRight;
    else if (strcmp(name, "lookul")    == 0) cmd = robot::GaitCmd::LookUpperLeft;
    else if (strcmp(name, "lookur")    == 0) cmd = robot::GaitCmd::LookUpperRight;
    else if (strcmp(name, "lookll")    == 0) cmd = robot::GaitCmd::LookLowerLeft;
    else if (strcmp(name, "looklr")    == 0) cmd = robot::GaitCmd::LookLowerRight;
    else if (strcmp(name, "flegL")     == 0) cmd = robot::GaitCmd::ForelegLiftL;
    else if (strcmp(name, "flegR")     == 0) cmd = robot::GaitCmd::ForelegLiftR;
    else if (strcmp(name, "blegL")     == 0) cmd = robot::GaitCmd::BacklegLiftL;
    else if (strcmp(name, "blegR")     == 0) cmd = robot::GaitCmd::BacklegLiftR;
    else if (strcmp(name, "heightup")  == 0) cmd = robot::GaitCmd::HeightUp;
    else if (strcmp(name, "heightdown")== 0) cmd = robot::GaitCmd::HeightDown;
    else if (strcmp(name, "balance")   == 0) cmd = robot::GaitCmd::Balance;
    else if (strcmp(name, "bowback")   == 0) cmd = robot::GaitCmd::BowBack;
    else if (strcmp(name, "bodycycle") == 0) cmd = robot::GaitCmd::BodyCycle;
    else if (strcmp(name, "headellipse")==0) cmd = robot::GaitCmd::HeadEllipse;
    else if (strcmp(name, "moveLF")    == 0) cmd = robot::GaitCmd::MoveLeftFront;
    else if (strcmp(name, "moveRF")    == 0) cmd = robot::GaitCmd::MoveRightFront;
    else if (strcmp(name, "moveLB")    == 0) cmd = robot::GaitCmd::MoveLeftBack;
    else if (strcmp(name, "moveRB")    == 0) cmd = robot::GaitCmd::MoveRightBack;
    else if (strcmp(name, "stanford")  == 0) cmd = robot::GaitCmd::StanfordWalk;
    else if (strcmp(name, "frontkick") == 0) cmd = robot::GaitCmd::FrontKick;
    else if (strcmp(name, "wiggle")    == 0) cmd = robot::GaitCmd::Wiggle;
    else if (strcmp(name, "buttshrug") == 0) cmd = robot::GaitCmd::ButtShrug;
    else if (strcmp(name, "wiggleL")   == 0) cmd = robot::GaitCmd::WiggleLeft;
    else if (strcmp(name, "wiggleR")   == 0) cmd = robot::GaitCmd::WiggleRight;
    else if (strcmp(name, "buttshrugL")== 0) cmd = robot::GaitCmd::ButtShrugLeft;
    else if (strcmp(name, "buttshrugR")== 0) cmd = robot::GaitCmd::ButtShrugRight;
    else {
        lua_pushfstring(L, "unknown gait name '%s'", name);
        return lua_error(L);
    }

    robot::send_gait_cmd(cmd);
    ESP_LOGI(TAG, "gait: %s", name);
    return 0;
}

/**
 * robot.get_mode() → string — Get the current gait command name.
 */
static int l_robot_get_mode(lua_State *L)
{
    /* One table, in robot.cc (gait_to_name): this used to be a 46-case
     * switch that had to be edited every time a move was added. */
    const char *name = robot::gait_to_name(robot::current_gait_cmd());
    lua_pushstring(L, name ? name : "none");
    return 1;
}


/* ── Configuration ────────────────────────────────────────────────────── */

/**
 * robot.set_config(period, height, up_height, stride, tilt, sg_speed)
 *   — Set gait parameters (integers).  sg_speed is the Stanford walk /
 *     diagonal speed in mm/s (10..200).
 *
 * All parameters are optional — pass nil to keep current value.
 */
static int l_robot_set_config(lua_State *L)
{
    robot::Config cfg = robot::get_config();

    int n = lua_gettop(L);
    if (n >= 1 && !lua_isnil(L, 1)) cfg.period    = (int)luaL_checkinteger(L, 1);
    if (n >= 2 && !lua_isnil(L, 2)) cfg.height    = (int)luaL_checkinteger(L, 2);
    if (n >= 3 && !lua_isnil(L, 3)) cfg.up_height = (int)luaL_checkinteger(L, 3);
    if (n >= 4 && !lua_isnil(L, 4)) cfg.stride    = (int)luaL_checkinteger(L, 4);
    if (n >= 5 && !lua_isnil(L, 5)) cfg.tilt      = (int)luaL_checkinteger(L, 5);
    if (n >= 6 && !lua_isnil(L, 6)) cfg.sg_speed  = (int)luaL_checkinteger(L, 6);

    robot::set_config(cfg);
    ESP_LOGI(TAG, "set_config: p=%d h=%d uh=%d s=%d t=%d sg=%d",
             cfg.period, cfg.height, cfg.up_height, cfg.stride, cfg.tilt,
             cfg.sg_speed);
    return 0;
}

/**
 * robot.get_config() → {period, height, up_height, stride, tilt}
 *   — Get current gait parameters as a table.
 */
static int l_robot_get_config(lua_State *L)
{
    robot::Config cfg = robot::get_config();

    lua_createtable(L, 0, 6);
    lua_pushinteger(L, cfg.period);    lua_setfield(L, -2, "period");
    lua_pushinteger(L, cfg.height);    lua_setfield(L, -2, "height");
    lua_pushinteger(L, cfg.up_height); lua_setfield(L, -2, "up_height");
    lua_pushinteger(L, cfg.stride);    lua_setfield(L, -2, "stride");
    lua_pushinteger(L, cfg.tilt);      lua_setfield(L, -2, "tilt");
    lua_pushinteger(L, cfg.sg_speed);  lua_setfield(L, -2, "sg_speed");
    return 1;
}

/* ── Low-level servo control ──────────────────────────────────────────── */

/**
 * robot.set_servo_angle(id, deg) — Set servo angle in degrees.
 */
static int l_robot_set_servo_angle(lua_State *L)
{
    int id;
    if (!check_servo_id(L, 1, &id)) return 0;
    float deg = (float)luaL_checknumber(L, 2);
    robot::set_servo_angle(id, deg);
    ESP_LOGV(TAG, "set_servo_angle: id=%d deg=%.1f", id, deg);
    return 0;
}

/**
 * robot.set_servo_speed(id, speed) — Set servo speed (0 = max, larger = slower).
 */
static int l_robot_set_servo_speed(lua_State *L)
{
    int id;
    if (!check_servo_id(L, 1, &id)) return 0;
    uint16_t speed = (uint16_t)luaL_checkinteger(L, 2);
    robot::set_servo_speed(id, speed);
    ESP_LOGV(TAG, "set_servo_speed: id=%d speed=%u", id, speed);
    return 0;
}

/**
 * robot.set_all_servo_speed(speed) — Set speed for all 12 servos.
 */
static int l_robot_set_all_servo_speed(lua_State *L)
{
    uint16_t speed = (uint16_t)luaL_checkinteger(L, 1);
    robot::set_all_servo_speed(speed);
    ESP_LOGV(TAG, "set_all_servo_speed: speed=%u", speed);
    return 0;
}

/**
 * robot.set_servo_current(id, mA) — Set the per-servo current limit.
 *
 * This is the torque control on the MP4-ESP32-CORE hardware: a ceiling on the
 * current the servo's driver board may source while holding position. Clamped
 * to 0..1500 mA and persisted across reboots.
 *
 * Contrast robot.set_servo_speed(), which is stored and never sent - the
 * driver boards have no speed field for it to travel in.
 */
static int l_robot_set_servo_current(lua_State *L)
{
    int id;
    if (!check_servo_id(L, 1, &id)) return 0;
    lua_Integer ma = luaL_checkinteger(L, 2);
    if (ma < 0) ma = 0;
    robot::set_servo_current(id, (uint16_t)ma);
    return 0;
}

/**
 * robot.get_servo_current(id) → current limit in mA, or 0 on a bad id.
 */
static int l_robot_get_servo_current(lua_State *L)
{
    int id;
    if (!check_servo_id(L, 1, &id)) return 0;
    lua_pushinteger(L, (lua_Integer)robot::get_servo_current(id));
    return 1;
}

/**
 * robot.flush() — Commit buffered servo positions (SyncWrite).
 */
static int l_robot_flush(lua_State *L)
{
    robot::flush();
    return 0;
}

/* ── Servo feedback ───────────────────────────────────────────────────── */

/**
 * robot.read_position(id) → raw position (0-1023), or -1 on error.
 */
static int l_robot_read_position(lua_State *L)
{
    int id;
    if (!check_servo_id(L, 1, &id)) return 0;
    lua_pushinteger(L, robot::read_position(id));
    return 1;
}

/**
 * robot.read_angle_cdeg(id) → signed centidegrees from centre, or
 *                             INT32_MIN on a bad id.
 *
 * The value is in the SAME frame robot.set_servo_angle() takes, so
 * read -> compare -> correct converges. robot.read_position() is in the
 * opposite frame and cannot be used for that.
 */
static int l_robot_read_angle_cdeg(lua_State *L)
{
    int id;
    if (!check_servo_id(L, 1, &id)) return 0;
    lua_pushinteger(L, robot::read_angle_cdeg(id));
    return 1;
}

/**
 * robot.read_speed(id) → signed speed, or -1 on error.
 */
static int l_robot_read_speed(lua_State *L)
{
    int id;
    if (!check_servo_id(L, 1, &id)) return 0;
    lua_pushinteger(L, robot::read_speed(id));
    return 1;
}

/**
 * robot.read_load(id) → signed load value, or -1 on error.
 */
static int l_robot_read_load(lua_State *L)
{
    int id;
    if (!check_servo_id(L, 1, &id)) return 0;
    lua_pushinteger(L, robot::read_load(id));
    return 1;
}

/**
 * robot.read_voltage(id) → voltage in 0.1V units, or -1 on error.
 */
static int l_robot_read_voltage(lua_State *L)
{
    int id;
    if (!check_servo_id(L, 1, &id)) return 0;
    lua_pushinteger(L, robot::read_voltage(id));
    return 1;
}

/**
 * robot.read_temperature(id) → temperature in °C, or -1 on error.
 */
static int l_robot_read_temperature(lua_State *L)
{
    int id;
    if (!check_servo_id(L, 1, &id)) return 0;
    lua_pushinteger(L, robot::read_temperature(id));
    return 1;
}

/**
 * robot.read_moving(id) → 0 (stopped) or 1 (moving), or -1 on error.
 */
static int l_robot_read_moving(lua_State *L)
{
    int id;
    if (!check_servo_id(L, 1, &id)) return 0;
    lua_pushinteger(L, robot::read_moving(id));
    return 1;
}

/**
 * robot.read_current(id) → current in mA, or -1 on error.
 */
static int l_robot_read_current(lua_State *L)
{
    int id;
    if (!check_servo_id(L, 1, &id)) return 0;
    lua_pushinteger(L, robot::read_current(id));
    return 1;
}

/**
 * robot.ping(id) → servo model number, or ≤0 on failure.
 */
static int l_robot_ping_servo(lua_State *L)
{
    int id;
    if (!check_servo_id(L, 1, &id)) return 0;
    lua_pushinteger(L, robot::ping_servo(id));
    return 1;
}

/* ── Calibration ──────────────────────────────────────────────────────── */

/**
 * robot.set_offset(id, deg) — Set calibration offset in degrees.
 */
static int l_robot_set_offset(lua_State *L)
{
    int id;
    if (!check_servo_id(L, 1, &id)) return 0;
    float deg = (float)luaL_checknumber(L, 2);
    robot::set_offset(id, deg);
    ESP_LOGV(TAG, "set_offset: id=%d deg=%.1f", id, deg);
    return 0;
}

/**
 * robot.get_offset(id) → offset in degrees.
 */
static int l_robot_get_offset(lua_State *L)
{
    int id;
    if (!check_servo_id(L, 1, &id)) return 0;
    lua_pushnumber(L, robot::get_offset(id));
    return 1;
}

/**
 * robot.reset_offsets() — Zero all calibration offsets.
 */
static int l_robot_reset_offsets(lua_State *L)
{
    robot::reset_offsets();
    ESP_LOGI(TAG, "reset_offsets");
    return 0;
}

/* ── Utility ──────────────────────────────────────────────────────────── */

/**
 * robot.delay_ms(ms) — Blocking delay. Returns immediately if ≤0.
 */
static int l_robot_delay_ms(lua_State *L)
{
    int ms = (int)luaL_checkinteger(L, 1);
    if (ms <= 0) return 0;

    // Break delay into 50 ms chunks
    const TickType_t CHUNK_MS = 50;
    while (ms > 0) {
        TickType_t delay = pdMS_TO_TICKS(
            (ms > (int)CHUNK_MS) ? CHUNK_MS : (TickType_t)ms);
        vTaskDelay(delay);
        ms -= (int)CHUNK_MS;
    }

    return 0;
}

/* ── Inverse Kinematics ───────────────────────────────────────────────── */

/**
 * robot.ik_fr(x, th0, z) — Front-right leg IK. Does NOT flush.
 */
static int l_robot_ik_fr(lua_State *L)
{
    float x   = (float)luaL_checknumber(L, 1);
    float th0 = (float)luaL_checknumber(L, 2);
    float z   = (float)luaL_checknumber(L, 3);
    robot::front_right_ik(x, th0, z);
    return 0;
}

/**
 * robot.ik_fl(x, th0, z) — Front-left leg IK. Does NOT flush.
 */
static int l_robot_ik_fl(lua_State *L)
{
    float x   = (float)luaL_checknumber(L, 1);
    float th0 = (float)luaL_checknumber(L, 2);
    float z   = (float)luaL_checknumber(L, 3);
    robot::front_left_ik(x, th0, z);
    return 0;
}

/**
 * robot.ik_rr(x, th0, z) — Rear-right leg IK. Does NOT flush.
 */
static int l_robot_ik_rr(lua_State *L)
{
    float x   = (float)luaL_checknumber(L, 1);
    float th0 = (float)luaL_checknumber(L, 2);
    float z   = (float)luaL_checknumber(L, 3);
    robot::rear_right_ik(x, th0, z);
    return 0;
}

/**
 * robot.ik_rl(x, th0, z) — Rear-left leg IK. Does NOT flush.
 */
static int l_robot_ik_rl(lua_State *L)
{
    float x   = (float)luaL_checknumber(L, 1);
    float th0 = (float)luaL_checknumber(L, 2);
    float z   = (float)luaL_checknumber(L, 3);
    robot::rear_left_ik(x, th0, z);
    return 0;
}

/* ── IMU ──────────────────────────────────────────────────────────────── */

/**
 * robot.imu_read() → {ax, ay, az, gx, gy, gz, mx, my, mz, mag_valid}
 *   — Returns a table with accelerometer (g), gyroscope (dps) and
 *     magnetometer (uT) data.
 *
 * The magnetometer is the BMM150 on the BMI270's auxiliary bus, present on
 * the new board only. mag_valid is false until it has been read at least
 * once: a dead or absent magnetometer must not cost the gait, so accel and
 * gyro still report and the m* fields stay at zero. Scripts written against
 * the old six fields keep working — the extra keys are additive.
 */
static int l_robot_imu_read(lua_State *L)
{
    robot::ImuData imu = robot::imu_read();

    lua_createtable(L, 0, 10);
    lua_pushnumber(L, imu.ax); lua_setfield(L, -2, "ax");
    lua_pushnumber(L, imu.ay); lua_setfield(L, -2, "ay");
    lua_pushnumber(L, imu.az); lua_setfield(L, -2, "az");
    lua_pushnumber(L, imu.gx); lua_setfield(L, -2, "gx");
    lua_pushnumber(L, imu.gy); lua_setfield(L, -2, "gy");
    lua_pushnumber(L, imu.gz); lua_setfield(L, -2, "gz");
    lua_pushnumber(L, imu.mx); lua_setfield(L, -2, "mx");
    lua_pushnumber(L, imu.my); lua_setfield(L, -2, "my");
    lua_pushnumber(L, imu.mz); lua_setfield(L, -2, "mz");
    lua_pushboolean(L, imu.mag_valid); lua_setfield(L, -2, "mag_valid");
    return 1;
}

/**
 * robot.imu_print() — Log the latest IMU data to the console.
 */
static int l_robot_imu_print(lua_State *L)
{
    robot::imu_print();
    return 0;
}

/* ── Registration ─────────────────────────────────────────────────────── */

static const struct luaL_Reg ROBOT_LIB[] = {
    /* Gait control */
    { "gait",              l_robot_gait              },
    { "get_mode",          l_robot_get_mode          },
    /* Configuration */
    { "set_config",        l_robot_set_config        },
    { "get_config",        l_robot_get_config        },
    /* Low-level servo control */
    { "set_servo_angle",   l_robot_set_servo_angle   },
    { "set_servo_speed",   l_robot_set_servo_speed   },
    { "set_all_servo_speed", l_robot_set_all_servo_speed },
    { "set_servo_current", l_robot_set_servo_current },
    { "get_servo_current", l_robot_get_servo_current },
    { "flush",             l_robot_flush             },
    /* Servo feedback */
    { "read_position",     l_robot_read_position     },
    { "read_angle_cdeg",   l_robot_read_angle_cdeg   },
    { "read_speed",        l_robot_read_speed        },
    { "read_load",         l_robot_read_load         },
    { "read_voltage",      l_robot_read_voltage      },
    { "read_temperature",  l_robot_read_temperature  },
    { "read_moving",       l_robot_read_moving       },
    { "read_current",      l_robot_read_current      },
    { "ping",              l_robot_ping_servo        },
    /* Calibration */
    { "set_offset",        l_robot_set_offset        },
    { "get_offset",        l_robot_get_offset        },
    { "reset_offsets",     l_robot_reset_offsets     },
    /* Utility */
    { "delay_ms",          l_robot_delay_ms          },
    /* Inverse Kinematics */
    { "ik_fr",             l_robot_ik_fr             },
    { "ik_fl",             l_robot_ik_fl             },
    { "ik_rr",             l_robot_ik_rr             },
    { "ik_rl",             l_robot_ik_rl             },
    /* IMU */
    { "imu_read",          l_robot_imu_read          },
    { "imu_print",         l_robot_imu_print         },
    /* Sentinel */
    { NULL, NULL }
};

void lua_register_robot_bindings(lua_State *L)
{
    /* Create the robot table */
    lua_createtable(L, 0, 26); /* pre-size for ~26 entries */

    /* Register all functions */
    for (const struct luaL_Reg *lib = ROBOT_LIB; lib->func != NULL; lib++) {
        lua_pushcfunction(L, lib->func);
        lua_setfield(L, -2, lib->name);
    }

    /* Set as global _G.robot */
    lua_setglobal(L, "robot");

    ESP_LOGI(TAG, "Registered %d robot bindings", 26);
}

/* ═══════════════════════════════════════════════════════════════
 *  wasm module — .wasm skill execution from Lua
 * ═══════════════════════════════════════════════════════════════ */

/**
 * wasm.run(path, func_name?) → bool
 *
 * Loads a .wasm file from LittleFS, instantiates it, and calls
 * the exported function.  func_name defaults to "on_start".
 *
 * Example:
 *   local ok = wasm.run("/walk.wasm", "on_start")
 *   if not ok then print("Skill failed") end
 */
static int l_wasm_run(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    const char *func_name = lua_isstring(L, 2) ? lua_tostring(L, 2) : nullptr;

    if (!path || path[0] == '\0') {
        lua_pushboolean(L, 0);
        return 1;
    }

    ESP_LOGI(TAG, "wasm.run: loading '%s' func='%s'", path,
             func_name ? func_name : "on_start");

    wasm::SandboxResult result = wasm::load_and_run(
        path, func_name ? func_name : "on_start", 30000);

    bool ok = (result == wasm::SandboxResult::Success);
    if (!ok) {
        ESP_LOGW(TAG, "wasm.run: '%s' failed (result=%d)", path,
                 static_cast<int>(result));
    }

    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

/**
 * wasm.run_bytes(data, func_name?) → bool
 *
 * Loads a .wasm module (plain or MPXE-encrypted) from a Lua string,
 * instantiates it, and calls the exported function.
 * func_name defaults to "on_start".
 *
 * Example:
 *   local data = fs.read("/walk.wasm")
 *   local ok = wasm.run_bytes(data, "on_start")
 */
static int l_wasm_run_bytes(lua_State *L)
{
    size_t data_len = 0;
    const char *data = luaL_checklstring(L, 1, &data_len);
    const char *func_name = lua_isstring(L, 2) ? lua_tostring(L, 2) : nullptr;

    if (!data || data_len == 0) {
        lua_pushboolean(L, 0);
        return 1;
    }

    ESP_LOGI(TAG, "wasm.run_bytes: %zu bytes func='%s'",
             data_len, func_name ? func_name : "on_start");

    // ── MPXE auto-detection (for raw bytes, no extension) ────
    const uint8_t *effective_bytes = reinterpret_cast<const uint8_t *>(data);
    size_t effective_size = data_len;
    uint8_t *decrypted_buf = nullptr;

    wasm::DecryptResult dr = wasm::decrypt_mpxe_autodetect(
        effective_bytes, effective_size, &decrypted_buf, &effective_size);

    if (dr == wasm::DecryptResult::Success) {
        ESP_LOGI(TAG, "wasm.run_bytes: decrypted MPXE → %zu bytes", effective_size);
        effective_bytes = decrypted_buf;
    } else if (dr != wasm::DecryptResult::NotEncrypted) {
        ESP_LOGE(TAG, "wasm.run_bytes: MPXE decrypt failed (result=%d)",
                 static_cast<int>(dr));
        lua_pushboolean(L, 0);
        return 1;
    }

    wasm::SandboxResult result = wasm::load_and_run_bytes(
        effective_bytes, effective_size,
        func_name ? func_name : "on_start", 30000);

    // Zero and free decrypted buffer
    if (decrypted_buf) {
        volatile uint8_t *p = decrypted_buf;
        for (size_t i = 0; i < effective_size; i++) p[i] = 0;
        std::free(decrypted_buf);
    }

    bool ok = (result == wasm::SandboxResult::Success);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

static const struct luaL_Reg WASM_LIB[] = {
    { "run",       l_wasm_run       },
    { "run_bytes", l_wasm_run_bytes },
    { NULL, NULL }
};

void lua_register_wasm_bindings(lua_State *L)
{
    lua_createtable(L, 0, 2);

    for (const struct luaL_Reg *lib = WASM_LIB; lib->func != NULL; lib++) {
        lua_pushcfunction(L, lib->func);
        lua_setfield(L, -2, lib->name);
    }

    lua_setglobal(L, "wasm");
    ESP_LOGI(TAG, "Registered wasm module (run, run_bytes)");
}

/* ═══════════════════════════════════════════════════════════════
 *  fs module — LittleFS file access from Lua
 * ═══════════════════════════════════════════════════════════════
 *
 * Write and delete operations broadcast an openclaw_action to the
 * PWA and wait for explicit user approval via semaphore.
 */

/**
 * fs.read(path) → string content or nil
 */
static int l_fs_read(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    if (!path) {
        lua_pushnil(L);
        return 1;
    }

    auto data = fs::read_file(path);
    if (data.empty()) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushlstring(L, reinterpret_cast<const char *>(data.data()), data.size());
    return 1;
}

/**
 * fs.write(path, content) → bool
 *
 * Requests user permission via action telemetry before writing.
 */
static int l_fs_write(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    size_t content_len = 0;
    const char *content = luaL_checklstring(L, 2, &content_len);

    if (!path || path[0] == '\0') {
        lua_pushboolean(L, 0);
        return 1;
    }

    // Build a description for the permission request
    char desc[256];
    std::snprintf(desc, sizeof(desc), "Write file '%s' (%zu bytes)", path, content_len);

    // Request user permission (blocks until approved/denied, 60s timeout)
    if (!network::request_permission("file_write", desc, 60000)) {
        ESP_LOGW(TAG, "fs.write: user denied write to '%s'", path);
        lua_pushboolean(L, 0);
        return 1;
    }

    bool ok = fs::write_file(path, content, content_len);
    if (!ok) {
        ESP_LOGE(TAG, "fs.write: failed to write '%s'", path);
    }

    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

/**
 * fs.delete(path) → bool
 *
 * Requests user permission via action telemetry before deleting.
 */
static int l_fs_delete(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    if (!path || path[0] == '\0') {
        lua_pushboolean(L, 0);
        return 1;
    }

    char desc[256];
    std::snprintf(desc, sizeof(desc), "Delete file '%s'", path);

    // Request user permission (blocks until approved/denied, 60s timeout)
    if (!network::request_permission("file_delete", desc, 60000)) {
        ESP_LOGW(TAG, "fs.delete: user denied delete of '%s'", path);
        lua_pushboolean(L, 0);
        return 1;
    }

    bool ok = fs::delete_file(path);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

/**
 * fs.exists(path) → bool
 */
static int l_fs_exists(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    lua_pushboolean(L, fs::file_exists(path) ? 1 : 0);
    return 1;
}

/**
 * fs.list(dir) → table of {name, size, is_dir}
 *
 * Returns a Lua array listing all entries in the directory.
 * Each entry is a table with "name", "size", and "is_dir" keys.
 */
static int l_fs_list(lua_State *L)
{
    const char *dir_path = lua_isstring(L, 1) ? lua_tostring(L, 1) : "/";
    if (dir_path[0] == '\0') dir_path = "/";

    // Build VFS path
    char vfs_path[256];
    if (dir_path[0] != '/') {
        std::snprintf(vfs_path, sizeof(vfs_path), "/fs/%s", dir_path);
    } else if (strncmp(dir_path, "/fs/", 4) != 0) {
        std::snprintf(vfs_path, sizeof(vfs_path), "/fs%s", dir_path);
    } else {
        strncpy(vfs_path, dir_path, sizeof(vfs_path) - 1);
        vfs_path[sizeof(vfs_path) - 1] = '\0';
    }

    DIR *dir = opendir(vfs_path);
    if (!dir) {
        lua_createtable(L, 0, 0);
        return 1;
    }

    lua_createtable(L, 16, 0);
    int idx = 1;

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name(entry->d_name);
        if (name == "." || name == "..") continue;

        lua_createtable(L, 0, 3);
        lua_pushstring(L, name.c_str());
        lua_setfield(L, -2, "name");

        bool is_dir = (entry->d_type == DT_DIR);
        lua_pushboolean(L, is_dir ? 1 : 0);
        lua_setfield(L, -2, "is_dir");

        if (!is_dir) {
            // Build full path for file_size
            char full_path[320];
            std::snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, name.c_str());
            std::size_t sz = fs::file_size(full_path);
            lua_pushinteger(L, static_cast<lua_Integer>(sz));
            lua_setfield(L, -2, "size");
        } else {
            lua_pushinteger(L, 0);
            lua_setfield(L, -2, "size");
        }

        lua_rawseti(L, -2, idx++);
    }

    closedir(dir);
    return 1;
}

/**
 * fs.info() → {total, used} bytes
 */
static int l_fs_info(lua_State *L)
{
    std::size_t total = 0, used = 0;
    fs::stats(total, used);

    lua_createtable(L, 0, 2);
    lua_pushinteger(L, static_cast<lua_Integer>(total));
    lua_setfield(L, -2, "total");
    lua_pushinteger(L, static_cast<lua_Integer>(used));
    lua_setfield(L, -2, "used");
    return 1;
}

/* ═══════════════════════════════════════════════════════════════
 *  crypto module — base64 decode for WASM deployment
 * ═══════════════════════════════════════════════════════════════ */

/**
 * crypto.base64_decode(str) → string (raw bytes)
 *
 * Decodes a standard base64-encoded string into raw bytes.
 * Supports both RFC 4648 base64 ('+', '/') with optional '=' padding
 * and unpadded base64 (no '=' chars).
 *
 * This handles '=' padding correctly, so callers can use standard
 * base64 encoding without needing null-byte padding tricks.
 */
static int l_crypto_base64_decode(lua_State *L)
{
    size_t in_len = 0;
    const char *in = luaL_checklstring(L, 1, &in_len);

    if (!in || in_len == 0) {
        lua_pushliteral(L, "");
        return 1;
    }

    // Base64 decoding table: index by ASCII value, 0xFF = invalid
    static const unsigned char DEC[256] = {
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x3E,0xFF,0xFF,0xFF,0x3F,
        0x34,0x35,0x36,0x37,0x38,0x39,0x3A,0x3B,0x3C,0x3D,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        0xFF,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,
        0x0F,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0xFF,0xFF,0xFF,0xFF,0xFF,
        0xFF,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F,0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,
        0x29,0x2A,0x2B,0x2C,0x2D,0x2E,0x2F,0x30,0x31,0x32,0x33,0xFF,0xFF,0xFF,0xFF,0xFF,
        // Above 0x7F: all invalid
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    };

    // Count valid base64 characters (a-z, A-Z, 0-9, +, /)
    // '=' is padding — excluded from valid count
    size_t valid = 0;
    for (size_t i = 0; i < in_len; i++) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        if (DEC[c] != 0xFF) {
            valid++;
        }
        // '=' and other chars (whitespace, newlines) are silently skipped
    }

    if (valid == 0) {
        lua_pushliteral(L, "");
        return 1;
    }

    // Output size calculation:
    //   Each complete group of 4 valid chars → 3 bytes.
    //   Trailing partial group: 2 valid → 1 byte, 3 valid → 2 bytes.
    //   '=' padding is already excluded from valid, so no extra
    //   subtraction is needed — the partial-group logic handles it.
    size_t out_len = (valid / 4) * 3;
    size_t rem = valid % 4;
    if (rem == 2) out_len += 1;
    else if (rem == 3) out_len += 2;
    // rem == 0 or 1: no partial output (1 valid char is invalid base64)

    // Allocate output buffer on heap (WASM files can be large)
    auto *out = static_cast<char *>(std::malloc(out_len + 1));
    if (!out) {
        lua_pushnil(L);
        return 1;
    }

    // Decode
    size_t out_pos = 0;
    unsigned char buf[4];
    int buf_pos = 0;

    for (size_t i = 0; i < in_len; i++) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        unsigned char val = DEC[c];

        if (val == 0xFF) continue;  // skip whitespace, '=', and other non-base64 chars

        buf[buf_pos++] = val;

        if (buf_pos == 4) {
            out[out_pos++] = (buf[0] << 2) | (buf[1] >> 4);
            out[out_pos++] = (buf[1] << 4) | (buf[2] >> 2);
            out[out_pos++] = (buf[2] << 6) | buf[3];
            buf_pos = 0;
        }
    }

    // Handle trailing partial group
    if (buf_pos >= 2) {
        out[out_pos++] = (buf[0] << 2) | (buf[1] >> 4);
    }
    if (buf_pos >= 3) {
        out[out_pos++] = (buf[1] << 4) | (buf[2] >> 2);
    }

    // Sanity check: decoded bytes should match our size prediction
    if (out_pos != out_len) {
        ESP_LOGW(TAG, "base64_decode: size mismatch (expected=%zu, actual=%zu) — "
                 "input may be malformed", out_len, out_pos);
    }

    out[out_pos] = 0;
    lua_pushlstring(L, out, out_pos);  // use actual decoded size, not prediction
    std::free(out);
    return 1;
}

/**
 * crypto.base64_encode(str) → string (base64)
 *
 * Encodes raw bytes as standard RFC 4648 base64 with '=' padding.
 * Useful for debugging and for generating deploy-compatible data.
 */
static int l_crypto_base64_encode(lua_State *L)
{
    size_t data_len = 0;
    const char *data = luaL_checklstring(L, 1, &data_len);

    if (!data || data_len == 0) {
        lua_pushliteral(L, "");
        return 1;
    }

    static const char ENC[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    size_t out_len = ((data_len + 2) / 3) * 4;  // +2 for ceiling division
    auto *out = static_cast<char *>(std::malloc(out_len + 1));
    if (!out) {
        lua_pushnil(L);
        return 1;
    }

    size_t out_pos = 0;
    for (size_t i = 0; i < data_len; i += 3) {
        uint32_t triple = static_cast<unsigned char>(data[i]) << 16;
        if (i + 1 < data_len)
            triple |= static_cast<unsigned char>(data[i + 1]) << 8;
        if (i + 2 < data_len)
            triple |= static_cast<unsigned char>(data[i + 2]);

        out[out_pos++] = ENC[(triple >> 18) & 0x3F];
        out[out_pos++] = ENC[(triple >> 12) & 0x3F];
        out[out_pos++] = (i + 1 < data_len) ? ENC[(triple >> 6) & 0x3F] : '=';
        out[out_pos++] = (i + 2 < data_len) ? ENC[triple & 0x3F] : '=';
    }

    out[out_pos] = 0;
    lua_pushlstring(L, out, out_len);
    std::free(out);
    return 1;
}

static const struct luaL_Reg CRYPTO_LIB[] = {
    { "base64_decode", l_crypto_base64_decode },
    { "base64_encode", l_crypto_base64_encode },
    { NULL, NULL }
};

void lua_register_crypto_bindings(lua_State *L)
{
    lua_createtable(L, 0, 2);

    for (const struct luaL_Reg *lib = CRYPTO_LIB; lib->func != NULL; lib++) {
        lua_pushcfunction(L, lib->func);
        lua_setfield(L, -2, lib->name);
    }

    lua_setglobal(L, "crypto");
    ESP_LOGI(TAG, "Registered crypto module (base64_decode, base64_encode)");
}

/* ═══════════════════════════════════════════════════════════════
 *  fs module — LittleFS file access from Lua
 * ═══════════════════════════════════════════════════════════════ */

static const struct luaL_Reg FS_LIB[] = {
    { "read",   l_fs_read   },
    { "write",  l_fs_write  },
    { "delete", l_fs_delete },
    { "exists", l_fs_exists },
    { "list",   l_fs_list   },
    { "info",   l_fs_info   },
    { NULL, NULL }
};

void lua_register_fs_bindings(lua_State *L)
{
    lua_createtable(L, 0, 6);

    for (const struct luaL_Reg *lib = FS_LIB; lib->func != NULL; lib++) {
        lua_pushcfunction(L, lib->func);
        lua_setfield(L, -2, lib->name);
    }

    lua_setglobal(L, "fs");
    ESP_LOGI(TAG, "Registered fs module (read, write, delete, exists, list, info)");
}
