"""Command line: python3 -m mp4link <command> ...

Servo ids 1..12: 1-3 FR, 4-6 FL, 7-9 RR, 10-12 RL (abduction, hip, knee).
Positions are raw AT32 deci-degrees (0.1 deg). Every motion command disarms on
exit; the firmware's watchdog covers a crash.

Calibration positions use the main firmware's servo code (0..1023, 511 =
centre). `centre` holds servos at the centre (code 511, offsets ignored) while
the legs are mounted; `calib` holds the Ini pose (code 511 + offset) and
adjusts the offsets, which the ESP32 stores in NVS; `offsets` reads them and
writes a stand file; `--stand nvs` uses them directly.
"""

from __future__ import annotations

import argparse
import json
import math
import select
import sys
import termios
import time
import tty

import numpy as np
import serial

from . import protocol as p
from .link import DEFAULT_BAUD, DEFAULT_PORT, Link, LinkError

NAMES = ["FR abd", "FR hip", "FR knee", "FL abd", "FL hip", "FL knee",
         "RR abd", "RR hip", "RR knee", "RL abd", "RL hip", "RL knee"]
ABDUCTION = (1, 4, 7, 10)
# How far a target may go from the stand pose [deg], and per send [deg].
MAX_DEV_DEG = np.array([20.0 if i + 1 in ABDUCTION else 60.0 for i in range(12)])
MAX_STEP_DEG = 15.0
# The main firmware's calibration convention, in float32: the Ini/stand code is
# 511 + (int)(offset * 10240 / 3100), and the AT32 target is
# (uint16)(3100 - code * 3100 / 1024) deci-degrees.
CENTRE_TICK = 511
OFFSET_MAX_DEG = 45.0
ONE_TICK_DEG = 3100.0 / 10240.0


# ---- helpers ------------------------------------------------------------------
class Pacer:
    """Wall-clock pacing: sleep, then spin the last millisecond."""

    def __init__(self, period: float):
        self.period = period
        self.next = time.monotonic()
        self.stamps: list[float] = []

    def wait(self) -> None:
        self.next += self.period
        delay = self.next - time.monotonic()
        if delay > 0.002:
            time.sleep(delay - 0.001)
        while time.monotonic() < self.next:
            pass
        if time.monotonic() - self.next > self.period:   # overrun: restart the grid
            self.next = time.monotonic()
        self.stamps.append(time.monotonic())


def smoothstep(x: float) -> float:
    x = min(max(x, 0.0), 1.0)
    return x * x * (3.0 - 2.0 * x)


def parse_ids(text: str) -> list[int]:
    ids: list[int] = []
    for part in text.split(","):
        if "-" in part:
            a, b = part.split("-")
            ids += list(range(int(a), int(b) + 1))
        elif part:
            ids.append(int(part))
    if any(i < 1 or i > 12 for i in ids):
        raise SystemExit("servo ids are 1..12")
    return ids


def offset_ticks(offsets_deg) -> np.ndarray:
    off = np.asarray(offsets_deg, dtype=np.float32)
    return CENTRE_TICK + np.trunc(off * np.float32(10240.0) / np.float32(3100.0)).astype(int)


def ticks_to_dd(ticks) -> np.ndarray:
    t = np.asarray(ticks, dtype=np.float32)
    return np.trunc(np.float32(3100.0) - t * np.float32(3100.0) / np.float32(1024.0)).astype(int)


def read_offsets(link: Link) -> tuple[np.ndarray, list[bool]]:
    """The twelve stored offsets [deg] (unset reads 0) and which are stored."""
    vals = [link.get_offset(i) for i in range(1, 13)]
    return np.array([0.0 if v is None else v for v in vals]), [v is not None for v in vals]


def require_config(link: Link, ids) -> None:
    """Holding a pose on the flash gains (kff 0.00022) is several times softer."""
    for i in ids:
        kff = link.get_param(i, "kff_current")
        if kff < 0.0009:
            raise LinkError("servo %d runs kff %g, not 0.001: run `config` first" % (i, kff))


