# Porting mpv2-claw features into mpx-dog (branch `local-claw`)

Running log of what was ported, how, and why. Companion to `NOTES.md`
(how mpx-dog works) and `mpv2-claw/.cursor/NOTES.md` (how mpv2-claw works,
with a feature-parity table in its section 17).

## Approach

mpv2-claw is ESP-Claw (an agent framework) plus product components. mpx-dog is
a single ESP-IDF app with its own HTTP server, Lua VM and cloud chat relay.
Rather than replace mpx-dog's shell with ESP-Claw, features are ported one at a
time **into mpx-dog's existing structure**, keeping its `/v1` API shapes and
adding mpv2-claw's new routes beside them. The phone app is taken from
mpv2-claw (`frontend_pwa_mpv2`) and adapted to detect which firmware features
exist, so one app build works on both the old and the new firmware.

Phases:

| Phase | Scope | Status |
|---|---|---|
| 1 | mpv2 phone app as the default UI; firmware routes it needs that have no hardware/agent dependency: `/v1/robot/moves`, `/v1/robot/look`, `/v1/robot/power`, `/v1/wifi/scan`, `/v1/skills/registry|status|stop`; skill manifest registry; skill-provided gait names | this branch |
| 2 | Robot additions from mpv2: extra moves (trajopt tables, backflips, `alive`), gain presets, Pi/ROS link (`robot::master`), `/v1/pi/status` | this branch (builds; bench checks owed) |
| 3 | LCD face + GIFs (`/v1/display*`), needs LVGL + ST7789 bring-up | this branch (builds; first light owed) |
| 4 | Audio + voice (esp-sr wake word, MangDang voice gateway, music, voice tools), `/v1/ai` (mangdang/off), `/v1/voice` | this branch (builds, 0 warnings; bench checks owed) |
| 5 | On-device agent (ESP-Claw, vendored), `/v1/ai` "own" mode, chat answered locally behind a per-conversation Pupper / Cloud toggle | this branch (builds, 0 warnings; bench checks owed) |
| 6 | Catch-up with mpv2-claw `ede0804` ("new pi link and RL"): RL host link on the Pi UART, Pi link UART/HOST_INFO fixes, raw servo path in the robot core, IMU at 200 Hz / +-16 g, `pi_announce`, PWA updates | this branch (not built here; PWA builds; bench checks owed) |
| 7 | Lua: run lock, enforced timeouts, runtime errors reported; a `display` module on `display_draw`'s canvas | this branch (bench checks owed) |

## Phase 1 changes

### Phone app: `pwa-claw/`

- New folder `pwa-claw/`, copied from
  `mpv2-claw/components/http_server/frontend_pwa_mpv2` (src, public, tooling;
  `node_modules/` and `www/` are gitignored as in the other two app folders).
  Same build (`npm install && npm run build` -> `www/` + `.gz`), same output
  names (`m.js`, `index.css`).
- Kconfig `PWA_DESIGN_CLAW` added and made the **default**; the original and
  redesign apps stay selectable. `sdkconfig` in this branch is switched to it.
