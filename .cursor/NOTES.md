# MPX Dog firmware: how it works

Reference notes for porting this firmware's behaviour to a different code base
that drives the same hardware (MangDang MP4-ESP32-CORE quadruped). Everything
here is read out of the source in `main/`, `pwa/` and `pwa-redesign/`; file and
symbol names are given so each claim can be checked against the code.

These notes describe the `main` branch. Branch `local-claw` ports mpv2-claw
features on top (new `/v1` routes, a third app `pwa-claw/`, skill registry,
rest/wake); what changed and why is in `PORT_NOTES.md` beside this file.

Contents:

1. Big picture
2. Build, partitions, and configuration
3. Boot sequence (`main/main.cc`)
4. Hardware layer: servos, IMU, I2C (`main/robot/`, `main/platform/`)
5. Motion: gaits, IK, joystick, calibration (`main/robot/`)
6. Network: Wi-Fi, HTTP server, REST API (`main/network/`)
7. Chat pipeline, cloud link and permissions (`main/network/chat_ws.cc`)
8. Marketplace proxy (`main/network/marketplace_proxy.cc`)
9. Scripting: Lua VM and WASM skills (`main/lua/`, `main/wasm/`, `main/sdk/`)
10. Storage and NVS layout
11. The PWAs
12. Porting checklist and hardware gotchas

---

## 1. Big picture

The robot is headless. Its only UI is a Svelte PWA served by the robot itself
over Wi-Fi. The ESP32-S3 runs everything:

```
 Phone / browser (PWA)                         Cloud gateway (optional)
   |  HTTP REST  /v1/...                          ^
   |  WS  /v1/chat/ui                             |  raw WS, AES-256-GCM frames
   v                                              |  + plain HTTP for marketplace
 ESP32-S3  ------------------------------------------------------------------
  Core 0: Wi-Fi (AP+STA), httpd (prio 6), chat upstream + Lua worker (prio 8),
          IMU poll (prio 4), socket reaper
  Core 1: gait task (prio 22) -> robot::flush() -> SPI2 -> 4x AT32F413 boards
  Any:    WASM skill task (pthread, prio 4), Lua VM (PSRAM heap)
 ---------------------------------------------------------------------------
   SPI2 (12 MHz, mode 0)            I2C1 (400 kHz)
   4 driver boards x 3 servos       BMI270 (+BMM150 on its AUX bus)
```

Control paths into the motors, all ending in the same 12-entry goal buffer and
`robot::flush()`:

- PWA buttons: `POST /v1/robot/gait {"mode":"..."}` selects a named gait.
- PWA joystick: `POST /v1/robot/joy {"f","s","t"}` drives the Stanford trot.
- Calibration page: `POST /v1/robot/calibrate`, plus Servo Studio `/v1/studio/*`.
- Lua scripts (from the PWA editor or from the cloud LLM via chat): `robot.*`.
- WASM skills (`.wasm` / encrypted `.mpxe` from the marketplace): host ABI v5.

---

## 2. Build, partitions, and configuration

- ESP-IDF `>=5.4,<6.0` (built against 5.5). Target ESP32-S3, module
  ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB octal PSRAM).
- Managed deps (`main/idf_component.yml`): `espressif/wasm-micro-runtime`,
  `joltwallet/littlefs`, `espressif/lua` (5.5), `espressif2022/bmi270`,
  `espressif/i2c_bus` (header only, needed by bmi270.h),
  `tangyumei3535/bmm150_sensorapi`. A vendored `components/dong-12345__ftservo`
  (Feetech SCS driver) is left over from the Mini Pupper 2 build and unused.
- `main/CMakeLists.txt` compiles every `.cc` under `main/` and **embeds the PWA
  build output** (`EMBED_FILES`) from `../pwa-redesign/www/` or `../pwa/www/`,
  chosen by Kconfig `PWA_DESIGN`. `www/` is gitignored, so the PWA must be built
  (`npm run build` in the chosen folder) before `idf.py build` or the embed fails.
  Lua needs `LUA_32BITS=1` on the component.

`partitions.csv`:

| Name | Type | Offset | Size |
|---|---|---|---|
| nvs | data/nvs | 0x9000 | 0x6000 |
| phy_init | data/phy | 0xF000 | 0x1000 |
| factory | app | 0x10000 | 0x280000 (2.5 MB) |
| storage | data/littlefs | 0x290000 | 0xD70000 (~13.4 MB) |

`sdkconfig.defaults` (all load-bearing):

- `CONFIG_LWIP_MAX_SOCKETS=16`, `CONFIG_LWIP_TCP_MSL=10000`,
  `CONFIG_LWIP_SO_LINGER=y` - socket pool exhaustion was the main stability bug
  (PWA polling + marketplace proxy + WS clients). See section 6.
- Octal PSRAM at 80 MHz, `SPIRAM_USE_MALLOC`, `MALLOC_ALWAYSINTERNAL=16384`,
  `RESERVE_INTERNAL=32768`. WASM linear memory (128 KB) and the Lua heap live in
  PSRAM; without PSRAM every skill fails to instantiate.

Kconfig menu "MPX Dog App Configuration" (`main/Kconfig.projbuild`):

| Symbol | Default | Meaning |
|---|---|---|
| `PWA_DESIGN_REDESIGN` / `_ORIGINAL` | redesign | which PWA gets embedded |
| `APP_LITTLEFS_PARTITION` | `storage` | LittleFS partition label |
| `APP_AP_SSID` / `_PASSWORD` / `_CHANNEL` / `_MAX_CONN` | `MPX-Dog` / open / 6 / 4 | soft-AP defaults (NVS overrides) |
| `APP_STA_AUTO_CONNECT` | y | rejoin saved STA network at boot |
| `APP_CHAT_SERVER_IP` | `echo` | cloud gateway host; `echo` = no cloud, local echo |
| `APP_CHAT_SERVER_PORT` | 8080 | gateway port (chat WS **and** marketplace HTTP) |
| `APP_CHAT_WS_PATH` | `/v1/chat/ingress` | upstream WS path |
| `APP_ROBOT_UUID` | `MPX-DOG-01` | robot id: frame AAD, marketplace routing, MPXE AAD |
| `APP_CHAT_AES_KEY_HEX` | `00..01` | AES-256 root key (64 hex chars) |
| `APP_MPX_WASM_ENCRYPTION` | y | allow `.mpxe` decrypt |
| `APP_MPX_EFUSE_BLK` | 0 | eFuse key block (not implemented; key always from Kconfig) |
| `APP_LUA_ENABLE`, `APP_LUA_HEAP_SIZE` | y, 64 KB | Lua VM |
| `APP_IMU_LOG_*` | on, every 50 reads | IMU console spam control |
| `APP_SERVO_MOTOR_*`, `APP_SERVO_BAUD_RATE` | - | legacy UART servo bus, unused |

---

## 3. Boot sequence (`main/main.cc`)

Strictly ordered; an early failure returns from `app_main` and leaves later
subsystems down:

