# MP4-Claw integration plan — rev 2

Bringing the **MP4 ESP32 CORE** board and the **MPX-Dog PWA + WASM skill runtime** onto **ESP-Claw**, structured the way **esp-mosaico-claw** structures a downstream ESP-Claw product.

Only `C:\esp\projects\mp4-claw` changes. `mangdang`, `SantaTest` and `esp-mosaico-claw` are read-only donors.

---

## 0. Decisions

| Question | Decision |
|---|---|
| Display panel | ST7789, 240×320 native, driven **320×240 landscape** |
| Skill runtime | Keep the WASM sandbox + marketplace; ESP-Claw is the chat/agent brain |
| Web UI | Port `pwa-redesign` (Svelte 5) in as the main UI; ESP-Claw's SolidJS settings UI stays reachable |
| **Repo shape** | **Restructure to the mosaico model** — esp-claw as a submodule, product code at the root |
| **Voice input** | **Not now.** Mics are wired and declared correctly; no ASR, no wake word, no AFE. §12 keeps the seam open. |
| **Voice output** | **Sound effects only** — chirps/barks for state changes, via `lua_module_audio` |

**Two facts that shape everything below:**

1. **`mp4-claw` is a pristine clone of `espressif/esp-claw` at `fb7b2481` (2026-08-20) with zero local commits.** Restructuring costs a day now and nothing later.
2. **`esp-mosaico-claw` runs an *unreleased* esp-claw** (submodule pinned at `1e048db6`, not present in the public repo). Its `audio_hub` and `claw_hw_registry` components **do not exist in your version**. Copy mosaico's *patterns*; you cannot copy its *components*.

---

## 1. Hardware map — MP4 ESP32 CORE

**MCU:** ESP32-S3-WROOM-1-**N16R8** (16 MB QIO flash, 8 MB octal PSRAM).

### The HeySanta discovery

`SantaTest/main/boards/HeySanta/` is the **same hardware family**. Its pin map matches yours almost exactly, which means its values are not guesses — they are debugged against real silicon:

| | HeySanta | MP4 | |
|---|---|---|---|
| I2S MCLK / BCLK / WS / DIN / DOUT | 38 / 14 / 13 / 12 / 45 | 38 / 14 / 13 / 12 / 45 | ✅ identical |
| I2C SDA / SCL | 1 / 2 | 1 / 2 | ✅ identical |
| LCD MOSI / SCK / DC / BL | 40 / 41 / 39 / 42 | 40 / 41 / 39 / 42 | ✅ identical |
| Buttons BOOT / WAKE | 0 / 21 | 0 / 21 | ✅ identical |
| ES7210 I²C address | `0x82` | schematic says 7-bit `0x41` → `0x82` | ✅ confirms |
| Backlight `output_invert` | `true` | (P-FET Q2 SI2301) | ✅ confirms |
| LCD `reset_gpio_num` | `GPIO_NUM_NC` | tied to board `RESET` | ✅ confirms |
| Sample rate in/out | 24000 / 24000 | — | adopt |
| **LCD CS** | `NC` (tied low; GPIO 9 = camera D7) | **GPIO 9** | ⚠️ only real difference |
| Camera | OV-series DVP on 3/4/5/6/7/8/9/15/16/17/18/46 | **none** | those GPIOs are free |

**Display values proven on HeySanta, adopt verbatim:** `swap_xy = true`, `mirror_x = true`, `mirror_y = false`, `invert_color = true`, `spi_mode = 2`, `pclk = 80 MHz`, `offset_x = offset_y = 0`, `lcd_cmd_bits = 8`, `lcd_param_bits = 8`, `bits_per_pixel = 16`, `rgb_ele_order = RGB`.

> HeySanta still calls `esp_lcd_panel_reset()` even with `reset_gpio_num = NC` — that issues a **software** reset (0x01) over SPI, which is exactly what makes an RST-tied-to-board-reset panel work. Keep the call.

### Full pin table

| Function | Net | GPIO |
|---|---|---|
| I2C0 SDA / SCL | `IO1_I2C_SDA` / `IO2_I2C_SCL` | **1 / 2** (1 kΩ pull-ups) |
| LCD CS / DC / MOSI / SCK / BL | `IO9/39/40/41/42` | **9 / 39 / 40 / 41 / 42** |
| LCD reset | board `RESET` net | **−1** |
| I2S MCLK / BCLK / WS / DIN / DOUT | | **38 / 14 / 13 / 12 / 45** |
| Servo SPI MOSI / CLK / MISO | `IO6/16/17` | **6 / 16 / 17** |
| Servo CS1..CS4 (CN3..CN6) | `IO15/7/4/5` | **15 / 7 / 4 / 5** |
| IMU INT1 | `IO10_INT1` | **10** |
| WAKE (SW3) / BOOT (SW2) | | **21 / 0** |
| Console UART (CH340K) | `U0TXD` / `U0RXD` | **43 / 44** |
| Expansion header J-PI | `IO18/8/3/11` | 18, 8, 3, 11 |
| Test points TP3 / TP4 | `IO19` / `IO20` | 19 / 20 |

### Devices

- **Display** — 10-pin TFT header J4 (`AFC34-S10FIA`): DC, SDA, CS, RST, SCL, GND, VDD, GND, BL_A, BL_K. Write-only 3-wire SPI, **no MISO, no touch controller, no touch pins.**
- **Audio in** — **ES7210** 4-ch ADC. Two ZTS6216 analog mics on MIC1/MIC2; **MIC3 is fed from the amplifier's OUTP/OUTN through R31/R32/R35/R36 — a hardware AEC reference.** MIC4 unused.
- **Audio out** — **MAX98357A**. `SD_MODE` pulled up through R24 1 MΩ only → mono `(L+R)/2`. `GAIN` to test point TP1 via R27 0 Ω. **No GPIO shutdown, no GPIO gain.**
- **IMU** — **BMI270** + **BMM150** on I2C0, INT1 on GPIO 10.
- **Servos** — 4 × `HC-PHD-2` (CN3–CN6), shared SPI + one CS each, powered from `Vbat+` with 22 µF per connector. Same 4 × AT32F413 driver boards as `mangdang/main/robot/driver_board.c`.
- **Power** — 3S Li (12.6 V) on CN1 or 12 V adapter on CN2, OR'd through D5. SY8105ADC → `VBUS` 5 V/5 A; two SY8088AAC → `3V3` and `AU_3V3`.

### ⚠️ Five hardware facts that break a naive port

1. **LCD reset is not a GPIO** → `reset_gpio_num: -1`. Confirmed by HeySanta.
2. **There is no servo power-enable pin.** `mangdang/main/robot/robot.h:206` defines `SERVO_POWER_PIN = 8` and `robot.cc:362` drives it high. On this board GPIO 8 is `IO8_PI_2` on the expansion header. **Delete it — don't repin it.**
3. **Console is UART0** (CH340K on 43/44), **not USB-Serial-JTAG** — IO19/IO20 go to bare test points. Most ESP-Claw boards set `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`; this one must not.
4. **No touch panel.** Don't declare an `lcd_touch` device.
5. **ES7210 address** — schematic annotates 7-bit `0x41` → `0x82`; HeySanta ships `0x82` and works. But every ES7210 board in esp-claw uses `0x80` (7-bit `0x40`), and R26/R28 strap AD0/AD1 ambiguously in the PDF. **I²C-scan bus 0 before trusting either.**

### Open hardware questions

- **`invert_color`** — HeySanta says `true`; if everything looks like a photo negative, flip it.
- **Panel gap** — 0/0 on HeySanta. If yours is offset, add `esp_lcd_panel_set_gap()` in `setup_device.c`.
- **Which `SERVO_BOARD` variant** is this robot? `driver_board.h` carries five wiring maps; default is 1.
- **Speaker** on J6 — impedance and power? MAX98357A into 4 Ω at 5 V is ~3 W.
- **All four legs populated** (12 servos)?

---

## 2. Phase 0 — restructure to the mosaico shape

### Why