- **Code-split chunk.** The app lazy-loads the three.js viewer
  (`import("./pupper3d.js")` in `Calibration.svelte`), which Vite emitted as
  `m-<hash>.js`. mpx-dog embeds a fixed list of files, so a hashed name can
  never be served. `vite.config.js` now uses `chunkFileNames: "m-[name].js"`
  -> stable `m-pupper3d.js`, embedded and routed like the other assets.
  (mpv2-claw's server globs its `www/` folder, so it never hit this.)
- **Flash budget: big files embedded as `.gz` only.** The claw app is
  roughly twice the redesign (two 400 KB JS files, 150 KB CSS, 157 KB model).
  Raw + gz of everything would push the 2.01 MB image past the 2.5 MB
  `factory` slot. For `PWA_DESIGN_CLAW`, `m.js`, `m-pupper3d.js`,
  `index.css` and `pupper_model.json` are embedded as `.gz` only (~310 KB in
  total). `static_handler()` gained a last-resort branch that serves the
  `.gz` even to a client that did not send `Accept-Encoding: gzip`
  (every browser decodes it; the alternative was the SPA fallback returning
  `index.html` for `/m.js`). The other two designs are unchanged.
- `main/CMakeLists.txt`, `main/network/www_assets.cc` and the static routes
  in `http_server.cc` carry the new file list; `favicon.ico` maps to
  `md.svg` for this design too.
- Adaptations in the app (small, each marked with an `mpx-dog:` comment):
  - `SettingsPage.svelte`: the AI section is shown only if `GET /v1/ai`
    answers 200; the Display row only if `GET /v1/display` answers 200; the
    Raspberry Pi section only if `/v1/wifi/status` carries a `pi` object
    (`hasPi` added to the network state in `app.svelte`). On this firmware
    all three are hidden until those phases land.
  - `public/sw.js`: cache version `v9-claw` so phones holding the redesign
    cache reload; `m-pupper3d.js` added to the network-first app shell.
  - `public/manifest.json`: icon points at `eye-open.svg` (same as
    `index.html`); the `icon.svg` it referenced is not shipped by this design.
  - `package.json` name/version, dev proxy default host.
- Not changed: the chat view still understands `command_result` and
  `openclaw_action`, which mpx-dog's cloud chat sends, so the cloud
  permission prompt keeps working. `Controller.svelte` already falls back to
  the eight discrete look gaits when `/v1/robot/look` 404s, and
  `continuousGaits` defaults to "everything is continuous" when
  `/v1/robot/moves` is absent -- both are what the older firmware gets.

### Firmware

- **`/v1/robot/moves`** (`GET`): `{moves:[{name,continuous}]}` from the one
  `GAIT_NAMES` table plus `robot::gait_is_continuous()`. The app uses
  `continuous` to decide whether releasing a button should send `none`.
- **`/v1/robot/look?yaw&pitch`** (`GET`): analog head look. Implemented as
  `robot::look()` on top of the existing `set_body_attitude()` with a slew
  rate (`LOOK_SLEW_DPS` = 120 deg/s) so repeated stick updates glide instead
  of snapping. Yaw +30 = right, pitch +20 = nose up (same signs as the
  discrete look gaits, checked against `LookRight`/`LookUp` in `gait_task`).
  `look(0, 0)` sets `s_look_release`; once the glide reaches neutral the
  BodyAttitude hold drops to `None` by itself, so a released stick leaves
  the robot idle (and lets the rest timer run) rather than holding a pose.
- **`/v1/robot/power`** (`GET`/`POST`): rest/wake. Design: a power state
  machine in the gait task. After `rest_after_s` seconds with no movement
  request (default 300 s, NVS `robot/rest_s`, 0 = never) the idle branch
  calls `lie_down()`: the stand glides from `cfg.height` to `REST_Z` = 40 mm
  over 2 s (cosine ease), then every servo is staged in `DB_MODE_IDLE` and
  committed in one pass. The gait loop then sleeps in 50 ms ticks. Any gait
  command, joystick movement, look, `action:"wake"`, a starting skill or a
  pose claim runs `stand_up()` (1.2 s glide) **before** the command executes,
  so nothing can start a move from the floor. "Activity" has one definition
  (`note_activity()`), called from `send_gait_cmd` (not for `none`),
  `joy_input` (only when the stick is actually deflected), and
  `set_body_attitude`. Rest is refused while Studio or a skill owns the servo
  bus, a WASM skill is running, or a skill holds a pose claim. Numbers copied
  from mpv2-claw.
- **`/v1/wifi/scan`** (`GET`): blocking `esp_wifi_scan_start` in APSTA mode,
  results deduplicated by SSID (strongest kept), `bars` 0-4 from RSSI, same
  JSON as mpv2-claw. Returns `{"ok":false,"error":"busy"}` while a connect
  attempt is in progress (the app retries).
- **Skill registry** (`main/wasm/registry.{h,cc}`, ported verbatim from
  mpv2-claw apart from include paths and skipping `/www/`): reads the `mpx`
  custom section (`slug, provides_gait, abi, autorun, behaviour, on[]`) of
  every `.wasm`/`.mpxe` on LittleFS. Rescanned at boot (`main.cc`, before
  the HTTP server), after upload and after delete. `GET /v1/skills/registry`
  returns it. `POST /v1/robot/gait` now also accepts a skill's
  `provides_gait` name and starts that skill (shared `start_skill_async()`
  helper, extracted from `/v1/skills/run`); unknown names still fall back to
  `none` for the old apps.
- **Slug resolution.** The mpv2 app posts `{skill: <slug>}` to
  `/v1/skills/run` and compares `/v1/skills/status`'s `name` against that
  slug; the older apps post the file name. `resolve_skill_name()` accepts
  either (slug -> registry file; file -> its slug if it has one; bare name
  -> `name.wasm` if that exists) and the status endpoint reports the slug
  when there is one, so both apps see "running" on the right row.
- **`/v1/skills/status`** (`{running,name,safe_mode:false}`) and
  **`POST /v1/skills/stop`** (`wasm::request_stop()`, cooperative), so the
  Skills tab can show and stop the running skill. Autorun, event triggers
  (`on[]`) and safe mode are parsed but not acted on in this phase;
  `safe_mode` is reported `false`.
- `max_uri_handlers` raised 64 -> 80 (route count is load-bearing, see the
  comment in `http_server.cc`).

## Phase 2 changes (robot + Pi link) and 4a (audio layer)

### Robot core: `main/robot/` replaced wholesale

mpv2-claw's `components/mpx_robot/src/robot/` is mpx-dog's robot with two
years of additions on top (its `robot.h` is a strict superset of ours), so
instead of back-porting move by move the whole folder was copied:

- Taken verbatim: `robot.cc`, `robot.h`, `driver_board.c/.h` (params 10-12,
  soft start), `stanford_kinematics.cc`, `board_config.h` (adds the Pi, LCD
  and audio pins), `imu.h`, new `alive.c/.h`, `moves_mpe.inc`,
  `mpe_tables.h`, `longjump_table.h`, `trajopt_jump.h` (1.9 MB of source,
  ~600 KB of flash). `mpx_robot.h` + `mpx_robot_c_api.cc` (the C facade and
  the `mpx_robot_set_skill_hooks()` seam) moved in beside them.
- Kept ours: `imu.cc` (board I2C via `platform/shared_i2c`), `stanford_gait.*`,
  `stanford_kinematics.h`. `imu.cc` now polls at 100 Hz and fills the `seq`
  / `t_us` fields the new `robot.cc` reads, and exposes `imu_mag_ready()`.
- **Sign conventions: mpv2's win** (reverses the phase-1 decision below).
  mpv2's `SK_SIGN` abduction row and strafe sign were verified on hardware
  on 2026-09-28; mpx-dog's were marked "never tested" in its own source.
  `SG_MIRROR_LR` is gone with it. One bench check owed: strafe left/right
  and turn directions.
- **WASM coupling through hooks.** `robot.cc` no longer includes the WASM
  sandbox; `main.cc` registers `s_skill_hooks` (`skill_is_running`,
  `skill_owns_pose`, `request_stop`) before `robot::init()`. The master
  (Pi) observer is `robot::set_master_observer(on_master_changed)`, which
  is the hook point for the display and voice phases.
- **Renamed API, callers updated**: `look` -> `look_input`,
  `power_state()` -> `power_state_name()`, `is_resting()` -> `servos_resting()`,
  `wake()` -> `wake_servos()`. `lua_bindings.cc`'s 46-case gait-name switch
  (which `-Werror=switch` rejected once new gaits existed) is now
  `robot::gait_to_name()`.
- **FreeRTOS tick 100 -> 1000 Hz** (`sdkconfig.defaults`). mpv2's move
  player paces 5 ms frames with `vTaskDelay(1)` in a spin; at 100 Hz every
  frame would take 10 ms and the trajopt moves would play at half speed.
- **Partition table** (`partitions.csv`, custom, 16 MB): factory 6 MB
  (trajopt tables now, LVGL and esp-sr next), new 1 MB `model` SPIFFS slot
  for the esp-sr wake-word model (`CONFIG_MODEL_IN_FLASH`), `storage`
  LittleFS moved to 0x700000. **Reflashing this layout loses the existing
  LittleFS contents** (skills, calibration file, Lua scripts); NVS stays.

### Pi/ROS link: `main/pi/`

`mpx_pi_link` ported as a namespace (`pi_link::`) instead of a C facade:
`pi_protocol.h` verbatim (`A5 5A | len | type | seq | payload | crc16` on
UART1 at 921600, host-present on GPIO 3, slave-out on GPIO 11), `pi_link.cc`
with Kconfig renamed `CONFIG_MP4_PI_LINK_*` -> `CONFIG_APP_PI_LINK_*` and
the firmware string set to `mpx-dog`, `pi_audio.cc` (mic/speaker forwarding,
needs 4a) and `pi_video.cc` (JPEG frames, sink set later by the display).

- HTTP: `/v1/robot/gait`, `/joy`, `/look` answer **423 Locked**
  `{"ok":false,"error":"pi_in_control"}` while the Pi is in slave mode;
  `/v1/robot/status` carries `pi_link`, `/v1/wifi/status` carries
  `pi:{ip}` (which makes the app's Raspberry Pi settings section appear),
  new `GET /v1/pi/status`. `/v1/robot/moves` items gained `note`.
- Kconfig `APP_PI_LINK_ENABLE` (default on), baud / timeout / state rate /
  require-host-pin mirror mpv2's defaults.

### Audio layer (4a): `main/audio/`

`mpx_audio.c/.h` + `mpx_audio_tune.c/.h` ported without the board manager:
I2S0 full duplex (MCLK 38 / BCLK 14 / WS 13 / DIN 12 / DOUT 45) opened
directly with `i2s_new_channel`, ES7210 (8-bit address 0x82) through
`esp_codec_dev` on the shared I2C bus (`hal_shared_i2c_bus()` C accessor
added to `platform/shared_i2c`), MAX98357A written straight to the TX
channel. Data root is `/fs`. Volume in NVS `voice/vol`, tune in NVS
`t_on t_hpf t_gain t_ceil` as in mpv2. Kconfig `APP_AUDIO_ENABLE`. No HTTP
route yet; the phone-facing pieces come with 4b.

### Build result

Zero warnings, 2.37 MB image, 60 % of the 6 MB slot free, `check_comments.py`
clean. Only `pi_link.cc` recompiled on the final pass (the rest had already
built in the run that stopped on the Lua switch).

Bench checks owed for phase 2/4a:

- Strafe/turn direction after the sign change; the trajopt moves
  (`backflip`, `longjump`, `frontflip`) on this chassis at the new tick rate.
- Pi link: `pi_link` state goes standalone -> monitor -> slave with a Pi
  attached; 423 from the app while slave.
- Mic capture and speaker playback (`mpx_audio` smoke test once a route or
  the voice link exists).

## Phase 3 changes (screen)

### `main/display/`: mpv2-claw's `cap_display` on a small `display_hw`

mpv2-claw's display code sits on ESP-Claw's `display_service` (board-manager
panel, `esp_lvgl_adapter`, sessions with an exclusive mode for Lua drawings,
a default screen). None of that exists here, and the face is the only client
of the panel, so the port is:

- **`display_hw.c/.h`** (new, ~250 lines): the whole "service". ST7789 on
  SPI3 (MOSI 40, SCK 41, CS 9, DC 39, 3-wire, mode 2, 80 MHz, software reset
  because RST is on the board RESET net), `swap_xy`/`mirror_x`/`invert_color`
  as the board YAML, LEDC backlight on GPIO 42 with `output_invert` for the
  P-FET, starting dark. LVGL through **`espressif/esp_lvgl_port`** (2.9.0
  resolved) with two 8-line draw buffers in internal DMA RAM
  (`CONFIG_APP_DISPLAY_BUFFER_LINES`), `swap_bytes` for the panel's
  big-endian RGB565, LVGL task stack 10 KB *internal* (it reads GIFs off
  LittleFS; a flash read from a PSRAM stack asserts). API: init, available,
  lock/unlock, panel handle, base mirror, backlight.
- **`cap_display.c`, `cap_display_face.c`, `cap_display_orient.c`,
  `cap_display.h`**: copied; `display_service_lock/unlock` ->
  `display_hw_lock/unlock`, the session / exclusive-owner / default-screen
  / "put the face back when system_ui loads its launcher" machinery removed
  (nobody else draws), `use_internal_draw_buffers()` removed (esp_lvgl_port
  already allocates them there; `cap_display_draw_bufs_internal()` returns
  true so the face never holds still for RAM). The claw_cap tool descriptors
  are gone; their bodies are plain functions: `cap_display_show_text`,
  `show_emotion`, `clear`, `set_brightness`, `brightness`, `set_colors`,
  `parse_color`. The `display` console command is gone (no console here);
  its knobs became `cap_display_set_shift/get_shift` plus the orientation
  getters, driven from HTTP. **Function and file names keep the
  `cap_display_` prefix** so these files diff against mpv2-claw's.
  `cap_display_face.c` (the eyes, moods, move faces, the GIF player on
  LVGL's AnimatedGIF) is verbatim apart from the include and `/fs/display`.
- **`display_glue.cc/.h`** (new): what mpv2-claw's `main.c` did -- the IMU
  motion source for the alive eyes (`mpx_robot_imu_read`, busy = continuous
  gait or Pi in control, `mpx_robot_playing_move` for the move faces), the
  network line under the eyes, the Pi's address, the ROS badge via
  `on_master_changed`, and `pi_link::set_video_sink(cap_display_show_jpeg)`.
  The network line is **polled once a second** from the display event task
  instead of hooking the Wi-Fi event handlers (mpv2-claw's own note: that
  handler must not wait on the LVGL lock). Boot order in `main.cc`:
  `start_face()` right after `robot::init()` (eyes first, backlight on once
  drawn), `start_animations()` after LittleFS, `start_events()` after Wi-Fi.
- **`display_assets.cc`**: blink/smile/sleepy GIFs (310 KB) are embedded in
  the app and written to `/fs/display/` at boot **only when missing or of a
  different size** (mpv2-claw bakes them into its storage image; mpx-dog has
  no build-time LittleFS image). Flash: 3.2 MB image, 48 % of the 6 MB slot
  free.
- **`network/http_display.cc`**: `/v1/display` GET/POST, `/v1/display/gif`
  GET/POST/DELETE, same JSON as mpv2-claw (cJSON; storage from
  `fs::stats()` instead of `esp_vfs_fat_info`). Additions on this firmware:
  GET also reports `flip180 mirror shift_x shift_y brightness eyes_alive`,
  and POST accepts those plus `emotion`, `text` (`""` clears), `background`
  / `eyes` colours -- a settings-only POST leaves the animation alone.
  Routes are registered through `http_server.cc`'s `register_uri` so the
  handler count stays in one place (`max_uri_handlers` 80 -> 88, 74 used).
  Nothing is registered with `APP_DISPLAY_ENABLE=n`, so the app hides the
  page as on a board without a screen.
- Kconfig `APP_DISPLAY_ENABLE` (y), `APP_DISPLAY_FLIP180_DEFAULT` (y),
  `APP_DISPLAY_BUFFER_LINES` (8). LVGL options in `sdkconfig.defaults`
  (RGB565, clib malloc so the 150 KB GIF frame lands in PSRAM, stdio FS on
  drive `S`, `LV_USE_GIF`, examples/demos off). Managed components added:
  `lvgl/lvgl >=9.5.0,<9.6.0` (the face uses LVGL's bundled AnimatedGIF
  directly, whose API moved between minors), `espressif/esp_lvgl_port ^2.6`,
  `espressif/esp_new_jpeg ^1.0.2` (Pi camera frames).
- **PWA**: `DisplayPage.svelte` gains a "Screen" section (brightness
  slider, turned 180°, mirrored, alive eyes) shown only when GET
  `/v1/display` carries `flip180` -- so the same app stays correct against
  mpv2-claw, which does not. Service-worker cache `v10-claw`.

Bench checks owed for phase 3:

- First light: eyes the right way up with the default flip, no white flash,
  colours right (yellow eyes, not blue -- if blue, `swap_bytes` is wrong);
  a black strip at one edge means `shift_x/shift_y` is needed.
- 80 MHz pclk through the GPIO matrix: tearing -> drop `LCD_PCLK_HZ` to 40.
- GIF upload from the app, loop survives a reboot, play-once returns to the
  eyes; Pi camera picture when a Pi streams VIDEO_DATA.
- Internal RAM headroom with WAMR + Lua + httpd + LVGL running (the eyes
  task and the event task are new consumers).

## Phase 4b changes (voice: wake word, MangDang gateway, music, voice tools)

Everything lives in `main/voice/` plus two HTTP files. mpv2-claw's four
components collapse into one directory because mpx-dog is a single
component; the file names keep mpv2's so a diff against the originals is a
one-liner (`diff mpv2-claw/components/mpx_voice_link/src/X main/voice/X`).

### Copied verbatim or nearly (mpv2-claw `components/mpx_voice_link`, `mpx_voice_wake`, `cap_music`)

| mpx-dog file | from | what changed |
|---|---|---|
| `voice/mpx_voice_link.h` | `mpx_voice_link/include/mpx_voice_link.h` | OTA URL default now `CONFIG_APP_VOICE_OTA_URL`; `register_voice_command()` dropped |
| `voice/mpx_voice_ws.c`, `mpx_voice_stream.c` | `mpx_voice_link/src/` | none |
| `voice/mpx_voice_codec.cc` | `mpx_voice_link/src/` | comment only (see PSRAM-stack rule below) |
| `voice/mpx_voice_ota.c` | `mpx_voice_link/src/` | identifies as `mpx-dog` / `mp4_esp32_core` / `mpx-dog/0.1.0` instead of `mp4-claw` + `CONFIG_ESP_BOARD_NAME` |
| `voice/mpx_voice_wake.{h,c}` | `mpx_voice_wake/` | console `wake` command removed; two accessors added (`mpx_voice_wake_last_seen`, `mpx_voice_wake_state_name`) so HTTP can report what the command printed; `CONFIG_MP4_VOICE_AUTO_MAX_SECONDS` -> `CONFIG_APP_VOICE_AUTO_MAX_SECONDS` |
| `voice/cap_music.{h,c}` | `cap_music/` | claw_cap tool descriptors, `cap_music_register_group()` and the `music` console command removed; `cap_music_init()` now does the wiring itself (level from NVS, follow-up gate, quiet hook); station accessors added for the tool bridge and the app |

The xiaozhi wire protocol, Opus framing (60 ms, 16 kHz up, 24 kHz
negotiated down, played at 16 kHz because mic and speaker share one I2S),
first-wake-word provisioning against the gateway (`mpx_voice` NVS namespace,
keys `ws_url` / `ws_token`), hands-free state machine (LISTEN / TALK / REPLY,
barge-in, follow-up window, WAKE button on GPIO 21), AFE setup
(`AFE_MODE_LOW_COST`, `AFE_MEMORY_ALLOC_MORE_PSRAM`, core 1, 4-channel
"RMNN" with slot 0 the echo reference, "MN" fallback) are all mpv2-claw's
and are documented in `mpv2-claw/.cursor/NOTES.md` and
`docs/ESP_CLAW_PLAN.md`; nothing was redesigned.

### Console commands -> HTTP (`main/network/http_voice.{h,cc}`)

mpx-dog has no esp_console, so mpv2's `voice`, `wake`, `music` commands
become routes the app can also use:

- `GET /v1/voice` -> `voice_glue::status_json()`: `available, mode, mic,
  speaker, volume, provisioned, gateway, link, hands_free
  (off|listening|talking|replying), listening, talking, playing_reply,
  pi_in_control, afe{ready, level_dbfs, speech, ringbuf_free},
  music{active, title, level, state}`.
- `POST /v1/voice` with any of `{"volume":0..100}` (persists via
  `mpx_audio_set_volume`), `{"music_level":5..100}`, `{"music":"jazz" |
  "https://..." | "stop"}`, `{"provision":true}` (re-provision now),
  `{"hands_free":bool}`. Answers the status, or 400 with a reason.
- `GET /v1/voice/tools` lists the tool table; `POST /v1/voice/tools
  {"name","arguments"}` runs one -- a bench substitute for the gateway, so
  every tool can be exercised from a phone without speaking.
- `GET/POST /v1/ai`: mpv2-claw's shape (`mode`, `running_mode`) plus
  `"modes":[...]` and `"available"`. Only `mangdang` and `off` exist here;
  `own` -> 400 with a message saying it is not in this firmware yet. A
  change applies live (hands-free starts or stops, link disconnects), so the
  POST answers `"restarting":false` and the app does not wait for a reboot.
  Mode is in NVS namespace `voice`, key `ai_mode`.

Routes are registered from `http_server.cc` after the display ones
(`max_uri_handlers` raised to 96; 80 in use).

### Voice tools: `main/voice/voice_tools.{h,cc}` replaces `mpx_mcp_ws`

mpv2-claw lets the gateway's LLM drive the robot over MCP: the websocket
carries `{"type":"mcp","payload":<jsonrpc>}` and `mpx_mcp_ws` answers from
the ESP-Claw capability registry (`claw_cap`) via mcp-c-sdk. mpx-dog has
neither, and pulling them in just for this would be most of Phase 5. So the
bridge is a static table of tools with a hand-written JSON-RPC handler:

- Same wire format (envelope with `session_id`, `tools/list` paged at 4000
  bytes with `nextCursor`, `tools/call` -> `{"content":[{"type":"text",...}],
  "isError"}`, `initialize` echoes the client's protocol version,
  `ping` -> `{}`, notifications get no reply, a `self.` prefix on names is
  tolerated). The gateway cannot tell the difference.
- Same tool names and JSON schemas as mpv2-claw's descriptors (so the
  gateway-side prompts keep working): `robot_move`, `robot_pose`,
  `robot_get_state`, `robot_set_config`, `display_show_text`,
  `display_show_emotion`, `display_clear`, `display_set_colors`,
  `display_set_brightness`, `mpx_list_skills`, `mpx_run_skill`,
  `mpx_stop_skill`, `music_play`, `music_stop`. Missing on purpose:
  `ask_robot_agent` (that *is* the on-device agent -- Phase 5).
- Tool calls run on their own task (`voice_tools`, 8 KB **internal** stack,
  queue depth 8) because `robot_set_config` writes NVS -- see the PSRAM
  rule. The websocket task only enqueues.
- Skills go through the same one-at-a-time path the app uses: `http_server`
  now exports `network::skill_start()`, `skill_start_for_movement()`,
  `skill_running()`, `skill_running_name()`, `skill_stop()` (declared in
  `http_server.h`) so a voice-started skill and an app-started skill cannot
  overlap, and `robot_move` with an unknown movement falls back to a skill
  that `provides_gait` for it, exactly like `/v1/robot/gait`.

### Glue: `main/voice/voice_glue.{h,cc}` (mpv2-claw's `main.c` voice section)

`voice_glue::start()` is the last call in `app_main` (after Lua): face hooks
(`mpx_voice_ui_hooks_t` -> `cap_display_voice_*`, under
`CONFIG_APP_DISPLAY_ENABLE`), `cap_music_init()`, `voice_tools::start()`,
then -- unless the stored mode is `off` -- `mpx_voice_wake_init()` and,
with `CONFIG_APP_VOICE_AUTO_LISTEN`, hands-free listening. A heap line is
logged after AFE init so the internal-RAM cost is visible on the console.

Pi/ROS handoff: `robot::master` callbacks already existed from Phase 2;
`voice_glue::on_master_changed(bool)` is now called too. Pi takes over ->
music stops, hands-free stops, a resume flag is set; Pi releases ->
hands-free restarts if it was on. The work runs on a small `voice_ev` task,
not in the Pi-link callback (voice teardown takes the audio lock).
`hands_free_start()` refuses with `ESP_ERR_INVALID_STATE` while the Pi is
in control; `/v1/voice` turns that into a 409.

Without `CONFIG_APP_VOICE_ENABLE` the glue compiles to stubs
(`available:false`, mode `off`), `/v1/ai` offers only `off`, and
`/v1/voice*` is not registered.

### Build plumbing

- `main/idf_component.yml`: `espressif/esp-sr ^2.5.2`,
  `78/esp-opus-encoder ~2.4.0` (brings `78/esp-opus`),
  `espressif/esp_websocket_client ^1.4.0`,
  `espressif/esp_audio_simple_player ^0.9.6` (brings gmf). esp-sr is pulled
  **unconditionally** rather than behind a `rules:` clause: mpv2-claw
  learned that a conditional dependency needs two configure passes before
  the Kconfig symbol exists. Only `main/CMakeLists.txt`'s source list is
  gated on `CONFIG_APP_VOICE_ENABLE`.
- `main/Kconfig.projbuild`: `APP_VOICE_ENABLE` (default y, depends on
  `APP_AUDIO_ENABLE`), `APP_VOICE_OTA_URL`, `APP_VOICE_AUTO_LISTEN`,
  `APP_VOICE_AUTO_MAX_SECONDS` -- mpv2's `MP4_VOICE_*` names with the
  `APP_` prefix the rest of mpx-dog uses.
- `sdkconfig.defaults`: `CONFIG_MODEL_IN_FLASH`, `AFE_INTERFACE_V1`,
  WakeNet9 "Hi ESP", VADNet1 medium, WebRTC NS, no MultiNet; audio simple
  player with HTTP + file readers, resample to 16 kHz mono 16-bit (what
  `mpx_audio_music_write` takes). The `model` partition (1 MB at 0x600000)
  was already in `partitions.csv` from Phase 1; `srmodels.bin` is 579 KB.
- Top-level `CMakeLists.txt`: mpv2-claw's configure-time patch of esp-sr's
  `model/movemodel.py` (`sys.stdout.reconfigure(errors='replace')`) so the
  Windows cp1252 console does not kill the build at "Move and Pack models".
  Re-applied on every configure; no-op on POSIX.

### The PSRAM-stack rule (superseded in Phase 5)

Through Phase 4 mpx-dog ran without `CONFIG_SPIRAM_XIP_FROM_PSRAM`, so the
rule here was: **a task whose stack is in PSRAM must never touch flash**
(NVS, LittleFS, OTA) -- a flash read disables the cache, and with the cache
off a PSRAM stack is unreachable (IDF asserts in
`spi_flash_disable_interrupts_caches_and_other_cpu`). The voice code
respects it (`wake_auto_task` internal because provisioning writes NVS;
talk, playback, feed, fetch and music tasks flash-free; `voice_tools`
internal because of `robot_set_config`; `voice_ev` internal and tiny).

Phase 5 turns XIP on (ESP-Claw puts every agent task stack in PSRAM and
those tasks read the filesystem constantly; there is no Kconfig to stop it),
so the rule is no longer load-bearing. The existing placements were left as
they are: they cost nothing and keep the voice stack identical to mpv2-claw's.
The flip side, as in mpv2-claw: **do not switch XIP off** to save PSRAM --
the firmware then dies in the first `stat()` from an agent task, which reads
like a filesystem bug.

### Result

Firmware builds with zero warnings; app image 4.81 MB, 23 % of the 6 MB slot
free (was 3.2 MB after Phase 3 -- esp-sr, Opus, gmf and the player are the
difference). `check_comments.py` clean.

Phone app: `AISettings.svelte` filters the mode cards by the GET `modes`
list (so `own` is hidden on this firmware and still shown on mpv2-claw),
treats `restarting:false` as "reload now", and gains a Voice card (link /
gateway / mic / speaker state, volume slider, start/stop listening, pair
now, music genre or stream URL + stop) that is only rendered when
`/v1/voice` answers `available:true`. Service-worker cache `v11-claw`.

Bench checks owed for phase 4b:

- Boot heap after `mpx_voice_wake_init` (console line `voice: heap after
  AFE`): internal free must stay well above what httpd + WAMR need; if it
  does not, drop the AFE to "MN" (2-channel) or disable VADNet.
- "Hi ESP" wakes the robot; first wake provisions against the gateway
  (`/v1/voice` -> `provisioned:true`), the eyes switch to the listening
  face, the reply plays; barge-in while it talks.
- Echo reference slot: if replies trigger the wake word, "RMNN" has the
  reference in the wrong slot for this I2S wiring -- swap to "MRNN"
  (mpv2's measured order was 0 = reference on the same board).
- Gateway tool calls: ask it to sit / stand / show an emotion / play jazz;
  `POST /v1/voice/tools` first to separate tool bugs from gateway bugs.
- Pi master on -> music and listening stop; off -> listening resumes.
- `/v1/ai` off -> link disconnects, mic stays closed (no AFE feed task in
  `ps`-equivalent heap logs).

## Phase 5 changes (the on-device agent)

The owner's brief: the local agent working inside mpx-dog, with a toggle in
the chat heading choosing which brain a *new* conversation uses -- the
robot's own agent or the cloud OpenClaw -- and no switching mid-conversation.
A real handoff between the two brains is the eventual goal and is not in
this phase.

### Shape of the integration

mpv2-claw *is* an ESP-Claw application: ESP-Claw's `app_claw` owns boot,
settings, Wi-Fi, the HTTP server and the console, and mpv2's robot hangs off
its extension points. mpx-dog has its own shell for all of that, so the
integration is inverted: **ESP-Claw is a library inside mpx-dog's app.**
Only the agent itself is taken -- `app_claw` (agent start-up, capability
registration, LLM config), `claw_core`, `claw_cap`, `cap_im_local` (the
"web" chat channel), `claw_event_router`, and whatever those pull in
(`claw_manager`, `claw_memory`, `claw_skill`, `cap_boards`, `cap_files`,
`cap_scheduler`, `cap_session_mgr`, `cap_skill_mgr`, `cap_system`,
`cap_http_request`, `cap_agent_mgr`, `cap_llm_config`, `skill_builder`,
`lua_module_builder`). Not taken: ESP-Claw's settings/Wi-Fi/HTTP/captive
portal components, its console, `system_ui`, `cap_lua` and the Lua modules
(mpx-dog has its own Lua VM and WASM skills; the agent's Lua is a different
runtime, and carrying both was judged not worth 1 MB of flash and the RAM).

**Vendored, not a submodule.** `third-party/esp-claw/` is a copy of the
`components/` tree of mpv2-claw's pinned ESP-Claw (plus `.agents/`,
`AGENTS.md`, `CLAUDE.md`, `LICENSE`, `README.md`, `CHANGELOG.md`; 357
files, 3.1 MB) with `docs/`, `tools/`, `application/`, `pages/`,
`components/lua_modules/` and the CI files dropped. mpv2-claw tracks ESP-Claw
directly too (not as a submodule), so there was no pin to point a submodule
at; the copy keeps the two trees byte-identical where they overlap, and
**the never-edit rule carries over**: nothing under `third-party/esp-claw/`
is modified. Where ESP-Claw needed bending, it was done from outside:

- `main/agent/claw_overrides.c`: the event router task's 8 KB stack (no
  Kconfig for it, overflowed on mpv2-claw) is raised to
  `CONFIG_APP_AGENT_EVENT_ROUTER_STACK` by `-Wl,--wrap=claw_event_router_init`.
- `main/Kconfig.projbuild` defines five `ESP_BOARD_DEV_*_SUPPORT` symbols
  (false, with prompts) that ESP-Claw's manifests reference and
  `esp_board_manager` would normally generate. Without them the component
  manager stops with "Missing required kconfig option after retry" --
  promptless symbols do not work because kconfgen leaves them out of
  `sdkconfig.json`, the only file the component manager reads.
- `components/gen_bmgr_codes/gen_board_metadata.yaml`: a hand-written board
  inventory (pins from `board_config.h`) because `cap_boards` is always
  linked and runs `generate_board_skill.py` on that path at configure time.
  No `CMakeLists.txt` beside it on purpose -- it must not become a component.
- All of ESP-Claw's Lua driver/module symbols are set `=n` in
  `sdkconfig.defaults`: they do not depend on `APP_CLAW_CAP_LUA`, and
  `app_lua_modules.c` includes each enabled one's header while the components
  are not in the tree.

**Dependencies are unconditional `path:` entries** in `main/idf_component.yml`
(`app_claw`, `http_reuse`, `claw_core`, `claw_cap`, `cap_im_local`,
`claw_event_router`). `CONFIG_APP_AGENT_ENABLE=n` still compiles them (dead
code the linker drops) but does not start the agent -- gating the manifest
on a Kconfig symbol would re-create the two-pass configure problem mpv2-claw
documents for esp-sr.

### sdkconfig and partitions

- `CONFIG_SPIRAM_XIP_FROM_PSRAM=y` (see the PSRAM-stack rule above),
  `ALLOW_BSS/NOINIT_SEG_EXTERNAL_MEMORY`, main task stack 6 KB,
  `VFS_MAX_COUNT=12`, `LWIP_MAX_SOCKETS=20`, `HTTP_REUSE_ENABLE=n`
  (esp-claw#101), memory mode FULL, stage verbosity VERBOSE (the "Round N"
  notes are the PWA's activity lines), subagents capped at 2, CLI and the
  unused capability groups off. Each has a comment in `sdkconfig.defaults`.
- `sdkconfig` had to be regenerated (ESP-Claw's Kconfig, the new partition
  table, XIP). It turned out to hold settings never recorded in
  `sdkconfig.defaults` -- AP SSID, cloud ingress IP, `HTTPD_WS_SUPPORT`,
  240 MHz, the Phase-4 Wi-Fi buffer trims -- which would have silently
  reverted. They are now in `sdkconfig.defaults` with a note; the diff of the
  regenerated file against the old one is exactly the intended changes.
- `partitions.csv`: `factory` 7 MB (app is 5.05 MB, 31 % free), `model`
  1 MB at 0x710000, new `system` LittleFS 1 MB at 0x810000 (read-only agent
  image: skills + `.recovery` seeds), `storage` LittleFS 6.9 MB at 0x910000.
  **storage moved again**, so flashing this build wipes installed skills,
  uploaded GIFs and the deployed PWA copy (re-seeded from the embedded
  assets at boot). `idf.py flash` writes all five images; `app-flash` alone
  leaves the agent without its skills.

### Agent filesystem: `main/agent/agent_fs.{h,cc}` + `agent_fs/system/`

ESP-Claw wants two roots: SYSTEM (read-only: skills, recovery seeds) and
DATA (sessions, memory, user skills, router rules, scheduler). Here SYSTEM is
the `system` partition mounted at `/system`, built by CMake from
`agent_fs/system/` (`.recovery/` seeds: `identity.md`/`soul.md` rewritten for
Pupper the robot dog, `user.md` and `schedules.json` verbatim,
`router_rules.json` without mpv2's light-switch rule) plus every build
component's `skills/<id>/SKILL.md` collected by `skill_builder` -- the four
robot skills live in `main/skills/` (`mpx_robot`, `mpx_skills`,
`mpx_display`, `mpx_music`, frontmatter `cap_groups` naming the capability
groups). DATA is `CONFIG_APP_AGENT_DATA_DIR` (`/fs/agent`) on the storage
LittleFS -- a subdirectory rather than `/fs` itself so the agent's `skills/`
never collides with the WASM skills in `/fs/skills`. `agent_fs::init()`
mounts, makes the DATA tree, copies missing `.recovery` files in
(mpv2-claw's seeding behaviour), and calls `claw_paths_set()`.

### Who the brain talks to: `agent_config`, `voice_glue`, `/v1/ai`

`main/agent/agent_config.{h,cc}` now owns the AI mode (`mangdang` | `own` |
`off`, NVS `voice/ai_mode` -- the key voice_glue used, so a robot set to
"off" stays off) and the owner's LLM account for `own` (NVS namespace
`agent_llm`: backend, URL, model, auth type, max-tokens field, key; the key
is never echoed, only `key_set`/`key_hint`). `voice_glue` lost its own
`Mode`/NVS code and reads `agent_config::mode()` through `voice_wanted()`;
`set_mode()` became `apply_mode()`. The model per mode (`agent_runtime.cc`
`fill_config`), as mpv2-claw: `mangdang` -> the gateway's OpenAI-compatible
`/v1/llm` (`CONFIG_APP_AGENT_GATEWAY_LLM_URL`) with the robot token
`"<uuid>.<HMAC-SHA256(aes_key, "mpx-ai-v1:"+uuid)[:32]>"`, never persisted;
`own` -> the stored account; `off` -> no model, `submit()` refuses. A change
through `POST /v1/ai` is saved, then applied live: `voice_glue::apply_mode()`
first (it may close the mic), then `agent::apply_config()` ->
`app_claw_update_config()`. No restart, unlike mpv2-claw. ESP-Claw's own
`/llm` chat command edits are persisted back as `own` by the save callback,
unless they point at the gateway URL.

**"own" is narrower than mpv2-claw's for now:** the key serves the chat
page's Pupper brain; a voice turn is still answered by the MangDang voice
gateway (its server runs the model). The voice -> local-agent handoff is the
"final solution" the owner described and is left for the next step; the
AISettings card says so when the robot reports an `agent` object.

### Tools: `robot_tools` shared by voice MCP and the agent

The tool bodies that were in `voice_tools.cc` (robot_move/pose/get_state/
set_config, display_*, mpx_list/run/stop_skill, music_play/stop) moved to
`main/agent/robot_tools.{h,cc}` as a plain table (`name, group, description,
brief, schema, run`). `voice_tools.cc` keeps only the JSON-RPC/MCP transport
and paging and calls into the table; `main/agent/agent_caps.cc` turns the
same table into `claw_cap_descriptor_t` arrays per group and registers four
external groups with the ids mpv2-claw uses (`cap_robot`, `cap_display`,
`cap_mpx_skill`, `cap_music`) so the SKILL.md `cap_groups` match. One tool
definition, two front ends. Descriptions longer than ESP-Claw's 256-byte
limit have a `brief` for the agent side (the full text stays in the skill
docs). `mpx_display_draw` (free-form Lua drawing on the face) was not ported:
it needs `cap_lua`.

### The chat bridge and the brain toggle

Firmware (`chat_ws.cc`): a frame carrying `"brain":"local"` is for the
on-device agent; anything else (including an older PWA that sends no brain)
is the cloud path, unchanged. Local `user_chat_input` -> `agent::submit(
session_id, text)` -> `cap_im_local_emit_user_message("web", ...)`, answered
with `ack{sent}` or an `error` frame saying why not (no agent in the build,
agent not running, AI off). The agent's outbound (bound in
`init_chat_pipeline`, before `agent::start()`) produces the PWA's existing
frames: progress notes (ESP-Claw's lobster-prefixed working reply, stripped)
become `step{seq,total}` with the running count as both -- the agent decides
how many rounds as it goes -- and the answer a `chat_reply`, after which the
per-client step counter resets. The session id is the PWA's conversation id,
so ESP-Claw's session history is per conversation and the robot keeps no
brain state at all. `session_reset` for a local conversation is acked
without the cloud round-trip. `POST /v1/chat/send` (the pre-PWA REST path)
stays cloud-only.

While there, the reply sender was unified: `send_reply_to_pwa()` splits long
answers on UTF-8 boundaries *before* JSON-escaping (two passes, so `total` is
right on the first chunk), and the cloud path uses it too -- the old
splitter cut the escaped string at fixed byte offsets and could end a chunk
inside `\uXXXX` or a multi-byte character.

PWA: conversations carry `brain` (`conversationStore.js`; records from
before default to `cloud`), and the last choice is remembered in
`mpx_chat_brain` for the next new conversation. `ChatView.svelte` puts a
segmented **Pupper / Cloud** control at the right end of the chat action bar;
it is live only while the conversation is empty and locks (with a padlock)
once a message exists -- the robot keeps the two histories apart by
conversation id, so a conversation changing sides would lose its own past.
Changing the toggle on an empty conversation re-sends `session_reset` with
the brain, so the first message lands on the chosen side. `/v1/ai` is probed
on connect: `agent.available:false`, `agent.running:false` or mode `off`
grey out Pupper with the reason as the tooltip (and fall back to Cloud for a
new conversation); a robot without `/v1/ai` leaves both offered and the
robot answers with an error frame if it cannot take it. The sidebar shows a
Pupper/Cloud badge per conversation. Service-worker cache `v12-claw`,
`pwa_build: brains-v15`.

### Deferred

- Voice -> local agent handoff (and the broader cloud <-> local handoff).
- ~~`mpx_display_draw` (needs `cap_lua`)~~ -- done natively in Phase 6 (`display_draw`).
- The ESP-Claw skills that mention Lua scripts (`skills_lab_downloader`,
  `skill_creator`) are in the system image because `cap_skill_mgr` ships
  them; without `cap_lua` their scripts cannot run. Harmless, but the agent
  may offer them. Filtering them out of the image is a small CMake step.
- Moving the PWA's `.gz` assets from the app image into the `system`
  partition would give the app slot ~0.7 MB back.

### Result

Firmware builds with zero warnings; app image 5.05 MB, 31 % of the 7 MB slot
free; `check_comments.py` clean. System image builds with the four robot
skills, ESP-Claw's built-in ones and the recovery seeds. Bench checks owed:

- Boot: `agent` heap line after `agent::start()`; the agent's first
  `/v1/ai` GET answers `agent.running:true`.
- New conversation on Pupper: "sit down" -> a step line, the robot sits, a
  reply; the sidebar shows the Pupper badge; reopening the conversation
  shows the toggle locked.
- Same question on Cloud still works; an old-PWA phone (no brain field)
  still reaches the cloud.
- `POST /v1/ai {"mode":"own", ...}` with a real key: a Pupper conversation
  answers from that model; voice still answers via MangDang.
- `POST /v1/ai {"mode":"off"}`: Pupper greys out in the chat, a queued local
  message gets the "AI is off" error frame.
- Internal RAM after the agent + AFE both up; if Wi-Fi starves again, the
  agent's `claw_core` task stacks are the first candidates to trim.

## Phase 6 changes (catch-up with mpv2-claw `ede0804`)

Phases 1-5 were ported from mpv2-claw `cf0378c` ("Pi link Uart"). mpv2-claw
then gained `ede0804` ("new pi link and RL", 2026-10-10). This phase brings
that commit over. Nothing in it touches the agent, voice or display.

### Robot core (`main/robot/`)

`robot.cc`, `robot.h`, `driver_board.c`, `driver_board.h` were still
byte-identical to mpv2-claw `cf0378c`, so they are now mpv2-claw `ede0804`'s
files verbatim:

- `driver_board_sync_raw()` / `driver_board_raw_feedback()` /
  `driver_board_raw_invalidate()`: the raw AT32 path (deci-degrees, no
  direction flip, no offset) an RL policy drives, with `DB_MODE_HOLD`.
  `DB_LIVE_SUPPLY_CURRENT_MA` (live id 13, AT32 fw >= 0.0.9).
- `robot::host::begin/end/active/store_offset`: the raw host is a master
  like the ROS bridge (`master_active()` is true, every local motion entry
  point is fenced, the master observer fires: voice off, ROS on the screen),
  but it drives the boards itself, so the gait parks. Servo gains are saved
  on begin and written back on end; end glides into the stand.
  `store_offset` writes this firmware's offset and the `parameter`
  namespace mp4-link / minipupperesp read.

`imu.cc` stays this firmware's (shared I2C), but now matches mpv2-claw's
`mpx_imu.cc` numbers: **200 Hz ODR, 5 ms poll, +-16 g** (was 100 Hz, 10 ms,
+-2 g). The RL STATE carries a fresh sample every 5 ms tick, and the
reflexes in `alive.c` were tuned on mpv2-claw at these settings.

### Pi link (`main/pi/`)

- `rl_link.{h,cc}` and `RL_LINK.md`: mpv2-claw's verbatim, Kconfig prefix
  `MP4_RL_*` -> `APP_RL_*`, include paths `pi/...`. mp4-link's frames byte
  for byte on the J-PI UART (`/dev/ttyAMA0` on the Pi, 921600), so
  `python -m mp3link` works unchanged. Sessions, watchdog, guard, HOLD
  before first ARM: see `RL_LINK.md`.
- `pi_link.cc` (3-way merge of mpv2-claw's diff onto this file's renamed
  version, no conflicts beyond names):
  - the link task waits for one byte and then takes what is buffered,
    instead of waiting for a full buffer (a 36-byte SET_ALL was held for
    the 10 ms timeout);
  - every received chunk also goes to `rl_link::feed()`; audio/video
    frames call `rl_link::note_bulk()`;
  - a running RL session keeps the ROS bridge in monitor (whoever came
    first keeps the robot), and no ROS telemetry is sent while RL drives;
  - the Pi's IP shows for a fresh HOST_INFO in **any** link state (was:
    not in standalone), so `tools/pi/pi_announce.py` can put it on the
    screen from boot without heartbeating;
  - `Status` / `GET /v1/pi/status` gain the `rl` object
    (`session, robot, armed, flags, fault_servo, frames, crc_err,
    sessions, ticks`). The handler's buffer is 1 KB now.
- Kconfig: `APP_RL_LINK_ENABLE` (y) and `APP_RL_TICK_HZ`,
  `APP_RL_WATCHDOG_MS`, `APP_RL_RELEASE_MS`, `APP_RL_GUARD_CUR_MA/_MS`,
  `APP_RL_GUARD_TEMP_DC/_MS`, inside `APP_PI_LINK_ENABLE`. `sdkconfig`
  picks the defaults up on the next configure.
- Not ported: mpv2-claw's `pi` console command's RL line (no console here;
  the same numbers are in `/v1/pi/status`).

### Tools (`tools/`)

- `tools/pi/`: `pi_announce.py` + `pi-announce.service` (the Pi sends only
  HOST_INFO every 2 s) and the Pi-side README (wiring, bridge parameters,
  checks), adapted to this firmware's names. mpv2-claw's `mp4_link.py` (the
  old text-line test link) is not carried: no firmware speaks it.
- `tools/check_comments.py`, which these notes used to run from
  `..\mpv2-claw\tools`.

### Phone app (`pwa-claw/`)

`ServoStudio.svelte`, `Controller.svelte`, `HomeScreen.svelte` and
`Calibration.svelte` were identical to mpv2-claw `cf0378c`, so they are
`ede0804`'s verbatim: Servo Studio rework (unsaved-change count, save all
four boards), right stick Turn / Look toggle (turn uses the existing
`/v1/robot/joy` `t`), the landscape pad inline on a PC-sized window,
calibration range +-90 deg. `app.svelte` and `ChatView.svelte` got
mpv2-claw's socket fixes by 3-way merge (no conflicts with the brain
toggle): at most one new WebSocket per 3 s, `untrack` around the
lend/take-back handlers (the Servo Studio "Robot offline" reconnect storm).
Service-worker cache `v13-claw`. `npm run build` is clean.

