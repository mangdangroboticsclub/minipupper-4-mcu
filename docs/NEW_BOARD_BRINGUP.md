# New board bring-up (MP4-ESP32-CORE)

This firmware now targets the MP4-ESP32-CORE board. The servo transport changed
completely — Feetech SCSCL over UART1 became four AT32F413 driver boards over
SPI2 — so the robot cannot be trusted until the steps below have been run **on
the bench, on blocks**.

Nothing here is optional. Step 2 in particular is the one that decides whether
the robot walks or fights itself, and it cannot be answered by reading code:
it depends on which side of the chassis the servo loom was routed down.

---

## What changed, in one table

| | Mini Pupper 2 (before) | MP4-ESP32-CORE (now) |
|---|---|---|
| Servo transport | Feetech SCSCL on UART1, TX=IO4 RX=IO5 | 4× AT32F413 on SPI2, MOSI=IO6 MISO=IO17 CLK=IO16 |
| Chip selects | n/a | FR=IO7(CN4) FL=IO4(CN5) RR=IO15(CN3) RL=IO5(CN6) |
| Servo power | GPIO8 enable | none — rail is on Vbat+ |
| Angle range | 180°, fixed in firmware | the boards' own `range_position_deg`, read at boot |
| "Speed" | SCSCL speed register | **no such field**; the frame carries a max current in mA |
| Feedback | pos/speed/load; V/T/I never worked | pos + current + NTC temperature, all real |
| IMU | QMI8658C on SPI2 | BMI270 + BMM150 on I2C (SDA=IO1, SCL=IO2, INT1=IO10) |

The chip-select map is `SERVO_CS_MAP` in `main/robot/board_config.h`. Default 1
is `FR=GPIO7(CN4) FL=GPIO4(CN5) RR=GPIO15(CN3) RL=GPIO5(CN6)`.

---

## 1. Does it boot, and do the boards answer?

Flash, then watch the console. You want:

```
Servo rail is always on (no power-enable GPIO)
Initialising SPI servo driver boards...
Driver board init done
Driver board range: 310.0 deg (33.032 ticks/deg)      <- a REAL number
Servo probe OK - all 12 channels responded            <- ALL TWELVE
```

**If `range` says 310.0 but the probe fails**, no board answered the scale query
either and the firmware is guessing. Check the servo rail first (CN3..CN6),
then the SPI wiring, then that the AT32 boards actually have firmware.

**If the probe lists missing channels**, that is the useful answer — a whole
silent board of three points at that board's chip select or its power, not at
the servos. Compare the missing ids against the CS map:

| Missing | Suspect |
|---|---|
| 1,2,3 | FR board, CS = IO7 |
| 4,5,6 | FL board, CS = IO4 |
| 7,8,9 | RR board, CS = IO15 |
| 10,11,12 | RL board, CS = IO5 |

Boot also prints all twelve servo offsets. **Twelve zeros means this robot has
never been calibrated** — see step 4.

## 2. Which leg is on which connector (do not skip)

This is the highest-risk item in the whole port. It has been got wrong twice
before in this family of firmware, both times by reasoning from source instead
of looking.

The map is not derivable from anything in the repository: it is a property of
how the loom was routed on your chassis. Symptoms of a wrong map are mild —
the robot still stands, because every leg holds a similar pose, and a symmetric
gait looks almost right. It only becomes obvious under asymmetric motion, and
then it looks like the robot fighting itself.

**Procedure.** Block the robot so the legs hang free. Then:

1. `GET /v1/studio/mode?on=1` — park the gait.
2. `GET /v1/studio/direct?id=1&m=1&deg=200&cur=300` — move channel 1.
3. **Watch which joint moves.** It must be the **front-right abduction** (the
   joint that swings that leg outward, not the one that lifts it).
4. Repeat for 2..12. In order, the expected answers are:
   FR abd, FR hip, FR knee, FL abd, FL hip, FL knee, RR abd, RR hip, RR knee,
   RL abd, RL hip, RL knee.
5. If they are not in that order, change `SERVO_CS_MAP` (or `SERVO_BOARD` for a
   within-leg hip/calf swap) in `main/robot/board_config.h` and reflash. Both
   are documented in full there. **Change the map, not the wiring.**

The calibration page in the PWA is the same test with a slider and is easier to
drive one-handed — but note that its leg labels were wrong until this change
(they were rotated by one leg, which is exactly how a correct loom once got
misdiagnosed as mirrored).

## 3. Directions and the neutral stand

Still on blocks, gait parked.

1. `POST /v1/robot/gait {"mode":"init"}` — all twelve to horn centre. The robot
   should settle into a stand, not collapse or fold a leg the wrong way.
