# mpx_robot

The MangDang quadruped: four AT32F413 servo driver boards over SPI, the
Stanford trot gait, inverse kinematics, and the BMI270 IMU.

Ported from `main/robot/` of the MPX-Dog firmware. Everything outside this
component talks to it through the C facade in `include/mpx_robot.h`.

## What changed in the port

| | MPX-Dog board | MP4 ESP32 CORE |
|---|---|---|
| Servo bus | SPI2, MOSI 11 / MISO 13 / CLK 12 | **SPI3**, MOSI 6 / MISO 17 / CLK 16 |
| Chip selects | 9 / 10 / 14 / 21 | **15 / 7 / 4 / 5** (CN3..CN6) |
| Servo power enable | GPIO 8, driven high at init | **none** — the rail is on Vbat+ |
| IMU | QMI8658C on SPI2, sharing the servo bus | **BMI270 on I2C0** |
| Bus contention | IMU vs. servo config frames, needed a lock | none — nothing else is on SPI3 |
| Skill runtime coupling | `#include "wasm/wasm_sandbox.h"` | a hook table, see below |
| NVS namespace | `robot` | `mpx_robot` |

Three of those deserve a sentence each.

**No power-enable pin.** The old board switched the servo rail with GPIO 8.
On the MP4 board that pin is `IO8_PI_2` on the expansion header, and the four
HC-PHD-2 connectors are wired to `Vbat+` directly, so there is nothing to
switch. `driver_board_power()` is kept as a no-op rather than deleted, because
WASM skills compiled against the old ABI still call it. The one-second delay
that used to follow it is kept too — it was never really about the rail, it
gives the four AT32F413s time to finish their own boot before the first probe.

**The IMU moved buses.** `driver_board.h` still carries a long note about a
bus lock. That hazard was real: a QMI8658C shared SPI2 with the driver boards,
transacted every 50 ms, and would land in the middle of a config
request/reply pair often enough that Servo Studio could not reliably write a
parameter. On this board the IMU is on I2C and SPI3 belongs to this driver
alone. The lock functions remain — they still take the driver's own mutex, so
a future bus sharer has somewhere to synchronise — but nothing outside this
component needs them today.

**The skill runtime is behind a hook table.** `robot.cc` used to call
`wasm::is_running()` and `sdk::control_owner_is_pose()` directly. Those live in
`mpx_wasm` now, and the dependency already runs the other way — mpx_wasm's host
functions drive this layer — so making it mutual would put the two components
in a requirement cycle. `mpx_wasm` registers the two predicates through
`mpx_robot_set_skill_hooks()` at init instead. With no runtime registered both
answer "no", which is correct for a build with no WASM support.

## Which connector is which leg

The schematic labels the servo connectors CN3..CN6 and says nothing about
legs — that is a harness decision. It is recorded in exactly one place, at the
top of `src/driver_board.c`:

```c
#define SPI_CS_FRONT_RIGHT  SPI_CS_CN3   /* board 0, servos 1-3   */
#define SPI_CS_FRONT_LEFT   SPI_CS_CN4   /* board 1, servos 4-6   */
#define SPI_CS_REAR_RIGHT   SPI_CS_CN5   /* board 2, servos 7-9   */
#define SPI_CS_REAR_LEFT    SPI_CS_CN6   /* board 3, servos 10-12 */
```

If the robot walks but the legs move in the wrong order, change those four
lines and nothing else. Within-board channel swaps are a different problem and
are handled by `db_phys()` / `CONFIG_MP4_ROBOT_SERVO_BOARD_VARIANT`.

## Servo ids

```
leg:   FR   FL   RR   RL
abd:    1    4    7   10   (hip yaw)
hip:    2    5    8   11   (shoulder)
knee:   3    6    9   12
```

## Two position frames

There are two raw frames and they run in opposite directions:

- **gait frame** — what `set_servo_angle()` speaks. 0..1023, 511 = centre,
  positive degrees increase the value.
- **AT32 frame** — what the driver boards, all feedback and Servo Studio
  speak. 0..1023, and `at32_raw == 1024 - gait_raw`.

Anything comparing a commanded position against a measured one must put both
in the same frame first. `mpx_robot_read_angle_cdeg()` is the reader to close
a loop with; `mpx_robot_read_position()` is the AT32 frame and will diverge.

## IMU ownership

This component is the single owner of the BMI270. ESP-Claw's `lua_module_imu`
has its own BMI270 backend, and two callers of `bmi270_sensor_create()` on one
chip does not work, so `CONFIG_APP_CLAW_LUA_MODULE_IMU` is off in the board
defaults. The chip is configured with the same ranges ESP-Claw's backend uses
— ±16 g, ±2000 dps, 200 Hz — so readings are comparable between the two.

## Recalibrate after the first flash

The per-servo offsets in NVS were calibrated against the Feetech servo scale
the firmware used before the AT32 driver boards arrived (see the note at
`robot.h:27`). They are stored in degrees and are now applied 1.5× smaller
than when they were set. Zero them and recalibrate.

## Files

| File | Lines | Purpose |
|---|---|---|
| `src/driver_board.c` / `.h` | ~1000 | SPI protocol to the four AT32F413 boards |
| `src/robot.cc` / `.h` | ~2700 | gait task, movement table, config, calibration |
| `src/stanford_gait.cc` | 179 | trot phase scheduler, ported from StanfordQuadruped |
| `src/stanford_kinematics.cc` | 169 | exact BSP inverse kinematics |
| `src/mpx_imu.cc` | 322 | BMI270 over I2C, replaces the QMI8658C/SPI driver |
| `src/mpx_robot_c_api.cc` | 160 | the C facade, name translation only |
| `include/mpx_robot.h` | 191 | the only header other components should include |