### Not ported, on purpose

- mpv2-claw's Wi-Fi changes (`mpx_wifi_tune` config lock, scan that
  interrupts a join attempt): they fix ESP-Claw's `wifi_manager`, which
  retries forever. This firmware's `network/wifi_sta.cc` stops at
  `NO_AP_FOUND`, already switches AP -> AP+STA to scan, and has no tuner.
- `fatfs_create_rawflash_image(... PRESERVE_TIME)`: mpv2-claw serves the
  PWA from a FAT image; here it is embedded in the app.
- `pi_ros_src.tgz` / `pi_ros_hashes.txt` (a 119 MB Pi-side source dump).

### Internal RAM (first boot: reboot at the wake word)

First bench boot rebooted in `i2s_tx_channel_start`: the wake word opened
the microphone, the I2S driver could not get its DMA buffers, and
esp_codec_dev enabled the half-configured channel anyway. Internal RAM after
the agent was 35 KB (14 KB DMA-capable); after the AFE, 0.4 KB.

- `sdkconfig(.defaults)`: mpv2-claw's internal-RAM budget, which never came
  over: Wi-Fi static RX/dynamic RX/static TX 3/6/8 (was 10/32/16), BA
  window 3, Wi-Fi IRAM opts off, `-Os`, FreeRTOS and ringbuf code in flash,
  system event task 4 KB.
