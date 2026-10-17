# tools/policy -- RL walk policies for the robot itself

`main/rl` runs a walk policy on the ESP32-S3 (the app's RL Walk page,
`/v1/policy`). These tools get a trained policy into it and prove the robot
computes what training expects.

## Export a policy

From an omniRL policy directory (`policy.npz`, written by omniRL
`scripts/export_policy.py`):

```bash
python tools/policy/export_mpxp.py <omniRL>/policies/bam_t5ub06_stops_v1 \
    --contract <omniRL>/mp3_walk_policy/data/sku2_contract.json \
    --mount <omniRL>/mp3_walk_policy/data/imu_mount.json \
    --robot sku2 --out main/rl/policies/default.mpxp
```

`--robot` names the robot the contract describes. The firmware compares it
with its own (`APP_POLICY_ROBOT`, default `sku1`) and does not run a policy
for the other robot unless the app asks with "force".

`main/rl/policies/default.mpxp` is built into the firmware; replace it and
rebuild. A `main/rl/policies/getup.mpxp` next to it is built in too, as the
get-up policy (omniRL `docs/GETUP.md`).

## Check the arithmetic on a PC

`host_test/` compiles the robot's `policy_core.c` on the PC and runs it next to
omniRL's own numpy (`contract.py`, `export.py`) on 5 x 300 random steps:
observation, network and the servo targets it sends.

```bash
cd tools/policy/host_test
make OMNIRL=/path/to/omniRL POLICY=bam_t5ub06_stops_v1
```

Expected: `max |dd diff| 0` (the same servo targets every step) and errors of
about 1e-6 on the observation and the network.