1. `nvs_flash_init()` (erase + retry on version/no-pages error).
2. `robot::init()` - NVS namespace `robot`, 1 s wait for the driver boards to
   boot, SPI bus + 4 devices, probe the boards' angular range, probe all 12
   channels, restore config/offsets/current caps from NVS, `imu_init()`, spawn
   the gait task (core 1, prio 22, 8 KB stack). Failure is logged, boot goes on.
3. `fs::init_littlefs()` - mount `storage` at `/fs` (formats on mount failure).
   Fatal if it fails.
4. `network::init_wifi_ap()` - netif + event loop + Wi-Fi in `APSTA` mode, AP
   at static `192.168.2.1/24`. Fatal.
5. `network::init_wifi_sta()` - STA netif; auto-connects if NVS has credentials.
6. `network::deploy_www_assets()` - copies the embedded PWA files into
   `/fs/www/` **every boot** (so a reflash always updates the UI). Fatal.
7. `network::start_http_server()` - port 80, core 0, prio 6. Fatal.
8. `network::init_chat_pipeline()` - AES key, queues, Lua worker + upstream task.
9. `wasm::init_sandbox()` - WAMR runtime in PSRAM, registers host ABI. Fatal.
10. `lua_init()` - Lua state in PSRAM, registers `robot`, `fs`, `wasm`, `crypto`, `display`.

---

## 4. Hardware layer

### 4.1 Pin map (`main/robot/board_config.h` - single source of truth)

| Function | Pins |
|---|---|
| Servo SPI2 | MOSI=IO6, MISO=IO17, CLK=IO16 |
| Servo chip selects (`SERVO_CS_MAP 1`) | FR=IO7 (CN4), FL=IO4 (CN5), RR=IO15 (CN3), RL=IO5 (CN6) |
| Servo power enable | **none** - CN3..CN6 sit on Vbat+ (`SERVO_HAS_POWER_EN 0`) |
| I2C (shared IMU + ES7210 codec) | port 1, SDA=IO1, SCL=IO2, 400 kHz, 1k pull-ups on board |
| BMI270 / BMM150 | 0x68 on host bus / 0x10 on the BMI270 AUX bus; INT1=IO10 |
| Pi 5 header J3 (unused) | UART1 TX=IO18, RX=IO8; spare IO3, IO11 |
| Buttons (unused) | BOOT=IO0, WAKE=IO21 |
| Claimed, not driven here | LCD (IO9,39,40,41,42), I2S/audio (IO12,13,14,38,45) |
| Console | UART0 IO43/IO44 via CH340 |

`SERVO_CS_MAP` 2 and 3 are alternative leg-to-connector maps; which one is right
depends on how the loom was routed, so it must be verified on the bench (see
`docs/NEW_BOARD_BRINGUP.md`).

### 4.2 Servo driver boards (`main/robot/driver_board.{h,c}`)

Four AT32F413 boards, one per leg, three servos each, on one SPI bus with four
chip selects. Board index (= SPI device) order: 0=FR (servos 1-3), 1=FL (4-6),
2=RR (7-9), 3=RL (10-12). Within a leg: abduction/hip-yaw, shoulder, knee.

**Wire format** (packed, little-endian, every frame 36 bytes, `check_sum` sent
as 0 and never checked):

```c
// host -> AT32
typedef struct { uint16_t mode, position; int16_t torque; uint16_t kp, kd; } servo_cmd_sub_t;
typedef struct { uint16_t start /*0xA5A5*/, mode /*0x0001*/;
                 servo_cmd_sub_t s1, s2, s3; uint16_t check_sum; } host_SMS_t;
// AT32 -> host (clocked back on the same transaction)
typedef struct { uint16_t status, position; int16_t torque; uint16_t res1, res2; } servo_fb_sub_t;
typedef struct { uint16_t start /*0xFEED*/, status /*0x0003 = feedback*/;
                 servo_fb_sub_t s1, s2, s3; uint16_t check_sum; } SMS_host_t;
```

- Per-servo `mode`: 0 idle, 1 position, 2 torque. In position mode `torque` is a
  **max current limit in mA**, not a speed. There is no speed field at all.
- `position` is in AT32 deci-degrees, 0..`scale_dd` (default 3100 = 310.0 deg,
  centre 1550 in deci-deg; Servo Studio's "135 = centre" note in the code is a
  leftover from 270-deg boards).
- `kp`/`kd` are sent as 0, meaning "use the board's stored gains"; stock AT32
  firmware ignores per-frame gains anyway.
- Feedback: `position` (same units), `torque` = present current (mA, signed),
  `res1` = NTC temperature, signed, 0.1 degC. Valid only if
  `start == 0xFEED && status != 0xC0DE`.

**Config frames** (`start = 0xC0DE`), same 36-byte size:

```c
typedef struct { uint16_t start, op, servo_index /*0..2 on that board*/, param_id;
                 float value; uint8_t padding[24]; } cfg_frame_t;
```

ops: 0 NOP, 1 SET, 2 GET, 3 SAVE (to AT32 flash), 4 RESTORE (factory, RAM only),
5 GET_LIVE. The reply comes back on the **next** transaction, so every request is
followed by NOPs (up to 6 tries, 600 us apart) until a reply with
`status == 0xC0DE` and `s1.position == param_id` arrives; the float is packed
across `s1.res1/res2`. Rules learned the hard way:

- A request and its NOP must never be split by another frame, otherwise the
  pending reply is decoded as feedback (a 31.03 degC reply reads as 1540.8 degC).
  One mutex covers each whole public operation, and the gait is parked
  ("studio mode" or a skill lock) during config traffic.
- Keep at least 2 ms between config ops on the same board, or it stops answering.
- After 3 consecutive failures a board is "muted" (fail fast) and re-probed every 5 s.

Parameters: 0 reverse_position_sensor, 1 min_position_adc, 2 max_position_adc,
3 range_position_deg, 4 reverse_motor, 5 kp_position, 6 kd_position,
7 kp_current, 8 kff_current, 9 max_pwm_duty_cycle. Live values (GET_LIVE): pos_adc,
cur_adc, setpoint/present/error deg, max/setpoint/present/error mA, pwm duty,
mode, loop counter, temperature.

**Scale probe.** At boot `driver_board_probe_scale()` reads `range_position_deg`
from servo 1, 4, 7, 10 (first sane answer wins) and adopts it. Boards keep this
in their own flash and the reference firmware's `guard` command may have trimmed
it (e.g. to 300). Assuming the wrong range scales every commanded angle.

**Logical vs physical channels.** `db_phys()` remaps logical servo ids to
physical channels for alternative board builds (`SERVO_BOARD` 1 = identity,
the black board; 2 = board 2, the default: FR on CN3 and RR on CN4, groups
1-3 <-> 7-9; 3 = "purple board" cross-leg swap). It is applied
only at the SPI boundary so gait, calibration and feedback all agree.

**Two raw position frames** (`robot.h`): the gait frame (0..1023, 511 centre,
positive deg = larger value) and the AT32 frame. `driver_board_sync_write()`
converts with a flip: `at32_dd = scale_dd - gait_raw * scale_dd / 1024`. The
feedback cache stores `at32_dd * 1024 / scale_dd` **without** flipping back, so
`at32_raw == 1024 - gait_raw`. Anything comparing commanded vs measured must
convert first (`read_angle_cdeg()` does).

