# AGENTS.md -- mpv2-uros

**mpv2-uros = mpv2-claw + micro-ROS**: the robot is a ROS 2 node itself, over
Wi-Fi to a micro-ROS agent on the PC; no Raspberry Pi. Read `README_UROS.md`
first. What this tree adds to mpv2-claw:

- `components/micro_ros/`: micro-ROS (Jazzy) PREBUILT for this IDF and
  sdkconfig. Never edit it by hand; rebuild with `tools/uros/rebuild_microros.sh`
  (Linux/WSL2) after an IDF update or an `app-colcon.meta` change.
- `components/mpx_uros/`: the node. All rcl/rclc calls stay on its one task.
- `MP4_PI_LINK_ENABLE` is off (sdkconfig.defaults); the Pi link code is still
  in the tree until phase 1 removes it.

Everything below is mpv2-claw's guide and still applies.

---

# AGENTS.md -- mpv2-claw

MP4 ESP32-S3 robot firmware = **mp4-claw's ESP-Claw integration** (this tree's
skeleton) + **mpv2's robot** (minipupperesp movement) + MangDang voice gateway
(zero setup) + later: mpv2's Pi/ROS link and the OpenClaw market cloud.

**Read `docs/ESP_CLAW_PLAN.md` first**: decisions, stage status, the RAM rules.

What differs from mp4-claw (everything else below this header is mp4-claw's
own guide and still applies):

- `components/mpx_robot/src/robot/` is mpv2's robot code, verbatim except the
  WASM coupling, which goes through `mpx_robot_set_skill_hooks()` (now with a
  `request_stop` hook). The IMU is still mp4-claw's `mpx_imu.cc` (board-manager
  I2C, shared with the ES7210), now 100 Hz and filling `seq`/`t_us`.
- Partition table: one 7 MB factory app, table at 0x8000 and nvs at 0x9000
  exactly as mpv2 -- moving a robot from mpv2 keeps NVS (Wi-Fi, calibration).
- Screen is turned 180 degrees at boot (`MP4_DISPLAY_FLIP180_DEFAULT`, NVS,
  `display flip|mirror|reset|info|test`).
- Voice provisions itself against the MangDang gateway on first wake word
  (`MP4_VOICE_OTA_URL`), re-provisioning once if a connect fails.
- With no LLM key at /settings, the agent uses the gateway's `/v1/llm` with a
  robot token from `MP4_SKILL_AES_KEY_HEX` (`MP4_GATEWAY_LLM_URL`).

---

# AGENTS.md

Firmware for the MangDang MP4 ESP32 CORE quadruped, built on ESP-Claw.

## The one rule

**Never edit anything under `third-party/esp-claw/`.** It is a pinned git
submodule. Everything this repo adds hangs off documented extension points, so
taking an upstream update is a submodule bump rather than a merge. If you find
yourself wanting to change an ESP-Claw file, the answer is almost always one
of: a capability group, a Lua module, a skill, a router rule, a board overlay,
or a component that shadows the ESP-Claw one by name.

ESP-Claw's own agent guide lives at `third-party/esp-claw/AGENTS.md` and is
worth reading for anything about the agent runtime itself.

## Layout

```
CMakeLists.txt            project root
boards/mp4_esp32_core/    board definition (ST7789, ES7210, MAX98357A, BMI270)
components/               product-owned components
  app_config/             persisted settings, projected into app_claw_config_t
  http_server/            device config server + embedded UI (shadows ESP-Claw's)
  cmd_wifi/               console wifi command
  mpx_robot/              the quadruped: servos, gait, IK, IMU
main/                     app entry point and service wiring
fatfs_image/              baked into the SYSTEM (read-only) and DATA (seed) partitions
third-party/esp-claw/     ESP-Claw, pinned submodule -- DO NOT EDIT
tools/cmake/              partition selection, ESP-IDF patches
.agents/                  design notes, including the integration plan
```

## Build

```bash
source /path/to/esp-idf/export.sh
python -m pip install -r requirements.txt
git submodule update --init --recursive

idf.py bmgr -c ./boards -b mp4_esp32_core
idf.py build
idf.py -p PORT flash monitor
```

`idf.py set-target` is not needed -- the target comes from `chip:` in
`boards/mp4_esp32_core/board_info.yaml`.

