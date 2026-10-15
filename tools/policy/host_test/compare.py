"""Run omniRL's numpy contract on random sequences and compare the C core.
   python3 compare.py <omniRL mp3_walk_policy dir> <policy dir> <file.mpxp> <host_policy binary>"""
import json, struct, subprocess, sys, tempfile
from pathlib import Path
import numpy as np
mp, pdir, mpxp, exe = sys.argv[1:5]
sys.path.insert(0, str(Path(mp).parent))
from mp3_walk_policy.contract import Contract, ActionDecoder, VelocityFilter, assemble_observation, CONTRACT_JSON
from mp3_walk_policy.export import NumpyPolicy
c = Contract.load()
pol = NumpyPolicy(Path(pdir) / "policy.npz")
# A get-up policy (omniRL getup.py): its own band about the stand, and no command (the firmware zeroes it).
getup = pol.meta.get("kind") == "getup"
band = np.radians(pol.meta["band_deg"]) if getup else None
R = np.array([[0.0, 1.0, 0.0], [1.0, 0.0, 0.0], [0.0, 0.0, -1.0]])   # swap_xy
rng = np.random.default_rng(1)
worst = dict(obs=0.0, raw=0.0, dd=0)
for trial in range(5):
    n = 300
    bias = rng.normal(0, 0.01, 3)
    dd = np.asarray(c.stand_dd, dtype=float)
    ticks, cmds = [], []
    for t in range(1 + 4 * n):
        dd = np.clip(np.rint(dd + rng.normal(0, 4, 12)), 1000, 2200)
        acc = np.array([0.02, -0.03, -1.0]) + rng.normal(0, 0.2, 3)
        gyr = rng.normal(0, 0.5, 3)
        ticks.append((dd.copy(), acc, gyr))
    for k in range(n):
        cmds.append(np.zeros(3) if getup else rng.uniform(-0.12, 0.12, 3) * np.array([1, 1, 5]))
    # python reference (runner.ObservationBuilder + loop)
    vf = VelocityFilter(); dec = ActionDecoder(c, pol.scale)
    if getup:
        dec.band = band
    ref = []
    k = 0
    for t, (ddt, acc, gyr) in enumerate(ticks):
        vf.push(c.rel_from_dd(ddt))
        if t == 0 or t % 4:
            continue
        gb = R @ (gyr - bias)
        a = R @ acc
        grav = -a / np.linalg.norm(a)
        obs = assemble_observation(gb, grav, cmds[k], c.rel_from_dd(ddt), vf.velocity(), dec.prev_action)
        raw = pol(obs)
        d = dec(raw)
        ref.append((obs, raw, d["dd"]))
        k += 1
    with tempfile.TemporaryDirectory() as td:
        inp = Path(td) / "in.bin"; outp = Path(td) / "out.txt"
        vals = [float(n)] + list(bias) + list(c.offsets_deg)
        ci = 0
        for t, (ddt, acc, gyr) in enumerate(ticks):
            vals += list(ddt) + list(acc) + list(gyr)
            if t and t % 4 == 0:
                vals += list(cmds[ci]); ci += 1
        inp.write_bytes(struct.pack("<%dd" % len(vals), *vals))
        r = subprocess.run([exe, mpxp, str(inp), str(outp)], capture_output=True, text=True)
        if trial == 0: print(r.stdout.strip())
        if r.returncode: print(r.stderr); sys.exit(1)
        rows = [np.array(l.split(), dtype=float) for l in outp.read_text().splitlines()]
    for (obs, raw, dd), row in zip(ref, rows):
        worst["obs"] = max(worst["obs"], float(np.abs(row[:45] - obs).max()))
        worst["raw"] = max(worst["raw"], float(np.abs(row[45:57] - raw).max()))
        worst["dd"] = max(worst["dd"], int(np.abs(row[57:69] - dd).max()))
print("5 x 300 steps: max |obs diff| %.3g, max |action diff| %.3g, max |dd diff| %d" % (worst["obs"], worst["raw"], worst["dd"]))