def mask(ids) -> int:
    m = 0
    for i in ids:
        m |= 1 << (i - 1)
    return m


def load_stand(path: str, link: Link | None = None) -> np.ndarray:
    if path == "nvs":
        if link is None:
            raise SystemExit("--stand nvs needs the link")
        return ticks_to_dd(offset_ticks(read_offsets(link)[0])).astype(float)
    with open(path) as f:
        stand = np.asarray(json.load(f)["stand_dd"], dtype=float)
    if stand.shape != (12,):
        raise SystemExit("%s: stand_dd must hold 12 values" % path)
    return stand


def present(link: Link) -> np.ndarray:
    """Positions from two consecutive fresh STATE frames that agree."""
    states = []
    for _ in range(20):
        st = link.wait_state(1.0)
        if st.boards == 0x0F:
            states.append(st)
            if len(states) == 2:
                break
    if len(states) < 2:
        raise LinkError("no fresh feedback from every board")
    a, b = (s.pos_dd.astype(float) for s in states)
    if np.any(a < 100) or np.any(a > 3000) or np.any(np.abs(a - b) > 20):
        raise LinkError("implausible positions (dd): %s / %s" % (a.astype(int).tolist(), b.astype(int).tolist()))
    return b


def clip_band(stand: np.ndarray, target: np.ndarray) -> np.ndarray:
    """Clip a target to the band about the stand pose (MAX_DEV_DEG)."""
    return np.clip(target, stand - MAX_DEV_DEG * 10, stand + MAX_DEV_DEG * 10)


def preflight(link: Link, clear: bool) -> None:
    """Refuse to arm over a latched fault unless --clear was given."""
    st = link.wait_state(1.0)
    latched = st.flags & (p.ST_FAULTS | p.ST_REJECTED)
    if latched:
        what = "%s (servo %d)" % (", ".join(n for b, n in p.FLAG_NAMES.items() if latched & b), st.fault_servo)
        if not clear:
            raise LinkError("fault latched: %s; look at it, then rerun with --clear" % what)
        print("clearing latched fault:", what)


class Mover:
    """Sends clipped targets, arms on the first frame, records what it sent."""

    def __init__(self, link: Link, stand: np.ndarray | None, enable: int, cap: int, clear: bool = False):
        self.link, self.stand, self.enable, self.cap, self.clear = link, stand, enable, cap, clear
        self.last: np.ndarray | None = None
        self.armed = False
        self.first_seq: int | None = None
        self.clipped = 0

    def send(self, target_dd: np.ndarray) -> None:
        t = np.asarray(target_dd, dtype=float).copy()
        if self.stand is not None:
            c = clip_band(self.stand, t)
            self.clipped += int(np.any(c != t))
            t = c
        if self.last is not None:
            step = MAX_STEP_DEG * 10
            c = np.clip(t, self.last - step, self.last + step)
            self.clipped += int(np.any(c != t))
            t = c
        t = np.clip(np.rint(t), 0, 3100)
        flags = 0 if self.armed else (p.SA_ARM | (p.SA_CLEAR if self.clear else 0))
        seq = self.link.set_all(t.astype(int), self.enable, flags, self.cap)
        if not self.armed:
            self.first_seq = seq
        self.armed = True
        self.last = t


def check_flags(link: Link, mover: Mover | None = None) -> None:
    """Raise if the reader stopped, a fault or the watchdog latched, or the
    ESP32 disarmed after our ARM (an IDLE from the other port, say)."""
    link.check()
    st = link.latest
    if st is None:
        return
    if st.flags & (p.ST_FAULTS | p.ST_WATCHDOG | p.ST_REJECTED):
        raise LinkError("ESP32 disarmed: %s (servo %d)" % (", ".join(st.flag_names()), st.fault_servo))
    if mover is not None and mover.first_seq is not None and not st.armed:
        if (st.seq - mover.first_seq) & 0xFFFF < 0x8000:     # STATE after our ARM
            raise LinkError("ESP32 disarmed after our ARM (an IDLE or DISARM from another port?)")