**Rerun `idf.py bmgr` after editing anything under `boards/`** -- not only the
YAML. `sdkconfig.defaults.board` is copied into the generated
`components/gen_bmgr_codes/board_manager.defaults`, and only that generated
file is on `SDKCONFIG_DEFAULTS`. Edit the board defaults without rerunning
bmgr and your settings are simply absent, with nothing to say so.

Config defaults only apply to a **fresh** `sdkconfig`; an existing one wins.
`bmgr` handles that for you when the *board changes*, but **not** when you edit
`sdkconfig.defaults.board` for the board already selected -- it logs
"Board unchanged, preserving sdkconfig" and your new defaults silently do
nothing. That case needs the delete:

```bash
idf.py bmgr -c ./boards -b mp4_esp32_core   # regenerate board_manager.defaults
rm sdkconfig                                # only needed when the board is the same
idf.py build
```

Either way, losing `sdkconfig` discards anything set through menuconfig -- the
LLM API key and `MP4_GATEWAY_HOST` being the ones that matter here. The backup
`bmgr` leaves at `components/gen_bmgr_codes/sdkconfig.bmgr_board.old` is where
to go looking for them.

The console is the CH340K on UART0, not USB-Serial-JTAG.

### On Windows: box-drawing characters

esp-sr's `model/movemodel.py` prints a box-drawing rule in its model report.
Under the default cp1252 console codec that raises `UnicodeEncodeError` and
fails the build at "Move and Pack models" -- before any of our code, with an
error that looks nothing like the cause. It is a bug in esp-sr, still present
on their master, so there is no version to bump to.

**Already handled.** The top-level `CMakeLists.txt` patches that script at
configure time to substitute unencodable characters rather than raise. It
re-applies on every configure, so a re-downloaded component cannot bring the
bug back. Nothing to set per shell.

Optional, for output that renders properly rather than as `ΓöîΓöÇ`:

```powershell
chcp 65001
```

That affects only how tables from `idf.py size` are drawn; the numbers are
correct either way.

### Switching between boards

```bash
idf.py fullclean                                    # recommended, see below
idf.py bmgr -c ./boards -b mp4_esp32_core           # or generic_esp32s3
idf.py build flash monitor
```

No `rm sdkconfig` here: **when the board changes, `bmgr` deletes `sdkconfig`
itself**, backing it up to
`components/gen_bmgr_codes/sdkconfig.bmgr_board.old`. It clears `CMakeCache.txt`
too, so no separate reconfigure is needed either.

`bmgr` also regenerates `components/gen_bmgr_codes/board_manager.defaults`,
which is the only place a board's settings reach `SDKCONFIG_DEFAULTS` -- and
where `flash_partition_defaults.cmake` scrapes `CONFIG_ESPTOOLPY_FLASHSIZE_*`
to pick `partitions_<size>.csv`. So the partition table follows the board
automatically.

`fullclean` is not strictly required, but a board switch changes the partition
table, whether esp-sr is in the build at all, and the staged filesystem images.
A full rebuild costs ten minutes and removes every question about stale
artefacts -- worth it when the point of the exercise is to trust the result.

`idf.py flash` writes the bootloader, partition table, app and all three data
partitions, which is what a changed layout needs.

The console moves with the board: the MP4 uses the CH340K on UART0
(`ESP_CONSOLE_UART_DEFAULT`), the generic devkit uses USB-Serial-JTAG. Expect a
different port, and a board that appears to say nothing if you open the old one.

## How ESP-Claw components enter the build

Through IDF Component Manager `path:` dependencies in `main/idf_component.yml`,
not `EXTRA_COMPONENT_DIRS`. Two idioms:

- `rules:` -- hard requirement gated on a Kconfig symbol; the dependency is
  absent when the condition is false.
- `matches:` -- optional inclusion when the condition holds.

## Extension points

These are the seams. Prefer them in this order.

| Want to | Use |
|---|---|
| Give the agent a new tool | a capability group, registered with `app_capabilities_register_external_group()` from `app_main` **before** `app_claw_start()` |
| Give Lua a new module | `cap_lua_register_module(name, luaopen_fn)`, called from the module's own component |
| Give the agent long instructions | a `skills/<id>/SKILL.md` in your component; the build syncs it into the SYSTEM image automatically |
| Receive agent replies on your channel | `claw_event_router_register_outbound_binding(channel, "<send_handler>")` |
| Describe hardware | the board YAML, not C |

