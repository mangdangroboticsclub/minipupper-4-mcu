# Raspberry Pi (ROS 2) ↔ MP4 ESP32 CORE link

The robot talks to a Raspberry Pi over the J-PI header using **mpv2's Pi link**:
the same wire protocol, pins, baud rate and behaviour as the mpv2 firmware. The
ROS 2 bridge that drives an mpv2 robot (SLAM, navigation, tracking, dance,
music…) drives this firmware unchanged.

- Firmware side: `components/mpx_pi_link` (protocol in `src/pi_protocol.h`,
  identical to mpv2's `main/pi/pi_protocol.h`).
- Pi side: `ros2jazzympv2/mini_pupper_esp_bridge` (its `protocol.py` mirrors
  `pi_protocol.h`, protocol version 1).

`mp4_link.py` in this folder is the **old** text-line test link (PING/HELLO at
115200). The firmware no longer speaks it; use the ROS bridge or its
`esp_link_test` instead.

## 1. Wires: six of them

| J-PI pin | Net | Wire | ESP role | Raspberry Pi 40-pin header |
|---|---|---|---|---|
| 1 | GND | black | ground | GND, pin 6 (or 9, 14, 20…) |
| 2 | IO18_PI_1 | blue | UART1 TX (ESP → Pi) | GPIO15 RXD, **pin 10** |
| 3 | IO8_PI_2 | green | UART1 RX (Pi → ESP) | GPIO14 TXD, **pin 8** |
| 4 | IO3_PI_3 | yellow | HOST_PRESENT input | GPIO17, **pin 11** (the Pi drives it) |
| 5 | IO11_PI_4 | white | ESP_SLAVE output | GPIO27, **pin 13** (the Pi reads it) |
| 6 | VBUS 5 V | red | 5 V | pin 2 / 4 (only if the Pi is powered from the robot) |

Both boards use 3.3 V logic: no level shifter. If the link stays silent,
swapping the two data wires is the first thing to try.

## 2. Prepare the Pi

```
sudo raspi-config
    Interface Options -> Serial Port
        login shell over serial?   No
        serial port hardware?      Yes
sudo usermod -aG dialout,gpio $USER
sudo reboot
ls -l /dev/ttyAMA0      # Pi 5: GPIO14/15 UART
```

## 3. Run the bridge

`mini_pupper_esp_bridge/config/esp_bridge.yaml` must match the firmware:

| Bridge parameter | Firmware (menuconfig → App Config → MP4 Robot) | Default |
|---|---|---|
| `baud` | `MP4_PI_LINK_UART_BAUD` | 921600 |
| `heartbeat_hz` | must beat `MP4_PI_LINK_TIMEOUT_MS` | 20 Hz vs 300 ms |
| `state_rate_hz` | `MP4_PI_LINK_STATE_HZ` (used when the host asks for none) | 50 |
| `host_present_gpio` | IO3 must be HIGH for control (`MP4_PI_LINK_REQUIRE_HOST_PIN`) | 17 |
| `take_control: false` | the robot stays on the app; telemetry still flows ("monitor") | true |

Then `ros2 launch mini_pupper_esp_bridge esp_bridge.launch.py`, or the bringup
launch you use with mpv2.

## 4. Check it on the robot

Serial console:

```
pi            # state (standalone / monitor / slave), IO3 level, heartbeat,
              # frame counters, the Pi's IP and hostname, music and camera stats
pi json       # the same as GET /v1/pi/status
```

Or open `http://<robot-ip>/v1/pi/status` in a browser.

| `pi` shows | Meaning |
|---|---|
| `standalone`, rx 0 frames | nothing heard: bridge not running, wrong baud, or TX/RX swapped |
| mostly CRC errors | the two sides use different baud rates |
| `monitor`, wants control, IO3 low | the yellow wire / GPIO17 is not set high |
| `slave` | the Pi drives the robot |

## 5. What happens in slave mode

The robot is the Pi's only while **both** IO3 is high **and** heartbeats asking
for control keep arriving (within 300 ms). Lose either one, for example when
the bridge is stopped, the Pi reboots or a cable falls out, and the robot
stops, stands and goes back to normal.

While the Pi is in control:

- "ROS" appears in front of the address under the eyes;
- voice (wake word) and music are off; hands-free listening comes back afterwards;
- the app's joystick and move buttons, the agent's robot tools and WASM skills
  are refused with "the Raspberry Pi is in control";
- the Pi can play sound through the speaker (`AUDIO_*`) and show its camera
  or tracking picture on the screen (`VIDEO_DATA`); the face comes back 1.5 s
  after the last picture;
- the Pi's IP is shown under the eyes (HOST_INFO) in monitor and slave mode.

Telemetry to the Pi:
- STATE (servo angles commanded and measured, IMU, executed velocity) at 50 Hz;
- INFO at 1 Hz (firmware "mpv2-claw", robot UUID);
- DIAG at 5 Hz (servo temperatures and currents, link counters);
- EVENTs (link up/down, rejected commands, protocol mismatch).

## 6. Show the Pi's IP from boot (`pi_announce`)

Without the bridge running, the robot doesn't know the Pi's IP. `pi_announce`
is a small service that sends the Pi's IP (a HOST_INFO frame) every 2 s and
nothing else, so the screen shows `<robot IP> | Pi <pi IP>` as soon as the Pi
has booted. No heartbeat, no reading, no HOST_PRESENT line: it cannot change
who controls the robot, even while the bridge runs. (The first version sent
"monitor" heartbeats; with a bridge it could not see, the robot flipped in and
out of ROS control.) Needs the mpv2-claw or mpv2 firmware that shows the IP
for HOST_INFO alone.

Install on the Pi (copy the two files over first, e.g. with `scp`):

```
sudo cp pi_announce.py /usr/local/bin/
sudo cp pi-announce.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now pi-announce
journalctl -u pi-announce -f      # check it
```