### 4.3 IMU (`main/robot/imu.cc`, `main/platform/shared_i2c.cc`)

- One shared `i2c_master` bus handle (`hal::i2c_bus()`), created on first use,
  because ESP-IDF allows one handle per port and the ES7210 codec sits on the
  same pins.
- Driven through the Bosch BMI270 Sensor API (the chip needs its 8 KB config
  blob uploaded). Accel 100 Hz, +/-2 g; gyro 100 Hz, +/-2000 dps. BMM150 read
  through the BMI270 AUX interface at 100 Hz; the BMI270's internal 2 k AUX
  pull-ups must be enabled (no external pull-ups on ASDX/ASCX).
- A task (core 0, prio 4) polls at 20 Hz and caches `ImuData {ax,ay,az (g),
  gx,gy,gz (dps), mx,my,mz (uT), mag_valid}`. A dead magnetometer leaves accel
  and gyro working. The IMU is read-only telemetry: no gait uses it for balance.

---

## 5. Motion (`main/robot/robot.cc`, `stanford_gait.*`, `stanford_kinematics.*`)

### 5.1 The control loop

- State: `s_goal_pos[13]` (gait frame, index 1..12), `s_current_ma[13]`
  (current cap, default `SERVO_CURRENT_MAX_MA = 1500`, persisted as `cur<N>`),
  `s_offset[13]` (calibration degrees), `s_overlay[13]` (additive trim from
  skills, +/-20 deg, never stored).
- `set_servo_angle(id, deg)`: `goal = 511 + deg * ticks_per_deg`, clamped
  0..1023, where `ticks_per_deg = 10240 / scale_dd`. Only fills the buffer.
- `flush()`: returns immediately if Studio or a skill owns the bus; otherwise
  paces to at least 5 ms between writes, adds the overlay, and calls
  `driver_board_sync_write(pos, cur)` (4 frames, ~200 Hz max).
- Bus ownership (`s_bus_owner`): FREE, STUDIO (human, via `/v1/studio/mode?on=1`)
  or SKILL (WASM `servo_lock`). Studio outranks a skill. While not FREE the
  robot just holds its last pose.

### 5.2 Gait task (core 1)

`send_gait_cmd()` writes a single volatile `GaitCmd`; the task loop reads it,
runs that motion, and each inner loop checks `s_gait_cmd != cmd` to abort early.

- `None` (idle): re-solves the neutral stand at `cfg.height` every tick and
  flushes (unless a WASM skill is running or owns the pose).
- One-shot motions reset to `None` when done (init, roll, pitch, stretch,
  jumps, front kick, ...). Continuous ones (step, advance/back/left/right,
  turns, stanford walk, wiggle/shrug holds) run until another command arrives.
- Legacy motions use the planar per-leg IK (`front_right_ik(x, th0, z)` etc.)
  with sinusoidal phase profiles over `cfg.period` ms.
- Stanford-IK motions (walk, look poses, leg lifts, height, balance, bow,
  body cycle, head ellipse, FPC choreography, body attitude) build four foot
  targets `sg_foot_t {x fwd, y left, z = body height above foot}` (FR, FL, RR,
  RL), optionally rotate them by roll/pitch/yaw (`sg_attitude_feet`), and run
  them through `stanford_kinematics_servo_deg()`. Transitions are sin-eased
  ramps (`sg_ramp_to`, duration `clamp(period*3, 250, 1500)` ms) and end with
  `sg_park()` back to the stand.

Gait names (the only table is `GAIT_NAMES` in `robot.cc`; HTTP, Lua and WASM
all use it): `none init step roll pitch stretch advance back left right turnL
turnR twerk jump jumpfwd testspeed lookup lookdown lookleft lookright lookul
lookur lookll looklr flegL flegR blegL blegR heightup heightdown balance bowback
bodycycle headellipse moveLF moveRF moveLB moveRB stanford frontkick wiggle
buttshrug wiggleL wiggleR buttshrugL buttshrugR`. `BodyAttitude` has no name; it
is entered via `set_body_attitude(roll, pitch, yaw)` (clamped +/-25, +/-20,
+/-30 deg) with optional per-axis slew rates.

`Config` (persisted in NVS `robot`): `period` 80 ms, `height` 70 mm,
`up_height` 10 mm, `stride` 10 mm, `tilt` 10 deg, `sg_speed` 50 mm/s (10..200).

### 5.3 Stanford trot and the joystick

- `stanford_gait.c` is a port of StanfordQuadruped's gait scheduler with the
  Mini Pupper 2 Pro constants: `dt = 15 ms`, overlap 6 ticks, swing 6 ticks,
  phase length 24 ticks, swing clearance 20 mm. Input `(vx mm/s, vy mm/s,
  wz rad/s)`.
- `joy_input(f, s, t)` (each -1..1, dead-zone 0.08): any input auto-starts
  `StanfordWalk` and timestamps it. In the walk loop, fresh input (<600 ms old)
  gives `vx = f*sg_speed`, `vy = -s*200*k`, `wz = t*2.0*k` with
  `k = sg_speed/200`; then `SG_MIRROR_LR` negates vy and wz. Stale input from a
  joystick-started walk steps in place; a button-started walk trots forward at
  `sg_speed`. Leaving the walk parks the feet in the stand.

### 5.4 Kinematics and calibration

- Planar IK (`calculate_ik`): L1=50, L2=56 mm. Angles are commanded relative to
  the neutral stand at `NEUTRAL_Z = 70` mm, so all servos centred == standing.
  Per-leg signs: FR `(+th0, -dth1, +dth2)`, FL `(+th0, +dth1, -dth2)`, RR like
  FR, RL like FL.
- Stanford IK (`stanford_kinematics.cc`): exact BSP geometry L1=50, L2=60,
  abduction offset 26, leg origins x=+/-59, y=+/-23.5 mm; gait origin y=49.5.
  Output = (joint angle - neutral joint angle at 70 mm) x `SK_SIGN[axis][leg]`:
  abduction `{-1,-1,+1,+1}`, hip `{+1,-1,+1,-1}`, knee `{+1,-1,+1,-1}` (FR, FL,
  RR, RL). The abduction row has never been verified on hardware.
  `SK_NEUTRAL_HEIGHT_MM` must equal `NEUTRAL_Z`.
- Offsets: degrees, added inside the IK to each servo. Stored as float blobs
  `offset1..offset12` in NVS `robot`. Seeded once from `kFactoryOffset[]` (all
  zero = uncalibrated) keyed by `calseed`; if all are zero the firmware imports
  `offset<N>` from the reference firmware's NVS namespace `parameter`.
  Printed every boot.

### 5.5 Feedback reads

All served from the cache refreshed by each `sync_write` (free while the gait
runs; stale when parked unless `driver_board_poll*()` is called):
`read_position` (AT32 frame 0..1023), `read_angle_cdeg` (gait frame,
centidegrees), `read_current`/`read_load` (mA), `read_temperature[_c]` (NTC),
`read_moving` (|measured - goal| > 2 counts, after frame conversion),
`ping_servo` (one param read). `read_speed` and `read_voltage` always return -1
on this hardware.

