/**
 * Robot API intellisense definitions for the in-browser Lua editor.
 *
 * Mirrors the C++ robot bindings registered in main/lua/lua_bindings.cc.
 * Used by CodeMirror's autocomplete to provide function signatures,
 * parameter hints, and hover documentation for the `robot.*` API.
 */

export interface RobotCompletion {
  label: string;
  detail: string;
  info?: string;
  boost?: number;
}

export const ROBOT_COMPLETIONS: RobotCompletion[] = [
  // ── Gait control ──────────────────────────────────────────────
  {
    label: "robot.gait",
    detail: "robot.gait(name)",
    info:
      "Start a gait by name.\n\n" +
      "Names: 'init', 'step', 'advance', 'back', 'left', 'right',\n" +
      "'turnL', 'turnR', 'jump', 'jumpfwd', 'twerk', 'roll',\n" +
      "'pitch', 'stretch', 'lookup', 'lookdown', 'lookleft',\n" +
      "'lookright', 'lookul', 'lookur', 'lookll', 'looklr',\n" +
      "'flegL', 'flegR', 'blegL', 'blegR',\n" +
      "'heightup', 'heightdown', 'balance', 'bowback',\n" +
      "'bodycycle', 'headellipse',\n" +
      "'moveLF', 'moveRF', 'moveLB', 'moveRB',\n" +
      "'stanford' (Stanford Pupper trot walk),\n" +
      "'frontkick', 'wiggle', 'buttshrug',\n" +
      "'wiggleL', 'wiggleR', 'buttshrugL', 'buttshrugR' (FPC),\n" +
      "'testspeed', 'none'",
  },
  {
    label: "robot.get_mode",
    detail: "robot.get_mode() → string",
    info: "Returns the name of the currently active gait command.",
  },

  // ── Configuration ────────────────────────────────────────────
  {
    label: "robot.get_config",
    detail: "robot.get_config() → {period, height, up_height, stride, tilt, sg_speed}",
    info: "Returns the current gait parameters as a Lua table.",
  },
  {
    label: "robot.set_config",
    detail: "robot.set_config(period, height, up_height, stride, tilt, sg_speed)",
    info:
      "Set gait parameters.\n" +
      "All arguments are optional — pass nil to keep the current value.\n" +
      "Persisted to NVS automatically.",
  },

  // ── Low-level servo control ──────────────────────────────────
  {
    label: "robot.set_servo_angle",
    detail: "robot.set_servo_angle(id, deg)",
    info:
      "Set a servo's target angle in degrees.\n" +
      "  id  — servo number (1-12)\n" +
      "  deg — target angle in degrees\n" +
      "Call robot.flush() to commit all buffered positions.",
  },
  {
    label: "robot.set_servo_speed",
    detail: "robot.set_servo_speed(id, speed)",
    info:
      "Set movement speed for a single servo.\n" +
      "  id    — servo number (1-12)\n" +
      "  speed — 0 = maximum speed, larger values = slower\n" +
      "\n" +
      "STORED, NEVER SENT on this robot. The servo driver boards have no speed\n" +
      "field: the command frame carries a max current limit instead, and the\n" +
      "board runs its own position loop underneath it. Use\n" +
      "robot.set_servo_current() for the control that actually does something.",
  },
  {
    label: "robot.set_all_servo_speed",
    detail: "robot.set_all_servo_speed(speed)",
    info:
      "Set movement speed for all 12 servos at once.\n" +
      "Stored, not sent — see robot.set_servo_speed().",
  },
  {
    label: "robot.set_servo_current",
    detail: "robot.set_servo_current(id, mA)",
    info:
      "Set a servo's current limit in milliamps — the torque control on this robot.\n" +
      "  id — servo number (1-12)\n" +
      "  mA — 0..1500; clamped to that range\n" +
      "A ceiling, not a forced draw: a lightly loaded leg still only takes what\n" +
      "it needs. 0 means hold with no torque, which for a loaded joint is the\n" +
      "same as releasing it. Persisted across reboots.",
  },
  {
    label: "robot.get_servo_current",
    detail: "robot.get_servo_current(id) → number (mA)",
    info: "Read a servo's current limit in milliamps. Returns 0 for a bad id.",
  },
  {
    label: "robot.flush",
    detail: "robot.flush()",
    info:
      "Push all buffered servo positions to the four driver boards in one SPI pass.\n" +
      "Must be called after set_servo_angle or IK functions for changes to take effect.\n" +
      "Does nothing while Servo Studio or a skill holds the servo bus.",
  },

  // ── Servo feedback ──────────────────────────────────────────
  {
    label: "robot.read_position",
    detail: "robot.read_position(id) → number (0-1023)",
    info:
      "Read a servo's raw position. Returns -1 on error.\n" +
      "\n" +
      "This is the driver boards' own frame, which runs OPPOSITE to the frame\n" +
      "robot.set_servo_angle() takes — so a read/compare/correct loop built on it\n" +
      "diverges instead of converging. Use robot.read_angle_cdeg() for that.",
  },
  {
    label: "robot.read_angle_cdeg",
    detail: "robot.read_angle_cdeg(id) → number (centidegrees)",
    info:
      "Read a servo's measured angle as signed centidegrees from centre, in the\n" +
      "SAME frame robot.set_servo_angle() takes. This is the one to close a loop\n" +
      "with. Returns INT32_MIN on a bad id.",
  },
  {
    label: "robot.read_speed",
    detail: "robot.read_speed(id) → -1",
    info:
      "Always -1 on this robot: the driver boards do not report speed, because\n" +
      "position control is current limited rather than velocity commanded.",
  },
  {
    label: "robot.read_load",
    detail: "robot.read_load(id) → number",
    info: "Read a servo's load, which on this robot is its measured motor current in mA.",
  },
  {
    label: "robot.read_voltage",
    detail: "robot.read_voltage(id) → -1",
    info:
      "Always -1: the servo rail is battery voltage straight through, so there\n" +
      "is no per-servo supply for the driver boards to measure.",
  },
  {
    label: "robot.read_temperature",
    detail: "robot.read_temperature(id) → number (°C)",
    info:
      "Read a servo's NTC temperature in degrees Celsius. A real reading on this\n" +
      "robot, and a free one: it rides on the feedback frame the gait already\n" +
      "fetches. Returns -1 if that servo has never answered.",
  },
  {
    label: "robot.read_moving",
    detail: "robot.read_moving(id) → 0 | 1",
    info: "Check if a servo is moving. 0 = stopped, 1 = moving. Returns -1 on error.",
  },
  {
    label: "robot.read_current",
    detail: "robot.read_current(id) → number (mA)",
    info: "Read servo current in milliamps. A real reading on this robot.",
  },
  {
    label: "robot.ping",
    detail: "robot.ping(id) → 1 | -1",
    info:
      "Check whether a servo channel answers.\n" +
      "Returns 1 if the board replies to a parameter read, -1 if not.",
  },

  // ── Calibration ──────────────────────────────────────────────
  {
    label: "robot.set_offset",
    detail: "robot.set_offset(id, deg)",
    info:
      "Set a calibration offset for a servo (degrees).\n" +
      "Persisted to NVS automatically.\n" +
      "  id  — servo number (1-12)\n" +
      "  deg — offset in degrees",
  },
  {
    label: "robot.get_offset",
    detail: "robot.get_offset(id) → number",
    info: "Get the calibration offset for a servo in degrees.",
  },
  {
    label: "robot.reset_offsets",
    detail: "robot.reset_offsets()",
    info: "Reset all servo calibration offsets to 0 and persist to NVS.",
  },

  // ── Utility ──────────────────────────────────────────────────
  {
    label: "robot.delay_ms",
    detail: "robot.delay_ms(ms)",
    info:
      "Blocking delay in milliseconds.\n" +
      "Returns immediately if ms ≤ 0.\n" +
      "Breaks the delay into 50ms chunks for responsiveness.",
  },

  // ── Inverse Kinematics ───────────────────────────────────────
  {
    label: "robot.ik_fr",
    detail: "robot.ik_fr(x, th0, z)",
    info:
      "Set front-right leg via inverse kinematics.\n" +
      "  x   — forward distance (mm)\n" +
      "  th0 — hip angle (degrees)\n" +
      "  z   — foot height (mm)\n" +
      "Does NOT call robot.flush() — call it manually to commit.",
  },
  {
    label: "robot.ik_fl",
    detail: "robot.ik_fl(x, th0, z)",
    info: "Set front-left leg via IK. Does NOT flush.",
  },
  {
    label: "robot.ik_rr",
    detail: "robot.ik_rr(x, th0, z)",
    info: "Set rear-right leg via IK. Does NOT flush.",
  },
  {
    label: "robot.ik_rl",
    detail: "robot.ik_rl(x, th0, z)",
    info: "Set rear-left leg via IK. Does NOT flush.",
  },

  // ── IMU ──────────────────────────────────────────────────────
  {
    label: "robot.imu_read",
    detail: "robot.imu_read() → {ax, ay, az, gx, gy, gz}",
    info:
      "Read the latest IMU data.\n" +
      "Returns a table with:\n" +
      "  ax, ay, az — accelerometer (g)\n" +
      "  gx, gy, gz — gyroscope (dps)",
  },
  {
    label: "robot.imu_print",
    detail: "robot.imu_print()",
    info: "Print the latest IMU data to the serial console (ESP_LOGI).",
  },
];

/**
 * CodeMirror 6 autocomplete source for the robot API.
 *
 * Returns completions when the user types `robot.` or continues
 * typing a robot function name.
 */
import { CompletionContext } from "@codemirror/autocomplete";

export function robotCompletionSource(context: CompletionContext) {
  const before = context.matchBefore(/\brobot\.\w*/);
  if (!before && !context.explicit) return null;

  // Only activate when we see "robot." prefix
  if (!before) return { from: context.pos, options: [], validFor: /^\brobot\.\w*$/ };

  const query = before.text;
  const from = before.from;

  const options = ROBOT_COMPLETIONS.filter((c) => {
    return query === "robot." || c.label.startsWith(query);
  }).map((c) => ({
    label: c.label,
    type: "function" as const,
    detail: c.detail,
    info: c.info || "",
    boost: c.boost ?? 99,
  }));

  return {
    from,
    options,
    validFor: /^\brobot\.\w*$/,
  };
}
