# RL walking on the ESP32-S3: how it works and how to run it

This guide covers the walking policy (reinforcement learning) that runs on the
robot's own ESP32-S3 in **mpv2-uros**: what it is, how to run it from your
phone, how to read its log, and how it differs from running the same policy on
a Raspberry Pi.

## The short version

- The policy is a small neural network that was **trained on the PC** (omniRL,
  in simulation) and **runs on the ESP32-S3**, 50 times a second.
- **It does not use micro-ROS, Wi-Fi or a Pi** while it walks. Everything in
  the loop is on the robot.
- Training (learning) happens only on the PC. The ESP, like a Pi, **only runs**
  the finished network: inputs in, servo targets out.
- You start it from the phone (**Settings → RL walk**) or the USB console
  (`policy run …`).

## 1. Does it go through micro-ROS? No.

The robot has three separate things, and only one of them walks the robot:

```
 Phone (web app) ──Wi-Fi──┐
                          ▼
 ┌──────────────────── ESP32-S3, on the robot ─────────────────────┐
 │                                                                 │
 │ web server ── start / stop / stick ──►  RL POLICY ──► 12 servos │
 │                                            ▲                    │
 │                               IMU + servo positions             │
 │                                                                 │
 │ micro-ROS node ── imu/data_raw, joint_states ──► Wi-Fi ──► PC   │
 │                    (optional: ROS 2 tools only watch)           │
 │                                                                 │
 │ Pi link (J-PI UART): not used in mpv2-uros                      │
 │                                                                 │
 └─────────────────────────────────────────────────────────────────┘
```

| Part | What it does | Needed to walk with RL? |
|---|---|---|
| **RL policy** (`components/mpx_policy`) | reads the IMU and servos, runs the network, drives the 12 servos | **yes**: this is the walking |
| **Web app** (Settings → RL walk) | start, stop, steer, read the log | only to start it (or use the console) |
| **micro-ROS** (`components/mpx_uros`) | sends `imu/data_raw` and `joint_states` to ROS 2 on your PC | no: optional, for watching in ROS |
| **Pi link** | the old way: a Pi runs the policy over the UART | no: switched off in mpv2-uros |

The phone only sends "start", "stop" and the stick position (4 times a
second). The 50-times-a-second control loop never leaves the ESP. micro-ROS
only watches (it sends the IMU and joint angles to ROS 2 on your PC), and the
Pi link isn't used.

## 2. Where the policy comes from

```
 ┌──── YOUR PC: once, hours ─────┐            ┌── THE ROBOT: every run ───┐
 │ omniRL + MuJoCo on the GPU:   │            │ ESP32-S3 or a Pi:         │
 │ thousands of virtual robots   │            │ loads the trained network │
 │ learn to walk                 │   .mpxp    │ and runs it 50 times a    │
 │   1. copy the firmware's trot │ ────────►  │ second on the real IMU    │
 │   2. improve it (PPO) with    │    file    │ and servo data            │
 │      pushes, noise, floors,   │            │                           │
 │      servo delays             │            │                           │
 └───────────────────────────────┘            └───────────────────────────┘
        learning happens here                     no learning: it only runs (same on the Pi)
```

Learning needs the GPU, the simulator and millions of tries, so it is done
once on the PC. A Pi doesn't learn either: with the omniRL runner it does
exactly what the ESP does, just on a different computer.

The network is small: 45 numbers in, 12 out (45 → 128 → 128 → 64 → 12, about
31,000 weights). The ESP32-S3 runs it in about **2.6 ms**; a step comes every
20 ms.

**The 45 inputs:** the gyro (3), which way gravity points (3), the command you
give (3: forward, sideways, turn), the 12 joint angles, the 12 joint speeds,
and its own previous output (12).

**The 12 outputs:** a target angle for each servo, relative to the standing
pose.

## 3. One step, every 20 ms

```
 hardware timer, every 5 ms
        │
        ▼
 ┌─────────────── every tick (5 ms) ────────────────┐
 │ send the targets to the 4 servo boards,          │
 │ read their positions                  0.4 ms     │
 │ read the IMU, check tilt / current / temperature │
 └──────────────────────────────────────────────────┘
        │
        ▼
 ┌─── every 4th tick (20 ms, 50 Hz): policy step ───┐
 │ build the 45 inputs → run the network  2.6 ms    │
 │ → 12 new targets (±20° per step, safe range)     │
 └──────────────────────────────────────────────────┘
        │
        ▼  sent on the next tick: sensors → servos in 5 ms, every time
```

