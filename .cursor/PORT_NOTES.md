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
| 5 | On-device agent (ESP-Claw or equivalent), `/v1/ai` "own" mode, chat answered locally | deferred by the owner; see "Open decisions" |

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

### The PSRAM-stack rule (differs from mpv2-claw)

mpv2-claw runs with `CONFIG_SPIRAM_XIP_FROM_PSRAM`, which makes a flash
write from a PSRAM-stacked task safe. mpx-dog does not, so here the rule
is: **a task whose stack is in PSRAM must never touch flash** (NVS, LittleFS,
OTA). The voice code already respects it -- mpv2 kept `wake_auto_task`
(provisioning writes NVS) on an internal stack and made the talk, playback,
feed, fetch and music tasks flash-free -- and the two new tasks follow it:
`voice_tools` is internal because of `robot_set_config`, `voice_ev` is
internal and tiny. Keep it that way when adding tools.

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

1. **On-device agent.** mpv2-claw's chat brain is ESP-Claw, a framework that
   owns boot, Lua, HTTP routes and memory. Options: (a) integrate ESP-Claw
   into mpx-dog as a component (large; effectively becomes mpv2-claw with
   mpx-dog's robot code), (b) keep mpx-dog's cloud chat and add only the
   agent-facing endpoints (`/v1/ai` as a mode switch for the cloud), (c) stop
   at phase 4. The owner chose to do phases 2-4 first and decide this later.
   What phase 4 already provides for it: `/v1/ai` with a `modes` list the
   app honours (adding `own` is a one-line change on each side), and a tool
   table (`voice_tools.cc`) whose names and schemas match mpv2-claw's
   capability descriptors, so an on-device agent could reuse it.
2. **Voice gateway credentials.** `CONFIG_APP_VOICE_OTA_URL` defaults to the
   same `http://35.220.215.160:8000/xiaozhi/ota/` as mpv2-claw; the
   provisioning body identifies the robot as `mpx-dog` / `mp4_esp32_core`.
   If the gateway keys on app or board name this needs confirming. The robot
   AES key / `MP4_SKILL_AES_KEY_HEX` path (gateway `/v1/llm` with a robot
   token) is Phase 5 material and was not ported.

## Build and verification

```powershell
cd pwa-claw && npm install && npm run build      # -> pwa-claw/www (gitignored)
cd .. && idf.py build                            # sdkconfig has PWA_DESIGN_CLAW=y
python ..\mpv2-claw\tools\check_comments.py      # no slash-asterisk inside comments (-Werror=comment)
```

After editing `main/idf_component.yml` or `main/Kconfig.projbuild`, run
`idf.py reconfigure` first (phase 4 added five managed components; the
component manager fetched them on reconfigure, not on build). Flashing
needs the `model` partition too -- `idf.py flash` writes it
(`0x600000 build\srmodels\srmodels.bin`); a plain `app-flash` leaves a
robot with no wake-word model, and `mpx_voice_wake_init` fails at boot.

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
