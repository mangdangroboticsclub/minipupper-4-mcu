"""The serial link to the ESP32: a reader thread, requests, SET_ALL, keepalive."""

from __future__ import annotations

import collections
import queue
import threading
import time

from . import protocol as p

DEFAULT_PORT = "/dev/ttyAMA0"
DEFAULT_BAUD = 921_600
# The `main` branch's firmware serves the link in sessions: it starts STATE
# only after the host's first valid frame, may take up to ~1.5 s to answer the
# first request of a session, and ends the session (restoring its own servo
# gains) after 2 s without a frame from the host. The opener and the keepalive
# cover that; this branch's firmware streams STATE from boot and answers them
# at once.
OPEN_TIMEOUT = 2.0          # the opening PING, and a request while STATE is absent
KEEPALIVE_S = 0.5           # PING when nothing has been written for this long
KEEPALIVE_TIMEOUT = 0.3
STATE_STALE_S = 1.0         # no STATE for this long: the session may need opening


class LinkError(IOError):
    pass


class Link:
    """One serial port to the ESP32.

    A reader thread parses every frame: ``STATE`` frames update ``latest``
    and the ``states`` ring (and ``record`` when it is a list); responses
    answer :meth:`request`. ``ser`` may be any object with ``read``,
    ``write`` and ``in_waiting`` (tests pass a pty).

    On opening, one PING to servo 0 opens a session on the ``main`` branch's
    firmware; a keepalive thread then sends a PING whenever nothing has been
    written for ``keepalive`` seconds (None turns it off). The keepalive never
    sends SET_ALL, so it cannot change targets or idle armed servos, and it
    does not feed the firmware's SET_ALL watchdog.
    """

    def __init__(self, port: str = DEFAULT_PORT, baud: int = DEFAULT_BAUD,
                 ser=None, keep: int = 2000, keepalive: float | None = KEEPALIVE_S):
        if ser is None:
            import serial
            # Opened with DTR and RTS released: on a CH340 port (USB) toggling
            # them resets the ESP32. The Pi's UART has no modem lines.
            ser = serial.Serial()
            ser.port, ser.baudrate, ser.timeout, ser.exclusive = port, baud, 0.01, True
            ser.dtr = False
            ser.rts = False
            ser.open()
            ser.reset_input_buffer()
        self.ser = ser
        self.parser = p.Parser()
        self.latest: p.State | None = None
        self.states: collections.deque = collections.deque(maxlen=keep)
        self.record: list | None = None
        self.n_states = 0
        self.tick_gaps = 0          # STATE ticks missed between consecutive frames
        self.latency: collections.deque = collections.deque(maxlen=20000)  # SET_ALL -> STATE echo [s]
        self.error: BaseException | None = None   # what stopped the reader thread
        self.sent: list[tuple[float, int, bytes]] = []   # (time, seq, frame) when recording
        self.keepalives = 0         # keepalive PINGs sent
        self._seq = 0
        self._sent_at: dict[int, float] = {}
        self._responses: queue.Queue = queue.Queue()
        self._cond = threading.Condition()
        self._write_lock = threading.Lock()
        self._req_lock = threading.Lock()     # one request in flight, keepalive included
        self._last_write = time.monotonic()
        self._run = True
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._reader, name="mp4link-rx", daemon=True)
        self._thread.start()
        self._open()
        self._keepalive_thread = None
        if keepalive:
            self._keepalive_thread = threading.Thread(target=self._keepalive, args=(keepalive,),
                                                      name="mp4link-keepalive", daemon=True)
            self._keepalive_thread.start()

    # ---- session ---------------------------------------------------------
    def _open(self) -> None:
        """PING servo 0, then give STATE a moment to start. Never raises:
        wait_state reports a link that stays silent."""
        deadline = time.monotonic() + OPEN_TIMEOUT
        try:
            self.request(p.CMD_PING, 0, timeout=OPEN_TIMEOUT, check=False, retries=1)
        except LinkError:
            pass
        with self._cond:
            while self.latest is None and self.error is None:
                left = deadline - time.monotonic()
                if left <= 0:
                    break
                self._cond.wait(left)

    def _keepalive(self, period: float) -> None:
        while not self._stop.wait(min(0.05, period / 4)):
            if time.monotonic() - self._last_write < period:
                continue
            # A request in flight writes anyway; never wait behind one.
            if not self._req_lock.acquire(blocking=False):
                continue
            try:
                self.keepalives += 1
                self._transact(p.CMD_PING, 0, p.pack_request(p.CMD_PING, 0), (KEEPALIVE_TIMEOUT,))
            except Exception:
                pass
            finally:
                self._req_lock.release()

    def _state_fresh(self) -> bool:
        st = self.latest
        return st is not None and time.monotonic() - st.t_rx < STATE_STALE_S

    # ---- receive ---------------------------------------------------------
    def _reader(self) -> None:
        try:
            while self._run:
                data = self.ser.read(max(1, getattr(self.ser, "in_waiting", 0) or 1))
                if not data:
                    continue
                t = time.monotonic()
                for magic, frame in self.parser.feed(data):
                    if magic == p.MAGIC_STATE:
                        self._on_state(p.parse_state(frame, t))
                    elif magic == p.MAGIC_RSP:
                        self._responses.put(p.parse_response(frame))
        except Exception as exc:     # surfaced by wait_state / check
            if self._run:
                self.error = exc
                with self._cond:
                    self._cond.notify_all()

    def check(self) -> None:
        """Raise if the reader thread has stopped."""
        if self.error is not None:
            raise LinkError("serial reader stopped: %r" % (self.error,))

    def _on_state(self, st: p.State) -> None:
        with self._cond:
            if self.latest is not None:
                gap = (st.tick - self.latest.tick - 1) & 0xFFFFFFFF
                if gap < 1000:
                    self.tick_gaps += gap
            self.latest = st
            self.n_states += 1
            self.states.append(st)
            if self.record is not None:
                self.record.append(st)
            sent = self._sent_at.pop(st.seq, None)
            if sent is not None:
                self.latency.append(st.t_rx - sent)
            self._cond.notify_all()

    def wait_state(self, timeout: float = 0.5, after: p.State | None = None) -> p.State:
        """The next STATE newer than ``after`` (default: the current one)."""
        deadline = time.monotonic() + timeout
        with self._cond:
            ref = after if after is not None else self.latest
            while self.latest is None or (ref is not None and self.latest.tick == ref.tick):
                self.check()
                left = deadline - time.monotonic()
                if left <= 0:
                    raise LinkError("no STATE from the ESP32 (wiring, baud, firmware?)")
                self._cond.wait(left)
            return self.latest

    # ---- send ------------------------------------------------------------
    def _write(self, frame: bytes) -> None:
        with self._write_lock:
            self.ser.write(frame)
            self._last_write = time.monotonic()

    def _transact(self, cmd: int, servo: int, frame: bytes, timeouts) -> p.Response | None:
        """Send ``frame`` once per timeout until a response to (cmd, servo)
        arrives. Caller holds ``_req_lock``, so every response in the queue
        belongs to this exchange or to an earlier one that timed out; the
        latter are dropped by the cmd/servo match or the drain before each send."""
        for timeout in timeouts:
            while not self._responses.empty():
                self._responses.get_nowait()
            self._write(frame)
            deadline = time.monotonic() + timeout
            while True:
                try:
                    rsp = self._responses.get(timeout=max(0.0, deadline - time.monotonic()))
                except queue.Empty:
                    break
                if rsp.cmd == cmd and rsp.servo == servo:
                    return rsp
        return None

    def request(self, cmd: int, servo: int = 0, arg1: int = 0, arg2: int = 0,
                pad: int = 0, timeout: float = 0.3, check: bool = True,
                retries: int = 2) -> p.Response:
        """One per-servo request, sent up to ``retries`` times on a timeout.

        While no STATE is arriving (a session that is not open yet) the first
        attempt waits up to OPEN_TIMEOUT: opening one can take ~1.5 s."""
        frame = p.pack_request(cmd, servo, arg1, arg2, pad)
        timeouts = [timeout] * max(1, retries)
        with self._req_lock:
            if not self._state_fresh():
                timeouts[0] = max(timeout, OPEN_TIMEOUT)
            rsp = self._transact(cmd, servo, frame, timeouts)
        if rsp is None:
            raise LinkError("no response to cmd %d servo %d" % (cmd, servo))
        if check and not rsp.ok:
            raise LinkError("servo %d cmd %d: %s" % (servo, cmd, p.STATUS.get(rsp.status, rsp.status)))
        return rsp

    def get_param(self, servo: int, name_or_id) -> float:
        pid = p.PARAM_ID[name_or_id] if isinstance(name_or_id, str) else int(name_or_id)
        return self.request(p.CMD_GET_PARAM, servo, pid).val

    def set_param(self, servo: int, name_or_id, value: float) -> float:
        """Write a parameter to the AT32's RAM; returns the read-back."""
        pid = p.PARAM_ID[name_or_id] if isinstance(name_or_id, str) else int(name_or_id)
        a1, a2 = p.float_args(value)
        return self.request(p.CMD_SET_PARAM, servo, a1, a2, pad=pid).val

    def get_live(self, servo: int, live_id: int) -> float:
        return self.request(p.CMD_GET_LIVE, servo, live_id).val

    # ---- calibration offsets (the robot's NVS, degrees) -------------------
    def get_offset(self, servo: int) -> float | None:
        """The stored offset, or None when the servo has none (the ``main``
        branch's firmware reports an unset offset as 0)."""
        rsp = self.request(p.CMD_GET_OFFSET, servo, check=False)
        if rsp.status == p.STATUS_UNSET:
            return None
        if not rsp.ok:
            raise LinkError("servo %d offset: %s" % (servo, p.STATUS.get(rsp.status, rsp.status)))
        return rsp.val

    def set_offset(self, servo: int, value: float) -> float:
        """Into the ESP32's RAM; save_offsets() writes NVS."""
        a1, a2 = p.float_args(value)
        return self.request(p.CMD_SET_OFFSET, servo, a1, a2).val

    def save_offsets(self) -> int:
        """Write every set offset to NVS; returns how many."""
        return int(round(self.request(p.CMD_SAVE_OFFSETS, 0).val))

    def set_all(self, pos_dd, enable: int = 0x0FFF, flags: int = 0, cur_cap_ma: int = 1500) -> int:
        """Send twelve targets; returns the frame's seq."""
        self._seq = (self._seq + 1) & 0xFFFF
        frame = p.pack_set_all(self._seq, pos_dd, enable, flags, cur_cap_ma)
        t = time.monotonic()
        with self._cond:
            self._sent_at[self._seq] = t
            if len(self._sent_at) > 256:
                self._sent_at.pop(next(iter(self._sent_at)))
        self._write(frame)
        if self.record is not None:
            self.sent.append((t, self._seq, frame))
        return self._seq

    def disarm(self) -> None:
        """Every servo IDLE: a DISARM frame, then an IDLE request as a second path."""
        pos = self.latest.pos_dd if self.latest is not None else [1550] * 12
        self.set_all(pos, enable=0, flags=p.SA_DISARM, cur_cap_ma=0)
        try:
            self.request(p.CMD_IDLE, 1, check=False, timeout=0.2)
        except LinkError:
            pass

    def close(self) -> None:
        self._stop.set()
        if self._keepalive_thread is not None:
            self._keepalive_thread.join(timeout=1.0)
        self._run = False
        self._thread.join(timeout=1.0)
        try:
            self.ser.close()
        except Exception:
            pass
