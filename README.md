> **mpv2-uros:** this copy of mpv2-claw runs ROS 2 on the ESP32-S3 itself
> (micro-ROS over Wi-Fi, no Raspberry Pi). Start with [README_UROS.md](README_UROS.md).
> The RL walking policy on the ESP32-S3: [README_RL.md](README_RL.md).

# mpv2-claw

Firmware for the **MangDang Mini Pupper / MP4 ESP32 CORE** quadruped
(ESP32-S3-WROOM-1-N16R8: 16 MB flash, 8 MB PSRAM), built on Espressif's
[ESP-Claw](https://github.com/espressif/esp-claw) agent framework.

It is three projects joined into one firmware:

| From | What it brings |
|---|---|
| **ESP-Claw** (`third-party/esp-claw`, git submodule) | The on-device AI agent: conversations, memory, skills (SKILL.md), Lua, capability ("tool") dispatch, `/settings` web UI, IM channels, MCP |
| **mp4-claw** (this tree's skeleton) | The board definition, the `/v1` HTTP API, WASM skill sandbox, voice (wake word + MangDang gateway), robot face, Servo Studio, Wi-Fi fixes |
| **mpv2 / minipupperesp** | The robot itself: SPI servo driver boards, Stanford trot gait, inverse kinematics, ~50 named moves and the trajectory-optimised move tables, plus mpv2's phone app (PWA) |

What makes it a *robot* rather than a dev board: the agent has the robot's
own tools. Say "stand up" in the app chat or "Hi ESP, look happy" out loud and
the agent calls `robot_move` / `display_show_emotion` itself.

> A file-by-file explanation of every source file is in
> **[docs/CODE_GUIDE.md](docs/CODE_GUIDE.md)**.

---

## Contents

1. [Is the Pi link the same as mpv2?](#1-is-the-pi-link-the-same-as-mpv2)
2. [Quick start: build and flash](#2-quick-start-build-and-flash)
3. [Flash layout (partition table)](#3-flash-layout-partition-table)
4. [How it fits together](#4-how-it-fits-together)
5. [Boot sequence](#5-boot-sequence)
6. [Repository layout](#6-repository-layout)
7. [The phone app and the HTTP API](#7-the-phone-app-and-the-http-api)
8. [Voice](#8-voice)
9. [Screen: face, moods and GIFs](#9-screen-face-moods-and-gifs)
10. [Skills: three different kinds](#10-skills-three-different-kinds)
11. [Serial console commands](#11-serial-console-commands)
12. [Configuration (Kconfig) and saved settings (NVS)](#12-configuration-kconfig-and-saved-settings-nvs)
13. [Hardware pins](#13-hardware-pins)
14. [Rules for working on this code](#14-rules-for-working-on-this-code)
15. [Known gaps and stale notes](#15-known-gaps-and-stale-notes)

---

## 1. Is the Pi link the same as mpv2?

**Yes.** `components/mpx_pi_link` is mpv2's Pi link (`main/pi/pi_link.cc`,
`pi_protocol.h`, `pi_audio.cc`, `pi_video.cc`) moved into a component, with
the protocol header copied byte for byte. The ROS 2 bridge that drives an mpv2
robot (`ros2jazzympv2/mini_pupper_esp_bridge`: SLAM, navigation, tracking,
dance, music) drives this firmware unchanged.

| | mpv2 and mpv2-claw (the same) |
|---|---|
| Wire | UART1, 921600 baud 8N1, frames `A5 5A len type seq payload crc16`, protocol version 1 |
| Pins | IO18 TX, IO8 RX, IO3 HOST_PRESENT (Pi → ESP), IO11 ESP_SLAVE (ESP → Pi) |
| Pi → ESP | HEARTBEAT, CMD_VEL, SET_MODE (stand / walk / named gait / init), BODY_POSE, FOOT_TARGETS, AUDIO_CTRL + AUDIO_DATA, HOST_INFO, VIDEO_DATA |
| ESP → Pi | STATE 50 Hz (servo angles, IMU, velocity), INFO 1 Hz, DIAG 5 Hz, EVENT |
| Slave mode | Only while IO3 is high **and** WANT_CONTROL heartbeats keep coming (300 ms timeout); losing either stops, stands and hands the robot back |
| While the Pi drives | Voice and music off, "ROS" on the screen, the app / agent / skills cannot move the robot; the Pi's music plays on the speaker and its camera picture shows on the screen |
| Status | `GET /v1/pi/status`, `pi_link` in `GET /v1/robot/status`, console `pi` |

Small differences you might notice:
- INFO reports the firmware as `mpv2-claw`. The bridge only logs this.
- The Kconfig names are `MP4_PI_LINK_*` instead of `APP_PI_LINK_*`; the defaults are the same.
- The camera picture is drawn by `cap_display`.
- The Pi's music borrows the speaker volume without saving it.

Wiring and the bridge settings: [tools/pi/README.md](tools/pi/README.md).

## 2. Quick start: build and flash

Requirements: ESP-IDF **5.5.x** (5.5.4 is what `dependencies.lock` pins),
Python requirements, Node.js (for the phone app) and pnpm (for `/settings`).

```bash
source /path/to/esp-idf/export.sh
python -m pip install -r requirements.txt
git submodule update --init --recursive

# 1. generate the board code (rerun after ANY change under boards/)
idf.py bmgr -c ./boards -b mp4_esp32_core

# 2. web UIs (only needed once, or after changing them)
cd components/http_server/frontend_pwa_mpv2 && npm ci && npm run build && cd -
cd components/http_server/frontend_source && pnpm install --frozen-lockfile && pnpm run build && cd -

# 3. firmware
idf.py build
idf.py -p PORT flash monitor
```

- `idf.py set-target` is not needed; the chip comes from `boards/<board>/board_info.yaml`.
- `idf.py flash` writes **every** partition including `storage`, which holds the
  user's uploads and the agent's memory. To update only the firmware use
  `idf.py app-flash`; to update only the web app use `idf.py system-flash`.
- Phone app live development: `cd components/http_server/frontend_pwa_mpv2 && ROBOT_HOST=<robot-ip> npm run dev`,
  then open `http://localhost:5173` (it proxies `/v1` to the robot).

First boot: the robot opens a hotspot `esp-claw-XXXXXX` at **192.168.2.1**.
Join it, open the app, pick your home Wi-Fi. After that the robot is reachable
at its home-network address (shown under the eyes).

---

## 3. Flash layout (partition table)

`partitions_16MB.csv` (chosen automatically from the board's flash size by
`tools/cmake/flash_partition_defaults.cmake`):

| Name | Type | Start | Size | Contents | Changed by |
|---|---|---|---|---|---|
| *(bootloader)* | | 0x0 | 32 KB | ESP32 bootloader | USB flash |
| *(partition table)* | | 0x8000 | 4 KB | this table | USB flash |
| `nvs` | data/nvs | 0x9000 | 24 KB | Settings: Wi-Fi, calibration, display, AI mode… | the app/console at runtime |
| `phy_init` | data/phy | 0xF000 | 4 KB | Radio calibration | automatic |
| `factory` | app | 0x10000 | 7 MB | The firmware (~5.2 MB today, ~26% free) | USB flash only |
| `model` | data/spiffs | 0x710000 | 1 MB | esp-sr wake word "Hi ESP" + VAD | USB flash only |
| `system` | data/fat, **read-only** | 0x810000 | 2.56 MB | Web app (`www/`), `/settings` UI, built-in skills and Lua, fonts, recovery seed | USB flash only |
| `storage` | data/fat | 0xAA0000 | 5.38 MB | Agent memory and sessions, uploaded skills (`mpx_skills/`), Lua scripts, GIFs (`display/`) | uploads from the app |

- `nvs` is at exactly the same place as mpv2, so moving a robot from mpv2 to
  this firmware keeps its Wi-Fi and servo calibration.
- There is **one** app slot, so there are no over-the-air firmware updates.
- Upload limits: skills 256 KB per file, general files 512 KB, GIFs 600 KB
  (≤320×240). All uploads share the 5.38 MB `storage` partition.
- `partitions_8MB.csv` (generic S3 devkit) and `partitions_32MB.csv` also exist.

---

## 4. How it fits together

```
   Phone app (Svelte PWA)          "Hi ESP" voice              Raspberry Pi (optional)
          │  HTTP /v1, WS /v1/chat/ui      │ mic / speaker            │ UART 921600 (ROS 2)
          ▼                                ▼                          ▼
 ┌─────────────────────┐   ┌──────────────────────────────┐   ┌──────────────┐
 │ http_server         │   │ mpx_voice_wake (esp-sr AFE)  │   │ mpx_pi_link  │
 │  /v1/robot, studio, │   │ mpx_voice_link (Opus + WS)───┼──►│ slave mode,  │
 │  skills, display,   │   │   to MangDang voice gateway  │   └──────────────┘
 │  wifi, ai, chat …   │   │ mpx_mcp_ws (MCP over voice)  │
 └─────────┬───────────┘   └──────────────┬───────────────┘
           │                              │
           ▼                              ▼
 ┌───────────────────────────────────────────────────────────┐
 │ ESP-Claw agent (app_claw)  ── LLM: MangDang gateway or own key
 │   tool groups:  cap_robot  cap_display  cap_mpx_skill  cap_music  cap_lua …
 └─────────┬───────────────────────┬──────────────────────┬──┘
           ▼                       ▼                      ▼
 ┌──────────────────┐   ┌───────────────────┐   ┌───────────────────┐
 │ mpx_robot        │◄──│ mpx_wasm          │   │ cap_display        │
 │ gait, IK, moves, │   │ WASM skill sandbox│   │ face/eyes, GIFs    │
 │ SPI servo boards,│   │ (WAMR, ABI v4)    │   │ ST7789 via LVGL    │
 │ IMU              │   └───────────────────┘   └───────────────────┘
 └──────────────────┘
```

Rules of the road:

- **Built-ins win.** A movement name is first looked up in the robot's own
  table, then in installed WASM skills (`mpx_wasm` movement namespace).
- **One skill at a time.** A second skill request is refused, not queued.
- **The agent's tools return immediately.** A move starts and the tool replies;
  long motions never use up the agent's turn.
- **Servo Studio owns the bus.** While Studio is connected the gait is parked
  so a config request/reply pair on the SPI bus is never split.

---

## 5. Boot sequence

From `main/main.c`, in order. Steps marked *(non-fatal)* log and carry on, so
a missing chip never becomes a silent boot loop.

1. Start the log ring (so `/v1/logs` has the boot log); print versions.
2. NVS, app config, timezone (default `CST-8`).
3. Board manager *(non-fatal; run `selftest` if it fails)*.
4. **Servos first** (`mpx_robot_init`), before the file systems, so the legs
   soft-start ~2 s after power-on instead of ~4.6 s. Prints the boot timing
   table and calibration. The **Pi link** starts right after, as in mpv2, so
   its failsafe exists before anything else can stall.
5. Mount `/system` (read-only) and `/fatfs` (storage); seed missing files from
   `/system/.recovery`.
6. Audio, WASM runtime *(non-fatal)*.
7. Wi-Fi init, display service (fixes the LVGL draw buffers), ESP-Claw UI,
   **robot face**, restore a looping GIF if one was chosen.
8. HTTP server, Wi-Fi start (hotspot renumbered to 192.168.2.1, Wi-Fi tuning,
   captive DNS); waits up to 30 s for home Wi-Fi.
9. Register the robot's tool groups (`cap_robot`, `cap_mpx_skill`,
   `cap_display`, `cap_music`), then **start the agent**.
10. Console commands, voice→face hooks.
11. AI mode: if not `off`, start the MCP bridge; with voice enabled, load the
    wake word and start hands-free listening.
12. **Last:** `mpx_wasm_start_skills()` (scan, IMU events, autorun) so a broken
    skill cannot brick the robot (after 3 failed boots autorun goes into safe mode).

---

## 6. Repository layout

```
CMakeLists.txt          project; stages web UIs + fatfs images into system/storage
partitions_*.csv        flash layouts (16 MB used)
sdkconfig.defaults      project-wide defaults
boards/                 board definitions for esp_board_manager
  mp4_esp32_core/         the robot (pins, LCD, audio, IMU, buttons)
  generic_esp32s3/        any S3 devkit, for running without robot hardware
  santy_control/          MangDang Santy Control board
main/                   app_main, file systems, Kconfig menu
components/             everything this project adds (table below)
fatfs_image/            files baked into the storage and system partitions
tools/                  build patches, checks, GIF converter, Pi-side script
docs/                   plan, voice, LLM setup, SDK, animations, CODE_GUIDE
third-party/esp-claw/   ESP-Claw submodule: DO NOT EDIT
managed_components/     downloaded by the IDF component manager: do not edit
```

| Component | Role (details per file in [docs/CODE_GUIDE.md](docs/CODE_GUIDE.md)) |
|---|---|
| `mpx_robot` | The quadruped: SPI driver for the 4 AT32 servo boards, gait task (core 1), Stanford trot, IK, all moves, IMU (BMI270 + BMM150), calibration, rest/power states. Mostly mpv2's code. |
| `cap_robot` | Agent tools `robot_move`, `robot_pose`, `robot_get_state`, `robot_set_config`. |
| `mpx_wasm` | WebAssembly skill runtime (WAMR), 70 host functions, encrypted `.mpxe` skills, registry, runner, events, autorun + safe mode. |
| `cap_mpx_skill` | Agent tools to list / run / stop WASM skills. |
| `cap_display` | Robot face (eyes, 17 moods, per-move expressions), GIF player, screen orientation/shift, agent display tools. |
| `cap_music` | Internet radio (SomaFM) and URL playback; always gives way to voice. |
| `mpx_audio` | Microphone (ES7210) and speaker (MAX98357A) on one shared I2S; volume and speaker tuning. |
| `mpx_voice_wake` | "Hi ESP" wake word, echo cancellation, barge-in, hands-free loop, WAKE button. |
| `mpx_voice_link` | Client for the MangDang voice gateway (xiaozhi protocol): provisioning, WebSocket, Opus. |
| `mpx_mcp_ws` | Serves the robot's tools over MCP on the voice socket; `ask_robot_agent`. |
| `mpx_pi_link` | mpv2's Raspberry Pi / ROS 2 link: binary UART protocol, slave mode, the Pi's music and camera picture (see section 1). |
| `http_server` | Port-80 web server: ESP-Claw `/api`, the robot `/v1` API, static web apps. Contains the phone app source. |
| `app_config` | Saved settings (`app_config_t`) and the AI mode (MangDang / own key / off). |
| `mpx_wifi_tune` | Better station defaults for mesh/Wi-Fi 6 routers, working captive-portal DNS. |
| `cmd_wifi` | `wifi` console command. |
| `mpx_selftest` | `selftest`, `imu`, `button` bring-up commands (never move a servo). |
| `mpx_util` | In-RAM log ring and skill trace ring behind `/v1/logs` and `/v1/trace`. |
| `gen_bmgr_codes` | Generated by `idf.py bmgr`. Never edit by hand. |

---

## 7. The phone app and the HTTP API

**Phone app:** `components/http_server/frontend_pwa_mpv2/` (Svelte 5 + Vite +
Tailwind). Tabs: Home (gamepad, quick actions), Skills, Store, Chat, Settings
(Wi-Fi, hotspot, AI & Voice, Display, Calibration, MangDang Servo Studio, Rest
when idle, Files). `npm run build` writes `www/` (+ `.gz`), which the firmware
build copies into the **system** partition, so it is served from flash, not
compiled into the firmware.

`/settings` serves ESP-Claw's own SolidJS settings UI (`frontend_source/`).
`frontend_pwa/` is the older mp4-claw app and is **not** shipped.

Main `/v1` routes (all JSON; full list in the code guide):

| Area | Routes |
|---|---|
| Robot | `/v1/robot/gait`, `/joy`, `/look`, `/status`, `/config`, `/calibrate`, `/moves`, `/movements`, `/power` |
| Servo Studio | `/v1/studio/mode`, `status`, `scan`, `dump`, `set`, `setall`, `save`, `restore`, `direct`, `live`, `temps` |
| Skills | `/v1/skills/list`, `registry`, `status`, `run`, `stop`, `upload`, `uninstall`, `safe-mode/clear`; `/v1/logs`, `/v1/trace` |
| Display | `/v1/display`, `/v1/display/gif?id=` |
| Files / Lua | `/v1/fs/list`, `info`, `read`, `delete`; `/v1/lua/run`, `enqueue`, `save`, `read`, `list`, `delete` |
| Wi-Fi / AI | `/v1/wifi/status`, `scan`, `connect`, `disconnect`, `forget`, `ap-config`; `/v1/ai` |
| Raspberry Pi | `/v1/pi/status` |
| Store | `/v1/gateway/config`, `/v1/marketplace/*` (proxied to the MangDang gateway) |
| Chat | WebSocket `/v1/chat/ui` → the on-device agent |

The server allows 12 open sockets and the app keeps one request in flight per
screen; see the comments in `http_server_core.c` and `ServoStudio.svelte`.

---

## 8. Voice

- **Wake:** "Hi ESP" (esp-sr WakeNet9, models in the `model` partition), or
  press the WAKE button (GPIO21). Echo cancellation uses the speaker
  reference on mic channel 0, so the robot can be interrupted while talking.
- **Talk:** audio goes to the **MangDang voice gateway** (xiaozhi protocol,
  Opus 16 kHz up / 24 kHz down), which does speech-to-text, the reply and
  text-to-speech. The robot provisions itself on first use (`MP4_VOICE_OTA_URL`);
  no setup needed.
- **Robot actions by voice:** the gateway calls the robot's tools over MCP on
  the same socket (`mpx_mcp_ws`): display, robot, skills, plus `ask_robot_agent`
  for anything else.
- **AI mode** (Settings → AI & Voice, or console `ai`):
  `mangdang` (default, gateway LLM with a per-robot token), `own` (your own
  LLM key; the on-device agent answers voice too), `off`. Changing it restarts
  the robot.
- After a reply it keeps listening for follow-ups for a while, then goes back
  to waiting for the wake word. Music pauses whenever voice is active.

---

## 9. Screen: face, moods and GIFs

- The face is the home screen: two eyes that blink, follow the IMU, and react
  (shake → dizzy, pick-up → surprised, still for 45 s → sleepy…).
- 17 moods the agent can ask for (`display_show_emotion`), and each named move
  plays its own expression track (scared eyes for `cower`, etc.).
- **Display page** in the app plays a GIF instead of the eyes (built-ins
  `blink`, `smile`, `sleepy`, plus your uploads), looped or once. The firmware
  has its own GIF player on top of LVGL's decoder, because LVGL 9.5's `lv_gif`
  mishandles "optimized" GIFs (frames that only store the changed pixels).
- Console `display`: `flip`, `mirror`, `shift x y`, `test`, `talk on|off`,
  `eyes …`. Screen is turned 180° by default.

---

## 10. Skills: three different kinds

| Kind | What it is | Where it lives | Run by |
|---|---|---|---|
| **ESP-Claw skills** | `SKILL.md` documents that teach the agent how to use its tools | built-in: `/system/skills`; user: `/fatfs/skills` | the agent reads them |
| **WASM skills** (`.wasm`, encrypted `.mpxe`) | Compiled robot programs (MPX SDK / `mpx-cli`), can drive servos directly | `/fatfs/mpx_skills/` | `mpx_wasm`: app, agent, store, events, autorun |
| **Lua scripts** | Scripts for ESP-Claw's Lua runtime (LVGL drawing, GPIO…) | `/fatfs/scripts/` | `cap_lua`, `/v1/lua/*` |

`.mpxe` skills from the Store are encrypted for one robot (`MP4_ROBOT_UUID`).
If a skill set to autorun crashes the robot 3 boots in a row, autorun goes into
**safe mode**; clear it with `POST /v1/skills/safe-mode/clear` or from the app.

---

## 11. Serial console commands

Console is UART0 (USB-serial, 115200).

| Command | What it does |
|---|---|
| `selftest [--system --i2c --imu --servo --display --backlight --buttons]` | Bring-up checks; never moves a servo |
| `imu [watch\|orient\|cal]`, `button` | IMU readings / compass, button edges |
| `servo rest <s>\|rest off\|sleep\|wake` | Rest-when-idle and power state |
| `display info\|flip\|mirror\|reset\|test [s]\|shift x y\|talk on\|off\|eyes …` | Screen orientation and face options |
| `wifi --status\|--scan\|--set --ssid X [--password Y] [--apply]` | Wi-Fi |
| `ai [mangdang\|own\|off]` | AI mode (saves and restarts) |
| `audio info\|rec\|tone\|play\|vol\|gain\|tune` | Mic / speaker tests and levels |
| `voice provision\|connect\|talk\|info…`, `wake auto on\|off\|info\|levels`, `mcp call\|info` | Voice pipeline |
| `music play [genre\|url]\|stop\|list\|level` | Radio |
| `pi [status\|json]` | Raspberry Pi (ROS) link state, counters, the Pi's IP, music and camera stats |

Plus ESP-Claw's own commands (agent, memory, files…).

---

## 12. Configuration (Kconfig) and saved settings (NVS)

`idf.py menuconfig` → **App Config** (defined in `main/Kconfig.projbuild`):

| Option | Default | Meaning |
|---|---|---|
| `MP4_ROBOT_ENABLE` | y on the robot board | Builds all robot components |
| `MP4_ROBOT_UUID` | `MPX-DOG-01` | Store identity, `.mpxe` key binding |
| `MP4_VOICE_ENABLE` / `MP4_VOICE_AUTO_LISTEN` | y / y | Voice, hands-free from boot |
| `MP4_VOICE_OTA_URL` | `http://35.220.215.160:8000/xiaozhi/ota/` | Voice gateway provisioning |
| `MP4_GATEWAY_HOST` / `PORT` | `35.220.215.160` / 8080 | Skill store gateway |
| `MP4_GATEWAY_LLM_URL` | `http://35.220.215.160:8000/v1/llm` | Agent LLM in `mangdang` mode |
| `MP4_SKILL_AES_KEY_HEX` | test key | **Replace for production**; refused in release builds |
| `MP4_MCP_GROUPS` | `cap_display cap_robot cap_mpx_skill` | Tools exposed to the voice gateway |
| `MP4_DISPLAY_FLIP180_DEFAULT` | y | Screen turned 180° |
| `MP4_DISPLAY_BUFFER_LINES` | 8 | LVGL draw buffer height |
| `MP4_PI_LINK_ENABLE` / `UART_BAUD` / `TIMEOUT_MS` / `REQUIRE_HOST_PIN` / `STATE_HZ` | y / 921600 / 300 / y / 50 | Pi (ROS) link, same as mpv2's `APP_PI_LINK_*` |
| `MP4_EVENT_ROUTER_STACK` | 16384 | Raised agent event-router stack |

Saved settings (NVS namespaces): `app` (Wi-Fi, LLM, AI mode…), `robot`
(walk config, calibration, servo current caps), `display` (brightness,
orientation, shift, eyes, GIF), `voice` (volume, speaker tuning), `mpx_voice`
(gateway URL + token), `music` (level), `mpx` (autorun failure count).

---

## 13. Hardware pins

MP4 ESP32 CORE (`boards/mp4_esp32_core/README.md` and
`components/mpx_robot/src/robot/board_config.h` have the details):

| Function | GPIO |
|---|---|
| Servo SPI (SPI2) MOSI / CLK / MISO | 6 / 16 / 17 |
| Servo board CS: FR (CN4) / FL (CN5) / RR (CN3) / RL (CN6) | 7 / 4 / 15 / 5 |
| LCD ST7789 320×240 (SPI3) CS / DC / MOSI / SCK / backlight | 9 / 39 / 40 / 41 / 42 |
| I2C SDA / SCL (IMU BMI270 0x68, mic codec ES7210) | 1 / 2 |
| IMU INT1 | 10 |
| I2S MCLK / BCLK / WS / DIN (mic) / DOUT (speaker) | 38 / 14 / 13 / 12 / 45 |
| Buttons BOOT / WAKE | 0 / 21 |
| J-PI header: Pi link TX / RX / HOST_PRESENT / ESP_SLAVE | 18 / 8 / 3 / 11 |
| Console UART0 (CH340K) | 43 / 44 |

Servo ids: 1–3 front right, 4–6 front left, 7–9 rear right, 10–12 rear left
(hip, thigh, calf).

---

## 14. Rules for working on this code

From [AGENTS.md](AGENTS.md):

- **Never edit `third-party/esp-claw/`** (or `managed_components/`). Extend
  through capability groups, skills, Lua modules, board overlays, or a
  component that shadows ESP-Claw's by name (as `http_server` does).
- Rerun `idf.py bmgr -c ./boards -b mp4_esp32_core` after changing anything in `boards/`.
- PSRAM is load-bearing (the firmware runs from PSRAM); internal RAM is tight
  after voice starts. Check `main_log_heap` lines when adding features.
- A `REQUIRES` inside `if(CONFIG_…)` in a component's CMakeLists is not seen
  early enough. Use `idf_component_optional_requires(PRIVATE …)`.
- Block comments must not contain `/*` (`-Werror=comment`); `tools/check_comments.py` finds them.
- After changing the phone app, bump `CACHE_VERSION` in `public/sw.js` so
  phones load the new version.

---

## 15. Known gaps and stale notes

- `MP4_ROBOT_SERVO_BOARD_VARIANT` exists in Kconfig but the servo driver uses
  a hard-coded variant 1.
- Some older comments disagree with the code: the `mpx_robot/README.md` bus
  and CS notes, the "74 host functions" count (70 in the table), the IMU
  "20 Hz" notes (it polls at 100 Hz), the display skill docs ("four tools",
  "ten expressions") and `docs/voice.md` ("nothing built yet"). The code is
  the authority.
- `docs/animations.md` describes an MPXA animation player that is not
  written; the Display page plays GIFs.

---

License: Apache-2.0 (see [LICENSE](LICENSE)). ESP-Claw and managed components
carry their own licenses.
