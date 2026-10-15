# mpv2-uros — ROS 2 on the ESP32-S3 with micro-ROS

The robot without a Raspberry Pi: the ESP32-S3 is a ROS 2 node itself and
talks to the PC over Wi-Fi. This is mpv2-claw (gait, servos, IMU, screen,
web app, voice) plus micro-ROS.

```
 Robot: ESP32-S3 only                                 PC: Windows + WSL2 (Ubuntu 24.04, ROS 2 Jazzy)
 gait, IK, servos, IMU, screen, web app, voice
 mpx_uros (micro-ROS node "mpv2")   ◀── Wi-Fi, UDP 8888 ──▶  micro-ROS agent ──▶ ROS 2 topics
   imu/data_raw, joint_states, uros/pong                        ros2 topic / RViz / your nodes
```

micro-ROS needs an **agent** on the PC: it is the bridge between the robot's
small XRCE-DDS client and the normal ROS 2 network. Without the agent the
robot runs exactly like mpv2-claw.

## Status: phase 0 (link check)

Phase 0 answers one question before any control is built on it: **is Wi-Fi
fast and steady enough?** The robot publishes, the PC measures. Nothing moves.

| Topic | Type | Direction | QoS | Rate |
|---|---|---|---|---|
| `imu/data_raw` | sensor_msgs/Imu | robot → PC | best effort | 100 Hz (`uros rate`) |
| `joint_states` | sensor_msgs/JointState | robot → PC | best effort | 50 Hz (`uros rate`) |
| `uros/ping` | std_msgs/Header | PC → robot | best effort | test tool |
| `uros/pong` | std_msgs/Header | robot → PC | best effort | echo of each ping |

- `imu/data_raw`: body frame (x forward, y left, z up), m/s² and rad/s,
  `frame_id: imu_link`, no orientation (`orientation_covariance[0] = -1`).
  Flat on the floor it reads about +9.8 on z.
- `joint_states`: the twelve `mini_pupper_mp4` joints, the same names and
  angles the Pi bridge published (a port of `servo_map.py`, checked against
  it). Measured servo angles; a servo that does not answer is reported at its
  commanded angle and counted in `uros status`.
- Time stamps are the PC's clock: the robot syncs to the agent when it
  connects and every 30 s.

The Pi link is switched off in this project (`MP4_PI_LINK_ENABLE`); its code
is still in the tree until phase 1 removes it.

## What is different from mpv2-claw

- `components/micro_ros/`: the micro-ROS libraries (ROS 2 Jazzy), **prebuilt**
  for this project, so `idf.py build` on Windows needs nothing extra.
- `components/mpx_uros/`: the node (settings, console, the session).
- `app-colcon.meta`: the sizes the libraries were built with (1 node, 10
  publishers, 8 subscriptions, UDP MTU 1024).
- `main/main.c` starts the node; `project(mpv2_uros)`, so the image is
  `build/mpv2_uros.bin`.
- `tools/uros/`: the PC-side check and the library rebuild script.

## 1. Build and flash (Windows, as usual)

```powershell
cd C:\esp\projects\mpv2-uros
idf.py build flash monitor
```

NVS sits where mpv2/mpv2-claw keep it, so Wi-Fi and the servo calibration
survive the reflash.

## 2. Set up the PC (once)

### 2a. WSL2 with mirrored networking

The robot must reach the agent inside WSL2. With *mirrored* networking WSL2
shares the PC's own IP address. In `C:\Users\<you>\.wslconfig`:

```ini
[wsl2]
networkingMode=mirrored
```

Then in PowerShell: `wsl --shutdown`, and start Ubuntu again. If you have no
Ubuntu 24.04 yet: `wsl --install -d Ubuntu-24.04`.

### 2b. Let UDP 8888 in (PowerShell **as Administrator**)

```powershell
New-NetFirewallRule -DisplayName "micro-ROS agent" -Direction Inbound -Protocol UDP -LocalPort 8888 -Action Allow
New-NetFirewallHyperVRule -Name microros-agent -DisplayName "micro-ROS agent (WSL)" -Direction Inbound `
    -VMCreatorId '{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}' -Protocol UDP -LocalPorts 8888
