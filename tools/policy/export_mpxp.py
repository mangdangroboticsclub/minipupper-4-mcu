#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Pack an omniRL-style walk policy into one .mpxp file the robot runs itself.

    python tools/policy/export_mpxp.py <omniRL>/policies/bam_t5ub06_stops_v1 \\
        --contract <omniRL>/mp3_walk_policy/data/sku2_contract.json \\
        --mount <omniRL>/mp3_walk_policy/data/imu_mount.json --robot sku2 \\
        --out bam_t5ub06_stops_v1.mpxp

The file holds everything the Pi runner (omniRL mp3_walk_policy/runner.py)
needs besides the robot: the network (Linear layers, ELU between them), the
contract (servo signs, calibration offsets, action scale, the band about the
stand, the per-step limit), the IMU mounting, the servo configuration the
policy was identified with (`mp3link config`), the safety limits, and a few
fixtures (observations with the actions training produced for them) so the
robot can check its own forward pass. numpy only.

Format (little-endian, every field 4 bytes): see HEADER below and
main/rl/policy_core.h, which must agree.
"""
import argparse
import json
import struct
import zlib
from pathlib import Path

import numpy as np

MAGIC = b"MPXP"
VERSION = 1
HEADER_SIZE = 640
MAX_LAYERS = 8
MAX_CFG = 8
ACT_ELU = 1
FLAG_OMNI = 1
FLAG_GETUP = 2      # omniRL getup.py: the firmware runs it after a fall (policy getup, never stays down)
FLAG_GETUP_PHASE = 4   # omniRL getup_track.py: it tracks a motion, and the firmware feeds the phase in the command
                       # inputs; cmd_max carries the motion's length (s) and the phase at which it is upright

# omniRL's runner and contract (mp3_walk_policy/contract.py, runner.py)
STEP_LIMIT_DEG = 20.0
ABDUCTION = (0, 3, 6, 9)
BAND_DEG = [20.0 if i in ABDUCTION else 60.0 for i in range(12)]
CTRL_DT = 0.020
STATE_DT = 0.005
TILT_LIMIT_DEG = 35.0
TILT_TAU_S = 0.1
TEMP_STOP_C = 55.0
TEMP_START_C = 45.0
CAP_MA = 2000.0
# Training's command ranges (docs/USER_GUIDE.md: 0.05-0.12 m/s in any
# direction, |wz| <= 0.6 rad/s); the robot clamps to these.
CMD_MAX = (0.12, 0.12, 0.6)
# `mp3link config` (the configuration the servos were identified with).
# Driver-board parameter ids: main/robot/driver_board.h.
SERVO_CFG = [(11, 1.0),      # control_mode
             (5, 80.0),      # kp_position
             (6, 400.0),     # kd_position
             (10, 0.0),      # ki_position
             (7, 0.0),       # kp_current
             (8, 0.001),     # kff_current
             (9, 0.99)]      # max_pwm_duty_cycle
IMU_MOUNTS = {
    "flip_x": [[1, 0, 0], [0, -1, 0], [0, 0, -1]],
    "flip_y": [[-1, 0, 0], [0, 1, 0], [0, 0, -1]],
    "swap_xy": [[0, 1, 0], [1, 0, 0], [0, 0, -1]],
    "swap_xy_neg": [[0, -1, 0], [-1, 0, 0], [0, 0, -1]],
}


def header(name, robot, dims, flags, n_fix, c, mount, band=BAND_DEG, tilt_limit=TILT_LIMIT_DEG, cmd_max=CMD_MAX):
    f = []
    f.append(struct.pack("<4sIII", MAGIC, VERSION, 0, 0))            # size, crc filled in later
    f.append(struct.pack("<48s16s", name.encode()[:47], robot.encode()[:15]))
    f.append(struct.pack("<I", len(dims) - 1))
    f.append(struct.pack("<9I", *(list(dims) + [0] * (MAX_LAYERS + 1 - len(dims)))))
    f.append(struct.pack("<IIII", ACT_ELU, flags, n_fix, 0))
    f.append(struct.pack("<4f", CTRL_DT, STATE_DT, STEP_LIMIT_DEG, tilt_limit))
    f.append(struct.pack("<4f", TILT_TAU_S, TEMP_STOP_C, TEMP_START_C, CAP_MA))
    f.append(struct.pack("<3f", *cmd_max))
    f.append(struct.pack("<9f", *np.asarray(mount, dtype=float).ravel()))
    f.append(struct.pack("<12f", *c["signs"]))
    f.append(struct.pack("<12f", *c["scale"]))
    f.append(struct.pack("<12f", *band))
    f.append(struct.pack("<12f", *c["offsets_deg"]))
    f.append(struct.pack("<12i", *[int(v) for v in c["stand_dd"]]))
    f.append(struct.pack("<I", len(SERVO_CFG)))
    cfg = SERVO_CFG + [(0xFFFFFFFF, 0.0)] * (MAX_CFG - len(SERVO_CFG))
    for pid, val in cfg:
        f.append(struct.pack("<If", pid, val))
    h = b"".join(f)
    assert len(h) <= HEADER_SIZE, len(h)
    return h + b"\0" * (HEADER_SIZE - len(h))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("policy", help="a policy directory with policy.npz (omniRL scripts/export_policy.py)")
    ap.add_argument("--contract", required=True, help="the contract JSON the policy was trained on")
    ap.add_argument("--mount", required=True, help="imu_mount.json (sensor -> body), or a mount name")
    ap.add_argument("--robot", help="sku1 or sku2: the robot it was trained for (default: the policy.npz "
                                    "metadata, written by omniRL's export since MP3_ROBOT)")
    ap.add_argument("--name", help="default: the policy directory's name")
    ap.add_argument("--fixtures", type=int, default=32)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    pdir = Path(a.policy)
    d = np.load(pdir / "policy.npz", allow_pickle=False)
    meta = json.loads(str(d["meta"]))
    robot = a.robot or meta.get("robot")
    if not robot:
        raise SystemExit("policy.npz does not say which robot it was trained for: give --robot sku1 or sku2")
    if a.robot and meta.get("robot") and a.robot != meta["robot"]:
        raise SystemExit(f"--robot {a.robot}, but the policy was trained for {meta['robot']}")
    n = int(d["n_layers"])
    layers = [(d[f"W{i}"].astype(np.float32), d[f"b{i}"].astype(np.float32)) for i in range(n)]
    dims = [layers[0][0].shape[1]] + [w.shape[0] for w, _ in layers]
    if n > MAX_LAYERS:
        raise SystemExit(f"{n} layers: at most {MAX_LAYERS}")
    if dims[0] != 45 or dims[-1] != 12:
        raise SystemExit(f"network {dims}: the contract needs 45 inputs and 12 outputs")
    if meta.get("activation", "elu") != "elu":
        raise SystemExit(f"activation {meta.get('activation')}: only ELU is implemented")

    contract = json.loads(Path(a.contract).read_text())
    if Path(a.mount).exists():
        mount_name = json.loads(Path(a.mount).read_text())["mount"]
    else:
        mount_name = a.mount
    if mount_name not in IMU_MOUNTS:
        raise SystemExit(f"unknown IMU mount {mount_name!r}")
    c = {"signs": [float(v) for v in contract["signs"]],
         "offsets_deg": [float(v) for v in contract["offsets_deg"]],
         "stand_dd": contract["stand_dd"],
         "scale": [float(v) for v in d["scale"]]}

    k = max(0, min(a.fixtures, d["fixture_obs"].shape[0]))
    fobs = d["fixture_obs"][:k].astype(np.float32)
    fact = d["fixture_act"][:k].astype(np.float32)
    flags = FLAG_OMNI if meta.get("omni", False) else 0
    name = a.name or pdir.name
    kind = meta.get("kind", "walk")
    extra = {}
    if kind == "getup":
        # Its own range about the stand; no tilt stop (it starts lying down: the firmware ignores the tilt in
        # GETUP, and the walk's limit decides when a walk has fallen); no command.
        band = meta.get("band_deg")
        if not band or len(band) != 12 or not all(0.0 < float(v) <= 150.0 for v in band):
            raise SystemExit(f"a get-up policy needs its band_deg (12 values, 0-150): {band}")
        flags |= FLAG_GETUP
        extra = dict(band=[float(v) for v in band], tilt_limit=180.0, cmd_max=(0.0, 0.0, 0.0))
        if meta.get("phase_T_s"):
            T, up = float(meta["phase_T_s"]), float(meta.get("phase_up", 1.0))
            if not (0.5 <= T <= 20.0 and 0.0 <= up <= 1.0):
                raise SystemExit(f"a tracked get-up's phase: length {T} s, upright at {up}")
            flags |= FLAG_GETUP_PHASE
            extra["cmd_max"] = (T, up, 0.0)
    elif kind != "walk":
        raise SystemExit(f"unknown policy kind {kind!r}")
    body = [header(name, robot, dims, flags, k, c, IMU_MOUNTS[mount_name], **extra)]
    for w, b in layers:
        body.append(np.ascontiguousarray(w, dtype="<f4").tobytes())
        body.append(np.ascontiguousarray(b, dtype="<f4").tobytes())
    body.append(np.ascontiguousarray(fobs, dtype="<f4").tobytes())
    body.append(np.ascontiguousarray(fact, dtype="<f4").tobytes())
    blob = bytearray(b"".join(body))
    struct.pack_into("<I", blob, 8, len(blob))
    struct.pack_into("<I", blob, 12, zlib.crc32(bytes(blob[16:])) & 0xFFFFFFFF)
    Path(a.out).write_bytes(bytes(blob))
    what = ("GET-UP (tracked motion)" if flags & FLAG_GETUP_PHASE else "GET-UP" if flags & FLAG_GETUP
            else "omni" if flags & FLAG_OMNI else "forward-only")
    print(f"{a.out}: {name} for {robot}, net {dims}, {what}, "
          f"mount {mount_name}, {k} fixtures, {len(blob)} bytes")


if __name__ == "__main__":
    main()
