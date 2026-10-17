# rl branch: the RL walk on the robot itself

A walk policy trained in simulation (omniRL) runs on the robot's own
ESP32-S3 and drives the twelve servos. No Raspberry Pi. The app's
**Settings → RL walk** page starts it, steers it and reads its logs.

## What is on

| | |
|---|---|
| On | the RL walk (`CONFIG_APP_POLICY_ENABLE`), the robot's own gait (before and after a run), Wi-Fi and the app, LittleFS, sysmon |
| Off | the face and GIFs (`DISPLAY`), the on-robot agent and skills (`AGENT`), Lua (`LUA`), speaker and microphones (`AUDIO`), voice (`VOICE`), the Raspberry Pi link (`PI_LINK`, `RL_LINK`), micro-ROS (`UROS`), the IMU log (`IMU_LOG`) |

All `CONFIG_APP_*` in `main/Kconfig.projbuild`. The choices are at the end of
`sdkconfig.defaults` (`sdkconfig` itself is not in git), so a fresh clone
builds exactly this firmware. The code of the switched-off features is still
in the tree, compiled out: any of them comes back with `idf.py menuconfig` →
*MPX Dog App Configuration*. Everything off leaves RAM, PSRAM and CPU to the
walk: a bigger network fits.

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

After any change to Kconfig or `sdkconfig`, run `idf.py reconfigure` once
first.

## The code

| | |
|---|---|
| `main/rl/policy_core.c` | the network and the observation/action contract (plain C, also built on the PC by `tools/policy/host_test`) |
| `main/rl/policy_run.cc` | a run: start, the 50 Hz loop, stops, logs, the size test |
| `main/network/http_policy.cc` | `/v1/policy` |
| `pwa-claw/src/lib/RLWalkPage.svelte` | the app's RL walk page |
| `main/rl/policies/*.mpxp` | the built-in policies |

## Policies

Built in (`main/rl/policies/`):

| File | Policy | For |
|---|---|---|
| `sku1.mpxp` | `sku1_368_stops_s2` | SKU1 |
| `default.mpxp` | `bam_t5ub06_stops_v1`, the first walk | SKU2 |
| `terrain.mpxp` | `bam_terrain_s2`, the terrain walk | SKU2 |
| `getup.mpxp` | the get-up policy, built in when the file is there (omniRL `docs/GETUP.md`) | |

All three walks are the same size (45 → 128 → 128 → 64 → 12, about 31k
weights) and omnidirectional: trained to follow forward, sideways and turning
commands up to 0.12 m/s, 0.12 m/s and 0.6 rad/s (the page's **Omni (stick)**;
**Walk forward** holds one forward speed).

At boot the robot picks the first one made for itself (`APP_POLICY_ROBOT`,
`sku1` or `sku2`, default `sku1`); the page switches between them. A policy
for the other robot needs "Run a policy for …" on the page: another body can
do anything on this one.

A new policy: export it from omniRL with `tools/policy/export_mpxp.py` into
`main/rl/policies/` and rebuild ([tools/policy/README.md](../tools/policy/README.md),
which also has the PC test of the arithmetic).

## A run

Servo configuration, 1 s still for the gyro bias, ramp to the stand, walk,
ramp back, and the gait takes over again. It stops by itself on a big tilt, a
silent servo board, a leg that stops following, or when the page stops
talking to it for 2 s (the stick is also the dead-man). First runs: robot on a
stand, feet off the ground.

The temperature guard (`APP_POLICY_TEMP_GUARD`) is off by default: the hottest
servo is still shown and logged. Watch it.

**The loop.** The 5 ms servo tick runs on core 1. The network has its own
task (`pol_net`, core 1, just below the tick): the tick hands it the
observation every 20 ms (50 Hz) and keeps reading the servos and the IMU while
it computes; the first tick after it is done sends its action. A network done
within one tick acts at once; a bigger one may take most of the 20 ms and acts
5 ms later for each tick it spans (train with that delay). Still busy at the
next step: a late step; two in a row stop the run.

**Weights: 32, 16 or 8 bits** (`APP_POLICY_WEIGHT_BITS`, default 16). The
file holds floats; the robot can round them to 16- or 8-bit integers (one
scale per row), so each step reads half or a quarter of the bytes. The
self-test compares the result with training's own answers and falls back to
floats if the rounding is too large (16-bit: 0.01° at worst; 8-bit: 0.4° mean,
2.3° at worst). The page's **Network** card switches it until the next boot.
The weights go to internal RAM only while `APP_POLICY_INTERNAL_RESERVE_KB`
(96 KB) stays free for Wi-Fi; otherwise PSRAM.

## How big a network fits: the size test

**RL walk → Size test → How big a network fits?** times networks of the walk's
shape from 31k weights up (random weights, nothing moves, about 10 s) and says
which fit and how much later than today's they act. Run it once more after a
walk on the stand: then it counts the real servo exchange too.

## Running without Wi-Fi

**RL walk → Without Wi-Fi** (walk forward for 10 s, 30 s or 2 min). The robot
answers the app, then switches its radio off for the whole run: no Wi-Fi
interrupts, no packets. When the run ends it:

1. prints a summary on the serial port (steps, late ticks, step period, tick,
   network and servo-exchange times, tilt, tracking, temperature, current);
2. saves the full log to flash, `/fs/rl_last_run.csv`;
3. switches Wi-Fi back on (its hotspot, and your router if one is saved).

The page counts down, knocks until the robot answers, then shows the log as
after any run, marked `WI-FI OFF`. Nothing can stop the robot from the app
during such a run: it stops by itself when the time is up, on a big tilt or if
a leg stops following. Run it on the stand first. At most 2 minutes; the radio
comes back at the latest 30 s after the run should have ended.

On the robot's own hotspot your phone may move to another network meanwhile:
join the robot's again and the page carries on. If the page was closed, open
RL walk again: **Show the last saved log** reads the copy on flash (it
survives a reboot).

The size test can run without Wi-Fi as well (**Size test → Without Wi-Fi**).
Run it once each way with the same bits to see what Wi-Fi costs the network.

## HTTP routes

| Route | |
|---|---|
| `GET /v1/policy` | state, the loaded policy, the last run's numbers |
| `POST /v1/policy` | `{"action":"run","seconds":10,"vx":0.05,…}` (`"offline":true`: Wi-Fi off for the run), `getup`, `cmd` (the stick, also the dead-man), `keepalive`, `stop`, `test` (self-test), `policy` (switch the walk), `weights` (32/16/8), `bench` (the size test) |
| `GET /v1/policy/log` | the last logged run, CSV, one line per 20 ms step |
| `GET /v1/policy/bench` | the size test's result |