It runs in its own task on core 1 at high priority, so Wi-Fi, the screen and
the web app don't disturb it.

## 4. Why a small ESP32-S3 can run it

Training a policy needs a GPU for hours. **Running** the trained policy is
much smaller: 31,000 multiply-adds, 50 times a second. That fits on a
microcontroller.

### Inside the chip

```
 ┌─────────────────── ESP32-S3 (N16R8) ────────────────────┐
 │ 240 MHz, two Xtensa LX7 cores                           │
 │                                                         │
 │ ┌──────── core 0 ────────┐   ┌──────── core 1 ────────┐ │
 │ │ Wi-Fi, web server,     │   │ the policy task:       │ │
 │ │ phone app, voice,      │   │ 5 ms timer, servos,    │ │
 │ │ screen, micro-ROS      │   │ IMU, the network       │ │
 │ └────────────────────────┘   └────────────────────────┘ │
 │                                                         │
 │ FPU on each core: float32 multiply-add in hardware      │
 │ 512 KB SRAM: code that must be fast, stacks             │
 │ 8 MB PSRAM: the network's weights (122 KB)              │
 │ 16 MB flash: the firmware, with the policy built in     │
 └─────────────────────────────────────────────────────────┘
      │ SPI                  │ I2C                 │ Wi-Fi
      ▼                      ▼                     ▼
 4 servo boards (AT32)   IMU (BMI270)        phone, PC
```

- **Core 1 is the walking core.** The policy task has it to itself at high
  priority. Wi-Fi, the web app and micro-ROS run on core 0, so they can't
  delay a step.
- **The FPU** (floating-point unit) does a float32 multiply-add in one
  instruction. The network uses float32, the same as on the PC, which is why
  the ESP's servo targets match omniRL's.
- **PSRAM** is 8 MB of extra RAM on the module. The weights (122 KB) live
  there; the fast 512 KB SRAM is too full (about 11 KB free).
- **The servos and the IMU are wired to this chip**, so reading them and
  sending new targets takes 0.4 ms, with no cable to another computer.

### The budget

| | Needed | The ESP32-S3 has | Used |
|---|---|---|---|
| Maths per step | 31,104 multiply-adds (45·128 + 128·128 + 128·64 + 64·12) | 240 MHz core with an FPU | **2.6 ms** of each 20 ms step: 13% of core 1 |
| Maths per second | 1.6 million multiply-adds (50 steps) | | |
| Memory | 31,436 weights and biases × 4 bytes = **122 KB** | 8 MB PSRAM | 1.5% |
| Timing | a step every 20 ms, on time | 5 ms hardware timer, real-time task | **0 late steps** in your 12 s walk |

The multiplying alone would take well under 1 ms. Most of the 2.6 ms is
fetching the weights from PSRAM: 122 KB doesn't fit in the 32 KB cache, so
every step reads them again. It still leaves 17 ms of every step free.

### Why it works

1. **It only runs, it never learns.** No simulator, no gradients, no
   millions of tries: one pass through the network per step.
2. **The network is tiny.** 31,000 weights. An image model has millions; a
   chat model has billions.
3. **Hardware floating point.** The same float32 maths as the PC, fast.
4. **Real time.** FreeRTOS runs the task when the timer fires, every time.
   Nothing like Linux deciding to do something else first.
5. **Everything on one chip.** The IMU, the servos and the network are all on
   the ESP, so sensors → servos is 5 ms, always.

### And the Pi 5?

| | ESP32-S3 | Raspberry Pi 5 |
|---|---|---|
| CPU | 2 × Xtensa LX7, 240 MHz | 4 × Cortex-A76, 2.4 GHz |
| RAM | 512 KB + 8 MB PSRAM | 4–8 GB |
| Network time | 2.6 ms | well under 1 ms |
| Operating system | FreeRTOS: on time, every time | Linux: almost always on time, sometimes not |
| Path to the servos | same chip | UART cable to the ESP, then the servos |

The Pi has far more computing power, but the job only needs 13% of one ESP
core. Extra speed doesn't help the walk; what matters is that every step
arrives on time, and on the ESP it does.

## 5. ESP vs Pi

| | **Pi** (omniRL runner, ros2jazzympv2) | **ESP32-S3** (mpv2-uros) |
|---|---|---|
| Where the network runs | Pi 5, Python/numpy | ESP32-S3, C, on the robot |
| What connects it to the servos | J-PI UART cable at 921600 baud | nothing: same chip |
| Sensors → command delay | about 5 ms, plus Linux and UART jitter | **5.0 ms, the same every step** |
| Network time | well under 1 ms | 2.6 ms (fits easily in 20 ms) |
| Late steps in a 12 s walk | depends on what else the Pi is doing | 0 (measured) |
| Needs | a Pi, its SD card, the cable, `mp3link config` | nothing extra |
| Start / steer | ssh to the Pi, `runner run …` | phone: Settings → RL walk |
| Results with the same policy file | the reference | the same servo targets (checked to be identical) |

