# mpx-dog · `ros` branch: ROS 2 without a Raspberry Pi

The robot's ESP32-S3 is a ROS 2 node by itself (micro-ROS over Wi-Fi). It
sends its IMU, joints, odometry and an LD06 lidar scan to a PC and walks on
`cmd_vel`. The PC maps the room (slam_toolbox) and navigates it (Nav2).

```
 robot (ESP32-S3 + LD06)                      PC (Windows + WSL2, ROS 2 Jazzy)
 imu, joints, odom, scan  ── Wi-Fi, UDP ──▶  micro-ROS agent ─▶ SLAM, Nav2, RViz
 its own gait             ◀── cmd_vel ─────
```

## Branches

| Branch | On the robot |
|---|---|
| `main` | the full firmware: gait, app, face, voice, agent, Raspberry Pi link |
| `rl` | the RL walk (a trained policy) on the ESP32-S3, nothing else |
| **`ros`** (this one) | micro-ROS and the LD06; mapping and navigation on a PC |
| RL + ROS (planned) | the RL walk, steered by Nav2 |

The branches share the code; what runs is chosen in `sdkconfig.defaults`.

## What you need

- The robot (MP4-ESP32-CORE, SKU1 body) and an LDROBOT LD06 lidar, wired to
  J-PI: 5 V → pin 6, GND → pin 1, Tx → pin 3.
- A PC with Windows 11 + WSL2 (Ubuntu 24.04) or Ubuntu 24.04, ROS 2 Jazzy, on
  the same Wi-Fi as the robot.
- ESP-IDF 5.5 and Node.js to build the firmware.

## Quick start

1. **Flash the robot:**
   ```
   cd pwa-claw && npm ci && npm run build && cd ..
   idf.py reconfigure
   idf.py reconfigure
   idf.py build flash
   ```
   (the first time on a fresh clone; why: the build section of [docs/ROS.md](docs/ROS.md#build-and-flash).
   Afterwards `idf.py build flash`.)
2. **Set up the PC** once: [tools/uros/README.md](tools/uros/README.md)
   (WSL networking, firewall, ROS 2, the micro-ROS agent, the packages).
3. **Connect:** start the agent on the PC; in the robot's app,
   **Settings → ROS 2**, enter the PC's IP, **Save and connect**.
4. **Drive, map, navigate:** the tabs to open for each are in
   [tools/uros/README.md](tools/uros/README.md#3-every-time).

## Docs

| | |
|---|---|
| [tools/uros/README.md](tools/uros/README.md) | the PC: setup, every-day use, topics, settings, troubleshooting |
| [docs/ROS.md](docs/ROS.md) | the robot: what the firmware publishes and does, the LD06, the app, routes |
| [docs/NEW_BOARD_BRINGUP.md](docs/NEW_BOARD_BRINGUP.md) | a new board, from scratch |

## What is in `tools/uros`

| | |
|---|---|
| `uros_check.py` | link test: rates, gaps, round trip |
| `teleop_keys.py` | drive from the keyboard |
| `pc/uros_slam.launch.py` | make a map |
| `pc/uros_nav.launch.py` | navigate on a saved map |
| `pc/uros_relay.py`, `pc/ekf_uros.yaml` | used by both launch files |
| `pc/mini_pupper_description/`, `pc/config/` | the robot model and the slam_toolbox, Nav2 and RViz settings (from ros2jazzympv2) |

Apache License 2.0 ([LICENSE](LICENSE)).