2. `POST /v1/robot/gait {"mode":"stanford"}` — a slow trot on the spot. Hip and
   knee directions are the same ones proven on Mini Pupper 2, so a leg folding
   backwards here means the *map* is wrong, not the sign.
3. Strafe and turn (joystick, or `left` / `right` / `turnL` / `turnR`). **This
   is the first time the abduction sign has ever been checked** — it was never
   verified even on Mini Pupper 2, because a forward walk never moves it. If a
   hip yaws the wrong way, flip that leg's entry in the abduction row of
   `SK_SIGN` in `main/robot/stanford_kinematics.cc`. That table is documented
   there as the only hardware knob left in the kinematics.
4. `NEUTRAL_Z` (in `robot.h`) and `SK_NEUTRAL_HEIGHT_MM` (in
   `stanford_kinematics.h`) must stay equal, and must equal the height at which
   all twelve servos centred is the correct stance. On a chassis with different
   leg geometry, re-measure both.

## 4. Calibrate

Set the twelve offsets from the calibration page, watching each joint. Then bake
the result into `kFactoryOffset[13]` in `main/robot/robot.cc` and bump
`kCalSeed` by one, so a future firmware reset does not silently start from
uncalibrated horn centres.

Two things to know:

- The offsets are in **degrees** and are applied by the IK, so they are
  relative to the servo scale. Numbers from a robot on a different range will
  not reproduce the pose they came from. On a fresh board this means there is
  nothing to carry over — start from zero and measure.
- The firmware will import offsets from the reference build's NVS namespace
  (`parameter`) if this firmware's own namespace is empty. That exists so a
  robot calibrated on that build is not thrown away by a reflash. Treat what it
  imports as a starting point, not a calibrated stance.

## 5. Current limits

The boards do position control with a **current limit**, not a speed, so this is
the real torque knob.

Bring-up value: start at **600 mA** and raise to the 1500 mA default once the
gait is clean.

```
GET /v1/studio/status          <- ma column is the LIVE current per servo
GET /v1/studio/temps           <- NTC temperature per servo
```

A joint that is fighting its own direction sign shows up here as current pinned
at the cap, and as a temperature that climbs while the others sit still. At 600
it stalls instead of tearing the leg. That is the point of starting low.

## 6. IMU

```
GET /v1/robot/status      -> look for the IMU log, or:
```
Watch boot for `IMU initialised`. If accel/gyro come up but the magnetometer
does not, that is a degraded but working state (`mag_valid` stays false in Lua)
— the BMM150 sits on the BMI270's auxiliary bus and needs the BMI270's internal
2 kΩ pull-ups, which `robot/imu.cc` enables. A dead magnetometer must not cost
the robot its gait, and does not.

## 7. Then, and only then

Marketplace browse/install, `.mpxe` decrypt, Lua run, and all 46 gait names.
Steps 2 and 3 are the ones that must be signed off before anything energetic —
a backflip on a mirrored loom is a destroyed robot, and a mirrored loom is
otherwise almost invisible.

---

## Servo Studio reference

All routes are `GET`. Parameter routes need `?on=1` on `/v1/studio/mode` first;
`status` and `temps` do not, because that telemetry arrives on every feedback
frame anyway.

| Route | What it does |
|---|---|
| `/v1/studio/mode?on=0\|1` | Park/resume the gait. Required before any parameter route. |
| `/v1/studio/status` | All 12: position (raw + degrees), current (mA), temperature. |
| `/v1/studio/temps` | All 12 NTC temperatures. |
| `/v1/studio/scan` | Which channels answer. |
| `/v1/studio/dump?id=N` | Every config parameter of one servo. |
| `/v1/studio/set?id=N&p=P&v=V` | Write one parameter to RAM, read it back. |
| `/v1/studio/setall?p=P&v=V[&save=1]` | One parameter to all 12. |
| `/v1/studio/live?id=N` | The AT32's live control-loop values (needs AT32 firmware support). |
| `/v1/studio/save?id=N` | Commit to AT32 flash. `id=0` = all four boards. |
| `/v1/studio/restore?id=N` | Factory defaults, RAM only. Follow with `save` to persist. |
| `/v1/studio/direct?id=N&m=M&deg=D&cur=C` | One servo directly. `m`: 0 idle, 1 position, 2 torque. |

Parameters (`p`): 0 reverse_position_sensor, 1 min_position_adc,
2 max_position_adc, 3 range_position_deg, 4 reverse_motor, 5 kp_position,
6 kd_position, 7 kp_current, 8 kff_current, 9 max_pwm_duty_cycle.