```

The first rule is Windows' own firewall, the second the Hyper-V firewall in
front of WSL2.

### 2c. ROS 2 Jazzy in WSL2

The same steps as on the Pi (`ros2jazzympv2` README, *Set up a new SD card*,
step 3), with `ros-jazzy-desktop` instead of `ros-jazzy-ros-base` so RViz is
there later.

### 2d. The micro-ROS agent

```bash
mkdir -p ~/uros_ws/src && cd ~/uros_ws
git clone -b jazzy https://github.com/micro-ROS/micro_ros_setup.git src/micro_ros_setup
sudo apt update && rosdep update
rosdep install --from-paths src --ignore-src -y
colcon build && source install/local_setup.bash
ros2 run micro_ros_setup create_agent_ws.sh
ros2 run micro_ros_setup build_agent.sh
echo "source ~/uros_ws/install/local_setup.bash" >> ~/.bashrc
```

Docker instead, if you use it: `docker run -it --rm --net=host
microros/micro-ros-agent:jazzy udp4 --port 8888`.

## 3. Connect

WSL2 terminal 1, the agent:

```bash
ros2 run micro_ros_agent micro_ros_agent udp4 --port 8888
```

The PC's address: `ipconfig` in PowerShell, the IPv4 address of the Wi-Fi (or
Ethernet) adapter on the robot's network, for example `192.168.1.50`.

On the robot's serial console (`idf.py monitor`):

```
uros agent 192.168.1.50
uros status
```

`uros status` should say `connected` within a few seconds, and the agent
prints `session established`. The setting is saved: after a reboot the robot
reconnects by itself.

## 4. Phase-0 tests

WSL2 terminal 2:

```bash
ros2 topic list                      # /imu/data_raw /joint_states /uros/ping /uros/pong
ros2 topic hz /imu/data_raw          # about 100
ros2 topic echo /joint_states --once
python3 /mnt/c/esp/projects/mpv2-uros/tools/uros/uros_check.py --seconds 60
```

`uros_check.py` prints rate, longest gap and message age for both topics, the
ping round trip and loss, and a verdict line for each:

| Check | Good |
|---|---|
| rate | within 10% of what the robot is set to |
| longest gap | under 200 ms |
| ping round trip | 95% under 30 ms |
| pings lost | under 1% |

Then push it:

1. **Full rate:** `uros rate 200 100` on the robot, then
   `uros_check.py --seconds 60 --imu-hz 200 --joint-hz 100`.
2. **Under load:** walk the robot with the web app joystick while the check
   runs (the gait, the web server and Wi-Fi all busy at once).
3. **Range:** carry the robot to the far side of the room or another room.
4. **Agent restart:** stop the agent (Ctrl-C) and start it again. The robot
   should notice within about 3 s and reconnect by itself (`sessions` in
   `uros status` goes up).

Keep the `uros status` output and the check reports: they decide how
phase 1 sends commands and whether an RL policy can run over Wi-Fi.

## Console: `uros`

```
uros [status]                  state, agent, rates, counters
uros agent <PC IP> [port]      where the agent runs (port 8888 by default); saved
uros domain <0..232>           ROS_DOMAIN_ID (must match the PC's)
uros rate <imu Hz> <joint Hz>  0 = off; imu up to 200, joints up to 100
uros on | off
```

Every change is saved in NVS (namespace `uros`) and the node reconnects with
it. Defaults are in `idf.py menuconfig` → *micro-ROS (ROS 2 on the ESP32-S3)*.

## Troubleshooting

| Symptom | Check |
|---|---|
| `uros status`: *looking for the agent* | agent running? the PC's IP right (`ipconfig`)? both firewall rules (2b)? `.wslconfig` mirrored and `wsl --shutdown` done? the robot and the PC on the same network? |
| *connected*, but `ros2 topic list` shows nothing | `ROS_DOMAIN_ID` in WSL must equal `uros domain` (default 0) |
| `age on arrival: unknown` | the robot's clock is not synced; it retries every 30 s |
| many `late ticks` | something on the robot is starving the node task; note what was running |

## Rebuilding the micro-ROS library

Only after an ESP-IDF update, a big `sdkconfig` change or a change to
`app-colcon.meta`. In WSL2 (or any Linux) with ESP-IDF 5.5 exported:

```bash
tools/uros/rebuild_microros.sh
idf.py fullclean build
```

## RL walk policy on the robot (`policy`)

**The easy guide, with diagrams: [README_RL.md](README_RL.md)** (how it works,
running it from the phone, reading the log, ESP vs Pi). The details follow.

The walk policy runs on the ESP32-S3 itself: no Pi, no Wi-Fi in the loop.
`components/mpx_policy` is omniRL's Pi runner moved into the firmware: the
same observation, network and action decoding (`src/policy_core.c`, checked
against omniRL's numpy by `tools/policy/host_test`: identical servo targets
over 1500 random steps), on a 200 Hz servo tick with a policy step every 4th.

A run, as the Pi runner's:

1. the robot is taken from the gait, the servos get the policy's
   configuration (`mp3link config`: control mode 1, kp 80, kd 400, ...),
   checked by reading it back;
2. gyro bias, 1 s: keep the robot still;
3. ramp to the policy's stand (2 s), hold it (`hold`, default 2 s);
4. the policy, for the given seconds or until `policy stop`;
5. ramp to the stand (1 s), hold 2 s, limp; the gait gets its own servo gains
   back and glides the robot into its stand.

It stops by itself: at once (limp) on a tilt over 35°, a servo over 500 mA for
0.5 s or 60 °C for 1 s, or a leg without fresh servo readings; gracefully
(ramp, hold, limp) on an IMU dropout, a servo at 55 °C, a non-finite action or
two late steps in a row.

```
policy                      status
policy info                 the loaded policy: robot, network, limits, stand, where it sits in RAM
policy test                 self-test against training's own outputs, and the time per step
policy imu [seconds]        the IMU as the policy sees it (mounting check)
policy run 10 0 0 0 dry     a DRY run: no servo touched (see below)
policy obs                  the last step: observation, action, servo targets
policy run 10 0.05 0 0      10 s at vx 0.05 m/s (vy, wz 0), after a 2 s hold
policy run 0 0.05 hold 5    until `policy stop`, after a 5 s hold
policy cmd 0.08 0 0.3       change the command while it runs
policy stop
policy offsets robot        use this robot's calibration instead of the policy contract's
                            (the default for a policy trained for this robot, e.g. sku1 on sku1)
policy list / load <name>   policies in /fatfs/policies (tools/policy/README.md)
```

**The built-in policy is omniRL's `bam_t5ub06_stops_v1`, trained for SKU2.**
It is there to test the runtime (`policy test`, and a run on a stand with
`force`). On an SKU1 robot run an SKU1 policy: train it in omniRL on the SKU1
model, export it with `tools/policy/export_mpxp.py --robot sku1`.

### From the phone: Settings → RL walk

The web app runs the same thing without the USB cable: **Settings → Robot →
RL walk** (shown when the firmware has a policy). It shows the loaded policy
and its state, and starts a run: **Walk forward** (one button, slow /
normal / fast) or **Omni** (drag the stick forward/back, left/right, and the
Turn slider; letting go is a stop in place); 10 s, 30 s, 2 min or until Stop;
dry run; **Use my calibration** (the robot's own offsets, or the ones saved
in the policy); a policy for another robot only with its switch on. The red
Stop ramps it to the stand.

**Record a log** keeps every 20 ms step of the run (step period, tick and
network time, servo exchange, how far the servos are from their targets,
tilt, temperature, current, command; up to 2 min). When the run ends the page
shows a summary to copy and the full CSV to download
(`GET /v1/policy/log`; on the console `policy log on` / `policy log print`).

A run started from the app has a dead-man: the page sends the stick every
250 ms, and when nothing arrives for 2 s (phone locked, page closed, Wi-Fi
gone) the robot ramps to its stand and stops by itself. Runs started from the
console have none (`policy stop` ends them). The API is `GET/POST /v1/policy`
(`components/http_server/http_server_mpx_robot_api.c`).

### Without servos: IMU and dry run

With only the ESP32-S3 board (no servo power), everything but the servos
can be checked:

1. **`policy test`**: `ok`, and the time per step. It is timed as a run
   sees it (core 1, the policy task's priority), and how much of the
   weights (about 130 KB) sit in internal RAM. Layers are copied there,
   biggest first, while at least `MP4_POLICY_INTERNAL_RESERVE_KB` (menuconfig,
   default 96) of internal RAM stays free; the rest is read from PSRAM, which
   makes a step several times slower (all in PSRAM: about 4 ms).
2. **`policy imu 20`**: four lines a second, in the policy's body frame,
   with the pose in words at the end. Hold the board **the way it sits in
   the robot** (the robot standing): gravity about `0 0 -1`, tilt near 0,
   `(upright)`. Then:

   | Move the board | Should read |
   |---|---|
   | nose (front) down | gravity x **positive** |
   | left side down | gravity y **positive** |
   | turn it left (counter-clockwise from above) | gyro z **positive** |

   A sign the wrong way round means the policy's IMU mounting does not
   match this board: say so before any real run.
3. **`policy run 10 0.05 0 0 dry`**: the whole loop at 50 Hz on the real IMU
   (gyro bias, observation, network, action decoding, timing) with the joints
   taken to be at the stand. No servo is touched, no `force` needed. Then
   `policy`: about 500 steps, **0 late**, `network <=` the slowest step.
   `policy obs` shows the last step. Tilting the board past 35° logs
   "a real run would disarm here" and goes on.

### First run with servos

Robot on a stand, feet off the ground, `policy test` first, then
`policy run 5 0 0 0 force` (stand still), then a slow `0.05`.

## Next

- **Phase 1:** the robot takes commands: `cmd_vel` (walk), body pose, moves,
  `odom/raw`, `battery_state`; ROS control hands back to the app when commands
  stop (about 0.5 s); the Pi link code removed.
- **Phase 2:** a launch package for the PC (agent, `robot_state_publisher`,
  EKF, joystick, RViz).
- **RL:** the policy on the PC with IMU and servo feedback from these topics
  and joint targets back. Whether 50 Hz holds over Wi-Fi is exactly what the
  phase-0 numbers show.
