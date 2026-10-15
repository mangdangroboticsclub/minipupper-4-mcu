# MangDang MP4 ESP32 CORE

Core board for the MangDang quadruped. ESP32-S3-WROOM-1-N16R8 (16 MB QIO
flash, 8 MB octal PSRAM) with a 240x320 SPI TFT, a two-microphone ES7210
array with a hardware echo-cancellation reference, a MAX98357A speaker
amplifier, a BMI270 + BMM150 IMU, and four SPI connectors for the AT32F413
servo driver boards.

Source of truth: `MP4ESP32_CORE_SCH_0827.pdf` (Altium, 4 sheets, 2026-08-27).

## Pin map

| Function | Schematic net | GPIO |
|---|---|---|
| I2C0 SDA / SCL | `IO1_I2C_SDA` / `IO2_I2C_SCL` | 1 / 2 |
| LCD CS / DC / MOSI / SCK (SPI3) | `IO9_LCD_CS` / `IO39_LCD_DC` / `IO40_LCD_MOSI` / `IO41_LCD_SCK` | 9 / 39 / 40 / 41 |
| LCD backlight | `IO42_LCD_BL` (via Q2 SI2301 P-FET) | 42 |
| LCD reset | board `RESET` net | — |
| I2S MCLK / BCLK / WS | `IO38_I2S_MCK` / `IO14_I2S_BCK` / `IO13_I2S_WS` | 38 / 14 / 13 |
| I2S DIN (ES7210 → ESP) | `IO12_I2S_DI` | 12 |
| I2S DOUT (ESP → MAX98357A) | `IO45_I2S_DO` | 45 |
| Servo SPI MOSI / CLK / MISO (SPI2 -- it has six CS lines, SPI3 only three) | `IO6_SPI_MOSI` / `IO16_SPI_CLK` / `IO17_SPI_MISO` | 6 / 16 / 17 |
| Servo CS1..CS4 (CN3..CN6) | `IO15_SPI_CS1` / `IO7_SPI_CS2` / `IO4_SPI_CS3` / `IO5_SPI_CS4` | 15 / 7 / 4 / 5 |
| IMU INT1 | `IO10_INT1` | 10 |
| BOOT (SW2) / WAKE (SW3) | `IO0_BOOT` / `IO21_WAKE` | 0 / 21 |
| Console UART (CH340K) | `U0TXD` / `U0RXD` | 43 / 44 |
| Expansion header J-PI | `IO18_PI_1` / `IO8_PI_2` / `IO3_PI_3` / `IO11_PI_4` | 18 / 8 / 3 / 11 |
| Test points TP3 / TP4 | `IO19` / `IO20` | 19 / 20 |

Power: 3S Li battery (12.6 V) on CN1 or a 12 V adapter on CN2, OR'd through
D5/D1. SY8105ADC steps down to `VBUS` 5 V (5 A max); two SY8088AAC provide
`3V3` and `AU_3V3`. Servos are powered directly from `Vbat+`.

## Five things that are not obvious

1. **The LCD reset pin is not a GPIO.** It is wired to the board `RESET` net,
   so `reset_gpio_num` is `-1` and esp_lcd issues a software reset (0x01) over
   SPI instead. This works, but the panel cannot be reset independently of the
   whole board.

2. **There is no servo power-enable pin.** The servo rail is connected to
   `Vbat+` unconditionally. The earlier MangDang board used GPIO 8 for this;
   here GPIO 8 is `IO8_PI_2` on the expansion header, so driving it would put
   a signal on the connector.

3. **The console is UART0, not USB-Serial-JTAG.** The USB-C port goes through
   a CH340K to `U0TXD`/`U0RXD` with the usual DTR/RTS auto-reset transistor
   pair. The native USB pins IO19/IO20 go to bare test points. Almost every
   other ESP-Claw board selects `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`; this one
   must not.

4. **There is no touch controller.** Connector J4 carries ten pins — DC, SDA,
   CS, RST, SCL, GND, VDD, GND, BL_A, BL_K — and no touch interface. On-device
   navigation is the WAKE button plus the web UI.

5. **ES7210 MIC3 is an echo-cancellation reference, not a microphone.** It is
   fed from the amplifier's `OUTP`/`OUTN` through the R31/R32/R35/R36 divider.
   `board_devices.yaml` labels it `RE`, matching esp_box_3. Nothing consumes it
   yet; it is declared so that adding an AFE later is additive.

## Unverified — check these at bring-up

| Item | Current value | How to check |
|---|---|---|
| ES7210 I²C address | `0x82` (7-bit `0x41`) | I²C-scan bus 0. The schematic annotates `0x41` and HeySanta ships `0x82`, but R26/R28 strap AD0/AD1 ambiguously and every ES7210 board in ESP-Claw uses `0x80`. Candidates: `0x80` `0x82` `0x84` `0x86`. |
| `invert_color` | `true` | If the image looks like a photo negative, set `false`. |
| Backlight `output_invert` | `true` | Q2 is a P-FET. If the backlight is on at 0 % and off at 100 %, flip it. |
| Panel offset | none | If the image is shifted, uncomment `esp_lcd_panel_set_gap()` in `setup_device.c`. |
| `pclk_hz` | 80 MHz | Drop to 40 MHz if the panel tears or shows artefacts. |

## Provenance of the display and audio settings

The MP4 ESP32 CORE shares its I2S, I2C, LCD and button pins with the
**HeySanta** board in the xiaozhi-esp32 tree — MCLK 38, BCLK 14, WS 13,
DIN 12, DOUT 45, SDA 1, SCL 2, LCD MOSI 40 / SCK 41 / DC 39 / BL 42,
BOOT 0, WAKE 21 are identical. The orientation, colour-inversion, SPI mode,
clock and backlight-polarity values here are taken from that working
configuration rather than derived from the schematic. The one divergence is
the LCD chip select: HeySanta ties CS low and uses GPIO 9 for a camera data
line, while this board has a real CS on GPIO 9 and no camera.

## Build

```bash
idf.py bmgr -c ./boards -b mp4_esp32_core
idf.py build
idf.py -p PORT flash monitor
```

Rerun `idf.py bmgr` after **every** edit to a YAML file in this directory, or
the stale generated C under `components/gen_bmgr_codes/` is compiled instead.

## Files

| File | Purpose |
|---|---|
| `board_info.yaml` | Board name, chip target, description |
| `board_peripherals.yaml` | I2C, duplex I2S0, display SPI, backlight LEDC |
| `board_devices.yaml` | ST7789, backlight, MAX98357A, ES7210, BMI270, BMM150, buttons |
| `setup_device.c` | ST7789 panel factory entry |
| `sdkconfig.defaults.board` | Flash, PSRAM, codecs, console, robot options |