- Task stacks moved to PSRAM (safe with XIP, see the PSRAM-stack rule):
  LVGL (10 KB, `task_stack_caps`), `chat_lua` and `chat_upstream` (8 KB
  each; their early exits now `vTaskSuspend`, a WithCaps task must not
  `vTaskDelete` itself), `voice_tools` (8 KB), `disp_events`, `voice_ev`
  (4 KB each), `sock_reaper` (3 KB). The HTTP server and the Wi-Fi/robot/Pi
  tasks stay internal.
- `mpx_audio_capture_start()` refuses with `ESP_ERR_NO_MEM` and logs the
  numbers when internal DMA RAM is under 20 KB, instead of rebooting.

### Drawing on the screen (`display_draw`)

mpv2-claw's agent draws anything ("draw a crab") by writing Lua for
`lua_module_lvgl` (skill `mpx_display_draw`), which needs ESP-Claw's
`cap_lua`, its Lua and `display_service` -- none of which this firmware has
(Phase 5 left them out; its own Lua VM would clash with ESP-Claw's). The same
job is now a native tool:

- `cap_display_draw()` (`display/cap_display.c`): a 320x240 RGB565 canvas
  (150 KB PSRAM, allocated on first use) painted with LVGL's draw API (rect,
  rounded rect, circle/oval, line, triangle, arc, label), shown instead of
  the face for `seconds`, then the face comes back (as `display_show_text`).