def save_log(link: Link, path: str, pacer: Pacer | None, meta: dict) -> None:
    states = link.record or []
    arr = lambda f, dt: np.array([f(s) for s in states], dtype=dt)
    np.savez_compressed(
        path,
        t_rx=arr(lambda s: s.t_rx, float), tick=arr(lambda s: s.tick, np.uint32),
        t_us=arr(lambda s: s.t_us, np.uint32), seq=arr(lambda s: s.seq, np.uint16),
        flags=arr(lambda s: s.flags, np.uint16), boards=arr(lambda s: s.boards, np.uint8),
        pos_dd=np.array([s.pos_dd for s in states], dtype=np.uint16).reshape(-1, 12),
        cur_ma=np.array([s.cur_ma for s in states], dtype=np.int16).reshape(-1, 12),
        temp_dc=np.array([s.temp_dc for s in states], dtype=np.int16).reshape(-1, 12),
        acc_mg=np.array([s.acc_mg for s in states], dtype=np.int16).reshape(-1, 3),
        gyr_ddps=np.array([s.gyr_ddps for s in states], dtype=np.int16).reshape(-1, 3),
        imu_t_us=arr(lambda s: s.imu_t_us, np.uint32),
        sent_t=np.array([s[0] for s in link.sent], dtype=float),
        sent_seq=np.array([s[1] for s in link.sent], dtype=np.uint16),
        sent_pos_dd=np.array([np.frombuffer(s[2][10:34], dtype="<u2") for s in link.sent],
                             dtype=np.uint16).reshape(-1, 12),
        sent_enable=np.array([int.from_bytes(s[2][6:8], "little") for s in link.sent], dtype=np.uint16),
        loop_t=np.array(pacer.stamps if pacer else [], dtype=float),
        latency=np.array(link.latency, dtype=float),
        meta=json.dumps(meta),
    )
    print("log: %s (%d STATE, %d SET_ALL)" % (path, len(states), len(link.sent)))


def summary(link: Link, pacer: Pacer | None) -> None:
    states = link.record or list(link.states)
    print("STATE frames %d, tick gaps %d, CRC errors %d, bytes skipped %d"
          % (link.n_states, link.tick_gaps, link.parser.crc_errors, link.parser.skipped))
    if len(states) > 1:
        dt = states[-1].t_rx - states[0].t_rx
        print("STATE rate %.1f Hz" % ((len(states) - 1) / dt if dt > 0 else 0))
    if pacer and len(pacer.stamps) > 2:
        per = np.diff(pacer.stamps) * 1000
        print("loop period ms: p50 %.2f  p99 %.2f  max %.2f  (n %d)"
              % (np.percentile(per, 50), np.percentile(per, 99), per.max(), len(per)))
    if link.latency:
        lat = np.array(link.latency) * 1000
        print("SET_ALL -> STATE echo ms: p50 %.2f  p95 %.2f  max %.2f  (n %d)"
              % (np.percentile(lat, 50), np.percentile(lat, 95), lat.max(), len(lat)))
    if states:
        cur = np.abs(np.array([s.cur_ma for s in states], dtype=float))
        tmp = np.array([s.temp_dc for s in states], dtype=float) / 10
        flags = 0
        for s in states:
            flags |= s.flags
        print("max |I| mA:", " ".join("%d" % v for v in cur.max(0)))
        print("max temp C:", " ".join("%.1f" % v for v in tmp.max(0)))
        print("flags seen:", ", ".join(n for b, n in p.FLAG_NAMES.items() if flags & b) or "none")