---

## 6. Network: Wi-Fi and HTTP server

### 6.1 Wi-Fi (`wifi_ap.cc`, `wifi_sta.cc`)

- Always `WIFI_MODE_APSTA`. AP: SSID/password from NVS namespace `wifi_ap`
  (falls back to Kconfig), static `192.168.2.1` with DHCP server, channel 6,
  4 clients, open if the password is empty else WPA2-PSK.
  Changing it via `POST /v1/wifi/ap-config` saves to NVS and **reboots** after
  500 ms.
- STA: credentials in NVS namespace `wifi_sta` keys `ssid`/`password`.
  `wifi_sta_connect()` stops Wi-Fi, reconfigures, restarts (the AP drops
  briefly). States: disconnected, connecting, connected (got IP), failed (auth
  fail / no AP found / handshake timeout). Cloud features need STA connected.

### 6.2 HTTP server (`http_server.cc`)

`esp_http_server` on port 80, core 0, prio 6, 8 KB stack, wildcard URI matching,
`max_uri_handlers = 64` (53 used; registration failures are counted and logged
because they silently drop the last-registered routes), `max_open_sockets = 5`
with `lru_purge_enable`, `recv_wait_timeout 60 s`, `send_wait_timeout 30 s`,
`close_fn = pwa_release_fd`. A `sock_reaper` task (every 4 s) closes port-80
sockets whose peer has gone and zombie fds with no PCB.

Request bodies are parsed with hand-rolled `strstr` helpers, not a JSON parser:
keys must appear as `"key":"value"` / `"key":123` with no space after the colon
for strings. A replacement firmware with a real JSON parser is strictly more
permissive, which is fine.

**Static files.** `/`, `/index.html`, `/m.js`, `/index.css`, `/manifest.json`,
`/sw.js`, `/favicon.ico` (mapped to the design's SVG), plus `/md.svg`,
`/eye-open.svg`, `/eye-close.svg` (redesign) or `/icon.svg` (original). Served
from `/fs/www`, `.gz` variant first if the client sends `Accept-Encoding: gzip`,
unknown paths fall back to `index.html` (SPA routing), then 404.

### 6.3 REST API reference

All JSON. "Q" = query string, "B" = JSON body.

Robot control:

| Method + path | Input | Response |
|---|---|---|
| `POST /v1/robot/gait` | B `{"mode":"<gait name>"}` | `{"ok":true,"mode":"<name>"}` (unknown name -> `none`) |
| `POST /v1/robot/joy` | B `{"f":-1..1,"s":-1..1,"t":-1..1}` (s,t: + = left) | `{"ok":true}` |
| `GET /v1/robot/status` | - | `{"mode","config":{period,height,up_height,stride,tilt,sg_speed},"offsets":[12 floats]}` |
| `GET /v1/robot/config` | - | `{period,height,up_height,stride,tilt,sg_speed}` |
| `POST /v1/robot/config` | B any subset of the six config ints | `{"ok":true}` (persisted) |
| `POST /v1/robot/calibrate` | B `{"servo":1..12,"offset":deg}` | `{"ok":true,"servo","offset"}` (persisted) |
| `POST /v1/robot/calibrate/reset` | - | `{"ok":true}` |
| `POST /v1/robot/diagnostic/ping` | B `{"servo":N}` | `{"ok","servo","responding"}` |
| `POST /v1/robot/diagnostic/sweep` | B `{"servo":N}` | blocks ~2.4 s sweeping +/-45 deg at 600 mA in studio mode, then `{"ok":true}` |
| `GET /v1/trace` | Q `since`, `max` (<500) | `{"next":N,"samples":[...]}` from `mpx_trace()` in skills (ring of 256) |

Servo Studio (all `GET`, `Cache-Control: no-store`; parameter routes return
`{"ok":false,"err":"studio mode off ..."}` unless studio mode is on):

| Path | Q | Response |
|---|---|---|
| `/v1/studio/mode` | `on=0|1` (optional) | `{"ok":true,"studio":bool}` |
| `/v1/studio/status` | - | `{"ok","studio","servos":[{"id","scs","deg","ma","c"}x12]}` (no mode needed) |
| `/v1/studio/temps` | - | `{"ok","studio","temps":[{"id","c"}]}` (no mode needed) |
| `/v1/studio/scan` | - | `{"ok","found":[ids]}` |
| `/v1/studio/dump` | `id` | `{"ok","id","params":[{"p","n","v"}]}` |
| `/v1/studio/set` | `id,p,v` | `{"ok","p","v"}` (read back) |
| `/v1/studio/setall` | `p,v[,save=1]` | `{"ok","p","v","n","fail":[],"saved"}` |
| `/v1/studio/live` | `id` | `{"ok","full":true,pos_adc,cur_adc,set_deg,now_deg,err_deg,cap_ma,set_ma,now_ma,err_ma,duty,mode,loop,temp_c}` or reduced `full:false` |
| `/v1/studio/save` | `id` (0 = all boards) | `{"ok"}` |
| `/v1/studio/restore` | `id` (0 = all) | `{"ok"}` (RAM only) |
| `/v1/studio/direct` | `id,m(0/1/2),deg,cur` | `{"ok"}` - raw AT32 degrees |

Wi-Fi:

| Method + path | Input | Response |
|---|---|---|
| `GET /v1/wifi/status` | - | `{"ap":{"ssid","ip"},"sta":{"state","ssid","ip"}}` |
| `POST /v1/wifi/connect` | B `{"ssid","password"}` | `{"ok":true}` (attempt started, poll status) |
| `POST /v1/wifi/disconnect` | - | `{"ok":true}` |
| `POST /v1/wifi/forget` | - | `{"ok":true}` |
| `POST /v1/wifi/ap-config` | B `{"ssid","password"}` | `{"ok":true,"restarting":true}` then reboot |

Files and skills (paths are relative to the LittleFS root `/fs`):

| Method + path | Input | Response |
|---|---|---|
| `GET /v1/skills/list` | - | `[{"name":"x.wasm","size":N}]` (`.wasm` and `.mpxe`, excludes `/www`) |
| `POST /v1/skills/upload` | Q `name=x.wasm`, raw binary body (<=256 KB) | `{"ok":true,"path"}` |
| `POST /v1/skills/run` | B `{"skill":"x.wasm","params":"a=1;b=2"}` | `{"output":"started"}`; 409 if one is running; runs async, 60 s watchdog |
| `GET /v1/fs/list` | Q `path` (default `/`) | `{"f":[{"n","s","r"(read-only)}],"d":[dir names]}` |
| `GET /v1/fs/read` | Q `path` | `{"ok":true,"p","c"}` (text content; `c:""` for empty files) |
| `POST /v1/fs/write` | B `{"path","content"}` (body <=32 KB) | `{"ok":true,"p","s"}`; overwrites an existing file only; `/www` never writable |
| `GET /v1/fs/info` | - | `{"total","used"}` |
| `POST /v1/fs/delete` | B `{"path","sudo":bool}` | `{"ok":true}`; `/www` never deletable; without sudo only `.wasm/.mpxe/.lua` |

Lua:

| Method + path | Input | Response |
|---|---|---|
| `POST /v1/lua/run` | B `{"script"}` or `{"path"}` | `{"ok":true,"output"}` / `{"ok":false,"error"}` - synchronous, 5 s timeout (60 s for `path`), 2 KB output. A `path` run gets its own globals, `args = {}`, and `require` from its folder (see `lua_run_file` in `main/lua/lua_vm.h`) |
| `POST /v1/lua/enqueue` | B `{"script"}` | `{"ok":true}`; written to `/lua/_deploy_N.lua` and run by the chat Lua worker (60 s), results arrive on the chat WS |
| `GET /v1/lua/list` | - | `{"ok":true,"files":[...]}` (under `/lua`) |
| `GET /v1/lua/read` | Q `name` | `{"ok":true,"name","code"}` |
| `POST /v1/lua/save` | B `{"name","code"}` | `{"ok","path"}` |
| `POST /v1/lua/delete` | B `{"name"}` | `{"ok"}` |

| Method + path | Input | Response |
|---|---|---|
| `GET /v1/agent/permissions` | - | `{"ok":true,"allow_writes":bool}` |
| `POST /v1/agent/permissions` | B `{"allow_writes":bool}` | same; saved in NVS (`agent_perm/allow_writes`). While on, `network::request_permission` (Lua/WASM `fs.write`/`fs.delete`) approves at once and broadcasts `approved` instead of `pending` |

The on-device agent runs scripts on the same VM through its `lua_run_script`
tool (`main/agent/agent_lua.cc`, group `cap_mpx_lua`, not offered to the voice
gateway), with `args` from the call. Scripts reach the agent's robot tools with
`tools.call(name, args)` (`main/lua/lua_tools.cc`). Agent skill scripts live in
`/agent/skills/<id>/scripts/`, so the Files page can open, edit and run them;
`main/skills/mpx_skill_creator` teaches the agent this layout.

Chat and cloud:

| Method + path | Input | Response |
|---|---|---|
| `WS /v1/chat/ui` | text JSON frames | see section 7 |
| `POST /v1/chat/send` | B `{"type":"user_chat_input","text","session_id"}` | echo: `{"type":"chat_reply","text","commands":[],"session_id"}`; cloud: waits up to 30 s for the reply, else `{"type":"ack","status":"sent"}` |
| `GET /v1/gateway/config` | - | `{"host","port","robot_uuid"}` |
| `GET/POST/PATCH/DELETE /v1/marketplace/*` | passthrough | see section 8 |
| `WS /v1/telemetry/stream` | - | placeholder, echoes text frames |

---

## 7. Chat pipeline (`chat_ws.cc`, `crypto.cc`)

Two modes, chosen at compile time by `APP_CHAT_SERVER_IP`:

- **Echo** (`"echo"`): every user message is returned as a `chat_reply`. No
  cloud, no Lua from chat.
- **Cloud**: an `upstream_task` (core 0, prio 8) waits for STA, then opens a
  raw TCP WebSocket (no TLS) to `ws://HOST:PORT/WS_PATH`, client-masked frames,
  fixed `Sec-WebSocket-Key`, sends ping every 15 s / 30 s idle, answers pings
  with matching pongs, reconnects every 5 s on failure.

**Upstream frame** (binary WS message, both directions):

```
[0:16]   robot_uuid, ASCII, zero-padded      (also the GCM AAD)
[16:28]  12-byte random IV
[28:-16] AES-256-GCM ciphertext of a UTF-8 JSON object
[-16:]   16-byte GCM tag
```

Key = `APP_CHAT_AES_KEY_HEX`. The robot forwards the PWA's JSON as-is (it
encrypts whatever text the PWA sent).