- `display_draw` in `agent/robot_tools.cc` (so the agent and the voice
  gateway both get it): `{background, seconds, shapes:[{type, color, ...}]}`;
  polylines become lines, polygons are fanned into triangles.
- `main/skills/mpx_display_draw/SKILL.md` (same skill name as mpv2-claw,
  group `cap_display`) with the coordinate system and a crab example;
  `mpx_display/SKILL.md` points at it.
- Fonts Montserrat 28 and 48 enabled for `medium` / `large` text.

What it cannot do that Lua could: animate (one call is one still picture).

### Cleanup

- **One phone app.** `pwa/` (original, dark) and `pwa-redesign/` are gone,
  with the `PWA_DESIGN` Kconfig choice and every `CONFIG_PWA_DESIGN_*`
  branch in `main/CMakeLists.txt`, `network/http_server.cc` and
  `network/www_assets.cc`. `pwa-claw/` is the app; its big files are
  embedded as `.gz` only, as before. The original app's Lua editor, WASM
  list and servo ping/sweep page went with it (mpv2-claw has none of them
  either; the HTTP routes they used are still there).
  `.devcontainer/post-create.sh` installs `pwa-claw/`'s packages.
- `components/dong-12345__ftservo` removed: nothing used it (the servos are
  the AT32 boards on SPI), but ESP-IDF compiled it on every build.