# ---- commands -------------------------------------------------------------------
def cmd_ping(link: Link, a) -> None:
    link.record = []
    pacer = Pacer(0.02)
    end = time.monotonic() + a.seconds
    pos = [1550] * 12
    while time.monotonic() < end:
        link.set_all(pos, enable=0, flags=0)       # disarmed: only measures the echo
        pacer.wait()
    summary(link, pacer)
    st = link.latest
    if st is None:
        raise LinkError("no STATE received")
    acc = np.array([s.acc_mg for s in link.record], dtype=float) / 1000
    gyr = np.array([s.gyr_ddps for s in link.record], dtype=float) / 10
    print("IMU %s: acc mean %s g (|a| %.3f), gyro mean %s deg/s, std %s"
          % ("ok" if st.flags & p.ST_IMU_OK else "ABSENT", np.round(acc.mean(0), 3),
             np.linalg.norm(acc.mean(0)), np.round(gyr.mean(0), 2), np.round(gyr.std(0), 2)))
    boards = np.array([[s.boards >> b & 1 for b in range(4)] for s in link.record])
    print("fresh feedback per board (CN4, CN5, CN3, CN6): %s" % np.round(boards.mean(0), 3))
    for i in range(12):
        print("  %2d %-9s pos %4d dd (%6.1f deg)  I %5d mA  T %5.1f C"
              % (i + 1, NAMES[i], st.pos_dd[i], st.pos_dd[i] / 10, st.cur_ma[i], st.temp_dc[i] / 10))
    if a.log:
        save_log(link, a.log, pacer, {"cmd": "ping", "seconds": a.seconds})


def cmd_dump(link: Link, a) -> None:
    ids = parse_ids(a.ids)
    out = {}
    print("%-4s %-9s " % ("id", "") + " ".join("%10s" % n[:10] for n in p.PARAMS) + "  temp_C  supply_mA")
    for i in ids:
        row = {}
        for name in p.PARAMS:
            try:
                row[name] = link.get_param(i, name)
            except LinkError:
                row[name] = None
        for key, lid in (("temperature_c", p.LIVE_TEMPERATURE_C), ("supply_current_ma", p.LIVE_SUPPLY_CURRENT_MA)):
            try:
                row[key] = link.get_live(i, lid)
            except LinkError:
                row[key] = None
        out[i] = row
        fmt = lambda v: "%10s" % ("-" if v is None else "%.6g" % v)
        print("%-4d %-9s " % (i, NAMES[i - 1]) + " ".join(fmt(row[n]) for n in p.PARAMS)
              + "  %6s  %9s" % (fmt(row["temperature_c"]).strip(), fmt(row["supply_current_ma"]).strip()))
    if a.out:
        with open(a.out, "w") as f:
            json.dump({"time": time.strftime("%Y-%m-%d %H:%M:%S"), "servos": out}, f, indent=1)
        print("saved", a.out)


def cmd_config(link: Link, a) -> None:
    ids = parse_ids(a.ids)
    values = [("control_mode", a.mode), ("kp_position", a.kp), ("kd_position", a.kd),
              ("ki_position", 0.0), ("kp_current", 0.0), ("kff_current", a.kff),
              ("max_pwm_duty_cycle", a.max_pwm)]
    for i in ids:
        got = []
        for name, value in values:
            rb = link.set_param(i, name, value)
            if abs(rb - value) > 1e-6 + 1e-4 * abs(value):
                raise LinkError("servo %d %s: wrote %g, read back %g" % (i, name, value, rb))
            got.append("%s=%g" % (name, rb))
        print("%2d %-9s %s" % (i, NAMES[i - 1], " ".join(got)))
    print("written to RAM (not saved; a power cycle restores each servo's flash)")