The ESP's maths is a C copy of omniRL's `contract.py` (the same inputs, the
same output rounding), tested against it: the same servo targets over 1500
random steps.

## 6. Run it

### From the phone

**Settings → Robot → RL walk**

1. **Mode:** *Walk forward* (Slow 0.05 / Normal 0.08 / Fast 0.12 m/s) or
   *Omni (stick)* (drag to walk any way, the slider turns).
2. **Run for:** 10 s, 30 s, 2 min or *Until Stop*.
3. Switches:
   - **Dry run:** nothing moves; it checks the IMU, the network and the timing.
   - **Use my calibration:** your robot's offsets (Settings → Calibration). Keep
     it **on** once you have calibrated.
   - **Record a log:** see section 8.
   - **Run a policy for sku2** (shown only for a policy trained for another
     robot): needed to start it.
4. **Start.** Keep the robot still for a second.
5. **Stop** (red) at any time.

### What happens during a run

```
 Start ─► getting ready ─► move to the stand ─► hold ─► WALK ─► move to the stand ─► hold ─► limp ─► normal gait
          1 s, keep still    2 s                 2 s     your time  1 s                2 s
          (servo settings,
           gyro zero)
```

### From the USB console

```
policy                         status
policy test                    self-test and time per step
policy run 10 0.08 0 0         10 s forward at 0.08 m/s
policy run 0 0 0 0.3           turn on the spot until `policy stop`
policy run 10 0.08 0 0 dry     the same, servos untouched
policy offsets robot           use the robot's calibration (until reboot)
policy log on                  record the next runs; `policy log print` shows the last one
policy getup                   get up from wherever it lies (needs the get-up policy, section 12)
policy autoup on               never stays down: a walk that falls gets up and walks on
policy stop
```

## 7. It stops by itself

| What | What it does |
|---|---|
| Body tilts over 35° | goes limp at once (with **Never stays down** on: gets up and walks on, section 12) |
| A servo over 500 mA for 0.5 s, or over 60 °C for 1 s | goes limp at once |
| A leg stops answering | goes limp at once |
| IMU drops out, a servo reaches 55 °C, two late steps in a row | ramps to the stand, then stops |
| The phone stops talking to it for 2 s (locked, closed, Wi-Fi gone) | ramps to the stand, then stops |
| The time you chose is up, or you press Stop | ramps to the stand, then stops |

## 8. The log: is it good?

Turn on **Record a log**. When the run ends the page shows a summary
(**Copy summary**) and the full log (**Download full log (CSV)**, one line per
20 ms step).

