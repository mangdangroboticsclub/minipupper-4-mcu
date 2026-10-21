"""A fake ESP32 on a pty speaking the mp4link frames (docs/PROTOCOL.md), for the host-side tests.

By default it streams STATE from the start, as this branch's firmware does.
With ``session_release_s`` set it serves the link in sessions, as the ``main``
branch's firmware does: frames are ignored until a valid one opens a session
(a request with cmd <= 14 and servo <= 12, or a SET_ALL with only known flag
and enable bits); STATE is sent only inside a session; the first responses of
a session wait ``session_wait_s``; a session ends after ``session_release_s``
without a frame from the host, which disarms and restores the servo
parameters saved when it began; GET_OFFSET reports an unset offset as 0.
"""

from __future__ import annotations

import os
import select
import struct
import threading
import time
import tty

from mp4link import protocol as p


class FakeEsp32:
    def __init__(self, tick_s: float = 0.005, watchdog_s: float = 0.1,
                 session_release_s: float | None = None, session_wait_s: float = 0.0):
        self.master, slave = os.openpty()
        tty.setraw(slave)
        self.port = os.ttyname(slave)
        self._slave = slave
        self.tick_s, self.watchdog_s = tick_s, watchdog_s
        self.parser = p.Parser(accept=(p.MAGIC_REQ, p.MAGIC_SET_ALL))
        self.lock = threading.Lock()
        self.armed = False
        self.latched = 0
        self.fault_servo = 0
        self.seq = 0
        self.enable = 0
        self.cap = 0
        self.target = [1550.0] * 12
        self.pos = [1500.0 + 10 * i for i in range(12)]
        self.last_set_all = 0.0
        self.params = {i: {pid: 0.0 for pid in range(len(p.PARAMS))} for i in range(1, 13)}
        self.nvs: dict[int, float] = {}                 # saved offsets (NVS)
        self.offsets: dict[int, float] = {}             # RAM table, loaded from NVS at boot
        self.armed_seen: list[tuple[int, list[float]]] = []   # (enable, targets) while armed
        self.caps_armed: set[int] = set()
        self.tick = 0
        self.pings = 0
        self.session_release_s, self.session_wait_s = session_release_s, session_wait_s
        self.session = session_release_s is None      # always "open" without session mode
        self.sessions = 0                               # sessions opened
        self.last_frame = 0.0
        self.ready_at = 0.0                             # session mode: responses wait until then
        self._saved_params: dict | None = None
        self._pending: list[bytes] = []                 # responses held until ready_at
        self._run = True
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()

    def close(self) -> None:
        self._run = False
        self._thread.join(timeout=1.0)
        os.close(self.master)
        os.close(self._slave)

    # ---- frames ----------------------------------------------------------
    def _respond(self, cmd, servo, status, val=0.0) -> None:
        pos = int(self.pos[servo - 1]) if 1 <= servo <= 12 else 0
        body = p.RSP.pack(p.MAGIC_RSP, cmd, servo, status, pos, 0, val)
        frame = body + struct.pack("<H", p.crc16(body))
        if self._pending or time.monotonic() < self.ready_at:
            self._pending.append(frame)
        else:
            os.write(self.master, frame)

    def _request(self, frame: bytes) -> None:
        _, cmd, servo, pad, a1, a2 = p.REQ.unpack(frame[:8])
        ok_servo = 1 <= servo <= 12
        if cmd == p.CMD_IDLE:
            self.armed = False
            return self._respond(cmd, servo, 0)
        if cmd == p.CMD_PING:
            self.pings += 1
            return self._respond(cmd, servo, 0)
        if cmd in (p.CMD_SET_POS, p.CMD_TORQUE) or self.armed:
            return self._respond(cmd, servo, 6)
        if cmd == p.CMD_SAVE_OFFSETS:
            self.nvs.update(self.offsets)
            return self._respond(cmd, servo, 0, float(len(self.offsets)))
        if not ok_servo:
            return self._respond(cmd, servo, 1)
        value = struct.unpack("<f", struct.pack("<I", a1 | a2 << 16))[0]
        if cmd == p.CMD_GET_PARAM:
            return self._respond(cmd, servo, 0, self.params[servo][a1])
        if cmd == p.CMD_SET_PARAM:
            self.params[servo][pad] = value
            return self._respond(cmd, servo, 0, self.params[servo][pad])
        if cmd == p.CMD_GET_OFFSET:
            if servo not in self.offsets and self.session_release_s is None:
                return self._respond(cmd, servo, p.STATUS_UNSET, 0.0)
            return self._respond(cmd, servo, 0, self.offsets.get(servo, 0.0))
        if cmd == p.CMD_SET_OFFSET:
            if not -45.0 <= value <= 45.0:
                return self._respond(cmd, servo, 2)
            self.offsets[servo] = value
            return self._respond(cmd, servo, 0, value)
        if cmd == p.CMD_GET_LIVE:
            return self._respond(cmd, servo, 0 if a1 == 12 else 3, 31.5)
        return self._respond(cmd, servo, 5)

    def _set_all(self, frame: bytes) -> None:
        v = p.SET_ALL.unpack(frame[:34])
        flags = v[3]
        self.seq, self.enable, self.cap = v[2], v[4], v[5]
        self.target = [float(x) for x in v[6:18]]
        self.last_set_all = time.monotonic()
        if flags & p.SA_CLEAR:
            self.latched, self.fault_servo = 0, 0
        if flags & p.SA_DISARM:
            self.armed = False
        elif flags & p.SA_ARM:
            if self.latched & p.ST_FAULTS:
                self.latched |= p.ST_REJECTED
            else:
                self.armed = True
                self.latched &= ~(p.ST_WATCHDOG | p.ST_REJECTED)

    def _state(self) -> bytes:
        flags = self.latched | (p.ST_ARMED if self.armed else 0) | p.ST_IMU_OK
        body = p.STATE.pack(p.MAGIC_STATE, 1, self.seq, self.tick, int(time.monotonic() * 1e6) & 0xFFFFFFFF,
                            flags, self.fault_servo, 0x0F,
                            *[int(x) for x in self.pos], *[0] * 12, *[315] * 12,
                            0, 0, 1000, 0, 0, 0, 0)
        return body + struct.pack("<H", p.crc16(body))

    # ---- sessions --------------------------------------------------------
    def _accept(self, magic: int, frame: bytes) -> bool:
        """Session mode: note a frame; False drops it (no session, not an opener)."""
        if self.session_release_s is None:
            return True
        if not self.session:
            if magic == p.MAGIC_REQ:
                _, cmd, servo = frame[:3]
                if cmd > p.CMD_SAVE_OFFSETS or servo > 12:
                    return False
            else:
                v = p.SET_ALL.unpack(frame[:34])
                if v[3] & ~0x7 or v[4] & ~0x0FFF:
                    return False
            self.session = True
            self.sessions += 1
            self.ready_at = time.monotonic() + self.session_wait_s
            self._saved_params = {i: dict(d) for i, d in self.params.items()}
        self.last_frame = time.monotonic()
        return True

    def _session_tick(self, now: float) -> None:
        if self._pending and now >= self.ready_at:
            for frame in self._pending:
                os.write(self.master, frame)
            self._pending.clear()
        if (self.session_release_s is not None and self.session
                and now - self.last_frame > self.session_release_s):
            self.session = False
            self.armed = False
            self.params = self._saved_params

    # ---- loop ------------------------------------------------------------
    def _loop(self) -> None:
        next_t = time.monotonic()
        while self._run:
            r, _, _ = select.select([self.master], [], [], max(0.0, next_t - time.monotonic()))
            if r:
                try:
                    data = os.read(self.master, 4096)
                except OSError:
                    return
                for magic, frame in self.parser.feed(data):
                    with self.lock:
                        if not self._accept(magic, frame):
                            continue
                        (self._request if magic == p.MAGIC_REQ else self._set_all)(frame)
            if time.monotonic() >= next_t:
                next_t += self.tick_s
                with self.lock:
                    now = time.monotonic()
                    self._session_tick(now)
                    if not self.session:
                        continue
                    if self.armed and now - self.last_set_all > self.watchdog_s:
                        self.armed = False
                        self.latched |= p.ST_WATCHDOG
                    if self.armed:
                        self.armed_seen.append((self.enable, list(self.target)))
                        self.caps_armed.add(self.cap)
                        for i in range(12):
                            if self.enable >> i & 1:
                                self.pos[i] += 0.5 * (self.target[i] - self.pos[i])
                    try:
                        os.write(self.master, self._state())
                    except OSError:
                        return
                    self.tick += 1