def cmd_jog(link: Link, a) -> None:
    if abs(a.deg) > 10:
        raise SystemExit("jog is limited to 10 deg")
    i = a.id
    link.record = []
    start = present(link)
    preflight(link, a.clear)
    mover = Mover(link, None, mask([i]), a.cap, a.clear)
    pacer = Pacer(0.02)
    n = max(1, int(a.seconds / 0.02))
    peak_i, peak_move = 0, 0.0
    try:
        for phase in (+1, 0, -1):
            for k in range(n):
                frac = smoothstep((k + 1) / n) if phase == +1 else (1.0 if phase == 0 else 1 - smoothstep((k + 1) / n))
                tgt = start.copy()
                tgt[i - 1] = start[i - 1] + a.deg * 10 * frac
                mover.send(tgt)
                pacer.wait()
                check_flags(link, mover)
                st = link.latest
                peak_i = max(peak_i, abs(int(st.cur_ma[i - 1])))
                mv = (float(st.pos_dd[i - 1]) - start[i - 1]) / 10
                if abs(mv) > abs(peak_move):
                    peak_move = mv
    finally:
        link.disarm()
    print("servo %d (%s): commanded %+.1f deg, moved %+.1f deg (sign %s), peak |I| %d mA"
          % (i, NAMES[i - 1], a.deg, peak_move, "OK" if peak_move * a.deg > 0 else "CHECK", peak_i))
    if a.log:
        save_log(link, a.log, pacer, {"cmd": "jog", "id": i, "deg": a.deg, "cap": a.cap})


def ramp_to(mover: Mover, pacer: Pacer, link: Link, frm: np.ndarray, to: np.ndarray, seconds: float) -> None:
    n = max(1, int(seconds / pacer.period))
    for k in range(n):
        mover.send(frm + (to - frm) * smoothstep((k + 1) / n))
        pacer.wait()
        check_flags(link, mover)


def hold(mover: Mover, pacer: Pacer, link: Link, target: np.ndarray, seconds: float) -> None:
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        mover.send(target)
        pacer.wait()
        check_flags(link, mover)


def cmd_stand(link: Link, a) -> None:
    stand = load_stand(a.stand, link)
    link.record = []
    start = present(link)
    preflight(link, a.clear)
    mover = Mover(link, None, 0x0FFF, a.cap, a.clear)
    pacer = Pacer(0.02)
    try:
        ramp_to(mover, pacer, link, start, stand, a.seconds)
        mover.stand = stand
        hold(mover, pacer, link, stand, a.hold)
    finally:
        link.disarm()
    err = (link.latest.pos_dd.astype(float) - stand) / 10
    summary(link, pacer)
    print("final error deg:", " ".join("%+.1f" % e for e in err))
    if a.log:
        save_log(link, a.log, pacer, {"cmd": "stand", "stand": stand.tolist(), "cap": a.cap})


def cmd_sine(link: Link, a) -> None:
    stand = load_stand(a.stand, link)
    ids = parse_ids(a.ids)
    link.record = []
    start = present(link)
    preflight(link, a.clear)
    mover = Mover(link, None, 0x0FFF, a.cap, a.clear)
    pacer = Pacer(0.02)
    try:
        ramp_to(mover, pacer, link, start, stand, 2.0)
        mover.stand = stand
        hold(mover, pacer, link, stand, 1.0)
        t0 = time.monotonic()
        while time.monotonic() - t0 < a.seconds:
            t = time.monotonic() - t0
            tgt = stand.copy()
            for i in ids:
                tgt[i - 1] += a.amp * 10 * math.sin(2 * math.pi * a.freq * t)
            mover.send(tgt)
            pacer.wait()
            check_flags(link, mover)
        hold(mover, pacer, link, stand, 1.0)
    finally:
        link.disarm()
    summary(link, pacer)
    if a.log:
        save_log(link, a.log, pacer, {"cmd": "sine", "ids": ids, "amp": a.amp, "freq": a.freq,
                                      "stand": stand.tolist(), "cap": a.cap})


def cmd_stream(link: Link, a) -> None:
    stand = load_stand(a.stand, link)
    data = np.load(a.file)
    delta = np.asarray(data["delta_dd"], dtype=float)
    period = float(data["period_s"])
    link.record = []
    start = present(link)
    preflight(link, a.clear)
    mover = Mover(link, None, 0x0FFF, a.cap, a.clear)
    pacer = Pacer(0.02)
    first = clip_band(stand, stand + delta[0])
    try:
        # the ramp starts wherever the legs are, so only its end is clipped
        ramp_to(mover, pacer, link, start, first, 2.0)
        mover.stand = stand
        hold(mover, pacer, link, first, 1.0)
        pacer.period = period
        for _ in range(a.repeat):
            for row in delta:
                mover.send(stand + row)
                pacer.wait()
                check_flags(link, mover)
        pacer.period = 0.02
        ramp_to(mover, pacer, link, mover.last, stand, 1.0)
        hold(mover, pacer, link, stand, 1.0)
    finally:
        link.disarm()
    summary(link, pacer)
    print("targets clipped on %d sends" % mover.clipped)
    if a.log:
        save_log(link, a.log, pacer, {"cmd": "stream", "file": a.file, "period_s": period,
                                      "repeat": a.repeat, "stand": stand.tolist(), "cap": a.cap})