- `main/test_skill.wasm` removed (not referenced by the build).
- Windows checkouts: `git config core.autocrlf true` in the clone, so the
  CRLF working tree no longer shows every file as modified.

### Bench checks owed

- `python -m mp3link ping` on the Pi: 200 Hz STATE, no CRC errors, IMU_OK set.
- A policy session: the gait parks, the app gets 423, voice stops, ROS on the
  screen; 2 s after the last frame the legs glide into the stand and the app
  works again.
- ROS bridge and an RL policy together: whoever starts first keeps the robot.
- `pi_announce` running alone: the Pi's IP under the eyes, robot stays
  `standalone`.
- IMU at +-16 g: picked-up / push reflexes still trigger as before.

## Phase 7 changes (Lua: the VM's lock and timeouts, drawing)

### The VM (`main/lua/lua_vm.cc`)

Problems that predated this phase: nothing serialised the shared
`lua_State` between `/v1/lua/run` (httpd) and the chat worker; the timeout
every caller passed was ignored (a `while true do end` held the httpd task
forever); and runtime errors never reached the output, only compile errors.
Runs now take a mutex (10 s wait, then a "busy" error), a count hook
enforces the deadline (also checked per `robot.delay_ms` chunk; permission
prompts in `fs.write`/`fs.delete` do not count), and the error message is
appended to the output. `/v1/lua/run` gives a saved file 60 s like the chat
worker; inline scripts keep 5 s. **Behaviour change**: a PWA script that
ran longer than its timeout used to finish; now it is stopped and reports
"script timed out".

