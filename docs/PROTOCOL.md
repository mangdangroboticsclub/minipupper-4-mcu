# Pi link protocol

| Port | Pins | Peer | Baud |
|---|---|---|---|
| UART1 | IO18 TX, IO8 RX (J-PI pins 2 and 3; pin 1 GND) | Raspberry Pi 5 `/dev/ttyAMA0` (GPIO15 RXD, header pin 10; GPIO14 TXD, header pin 8) | 921 600 (`APP_PI_LINK_UART_BAUD`) |
| UART0 | IO43/IO44 through the CH340K (USB-C) | a PC, only with `APP_RL_USB_LINK` (otherwise the log console at 115 200) | 921 600 (`APP_RL_USB_BAUD`) |

8N1, little-endian, packed. Every frame ends with a CRC16-CCITT-FALSE (init 0xFFFF, poly 0x1021,
no reflection, no final XOR) over all bytes before it. A receiver that sees a bad CRC drops one
byte and looks for the next magic byte.

Positions are raw AT32 deci-degrees, 0..3100 for the 310° track (centre 1550), per servo id 1..12
(board `(id-1)//3`: 0 = FR (CN4), 1 = FL (CN5), 2 = RR (CN3), 3 = RL (CN6)). Currents are signed mA of the motor.

## Per-servo request / response (10 / 14 bytes)

```
request   42 cmd servo pad arg1:u16 arg2:u16 crc:u16
response  62 cmd servo status pos_dd:u16 cur_mA:i16 val:f32 crc:u16
```

| cmd | | While armed |
|---|---|---|
| 0 PING | cached feedback; `val` = raw `res` word (low 16 bits: housing NTC, 0.1 °C) | allowed |
| 2 IDLE | **disarms every servo** (any servo id) | allowed |
| 1 SET_POS, 3 TORQUE | refused (status 6): motion goes through `SET_ALL` | — |
| 4 SET_KP, 5 SET_KD | `arg1 \| arg2<<16` = float, written to RAM | refused |
| 6 GET_PARAM | `arg1` = parameter id, `val` = value | refused |
| 10 SET_PARAM | `pad` = parameter id, `arg1 \| arg2<<16` = float, RAM only; `val` = read-back | refused |
| 7 GET_LIVE | `arg1` = live id | refused |
| 8 SET_SCALE | `arg1` = the boards' position full scale in deci-degrees (101..3600, otherwise unchanged); `val` = the scale in use | refused |
| 9 RAW_REPLY | the board's next raw reply: `val` packs the count of 0xFF bytes and the first three bytes | refused |
| 11 GET_STATUS | `val` = the servo's status word | refused |
| 12 GET_OFFSET | `val` = the servo's calibration offset, degrees (status 7 if none is stored) | refused |
| 13 SET_OFFSET | `arg1 \| arg2<<16` = float offset, \|offset\| ≤ 45°, into RAM; `val` = read-back | refused |
| 14 SAVE_OFFSETS | any servo id: writes every offset set in RAM to NVS; `val` = how many | refused |

Status: 0 OK, 1 bad servo, 2 bad argument, 3 board did not answer, 5 unknown command, 6 refused,
7 no offset stored, 8 NVS unavailable or a write failed.

Calibration offsets: NVS namespace `parameter`, keys `offset1`..`offset12`,
each a 4-byte float blob in degrees, read at boot. They are only stored and served; `SET_ALL`
positions stay raw. NVS also holds other firmwares' data and is never erased: if it does not
initialise, the offset commands answer status 8.
Responses go to the port the request came from.

## SET_ALL (host → ESP32, 36 bytes)

| Offset | Field | Type |
|---|---|---|
| 0 | magic `0x43` | u8 |
| 1 | version `1` | u8 |
| 2 | seq | u16 |
| 4 | flags: bit 0 ARM, bit 1 DISARM, bit 2 CLEAR | u16 |
| 6 | enable: bit i = servo i+1 in position mode, otherwise IDLE | u16 |
| 8 | current cap, mA (in AT32 control_mode 1 a duty cap: duty ≤ min(max_pwm, kff × cap)) | u16 |
| 10 | 12 × position | u16 |
| 34 | crc | u16 |

A `SET_ALL` whose version is not 1 is ignored. Every other valid `SET_ALL` refreshes the
watchdog, records `seq`, and makes its port the `STATE` port.
- DISARM: every servo IDLE. DISARM takes precedence over ARM in the same frame.
- ARM: armed, unless a fault is latched (then `REJECTED` is latched instead). Arming clears
  `WATCHDOG` and `REJECTED`.