# ---- calibration --------------------------------------------------------------
def cmd_offsets(link: Link, a) -> None:
    offs, stored = read_offsets(link)
    ticks = offset_ticks(offs)
    dd = ticks_to_dd(ticks)
    for i in range(12):
        off = ("%+6.1f deg" % offs[i]) if stored[i] else "     unset"
        print("%2d %-9s offset %s   stand code %4d   %4d dd" % (i + 1, NAMES[i], off, ticks[i], dd[i]))
    if a.json:
        with open(a.json, "w") as f:
            json.dump({"stand_dd": dd.tolist(), "stand_ticks": ticks.tolist(), "offsets_deg": offs.tolist(),
                       "offsets_stored": stored,
                       "source": "NVS parameter:offsetN, read %s; code = 511 + (int)(offset*10240/3100), "
                                 "dd = (uint16)(3100 - code*3100/1024)" % time.strftime("%Y-%m-%d %H:%M:%S")},
                      f, indent=1)
        print("stand file:", a.json)


def _status(link: Link, ids) -> str:
    st = link.latest
    k = np.array(ids) - 1
    return "max |I| %d mA, max housing %.1f C" % (np.abs(st.cur_ma[k]).max(), st.temp_dc[k].max() / 10.0)


def cmd_centre(link: Link, a) -> None:
    ids = parse_ids(a.ids)
    require_config(link, ids)
    start = present(link)
    preflight(link, a.clear)
    target = start.copy()
    target[np.array(ids) - 1] = ticks_to_dd([CENTRE_TICK])[0]
    mover = Mover(link, None, mask(ids), a.cap, a.clear)
    pacer = Pacer(0.02)
    print("servos %s to the centre: code %d = %d dd, offsets ignored. Ctrl-C ends (all idle)."
          % (a.ids, CENTRE_TICK, ticks_to_dd([CENTRE_TICK])[0]))
    try:
        ramp_to(mover, pacer, link, start, target, 2.0)
        end, last = time.monotonic() + a.seconds, 0.0
        while time.monotonic() < end:
            mover.send(target)
            pacer.wait()
            check_flags(link, mover)
            if time.monotonic() - last > 5.0:
                last = time.monotonic()
                print("  holding, %s, %.0f s left" % (_status(link, ids), end - last))
    except KeyboardInterrupt:
        pass
    finally:
        link.disarm()
    print("idle")


def _keys(fd):
    """Characters typed since the last call (non-blocking)."""
    out = ""
    while select.select([fd], [], [], 0)[0]:
        out += sys.stdin.read(1)
    return out


