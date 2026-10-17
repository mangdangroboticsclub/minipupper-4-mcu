# RL host link (mp4-link protocol) in mpx-dog

`rl_link.cc` lets a policy on the Raspberry Pi drive all twelve joints
directly, exactly as on the `mp4-link` firmware: the Pi sends twelve raw
targets (`SET_ALL`), the robot answers every tick with positions, currents,
temperatures and the IMU (`STATE`). The frames are mp4-link's byte for byte
(its `PROTOCOL.md`), so `mp3link` and anything built on `mp3link.Link` work
unchanged. Only the port and the baud rate differ.

Host side: `ros2jazzympv2/rl_link/` (`python3 -m mp3link ...`).

## Wiring and port

| | mp4-link firmware | mpx-dog |
|---|---|---|
| Pi port | `/dev/ttyAMA2` (GPIO 4/5, pins 7/29) | `/dev/ttyAMA0` (GPIO 14/15, pins 8/10) |
| ESP pins | IO8 TX, IO18 RX | IO18 TX, IO8 RX (the ROS link's wires) |
| Baud | 1 000 000 | `APP_PI_LINK_UART_BAUD`, 921 600 |
| USB host | UART0 at 921 600 | none (UART0 is the console) |

The RL frames share the UART with the ROS link (`pi_protocol.h`). Both
parsers see every byte; a frame only counts when its CRC checks, so the two
cannot be mistaken for each other.

## Sessions

mp4-link is nothing but the link. Here the robot also walks, talks and runs
skills, so the link works in sessions:

- **Start:** the first valid RL frame (a request or a `SET_ALL`).
  `robot::host::begin()` parks the gait and fences off the app, the agent and
  skills, as the ROS bridge's slave mode does (`/v1/pi/status` `in_control`,
  "ROS" on the screen, voice off).
- **Before the first ARM:** the servos HOLD the position the gait last sent.
  A `ping` does not drop a standing robot. A joint a skill left in torque mode
  goes IDLE instead of being replayed outside the guard.
- **After any ARM, DISARM, IDLE, watchdog or guard trip:** mp4-link's rule.
  Disarmed means every servo IDLE (limp).
- **End:** no RL frame for `APP_RL_RELEASE_MS` (2 s). Every servo goes IDLE,
  then the robot reads where the legs are and glides into the stand, and the
  app, voice and skills get it back.
- **ROS bridge in control:** RL frames are ignored and get no answer. A
  running RL session in turn keeps the ROS bridge in monitor mode. Whoever
  came first keeps the robot.
- **Ready:** board requests and ARM wait (up to 300 ms) until the session
  holds the robot and the boards have answered one of its frames. A session
  that is ending accepts no ARM (`REJECTED`).
- **Opening a session takes more than a CRC.** The ROS link's audio and
  video share these bytes, and about 1 in 65 536 of its windows matches a CRC.
  So the frame that opens a session must also make sense: a request needs a
  known command and servo, and SET_ALL needs clean flags and enable bits. For
  a second after any ROS audio or video frame, only a SET_ALL can open one.
  Inside a session, frames are taken exactly as mp4-link takes them, with its
  status codes and its trimming.
- **Start standing still.** A move or skill that is still running when the
  session starts can finish its own gain restore on the boards' config path
  while the policy drives.

`pi_link` sends no ROS telemetry while a session runs. `pi_announce` on the Pi
steps aside on its own when `mp3link` opens the port.

## What is the same

- `SET_ALL` (36 B), `STATE` (106 B), request (10 B) and response (14 B)
  frames, the flags, the CRC and the resync.
- 200 Hz tick (`APP_RL_TICK_HZ`). `t_us` is the previous exchange's time.
- Watchdog: armed and no `SET_ALL` for 100 ms means DISARM and `WATCHDOG`.
- Guard, on driven servos only: over 500 mA for 0.5 s gives `FAULT_CUR`;
  housing over 60.0 °C for 1 s gives `FAULT_TEMP`; a board silent for 20 ticks
  gives `FAULT_BOARD`. Faults latch until a `SET_ALL` with CLEAR.
- Per-servo requests run only while disarmed. `SET_POS` and `TORQUE` are
  refused; `IDLE` from anywhere disarms.
- The config reply, and the board's first frame after it, are never reported
  as feedback.

## What differs

| | mp4-link | mpx-dog |
|---|---|---|
| IMU | own task, 400 Hz, ±4 g / ±1000 °/s | the robot's (`robot/imu.cc`), 200 Hz, ±16 g / ±2000 °/s. Same raw BMI270 axes and units (mg, 0.1 °/s). `IMU_OK` while the sample is under 30 ms old (mp4-link: 20 ms). |
| Servo gains | whatever the host wrote | saved when a session takes the robot and written back when it ends, so the walk keeps its own tuning after `mp3link config` |
| Calibration offsets | NVS `parameter:offsetN` | the gait's own `offset[]` (same stand formula, `511 + offset × 10240 / scale`). `SAVE_OFFSETS` writes it **and** `parameter:offsetN`, so every firmware on the robot keeps one calibration. `GET_OFFSET` never answers status 7. |
| `SET_SCALE` | sets the scale | read-only: the boards' range, read at boot, is what the gait uses. A different value is refused. |
| `RAW_REPLY` | bench diagnostic | status 5 (not kept) |
| `GET_LIVE` | ids 0..13 | ids 0..13 (13, supply current, needs AT32 fw 0.0.9) |
| Stalled tick | task watchdog reboots after 500 ms | no reboot; the boards hold their last frame (the firmware's task watchdog setup belongs to the whole app) |
| Servo board variant | none | `SERVO_BOARD` remap applies: ids are logical, as in the rest of the firmware |

## Checking it

- `GET /v1/pi/status` has an `rl` object with the same.
- Pi: `python3 -m mp3link ping` shows the `STATE` rate (200 Hz), CRC errors
  and the SET_ALL → STATE echo time.