### A `display` module for Lua

mpv2-claw's Lua drew through ESP-Claw's `lua_module_lvgl`: a script built
LVGL widgets on a screen it held exclusively, and the drawing lived exactly
as long as the script, run as an async job. mpx-dog's VM runs one blocking
script at a time on the caller's task -- a script that had to stay alive to
keep its picture up would hold the VM, and with it the chat worker and
`/v1/lua/run`. So the picture outlives the script instead:

- **One canvas.** Lua paints the same 320x240 PSRAM canvas as Phase 6's
  `display_draw` (`display/cap_display.c`). Canvas creation and "show it,
  then hand back to the face" are split out of `cap_display_draw()` so both
  painters use them: one 150 KB buffer, one hold timer, and a text message
  is cancelled by either.
- **Primitives:** `cap_display_draw_fill/pixel/rect/circle/line/arc/text/
  blit/show`. Shapes and text go through LVGL's renderer under the lock;
  pixels, fills and blits write the buffer directly. `show()` invalidates
  the canvas, so redrawing a picture that is already up is seen.
- **The module** (`lua/lua_display.cc`): `display.clear/pixel/rect/circle/
  line/arc/text/blit/show/face/width/height/rgb`. Colours are 0xRRGGBB,
  `"#rrggbb"` or a name; coordinates accept floats. Text sizes 14/28/48 use
  the Montserrat fonts Phase 6 enabled.