def cmd_calib(link: Link, a) -> None:
    offs, stored = read_offsets(link)
    if a.zero:
        offs[np.array(parse_ids(a.zero)) - 1] = 0.0       # re-zeroed joints start from 0
    start_offs = offs.copy()
    for item in a.set or []:
        sid, deg = item.split("=")
        offs[int(sid) - 1] = float(deg)
    if np.any(np.abs(offs) > OFFSET_MAX_DEG):
        raise SystemExit("offsets are limited to +-%g deg" % OFFSET_MAX_DEG)
    interactive = a.seconds is None
    if interactive and not sys.stdin.isatty():
        raise SystemExit("calib needs a terminal (ssh -t), or --seconds for a timed hold")
    require_config(link, range(1, 13))
    pose = lambda: ticks_to_dd(offset_ticks(offs)).astype(float)
    start = present(link)
    preflight(link, a.clear)
    mover = Mover(link, None, 0x0FFF, a.cap, a.clear)
    pacer = Pacer(0.02)
    save = a.save
    fd = sys.stdin.fileno() if interactive else None
    old = termios.tcgetattr(fd) if interactive else None
    try:
        ramp_to(mover, pacer, link, start, pose(), 2.0)
        if not interactive:
            hold(mover, pacer, link, pose(), a.seconds)
        else:
            tty.setcbreak(fd)
            sel, step = 0, a.step
            print("Ini pose held (code 511 + offset). Keys: ] / [ next / previous servo, + / - step,\n"
                  "f fine (one code step, %.2f deg) / coarse, r restore this servo, s save to NVS and quit,\n"
                  "q quit without saving (Ctrl-C too)." % ONE_TICK_DEG)

            def show():
                print("  servo %2d %-9s offset %+6.2f deg   stand code %4d   step %.2f deg   [%s]"
                      % (sel + 1, NAMES[sel], offs[sel], offset_ticks(offs)[sel], step, _status(link, range(1, 13))))
            show()
            done = False
            while not done:
                for c in _keys(fd):
                    if c in "]n":
                        sel = (sel + 1) % 12
                    elif c in "[p":
                        sel = (sel - 1) % 12
                    elif c in "+=":
                        offs[sel] = min(offs[sel] + step, OFFSET_MAX_DEG)
                    elif c in "-_":
                        offs[sel] = max(offs[sel] - step, -OFFSET_MAX_DEG)
                    elif c == "f":
                        step = ONE_TICK_DEG if step != ONE_TICK_DEG else a.step
                    elif c == "r":
                        offs[sel] = start_offs[sel]
                    elif c == "s":
                        save, done = True, True
                    elif c == "q":
                        save, done = False, True
                    else:
                        continue
                    show()
                    if done:
                        break
                mover.send(pose())
                pacer.wait()
                check_flags(link, mover)
    except KeyboardInterrupt:
        save = False if interactive else save
    finally:
        if interactive:
            termios.tcsetattr(fd, termios.TCSADRAIN, old)
        link.disarm()
    for i in range(12):
        flag = "" if offs[i] == start_offs[i] else "   (was %+.2f)" % start_offs[i]
        print("%2d %-9s offset %+6.2f deg%s" % (i + 1, NAMES[i], offs[i], flag))
    if not save:
        print("not saved: NVS keeps the previous offsets")
        return
    t_end = time.monotonic() + 1.0                       # offsets are refused while armed
    while link.wait_state(0.5).armed and time.monotonic() < t_end:
        pass
    for i in range(12):
        link.set_offset(i + 1, float(offs[i]))
    n = link.save_offsets()
    back, _ = read_offsets(link)
    if not np.allclose(back, offs.astype(np.float32), atol=1e-4):
        raise LinkError("read-back differs: %s" % back.tolist())
    print("saved %d offsets to NVS and read them back" % n)


