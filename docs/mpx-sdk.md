# Using the MPX SDK with this firmware

Short answer: **yes, it connects, and nothing needed building.** Every
robot-facing endpoint `mpx-cli` uses was ported verbatim, the WASM ABI is
unchanged, and the skill decryption keys are the same values as before. A
skill you built against the mangdang firmware runs here.

This page is what to check, and the one thing that changed.

## The one thing that changed: the robot's address

Mangdang's firmware put the robot's own hotspot on `192.168.2.1`. ESP-Claw's
`wifi_manager` uses the ESP-IDF default, `192.168.4.1` — and `mpx-cli` defaults
to `192.168.2.1`, names it in its connection-failure hint, and so does every
note anyone has written about these robots.

**The firmware now moves the AP back to `192.168.2.1`** (`main_apply_ap_ip()` in
`main/main.c`), so the toolchain works with no per-machine configuration.

Two details worth knowing if you ever change this:

- The renumbering happens **before** `captive_dns_start()`. Restarting the DHCP
  server is what applies a new address, and it discards the DNS option the
  captive portal writes into that same DHCP configuration.
- `wifi_manager` snapshots the AP address into a string when the AP starts and
  never refreshes it, so it would keep reporting `192.168.4.1` afterwards.
  `main_get_wifi_status()` substitutes the real address, which is where
  `/v1/wifi/status` and the captive-portal redirect both read it from.

## Setting up the CLI

```powershell
cd C:\esp\projects\mpx-sdk
pip install -e cli
mpx-cli doctor
```

`doctor` checks the SDK headers, the toolchain and robot connectivity in one
go, and tells you what to type when something is missing. Run it before
anything else.

**Do not run it inside the ESP-IDF environment.** Building skills needs WASI
SDK's clang targeting `wasm32-wasip1`; ESP-IDF puts the Xtensa clang on PATH,
which reports a plausible version number and then cannot emit wasm. The SDK's
detector already tries known WASI install locations before PATH for exactly
this reason, but a separate shell avoids the question entirely.

## Connecting

| Situation | Address |
|---|---|
| Joined to the robot's own hotspot | `192.168.2.1` — the default, nothing to set |
| Robot joined to your Wi-Fi | whatever DHCP gave it |

For the second case the robot's log prints it as `sta=connected ... ip=<addr>`,
and it is also on the panel under the eyes. Then:

```powershell
mpx-cli deploy --ip 192.168.1.50            # once
'MPX_HOST=192.168.1.50' | Out-File -Encoding utf8 .env   # from now on
```

There is no authentication on these endpoints — same as the old firmware. Any
device on the same network can upload and run a skill. That is a LAN-trust
model, and it is worth remembering before putting the robot on a network you
do not control.

## What works without the marketplace gateway

The whole local development loop, with no gateway configured at all:

```
mpx-cli deploy      build, upload, run, report
mpx-cli upload      push a .wasm you already have
mpx-cli run/stop    start and stop a skill
mpx-cli list        what is on the robot
mpx-cli logs        the robot's log ring
mpx-cli trace       live values from mpx_trace_f()
mpx-cli robot       status, movements, gaits
```

These need `MP4_GATEWAY_HOST` set in `menuconfig` (it is empty by default,
which disables the marketplace):

```
mpx-cli publish     push a skill to the marketplace
mpx-cli install     pull a purchased skill down onto the robot
mpx-cli auth        log in
mpx-cli search      browse the catalogue
```

## Testing ESP-Claw on a different board

**Do not flash the MP4 build onto other hardware.** `main.c` wraps
`esp_board_manager_init()` in `ESP_ERROR_CHECK`, and the MP4 board declares an
ST7789, an ES7210 and a BMI270. On a devkit where those chips are absent,
device init fails, the check aborts, and the board sits in a boot loop -- which
tells you nothing about ESP-Claw.

Build for the generic board instead:

```bash
idf.py bmgr -c ./boards -b generic_esp32s3
rm sdkconfig
idf.py build flash monitor
```

That board declares no devices, so nothing can fail for want of a chip. It
turns off the panel, the codecs and the voice models, keeps the agent, the web
server, the skill runtime and the `selftest` command, and assumes 8 MB flash.
Change `CONFIG_ESPTOOLPY_FLASHSIZE` in its `sdkconfig.defaults.board` if yours
differs -- the partition table is chosen from it.

Then:

```
selftest --other-board
```

The firmware runs on any ESP32-S3 with enough flash. None of the MP4 wiring
will be there, so tell the self-test that:

```
selftest --other-board
```

Checks that describe MP4 wiring then report **WARN** rather than FAIL. What
still reports for real is everything the firmware controls: the partitions,
the SYSTEM and DATA mounts, RAM, and the console. If those pass, ESP-Claw
came up correctly and the agent, web server and skill runtime have what they
need -- which is the question worth answering before the real board arrives.

Expect the I2C section to report a silent bus, and the servo and display
sections to find nothing. That is the correct result on other hardware.

## Bring-up order

Do these in order the first time. The first step is the one that protects
hardware.

1. **Power the servo rail off.** `mpx_robot_init()` runs at boot and nothing
   validates that `MP4_ROBOT_SERVO_BOARD_VARIANT` matches how the legs are
   actually plugged in. A wrong variant drives the wrong physical joint. Get
   the display, Wi-Fi and API working with the servos unpowered.
2. **Flash and watch the log.** The face should appear before Wi-Fi connects.
   Note the AP SSID it prints, and confirm `AP address set to 192.168.2.1`.
3. **Join the AP, open `http://192.168.2.1/`.** The PWA should load. Check the
   panel orientation while you are here — mirrored or upside down is a one-line
   fix in `boards/mp4_esp32_core/board_devices.yaml`.
4. **`mpx-cli doctor`.** It should reach the robot and report the toolchain.
5. **`mpx-cli deploy`** on one of the SDK examples. This exercises upload, the
   sandbox and the ABI check in one command.
6. **Check the IMU** in the boot log. `mpx_imu` prints an I2C bus scan before
   it initialises; the BMI270 should answer at **0x68**. If it answers at 0x69
   instead, the schematic reading was wrong again and the board YAML needs
   flipping back. Confirm tilt looks sane before anything is built on it --
   the magnetometer work is deliberately waiting on this.
7. **Only then power the servos**, and confirm the board variant with a single
   slow movement before trying a gait.

## Known rough edge

The PWA's marketplace **Deploy** button still uses the old install path: it
POSTs generated Lua to `/v1/lua/enqueue`, which now runs ESP-Claw's `cap_lua`
rather than the old firmware's interpreter. Scripts written against the old
bindings will fail there.

`mpx-cli install` does the same job over `/v1/skills/upload` and is unaffected —
your own `install.py` documents why that path is better anyway (a 256 KB skill
needed ~340 KB of contiguous heap as base64 inside a Lua program).