| Line | Good | Your 12 s walk (14 Oct) |
|---|---|---|
| steps with a late tick | 0 (a few is fine) | **0** |
| step period | 20 ms | **20.0 ms** (19.5–20.5) |
| tick | under 5 ms (up to 7.5 ms is still on time) | 3.7 ms average, 5.8 max |
| network | well under 20 ms | **2.6 ms** |
| servo exchange | about 0.4 ms | **0.39 ms** |
| legs answering | 4/4 | **4/4** |
| servos behind their target, walking | a few degrees on average | 1.9° (max 12.7° in fast swings) |
| tilt while holding the stand | about the same as the normal stand | **2.6°** (7.2° with the policy's calibration) |
| hottest servo / max current | under 55 °C / 2 A | 38.6 °C / 1.3 A |

**The tilt line** is the calibration check: if the body leans more in the
policy's stand than in the robot's normal stand, the calibration doesn't match.
Calibrate, and turn on **Use my calibration**.

## 9. Changing the policy

The built-in policy is `components/mpx_policy/policies/default.mpxp`. Two are
kept next to it:

| File | Trained for |
|---|---|
| `sku2_stops_v1.mpxp` | SKU2 (450 g), omniRL's `bam_t5ub06_stops_v1` |
| `sku1_stops_s1.mpxp` | SKU1 (351.5 g), trained on this PC (11 Oct) |

To switch, copy one over `default.mpxp`, then build and flash:

```powershell
cd C:\esp\projects\mpv2-uros\components\mpx_policy\policies
copy sku1_stops_s1.mpxp default.mpxp
cd C:\esp\projects\mpv2-uros
idf.py -p com18 build flash monitor
```

A **new** policy: train it in omniRL (`docs/SKU1.md`), then export it for the
ESP:

```powershell
cd C:\esp\projects\omniRL
. .\tools\sku1_env.ps1
& $PY -B scripts\export_policy.py runs\ppo\<run>\model_1299.pt policies\<name>
& $PY C:\esp\projects\mpv2-uros\tools\policy\export_mpxp.py policies\<name> `
    --contract mp3_walk_policy\data\sku2_contract.json --mount mp3_walk_policy\data\imu_mount.json `
    --out policies\<name>\<name>.mpxp
```

…and copy the `.mpxp` over `default.mpxp` as above.

## 10. Calibration: which one does it use?

| Policy | Calibration by default |
|---|---|
| Trained for this robot (sku1 policy, firmware set to sku1) | your robot's own |
| Trained for another robot (the sku2 policy) | the one saved in the policy (that robot's) |

The **Use my calibration** switch overrides it for a run. The policy itself
does not need retraining for a different calibration: it works in angles
relative to the standing pose, and the calibration only turns those into servo
positions.

## 11. Troubleshooting

| You see | Do |
|---|---|
| Nothing moves | Dry run is on (by design), or no servo power. Look at the *Ended:* line. |
| *Start* refused: "this policy is for sku2…" | turn on **Run a policy for sku2** |
| The body leans in the stand | calibrate, turn on **Use my calibration** |
| It stops after a second or two: "the app stopped answering" | the phone locked or Wi-Fi dropped: keep the page open |
| Late steps in the log | send me the log |
| The RL walk row isn't in Settings | the phone has the old app: close and reopen it |
| No **Get up** button, no **Never stays down** switch | no get-up policy is built in: section 12 |
| Get up ends "could not get up in 10 s" | send me the log; check nothing blocks the legs |

## 12. Getting up after a fall (never stays down)

A second policy, trained in omniRL (`docs/GETUP.md`), rolls the robot back onto its feet from its back, a side or any
angle. It has the walk's inputs and outputs, so the ESP runs it the same way, from PSRAM, in 2.6 ms a step.

```
 walk (RUN) ── tilt over 35° ──► GETUP: the get-up policy ── upright, still, at the stand 0.5 s ──► walk again
                                   │                                                               (up to 3 falls)
                                   └── not up in 10 s ──► ramp to the stand, limp
```

**Put it on the robot:** export it as `components/mpx_policy/policies/getup.mpxp` (`docs/GETUP.md` section 3 in
omniRL), then build and flash. Without that file the firmware builds as before, without the feature. `policy`
shows the get-up line when it is in.

**Use it:**

- **Get up** (RL walk page, *Fallen over?*; console `policy getup`): from wherever it lies to the stand, then the
  robot is handed back standing.
- **Never stays down** (a switch above Start; console `policy autoup on`): during a walk, a fall starts GETUP instead
  of going limp, and the walk goes on once it stands.

**What changes in GETUP:**

| | Walk | GETUP |
|---|---|---|
| Joint range about the stand | 20° (abduction), 60° | 90° on every joint |
| Tilt stop | over 35°: limp | none (it starts lying down) |
| Current guard | 500 mA for 0.5 s | 1000 mA for 1 s |
| Temperature, a leg not answering, the phone | as always | as always |
| Gives up | | after 10 s: ramps to the stand, limp |

First tries: section 4 of omniRL's `docs/GETUP.md` (held in the air first, then on a soft surface). The log's
summary has a *getting up* line: how many times, how many stood, the peak current.

## Files

| Path | What |
|---|---|
| `components/mpx_policy/src/policy_core.c` | the maths: inputs, network, outputs (a C copy of omniRL's `contract.py`) |
| `components/mpx_policy/src/policy_run.cc` | the 5 ms loop, the run sequence, the safety stops, the log |
| `components/mpx_policy/src/policy_console.c` | the `policy` console command |
| `components/mpx_policy/policies/` | the built-in policy and the saved ones |
| `components/http_server/http_server_mpx_robot_api.c` | `/v1/policy` (the phone's start, stop, stick, log) |
| `components/http_server/frontend_pwa_mpv2/src/lib/PolicyPage.svelte` | the RL walk page |
| `tools/policy/export_mpxp.py` | turns an omniRL policy into a `.mpxp` file |
| `tools/policy/host_test/` | the check against omniRL's own numbers (walk and get-up policies) |
| `components/mpx_policy/policies/getup.mpxp` | the get-up policy, built in when it is there |