- CLEAR: clears every latched flag first (so CLEAR | ARM re-arms after a fault).
- Without ARM, a frame updates the targets; they move servos only while armed.

## STATE (ESP32 → host, 106 bytes, every tick = 200 Hz)

| Offset | Field | Type |
|---|---|---|
| 0 | magic `0x63` | u8 |
| 1 | version `1` | u8 |
| 2 | seq of the last `SET_ALL` received | u16 |
| 4 | tick | u32 |
| 8 | t_us: ESP32 time of the exchange whose state the feedback carries (the AT32 answers each frame with the state latched at the previous one, so this is the previous tick's time) | u32 |
| 12 | flags (below) | u16 |
| 14 | fault servo (1..12, 0 = none) | u8 |
| 15 | boards: bit b = board b gave fresh feedback this tick | u8 |
| 16 | 12 × position | u16 |
| 40 | 12 × current, mA | i16 |
| 64 | 12 × housing temperature, 0.1 °C | i16 |
| 88 | acceleration x, y, z, mg (BMI270 axes) | 3 × i16 |
| 94 | angular rate x, y, z, 0.1 °/s | 3 × i16 |
| 100 | imu_t_us: ESP32 time of the IMU read | u32 |
| 104 | crc | u16 |

Flags: bit 0 ARMED; bit 1 WATCHDOG (latched); bit 2 FAULT_CUR (latched); bit 3 FAULT_TEMP
(latched); bit 4 FAULT_BOARD (latched); bit 5 IMU_OK; bit 6 BOARD_MISSING (this tick); bit 7
REJECTED (latched); bit 8 the STATE port is USB (`APP_RL_USB_LINK`).

A servo whose board did not answer reads 0 in position, current and temperature. A board whose
`boards` bit is clear this tick reports its previous feedback (older than `t_us` says): the reply
was a config reply or the frame after one. The IMU fields read 0 and `IMU_OK` is clear when the
last good sample is older than 20 ms.

## Behaviour

- Every channel is IDLE at boot (an IDLE frame to every board before the ports open).
- Each tick sends one frame to each of the four boards: the targets of enabled servos while armed,
  IDLE for the rest.
- **Watchdog:** armed and no `SET_ALL` for 100 ms → disarm, `WATCHDOG`.
- **Guard** (only servos driven by `SET_ALL`, dwell in elapsed time): |current| above 1000 mA
  (`APP_RL_GUARD_CUR_MA`) for 0.5 s → `FAULT_CUR`; housing above 60.0 °C for 1 s → `FAULT_TEMP`
  (a backstop: the housing lags the winding); an enabled servo's board silent for more than
  20 ticks (100 ms at 200 Hz) → `FAULT_BOARD`. Each disarms and names the servo. Temperature
  readings of 150 °C or more are ignored as invalid.
- **Task watchdog:** if the tick stops for 500 ms, the ESP32 panics and reboots, and the boot
  path idles every board. The AT32 boards have no command timeout and hold the last command
  while no frames arrive, so they hold it between a stall and the reboot.
- The IMU is read in its own task (its driver waits on I2C without a timeout); the tick only
  copies the latest sample.
- The config reply that follows a parameter request, and the frame after it (which repeats the
  parameter in the board's first channel), are never reported as feedback.
- The limits are build options (`idf.py menuconfig` → Pi link).

## Two hosts

With `APP_RL_USB_LINK` both ports are live at once.
- `IDLE` from either port disarms everything; the host that armed sees `ARMED` drop (the Pi CLI
  stops with an error).
- Any `SET_ALL` moves the `STATE` stream to its port and replaces the targets and the enable
  mask, armed or not: a second host sending `SET_ALL` (`mp4link ping` does, with every servo
  disabled) takes over and, if the robot was armed, drops every servo to IDLE.

## Hosts that also drive the main firmware

The `main` branch's firmware carries the same frames (`main/pi/rl_link.cc`) inside sessions: it sends
`STATE` only after a frame from the Pi opens one, and ends it (servo gains restored, the robot back
to its own gait) when the Pi stays silent for `APP_RL_RELEASE_MS` (a `main` branch build option,
2 s by default). A host that works with both firmwares sends a frame first (a `PING` request to
servo 0) and keeps frames flowing (a `PING` when it has nothing else to send). `mp4link` does
both; this firmware answers either at once.

The `main` branch's firmware answers `GET_OFFSET` for a servo with no stored offset with status 0
and `val` 0, not status 7.
