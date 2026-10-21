# mp4link: the Raspberry Pi side

`mp4link` is a small Python package for the Raspberry Pi 5 of the Mini Pupper v4. It talks to the
ESP32-S3 over the Pi's UART using the binary frames in [docs/PROTOCOL.md](../../docs/PROTOCOL.md)
(per-servo requests and responses, `SET_ALL` targets, the `STATE` stream, CRC16), and has a
command line for checking the link, reading and writing servo parameters, moving the legs and
calibrating.

It also follows the session behaviour of the `main` branch's firmware, which serves the same
frames in sessions (`main/pi/rl_link.cc` on that branch; the tests include a simulation of it): the
link sends a PING when it opens and whenever it has
been quiet for 0.5 s, so a session starts at once and stays open while a program runs.

## Install on the Pi

Raspberry Pi OS (Bookworm) refuses a system-wide `pip install`, so use a virtual environment:

```
python3 -m venv --system-site-packages .venv
. .venv/bin/activate
pip install -r requirements.txt
```

or the system packages: `sudo apt install python3-numpy python3-serial python3-pytest`.

On another Linux or macOS machine (for the tests), a plain virtual environment without
`--system-site-packages` works the same way. Python 3.10 or newer.

## Enable the UART

The default port is `/dev/ttyAMA0` at 921 600 baud: GPIO14 (TX) and GPIO15 (RX), header pins 8
and 10, the J-PI wiring. On a Pi 5 add this line to `/boot/firmware/config.txt` and reboot:

```
dtparam=uart0=on
```

No serial console may run on that port: `/boot/firmware/cmdline.txt` must not name `ttyAMA0`
(nor `serial0` when `ls -l /dev/serial0` shows it pointing there). The user needs to be in the
`dialout` group. Other wiring: `--port` and `--baud`.

## Run

From `tools/pi`:

```
python3 -m mp4link ping
python3 -m mp4link <command> --help
```

Calibration positions use the main firmware's servo code (0..1023, 511 = centre). The Ini pose is
every servo at 511 + its offset; it is the pose the offsets are calibrated in.

| Command | What it does |
|---|---|
| `ping` | Link check with the servos idle: STATE rate, CRC errors, SET_ALL→STATE latency, IMU, per-servo position, current, temperature |
| `dump` | Read every AT32 parameter, temperature and supply current (read-only; `--out` saves JSON) |
| `config` | Write control mode and gains to the AT32s' RAM and read them back (a power cycle restores flash) |
| `jog` | Move one servo a few degrees (at most 10) and back, report the direction and peak current |
| `stand` | Ramp every servo to a stand pose (`--stand file.json` or `--stand nvs`) and hold it |
| `sine` | Sines on chosen servos about the stand pose |
| `stream` | Play an npz of targets (`delta_dd`, N × 12 deci-degrees about the stand; `period_s`) |
| `offsets` | Read the calibration offsets from the robot's NVS; `--json` writes a stand file |
| `centre` | Hold servos at the centre (code 511, offsets ignored) while the legs are mounted |
| `calib` | Hold the Ini pose (code 511 + offset), adjust the offsets with the keyboard, save them to NVS |
| `idle` | Disarm: every servo IDLE |

`centre` and `calib` refuse to run until `config` has set the gains. Motion commands limit each
step, keep `stand`, `sine` and `stream` targets within a band about the stand pose, refuse to arm
over a latched fault unless given `--clear`, and disarm on exit. With `ping`, `jog`, `stand`,
`sine` and `stream`, `--log file.npz` records the STATE stream and the frames sent.

## Tests

With pytest installed (in the virtual environment, `pip install pytest`; or the `python3-pytest`
package; `requirements.txt` holds only what the tools need at run time):

```
python3 -m pytest tests
```

The tests run against a fake ESP32 on a pseudo-terminal (Linux or macOS); no robot is needed.
