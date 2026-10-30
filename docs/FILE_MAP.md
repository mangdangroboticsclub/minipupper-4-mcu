# File map

Where everything lives and what it does. This is a map, not a manual: for how
the pieces work together read [.cursor/NOTES.md](../.cursor/NOTES.md) (the
firmware) and [.cursor/PORT_NOTES.md](../.cursor/PORT_NOTES.md) (what was
ported from mpv2-claw and why); for bench bring-up read
[NEW_BOARD_BRINGUP.md](./NEW_BOARD_BRINGUP.md).

The firmware is a single ESP-IDF app (`main/`, component `MPX-Dog`) that runs
everything on an ESP32-S3: the robot, the screen, audio, voice, Wi-Fi, an HTTP
server, a Lua VM, a WASM skill sandbox, an on-device agent, and the phone app
it serves. The only UI is that phone app (a Svelte PWA served from flash).

```
Phone / browser  ──  HTTP /v1 + WS /v1/chat/ui  ──►  ESP32-S3
  (pwa-claw/)                                         ├─ network/   HTTP, Wi-Fi, chat, marketplace
                                                      ├─ robot/     servos, gait, IK, moves
Cloud gateway (optional)                              ├─ display/   ST7789 face + GIFs
  ▲ encrypted WS                                      ├─ voice/     wake word, gateway, music
  │                                                   ├─ agent/     on-device agent (ESP-Claw)
main/network/chat_ws.cc                               ├─ lua/, wasm/, sdk/   scripting
                                                      └─ pi/        Raspberry Pi / ROS 2 link
```

---

## Top level

