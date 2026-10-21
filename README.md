# mp4link · `mp4link-sku2` branch: the ESP32-S3 as a servo link for a Raspberry Pi

A controller on the robot's Raspberry Pi 5 (for example a walking policy trained in simulation) drives
the twelve servos directly: the Pi sends twelve targets, and the ESP32-S3 answers every 5 ms with the
positions, currents, temperatures and the IMU. The ESP32 runs nothing else. The branch is for
SKU2: the Mini Pupper v4 with a Raspberry Pi 5.

## Related branches

| Branch | On the robot |
|---|---|
| `main` | the full firmware: gait, app, face, voice, agent, Raspberry Pi link |
| `rl` | the RL walk on the ESP32-S3, nothing else |
| `micro-ros` | micro-ROS and an LD06 lidar |
| **`mp4link-sku2`** (this one) | the servo link for a controller on the Raspberry Pi (SKU2) |

The wiring is the same as `main`'s, so switching branches only needs a reflash. The frames are also the
ones `main` serves in its Pi link sessions (`main/pi/rl_link.cc`); the Pi tools here follow that session
behaviour too (their tests include a simulation of it).

## Wiring

The J-PI connector to the Pi 5 header (`main/board_config.h`, the `main` branch's board file, copied
unchanged: its comments refer to that branch's sources):

| J-PI pin | ESP32 | Pi 5 header |
|---|---|---|
| 1 | GND | pin 6 or 9 (GND) |
| 2 | IO18, UART1 TX | pin 10, GPIO15 RXD |
| 3 | IO8, UART1 RX | pin 8, GPIO14 TXD |

On the Pi this is `/dev/ttyAMA0` at 921 600 baud: add `dtparam=uart0=on` to
`/boot/firmware/config.txt`, and keep the serial console off that port (see
[tools/pi/README.md](tools/pi/README.md)).

## Quick start

1. **Build and flash** with ESP-IDF 5.5 installed (as in `.devcontainer/`):
   ```
   idf.py build
   idf.py -p PORT flash
   ```
   or with Docker, from this directory (here the ESP32 is on `/dev/ttyUSB0`):
   ```
   docker run --rm -u $(id -u):$(id -g) -e HOME=/tmp -v "$PWD":/project -w /project espressif/idf:v5.5.4 idf.py build
   docker run --rm -u $(id -u):$(id -g) -e HOME=/tmp --group-add $(stat -c %g /dev/ttyUSB0) \
       --device=/dev/ttyUSB0 -v "$PWD":/project -w /project espressif/idf:v5.5.4 idf.py -p /dev/ttyUSB0 flash
   ```
   The first build downloads the `espressif/bmi270` component, so it needs network access.
   Flashing writes the bootloader, the partition table and the app; the NVS partition, which holds
   the calibration offsets, is not touched.
2. **Check the link** on the Pi, from `tools/pi`, after installing the tools and enabling the UART
   ([tools/pi/README.md](tools/pi/README.md)):
   ```
   python3 -m mp4link ping
   ```
   Expect `STATE` at about 200 Hz, 0 CRC errors, `IMU ok` and fresh feedback from all four boards.
3. **Configure the servos** after every power-up (RAM only): `python3 -m mp4link config`.

The console (UART0, the USB port) prints the boot log at 115 200 baud.

## Behaviour

- Every servo is IDLE at boot and whenever the link is disarmed.
- Armed, each 5 ms tick sends the latest `SET_ALL` targets to the four servo boards.
- **Link watchdog:** no `SET_ALL` for 100 ms → every servo IDLE.
- **Guards:** a driven servo above 1000 mA for 0.5 s, or a housing above 60 °C for 1 s, or a silent servo
  board → every servo IDLE, latched until the host clears it.
- **Task watchdog:** if the tick stalls for 500 ms, the ESP32 reboots and idles every board.
- **Calibration offsets** stay in NVS (namespace `parameter`, keys `offset1`..`offset12`, degrees). The ESP32
  stores and serves them; the host applies them. The `main` branch's firmware keeps its own copy (namespace
  `robot`), takes `parameter`'s only while its own offsets are all zero, and writes both when it saves: after
  switching to `main`, check the stand pose and recalibrate there if it is off.

The limits are build options (`idf.py menuconfig` → Pi link). To use the protocol on the USB port as
well (for bench work from a PC), first set Component config → ESP System Settings → Channel for
console output → No console output, then enable `APP_RL_USB_LINK` under Pi link.

## Docs

| | |
|---|---|
| [docs/PROTOCOL.md](docs/PROTOCOL.md) | the frames, commands, flags and guards |
| [tools/pi/README.md](tools/pi/README.md) | the Pi tools: link check, servo configuration, calibration, motion tests |

The servo board driver and the calibration layout follow the `main` branch's firmware.

Apache License 2.0 ([LICENSE](LICENSE)).
