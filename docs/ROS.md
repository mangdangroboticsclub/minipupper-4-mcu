# ros branch: the robot side

The robot is a ROS 2 Jazzy node on Wi-Fi (micro-ROS, XRCE-DDS over UDP to a
micro-ROS agent on a PC). It publishes its IMU, joints, odometry and an LD06
lidar scan, and walks with its own gait on `cmd_vel`. Mapping and navigation
run on the PC. No Raspberry Pi.

**The PC side, step by step: [tools/uros/README.md](../tools/uros/README.md).**
This file is the firmware: what it does, how it is built, and why.

## What is on

| | |
|---|---|
| On | micro-ROS (`CONFIG_APP_UROS_ENABLE`), the LD06 on J-PI (`CONFIG_APP_UROS_LIDAR_LD06`), the robot's own gait, Wi-Fi and the app, sysmon |
| Off | the face and GIFs (`DISPLAY`), the on-robot agent and skills (`AGENT`), Lua (`LUA`), speaker and microphones (`AUDIO`), voice (`VOICE`), the Raspberry Pi link (`PI_LINK`, `RL_LINK`), the RL walk (`POLICY`), the IMU log (`IMU_LOG`) |

All `CONFIG_APP_*` in `main/Kconfig.projbuild`. The choices are at the end of
`sdkconfig.defaults` (`sdkconfig` itself is not in git), so a fresh clone
builds exactly this firmware. The code of the switched-off features is still
in the tree, compiled out: any of them comes back with `idf.py menuconfig` →
*MPX Dog App Configuration* (the Pi link only without the LD06: they share
J-PI). Everything off leaves Wi-Fi, core 0 and RAM to micro-ROS.

## Build and flash

A fresh clone, the first time (ESP-IDF 5.5, Node.js):

```
cd pwa-claw
npm ci
npm run build
cd ..
idf.py reconfigure
idf.py reconfigure
idf.py build flash monitor
```

`npm run build` once, because the build needs the phone app (`pwa-claw/www`)
before it can configure; after that every build rebuilds the app itself.
`reconfigure` twice, because the first run creates `sdkconfig` from
`sdkconfig.defaults` and only the second picks the components that depend on
it (without it: `cap_scheduler.h: No such file or directory`). After that,
just `idf.py build flash monitor`.

The micro-ROS client libraries are prebuilt
(`components/micro_ros/libmicroros.a`), so Windows needs no colcon.

## The node

Node `mpv2`, domain 0 by default.

| Topic | Type | | QoS | Rate |
|---|---|---|---|---|
| `imu/data_raw` | sensor_msgs/Imu | out | best effort | 100 Hz |
| `joint_states` | sensor_msgs/JointState | out | best effort | 50 Hz |
| `odom/raw` | nav_msgs/Odometry | out | best effort | 50 Hz |
| `scan` | sensor_msgs/LaserScan | out | reliable | about 10 Hz |
| `cmd_vel` | geometry_msgs/Twist | in | best effort | 10 Hz or more |
| `uros/ping` → `uros/pong` | std_msgs/Header | in → out | best effort | round-trip test |

- `imu/data_raw`: body frame (x forward, y left, z up), m/s² and rad/s,
  `frame_id: imu_link`, no orientation. Flat on the floor: about +9.8 on z.
- `joint_states`: the twelve `mini_pupper_mp4` joints, measured servo angles.
- Time stamps are the PC's clock: the robot syncs to the agent when it
  connects and every 30 s.
- Best effort cannot be split into pieces, so each best-effort message has to
  fit one UDP packet (MTU 1024); the scan (360 ranges, about 1.5 KB) goes on the
  reliable stream.

Code: `main/ros/` (`mpx_uros.c` settings and status, `uros_node.c` the
session, `uros_robot.cc` driving, `uros_lidar.c` the LD06,
`uros_joint_map.c` servo angles to joints), `main/network/http_uros.cc`.

### Driving: `cmd_vel` and `odom/raw`

The robot's own gait walks it, as the Raspberry Pi bridge used to over J-PI:

- the first `cmd_vel` takes the robot (the app's motion controls are fenced);
- a command older than 0.5 s: it steps in place; zero for 1 s: it stands;
- no `cmd_vel` for 2 s, or the agent gone: the robot goes back to the app;
- `odom/raw` is the velocity the gait executes (the command after clamping),
  integrated: `odom` → `base_link`, planar, reset at each connect. Dead
  reckoning, meant for an EKF together with the IMU's yaw rate (the PC's
  `ekf_uros.yaml`).

The gait walks at about 0.624 of what it is told (ros2jazzympv2's
measurement). The PC's `uros_relay.py` scales `odom/raw` down to real m/s and
Nav2's commands up; see the PC guide to check the number on your robot.

### The LD06 lidar

| LD06 | J-PI | ESP |
|---|---|---|
| P5V | pin 6 | VBUS 5 V |
| GND | pin 1 | GND |
| Tx | pin 3 | IO8 (UART1 RX, 230400 baud) |
| PWM | - | not connected: the LD06 runs at its own 10 Hz |

The robot reads the LD06's packets (CRC checked) on its own task, bins each
turn into 360 one-degree beams and publishes `scan`, frame `lidar_link`,
counter-clockwise from the LD06's zero as ldlidar_stl_ros2 does. NaN: no
point in that degree; +inf: no echo. The URDF's `lidar_joint` (yaw +90°,
`tools/uros/pc/mini_pupper_description/`) is the mount.

The lidar sits on the robot's back and sees its own legs and body: the PC's
`uros_relay.py` publishes `scan_filtered` without them, and mapping and
Nav2 use that.

## The app

**Settings → ROS 2**: the PC's IP (where the agent runs), port 8888, domain
ID, **Save and connect**. The robot keeps them (NVS, namespace `uros`) and
reconnects by itself. The card shows the state and ping, "ROS is driving the
robot" while `cmd_vel` holds it, and the counters, the LD06 among them.

| Route | |
|---|---|
| `GET /v1/uros` | settings, state, counters (`imu_sent`, `cmd_vel`, `cmd_vel_refused`, `odom_sent`, `scan_sent`, `in_control`, `lidar`: Hz, points per turn, CRC errors) |
| `POST /v1/uros` | `{"enabled":true,"agent_ip":"…","agent_port":8888,"domain_id":0,"imu_hz":100,"joint_hz":50}` (any subset) |

`APP_UROS_*` in menuconfig are the defaults before anything is saved.

## Measured

Link test (`tools/uros/uros_check.py`, 60 s, home Wi-Fi): imu about 100 Hz,
joints about 50 Hz, no pings lost, round trip median 8.6 ms, 95% 42–62 ms
(Wi-Fi spikes). Mapping a room with slam_toolbox and navigating it with Nav2
work with the PC guide's settings.

## Rebuilding the micro-ROS library

Only after an ESP-IDF update or a change to `app-colcon.meta` (more topics, a
bigger MTU or buffers): on Linux or WSL2 with ESP-IDF exported,
`tools/uros/rebuild_microros.sh`, then `idf.py fullclean build`.