| Path | What it is |
|---|---|
| [CMakeLists.txt](../CMakeLists.txt) | ESP-IDF project entry point. Declares the `MPX-Dog` project, builds the PWA (`npm run build` in `pwa-claw/`) as a dependency of the app, patches an esp-sr console-encoding bug, and stages + packs the agent's read-only `system` LittleFS partition. |
| [main/](../main) | The firmware itself — one component with all of its source under subdirectories (see [main/](#main) below). |
| [pwa-claw/](../pwa-claw) | The phone app (Svelte 5 + Vite + Tailwind + three.js), built to `www/` and embedded into the firmware. The only app tracked in git. |
| [third-party/esp-claw/](../third-party/esp-claw) | Vendored copy of the ESP-Claw agent framework's `components/` tree. **Never edited** — where it needed bending, it was done from `main/agent/claw_overrides.c` and `main/Kconfig.projbuild`. |
| [components/](../components) | One component, `mpx_identity`: the robot ID and AES key (format, eFuse record, key derivation, sealing, `@MPXPROV` provisioning, first-connect enrolment). Everything else lives under `main/`. |
| [agent_fs/](../agent_fs) | Source tree for the agent's read-only `system` partition. `system/.recovery/` seeds `memory/` (`identity.md`, `soul.md`, `user.md`), `router_rules/` and `scheduler/`; copied into DATA at boot when a file is missing. |
| [docs/](../docs) | Documentation: this file, the pilot-device guide ([PILOT_DEVICE_GUIDE.md](./PILOT_DEVICE_GUIDE.md)), how identity works ([IDENTITY_AND_KEYS.md](./IDENTITY_AND_KEYS.md)), the enrolment flow, the milestone plan, and the new-board bring-up guide. |
| [components/mpx_identity/](../components/mpx_identity) | The identity component: ID format and checksum (`mpx_id.c`), key derivation (`mpx_derive.c`), eFuse record and key blocks (`mpx_efuse*.c`), RSA sealing (`mpx_seal*.c`), the UART provisioning listener (`mpx_provision.c`), first-connect enrolment (`mpx_enroll.c`), and the `mpx_identity` façade every other module calls. |
| [tools/](../tools) | Host-side helpers, not part of the firmware build (see [tools/](#tools) below). |
| [partitions.csv](../partitions.csv) | Flash partition table: `nvs`, `phy_init`, `factory` (app), `model`, `system` (read-only agent LittleFS), `storage` (writable LittleFS). |
| [sdkconfig](../sdkconfig) | Generated, machine-specific ESP-IDF configuration. `sdkconfig.defaults` restores it. |
| [sdkconfig.defaults](../sdkconfig.defaults) | The load-bearing settings that must survive a config regeneration: PSRAM/XIP, Wi-Fi RAM budget, socket counts, ESP-Claw options, the eFuse custom table. |
| [sdkconfig.defaults.pilot](../sdkconfig.defaults.pilot) | Adds to the above for a pilot/bench image: eFuse identity source and the `@MPXPROV` listener. Never ship. |
| [sdkconfig.old](../sdkconfig.old) | Backup auto-written by `menuconfig`. Not used by the build. |
| [dependencies.lock](../dependencies.lock) | Generated lockfile for the ESP-IDF component manager. |
| [managed_components/](../managed_components) | Third-party components downloaded by the component manager. Generated, gitignored; do not edit. |
| [build/](../build) | Generated build output. Gitignored and empty until the first build. |
| [.devcontainer/](../.devcontainer) | Pinned ESP-IDF Docker dev environment (`Dockerfile`, `devcontainer.json`, `post-create.sh` installs `pwa-claw/`'s packages, `README.md`). |
| [.cursor/](../.cursor) | `NOTES.md` (how the firmware works) and `PORT_NOTES.md` (the porting log). Reference material, not code. |
| [.clangd](../.clangd) | clangd config (strips `-f*`/`-m*` so the IDE can parse Xtensa/RISC-V flags). |
| [.vscode/](../.vscode) | Editor settings. |
| [README.md](../README.md), [LICENSE](../LICENSE) | Project readme (a stub) and license. |

> `pwa/` and `pwa-redesign/` may exist locally but are **not tracked** — they
> are gitignored leftovers from before the app was consolidated into
> `pwa-claw/`. Nothing in the build references them.

---

## main/

The firmware component. `main/CMakeLists.txt` lists every source file
explicitly, gates the agent and voice sources on `CONFIG_APP_AGENT_ENABLE` /
`CONFIG_APP_VOICE_ENABLE`, and embeds the PWA assets as `EMBED_FILES`.

| File | What it does |
|---|---|
| [main.cc](../main/main.cc) | `app_main()` — the boot sequence, in order: NVS, robot HAL, screen, audio, Pi link, LittleFS, Wi-Fi AP/STA, deploy PWA assets, rescan skills, HTTP server, chat, WASM sandbox, Lua, agent, voice. Also wires the robot's skill hooks and the Pi master observer. |
| [CMakeLists.txt](../main/CMakeLists.txt) | Source list, conditional sources, embedded assets, `REQUIRES`, the Lua 32-bit def, and the `--wrap` of ESP-Claw's event-router init. |
| [idf_component.yml](../main/idf_component.yml) | Component-manager manifest: every managed dependency and the vendored ESP-Claw components as `path:` entries. Pins the IDF range. |
| [Kconfig.projbuild](../main/Kconfig.projbuild) | The "MPX Dog App Configuration" menu: AP/STA defaults, cloud gateway, robot UUID and AES key, and the `APP_AUDIO_ENABLE`, `APP_PI_LINK_ENABLE`, `APP_RL_*`, `APP_DISPLAY_ENABLE`, `APP_VOICE_ENABLE`, `APP_AGENT_ENABLE` switches. |

### main/agent/ — the on-device agent

ESP-Claw (the vendored framework) used as a library: mpv2-claw's `main.c`
agent section, in one place.

| File | What it does |
|---|---|
| [agent_runtime.{h,cc}](../main/agent/agent_runtime.cc) | Brings the agent up and down: filesystem roots, LLM config, capabilities, `app_claw_start()`, and the "web" chat channel. `submit()` takes a chat message from `chat_ws.cc`; replies return through one outbound callback. Compiles to stubs when the agent is off. |
| [agent_config.{h,cc}](../main/agent/agent_config.cc) | The owner's "AI & Voice" choice (`mangdang` / `own` / `off`, NVS `voice/ai_mode`) and the stored LLM account (NVS `agent_llm`). Read by both `voice_glue` and `agent_runtime`; applied live from `POST /v1/ai`. |
| [agent_fs.{h,cc}](../main/agent/agent_fs.cc) | Mounts the `system` partition, creates the DATA tree (`/fs/agent`), seeds missing `.recovery` files, and points `claw_paths` at both roots. |
| [agent_caps.{h,cc}](../main/agent/agent_caps.cc) | Turns `robot_tools`' table into ESP-Claw capability groups (`cap_robot`, `cap_display`, `cap_mpx_skill`, `cap_music`) so the `main/skills/*/SKILL.md` files unlock the right tools. |
| [robot_tools.{h,cc}](../main/agent/robot_tools.cc) | The robot's tools as one table (name, group, description, JSON schema, body) shared by the agent and the voice MCP bridge: robot_move/pose/get_state/set_config, `display_*`, `display_draw`, skill list/run/stop, music play/stop. `mpx_run_skill` blocks until the skill has finished, so the model's next turn starts after the robot is free again. |
| [claw_overrides.c](../main/agent/claw_overrides.c) | `__wrap_claw_event_router_init()` — raises ESP-Claw's 8 KB event-router stack via the linker, without editing the vendored tree. |

### main/audio/ — microphones and speaker

| File | What it does |
|---|---|
| [mpx_audio.{h,c}](../main/audio/mpx_audio.c) | Direct I2S/ES7210/MAX98357A init, capture and playback, volume. The layer voice and Pi audio sit on. |
| [mpx_audio_tune.{h,c}](../main/audio/mpx_audio_tune.c) | Private speaker-tuning stage every voice and music block runs through (public knobs stay in `mpx_audio.h`). |

### main/display/ — the screen

mpv2-claw's `cap_display` on a small ST7789 + `esp_lvgl_port` service.

| File | What it does |
|---|---|
| [display_hw.{h,c}](../main/display/display_hw.c) | The whole "display service": ST7789 on SPI3, LEDC backlight, LVGL init, lock/unlock and the panel handle. Compiled (inert) when the display is off. |
| [cap_display.{h,c}](../main/display/cap_display.c) | The face API used by the rest of the firmware: show text, emotion, colors, brightness, orientation, and `cap_display_draw()` (the agent's 320×240 canvas). |
| [cap_display_face.{h,c}](../main/display/cap_display_face.c) | The eyes: moods, move faces, and the GIF player on LVGL's AnimatedGIF. |
| [cap_display_orient.c](../main/display/cap_display_orient.c) | Orientation/rotate settings (flip, mirror, shift). |
| [display_glue.{h,cc}](../main/display/display_glue.cc) | What the rest of the firmware tells the screen: the IMU feed for the alive eyes, the network line, the Pi's address and ROS badge, and the Pi camera sink. `main.cc` calls `start_face()` / `start_animations()` / `start_events()`. |
| [display_assets.{h,cc}](../main/display/display_assets.cc) | Writes the embedded blink/smile/sleepy GIFs into `/fs/display` when missing or a different size. |
| [assets/](../main/display/assets) | The three GIFs embedded into the app. |

### main/fs/ — storage

| File | What it does |
|---|---|
| [littlefs_manager.{h,cc}](../main/fs/littlefs_manager.cc) | Mounts/formats the LittleFS partition (`storage`, at `/fs`) and exposes file list/read/write/delete/stats used by the HTTP file API, skills and the agent. |

### main/lua/ — the Lua scripting VM

| File | What it does |
|---|---|
| [lua_vm.{h,cc}](../main/lua/lua_vm.cc) | Lua 5.5 state (PSRAM heap), the serialising lock, the instruction/timeout count hook, and run entry points for `/v1/lua/*` and the chat worker. |
| [lua_bindings.{h,cc}](../main/lua/lua_bindings.cc) | The `robot`, `fs`, `wasm` and `crypto` modules exposed to Lua. |
| [lua_display.cc](../main/lua/lua_display.cc) | The `display` module (clear/pixel/rect/circle/line/arc/text/blit/show/face/…) over `cap_display.c`. |

### main/network/ — Wi-Fi, HTTP, chat, marketplace

| File | What it does |
|---|---|
| [wifi_ap.{h,cc}](../main/network/wifi_ap.cc) | Soft-AP (`192.168.2.1`, DHCP, NVS-customisable SSID/password); AP config changes save then reboot. |
| [wifi_sta.{h,cc}](../main/network/wifi_sta.cc) | Station mode: saved credentials, connect/disconnect/forget, state reporting, scanning. |
| [http_server.{h,cc}](../main/network/http_server.cc) | `esp_http_server` on port 80: all `/v1/*` handlers, the static-file handler (`.gz` first, SPA fallback) and the socket reaper. Also hosts the shared skill-run helpers — including the `voice_glue::hold()`/`release()` pair that gives every skill run the robot to itself, and `skill_wait()` so the agent's tool call stays inside the run. |
| [www_assets.{h,cc}](../main/network/www_assets.cc) | Copies the embedded PWA assets (`EMBED_FILES`) into `/fs/www` at boot; `www_assets.h` just declares `deploy_www_assets()`. |
| [crypto.{h,cc}](../main/network/crypto.cc) | AES-256-GCM helpers and the robot-key load used by the chat frames and MPXE skills. |
| [chat_ws.{h,cc}](../main/network/chat_ws.cc) | The chat pipeline: `/v1/chat/ui` (PWA WebSocket), the echo/cloud modes, the encrypted upstream frame, permission prompts and the Lua worker. A message that arrives while a skill runs is refused (the skill has the robot, and voice is held), not queued. |
| [marketplace_proxy.{h,cc}](../main/network/marketplace_proxy.cc) | Passes `/v1/marketplace/*` requests through to the cloud gateway over STA. |
| [http_display.{h,cc}](../main/network/http_display.cc) | `/v1/display` and `/v1/display/gif` routes. Registered only when the display is enabled. |
| [http_voice.{h,cc}](../main/network/http_voice.cc) | `/v1/voice`, `/v1/voice/tools` and the `/v1/ai` routes — the console commands mpv2-claw had, as HTTP. `/v1/voice` also takes `hold` (same counted hold a skill run takes) and `task_priority` (1..24, the wake front end's feed/fetch tasks, not saved). |

### main/pi/ — the Raspberry Pi / ROS 2 link

| File | What it does |
|---|---|
| [pi_link.{h,cc}](../main/pi/pi_link.cc) | The J-PI UART link: framing, heartbeat, slave/monitor mode, `robot::master::begin()`, telemetry, and `/v1/pi/status`. The Pi is optional; losing it returns control to the robot. |
| [pi_protocol.h](../main/pi/pi_protocol.h) | **The wire format** shared with the Pi's Python bridge: pins, frame layout, message types, version. Change alongside the Python side. |
| [pi_audio.{h,cc}](../main/pi/pi_audio.cc) | The Pi's streamed 16 kHz PCM, buffered in PSRAM and played through `mpx_audio`. |
| [pi_video.{h,cc}](../main/pi/pi_video.cc) | Reassembles the Pi's JPEG camera chunks and hands complete frames to the registered display sink (newest frame wins). |
| [rl_link.{h,cc}](../main/pi/rl_link.cc) | The RL-policy host protocol (mp4-link) on the same UART: sessions, watchdog, guards, HOLD/ARM. |
| [RL_LINK.md](../main/pi/RL_LINK.md) | The RL link's protocol and state-machine documentation. |

### main/platform/

| File | What it does |
|---|---|
| [shared_i2c.{h,cc}](../main/platform/shared_i2c.cc) | The one I2C master bus (BMI270 IMU + ES7210 codec + BMM150) — named `shared_i2c` to stop it shadowing espressif's legacy `i2c_bus.h`. |

### main/robot/ — the robot

| File | What it does |
|---|---|
| [robot.{h,cc}](../main/robot/robot.cc) | The control loop and public API: goal buffer, `flush()`, gait commands, joystick, body attitude, `look`, rest/wake, host/master modes, config + offsets in NVS. |
| [robot.h](../main/robot/robot.h) | The C++ API for everything above (namespace `robot`). |
| [mpx_robot.h](../main/robot/mpx_robot.h) + [mpx_robot_c_api.cc](../main/robot/mpx_robot_c_api.cc) | The C facade over the C++ robot, used by the HTTP handlers, Lua/WASM bindings, capability groups and the IMU/display glue, plus the skill-hooks seam. |
| [driver_board.{h,c}](../main/robot/driver_board.c) | The four AT32F413 servo driver boards over SPI2: modes, params, raw path for RL, feedback. |
| [board_config.h](../main/robot/board_config.h) | **The pin map** — single source of truth for every GPIO, SPI/I2S/UART assignment. |
| [stanford_gait.{h,cc}](../main/robot/stanford_gait.cc) | The Stanford trot gait (the joystick-driven walk). |
| [stanford_kinematics.{h,cc}](../main/robot/stanford_kinematics.cc) | Leg inverse kinematics. |
| [imu.{h,cc}](../main/robot/imu.cc) | BMI270 driver on the shared I2C bus; 200 Hz polling with `seq`/`t_us` for the control loop. |
| [alive.{h,c}](../main/robot/alive.c) | Pure-C IMU reflexes: "picked up → cartoon running" and the push reflex. |
| [mpe_tables.h](../main/robot/mpe_tables.h) | Overview of the minipupperesp reference motion tables copied in below. |
| [moves_mpe.inc](../main/robot/moves_mpe.inc) | The reference firmware's moves, included into `robot.cc` so they share the goal buffer and config. |
| [longjump_table.h](../main/robot/longjump_table.h) | Generated long/far jump tracks. Do not edit by hand. |
| [trajopt_jump.h](../main/robot/trajopt_jump.h) | Generated spin jumps and character moves. Do not edit by hand. |

### main/sdk/ — the WASM host ABI

| File | What it does |
|---|---|
| [wasm_host_functions.{h,cc}](../main/sdk/wasm_host_functions.cc) | The `env` module a skill imports: `MPX_ABI_VERSION`, the `robot_*` / `mpx_*` native symbols and their implementations, plus the `control_owner` query used by `main.cc`. ABI v7 adds the microphone (`mpx_mic_start` / `mpx_mic_read` / `mpx_mic_stop`), which a skill can open because the run holds voice back. |

### main/skills/ — the agent's skills

Each `<id>/SKILL.md` is collected by `skill_builder` into the read-only
`system` partition; its frontmatter `cap_groups` names the capability groups
that unlock its tools.

| File | What it does |
|---|---|
| [mpx_robot/SKILL.md](../main/skills/mpx_robot/SKILL.md) | Drive the robot: walk, turn, hold a pose, read gait/IMU/servo state (`cap_robot`). |
| [mpx_skills/SKILL.md](../main/skills/mpx_skills/SKILL.md) | List, run and stop compiled WASM skills (`cap_mpx_skill`). |
| [mpx_display/SKILL.md](../main/skills/mpx_display/SKILL.md) | Show text and faces, control the backlight (`cap_display`). |
| [mpx_display_draw/SKILL.md](../main/skills/mpx_display_draw/SKILL.md) | Draw anything on the 320×240 screen with `display_draw`. |
| [mpx_music/SKILL.md](../main/skills/mpx_music/SKILL.md) | Internet radio by genre/mood or a direct audio URL (`cap_music`). |

### main/util/

| File | What it does |
|---|---|
| [trace_ring.{h,cc}](../main/util/trace_ring.cc) | `mpx_trace()`'s named-float ring (256 entries, internal RAM), read by `/v1/trace` and `mpx-cli trace` to plot what a running skill is doing. |

### main/voice/ — wake word, gateway, music

| File | What it does |
|---|---|
| [voice_glue.{h,cc}](../main/voice/voice_glue.cc) | mpv2-claw's `main.c` voice section in one module: brings the voice stack up/down per the AI mode, reports status, and hands the Pi's master changes in. Also the hold a skill run takes (`hold()`/`release()`, counted, with a manual `POST /v1/voice {"hold":true}` on the same counter) and the wake front end's task priority. |
| [voice_tools.{h,cc}](../main/voice/voice_tools.cc) | The MCP transport (JSON-RPC paging over the voice socket) that exposes `robot_tools`' table to the MangDang gateway's model. |
| [mpx_voice_link.h](../main/voice/mpx_voice_link.h) | The voice-link API and configuration (the xiaozhi protocol, gateway URL/token). |
| [mpx_voice_ws.c](../main/voice/mpx_voice_ws.c) | The WebSocket link to the voice gateway. |
| [mpx_voice_stream.c](../main/voice/mpx_voice_stream.c) | Mic → Opus → socket, and socket → speaker streaming. |
| [mpx_voice_codec.cc](../main/voice/mpx_voice_codec.cc) | Opus encode/decode wrappers. |
| [mpx_voice_wake.{h,c}](../main/voice/mpx_voice_wake.c) | esp-sr wake word (AFE setup, wake detection, accessors for status). |
| [mpx_voice_ota.c](../main/voice/mpx_voice_ota.c) | First-wake-word provisioning of the gateway URL/token (OTA identification). |
| [cap_music.{h,c}](../main/voice/cap_music.c) | Music playback (internet radio by genre, or a URL) sharing the speaker with voice. |

### main/wasm/ — the skill sandbox

| File | What it does |
|---|---|
| [wasm_sandbox.{h,cc}](../main/wasm/wasm_sandbox.cc) | The WAMR runtime: run lifecycle (`on_start`/`on_tick`/`on_stop`), the one-skill-at-a-time rule, watchdog/cancel, and the native-symbol table registration. Every run executes on a pthread whose stack lives in PSRAM (`esp_pthread_cfg_t.stack_alloc_caps`), because internal RAM has no 16 KB contiguous block once voice is up — safe only while `CONFIG_SPIRAM_XIP_FROM_PSRAM` keeps the flash cache on. |
| [wasm_decrypt.{h,cc}](../main/wasm/wasm_decrypt.cc) | MPXE encrypted-skill support: magic/version checks, key unwrap with the robot key + UUID, and WASM decryption. |
| [registry.{h,cc}](../main/wasm/registry.cc) | Reads the `mpx` custom section of every `.wasm`/`.mpxe` on LittleFS (slug, provides_gait, abi, autorun, behaviour, `on[]`); rescanned at boot, after upload and after delete. |

---

## tools/

| Path | What it does |
|---|---|
| [check_comments.py](../tools/check_comments.py) | House style checker for source comments. |
| [mpx_id.py](../tools/mpx_id.py) | Robot ID reference implementation: `new`, `check`, `derive`, `vectors`, `selftest`. The device code and the test vectors are checked against it. |
| [mpx_seal.py](../tools/mpx_seal.py) | Reference for the eFuse record and the sealed-envelope layouts; generates `tools/vectors/seal.json`. |
| [mpx_provision.py](../tools/mpx_provision.py) | Bench station: talks `@MPXPROV` JSON over UART0 — `scan`, `status`, `provision`, `reseal`, `lock`, `unseal`, `verify`. |
| [mpx_dev_identity.py](../tools/mpx_dev_identity.py) | Prints a matching dev ID + key pair for `APP_ROBOT_UUID` / `APP_CHAT_AES_KEY_HEX`. |
| [host_tests/](../tools/host_tests) | Host unit tests (no board, no ESP-IDF): `idf.py host-test`. Covers ID format, derivation, eFuse record, seal layout, provisioning JSON and state table. |
| [ci_check_sdkconfig.sh](../tools/ci_check_sdkconfig.sh) | Compares a regenerated `sdkconfig` against the developer's copy; used to prove `sdkconfig.defaults` is complete. |
| [vectors/](../tools/vectors) | Frozen `identity.json` and `seal.json` vectors, compiled into the host tests. |
| [secrets/](../tools/secrets) | Gitignored. The dev seal private key lives here. |
| [pi/pi_announce.py](../tools/pi/pi_announce.py) | Runs on the Raspberry Pi: sends `HOST_INFO` so the robot shows the Pi's address without heartbeating. |
| [pi/pi-announce.service](../tools/pi/pi-announce.service) | systemd unit for the above. |
| [pi/README.md](../tools/pi/README.md) | Pi-side wiring, bridge parameters and checks. |

---

## Generated / third-party — do not edit

- `managed_components/` — ESP-IDF component manager output.
- `build/` — build output and the staged `system_fs/`.
- `dependencies.lock` — component manager lockfile.
- `sdkconfig`, `sdkconfig.old` — generated **and no longer tracked**; put durable settings in `sdkconfig.defaults`. A fresh clone must pass `-DIDF_TARGET=esp32s3`.
- `third-party/esp-claw/` — vendored agent framework.
- `pwa-claw/www/`, `pwa-claw/node_modules/` — PWA build output and dependencies.
