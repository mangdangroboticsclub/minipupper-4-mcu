# micro-ROS on the PC: setup and use

The robot (the `ros` branch firmware) is a ROS 2 node by itself: no Raspberry Pi.
It talks to a micro-ROS agent on the PC over Wi-Fi. Everything heavy (SLAM,
Nav2, RViz) runs on the PC.

```
 robot (ESP32-S3)                          PC (Windows + WSL2, ROS 2 Jazzy)
 ─────────────────                         ────────────────────────────────
 imu/data_raw  100 Hz ─┐                   micro_ros_agent (UDP 8888)
 joint_states   50 Hz  │   Wi-Fi, UDP        │
 odom/raw       50 Hz  ├──────────────────►  ├─ uros_relay.py   pc/imu, pc/odom, pc/joint_states,
 scan (LD06)    10 Hz ─┘                     │                  scan_filtered, speed scaling
 cmd_vel ◄───────────────────────────────────┤─ ekf (robot_localization)  odom -> base_link
   (the robot's own gait walks it)           ├─ robot_state_publisher     the SKU1 URDF
                                             ├─ slam_toolbox  (mapping)   map -> odom
                                             ├─ Nav2 + AMCL   (navigation)
                                             └─ RViz
```

This file is the PC side, step by step. Everything the PC needs is in this
folder: the robot model and the slam/Nav2 settings come with it, nothing else
has to be cloned or built apart from the micro-ROS agent. What the firmware does and why is in
[docs/ROS.md](../../docs/ROS.md).

| File | What it is |
|---|---|
| `uros_check.py` | link test: topic rates, gaps, ping round trip |
| `teleop_keys.py` | drive the robot from the keyboard (`cmd_vel`) |
| `pc/uros_relay.py` | best effort -> reliable topics, gyro bias, speed scale, body filter for the lidar |
| `pc/ekf_uros.yaml` | EKF settings (odometry + IMU yaw rate) |
| `pc/uros_slam.launch.py` | make a map |
| `pc/uros_nav.launch.py` | navigate on a saved map (or `slam:=True`: map and navigate at once) |
| `pc/uros_slam.rviz` | RViz view for mapping |
| `pc/mini_pupper_description/` | the SKU1 robot model (URDF + meshes), from ros2jazzympv2 |
| `pc/config/` | Nav2 (`mp4_room.yaml`), slam_toolbox and Nav2 RViz settings, from ros2jazzympv2 |
| `rebuild_microros.sh` | rebuild the robot's micro-ROS library (rarely needed) |

---

## 1. Once: the PC

Windows 11, WSL2 with Ubuntu 24.04. All commands below are in a WSL terminal
unless they say PowerShell.

### 1.1 WSL2 networking

The robot has to reach WSL directly. In Windows, create or edit
`C:\Users\<you>\.wslconfig`:

```ini
[wsl2]
networkingMode=mirrored
```

Then in PowerShell: `wsl --shutdown`, and open WSL again. WSL now has the same
IP as Windows.

### 1.2 Firewall: let UDP 8888 in

PowerShell **as administrator**, once:

```powershell
New-NetFirewallRule -DisplayName "micro-ROS agent UDP 8888" -Direction Inbound -Protocol UDP -LocalPort 8888 -Action Allow
New-NetFirewallHyperVRule -Name "micro-ROS-8888" -DisplayName "micro-ROS agent UDP 8888 (WSL)" -Direction Inbound -VMCreatorId '{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}' -Protocol UDP -LocalPorts 8888
```

The first rule is Windows' own firewall, the second is the Hyper-V firewall in
front of WSL. Both are needed with mirrored networking.

### 1.3 ROS 2 Jazzy

Install `ros-jazzy-desktop` as in the official guide
(docs.ros.org → Jazzy → Installation → Ubuntu deb packages), plus:

```bash
sudo apt install python3-rosdep python3-colcon-common-extensions
sudo rosdep init    # only if it was never done
rosdep update
```

### 1.4 The micro-ROS agent

```bash
mkdir -p ~/uros_ws/src && cd ~/uros_ws
git clone -b jazzy https://github.com/micro-ROS/micro_ros_setup.git src/micro_ros_setup
source /opt/ros/jazzy/setup.bash
rosdep install --from-paths src --ignore-src -y
colcon build && source install/local_setup.bash
ros2 run micro_ros_setup create_agent_ws.sh
ros2 run micro_ros_setup build_agent.sh
```

### 1.5 Packages for mapping and navigation