`esp-mosaico-claw` never uses `EXTRA_COMPONENT_DIRS`. Every esp-claw component enters the build as an IDF Component Manager **`path:` dependency** declared in `idf_component.yml`, with `rules:` / `matches:` gating on Kconfig. Board discovery is separate, through `esp_board_manager` codegen into `components/gen_bmgr_codes/`.

**The good news: esp-claw already works this way.** `application/edge_agent/main/idf_component.yml` is already 11 `path:` deps pointing at `../../../components/...`. The restructure is mostly a path rewrite.

### Target tree

```
mp4-claw/
├── CMakeLists.txt                    ← new root project (mosaico's, FATFS variant)
├── .gitmodules
├── partitions_16MB.csv               ← copied from esp-claw, then adjusted (§11)
├── sdkconfig.defaults                ← copied from esp-claw's application/edge_agent/
├── requirements.txt                  ← esp-bmgr-assist>=0.8.2
├── third-party/esp-claw/             ← SUBMODULE, pinned at fb7b2481
├── tools/cmake/flash_partition_defaults.cmake   ← copied verbatim
├── boards/mp4_esp32_core/            ← §3
├── fatfs_image/{system,storage}/     ← copied from esp-claw, then extended
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml             ← the stitch point
│   ├── Kconfig.projbuild             ← CONFIG_MP4_ROBOT_ENABLE
│   ├── main.c                        ← seeded from esp-claw's, then extended
│   └── app_fs.c / app_fs.h
└── components/
    ├── app_config/                   ← from esp-claw's application/edge_agent/components/
    ├── http_server/                  ← ditto, then extended with /v1/* (§8)
    ├── mpx_robot/                    ← §4
    ├── mpx_wasm/                     ← §5
    ├── cap_robot/  cap_display/  cap_mpx_skill/   ← §6
    └── lua_module_mpx_robot/         ← §6
```

### Steps

```bash
cd C:\esp\projects\mp4-claw
git branch upstream-snapshot            # keep the pristine history
git remote rename origin upstream

# 1. clear the working tree (keep .git)
git rm -r --cached . && rm -rf application components docs pages tools .github .gitlab

# 2. esp-claw becomes a submodule, pinned
git submodule add https://github.com/espressif/esp-claw.git third-party/esp-claw
cd third-party/esp-claw && git checkout fb7b2481 && cd ../..

# 3. seed the product tree from the submodule
cp -r third-party/esp-claw/application/edge_agent/main            main
cp -r third-party/esp-claw/application/edge_agent/components/*    components/
cp    third-party/esp-claw/application/edge_agent/partitions_16MB.csv .
cp    third-party/esp-claw/application/edge_agent/sdkconfig.defaults   .
cp -r third-party/esp-claw/application/edge_agent/fatfs_image     fatfs_image
cp -r third-party/esp-claw/application/edge_agent/tools           tools
```

### `main/idf_component.yml` — the only real edit

