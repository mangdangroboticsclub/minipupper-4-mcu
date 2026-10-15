# mpv2-claw code guide

Every file in this project, what it does, and what to know before changing it.
Start with the [README](../README.md) for the big picture. Paths are relative
to the repository root. `third-party/esp-claw` (ESP-Claw) and
`managed_components/` are not ours and are not covered here.

**Where the code came from** (the file headers say so too):

- **mpv2 / minipupperesp:** gait, kinematics, moves, servo driver (copied verbatim into `components/mpx_robot/src/robot/`), and the phone app (`frontend_pwa_mpv2`).
- **MPX-Dog firmware:** the WASM skill runtime, the `/v1` API shapes, and the PWA design.
- **StanfordQuadruped / mini_pupper_2_bsp:** the trot gait and IK maths.
- **ESP-Claw:** everything under `third-party/esp-claw`, plus the shadowed `http_server` base files.
- **New in mp4-claw / mpv2-claw:** the IMU driver, C facades, the skill hook table, voice, the face, Servo Studio, Wi-Fi tuning, selftest.

## Contents

1. [Top level and build](#1-top-level-and-build)
2. [main/](#2-main)
3. [boards/](#3-boards)
4. [components/mpx_robot: the quadruped](#4-componentsmpx_robot-the-quadruped)
5. [components/cap_robot](#5-componentscap_robot)
6. [components/mpx_wasm: WASM skills](#6-componentsmpx_wasm-wasm-skills)
7. [components/cap_mpx_skill](#7-componentscap_mpx_skill)
8. [components/cap_display: face, GIFs, screen](#8-componentscap_display-face-gifs-screen)
9. [components/mpx_audio](#9-componentsmpx_audio)
10. [components/mpx_voice_wake](#10-componentsmpx_voice_wake)
11. [components/mpx_voice_link](#11-componentsmpx_voice_link)
12. [components/mpx_mcp_ws](#12-componentsmpx_mcp_ws)
13. [components/cap_music](#13-componentscap_music)
14. [components/mpx_pi_link](#14-componentsmpx_pi_link)
15. [components/http_server: C side](#15-componentshttp_server-c-side)
16. [components/http_server: phone app (frontend_pwa_mpv2)](#16-componentshttp_server-phone-app-frontend_pwa_mpv2)
17. [components/http_server: other web UIs](#17-componentshttp_server-other-web-uis)
18. [components/app_config](#18-componentsapp_config)
19. [components/mpx_wifi_tune and cmd_wifi](#19-componentsmpx_wifi_tune-and-cmd_wifi)
20. [components/mpx_selftest and mpx_util](#20-componentsmpx_selftest-and-mpx_util)
21. [components/gen_bmgr_codes](#21-componentsgen_bmgr_codes)
22. [fatfs_image/](#22-fatfs_image)
23. [tools/](#23-tools)
24. [docs/](#24-docs)

---

## 1. Top level and build

| File | What it does |
|---|---|
| `CMakeLists.txt` | The project (`mpv2_claw` 0.2.0). In order:<br>1. Applies the ESP-IDF text patches (`tools/cmake/esp_idf_patch.cmake`).<br>2. Picks the partition table from the board's flash size.<br>3. Patches esp-sr's `movemodel.py` so model packing does not crash on Windows code pages.<br>4. Stages `fatfs_image/storage` into the **storage** image.<br>5. Stages `fatfs_image/system` + board overlay + the web UIs (`frontend_pwa_mpv2/www/*.gz` → `system/www/`, `frontend_source/dist/index.html.gz` → `system/settings/`) into the read-only **system** image.<br>6. Syncs component skills and built-in Lua into `system/skills` and `system/scripts`.<br>7. Builds both FAT images with `FLASH_IN_PROJECT`, so `idf.py flash` writes them. |
| `partitions_16MB.csv` | The layout used on the robot. See README section 3. `nvs` is kept at 0x9000 like mpv2; one 7 MB `factory` app, no OTA. |
| `partitions_8MB.csv`, `partitions_32MB.csv` | Layouts for 8 MB boards (generic devkit) and 32 MB boards. |
| `sdkconfig.defaults` | Project-wide defaults, notably:<br>• PSRAM XIP and `-Os`; FreeRTOS 1 kHz; main task stack 6144.<br>• `LWIP_MAX_SOCKETS=20`; FAT volume count 3.<br>• LVGL with GIF and stdio FS letter `S:`.<br>• Audio player resampling to 16 kHz mono (the speaker shares the mic's I2S clock).<br>• Bluetooth off. |
| `sdkconfig.defaults.esp32p4`, `sdkconfig.console.jtag` | Overlays for an ESP32-P4 build and for a USB-JTAG console. |
| `sdkconfig` | Generated. Keep line endings as they are when editing. |
| `requirements.txt` | `esp-bmgr-assist` (needed for `idf.py bmgr`). |
| `dependencies.lock` | Component-manager lock. Pins IDF 5.5.4, LVGL 9.5.0, esp-sr 2.5.3, WAMR 1.3.2, mcp-c-sdk 2.0.1 and others. |
| `skills-lock.json` | A coding-assistant skill; unrelated to the firmware. |
| `AGENTS.md` / `CLAUDE.md` | Rules for anyone (human or AI) editing the repo. `CLAUDE.md` just points to `AGENTS.md`. Read "The one rule" and the build pitfalls. |
| `README.md` | Overview (this guide's companion). |
| `LICENSE` | Apache-2.0. |
| `.agents/` | Design notes from mp4-claw: ESP-Claw gotchas, the mp4 integration plan, the skill and Lua module specs. |

---

## 2. main/

| File | What it does |
|---|---|
| `main.c` | `app_main()`; the boot sequence is listed in README section 5. Starts the Pi link right after the robot and reacts to slave mode (`pi_events` task, see section 14). Other jobs:<br>• Feeds the face its motion source (IMU + current move).<br>• Fills the HTTP server's service table (config, Wi-Fi connect/forget applied live, restart).<br>• Turns Wi-Fi disconnect reason codes into words ("wrong password").<br>• Renumbers the hotspot to 192.168.2.1.<br>• Registers the robot's agent tool groups.<br>• Registers the `ai` and `servo` console commands.<br>• Wires voice events to the face. |
| `app_fs.c` / `app_fs.h` | Mounts `/system` read-only and `storage` at `/fatfs` (or an SD card if the board has one). Copies any missing seed files from `/system/.recovery` without overwriting. Reformats a storage volume left over from another layout. Adds a 512 KB `/ramfs` in PSRAM. |
| `claw_overrides.c` | Linker wrap of `claw_event_router_init()` that raises ESP-Claw's event-router stack to `MP4_EVENT_ROUTER_STACK` (16 KB). One chat message fans out to several sockets and overflowed the original 8 KB. |
| `edge_agent_version.c/.h` | Returns the firmware version (CMake `project(VERSION)`). |
| `Kconfig.projbuild` | The "App Config" menu: Wi-Fi defaults, `MP4_ROBOT_*`, display, voice, MCP groups, Pi link, gateway URLs, skill key, event-router stack. Table in README section 12. |
| `CMakeLists.txt` | Builds `main/`; adds the `--wrap=claw_event_router_init` linker flag for robot builds. |
| `idf_component.yml` | Which components get pulled in. Always: ESP-Claw's `app_claw`, `wifi_manager`, `settings`, `app_config`, `http_server`, `cmd_wifi`, `mpx_wifi_tune`. Robot builds add every `mpx_*`/`cap_*` component. Voice builds add esp-sr and `mpx_voice_wake`. |

---

## 3. boards/

Selected with `idf.py bmgr -c ./boards -b <board>`; the generated code lands in `components/gen_bmgr_codes`.

| File | What it does |
|---|---|
| `mp4_esp32_core/board_info.yaml` | Chip `esp32s3`, board name. |
| `mp4_esp32_core/board_peripherals.yaml` | I2C0 (GPIO1/2), duplex I2S0, LCD SPI on **SPI3** (SPI2 is for the servos), LEDC backlight on GPIO42 (inverted). |
| `mp4_esp32_core/board_devices.yaml` | Devices:<br>• ST7789 320×240 (swap_xy, mirror_x, inverted colours, 80 MHz).<br>• Backlight, starting at 0% (the face turns it on).<br>• MAX98357A speaker.<br>• ES7210 microphones.<br>• BMI270 IMU (`init_skip`; `mpx_robot` owns it).<br>• Buttons BOOT (0) and WAKE (21).<br>The BMM150 magnetometer is behind the BMI270 and is handled in code. |
| `mp4_esp32_core/setup_device.c` | Panel factory for the ST7789. Every board with `display_lcd` needs this file. |
| `mp4_esp32_core/sdkconfig.defaults.board` | Board defaults: 240 MHz, 16 MB QIO flash, octal PSRAM, ES7210 codec, robot + servos + voice on, WakeNet9 "Hi ESP", console on UART0. |
| `mp4_esp32_core/README.md` | Full pin map and the hardware details still marked "unverified". |
| `generic_esp32s3/*` | Any S3 devkit with octal PSRAM: runs ESP-Claw plus `selftest` with no robot hardware, voice and LCD off, 8 MB layout. |
| `santy_control/*` | MangDang Santy Control board (camera, DC motors, QMI8658 IMU); same audio/LCD pins as the MP4. Target of `docs/voice-plan.md`. |

---

## 4. components/mpx_robot: the quadruped

**What it does:**
- Drives the four AT32F413 servo driver boards over SPI.
- Runs the gait task on core 1 (Stanford trot, ~50 named moves, backflip presets, trajectory-optimised tables).
- Solves the leg IK and reads the BMI270 IMU and BMM150 magnetometer.
- Handles calibration, rest/sleep power states, and Servo Studio bus ownership.

The core is C++ `namespace robot` (mostly mpv2 verbatim). Other components
use the C facade `mpx_robot.h`; only `mpx_wasm` calls `robot::` directly.

**Depends on:** `esp_board_manager`, SPI/GPIO drivers, `nvs_flash`, `i2c_bus`, `bmi270_sensor`. NVS namespace `robot`.

| File | What it does |
|---|---|
| `include/mpx_robot.h` | **The public C API** (incl. `mpx_robot_pi_in_control`, `mpx_robot_set_master_observer`). Angles are degrees from centre; servo ids 1..12 (FR, FL, RR, RL × hip, thigh, calf). Functions:<br>• Init and moves: `mpx_robot_init`, `gait_by_name`, `playing_move`, `drive(fwd, side, turn)`.<br>• Body: `set_body_attitude` (roll ±25°, pitch ±20°, yaw ±30°), `look`.<br>• Walk config: period, height, step height, stride, tilt, speed (saved).<br>• Rest and calibration: `set_rest_after`, `rest_now`, `wake`; calibration offsets.<br>• Telemetry: IMU, angles, current, temperature, `ping_servo`.<br>• Studio and low level: `set_studio_mode`, low-level `set_servo_angle`/`flush`, overlay.<br>• `mpx_robot_set_skill_hooks()`, which lets `mpx_wasm` plug in without a dependency loop. |
| `src/mpx_robot_c_api.cc` | The facade: each C function forwards to `robot::`. |
| `src/mpx_imu.cc` | **New here.** Owns the BMI270 on the shared I2C bus (±16 g, ±2000 dps, 200 Hz output), reads the BMM150 through the BMI270's aux bus, and polls at **100 Hz** in task `mpx_imu`. A dead magnetometer is logged and ignored. |
| `src/robot/robot.h` | The C++ API:<br>• Constants: leg lengths, neutral height 70 mm, `SERVO_CURRENT_MAX_MA` 1500.<br>• `enum class GaitCmd` (all moves; 96+ = backflip presets, 128+ = trajopt tables) and `Config` defaults.<br>• Bus ownership (`set_studio_mode`, `servo_lock`), per-leg IK.<br>• `namespace master`, the Pi/ROS control entry points copied from mpv2 (`begin/end/set_velocity/...`). **Nothing calls `master::begin()` in this firmware.** |
| `src/robot/robot.cc` | **The robot** (~3,200 lines).<br>• `init()`: NVS `robot`, wait for all 4 boards, probe their scale, load calibration and per-servo current caps, start the gait task (core 1, priority 22) and the IMU, print the boot timing table.<br>• `GAIT_NAMES[]`: the single name table.<br>• `flush()`: at most one SPI frame every 5 ms; applies the overlay; runs the soft-start / glide / rest power state machine; stays silent while Studio or a skill owns the bus.<br>• `gait_task()`: dispatches every `GaitCmd`. The Stanford walk ticks every 15 ms from joystick / app / Pi input.<br>• Also: joystick timeout, attitude slew, look pad, the `playing_move` marker the face uses. |
| `src/robot/moves_mpe.inc` | Line-for-line ports of the minipupperesp moves, included into `robot.cc`:<br>• Helpers: stand ramps, servo gain presets ("Strong" for flips).<br>• Jumps: `jump_once`, long/far jump `lj_play`, trajopt player `tj_play` (45° tip-over guard).<br>• Sequences: `char_show` (Character moves in a row), the 9 backflip presets (`bf_play`), twerk, mate, hip test, "alive" IMU reflex loop, look pad. |
| `src/robot/trajopt_jump.h` | **Generated** (minipupper_trajopt `step5_export.py`); do not edit. 25 tables of servo deg×10 per 5/10 ms frame:<br>• Spin jumps.<br>• Character moves: bow, chase, cower, dig, look, paw, **scratch**, **shake**, trot, **wakeup** (stretch + yawn + shake, no walk), tripod, climb, rearclap, situp, surrender, runningman, moonwalk, sixseven, pushup.<br>• System-identification tests.<br>To update a move, regenerate it in minipupperesp and copy the table. |
| `src/robot/mpe_tables.h`, `longjump_table.h` | Long/far-jump segment tables (generated by `sim/jump_opt.py`). |
| `src/robot/stanford_gait.h/.cc` | C port of StanfordQuadruped's trot (vx, vy, wz; 15 ms tick, 24-tick phase). Pure maths. |
| `src/robot/stanford_kinematics.h/.cc` | Port of `pupper/Kinematics.py` with the Mini Pupper 2 Pro geometry and calibrated direction signs. |
| `src/robot/driver_board.h/.c` | SPI driver for the 4 AT32F413 boards (12 MHz, one mutex per operation so a config request and its reply are never split).<br>• Frames: mode, position, current limit (mA), kp, kd.<br>• Parameters 0–12: min/max ADC, range, gains, control mode, PWM scale…<br>• 13 live readings for Studio.<br>• Save to board flash, factory restore, direct torque/position commands. |
| `src/robot/board_config.h` | **Every GPIO in one place**, with notes on why each moved from the Mini Pupper 2 board. Servo CS map: FR=CN4/IO7, FL=CN5/IO4, RR=CN3/IO15, RL=CN6/IO5. |
| `src/robot/imu.h` | `robot::ImuData` and the IMU calls. |
| `src/robot/alive.h` / `alive.c` | Pure-C IMU reflex state machine (pushed, lifted → cartoon run, landed, shake). Builds on a PC too (`tools/alive_test.c`). |
| `src/bmm150/*` | Bosch BMM150 driver (BSD-3), unchanged. Keep `CONFIG_APP_CLAW_LUA_MODULE_MAGNETOMETER` off or its symbols collide. |
| `CMakeLists.txt`, `idf_component.yml` | Build; exports `src/` on purpose for `mpx_wasm`. |
| `README.md` | Port notes. **Partly stale**: it says SPI3 and other CS pins; `board_config.h` is right. |

---

## 5. components/cap_robot

Gives the agent the robot as four tools (group `cap_robot`). Tools return at
once; a move runs on its own. **Depends on:** `claw_cap`, `mpx_robot`, `mpx_wasm`.

| File | What it does |
|---|---|
| `src/cap_robot.c` | The four tools:<br>• `robot_move {name}`: runs a move by name. Built-ins first, then installed skills; without a name it does a timed analog drive (≤10 s).<br>• `robot_pose {roll,pitch,yaw,speed_dps}`.<br>• `robot_get_state`: gait, config, IMU, hottest servo, power state, all move names.<br>• `robot_set_config`: partial walk-config update.<br>Unknown names reply `ok:false` instead of failing, so the model does not retry.<br>While the Raspberry Pi is in control, `robot_move`, `robot_pose` and `robot_set_config` reply `pi_in_control`. |
| `include/cap_robot.h` | `cap_robot_register_group()`. |
| `skills/mpx_robot/SKILL.md` | Teaches the agent how to use the tools: check state first, stop above ~60 °C, which walk to use, pose limits, config ranges. |

---

## 6. components/mpx_wasm: WASM skills

The WebAssembly skill runtime (WAMR), ported from MPX-Dog.
- **ABI:** frozen at `MPX_ABI_VERSION 4`; host functions may be added but never changed or reordered.
- **Encrypted skills:** `.mpxe` files are bound to `MP4_ROBOT_UUID`.
- **Around the runtime:** a manifest registry, a one-at-a-time runner, IMU/chat events, autorun with **safe mode**, and the movement name space.
- **Storage:** skills live in `/fatfs/mpx_skills/`.

**Depends on:** `mpx_robot`, `mpx_util`, WAMR, mbedtls, NVS.

| File | What it does |
|---|---|
| `include/mpx_wasm.h` | Public C API:<br>• Lifecycle: `mpx_wasm_init`, `start_skills`.<br>• Running: `run_skill(file, "k=v;k=v", why)`, `stop_skill`, `skill_running`, `rescan`, `skill_at`.<br>• Moves: `movement_run(name)`.<br>• Files: skill file read/write/delete.<br>• Events and safe mode: `fire_chat`, `safe_mode`, `clear_safe_mode`. |
| `src/mpx_wasm_c_api.cc` | Implements the C API; registers the skill hooks with `mpx_robot`. |
| `src/wasm_sandbox.h/.cc` | Loads and runs a module on a pthread (16 KB stack).<br>• 10 ms watchdog.<br>• Lifecycle: `on_start`, optional `on_tick(dt_ms)` loop, `on_stop`.<br>• On exit it always restores servo gains, releases the bus lock and clears the overlay.<br>• Memory: 128 KB heap / 128 KB linear memory in PSRAM. |
| `src/wasm_host_functions.h` | **The ABI:** error codes, control domains (gait / feet / joints / bus), and the `NATIVE_SYMBOLS[]` table (70 functions: `robot_*`, `servo_*`, `mpx_*`). Must stay non-`const` (WAMR sorts it in place). |
| `src/wasm_host_functions.cc` | The ~1,200-line implementations on top of `robot::` and `driver_board_*`, with control arbitration and gain restore. |
| `src/wasm_decrypt.h/.cc` | MPXE format: the per-skill key is unwrapped with the robot key (AES-GCM, AAD bound to the UUID), then the module is decrypted. Keys are zeroed after use. |
| `src/mpx_crypto.h/.cc` | AES-256-GCM, random bytes, hex helpers (mbedtls). |
| `src/mpx_skill_fs.h/.cc` | Skill files under `<DATA>/mpx_skills` (read/list/write/delete); refuses `..`. |
| `src/registry.h/.cc` | Reads each module's `mpx` custom section (bounds-checked, treats files as untrusted). Manifest fields: slug, provided gait name, events, ABI, autorun, behaviour. |
| `src/runner.h/.cc` | One skill at a time. One-shot skills get a 60 s watchdog; behaviour skills get none. Status JSON for the app. |
| `src/movement.h/.cc` | Move names: built-in first, else a skill that provides that gait, else "unknown". |
| `src/events.h/.cc` | Manifest triggers: `boot`, `imu.lifted`, `imu.fallen`, `imu.shaken`, `chat:<word>`. Polled every 100 ms; 4 s cooldown. |
| `src/autorun.h/.cc` | Runs the autorun skill at boot. Counts failed boots in NVS `mpx/arun_fail`; 3 failures turn on safe mode. |
| `README.md` | Port notes, boot order, memory budget, safe-mode recovery. |

---

## 7. components/cap_mpx_skill

Agent tools for WASM skills (group `cap_mpx_skill`): `mpx_list_skills`,
`mpx_run_skill {skill, params}`, `mpx_stop_skill`. They are named `mpx_` so
they are not confused with ESP-Claw's own `cap_skill` (SKILL.md documents).
`skills/mpx_skills/SKILL.md` teaches the agent the one-at-a-time rule, params,
and how to report safe mode.

---

## 8. components/cap_display: face, GIFs, screen

**What it does:**
- Draws the robot's face: two LVGL eyes that blink, follow the IMU, change mood, and play per-move expressions.
- Plays full-screen GIFs.
- Handles screen orientation and shift.
- Registers the agent tools `display_show_text`, `display_show_emotion`, `display_clear`, `display_set_colors`, `display_set_brightness`.

It opens a *shared* display session so Lua drawings can still take the screen.

**Depends on:** `claw_cap`, `display_service`, `lvgl`. NVS namespace `display`: `bright`, `talk`, `alive`, `eyes_inv`, `flip`, `mirror`, `gap_x`, `gap_y`, `anim_on`, `anim_id`, `anim_loop`.

| File | What it does |
|---|---|
| `include/cap_display.h` | Public API:<br>• Startup order: `service_start` → `face_start` → `anim_restore`.<br>• Status line: network and Pi IP.<br>• Voice hooks (heard / saying / emotion…).<br>• Orientation, test pattern, motion source (IMU + current move), eyes options.<br>• GIF control: `cap_display_anim_play/stop/get/restore`. |
| `src/cap_display.c` | Display session and backlight (LEDC); NVS writes are deferred to a timer.<br>• The Pi's camera screen (`cap_display_show_jpeg`: esp_new_jpeg decode, double buffer, back to the face 1.5 s after the last frame) and the "ROS" marker on the network line (`cap_display_set_ros_mode`).<br>• Moves the LVGL draw buffers into internal DMA RAM (so SPI flushes need no bounce buffer).<br>• Puts the face back whenever another screen takes over.<br>• Maps voice emotions to moods; shows subtitles.<br>• Draws the colour test pattern; defines the tool descriptors. |
| `src/cap_display_face.c` | The face. Eye geometry; 17 public moods plus internal ones (peek, yawn, asleep…). "Alive" gaze driven by IMU pitch/roll, breathing, body reactions.<br>• **`s_move_faces`:** timed expression tracks per move (wakeup, cower, bow, jumps…).<br>• **GIF player:** uses LVGL's AnimatedGIF decoder directly, not `lv_gif`, because LVGL 9.5's widget paints "unchanged" pixels with the background colour and breaks optimized GIFs. Handles transparency and disposal 2/3, plays into a PSRAM RGB565 canvas, can play once or loop, and is restored at boot from NVS. |
| `src/cap_display_face.h` | Internal calls between the two `.c` files. |
| `src/cap_display_orient.c` | Applies flip / mirror on the ST7789 and the picture shift (`esp_lcd_panel_set_gap`). Console `display`: `info`, `flip`, `mirror`, `reset`, `test [s]`, `shift x y`, `talk on|off`, `eyes …`. |
| `skills/mpx_display/SKILL.md` | Teaches the agent the display tools. |
| `skills/mpx_display_draw/SKILL.md` | Teaches the agent to draw custom screens in Lua and hand the screen back. |

---

## 9. components/mpx_audio

**What it does:** the microphone (ES7210) and speaker (MAX98357A).
- **Shared clock:** they share one I2S clock, so the output is never stopped or re-clocked while the mic is open.
- **Who gets the speaker:** voice always wins it over music.
- **Speaker tuning:** high-pass, gain, soft ceiling, volume.

NVS `voice`: `vol` (default 80), `t_on`, `t_hpf`, `t_gain`, `t_ceil`.

| File | What it does |
|---|---|
| `include/mpx_audio.h` | Init, record/play WAV, tone, volume (`set_volume` saves to NVS; `set_volume_transient` does not, for the Pi's music), mic gain, streaming capture/output, music write, tuning. |
| `src/mpx_audio.c` | Implementation (stubs on boards with no codec). Mic gain default 24 dB. |
| `src/mpx_audio_tune.c/.h` | Speaker DSP at 16 kHz: 4th-order high-pass (600 Hz), gain, tanh soft ceiling. |
| `src/mpx_audio_cmd.c` | Console `audio`: `info`, `rec`, `tone`, `play`, `vol`, `gain`, `tune`. |

---

## 10. components/mpx_voice_wake

"Hi ESP" and the hands-free loop.
- **Detection:** runs esp-sr's AFE (echo cancellation, VAD, WakeNet) on a 4-channel mic feed; the speaker reference is channel 0.
- **Loop:** wake → beep → stream the question → reply → follow-up listening → back to waiting.
- **Barge-in:** saying the wake word during a reply stops it.
- **WAKE button:** GPIO21 does the same as the wake word.

Kconfig: `MP4_VOICE_ENABLE`, `MP4_VOICE_AUTO_LISTEN`, `MP4_VOICE_AUTO_MAX_SECONDS`.

| File | What it does |
|---|---|
| `src/mpx_voice_wake.c` | Everything above. Console `wake`: `auto on|off`, `start`, `stop`, `info`, `debug`, `levels` (find the echo-reference channel). |
| `include/mpx_voice_wake.h` | Init/start/stop, `auto_start`, `conversing()`, follow-up gate hook (used by music). |
| `CMakeLists.txt` | Explains why esp-sr is attached with `idf_component_optional_requires`. |

---

## 11. components/mpx_voice_link

Client for the **MangDang voice gateway** (xiaozhi protocol).
- **Provisioning:** one POST returns a WebSocket URL + token, saved in NVS `mpx_voice`.
- **Hello:** advertises MCP support.
- **Audio:** Opus at 16 kHz up and 24 kHz down (decoded at 16 kHz), 60 ms frames.
- **Hooks:** passes MCP messages and UI events (heard, saying, emotion…) to other components, so it depends on neither.

| File | What it does |
|---|---|
| `src/mpx_voice_ota.c` | Provisioning POST (device id from the MAC); stores URL + token. |
| `src/mpx_voice_ws.c` | WebSocket client and message handling: hello, `stt`, `tts`, `llm` (emotion), `mcp`, `goodbye`, binary audio. |
| `src/mpx_voice_stream.c` | Mic → Opus → socket (talk task) and socket → Opus → speaker (playback task, ~7-frame jitter buffer). Manual and auto listen. |
| `src/mpx_voice_codec.cc` | Opus encoder/decoder wrappers; loopback test. |
| `src/mpx_voice_cmd.c` | Console `voice`: `provision`, `connect`, `disconnect`, `talk [secs|auto|stop]`, `send <json>`, `loopback`, `info`. |
| `include/mpx_voice_link.h` | All of the above, plus "device brain" (own mode: the robot's agent answers and the gateway only speaks). |

---

## 12. components/mpx_mcp_ws

Turns the robot's agent tools into an **MCP server** on the voice socket, so the gateway's LLM can move the robot.
- **Which tools:** filtered by `MP4_MCP_GROUPS` and paged.
- **Name clash:** `get_current_time` is renamed `robot_get_current_time`.
- **`ask_robot_agent`:** hands a request to the on-board agent and waits up to 30 s.
- **Own mode:** voice transcripts go to the robot's agent.

`src/mpx_mcp_ws.c`, console `mcp call|send|info`.

---

## 13. components/cap_music

Internet radio (20 SomaFM stations, picked by genre) and direct audio URLs, resampled to 16 kHz.
- **Pauses for voice:** pauses or stops whenever voice is active and resumes after 700 ms of quiet; "be quiet" stops it.
- **Agent tools:** `music_play {genre|url}` and `music_stop`.
- **Console:** `music status|play|stop|list|level`.
- **Settings:** NVS `music/level`.
- **Lua audio:** never use ESP-Claw's Lua `audio` module on this robot; it re-clocks the shared I2S and breaks the wake word (the skill doc says so).

---

## 14. components/mpx_pi_link

**mpv2's Raspberry Pi / ROS 2 link**, moved into a component. The protocol header is byte for byte the same as mpv2's, so `ros2jazzympv2/mini_pupper_esp_bridge` drives this firmware unchanged. Wiring and bridge settings: `tools/pi/README.md`.

| | |
|---|---|
| Wire | UART1 at `MP4_PI_LINK_UART_BAUD` (921600), IO18 TX → Pi pin 10, IO8 RX ← Pi pin 8; IO3 HOST_PRESENT (Pi → ESP, pulled down); IO11 ESP_SLAVE (ESP → Pi) |
| States | `standalone` (nothing heard) → `monitor` (heartbeats, no control) → `slave` (IO3 high + WANT_CONTROL heartbeat within `MP4_PI_LINK_TIMEOUT_MS`, 300 ms) |
| Safety | Losing IO3 (~30 ms low) or the heartbeat calls `robot::master::end()`: stop, stand, hand back. CMD_VEL also times out inside the robot. A different protocol version is refused. |
| Kconfig | `MP4_PI_LINK_ENABLE`, `UART_BAUD` 921600, `TIMEOUT_MS` 300, `REQUIRE_HOST_PIN` y, `STATE_HZ` 50 (mpv2's `APP_PI_LINK_*`) |

| File | What it does |
|---|---|
| `src/pi_protocol.h` | **The wire format**, copied unchanged from mpv2. Frames `A5 5A len type seq payload crc16`, message ids, packed payload structs with size asserts, CRC-16/CCITT-FALSE. The Pi's `protocol.py` mirrors it; change both together. |
| `src/pi_link.cc` | mpv2's link task (core 0, priority 9).<br>• Byte parser and frame handlers: CMD_VEL, SET_MODE, BODY_POSE, FOOT_TARGETS → `robot::master::*`; AUDIO → `pi_audio`; VIDEO → `pi_video`; HOST_INFO → the Pi's IP.<br>• State machine; `enter_slave`/`exit_slave`.<br>• Telemetry: STATE at 50 Hz, INFO at 1 Hz, DIAG at 5 Hz, EVENTs.<br>• Plus the C API: `mpx_pi_link_init`, `is_slave`, `get_status`, `state_str`, `host_info`, `ip`, observers, `status_json`. |
| `src/pi_audio.h/.cc` | The Pi's music on the speaker (slave mode only): 2 s ring buffer in PSRAM, starts after 150 ms, plays 16 kHz mono through `mpx_audio`. Same as mpv2, except that it borrows the volume with `mpx_audio_set_volume_transient()`, so nothing is saved to flash. |
| `src/pi_video.h/.cc` | Reassembles VIDEO_DATA chunks into one JPEG and passes it to the sink `main.c` sets (`cap_display_show_jpeg`). Newest frame wins. |
| `src/mpx_pi_link.c` | Console `pi [status\|json]` with wiring hints. Replaces the old text-line test link. |
| `include/mpx_pi_link.h` | Public C API. |

**Who reacts to slave mode** (`main.c`):
- **Main task:** `main_pi_link_start()` registers `mpx_robot_set_master_observer()` and the IP observer.
- **`pi_events` task:**
  - Pi takes control: stops music and hands-free voice, shows "ROS" on the screen.
  - Pi lets go: brings hands-free voice back if it was on.
  - New Pi IP: updates the address under the eyes.
- **Refusals:** `cap_robot`, `/v1/robot/gait|joy|look` (423) and `mpx_wasm_run_skill` all refuse while `mpx_robot_pi_in_control()`.

## 15. components/http_server: C side

This component **shadows** ESP-Claw's `http_server` (same name, ours wins).
The base files are ESP-Claw's. The `http_server_mpx_*` files are the robot's
`/v1` API, built when `MP4_ROBOT_ENABLE` is set.

**Server settings:**
- Port 80, 12 sockets with least-recently-used purging, up to 104 route handlers.
- The httpd stack must stay in internal RAM (see the comment in `core`).
- The GET catch-all `/*` must be registered **last**.

| File | What it does |
|---|---|
| `http_server_core.c` | Starts httpd, registers every route group in order, captive-portal redirect, socket close hooks. |
| `http_server_priv.h` | Shared constants (upload max 512 KB, path max 256) and register functions. Logs `UNREGISTERED: <uri>` if a route does not fit. |
| `include/http_server.h` | Public init/start/stop and the services table `main.c` fills in. |
| `http_server_utils.c`, `http_server_json.c` | Path safety (no `..`), URL decoding, query parameters, JSON body (≤8 KB) and replies. |
| `http_server_assets.c` | ESP-Claw's embedded UI; **not built** on the robot (`http_server_mpx_web.c` replaces it). |
| `http_server_capabilities_api.c`, `_lua_modules_api.c`, `_config_api.c`, `_status_api.c` | ESP-Claw `/api/capabilities`, `/api/lua-modules`, `/api/config`, `/api/status`, `/api/restart` (used by `/settings`). |
| `http_server_files_api.c` | ESP-Claw file manager `/api/files…`, `/files/*` (upload ≤512 KB). |
| `http_server_lua_app_api.c` | Lets Lua scripts serve `/lua/*` and `/api/lua/*`. |
| `http_server_wechat_api.c`, `_webim_api.c` | WeChat QR login; ESP-Claw web chat `/ws/webim`. Also mirrors agent replies to the app chat and to MCP. |
| `http_server_mpx_robot_api.c` | `/v1/robot/gait`, `joy`, `look`, `status` (with `pi_link`), `config`, `calibrate`, `calibrate/reset`, `diagnostic/ping`, `moves`, `movements`, `power`, and **`/v1/pi/status`** (mpv2's fields). Moves go through the WASM movement name space; gait, joy and look answer 423 `pi_in_control` while the Pi drives. |
| `http_server_mpx_studio_api.c` | Servo Studio `/v1/studio/*` (all GET with query args, plus POST `mode` for `sendBeacon`). Parameter access needs studio mode (gait parked). |
| `http_server_mpx_skills_api.c` | WASM skills: list, registry, status, run, stop, upload (≤256 KB, filename allowlist, `.wasm`/`.mpxe` only), uninstall, safe-mode clear; `/v1/logs`, `/v1/trace` (polled). |
| `http_server_mpx_fs_api.c` | `/v1/fs/list|info|read|delete` for the app's file browser (read ≤64 KB). |
| `http_server_mpx_lua_api.c` | `/v1/lua/run|enqueue|list|save|read|delete`; scripts in `<DATA>/scripts` (≤32 KB). |
| `http_server_mpx_display_api.c` | `/v1/display` (eyes or GIF, loop/once) and `/v1/display/gif?id=` (download / upload ≤600 KB, ≤320×240 / delete). Built-in GIFs are protected. |
| `http_server_mpx_wifi_api.c` | `/v1/wifi/status|scan|connect|disconnect|forget|ap-config`. Changes apply live; renaming the hotspot reboots. |
| `http_server_mpx_ai_api.c` | `/v1/ai`: AI mode and own-LLM settings. The key is never sent back (only a hint). Saving restarts. |
| `http_server_mpx_market_api.c` | `/v1/gateway/config`, `/v1/marketplace/*`: proxy to the skill store gateway, with request limits and a small cache. |
| `http_server_mpx_chat_api.c` | WebSocket `/v1/chat/ui`: the app chat, answered by the **on-device agent**. Long replies are sent in chunks. |
| `http_server_mpx_web.c` | Serves `/system/www/*` (app) and `/system/settings/*` (gzip, ETag). Unknown `/v1` paths get JSON 404; other paths get the app shell. Also runs a dead-socket reaper. |
| `CMakeLists.txt`, `idf_component.yml` | Picks sources by Kconfig; needs `frontend_source/dist/index.html.gz` to exist. |

---

## 16. components/http_server: phone app (frontend_pwa_mpv2)

Svelte 5 + Vite + Tailwind. `npm run build` writes `www/` and `.gz` copies;
the firmware build puts them in the system partition. Bump `CACHE_VERSION` in
`public/sw.js` with each release so phones load the new app.

| File | What it does |
|---|---|
| `package.json`, `vite.config.js`, `svelte.config.js`, `tailwind.config.js`, `postcss.config.js` | Tooling. Fixed output names (`m.js`, `index.css`); dev proxy to `ROBOT_HOST`. |
| `gzip-www.mjs` | Writes the `.gz` copies after the build. |
| `public/sw.js` | Service worker: network-first for the app shell, so updates arrive. |
| `public/manifest.json`, `eye-*.svg`, `md.svg`, `pupper_model.json` | App manifest, icons, logo, 3D robot model for Calibration. |
| `src/index.html`, `main.js`, `app.css`, `app.svelte` | Shell, theme tokens, connection tracking (polls `/v1/wifi/status`). |
| `src/lib/LockScreen.svelte` | Slide-to-unlock screen with the eyes. |
| `src/lib/WelcomeScreen.svelte` | First-run connect page. |
| `src/lib/HomeScreen.svelte` | Tab bar (Home, Skills, Store, Chat, Settings); Home tab with gamepad and quick actions. |
| `src/lib/Controller.svelte` | The gamepad: sticks, D-pad, moves, look (`/v1/robot/joy|gait|moves|look`). |
| `src/lib/ActionAssignment.svelte`, `ActionIcon.svelte`, `AllActions.svelte`, `AddActionView.svelte` | Map buttons to moves, all moves list, add a custom action from a robot file. |
| `src/lib/SettingsPage.svelte` | Settings hub and its sub-pages. |
| `src/lib/WiFiSetup.svelte`, `WifiPicker.svelte`, `WifiSheet.svelte`, `APModeConfig.svelte` | Wi-Fi join/forget, network picker, hotspot rename. |
| `src/lib/AISettings.svelte` | AI & Voice mode and own-LLM settings. |
| `src/lib/Calibration.svelte`, `pupper3d.js` | Per-leg offset calibration with a 3D model (three.js, loaded on demand). |
| `src/lib/ServoStudio.svelte` | MangDang Servo Studio, phone and PC layouts:<br>• Servo list; per-servo settings with an edit sheet; live gauge/meters; torque switch.<br>• Graph (traces, CSV, PNG); pot calibration (manual and auto sweep); All servos.<br>• Keeps one request in flight (see its comments). |
| `src/lib/DisplayPage.svelte` | Eyes or GIF, loop/once, add/delete GIFs, storage bar. |
| `src/lib/FileViewer.svelte`, `UploadView.svelte` | Browse robot files, run Lua/skills, upload skills. |
| `src/lib/SkillsManagement.svelte`, `SkillsMarketplace.svelte`, `marketplaceApi.js`, `currency.js` | Skills tab, Store tab, store client, price display. |
| `src/lib/PermissionCard.svelte` | Approve/deny card for privileged agent actions. |
| `src/lib/PupperEyes.svelte` | Animated eyes for the chat empty state. |
| `src/lib/colors.js`, `conversationStore.js` | Palette; chat history in localStorage. |
| `src/lib/chat/ChatView.svelte`, `ChatMessage.svelte`, `ChatSidebar.svelte`, `AgentActivity.svelte`, `EmptyChat.svelte` | Chat over `/v1/chat/ui`: messages (markdown), conversation list, agent step timeline, starter prompts. |
| `src/lib/pupperModel.js` | Older stick-figure model; no longer imported. |

---

## 17. components/http_server: other web UIs

| Folder | What it is |
|---|---|
| `frontend_source/` | ESP-Claw's own SolidJS settings UI, served at **`/settings`** (LLM, IM channels, capabilities, files, memory, skills). Build with `pnpm install --frozen-lockfile && pnpm run build`; the firmware build requires its `dist/index.html.gz`. |
| `frontend_pwa/` | The older mp4-claw phone app. **Not shipped**; kept for reference. To switch back, point `EDGE_AGENT_PWA_WWW_DIR` in the top-level `CMakeLists.txt` at it. |

---

## 18. components/app_config

Saved settings struct `app_config_t` (Wi-Fi, hotspot, LLM, IM bots, search
keys, enabled tool groups, Lua modules, timezone, AI mode), stored in NVS
namespace `app` through ESP-Claw's settings store.

**AI mode** (`ai_mode`) is applied when settings load:
- `mangdang`: forces `MP4_GATEWAY_LLM_URL` with a per-robot token (HMAC of the UUID with the skill key).
- `own`: uses the stored key.
- `off`: blanks the LLM.

The running mode is fixed until restart. Files: `app_config.c`, `include/app_config.h`.

---

## 19. components/mpx_wifi_tune and cmd_wifi

| File | What it does |
|---|---|
| `mpx_wifi_tune/src/mpx_wifi_tune.c` | Better station settings for mesh / Wi-Fi 6 routers: scan all channels, strongest AP, PMF capable. A "quiet mode" keeps the setup page responsive while a phone is on the hotspot and home Wi-Fi keeps failing. |
| `mpx_wifi_tune/src/mpx_captive_dns.c` | DNS responder on the hotspot (every name → robot), replacing ESP-Claw's, whose replies some phones could not parse. |
| `cmd_wifi/cmd_wifi.c` | Console `wifi --status|--scan|--set --ssid X [--password Y] [--apply]|--apply`. |

---

## 20. components/mpx_selftest and mpx_util

| File | What it does |
|---|---|
| `mpx_selftest/mpx_selftest.c` | `selftest` (`--system`, `--i2c`, `--imu`, `--servo` ping only, `--backlight`, `--display`, `--buttons`, `--other-board`), `imu` (`watch`, `orient`, `cal`, compass), `button`. Never moves a servo: run it first, with the servo power off. |
| `mpx_util/src/log_ring.cc` | Keeps the last ~104 log lines in internal RAM for `/v1/logs` (still prints to UART). |
| `mpx_util/src/trace_ring.cc` | 256 named values traced by skills for `/v1/trace` / `mpx-cli trace`. |
| `mpx_util/src/mpx_rings.cc` | C wrappers for the HTTP layer. |

---

## 21. components/gen_bmgr_codes

Generated by `idf.py bmgr` from `boards/<board>/*.yaml`. Contains the device
and peripheral tables and handles. **Never edit**; regenerate.

---

## 22. fatfs_image/

| Path | Ends up in | Contents |
|---|---|---|
| `storage/display/` | storage `/fatfs/display` | Built-in GIFs `blink`, `smile`, `sleepy` (protected from deletion) |
| `storage/skills/` | storage | Example ESP-Claw skills: `light_switch`, `plan_mode`, `scheduled_task` |
| `storage/static/` | storage | ESP-Claw logo |
| `system/fonts/` | system (read-only) | Noto Sans SC subset for Chinese text on screen |
| `system/.recovery/` | system | Seed copied into storage when missing: default agent memory (`identity.md`, `soul.md`, `user.md`), router rules, demo schedules (all off) |
| *(added by the build)* | system | `www/` (phone app), `settings/` (ESP-Claw UI), `skills/` (every component's `skills/*/SKILL.md`), `scripts/builtin` + `scripts/docs` (Lua) |

The wake-word models go into the `model` partition through esp-sr, not through here.

---

## 23. tools/

| File | What it does |
|---|---|
| `cmake/esp_idf_patch.cmake` | Small idempotent text patches to ESP-IDF files, applied at configure (fails loudly if a patch no longer fits). |
| `cmake/flash_partition_defaults.cmake` | Picks `partitions_<flash size>.csv` from the generated board defaults. |
| `check_comments.py` | Finds `/*` inside block comments (the build treats them as errors). |
| `check_paths.py` | Checks `path:` dependencies and relative paths stay inside the repo, and there are no empty source files. |
| `gif_to_mpxa.py` | Converts a GIF to the MPXA animation format (player not written yet; see `docs/animations.md`). |
| `pi/README.md` | Raspberry Pi / ROS 2 setup: the six wires, serial port, matching the bridge's `esp_bridge.yaml`, checking with `pi`, what slave mode does. |
| `pi/mp4_link.py` | The old text-line test link (PING/HELLO at 115200). **Obsolete**: the firmware now speaks mpv2's protocol. |

---

## 24. docs/

| File | What it covers |
|---|---|
| `ESP_CLAW_PLAN.md` | The plan for mpv2-claw: decisions, architecture, RAM rules, and the stage table. Stage 5 (Pi/ROS) is now ported; it still needs testing on the robot with the ROS bridge. |
| `voice.md`, `voice-plan.md` | Why voice is built the way it is (gateway as brain, MCP for robot tools) and the phased plan. |
| `llm-setup.md` | Setting your own LLM at `/settings`. Set `llm_supports_tools` to true or the robot never acts. |
| `mpx-sdk.md` | Using `mpx-cli` and the MPX SDK against this firmware. |
| `animations.md` | The MPXA format and converter (firmware player not written). |
| `CODE_GUIDE.md` | This file. |
