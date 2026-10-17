# mpx-dog · `rl` branch: the RL walk on the ESP32-S3

A walk policy trained in simulation runs on the robot's own ESP32-S3 and
drives its twelve servos at 50 Hz. No Raspberry Pi, no PC during the run: the
phone app starts it, steers it and reads its logs.

## Branches

| Branch | On the robot |
|---|---|
| `main` | the full firmware: gait, app, face, voice, agent, Raspberry Pi link |
| **`rl`** (this one) | the RL walk on the ESP32-S3, nothing else |
| `ros` | micro-ROS and an LD06 lidar; mapping and navigation on a PC |
| RL + ROS (planned) | the RL walk, steered by Nav2 |

The branches share the code; what runs is chosen in `sdkconfig.defaults`.

## Quick start

1. **Flash the robot** (ESP-IDF 5.5, Node.js):
   ```
   cd pwa-claw && npm ci && npm run build && cd ..
   idf.py reconfigure
   idf.py reconfigure
   idf.py build flash
   ```
   (the first time on a fresh clone; why: the build section of [docs/RL.md](docs/RL.md#build-and-flash).
   Afterwards `idf.py build flash`.)
2. **Open the app** (the robot's hotspot or your Wi-Fi) → **Settings → RL
   walk**. Run the **Self-test** once.
3. **Walk:** robot on a stand first, feet off the ground. **Walk forward**
   (a fixed speed) or **Omni (stick)**, a time, **Start**; **Stop** ends it.
   The robot also stops by itself on a big tilt, or if the app goes quiet for
   2 s.
4. **Read the run:** the page shows the log after each run; **Without Wi-Fi**
   runs with the radio off and shows the log when it comes back.

## Docs

| | |
|---|---|
| [docs/RL.md](docs/RL.md) | the RL walk: what is on, the policies, a run, the loop and the weights, the size test, running without Wi-Fi, routes |
| [tools/policy/README.md](tools/policy/README.md) | put a new policy in (export from omniRL) and check its arithmetic on a PC |
| [docs/NEW_BOARD_BRINGUP.md](docs/NEW_BOARD_BRINGUP.md) | a new board, from scratch |

Training happens outside this repo (omniRL); this branch runs the result.

Apache License 2.0 ([LICENSE](LICENSE)).
