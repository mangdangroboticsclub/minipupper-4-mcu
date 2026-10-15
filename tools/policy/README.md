# tools/policy — RL walk policies for the robot itself

`components/mpx_policy` runs a walk policy on the ESP32-S3 (`policy` on the
serial console). These tools get a trained policy into it and prove the robot
computes what training expects.

## Export a policy

From an omniRL-style policy directory (`policy.npz`, written by omniRL
`scripts/export_policy.py`):

```bash
python tools/policy/export_mpxp.py <omniRL>/policies/bam_t5ub06_stops_v1 \
    --contract <omniRL>/mp3_walk_policy/data/sku2_contract.json \
    --mount <omniRL>/mp3_walk_policy/data/imu_mount.json \
    --robot sku2 --out bam_t5ub06_stops_v1.mpxp
```

`--robot` names the robot the contract describes. The firmware compares it
with its own (`MP4_POLICY_ROBOT`, default `sku1`) and will not run a policy for
another robot without `force`.

Put the file on the robot as `/fatfs/policies/<name>.mpxp`, then
`policy load <name>` (the choice is remembered). To build one in instead,
replace `components/mpx_policy/policies/default.mpxp` and rebuild.

## Check the arithmetic on a PC

`host_test/` compiles the robot's `policy_core.c` on the PC and runs it next to
omniRL's own numpy (`contract.py`, `export.py`) on 5 × 300 random steps:
observation, network and the servo targets it sends.

```bash
cd tools/policy/host_test
make OMNIRL=/path/to/omniRL POLICY=bam_t5ub06_stops_v1
```

Expected: `max |dd diff| 0` (the same servo targets every step) and errors of
about 1e-6 on the observation and the network.