Capability descriptions are truncated at **256 bytes** (`CLAW_CAP_TOOL_DESCRIPTION_MAX`).
Long instructions belong in a SKILL.md, not the descriptor.

## Runtime paths

Never hard-code `/fatfs` or `/system`. Compose paths with
`claw_paths_join(CLAW_PATH_DATA, ...)` / `claw_paths_join(CLAW_PATH_SYSTEM, ...)`.
DATA may live on flash or an SD card depending on the board.

## Board YAML gotchas

- The generator emits `custom` device struct fields **in YAML order** and
  silently omits absent keys. A consuming C mirror struct must match field for
  field; use `-1` for unused GPIOs rather than dropping the key. A mismatch
  shows up as `ESP_ERR_INVALID_SIZE` at runtime if the consumer checks
  `cfg_size`, and as garbage if it does not.
- A scalar's C type is chosen from the literal's magnitude. Keep masks as
  strings.
- Device names are load-bearing: `display_lcd`, `lcd_touch`, `audio_dac`,
  `audio_adc`, `imu_sensor`, `magnetometer_sensor`, `fs_sdcard`,
  `lcd_brightness`.
- **A board with a `display_lcd` device also needs a `setup_device.c` in its
  board folder**, defining `lcd_panel_factory_entry_t`.
  `dev_display_lcd_sub_spi.c` declares that symbol `extern` and leaves the
  board to supply it -- so a board that only has YAML files builds every
  object file and then dies at the final link with

      undefined reference to `lcd_panel_factory_entry_t'

  Nothing in the YAML suggests a C file is required, and the error names a
  file inside `managed_components/`, so it reads like a broken dependency
  rather than a missing board file. Copy `boards/mp4_esp32_core/setup_device.c`
  and change the panel constructor.
- **After ADDING a file to a board folder, run `idf.py reconfigure`.** The
  generated `components/gen_bmgr_codes/CMakeLists.txt` pulls the board in with
  `SRC_DIRS "../../boards/<board>"`, and CMake expands that glob at configure
  time only. A plain `idf.py build` does not re-run CMake unless a
  `CMakeLists.txt` changed, so a newly added `.c` file is invisible and the
  link fails with exactly the same error as if the file did not exist. Editing
  a file that was already there is fine; adding one is not.

## This board, specifically

Five things that are not obvious and have already cost time once:

1. The LCD reset pin is the board `RESET` net, not a GPIO (`reset_gpio_num: -1`).
2. There is no servo power-enable pin; GPIO 8 is on the expansion header.
3. The console is UART0 via CH340K, not USB-Serial-JTAG.
4. There is no touch controller.
5. ES7210 MIC3 is an echo-cancellation reference fed from the amplifier, not a
   microphone. Labelled `RE` in `board_devices.yaml`.

`boards/mp4_esp32_core/README.md` has the full pin table and the list of values
that still need confirming on hardware.

## PSRAM is load-bearing, and XIP_FROM_PSRAM is part of that

This firmware does not run without PSRAM, and `CONFIG_SPIRAM_XIP_FROM_PSRAM`
is not an optimisation you can switch off.

ESP-Claw puts task stacks in external RAM by default: `claw_task_memory_caps()`
returns `MALLOC_CAP_SPIRAM` for every policy except `INTERNAL_ONLY`, and
`cap_mcp_server` does it unconditionally, with no Kconfig guard. A flash read
from one of those tasks disables the instruction cache, and with the cache off
the task's own stack is unreachable. IDF asserts rather than faulting:

    assert failed: spi_flash_disable_interrupts_caches_and_other_cpu
    cache_utils.c:127 (esp_task_stack_is_sane_cache_disabled())

`XIP_FROM_PSRAM` is what makes it safe -- with code executing from PSRAM the
cache is never disabled for a flash read. Turn it off and the firmware works
until the first request that touches the filesystem, then dies inside `stat()`,
which reads like a filesystem bug and is not one.

Two consequences:

- **Do not chase `SPIRAM_IGNORE_NOTFOUND`.** It is incompatible with
  `XIP_FROM_PSRAM` and the other fixed-address PSRAM placements. A board with
  no PSRAM should fail loudly at boot; it could not run the agent anyway
  (`app_claw_start()` returns `ESP_ERR_NO_MEM`).
- **Our own httpd task stack is pinned to internal RAM regardless**
  (`http_server_core.c`). It serves files from the FAT partition on every
  request, so it is the one task guaranteed to hit flash. That fix stands on
  its own and does not depend on the XIP setting -- which is the point.

## Internal compiler error in esp_lcd_panel_rgb.c: just build again

    during RTL pass: ira
    esp_lcd_panel_rgb.c: In function 'rgb_panel_draw_bitmap':
    internal compiler error: Segmentation fault

This is GCC crashing, not a problem with the code -- and the file is
ESP-IDF's, not ours. It has happened twice in this project, both times on
that same function, and both times a plain re-run of `idf.py build` compiled
it without complaint.

Non-deterministic, most likely memory pressure during a wide parallel build.
Re-run first. If the same file fails twice more, `idf.py build -j 4` narrows
the parallelism and clears it. Do not go looking for a cause in whatever was
changed just before it -- the timing is coincidence.

## -Werror=comment

A `/*` inside a block comment is a **build failure**, not a warning. This has
broken the build three times, always while documenting a URI pattern or a
glob. The message points at the comment rather than at anything wrong with the
code, so it reads like a spurious diagnostic and gets skimmed past.

Write the pair in prose ("a slash followed by an asterisk") or in a string
literal, never inside a comment. Before committing:

    python tools/check_comments.py

It walks every `.c`/`.h`/`.cc` outside `build/`, `third-party/` and
`managed_components/` as a character stream -- tracking string and char
literals so it does not false-positive on real code -- and exits non-zero with
file:line for each offence.

## Silent boot loop: read this before debugging anything else

Symptom: the terminal repeats the ROM banner (`ESP-ROM:esp32s3-...`, `load:`,
`entry 0x403c8920`) and nothing else. No `I (31) boot:` line, no app log, no
backtrace.

The missing **second-stage bootloader banner is the tell**. The ROM bootloader
always prints on UART0 no matter how the project is configured; everything
after `entry` prints on whatever `CONFIG_ESP_CONSOLE_*` selects. If you see one
and not the other, the console is on a port you are not watching -- almost
always USB-Serial-JTAG while you are on the UART bridge. The chip is not hung;
it is panicking and rebooting with the evidence on the other port.

`rst:0xc (RTC_SW_CPU_RST)` with `Saved PC: ... esp_restart_noos` confirms it:
that is a *software* restart, so app code ran and called it -- the panic
handler, in practice.

Both boards now default to UART0 primary with USB-Serial-JTAG as a secondary
sink, so this should not recur. If you change a console setting, change it in
`boards/<board>/sdkconfig.defaults.board` and remember the rule below.

## Editing sdkconfig.defaults.board does nothing on its own

`idf.py bmgr` deletes `sdkconfig` **only when the board changes**. On an
unchanged board it prints `Board unchanged, preserving sdkconfig` and your edit
never reaches the build -- the old value is already in `sdkconfig` and wins.

    idf.py bmgr -c ./boards -b generic_esp32s3
    del sdkconfig            # rm sdkconfig on POSIX -- this is the step people skip
    idf.py build

**`-c ./boards` is not optional.** Without it bmgr scans only
`managed_components/` and `components/`, finds the thirteen Espressif dev
boards and none of ours, and fails with `Board "generic_esp32s3" not found`
followed by a list that looks authoritative and is not. Passing the board as a
positional argument (`idf.py bmgr generic_esp32s3`) is also rejected as of
esp-bmgr-assist 0.8.2 -- it prints a usage hint and exits **0** having done
nothing, which reads like success.

This is what made the esp-sr model options appear to be ignored for an
afternoon. Check `components/gen_bmgr_codes/board_manager.defaults` if you want
to see what was actually handed to the build.

## The plan

`.agents/mp4-integration-plan.md` is the phase-by-phase integration plan, with
the hardware map and the reasoning behind the structure. Phases 0-2 are done.