def cmd_idle(link: Link, a) -> None:
    link.disarm()
    st = link.wait_state(0.5)
    print("flags:", ", ".join(st.flag_names()) or "none")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="mp4link", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=DEFAULT_PORT, help="serial port (default %(default)s)")
    ap.add_argument("--baud", type=int, default=DEFAULT_BAUD, help="default %(default)s")
    sub = ap.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("ping", help="link check: STATE rate, CRC, IMU, latency (servos stay idle)")
    s.add_argument("--seconds", type=float, default=10.0, help="default %(default)s")
    s.add_argument("--log", help="save the STATE stream and the frames sent to this .npz")
    s = sub.add_parser("dump", help="read every AT32 parameter (read-only)")
    s.add_argument("--ids", default="1-12")
    s.add_argument("--out")
    s = sub.add_parser("config", help="write mode and gains to the AT32s' RAM and read them back")
    s.add_argument("--ids", default="1-12")
    s.add_argument("--mode", type=float, default=1)
    s.add_argument("--kp", type=float, default=80)
    s.add_argument("--kd", type=float, default=400)
    s.add_argument("--kff", type=float, default=0.001)
    s.add_argument("--max-pwm", type=float, default=0.99)
    s = sub.add_parser("jog", help="move one servo by a few degrees and back")
    s.add_argument("--id", type=int, required=True)
    s.add_argument("--deg", type=float, default=5.0)
    s.add_argument("--cap", type=int, default=300, help="frame cap mA (control_mode 1: duty <= kff x cap)")
    s.add_argument("--clear", action="store_true", help="clear a latched fault before arming")
    s.add_argument("--seconds", type=float, default=1.0)
    s.add_argument("--log")
    for name, hlp in (("stand", "ramp every servo to the stand pose and hold"),
                      ("sine", "sines about the stand pose"),
                      ("stream", "play a target file about the stand pose")):
        s = sub.add_parser(name, help=hlp)
        s.add_argument("--stand", required=True,
                       help="JSON with stand_dd (12 values), or 'nvs' for the robot's calibration")
        s.add_argument("--cap", type=int, default=600,
                       help="frame cap mA; in control_mode 1 a duty cap of kff x cap (0.6 at kff 0.001)")
        s.add_argument("--clear", action="store_true", help="clear a latched fault before arming")
        s.add_argument("--log")
        if name == "stand":
            s.add_argument("--seconds", type=float, default=2.0)
            s.add_argument("--hold", type=float, default=5.0)
        if name == "sine":
            s.add_argument("--ids", required=True)
            s.add_argument("--amp", type=float, default=5.0)
            s.add_argument("--freq", type=float, default=0.5)
            s.add_argument("--seconds", type=float, default=10.0)
        if name == "stream":
            s.add_argument("file", help="npz with delta_dd (N x 12) and period_s")
            s.add_argument("--repeat", type=int, default=1)
    s = sub.add_parser("offsets", help="read the calibration offsets from the robot's NVS")
    s.add_argument("--json", help="also write them as a stand file for --stand")
    s = sub.add_parser("centre", help="hold servos at the centre (code 511, offsets ignored) to mount the legs")
    s.add_argument("--ids", default="1-12")
    s.add_argument("--cap", type=int, default=300, help="frame cap mA (control_mode 1: duty <= kff x cap)")
    s.add_argument("--seconds", type=float, default=600.0)
    s.add_argument("--clear", action="store_true", help="clear a latched fault before arming")
    s = sub.add_parser("calib", help="hold the Ini pose, adjust the offsets, save them to NVS")
    s.add_argument("--step", type=float, default=1.0, help="deg per key press (f: one code step)")
    s.add_argument("--cap", type=int, default=300, help="frame cap mA (control_mode 1: duty <= kff x cap)")
    s.add_argument("--zero", metavar="IDS", help="start these servos from offset 0 (after `centre` and remounting)")
    s.add_argument("--set", action="append", metavar="ID=DEG", help="start from this offset (repeatable)")
    s.add_argument("--seconds", type=float, help="no keys: hold this long, then stop")
    s.add_argument("--save", action="store_true", help="with --seconds: save the offsets to NVS after")
    s.add_argument("--clear", action="store_true", help="clear a latched fault before arming")
    sub.add_parser("idle", help="disarm: every servo IDLE")

    a = ap.parse_args(argv)
    try:
        link = Link(a.port, a.baud)
    except serial.SerialException as exc:
        print("error: cannot open %s (%s); see tools/pi/README.md, Enable the UART, or use --port"
              % (a.port, exc), file=sys.stderr)
        return 1
    try:
        {"ping": cmd_ping, "dump": cmd_dump, "config": cmd_config, "jog": cmd_jog,
         "stand": cmd_stand, "sine": cmd_sine, "stream": cmd_stream, "idle": cmd_idle,
         "offsets": cmd_offsets, "centre": cmd_centre, "calib": cmd_calib}[a.cmd](link, a)
    except LinkError as exc:
        print("error:", exc, file=sys.stderr)
        return 1
    finally:
        link.close()
    return 0