- **Not an agent tool.** The agent keeps `display_draw` (JSON shapes); a
  second drawing tool taking Lua would leave the model choosing between two
  ways to do the same thing. The module is for scripts from the app and the
  chat worker -- including animation, which `display_draw` cannot do.

### Bench checks owed

- `POST /v1/lua/run {"script":"while true do end"}` answers "script timed
  out" after 5 s instead of hanging the app.
- `POST /v1/lua/run {"script":"display.clear('navy') display.circle(160,120,60,'gold') display.text(0,190,'hello',0xFFFFFF,{size=28,align='center',w=320}) display.show(5000)"}`:
  the picture for 5 s, then the eyes.
- A `display_draw` from the agent, then a Lua drawing while it is up: one
  canvas, the second picture replaces the first, the face comes back once.

## Design decisions

- **(Superseded in phase 2) Keep mpx-dog's sign conventions.** mpv2-claw
  changed the Stanford abduction signs and the joystick strafe sign together
  after bench verification on its own robot. In phase 1 mpx-dog's were kept;
  in phase 2 the robot core was replaced wholesale and mpv2's verified
  conventions came with it.
- **Feature detection in the app, not a build flag.** The same app build
  must work against the phase-1 firmware (no display, no AI) and later
  phases. Hiding rows on a 404 costs one request per Settings visit.
- **`none` fallback kept for unknown gaits.** mpv2-claw answers 404; the
  older mpx-dog apps rely on a 200. The claw app treats both the same.
- **Rest implemented with IDLE frames, not a 0 mA cap.** A 0 mA position
  command still makes the boards run their loop; IDLE mode is what mpv2-claw
  sends and what actually releases the joints.

## Known gaps in phase 1

- A Lua script or SDK host call that writes servos directly while the robot
  is resting re-engages the joints at whatever pose it commands, without the
  1.2 s wake glide. Only the gait-task paths go through `stand_up()`. The
  skill path is partly covered (a running skill makes the loop wake first),
  but a skill's first frames can race the glide.
- `skills::rescan()` reads every `.wasm` on the filesystem into RAM one at
  a time; with many large skills the upload/delete handlers get slower. Same
  as mpv2-claw.
- `/v1/skills/stop` is cooperative, like mpv2-claw's: a skill that never
  calls a host function keeps going until its 60 s watchdog.
- `power_state()` strings are mpv2-claw's and shown verbatim by the app.

## Open decisions (need the owner)

1. **(Decided in Phase 5) On-device agent.** The owner chose option (a):
   ESP-Claw integrated into mpx-dog -- but as a library inside mpx-dog's
   shell rather than mpx-dog's code inside ESP-Claw's shell. See "Phase 5
   changes". Still open within it: the handoff between the two brains (one
   conversation moving between Pupper and the cloud, and voice turns
   answered locally), which the per-conversation toggle stands in for.
2. **Voice gateway credentials.** `CONFIG_APP_VOICE_OTA_URL` defaults to the
   same `http://35.220.215.160:8000/xiaozhi/ota/` as mpv2-claw; the
   provisioning body identifies the robot as `mpx-dog` / `mp4_esp32_core`.
   If the gateway keys on app or board name this needs confirming. The
   agent's robot token (`CONFIG_APP_ROBOT_UUID` + HMAC of
   `CONFIG_APP_CHAT_AES_KEY_HEX`, Phase 5) uses mpx-dog's existing chat key,
   which is still the insecure default `...0001`; the gateway's `/v1/llm`
   has to know this robot's uuid/key pair for `mangdang` mode to answer.

## Build and verification

```powershell
cd pwa-claw && npm install && npm run build      # -> pwa-claw/www (gitignored)
cd .. && idf.py build                            # sdkconfig has PWA_DESIGN_CLAW=y
python tools\check_comments.py                  # no slash-asterisk inside comments (-Werror=comment)
```

After editing `main/idf_component.yml` or `main/Kconfig.projbuild`, run
`idf.py reconfigure` first (phase 4 added five managed components; the
component manager fetched them on reconfigure, not on build). Flashing
needs the data partitions too -- `idf.py flash` writes them
(`0x710000 build\srmodels\srmodels.bin`, `0x810000 build\system.bin`); a
plain `app-flash` leaves a robot with no wake-word model and an agent with
no skills.

Phase 5 build notes: `sdkconfig` is tracked; when `sdkconfig.defaults` gains
a value, delete `sdkconfig` and reconfigure (defaults only apply to a fresh
file), then diff the result against the old one before committing -- that
is how the unrecorded settings above were caught. `cap_boards` needs PyYAML
in the IDF Python (present in this venv); the first `system` image build has
`joltwallet/littlefs` create a Python venv and pip-install littlefs-python,
so it needs internet once. The system image is staged in `build/system_fs/`;
look there to see which skills the agent will find.

Phase 1 state: PWA builds (Vite, no warnings beyond Svelte's usual
`state_referenced_locally` notes inherited from mpv2-claw); firmware builds
with zero warnings; image 1.79 MB, 32 % of the 2.5 MB app slot free (was
2.01 MB with the redesign app, because that one shipped raw + gz).

Environment note: this machine has two ESP-IDF 5.5.4 trees
(`C:\esp\v5.5.4\esp-idf`, used by the IDE, and
`C:\Espressif\frameworks\esp-idf-v5.5.4`). `build/` had been configured with
the first; building from a shell that exports the second fails in the
bootloader sub-build with "source ... does not match the source used to
generate cache". `idf.py reconfigure` after deleting `build/bootloader`
clears it. `export.ps1` also picks up the system Python 3.13 instead of the
3.11 venv; run `tools/activate.py --export` with
`C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe` and dot-source
the `.ps1` it prints.

Bench checks still owed (no robot attached while porting):

- Rest: lower to 40 mm looks right on this chassis; IDLE really releases
  the joints on these driver boards (mpv2-claw's do); wake from the floor is
  a hold-then-lift, not a jump.
- Look: stick right = head right, stick up = nose up.
- Wi-Fi scan with a phone joined to the robot's AP stays connected (dwell
  times are mpv2-claw's).
- A skill with `provides_gait` appears in the app's move grid and runs from
  there; `/v1/skills/status` shows its slug.
- Old redesign-app phones pick up the new app (service worker cache
  `mpx-dog-v9-claw`).