**PWA <-> robot WebSocket `/v1/chat/ui`** (up to 4 clients; a new client is
refused when all 4 are alive, dead ones are reclaimed; no app-level pings
because browser pongs confuse the ESP-IDF WS parser; TCP keepalive 10/3/3 and a
5 s send timeout instead; fragmented text frames are reassembled up to 4 KB):

PWA -> robot:

- `{"type":"user_chat_input","text":"...","session_id":"...","ts":...}` -
  forwarded upstream; robot replies `{"type":"ack","status":"sent"}` or
  `{"type":"error","text":"Upstream not available"}`. The `session_id` seen on
  a socket binds that client to the session for broadcasts.
- Any message containing `"session_reset"` - forwarded upstream, acked with
  `{"type":"ack","status":"session_reset"}`.
- `{"type":"permission_response","action_id":"...","approved":true|false}` -
  acked with `{"type":"ack","status":"permission_received"}`.

Robot -> PWA (broadcast to all clients not bound to a different session):

- `{"type":"chat_reply","text","commands":[],"session_id"}`; long replies as
  `{"type":"chat_reply_chunk","text","seq","total","session_id"}` (1536-byte
  pieces, `seq` from 0) for the PWA to reassemble.
- `{"type":"step","text","seq","total","session_id"}` - progress, either
  relayed from the cloud or emitted before each Lua command.
- `{"type":"command_result","script","output","status":"completed|timeout|error","session_id"}`.
- `{"type":"openclaw_action","action_type","description","status","action_id"?,"session_id"}`
  - status `pending` (with `action_id`, the PWA must answer with
  `permission_response`), then `approved` / `denied` / `timeout`, or
  `completed` / `failed` for plain telemetry.

Cloud -> robot (decrypted JSON):

- `{"type":"step",...}` - relayed to the PWA.
- `{"type":"chat_reply","text","session_id","commands":[{"type":"lua","script":"..."}]}`
  - text goes to the PWA (and to a waiting `POST /v1/chat/send`); each Lua
  command (max 1023 chars) is queued (depth 8) to the `chat_lua` worker (core 0,
  prio 8), which runs it (5 s for inline strings, 60 s for files), sends `step`
  and `command_result` to the PWA, and sends the output back upstream as
  `{"type":"user_chat_input","text":"LUA: <output>","session_id","ts"}` so the
  LLM sees the result.

**Permissions.** `request_permission(type, desc, timeout)` broadcasts a pending
`openclaw_action` with id `perm_<us>_<rand>` and blocks on a semaphore until the
matching `permission_response` or the timeout. Only one request can be pending.
Lua `fs.write` and `fs.delete` use it with a 60 s timeout.

**The local brain (branch `local-claw`, Phase 5).** A PWA frame carrying
`"brain":"local"` bypasses the cloud: `user_chat_input` goes to the on-device
ESP-Claw agent (`main/agent/agent_runtime.cc`, `agent::submit`) with the
conversation id as the agent's session; the agent's progress notes come back
as `step` frames and its answer as `chat_reply`/`chat_reply_chunk` through
the same broadcast. Frames without a brain are the cloud path above, so an
older PWA is unaffected. Replies from either brain are chunked on UTF-8
boundaries by `send_reply_to_pwa()`. Details and the reasoning in
`PORT_NOTES.md`, "Phase 5 changes".

---