```bash
sudo apt install ros-jazzy-robot-state-publisher ros-jazzy-xacro \
  ros-jazzy-robot-localization ros-jazzy-slam-toolbox ros-jazzy-rviz2 \
  ros-jazzy-nav2-map-server ros-jazzy-navigation2 ros-jazzy-nav2-bringup
```

### 1.6 Shortcuts (optional)

Add to `~/.bashrc`, so every new terminal is ready:

```bash
source /opt/ros/jazzy/setup.bash
export UROS=/mnt/c/esp/projects/mpv2onlyROS/tools/uros    # this folder, as WSL sees it
alias uros-agent='source ~/uros_ws/install/local_setup.bash && ros2 run micro_ros_agent micro_ros_agent udp4 --port 8888'
```

The commands below use `$UROS` and `uros-agent`. Without the shortcuts, write
the full paths and the agent command out.

---

## 2. Once: the robot

1. **Flash this branch** (ESP-IDF 5.5, from the project folder):
   ```
   cd pwa-claw && npm ci && npm run build && cd ..
   idf.py reconfigure
   idf.py reconfigure
   idf.py build flash
   ```
   (the first time on a fresh clone, see [docs/ROS.md](../../docs/ROS.md#build-and-flash);
   afterwards `idf.py build flash`).
   `sdkconfig.defaults` holds the micro-ROS-only choices (display, agent,
   Lua, audio, voice, Pi link and the IMU log off; micro-ROS and the LD06 on),
   so a fresh clone builds the same firmware.
2. **Wire the LD06** to J-PI (pin numbers in [docs/ROS.md](../../docs/ROS.md)):
   P5V → pin 6 (5 V), GND → pin 1, Tx → pin 3 (IO8). Leave PWM unconnected.
3. **Point the robot at the PC.** Find the PC's IP: `ipconfig` in Windows
   (the Wi-Fi adapter's IPv4), or `hostname -I` in WSL (the same IP with
   mirrored networking). In the robot's app: **Settings → ROS 2**,
   enter that IP, port 8888, domain 0, **Save and connect**. The robot keeps
   it and reconnects by itself after a reboot.

The IP can change when the router gives the PC a new one. Then enter the new
one in the app, or reserve a fixed IP for the PC in the router.

**Domain ID:** the number in the app's ROS 2 card must match `ROS_DOMAIN_ID`
in WSL, or the PC sees no topics. Both are 0 unless you changed them
(`echo $ROS_DOMAIN_ID` in WSL; empty means 0).

---

## 3. Every time

Each tab is a WSL terminal. Start tab 1 first; the robot's ROS 2 card turns
green within a few seconds.

| | Tab 1 | Tab 2 | Tab 3 |
|---|---|---|---|
| **Test the link** | `uros-agent` | `python3 $UROS/uros_check.py --seconds 60` | |
| **Drive** | `uros-agent` | `python3 $UROS/teleop_keys.py` | |
| **Make a map** | `uros-agent` | `ros2 launch $UROS/pc/uros_slam.launch.py` | `python3 $UROS/teleop_keys.py`, then save |
| **Navigate** | `uros-agent` | `ros2 launch $UROS/pc/uros_nav.launch.py map:=/mnt/c/esp/maps/room.yaml` | |

Keep the robot **still for the first 2 s** after starting tab 2: `uros_relay.py`
measures the gyro bias then.

### Test the link

`uros_check.py` prints each topic's rate, the longest gap and the ping round
trip. On a good Wi-Fi: imu about 100 Hz, joints about 50 Hz, no pings lost,
median round trip under 15 ms.

### Drive

`w`/`s` forward/back, `a`/`d` sideways, `q`/`e` turn, space stop, `x` quit.
The first command takes the robot; with no command for 2 s it goes back to
the app. Maximum speeds: `--max-vx 0.15 --max-vy 0.10 --max-wz 1.0`.

### Make a map

Drive slowly (about 0.05 m/s, turns about 0.4 rad/s) and go back past places
you have seen, so slam_toolbox closes the loops. When the map in RViz looks
right, in tab 3 (stop teleop with `x` first):

```bash
mkdir -p /mnt/c/esp/maps
ros2 run nav2_map_server map_saver_cli -f /mnt/c/esp/maps/room
```

This writes `room.pgm` and `room.yaml`. Then stop tab 2 (Ctrl+C).

### Navigate

1. Start tab 2 with the robot standing still.
2. In RViz, **2D Pose Estimate**: click where the robot really is on the map
   and drag towards where it faces. The green particles gather round the
   robot and the red scan points fall on the black walls. If they don't,
   do it again.
3. **Nav2 Goal**: click a free spot and drag to set the direction. Start with
   goals 0.5–1 m away.

`slam:=True` instead of `map:=...`: no saved map, it maps and navigates at
once.

---

## 4. Topics

| Topic | Type | From | Notes |
|---|---|---|---|
| `imu/data_raw` | sensor_msgs/Imu | robot | 100 Hz, best effort, body frame |
| `joint_states` | sensor_msgs/JointState | robot | 50 Hz, best effort |
| `odom/raw` | nav_msgs/Odometry | robot | 50 Hz, the gait's commanded velocity, integrated |
| `scan` | sensor_msgs/LaserScan | robot | LD06, about 10 Hz, 360 beams, frame `lidar_link` |
| `cmd_vel` | geometry_msgs/Twist | PC | to the robot, 10 Hz or more |
| `uros/ping` → `uros/pong` | std_msgs/Header | PC → robot → PC | round trip test (`uros_check.py`) |
| `pc/imu`, `pc/odom`, `pc/joint_states` | | relay | reliable copies; gyro bias removed, odometry in real m/s |
| `scan_filtered` | sensor_msgs/LaserScan | relay | `scan` without the robot's own legs and body |
| `cmd_vel_navigation2` | geometry_msgs/Twist | Nav2 | the relay scales it up into `cmd_vel` |
| `odom` | nav_msgs/Odometry | EKF | odometry + IMU, with the `odom -> base_link` TF |

The robot's own counters: `http://<robot-ip>/v1/uros` in a browser.

## 5. Settings

`uros_nav.launch.py`:

| Argument | Default | |
|---|---|---|
| `map` | `/mnt/c/esp/maps/room.yaml` | saved map |
| `slam` | `False` | `True`: map as you go, no saved map |
| `linear_scale` | `0.624` | real speed ÷ commanded speed of the gait |
| `params` | `pc/config/mp4_room.yaml` | Nav2 settings |
| `slam_params` | `pc/config/slam_toolbox_room.yaml` | slam_toolbox settings |
| `urdf` | `pc/mini_pupper_description/` mini_pupper_mp4 | robot model |
| `rviz` | `true` | `false`: no RViz |

`uros_slam.launch.py` takes `urdf`, `slam_params` and `rviz`.

Both launch files change the scan topic to `/scan_filtered` in a copy of the
settings in `/tmp` (`uros_nav2_params.yaml`, `uros_slam_params.yaml`); the
files in `pc/config/` themselves are never changed.

**Checking `linear_scale`:** drive straight about 1 m with `teleop_keys.py`,
measure the real distance with a tape, and compare it with how far `/odom`
moved (`ros2 topic echo /odom --field pose.pose.position`). New value =
0.624 × real ÷ odom.

---

## 6. Troubleshooting

| What you see | Try |
|---|---|
| The ROS 2 card stays red, the agent prints nothing | Wrong IP in the app (check `ipconfig`); the two firewall rules (1.2); `networkingMode=mirrored` and `wsl --shutdown`; turn off a VPN on Windows; robot and PC on the same Wi-Fi (not a guest network). |
| Agent says "session established" but `ros2 topic list` shows nothing | `ROS_DOMAIN_ID` differs between WSL and the app. |
| Rates low or jumpy in `uros_check.py` | Move closer to the router; 2.4 GHz busy; a VPN on Windows. Short spikes of 40–60 ms are normal Wi-Fi. |
| `lidar` shows 0 Hz at `/v1/uros` | Wiring: LD06 Tx must go to J-PI pin 3, and it needs 5 V. |
| The scan is turned the wrong way in RViz | Put a box 30 cm in front of the robot: it must show in front (+x, the red axis). If not, change the yaw of `lidar_joint` in `pc/mini_pupper_description/urdf/mini_pupper_mp4/mini_pupper_description.urdf.xacro`. |
| No robot model in RViz | `pc/mini_pupper_description/meshes/` missing (copy the whole `tools/uros` folder); RobotModel display's Description Topic must be `/robot_description`. |
| Nav2: "no map at ..." | Wrong `map:=` path; save the map first. |
| Goal "aborted", a red square around the robot | Use `/scan_filtered` (the launch files do) and set the 2D Pose Estimate. |
| The robot overshoots or stops short of goals | Check `linear_scale` (section 5). |
| TF warnings about time going back, the map jumping | The WSL clock is stepping: `timedatectl` should say "synchronized: yes"; if not, `sudo systemctl enable --now systemd-timesyncd`, or `wsl --shutdown` and start again. |
| The robot ignores `cmd_vel` | Something else holds the robot (a move from the app): stop it first (`cmd_vel_refused` counts up at `/v1/uros`). |