Rewrite each `../../../components/X` as `../third-party/esp-claw/components/X`, and each `../components/X` as `../components/X` (unchanged — they're at the root now):

```yaml
dependencies:
  espressif/esp_board_manager: { version: "^0.6", public: true }

  app_claw:     { path: ../third-party/esp-claw/components/common/app_claw }
  captive_dns:  { path: ../third-party/esp-claw/components/common/captive_dns }
  claw_utils:   { path: ../third-party/esp-claw/components/claw_modules/claw_utils }
  http_reuse:   { path: ../third-party/esp-claw/components/common/http_reuse }
  settings:     { path: ../third-party/esp-claw/components/common/settings }
  wifi_manager: { path: ../third-party/esp-claw/components/common/wifi_manager }
  espressif/ramfs: "^0.1"

  system_ui:
    rules: [ { if: "$CONFIG{ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT} == True" } ]
    path: ../third-party/esp-claw/components/common/system_ui

  # product-owned, at the repo root
  app_config:   { path: ../components/app_config }
  http_server:  { path: ../components/http_server }

  # new for the robot
  mpx_robot:    { path: ../components/mpx_robot }
  mpx_wasm:     { path: ../components/mpx_wasm }
  cap_robot:    { path: ../components/cap_robot }
  cap_display:
    rules: [ { if: "$CONFIG{ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT} == True" } ]
    path: ../components/cap_display
  cap_mpx_skill: { path: ../components/cap_mpx_skill }
  lua_module_mpx_robot: { path: ../components/lua_module_mpx_robot }
```

Two idioms worth internalising: **`rules:`** = hard requirement gated on Kconfig (the dep is absent otherwise); **`matches:`** = optional inclusion. And `public: true` on `esp_board_manager` so board headers propagate.

### Root `CMakeLists.txt`

Mosaico's, minus the S31 IDF patches, and with **FATFS instead of LittleFS** — esp-claw at `fb7b2481` uses `fatfs_create_rawflash_image` / `fatfs_create_spiflash_image`, not `littlefs_create_partition_image`:

```cmake
cmake_minimum_required(VERSION 3.16)

include("${CMAKE_SOURCE_DIR}/tools/cmake/flash_partition_defaults.cmake")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mp4_claw VERSION 0.1.0)

set(MP4_FATFS_IMAGE_ROOT          "${CMAKE_SOURCE_DIR}/fatfs_image")
set(MP4_FATFS_SOURCE_ROOT         "${MP4_FATFS_IMAGE_ROOT}/storage")
set(MP4_FATFS_BUILD_ROOT          "${CMAKE_BINARY_DIR}/fatfs_image")
set(MP4_FATFS_SYSTEM_SOURCE_ROOT  "${MP4_FATFS_IMAGE_ROOT}/system")
set(MP4_FATFS_SYSTEM_BUILD_ROOT   "${CMAKE_BINARY_DIR}/system_fs_image")
set(MP4_BOARD_GENERATED_CMAKELISTS "${CMAKE_SOURCE_DIR}/components/gen_bmgr_codes/CMakeLists.txt")

# Board fatfs overlay: scrape the board path out of the GENERATED CMakeLists.
set(MP4_BOARD_FATFS_COPY_COMMANDS "")
if(EXISTS "${MP4_BOARD_GENERATED_CMAKELISTS}")
    file(STRINGS "${MP4_BOARD_GENERATED_CMAKELISTS}" MP4_BOARD_PATH_LINE
         REGEX "^message\\(STATUS \"Board Path: .+\"\\)$")
    if(MP4_BOARD_PATH_LINE)
        list(GET MP4_BOARD_PATH_LINE 0 MP4_BOARD_PATH_LINE)
        string(REGEX REPLACE "^message\\(STATUS \"Board Path: (.*)\"\\)$" "\\1"
               MP4_BOARD_PATH "${MP4_BOARD_PATH_LINE}")
        if(EXISTS "${MP4_BOARD_PATH}/fatfs_image")
            list(APPEND MP4_BOARD_FATFS_COPY_COMMANDS
                 COMMAND ${CMAKE_COMMAND} -E copy_directory
                         "${MP4_BOARD_PATH}/fatfs_image" "${MP4_FATFS_SYSTEM_BUILD_ROOT}")
        endif()
    endif()
endif()

add_custom_target(mp4_prepare_system_fs_image ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${MP4_FATFS_SYSTEM_BUILD_ROOT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MP4_FATFS_SYSTEM_BUILD_ROOT}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${MP4_FATFS_SYSTEM_SOURCE_ROOT}" "${MP4_FATFS_SYSTEM_BUILD_ROOT}"
    ${MP4_BOARD_FATFS_COPY_COMMANDS}
    VERBATIM)

fatfs_create_rawflash_image(system  "${MP4_FATFS_SYSTEM_BUILD_ROOT}" FLASH_IN_PROJECT)
fatfs_create_spiflash_image(storage "${MP4_FATFS_BUILD_ROOT}"        FLASH_IN_PROJECT)

skill_builder_configure_skill_sync(
    TARGET fatfs_system_bin
    SKILL_OUTPUT_DIR "${MP4_FATFS_SYSTEM_BUILD_ROOT}/skills")

lua_module_builder_configure_builtin_lua_sync(
    TARGET fatfs_system_bin
    BUILTIN_OUTPUT_DIR "${MP4_FATFS_SYSTEM_BUILD_ROOT}/scripts/builtin"
    DOCS_OUTPUT_DIR    "${MP4_FATFS_SYSTEM_BUILD_ROOT}/scripts/docs")

add_dependencies(skill_builder_sync_skills                 mp4_prepare_system_fs_image)
add_dependencies(lua_module_builder_sync_builtin_scripts   mp4_prepare_system_fs_image)
add_dependencies(lua_module_builder_sync_docs              mp4_prepare_system_fs_image)
add_dependencies(fatfs_system_bin                          mp4_prepare_system_fs_image)
```

**Ordering constraint to preserve:** `flash_partition_defaults.cmake` is included **before** `project.cmake`, because it mutates `SDKCONFIG_DEFAULTS`. Copy it verbatim from either repo — it's board-agnostic, scrapes `CONFIG_ESPTOOLPY_FLASHSIZE_*` out of `components/gen_bmgr_codes/board_manager.defaults`, strips stale `CONFIG_PARTITION_TABLE_*FILENAME` lines, and injects the right CSV.

### One Kconfig switch

`main/Kconfig.projbuild`, following mosaico's `APP_CLAW_MOSAIC_GSP_ENABLE`:

```kconfig
config MP4_ROBOT_ENABLE
    bool "MP4 quadruped robot support"
    depends on APP_CLAW_CAP_LUA
    depends on APP_CLAW_CAP_SKILL_MGR
    default n
    help
      Enable the MangDang quadruped robot stack: servo driver boards,
      gait generator, WASM skill runtime, and the /v1 PWA API.
```

### Phase 0 exit criteria

```bash
. $IDF_PATH/export.sh
pip install -r requirements.txt
git submodule update --init --recursive
idf.py bmgr -c ./boards -b esp32_S3_DevKitC_1     # a stock board, from the submodule
idf.py build
```

**Record `idf.py size`.** That baseline decides §11.

> Note: `idf.py bmgr -c ./boards` searches *your* boards dir. To build a stock esp-claw board before yours exists, point at the submodule's: `-c ./third-party/esp-claw/application/edge_agent/boards`.

---

## 3. Phase 1 — the board definition

`boards/mp4_esp32_core/` — five files. Smallest, most independently testable piece. Flash it before touching anything else.

### `board_info.yaml`
```yaml
board: mp4_esp32_core
chip: esp32s3
version: 1.0.0
description: "MangDang MP4 ESP32 CORE - quadruped core board with TFT, mic array and speaker"
manufacturer: "MangDang"
```

### `board_peripherals.yaml`
```yaml
version: 1.0.0
peripherals:
  - name: i2c_master
    type: i2c
    role: master
    config:
      port: 0
      pins: { sda: 1, scl: 2 }

  # I2S0 full duplex: ES7210 in, MAX98357A out. Anchor/alias like esp_box_3.
  - name: i2s_audio_out
    type: i2s
    role: master
    format: std-out
    config: &i2s0_config
      port: 0
      sample_rate_hz: 24000          # HeySanta runs 24 kHz both ways
      mclk_multiple: 256
      data_bit_width: 16
      slot_bit_width: I2S_SLOT_BIT_WIDTH_AUTO
      slot_mode: I2S_SLOT_MODE_STEREO
      slot_mask: I2S_STD_SLOT_BOTH
      ws_width: 16
      pins: { mclk: 38, bclk: 14, ws: 13, dout: 45, din: 12 }

  - name: i2s_audio_in
    type: i2s
    role: master
    format: std-in
    config: *i2s0_config

  - name: spi_display
    type: spi
    role: master
    config:
      spi_bus_config:
        spi_port: SPI2_HOST
        data0_io_num: 40           # MOSI, 3-wire write-only
        sclk_io_num: 41
        max_transfer_sz: 153600    # 320 * 240 * 2, as HeySanta

  - name: ledc_backlight
    type: ledc
    role: none
    config:
      gpio_num: 42
      freq_hz: 5000
      duty: 0
      duty_resolution: LEDC_TIMER_10_BIT
      output_invert: true          # P-FET Q2; HeySanta ships invert=true
```

### `board_devices.yaml`
```yaml
version: 1.0.0
devices:
  - name: display_lcd
    chip: st7789
    type: display_lcd
    sub_type: spi
    version: default
    config:
      mirror_x: true               # ─┐
      mirror_y: false              #  ├─ all four proven on HeySanta
      swap_xy: true                #  │
      invert_color: true           # ─┘
      x_max: 320                   # landscape; native panel is 240x320
      y_max: 240
      io_spi_config:
        cs_gpio_num: 9             # ⚠ HeySanta ties CS low; you have a real pin
        dc_gpio_num: 39
        spi_mode: 2                # HeySanta
        pclk_hz: 80000000          # HeySanta; back off to 40 MHz if it tears
        lcd_cmd_bits: 8
        lcd_param_bits: 8
        flags:
          sio_mode: true           # no MISO on J4
      lcd_panel_config:
        reset_gpio_num: -1         # ⚠ RST is on the board RESET net
        bits_per_pixel: 16
        rgb_ele_order: LCD_RGB_ELEMENT_ORDER_RGB
        data_endian: LCD_RGB_DATA_ENDIAN_BIG
        vendor_config: NULL
    peripherals:
      - name: spi_display

  - name: lcd_brightness
    type: ledc_ctrl
    version: default
    config:
      default_percent: 80
    peripherals:
      - name: ledc_backlight

  # MAX98357A: no I2C, no PA GPIO. chip: internal == "raw I2S, no codec".
  - name: audio_dac
    chip: internal
    type: audio_codec
    version: default
    config:
      adc_enabled: false
      dac_enabled: true
      dac_max_channel: 2
      dac_channel_mask: "11"
      dac_init_gain: 0
      mclk_enabled: false
    peripherals:
      - name: i2s_audio_out

  # ES7210. Channel layout is IDENTICAL to esp_box_3: mic3 is the AEC reference.
  #   MSB~LSB = [mic4, mic3, mic2, mic1]
  #   'RE' is esp-claw's label for the reference channel.
  - name: audio_adc
    chip: es7210
    type: audio_codec
    version: default
    config:
      adc_enabled: true
      dac_enabled: false
      adc_max_channel: 4
      adc_channel_mask: "0111"
      adc_channel_labels: ['NA', 'RE', 'FR', 'FL']
      adc_init_gain: 30
      mclk_enabled: true
    peripherals:
      - name: i2s_audio_in
      - name: i2c_master
        address: 0x82              # ⚠ 7-bit 0x41 << 1; I²C-scan to confirm (0x80 also plausible)
        frequency: 400000

  - name: imu_sensor
    chip: bmi270
    type: custom
    version: 1.0.0
    init_skip: true
    config:                        # field order MUST match lua_imu_board_cfg_t
      i2c_addr: 0x68
      frequency: 400000
      int_gpio_num: 10
      sdo_gpio_num: -1
    peripherals:
      - name: i2c_master

  - name: magnetometer_sensor
    chip: bmm150
    type: custom
    version: 1.0.0
    init_skip: true
    config:
      i2c_addr: 0x10
      frequency: 400000
      int_gpio_num: -1
      sdo_gpio_num: -1
    peripherals:
      - name: i2c_master
```

**Copy templates, all inside the submodule:**
- `boards/espressif/esp_box_3/` — the ES7210 channel layout you need (`"0111"` + `['NA','RE','FR','FL']`, mic3 = reference). Your board's MIC3-fed-from-amp is the same arrangement.
- `boards/dfrobot/dfrobot_k10/` — the audio split: an I²C-controlled ADC codec plus a codec-less I2S amp (`chip: internal`), no PA GPIO.
- `boards/waveshare/waveshare_esp32_s3_geek/` — ST7789 over SPI, plus the `esp_lcd_panel_set_gap()` idiom if your panel turns out to be offset.
- `boards/espressif/esp_Ditto/` — the BMI270 + BMM150 pair as `type: custom`.

**On `custom` device configs:** the generator emits struct fields *in YAML order* and *silently drops absent keys*; `lua_module_imu.c` casts `desc->cfg` with a `cfg_size` equality check. Every field in `lua_imu_board_cfg_t` must be present, in order, `-1` for unused pins — otherwise `ESP_ERR_INVALID_SIZE` at runtime.

### `setup_device.c`
```c
#include "esp_lcd_panel_st7789.h"

esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *cfg,
                                    esp_lcd_panel_handle_t *ret_panel)
{
    return esp_lcd_new_panel_st7789(io, cfg, ret_panel);
}
```
ST7789 is built into IDF's `esp_lcd` — **no `dependencies:` block needed**, unlike ILI9341/GC9A01/CO5300.

### `sdkconfig.defaults.board`
```
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y

# 16 MB QIO @ 80 MHz (flash_partition_defaults.cmake scrapes FLASHSIZE from here)
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
CONFIG_ESPTOOLPY_FLASHFREQ_80M=y
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y

# 8 MB octal PSRAM  (values lifted from HeySanta's working N16R8 config)
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=512
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=65536
CONFIG_SPIRAM_MEMTEST=n
CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y
CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB=y
CONFIG_ESP32S3_DATA_CACHE_64KB=y
CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y

# Codecs
CONFIG_CODEC_DUMMY_SUPPORT=y          # MAX98357A "internal" DAC path
CONFIG_CODEC_ES7210_SUPPORT=y
CONFIG_CODEC_I2C_BACKWARD_COMPATIBLE=n  # esp_codec_dev takes i2c_master_bus_handle_t

# Board device gates
CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT=y
CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT=y

# IMU + magnetometer Lua modules
CONFIG_APP_CLAW_LUA_MODULE_IMU=y
CONFIG_LUA_MODULE_IMU_CHIP_BMI270=y
CONFIG_APP_CLAW_LUA_MODULE_MAGNETOMETER=y

# Robot
CONFIG_MP4_ROBOT_ENABLE=y

# ⚠ Console is the CH340K on UART0, NOT USB-Serial-JTAG.
CONFIG_ESP_CONSOLE_UART_DEFAULT=y

# The PWA opens 2 WebSockets per tab plus polls. esp-claw's app defaults already
# set CONFIG_LWIP_MAX_SOCKETS=20; these two are the TIME_WAIT fixes mangdang
# learned the hard way.
CONFIG_LWIP_TCP_MSL=5000
CONFIG_LWIP_SO_LINGER=y
```

### Phase 1 exit criteria
```bash
idf.py bmgr -c ./boards -b mp4_esp32_core     # rerun after EVERY yaml edit
idf.py build flash monitor
```
- All devices initialise; I²C scan finds ES7210, BMI270, BMM150
- Backlight responds; ESP-Claw's boot screen appears, right way up, right colours
- `lua_module_audio` records from `audio_adc` and plays back through `audio_dac`

**Do not go past this until the panel and mics work.** Everything downstream assumes them.

---

## 4. Phase 2 — the robot layer (`components/mpx_robot/`)

Port from `mangdang/main/robot/`. Keep the C++; rewriting 3000 lines of proven gait code in C is a bad trade, and esp-claw's C-style-OO convention applies to *new* modules.

```
components/mpx_robot/
├── CMakeLists.txt
├── README.md                  ← agent-facing, if you expose it to Lua
├── include/mpx_robot.h
└── src/
    ├── driver_board.c         ← REPINNED
    ├── robot.cc               ← minus servo power, minus IMU
    ├── stanford_gait.c        ← verbatim (179 lines)
    ├── stanford_kinematics.c  ← verbatim (169 lines)
    └── mpx_imu.c              ← NEW: BMI270 via board manager
```

### Changes, file by file

**`driver_board.c/.h`** — repin, drop the power rail:
```c
/* was: SPI2_HOST MOSI=11 MISO=13 CLK=12, CS 9/10/21/14, power GPIO8 */
#define DB_SPI_HOST   SPI3_HOST     /* SPI2 belongs to the LCD */
#define DB_MOSI_GPIO  6
#define DB_MISO_GPIO  17
#define DB_CLK_GPIO   16
static const int DB_CS_GPIO[4] = { 15, 7, 4, 5 };   /* FR, FL, RR, RL */
```
- Delete `driver_board_power()` and all callers. **No power-enable pin on this board.**
- Delete `robot::SERVO_POWER_PIN` and the `gpio_set_level(8, 1)` in `robot::init()`.
- **`driver_board_bus_lock()` / `_unlock()` can become no-ops** — the long comment in `driver_board.h` about the IMU stealing SPI2 mid-config-exchange no longer applies now that the IMU is on I²C. *Keep the driver's own internal mutex* — Servo Studio and the gait task still race.
- Confirm the `SERVO_BOARD` variant.

**`imu.cc` → `mpx_imu.c`** — full rewrite; QMI8658C/SPI is gone. **Reuse esp-claw's driver:** `third-party/esp-claw/components/lua_modules/lua_module_imu/src/bmi270/bmi270_backend.c` already wraps `espressif/bmi270_sensor` and resolves the `imu_sensor` board device. Write a thin C shim so `robot::imu_read()` keeps its `ImuData {ax,ay,az,gx,gy,gz}` shape. One driver, one owner. Keep the polling task and the thread-safe copy — `skills/events.cc` polls it at 10 Hz for `imu.lifted` / `imu.fallen` / `imu.shaken`.

**`robot.cc`** — mostly verbatim. Watch:
- Gait task is pinned to core 1 at high priority. Check nothing else in esp-claw is pinned there at ≥ that priority.
- **NVS namespace collision.** `robot.cc` opens its own handle for servo offsets and config; esp-claw's `app_config` uses the default namespace. Use an explicit `mpx_robot` namespace.
- `NEUTRAL_Z = 70.0f` must equal `SK_NEUTRAL_HEIGHT_MM` in `stanford_kinematics.c`. It does today. Don't let them drift.
- Servo offsets in NVS were calibrated on the old board's servo scale (`robot.h:27`, the 1.5× note). **Recalibrate after first flash.**

**`stanford_gait` / `stanford_kinematics`** — verbatim. Pure math, no hardware.

### Phase 2 exit criteria
A console command (esp-claw's `app_claw_cli` already runs one) that runs `advance`, `stanford`, `lookup`, reads back servo positions and temperatures, and prints an IMU sample. **The robot walks from a serial prompt before any web or agent code exists.**

---

## 5. Phase 3 — the WASM runtime (`components/mpx_wasm/`)

Port `mangdang/main/{wasm,sdk,skills}/` essentially unchanged. **ABI 4 must stay byte-compatible** — that's the point of keeping WASM: your existing `.wasm`/`.mpxe` skills keep running.

```
components/mpx_wasm/src/
├── wasm_sandbox.cc          ← verbatim
├── wasm_decrypt.cc          ← verbatim (MPXE envelope)
├── wasm_host_functions.cc   ← 74 symbols, ABI 4 — verbatim
├── registry.cc              ← "mpx" custom-section manifest parser
├── runner.cc                ← one-skill-at-a-time, OneShot/Behaviour
├── events.cc                ← boot / imu.lifted / imu.fallen / imu.shaken / chat:*
├── autorun.cc               ← safe-mode counter
└── movement.cc              ← builtin-vs-skill gait name resolution
```

### What must change

**Storage root.** mangdang keeps skills at LittleFS root with provenance in `/installed.json`. esp-claw has FATFS at `CLAW_PATH_DATA`.

> **Never hard-code `/fatfs`.** esp-claw's rule (`AGENTS.md`, "Runtime Path Rules") is `claw_paths_join(CLAW_PATH_DATA, ...)`. Use `<DATA>/mpx_skills/*.wasm|.mpxe` and `<DATA>/mpx_skills/installed.json`. Bonus: they show up in esp-claw's `/api/files` browser and to `cap_files` for free.

**`events.cc`** — `fire_chat()` currently hooks `POST /v1/chat/send`. Rewire it to fire from the chat bridge (§9) so `"on":["chat:dance"]` still works. Publish an esp-claw router event rather than reaching into `claw_core`.

**`wasm_decrypt.cc`** — the MPXE key comes from `CONFIG_APP_CHAT_AES_KEY_HEX`, shared with the (now removed) chat encryption. Keep the symbol, rename it to `CONFIG_MPX_SKILL_AES_KEY_HEX`, keep the `#ifdef NDEBUG` all-zeros refusal. **Skills stay hardware-bound to `CONFIG_APP_ROBOT_UUID` via the `wasm-wrap:v1:<uuid>` AAD — break that and every purchased skill stops loading.**

**Threading.** `load_and_run_bytes()` must stay on a **pthread**, not `xTaskCreate` — WAMR's ESP-IDF platform layer calls `pthread_self()`. Documented in `wasm_sandbox.cc`, not optional.

**PSRAM.** 128 KB runtime heap + 32 KB operand stack + 128 KB linear memory per instance, all PSRAM; 16 KB native pthread stack from internal RAM. Already covered by §3's board defaults.

**Boot order matters.** From `mangdang/main/main.cc`:
> the HTTP server starts **before** `autorun_boot()`, so that if an autorun skill misbehaves the web UI is already up and the user can uninstall it. That's the difference between a bad skill and a brick.

In esp-claw's `main.c`, `http_server_start()` is step 17 and `app_claw_start()` step 22 — hook `rescan()` / `events_start()` / `autorun_boot()` **after** `app_claw_start()`. Order preserved.

### Phase 3 exit criteria
Upload a known-good `.wasm` over the console or `/api/files/upload`, run it from the CLI, watch it walk. Then the same with an `.mpxe`.

---

## 6. Phase 4 — capabilities, Lua and sounds

This is where "maximum potential" lives, and where mosaico's evidence is decisive.

### What a real downstream product actually uses

I grepped every extension-point call site in `esp-mosaico-claw`. The complete answer:

| Extension point | Call sites in mosaico | Verdict |
|---|---|---|
| `app_capabilities_register_external_group()` | **1** — `main/main.c:1064`, immediately before `app_claw_start()` | **The** capability extension point |
| `claw_cap_register_group()` | **2** — always from inside the owning component, invoked by the `.reg` callback, guarded by `claw_cap_group_exists()` | The idiom |
| `claw_event_router_register_outbound_binding()` | `main.c:107` — the return path | Don't forget it |
| `cap_lua_register_module()` | **1** — `lua_module_lcd_touch.c:262`, called directly from the module | 263 lines, one call |
| `app_lua_modules_register_external()` | **0** | Dead code in practice |

**Mosaico forked `app_claw` but never needed to for the extensions it shipped** — `cap_im_ai_create` and `lua_module_lcd_touch` both go in through the external registries with **zero edits to the compiled tables**. The fork exists only to own the 482-line Kconfig and the 361-line dependency manifest.

**So: extend, don't fork.** Use upstream's `common/app_claw`; register everything externally.

### The four groups

```c
/* in app_main(), BEFORE app_claw_start() */
app_capabilities_register_external_group(&(app_capability_external_group_t){
    .group_id = "cap_robot",
    .display_name = "Robot",
    .llm_visible_by_default = true,
    .reg = cap_robot_register_group,
});
```
External groups are appended in both `app_capabilities_init()` and `app_capabilities_get_compiled_groups()`, so they appear in esp-claw's settings UI automatically.

| Group | Tools | LLM-visible by default |
|---|---|---|
| `cap_robot` | `robot_move` (gait name or `{f,s,t}` drive), `robot_pose` (roll/pitch/yaw), `robot_get_state` (mode, config, IMU, servo temps), `robot_set_config` | **yes** |
| `cap_display` | `display_show_text`, `display_show_emotion`, `display_set_brightness`, `display_clear` | **yes** |
| `cap_mpx_skill` | `mpx_list_skills`, `mpx_run_skill` (name + params), `mpx_stop_skill`, `mpx_skill_status` | **yes** |
| `cap_servo_studio` | `servo_get_param`, `servo_set_param`, `servo_save_config`, `servo_scan` | **no** — `CLAW_CAP_FLAG_RESTRICTED`, unlocked per-session by a skill |

### The descriptor pattern

Copy `third-party/esp-claw/components/claw_capabilities/cap_llm_inspect/` (111 lines, one tool, no state), or mosaico's `cap_im_ai_create` for the group-registration shape:

```c
static const claw_cap_descriptor_t s_robot_descriptors[] = {
    {
        .id = "robot_move",
        .name = "robot_move",
        .family = "robot",
        .description = "Make the robot walk, turn or perform a named movement. "
                       "Use the list from robot_get_state.movements.",   /* ≤256 bytes! */
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{"
            "\"movement\":{\"type\":\"string\"},"
            "\"forward\":{\"type\":\"number\",\"minimum\":-1,\"maximum\":1},"
            "\"strafe\":{\"type\":\"number\",\"minimum\":-1,\"maximum\":1},"
            "\"turn\":{\"type\":\"number\",\"minimum\":-1,\"maximum\":1},"
            "\"duration_ms\":{\"type\":\"integer\",\"maximum\":10000}}}",
        .execute = cap_robot_execute_move,
    },
};

static const claw_cap_group_t s_robot_group = {
    .group_id = "cap_robot",
    .descriptors = s_robot_descriptors,
    .descriptor_count = sizeof(s_robot_descriptors) / sizeof(s_robot_descriptors[0]),
};

esp_err_t cap_robot_register_group(void)
{
    return claw_cap_group_exists(s_robot_group.group_id) ? ESP_OK
                                                         : claw_cap_register_group(&s_robot_group);
}
```

**Three hard limits from `claw_cap.c`:**
- `CLAW_CAP_TOOL_DESCRIPTION_MAX = 256` bytes; longer is silently truncated. **Long instructions go in a SKILL.md, not the descriptor.**
- `type: "object"` with no `properties` warns and can cause an LLM API 400.
- Output buffer is 32 KiB from the agent loop (PSRAM-preferred).

**Safety.** `execute()` runs on the caller's task and may block, but the agent loop has `max_tool_iterations = 32`. Cap `duration_ms`, and make `robot_move` return promptly ("started") rather than blocking through a 10-second gait — exactly as `POST /v1/robot/gait` does.

Mosaico patterns worth lifting from `cap_im_ai_create`: an explicit protocol version validated on every request; a fixed `pending[8]` slot table instead of dynamic allocation; `EXT_RAM_BSS_ATTR` state with a `DRAM_ATTR StaticSemaphore_t`.

### Skills that unlock them

Ship a `skills/` dir in each capability component — the build syncs every component's `skills/` into the SYSTEM image automatically:

```
components/cap_robot/skills/mpx_robot/SKILL.md
```
```md
---
{
  "name": "mpx_robot",
  "description": "Drive the MangDang quadruped: walk, turn, pose, and read servo and IMU state.",
  "metadata": {
    "category": ["robot"],
    "peripherals": ["display"],
    "cap_groups": ["cap_robot", "cap_display"],
    "manage_mode": "readonly"
  }
}
---

# MangDang quadruped

...the long instructions that don't fit in 256 bytes: the 46 movement names,
`stanford` vs `advance`, safe pose limits, what to do when a servo reports
over 60 °C...
```

`metadata.cap_groups` is the sanctioned per-session unlock: `cap_skill_mgr` feeds it into `claw_cap_set_session_llm_visible_groups()` when the skill activates. That's how `cap_servo_studio` stays hidden until someone deliberately opens it.

### The Lua module — the highest-leverage piece

`cap_lua` is already LLM-visible and `lua_run_script` is already a tool. A `lua_module_mpx_robot` (mirroring mangdang's `ROBOT_LIB[]` 28 functions, in **degrees**) means the agent can *write and run new movement scripts* rather than only calling fixed tools.

Follow `mosaico/components/lua_module_lcd_touch/` — 263 lines, one file, one `cap_lua_register_module("mpx_robot", luaopen_mpx_robot)` call, plus a `README.md`. **That README is what the model reads as the API contract**; write it carefully.

`.agents/design.md` explicitly prefers this route:
> If a behavior can live in a capability group, Lua module, skill, router rule, board overlay, or context provider, keep it out of the core loop.

### Sound effects

esp-claw has no TTS, but `lua_module_audio` plays WAV/MP3 from storage and over HTTP/HTTPS. That's enough:

- Put short WAVs in `fatfs_image/system/sounds/` (`listening.wav`, `thinking.wav`, `ok.wav`, `error.wav`, `bark.wav`, `startup.wav`) — they ride in the read-only SYSTEM partition.
- A `lua_module_mpx_sound` or a handful of Lua helpers wrap `audio.player` over `board_manager.get_audio_codec_output_params("audio_dac")`.
- Wire them to router rules and to `cap_robot` (a bark when a gait starts, an error chirp on a servo fault).
- Expose one `play_sound {name}` tool so the agent can punctuate its own replies.

**Watch the volume path.** `chip: internal` means there is no codec to set gain on — `esp_codec_dev_set_out_vol` is a no-op. HeySanta applies volume in software with a squared curve (`(vol/100)^2 * 65536`) before writing to I2S. If `lua_module_audio`'s `volume` doesn't take effect, that's why.

---

## 7. Phase 5 — the `/v1/*` compatibility layer

The PWA calls **68 routes**. Reimplement its route table on top of the ported layers rather than rewriting the PWA.

Mosaico's answer to "how do I extend esp-claw's http_server": **shadow the whole component.** `components/http_server` with a `path:` dep beats upstream's, and upstream's is then never referenced. You already copied it to the root in Phase 0, so this is just editing your own copy.

```
components/http_server/
├── http_server_mpx_robot_api.c      ← /v1/robot/*, /v1/studio/*
├── http_server_mpx_skills_api.c     ← /v1/skills/*, /v1/lua/*
├── http_server_mpx_fs_api.c         ← /v1/fs/*
├── http_server_mpx_wifi_api.c       ← /v1/wifi/*
├── http_server_mpx_market_api.c     ← /v1/marketplace/*, /v1/gateway/config
├── http_server_mpx_chat_ws.c        ← /v1/chat/ui  (§8)
└── http_server_mpx_assets.c         ← the PWA (§9)
```

### Mapping

| PWA route group | Backing |
|---|---|
| `/v1/robot/gait\|joy\|status\|config\|calibrate*\|movements\|diagnostic/*` | `mpx_robot` directly; near-verbatim handler port |
| `/v1/studio/*` (11) | `driver_board_*_param` / `_live` / `_direct`. Verbatim |
| `/v1/skills/*` (12) | `mpx_wasm` registry + runner, storage root moved to `<DATA>/mpx_skills/`. Keep `skill_filename_ok()` **verbatim** — it's the upload validator |
| `/v1/lua/*` (6) | **Rewire to `cap_lua`.** `cap_lua_run_script(path, args_json, timeout_ms, out, size)` and `cap_lua_run_script_async(...)` replace mangdang's hand-rolled `lua_vm.cc`; `enqueue` → `run_script_async` |
| `/v1/fs/*` (4) | Thin adapters over esp-claw's existing `/api/files*` helpers |
| `/v1/wifi/*` (5) | `wifi_manager_*` + `app_config` + `POST /api/restart`. **Bonus:** `wifi_manager_scan_aps()` exists but has no route — add `GET /v1/wifi/scan` and give the PWA a real network picker instead of typed SSIDs |
| `/v1/logs`, `/v1/trace` | Port `log_ring.cc` / `trace_ring.cc` as-is (16 KB + 6 KB **internal** RAM, deliberately). The PWA doesn't call them today; **add a log view** — it's a real gap |
| `/v1/marketplace/*`, `/v1/gateway/config` | `marketplace_proxy.cc` verbatim, still pointing at `35.220.215.160:8080`. **Keep the 15 s GET cache and `MAX_INFLIGHT_GATEWAY = 3`** — both exist because the socket pool ran dry without them |
| `/v1/telemetry/stream` | Dead echo stub. **Drop it** |
| `/v1/chat/send`, `/v1/chat/ui` | §8 |

### Three changes in `http_server_core.c`

1. **`max_uri_handlers = 32` → `88`.** esp-claw uses 21 of 32; the PWA needs ~68 more. mangdang's file carries a long comment about marketplace routes *silently* failing to register when the table filled — **port `register_uri()`'s failure counter too**, so a full table logs `UNREGISTERED: POST /v1/xxx` instead of mystifying 404s.
2. **Sockets.** esp-claw has `max_open_sockets = 12` with `CONFIG_LWIP_MAX_SOCKETS=20`; httpd needs `max_open_sockets + 3`, so 12 fits. But **port `socket_reaper_task`** anyway (closes zombie and half-open port-80 sockets every 4 s), plus the `esp_log_level_set("httpd_txrx"/"httpd_ws", ESP_LOG_ERROR)` calls that silence per-disconnect ECONNRESET spam.
3. **SPA fallback.** esp-claw has none and its own UI uses hash routing; the PWA uses real client-side paths. Add mangdang's `static_handler` behaviour: unknown non-`/v1/` paths fall back to `/index.html`.

Also worth copying from mosaico's `http_server`: a **PSRAM-preferring httpd task stack**, and the **services vtable** (`http_server_services_t`) so the server never links against `app_config` / `wifi_manager` directly. esp-claw already has the vtable — extend it rather than adding direct calls.

---

## 8. Phase 6 — the chat bridge

Today: PWA ⇄ `ws://robot/v1/chat/ui` ⇄ AES-GCM frames ⇄ cloud gateway ⇄ LLM.
After: PWA ⇄ `ws://robot/v1/chat/ui` ⇄ **`claw_core` on the ESP32**.

**Keep the PWA's protocol; put an adapter behind it.** esp-claw's `/ws/webim` is much simpler than what `ChatView.svelte` expects, and rewriting the chat UI would cost the permission modal, the step display, chunk reassembly and the conversation sidebar.

| PWA expects | esp-claw provides | Adapter work |
|---|---|---|
| WS in `{"type":"user_chat_input","text","session_id"}` | `cap_im_local_emit_user_message("web", chat_id, "web_user", NULL, text, links, n)` | Map `session_id` → `chat_id`. **`chat_id` becomes the agent session id, so conversation memory works across reloads for free** |
| WS in `{"type":"session_reset"}` | `cap_session_mgr` delete-session handler | Direct call |
| WS in `{"type":"permission_response","action_id","approved"}` | — | Keep mangdang's `s_perm_semaphore` machinery |
| WS out `{"type":"chat_reply","text","commands":[]}` | outbound binding `"web"` → `local_send_message` → your callback | Wrap. `commands: []` — the agent executes tools itself now |
| WS out `chat_reply_chunk` | one frame per complete message | Keep the `SINGLE_MAX = 2048` / `CHUNK_TEXT = 1536` splitter — it's for buffer/TCP pressure, not the LLM |
| WS out `{"type":"step",...}` | — | **The good bit.** Emit a `step` per tool call. `claw_cap_call()` is the choke point; hook it and the user watches the robot think |
| WS out `command_result` | tool result | Same hook |
| WS out `openclaw_action` | — | Keep, for file writes during marketplace deploys |

**Mosaico's alternative, worth knowing:** it injects UI text into the agent with `claw_agent_mgr_post_root_message(&input, 5000, &receipt)` and branches on `receipt.disposition` — `CLAW_CORE_MESSAGE_APPENDED_TO_RUN` means an existing run will answer, otherwise a worker task blocks on the run. That's a cleaner API than the IM-event path if you don't need the router in the middle. Both work; the IM path gets you router rules for free.

**Permissions.** `network::request_permission()` blocks a worker, broadcasts `status:"pending"`, waits on a semaphore, and **auto-denies immediately if no PWA client is connected** rather than hanging. Keep that verbatim — it's the human-in-the-loop gate on every marketplace skill install (`fs.write` / `fs.delete` from a deploy script).

**Deleted:** `crypto.cc`'s chat consumer, `chat_ws.cc`'s upstream socket, `build_upstream_frame()`, the `chat_upstream` task, `CONFIG_APP_CHAT_SERVER_IP/PORT/WS_PATH`. Keep `crypto.cc` itself — `wasm_decrypt.cc` still needs AES-GCM. Keep `/v1/gateway/config` — the storefront still lives there.

**No application-level PING.** mangdang deliberately doesn't send WS PINGs:
> the browser's PONG response (masked per RFC 6455) can desync the ESP-IDF HTTP server's internal WS frame parser

TCP keepalive instead (idle 10 s / intvl 3 s / cnt 3). Carry it over.

**Client budget.** One browser tab = 2 sockets (`ChatView` plus `app.svelte`'s global permission socket). mangdang allows 4, esp-claw's webim 8. Set the bridge to 8 and keep "evict only dead clients, reject if all alive" — it stops a reconnect storm killing a good session.

---

## 9. Phase 7 — the PWA as the front door

```
components/http_server/
├── frontend_source/     ← esp-claw SolidJS UI (unchanged) → /settings
└── frontend_pwa/        ← copy of mangdang/pwa-redesign → /
```

### Embedding

The PWA is **multi-file**: `index.html` 433 B, `index.css` 23.3 KB, `m.js` 101 KB, `manifest.json` 310 B, `sw.js` 2.6 KB, 3 SVGs — **129 KB gzipped**, plus `studio.html.gz` at 12 KB. esp-claw's own frontend is a single `viteSingleFile` bundle at 75 KB.

Keep the multi-file layout; port mangdang's `www_assets.cc` approach:

```cmake
set(pwa_www "${CMAKE_CURRENT_LIST_DIR}/frontend_pwa/www")
idf_component_register(
    SRCS ${http_server_srcs}
    INCLUDE_DIRS "include"
    EMBED_FILES
        "${FRONTEND_DIST_HTML_GZ}"      # esp-claw settings UI → /settings
        "${FRONTEND_PUBLIC_FAVICON_ICO}"
        "${pwa_www}/index.html.gz"      # PWA → /
        "${pwa_www}/index.css.gz"
        "${pwa_www}/m.js.gz"
        "${pwa_www}/manifest.json.gz"
        "${pwa_www}/sw.js.gz"
        "${pwa_www}/md.svg.gz"
        "${pwa_www}/eye-open.svg.gz"
        "${pwa_www}/eye-close.svg.gz"
        "${CMAKE_CURRENT_LIST_DIR}/studio/studio.html.gz"
    REQUIRES ${http_server_requires}
)
```

Symbol naming: **filename only, path stripped, non-alphanumerics → `_`** — `index.css.gz` becomes `_binary_index_css_gz_start`. So the PWA's `vite.config.js` must keep its fixed output names (`entryFileNames: "m.js"`, css → `index.css`); cache-busting is the CRC32-derived ETag, not hashed filenames.

**Keep `studio.html.gz` outside the Svelte bundle.** mangdang's reason is right:
> this is the page you need when the robot will not walk, so it must not depend on the app build.

### Rewiring `frontend_pwa/src/`

Only three things change:

1. **`lib/chat/ChatView.svelte` and `app.svelte`** — nothing, if you build the §8 bridge. That's why you build it.
2. **Add a link to `/settings`** for LLM provider, API key, memory, IM channels. esp-claw's config surface has no PWA equivalent and shouldn't be reimplemented.
3. **`lib/SkillsManagement.svelte`** — drop `localStorage: mpx_deployed_skills`. Legacy client-owned tracking that `/v1/skills/installed` + `/v1/skills/record` already replaced in `SkillsView`. Two sources of truth otherwise.

**Optionally add** (endpoints that exist but no screen calls): a log/trace viewer, `/v1/skills/stop`, `/v1/skills/safe-mode/clear` (a real gap — a wedged autorun skill currently needs a serial cable), and `/v1/robot/movements` in `AddActionView` instead of a raw file browser.

**Service worker.** `public/sw.js` already forces network-only for `/v1/*`. **Add `/api/*` and `/files/*`** or esp-claw's own endpoints get cached.

### Dev loop
```bash
cd components/http_server/frontend_pwa
ROBOT=192.168.4.1 npm run dev     # vite proxies /v1 + /studio, ws:true
npm run build                     # → www/*.gz via gzip-www.mjs
```
Add `/api`, `/files` and `/ws` to the vite proxy so the settings UI works in dev too.

---

## 10. Phase 8 — the on-device display

With `CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT=y` from Phase 1, esp-claw's `system_ui` (LVGL 9.5 launcher fed by skill `execution` blocks), `display_service`, `lua_module_display`, `lua_module_lvgl` and `lua_module_lcd` all come up **for free**.

In order of value:

1. **`cap_display`** (§6) — text, emotions, brightness. Small; makes "chat to change the display" work immediately.
2. **A robot status face** — battery, gait mode, WiFi, servo temperature. As a Lua skill using `lua_module_lvgl`, not C, so the agent can modify it.
3. **Skill launcher tiles** — give each WASM skill an `execution` block with an `icon`. Note `execution.entry` is **`.lua`-only** (`skill_payload_path_is_valid`); a `.wasm` entry is dropped with a warning and the skill still loads without a tile. Ship a 3-line Lua shim per skill that calls `mpx_run_skill`.

**Two things mosaico does that are worth stealing later:**
- **`mosaico_boot_splash`** — a bootloader-side component (via `BOOTLOADER_EXTRA_COMPONENT_DIRS`) that paints the panel before the app starts, with a bootloader→app handoff protocol so the app's panel init skips the redundant reset. Sub-second time-to-first-pixel.
- **`display_service`'s three modes** (`SHARED_LVGL` / `EXCLUSIVE_LVGL` / `EXCLUSIVE_RAW` with session handles) — you need exactly this arbiter once a Lua skill can draw LVGL *and* a product UI owns the screen. esp-claw's `display_service` already exists in your tree; check whether it has the session API yet.

**No touch panel** — navigation is the WAKE button (GPIO 21) plus the web UI.

---

## 11. Budget

### Flash (16 MB, `partitions_16MB.csv`)

| Region | Size |
|---|---|
| `ota_0` / `ota_1` (app) | 5 MB **each** |
| `system` (FAT, read-only) | 2600 KB |
| `storage` (FAT, wear-levelled) | 3 MB |

**The app partition is the constraint.** Additions:

| Item | Est. |
|---|---|
| esp-claw baseline | measure in Phase 0 — likely 3.0–3.8 MB |
| WAMR (interpreter, no AOT) | 250–350 KB |
| `mpx_robot` | ~120 KB |
| PWA + studio, embedded gz | 141 KB |
| esp-claw SolidJS UI (already there) | 75 KB |
| New capabilities + API layer | ~80 KB |
| Sound effects (WAVs) | SYSTEM partition, not app |

Mitigations if it's tight, in order:
1. `CONFIG_COMPILER_OPTIMIZATION_SIZE=y` + `CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH=y`.
2. Disable IM channels you don't want: `CONFIG_APP_CLAW_CAP_IM_QQ/FEISHU/TG/WECHAT=n`. WeChat is large.
3. Serve the PWA from FATFS via `lua_module_http_server`'s `app:mount_static()`. Costs `storage` (3 MB) instead of app flash, **and embedded assets cost app flash twice** — once per OTA slot. Downside: the UI isn't up until the agent boots.
4. Custom `partitions_16MB_mp4.csv`: single 7 MB app, no `ota_1`. **Last resort** — you lose OTA rollback. (For reference, HeySanta ships an asymmetric 6 M / 3 M, which quietly breaks OTA once the app passes 3 MB. Don't copy that.)

`storage` at 3 MB holds sessions, memory, user skills and `mpx_skills/`. At ~50–250 KB per `.wasm` that's roughly 15–50 skills; mangdang had 13.4 MB. Grow it in a custom CSV if the marketplace library is large.

### RAM

PSRAM (8 MB) is comfortable: WAMR ~290 KB/instance, Lua state, LVGL buffers, the agent's 32 KB tool output buffer. Internal RAM is tight — WAMR's 16 KB pthread stack, the log/trace rings (22 KB, deliberately internal), lwIP with 20 sockets, esp-claw's 16 KB core task.

**One number from HeySanta worth copying:** LVGL's draw buffer is deliberately **not** in PSRAM — `buff_dma = 1, buff_spiram = 0`, single buffer of `width * 20` px (320×20×2 = 12.8 KB internal DMA-capable), no double buffering, `swap_bytes = 1`.

---

## 12. Deferred — voice, and the seam that keeps it open

Not in scope now. But three decisions here cost nothing today and a rewrite later:

1. **Declare all three ES7210 channels in the board YAML now** (§3 does: `"0111"` + `['NA','RE','FR','FL']`). The `adc_channel_labels` vector *is* the channel-format declaration an AFE consumes, and `RE` names your reference channel. Getting it into the board definition now means the voice work later is additive.
2. **Keep audio access behind `board_manager` + `lua_module_audio`,** never a direct `i2s_channel_read()`. When esp-claw's `audio_hub` (mixer + capture subscribers with ducking) lands publicly, swapping to it is a component change, not a rewrite.
3. **Reserve the model partition decision.** If you later want on-device wake word, esp-sr needs ~960 KB of SPIFFS for one WakeNet9 + nsnet + vadnet (4 MB for a MultiNet custom phrase). That has to come out of the 16 MB somewhere. Decide before you commit to a partition layout in Phase 0.

When you do want voice, the two references split cleanly:
- **Capture/ASR plumbing → mosaico's `asr_service`**: an `asr_provider_ops_t` vtable (create/connect/start_stream/send_audio/finish_stream/get_final_text/disconnect/delete), a capture task that *never blocks on network*, a bounded PSRAM PCM ring between capture and sender, and 20 ms frames. Engine-agnostic and well factored. It delivers text into the agent via `claw_agent_mgr_post_root_message()` on a custom IM channel — **ASR text enters as an IM message, not a "voice capability"**.
- **AEC/wake word → xiaozhi's `afe_audio_processor` / `afe_wake_word`**: `afe_config_init(format, models, AFE_TYPE_VC|SR, AFE_MODE_HIGH_PERF)`, format string built as all-`M`-then-all-`R`. With your MIC1 + MIC3-reference that's `"MR"`; with both mics, `"MMR"` (new de-interleave code — xiaozhi hardcodes the 2-channel case).

The join: an AFE feed/fetch task *upstream* of mosaico's capture task, pushing clean mono 16 kHz frames into the same PCM ring. Everything below that point works unchanged.

---

## 13. Risks

| # | Risk | Mitigation |
|---|---|---|
| 1 | **App partition overflow** | Measure in Phase 0. §11, in order |
| 2 | **Board manager may not init an SPI peripheral no device references.** The servo bus has no `display_lcd`/`fs_fat` consumer, and no in-repo board attaches a `custom` device to SPI | Don't declare `spi_servo` in YAML. Call `spi_bus_initialize()` directly in `driver_board.c` and document the pins in a `custom` device config for the board-info skill. This is how mangdang already does it |
| 3 | **Two SPI hosts, both busy.** LCD on SPI2 at 80 MHz, servos on SPI3, PSRAM on the octal bus | Separate hosts, no shared CS. If the display tears while walking, drop `pclk_hz` to 40 MHz |
| 4 | **Gait task vs. agent task CPU contention.** Gait is high-priority on core 1; LLM inference is bursty | Pin the agent to core 0 with httpd. Gait jitter shows up as a limping robot, not a log line |
| 5 | **ES7210 address wrong** → no mic, silent failure | I²C-scan in Phase 1. Candidates: `0x80` (esp-claw boards) or `0x82` (HeySanta + your schematic) |
| 6 | **Backlight polarity** | HeySanta says `invert = true`. Two-minute test in Phase 1 |
| 7 | **NVS namespace collision** between `robot.cc` offsets and esp-claw's `app_config` | Explicit `mpx_robot` namespace |
| 8 | **Servo offsets from the old board are on the wrong scale** (`robot.h:27`, the 1.5× note) | Recalibrate after first flash. Say so in the README |
| 9 | **Skill shadowing direction is ambiguous.** The spec says DATA skills win; `init_skills()` adds `/system/skills` first, which makes SYSTEM win | Verify empirically before relying on it |
| 10 | **MPXE skills are hardware-bound to `CONFIG_APP_ROBOT_UUID`** | Keep the UUID identical to the current firmware, or every purchased skill fails to unwrap |
| 11 | **`fs.write` / `crypto.base64_decode` / `wasm.run_bytes` from a cloud deploy script is arbitrary code execution**, gated only by the permission prompt | Keep the prompt. When rewiring `/v1/lua/*` onto `cap_lua`, make sure the gate survives the move. Most security-relevant line in the port |
| 12 | **`max_uri_handlers` overflow fails silently** | Bump to 88 **and** port the registration failure counter |
| 13 | **Submodule drift.** Pinning at `fb7b2481` is right, but esp-claw moves | Pin deliberately; bump on purpose with a build+flash test. Never `--remote` blind |
| 14 | **`display_service` / `system_ui` API gaps.** Mosaico's versions are richer than the public ones (session handles, TE presentation) | Check what your `fb7b2481` copy actually exposes before designing around it |

---

## 14. Order

| Phase | Deliverable | Independently testable? |
|---|---|---|
| **0** | **Repo restructure + baseline size** | ✅ stock board builds from the submodule |
| 1 | Board definition, 5 files | ✅ display + audio + IMU work |
| 2 | `mpx_robot` | ✅ robot walks from the CLI |
| 3 | `mpx_wasm` | ✅ existing `.wasm`/`.mpxe` skills run |
| 4 | Capabilities + Lua module + sounds | ✅ **you can chat to the robot over esp-claw's own UI at `/`** |
| 5 | `/v1/*` compat layer | ✅ `curl` every route |
| 6 | Chat bridge | ✅ |
| 7 | PWA front door | ✅ the whole thing |
| 8 | On-device display | ✅ |
| — | *(deferred)* voice | §12 |

**Phase 4 is the milestone worth pausing at** — the first point where the robot is genuinely agent-driven, and it lands before any UI work. Flash it and play with it before committing to 5–7.

Phases 2 and 3 are independent of each other and of 5–7; parallelisable.

### Files touched in `mp4-claw`

**Restructure (Phase 0):** everything moves; `third-party/esp-claw` becomes a submodule pinned at `fb7b2481`; `main/` and `components/{app_config,http_server}` are seeded from `application/edge_agent/`.

**New:**
```
CMakeLists.txt  .gitmodules  requirements.txt  tools/cmake/flash_partition_defaults.cmake
boards/mp4_esp32_core/{board_info,board_devices,board_peripherals}.yaml
boards/mp4_esp32_core/{setup_device.c,sdkconfig.defaults.board,README.md}
main/Kconfig.projbuild
components/mpx_robot/**   components/mpx_wasm/**
components/cap_robot/**   components/cap_display/**   components/cap_mpx_skill/**
components/lua_module_mpx_robot/**
components/http_server/http_server_mpx_*.c
components/http_server/frontend_pwa/**        (from mangdang/pwa-redesign)
components/http_server/studio/studio.html.gz  (from mangdang/studio)
fatfs_image/system/sounds/*.wav
```

**Modified:**
```
main/main.c              ← external cap registration, skills boot hooks
main/idf_component.yml   ← path deps into the submodule + product components
components/http_server/{CMakeLists.txt,http_server_core.c}
```

**Untouched, inside the submodule:** `claw_core`, `claw_cap`, `claw_event_router`, `claw_memory`, `claw_skill`, `cap_lua`, `common/app_claw`, `common/wifi_manager`, every `lua_module_*`. Everything new hangs off documented extension points — `app_capabilities_register_external_group`, `cap_lua_register_module`, `claw_cap_register_group`, `claw_event_router_register_outbound_binding` — which is exactly the set mosaico proved is sufficient.