## 8. Marketplace proxy (`marketplace_proxy.cc` + `api_marketplace_proxy`)

The PWA never talks to the cloud directly for the marketplace; it calls
`/v1/marketplace/*` on the robot, which forwards plain HTTP/1.1
(`Connection: close`, no TLS) to `APP_CHAT_SERVER_IP:APP_CHAT_SERVER_PORT`:

| Robot path | Gateway path |
|---|---|
| `/v1/marketplace/skills[/...]` | `/v1/skills[/...]` |
| `/v1/marketplace/orders[/...]` | `/v1/orders[/...]` |
| `/v1/marketplace/robot/<rest>` | `/v1/robots/<ROBOT_UUID>/<rest>` (skills, deploy, checkout, orders, ...) |

- Bodies up to 4 KB are forwarded for POST/PATCH; responses are buffered whole.
- Max 3 concurrent gateway requests (counting semaphore, 8 s wait, then shed);
  4 s connect timeout, 10 s I/O timeout, `SO_LINGER {1,0}` to avoid TIME_WAIT.
- GET responses (except `/v1/orders...`) are cached 15 s in 4 slots; any
  non-GET invalidates the cache.
- Errors: unreachable -> `502 "gateway unreachable"`; non-2xx -> 502 with the
  gateway's body. Echo mode or no STA -> 502.

---

## 9. Scripting

### 9.1 Lua (`lua/lua_vm.cc`, `lua/lua_bindings.cc`)

Lua 5.5, one global state with a PSRAM allocator, `luaL_openlibs` plus:

- `robot.gait(name)`, `get_mode()`, `get_config()`, `set_config(p,h,u,s,t)`,
  `set_servo_angle(id,deg)`, `set_servo_speed` / `set_all_servo_speed`
  (stored, never sent), `set_servo_current(id,mA)`, `get_servo_current(id)`,
  `flush()`, `read_position/angle_cdeg/speed/load/voltage/temperature/moving/current(id)`,
  `ping(id)`, `set_offset/get_offset/reset_offsets`, `delay_ms(ms)`,
  `ik_fr/ik_fl/ik_rr/ik_rl(x,th0,z)`, `imu_read()` -> table, `imu_print()`.
- `fs.read/write/delete/exists/list/info` (write and delete ask the PWA for
  permission), `wasm.run(path, fn?)`, `wasm.run_bytes(data, fn?)`,
  `crypto.base64_encode/decode`.
- `display.clear/pixel/rect/circle/line/arc/text/blit/show/face/width/height/rgb`
  (`lua/lua_display.cc` over `display/cap_display.c`): paint the full-screen
  canvas the agent's `display_draw` tool uses, `show(hold_ms)`, and the face
  returns after the hold. The API is documented in `lua_bindings.h`.
- `print` is redirected into the caller's output buffer; a runtime error's
  message is appended to it too.
- Runs are serialised (one mutex; a second caller waits up to 10 s, then
  gets a "busy" error) and the caller's timeout is enforced by a count hook
  every 1000 instructions plus a check in each `robot.delay_ms` chunk;
  `fs.write`/`fs.delete` add the permission-prompt wait back onto the
  deadline. A timeout returns `ESP_ERR_TIMEOUT`. Callers: `/v1/lua/run`
  (5 s inline, 60 s for a saved file) and the chat worker (5 s / 60 s).

### 9.2 WASM skills (`wasm/wasm_sandbox.cc`, `wasm/wasm_decrypt.cc`, `sdk/`)

- WAMR runtime, all WAMR calls on a pthread (WAMR's ESP-IDF port requires it).
  Per run: 32 KB stack, 128 KB linear memory, watchdog (60 s from
  `/v1/skills/run`), only one skill at a time.
- Lifecycle: `on_start()` (or `_start`) -> optional `on_tick()` loop if
  `mpx_tick_every(ms)` was called (10..1000 ms) -> `on_stop(reason)` with
  0 = normal, 1 = trapped, 3 = cancelled. Per-run state (params, trace ring,
  control claims, gain bookkeeping, overlay, skill bus lock) is reset/cleared
  around each run.
- Host ABI (module `env`, `MPX_ABI_VERSION = 5`, table `NATIVE_SYMBOLS` in
  `sdk/wasm_host_functions.h`): `print`, the `robot_*` set mirroring the Lua
  API plus `robot_set_body_pose(fff)` and attitude slew rates, low-level
  `servo_lock/unlock/stage/commit/write_all/read/read_all/poll/direct/scan/set_gain/get_gain/save_config/restore_config`,
  and `mpx_*` helpers (`millis`, `sleep_until`, `drive`, `drive_stop`,
  `set/get_walk_speed`, `foot`, `param_f/i`, `overlay*`, `tick_*`, `trace`,
  `control_take/release/owner`, `read_temperature_c`, `board_scale_dd`).
- **MPXE encrypted skills** (`.mpxe`, detected by magic `"MPXE"`):

  ```
  0   "MPXE"
  4   version 0x01
  5   wrap IV (12)
  17  wrapped skill key: 32-byte ciphertext + 16-byte tag
  65  key_algo (0x00)
  66  skill id hash (first 8 bytes of SHA-256)
  74  wasm IV (12)
  86  encrypted wasm ... + 16-byte tag
  ```

  Skill key = GCM-decrypt(robot key, wrap IV, AAD `"wasm-wrap:v1:" + ROBOT_UUID`).
  WASM = GCM-decrypt(skill key, wasm IV, AAD `"wasm-skill:v1:" + hex(hash8)`).
  So a blob only opens on the robot whose UUID and root key it was built for.

---

## 10. Storage and NVS layout

LittleFS mounted at `/fs` (partition `storage`):

- `/www/*` - PWA assets, rewritten from the firmware every boot, read-only to the API.
- `/*.wasm`, `/*.mpxe` - uploaded / deployed skills.
- `/lua/*.lua` - saved scripts; `/lua/_deploy_N.lua` - temporary enqueued scripts.

NVS namespaces:

| Namespace | Keys |
|---|---|
| `robot` | `period height upHeight stride tilt sgspeed` (i32), `offset1..12` (float blob), `cur1..12` (i32 mA), `calseed` (i32) |
| `parameter` | read-only import of `offset1..12` from the reference minipupperesp firmware |
| `wifi_sta` | `ssid`, `password` |
| `wifi_ap` | `ssid`, `password` (custom AP; empty password = open, else WPA2-PSK) |

---

## 11. The PWAs

> **Since local-claw Phase 6** there is one app, `pwa-claw/` (mpv2-claw's,
> see `PORT_NOTES.md`); `pwa/`, `pwa-redesign/` and `PWA_DESIGN` were
> removed. The rest of this section describes those two older apps.

Two Svelte 5 + Vite + Tailwind apps talk to the same firmware API. Kconfig
`PWA_DESIGN` picks which one is embedded; **the default is `pwa-redesign/`**.

| | `pwa/` (original) | `pwa-redesign/` (default) |
|---|---|---|
| Theme | dark, utilitarian | light, consumer |
| Extra assets | `icon.svg` | `md.svg`, `eye-open.svg`, `eye-close.svg` |
| Unique features | Lua editor (CodeMirror + autocomplete), WASM skill list, servo ping/sweep diagnostics | lock/welcome flow, virtual gamepad, 3D calibration sketch, AP config, paid checkout, custom/pinned actions |

### 11.1 Build and delivery

- `npm run build` = `vite build && node gzip-www.mjs`. Vite root is `src/`,
  public dir `public/`, output `www/` (not `dist/`). Entry is renamed `m.js`,
  CSS `index.css`, SVGs keep their names. `gzip-www.mjs` writes a level-9 `.gz`
  next to every `.html/.js/.css/.svg/.json`.
- Firmware embeds both plain and `.gz` files (`EMBED_FILES`), copies them to
  `/fs/www/` at boot, and serves the `.gz` with `Content-Encoding: gzip`. A
  new firmware needs only "serve these files at these paths, gzip if
  accepted, SPA fallback to `index.html`".
- Redesign dev server: `ROBOT_HOST=<robot ip> npm run dev` proxies `/v1`
  (including WebSockets) to a real robot; the service worker is unregistered in dev.
- Service worker (`public/sw.js`, cache `mpx-dog-v2`): precaches the shell,
  never caches `/v1/*` or `ws://`, network-first for navigation, cache-first
  for assets. Bump the cache name when the asset set changes. The redesign
  still precaches and references `/icon.svg` (manifest too), which the
  redesign build does not ship; the failure is swallowed.

### 11.2 App structure

Neither app uses a router: screens are a Svelte `$state` string.

- **Original** (`pwa/src/app.svelte`): `home | chat | control | skills | wasm |
  marketplace | files | upload | wifi | lua`. Polls `/v1/wifi/status` every
  15 s.
- **Redesign** (`pwa-redesign/src/app.svelte`): `LockScreen` (slide to unlock,
  UI only) -> `WelcomeScreen` (IP / hotspot, links to Wi-Fi and AP setup) ->
  `HomeScreen` with tabs `home | skills | store | chat | settings` plus
  overlays (controller, calibration, Wi-Fi, AP, files/upload, action editors).
  **Link health:** `GET /v1/wifi/status` with a 4 s abort, every 5 s while
  online and 2.5 s while offline; offline after 2 consecutive failures. RTT
  is shown as latency. Connection mode comes from comparing `location.hostname`
  with the AP IP (`192.168.2.1`) and STA IP.
- Both apps open a **global** WebSocket to `/v1/chat/ui` just to receive
  `openclaw_action` permission prompts and answer them, and the chat screen
  opens a **second** one. The firmware must accept at least 2 concurrent
  clients per browser tab (it allows 4). Reconnect is 3 s after close.

### 11.3 What each screen calls

Robot control (both apps):

- Gait buttons: `POST /v1/robot/gait {"mode":...}`. Releasing a held control
  sends `{"mode":"none"}`.
- Joystick: `POST /v1/robot/joy {"f","s","t"}` every **100 ms** while held,
  plus one send on press and one zero send on release.
  - Redesign `Controller.svelte`: move stick only, `f = -ny`, `s = -nx`,
    `t = 0` always (turning uses the bumpers -> `turnL`/`turnR`), dead-zone
    0.18. The look stick (dead-zone 0.35) maps 8 sectors to gaits: E
    `lookright`, NE `looklr`, N `lookdown`, NW `lookll`, W `lookleft`, SW
    `lookul`, S `lookup`, SE `lookur`, centre `init`. The N/S mapping is
    inverted relative to the names; it is what the code does. D-pad/keys:
    `advance/back/left/right`. Face buttons are user-assignable
    (`mpx_action_assignments`), defaults top `jump`, right `twerk`, left
    `stretch`, bottom `sit`.
  - Original `Control.svelte`: dual pads, sends turn on `t`.
- Gait parameters: `GET /v1/robot/status` -> `config.*`; `POST /v1/robot/config`.
  Redesign sliders: period 0-200, height 0-150, up_height 0-50, stride 0-50,
  tilt -30..30 (never sends `sg_speed`). Original also sends `sg_speed`.
- Calibration: `GET /v1/robot/status` -> `offsets[12]` (FR 0-2, FL 3-5, RR
  6-8, RL 9-11, each abd/hip/knee); `POST /v1/robot/calibrate
  {"servo": base + joint + 1, "offset"}` (sliders -50..50 deg);
  `POST /v1/robot/calibrate/reset`. Redesign draws a stick figure with
  `pupperModel.js` (L1=50, L2=56, neutral thigh 52.5 deg / knee 45 deg, hips
  at x=+/-48.5, y=+/-31.5, sign tables mirroring `SK_SIGN`). Neither app uses
  Servo Studio (`/v1/studio/*`); that is a curl/bring-up tool.
- Original only: `POST /v1/robot/diagnostic/ping` (reads `ok`, `error`) and
  `/sweep`.

Wi-Fi:

- `GET /v1/wifi/status` (`sta.state` in `connected|connecting|failed|disconnected`),
  `POST /v1/wifi/connect|disconnect|forget`. Redesign polls status every 2 s
  while a connect is in progress.
- Redesign only: `POST /v1/wifi/ap-config {"ssid","password"}`; the UI then
  tells the user to rejoin the new hotspot (the robot reboots).

Files, skills, Lua:

- File browser (both): `GET /v1/fs/list?path=`, `GET /v1/fs/info`,
  `GET /v1/fs/read?path=`, `POST /v1/fs/delete {"path","sudo"}`; "run" on a
  file calls `POST /v1/skills/run {"skill": "<full path>"}` for `.wasm/.mpxe`
  or `POST /v1/lua/run {"path"}` for `.lua`.
- Upload (both): `POST /v1/skills/upload?name=<file>` with the raw file bytes.
- Original only: skill list `GET /v1/skills/list` + run; Lua editor uses
  `/v1/lua/list`, `/read?name=`, `/save {"name","code"}`, `/delete {"name"}`,
  and `/run` (`{"path":"/lua/<name>"}` after saving, `{"script"}` if
  unsaved). `pwa/src/lib/lua-robot-api.ts` is the autocomplete list for the
  `robot.*` Lua API and documents argument meanings.
- Redesign "custom actions" (`AddActionView`) only store
  `{id:"custom_<ts>", label, emoji, filePath, fileType}` locally; triggering
  one posts its id as a gait `mode`, so it does nothing on the robot today.

Marketplace (both, via `marketplaceApi.js`, always same-origin, no auth
headers; the robot adds its UUID when proxying):

| Call | Path |
|---|---|
| list catalog | `GET /v1/marketplace/skills` (redesign adds `?currency=XXX`) |
| skill detail / manifest | `GET /v1/marketplace/skills/{id}[?currency=]`, `GET .../{id}/manifest` |
| robot's skills | `GET /v1/marketplace/robot/skills` -> `[{skill_id,title,skill_type,enabled}]` |
| assign / toggle / remove | `POST /v1/marketplace/robot/skills {"skill_id"}`, `PATCH .../{id} {"enabled"}`, `DELETE .../{id}` |
| deploy | `POST /v1/marketplace/robot/deploy {"skill_id"}` -> `{"skills":[{"commands":[{"script"}]}]}`; the PWA sends each `script` to `POST /v1/lua/enqueue` |
| paid checkout (redesign) | `POST /v1/marketplace/robot/checkout {"skill_id"}` -> `{order_id, checkout_url, status}`; opens `checkout_url` in a new tab, polls `GET /v1/marketplace/orders/{order_id}` until `paid|fulfilled|failed|expired` |
| gateway info | `GET /v1/gateway/config` (fetched, not used by the UI) |

Deploy scripts are Lua that typically call `fs.write(...)` to drop a
`.wasm`/`.mpxe` onto the robot (and so trigger a permission prompt). The
redesign records the written paths per skill in `mpx_deployed_skills` (parsed
from the scripts) so "remove" can `POST /v1/fs/delete` them.

Skill fields used: `id, title, description, skill_type ("awa" = AI skill,
"wasm" = move skill), current_version, display_price | price.{amount,currency}`;
manifest `name, version, author, icon_url, description, capabilities[], tags[], readme`.

Chat (`Chat.svelte` / `chat/ChatView.svelte`):

- On open: `{"type":"session_reset","session_id","ts"}` (redesign adds
  `"pwa_build"`); also on new/select/delete conversation.
- Send: `{"type":"user_chat_input","text","session_id","ts"}`.
- Permission: `{"type":"permission_response","action_id","approved","ts"}`;
  the redesign's `PermissionCard` shows a 60 s countdown (matches the
  firmware's Lua fs timeout) and knows `file_write`, `file_delete`,
  `wasm_run`, `lua_run`.
- Handles every robot-to-PWA type listed in section 7. A `chat_reply` whose
  text contains `"Session reset acknowledged"` is shown only as a header badge.
- Conversations are browser-side only (`mpx_chat_conversations`); the robot
  keeps no history.

### 11.4 Browser storage keys

`mpx_active_session_id`, `mpx_chat_conversations` (both apps);
redesign also `mpx_currency`, `mpx_action_assignments`, `mpx_custom_actions`,
`mpx_pinned_actions`, `mpx_deployed_skills`.

### 11.5 Minimum API a new firmware needs for the default (redesign) PWA

Static files from 6.2, `WS /v1/chat/ui`, and:
`GET /v1/wifi/status`, `POST /v1/wifi/{connect,disconnect,forget,ap-config}`,
`GET /v1/robot/status`, `POST /v1/robot/{gait,joy,config,calibrate,calibrate/reset}`,
`GET /v1/fs/{list,info,read}`, `POST /v1/fs/delete`, `POST /v1/skills/{run,upload}`,
`POST /v1/lua/{run,enqueue}`, `GET /v1/gateway/config`, `* /v1/marketplace/*`.
The original PWA additionally needs `GET /v1/skills/list`, the Lua CRUD
routes, and `POST /v1/robot/diagnostic/{ping,sweep}`.

---

## 12. Porting checklist and hardware gotchas

### 12.1 Keep the PWA working unchanged

- Match the JSON field names exactly: `mode`, `f/s/t`, 1-based `servo`, the
  short file-list keys `f/d/n/s/r` and `p/c`, `ap/sta` in Wi-Fi status, the
  chat `type` strings, `chat_reply_chunk` with 0-based `seq`.
- Gait names are case-sensitive (`turnL`, `flegR`, `moveLF`, `wiggleL`, ...).
  An unknown name must not crash: this firmware maps it to `none`.
- Allow several WebSocket clients on `/v1/chat/ui` and broadcast
  `openclaw_action` to all of them.
- `POST /v1/robot/joy` arrives at 10 Hz; stop the walk motion if input goes
  stale (this firmware: 600 ms -> step in place).
- `POST /v1/wifi/ap-config` must answer before rebooting.
- Keep the marketplace behind the robot (`/v1/marketplace/*`) unless the new
  firmware serves the PWA over HTTPS; the browser on the robot's AP has no
  route to the cloud anyway.

### 12.2 Existing PWA/firmware mismatches (fix on whichever side you port)

- The redesign offers a `sit` gait (default bottom face button); the firmware
  has no `sit`, so it becomes `none`.
- The redesign's tilt slider goes -30..30, but `POST /v1/robot/config` ignores
  any value < 0 (`json_get_int(..., -1) >= 0`), so negative tilt is dropped.
  Its default tilt is 0; the firmware's is 10.
- Redesign look-stick N/S sectors send `lookdown`/`lookup` (inverted names).
- Strafe sign is flipped twice: the redesign sends `s = -nx`, and the Stanford
  walk negates `s` again (`vy = -s_js_s * ...`, a "left/right reversed" fix).
  A new firmware should pick one convention and document it, not copy both.
- Redesign custom actions are not executable (see 11.3).
- `/icon.svg` is referenced by the redesign's manifest and service worker but
  only shipped by the original build.
- `WS /v1/telemetry/stream` is a stub; no live telemetry push exists. Telemetry
  is polled (`/v1/studio/status`, `/v1/trace`).

### 12.3 Hardware facts that must survive the port

1. Servos are behind 4 AT32F413 boards on SPI, not on a serial bus. 36-byte
   frames; a position command carries a **current cap in mA**, not a speed.
2. Read the boards' `range_position_deg` at boot; do not hardcode 270 or 310.
3. Config request + NOP reply must be atomic on the bus, >= 2 ms apart per
   board, with the gait paused. Validate feedback frames (`0xFEED` and status
   not `0xC0DE`).
4. The command path flips direction (`scale - pos`) and the feedback path does
   not; convert before comparing commanded vs measured.
5. The leg-to-connector map (`SERVO_CS_MAP`) and board variant (`SERVO_BOARD`)
   are chassis-specific. Verify servo 1 = front-right abduction on blocks.
6. No servo power switch: the servo rail is on whenever the battery is. Wait
   about 1 s after boot before talking to the boards.
7. Start a new chassis at a ~600 mA current cap, then raise it to 1500.
8. The BMI270 needs its Bosch config blob; the BMM150 is only reachable
   through the BMI270 AUX bus with internal pull-ups enabled; IMU and audio
   codec share one I2C bus handle.
9. Neutral stand: all servos centred == 70 mm body height; both IK paths
   compute angles relative to that stance. Offsets are degrees applied in IK.
10. Keep the NVS layout (section 10) if robots should keep their calibration
    and Wi-Fi when reflashed between firmwares; this firmware already imports
    offsets from the reference firmware's `parameter` namespace.
11. PSRAM is required for the WASM linear memory and the Lua heap.
12. The socket budget is tight (16 lwIP sockets): limit httpd's open sockets,
    reap dead clients, cap and short-linger outbound proxy connections, and
    cache marketplace GETs.

### 12.4 Bring-up references

`docs/NEW_BOARD_BRINGUP.md` is the bench procedure (boot log checks, connector
map test with `/v1/studio/direct`, direction checks, calibration, current
limits, IMU). Expected healthy boot lines: `Driver board range: 310.0 deg`,
`Servo probe OK - all 12 channels responded`, `IMU initialised`.
