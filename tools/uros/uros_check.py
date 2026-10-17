#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Phase-0 link check for the micro-ROS node (main/ros): how well does ROS 2 over Wi-Fi work?

Run it on the PC (WSL2) next to the micro-ROS agent, with the robot connected
(`uros status` on the robot says "connected"):

    python3 uros_check.py              # 30 s
    python3 uros_check.py --seconds 120 --ping-hz 50

It measures, from the robot's own topics:
  * imu/data_raw and joint_states: rate, the longest gap, and how stale a
    message is when it arrives (needs the robot's clock synced to the agent,
    which the robot does by itself);
  * uros/ping -> uros/pong: the round trip PC -> robot -> PC and the share of
    pings that never came back.
and prints a verdict for each. Nothing on the robot moves.
"""
import argparse
import math
import statistics
import threading
import time

import rclpy
from rclpy.executors import SingleThreadedExecutor
from rclpy.node import Node
from rclpy.qos import HistoryPolicy, QoSProfile, ReliabilityPolicy
from sensor_msgs.msg import Imu, JointState
from std_msgs.msg import Header

BEST_EFFORT = QoSProfile(reliability=ReliabilityPolicy.BEST_EFFORT,
                         history=HistoryPolicy.KEEP_LAST, depth=50)


def pct(values, p):
    if not values:
        return float('nan')
    v = sorted(values)
    return v[min(len(v) - 1, int(round(p / 100.0 * (len(v) - 1))))]


class Stream:
    """Arrival statistics of one topic."""

    GAP_S = 0.2   # a silence longer than this is listed in the report

    def __init__(self, name):
        self.name = name
        self.n = 0
        self.first = None
        self.last = None
        self.max_gap = 0.0
        self.gaps = []          # (seconds into the test, length s)
        self.age_ms = []
        self.t0 = None          # start of the measured window

    def arrived(self, now, stamp_s, wall):
        if self.t0 is None:
            return               # before the measured window
        if self.last is not None:
            gap = now - self.last
            self.max_gap = max(self.max_gap, gap)
            if gap > self.GAP_S:
                self.gaps.append((self.last - self.t0, gap))
        if self.first is None:
            self.first = now
        self.last = now
        self.n += 1
        if stamp_s > 1e9:                      # an epoch time: the clock is synced
            self.age_ms.append((wall - stamp_s) * 1000.0)

    def close(self, t_end):
        """A silence still going on at the end of the test counts too."""
        if self.last is not None and t_end - self.last > self.GAP_S:
            gap = t_end - self.last
            self.max_gap = max(self.max_gap, gap)
            self.gaps.append((self.last - self.t0, gap))

    def rate(self, now=None):
        """Messages per second over the measured window so far (a silence
        lowers it, it does not hide in it)."""
        if self.t0 is None or self.n == 0:
            return 0.0
        span = (now or time.monotonic()) - self.t0
        return self.n / span if span > 0 else 0.0


class Check(Node):

    def __init__(self, ping_hz):
        super().__init__('uros_check')
        self.imu = Stream('imu/data_raw')
        self.js = Stream('joint_states')
        self.last_imu = None
        self.last_js = None
        self.create_subscription(Imu, 'imu/data_raw', self.on_imu, BEST_EFFORT)
        self.create_subscription(JointState, 'joint_states', self.on_js, BEST_EFFORT)
        # The ping has a node (and a thread) of its own: on the topics' node a
        # pong would wait behind every IMU and joint message ahead of it, and
        # the ping timer would starve (Python handles ~150 callbacks a second
        # here), so the round trip would measure this script, not the link.
        self.pinger = Node('uros_check_ping')
        self.pinger.create_subscription(Header, 'uros/pong', self.on_pong, BEST_EFFORT)
        self.ping_pub = self.pinger.create_publisher(Header, 'uros/ping', BEST_EFFORT)
        self.ping_lock = threading.Lock()
        self.ping_seq = 0          # pings counted (sent in the measured window)
        self.ping_sent = {}
        self.ping_lost_at = []     # seconds into the test of each lost ping
        # The measured window starts 2 s after the robot's first message:
        # DDS discovery between this node and the robot takes a moment, and
        # pings sent before it settled would count as lost. They still go out.
        self.first_seen = None
        self.t0 = None
        self.rtt_ms = []
        self.wall_off = None
        self.clock_steps = []      # steps of this PC's wall clock, s
        self.pinger.create_timer(1.0 / ping_hz, self.send_ping)

    # Gaps, rates and round trips on the monotonic clock: WSL2's wall clock
    # steps (seconds at a time, either way) when it re-syncs with Windows, and
    # a step there would show up as a silence or a burst that never happened.
    # The wall clock is only for the message age (stamps are epoch times),
    # and its steps are counted (clock_steps) so an odd age can be explained.
    @staticmethod
    def now():
        return time.monotonic()

    def wall(self):
        w = time.time()
        off = w - time.monotonic()
        if self.wall_off is not None and abs(off - self.wall_off) > 0.05:
            self.clock_steps.append(off - self.wall_off)
        self.wall_off = off
        return w

    @staticmethod
    def stamp_s(h):
        return h.stamp.sec + h.stamp.nanosec * 1e-9

    def seen(self):
        if self.first_seen is None:
            self.first_seen = self.now()

    def start_window(self):
        self.t0 = self.now()
        self.imu.t0 = self.js.t0 = self.t0

    def on_imu(self, m):
        self.seen()
        self.imu.arrived(self.now(), self.stamp_s(m.header), self.wall())
        self.last_imu = m

    def on_js(self, m):
        self.seen()
        self.js.arrived(self.now(), self.stamp_s(m.header), self.wall())
        self.last_js = m

    def send_ping(self):
        h = Header()
        h.stamp = self.get_clock().now().to_msg()
        with self.ping_lock:
            if self.t0 is None:
                h.frame_id = 'warmup'
            else:
                h.frame_id = str(self.ping_seq)
                self.ping_sent[self.ping_seq] = self.now()
                self.ping_seq += 1
        self.ping_pub.publish(h)

    def on_pong(self, h):
        try:
            seq = int(h.frame_id)
        except ValueError:
            return
        now = self.now()
        with self.ping_lock:
            t0 = self.ping_sent.pop(seq, None)
        if t0 is not None:
            self.rtt_ms.append((now - t0) * 1000.0)

    def expire_pings(self):
        """A ping not back after 1 s is lost; remember when it went out."""
        now = self.now()
        with self.ping_lock:
            for seq, t in list(self.ping_sent.items()):
                if now - t > 1.0:
                    del self.ping_sent[seq]
                    self.ping_lost_at.append(t - self.t0)

    def line(self):
        now = self.now()
        quiet = now - max(self.imu.last or 0, self.js.last or 0)
        return '%5.0f s  imu %5.1f Hz  joints %5.1f Hz  ping %s ms  (%d sent, %d back)%s' % (
            now - self.t0, self.imu.rate(now), self.js.rate(now),
            ('%.0f' % statistics.median(self.rtt_ms[-50:])) if self.rtt_ms else '-',
            self.ping_seq, len(self.rtt_ms),
            ('   <- NOTHING from the robot for %.1f s' % quiet) if quiet > 1.0 else '')


def verdict(ok, text):
    return ('  OK    ' if ok else '  CHECK ') + text


def bursts(times, join_s=1.0):
    """Group lost-ping times into bursts: [(start s, end s, count)]."""
    out = []
    for t in sorted(times):
        if out and t - out[-1][1] <= join_s:
            out[-1] = (out[-1][0], t, out[-1][2] + 1)
        else:
            out.append((t, t, 1))
    return out


def report(c, seconds, expect_imu, expect_js):
    t_end = c.now()
    c.expire_pings()
    print('\n=== micro-ROS link check, %d s ===' % seconds)
    for s, want in ((c.imu, expect_imu), (c.js, expect_js)):
        s.close(t_end)
        r = s.rate(t_end)
        print('%-13s %6d msgs  %6.1f Hz (robot set to %s)  longest gap %4.0f ms' % (
            s.name, s.n, r, want if want else '?', s.max_gap * 1000.0))
        if s.age_ms:
            print('%13s age on arrival: median %.1f ms, 95%% %.1f ms, max %.1f ms' % (
                '', statistics.median(s.age_ms), pct(s.age_ms, 95), max(s.age_ms)))
        else:
            print('%13s age on arrival: unknown (robot clock not synced to the agent)' % '')
        if want:
            print(verdict(r >= 0.9 * want, 'rate within 10%% of %s Hz' % want))
        print(verdict(s.n > 0 and s.max_gap < 0.2, 'no gap longer than 200 ms'))
        for at, gap in sorted(s.gaps, key=lambda g: -g[1])[:5]:
            print('%13s silent for %5.0f ms from %5.1f s' % ('', gap * 1000.0, at))

    sent = c.ping_seq
    back = len(c.rtt_ms)
    lost = len(c.ping_lost_at)
    print('ping          %6d sent, %d back, %.1f%% lost' % (
        sent, back, 100.0 * lost / max(1, sent)))
    if c.rtt_ms:
        print('%13s round trip: median %.1f ms, 95%% %.1f ms, 99%% %.1f ms, max %.1f ms' % (
            '', statistics.median(c.rtt_ms), pct(c.rtt_ms, 95), pct(c.rtt_ms, 99),
            max(c.rtt_ms)))
        print(verdict(pct(c.rtt_ms, 95) < 30.0, '95% of round trips under 30 ms'))
    print(verdict(lost <= 0.01 * max(1, sent), 'under 1% of pings lost'))
    for a, b, n in sorted(bursts(c.ping_lost_at), key=lambda x: -x[2])[:5]:
        print('%13s %4d lost between %5.1f s and %5.1f s' % ('', n, a, b))

    if c.clock_steps:
        print('PC clock      stepped %d time(s) during the test (largest %+.2f s): ages are off around '
              'those moments; gaps and rates are not (monotonic clock)' % (
                  len(c.clock_steps), max(c.clock_steps, key=abs)))
    if c.last_imu is not None:
        a = c.last_imu.linear_acceleration
        print('last imu      acc = (%.2f, %.2f, %.2f) m/s^2' % (a.x, a.y, a.z))
        print(verdict(a.z > 8.0 and abs(a.x) < 2.0 and abs(a.y) < 2.0,
                      'flat robot reads about +9.8 on z (only meaningful if it IS flat)'))
    if c.last_js is not None:
        bad = [n for n, q in zip(c.last_js.name, c.last_js.position) if not math.isfinite(q)]
        print(verdict(len(c.last_js.name) == 12 and not bad, '12 joints, all finite'))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--seconds', type=float, default=30.0)
    ap.add_argument('--ping-hz', type=float, default=20.0)
    ap.add_argument('--imu-hz', type=float, default=100.0,
                    help='the rate set on the robot (uros rate), for the verdict')
    ap.add_argument('--joint-hz', type=float, default=50.0)
    args = ap.parse_args()

    rclpy.init()
    c = Check(args.ping_hz)
    execs = []
    for node in (c, c.pinger):
        ex = SingleThreadedExecutor()
        ex.add_node(node)
        threading.Thread(target=ex.spin, daemon=True).start()
        execs.append(ex)
    print('waiting for the robot (imu/data_raw or joint_states)...')
    t_give_up = time.monotonic() + 30.0
    try:
        while c.first_seen is None or time.monotonic() < c.first_seen + 2.0:
            time.sleep(0.05)
            if c.first_seen is None and time.monotonic() > t_give_up:
                break
    except KeyboardInterrupt:
        pass
    if c.first_seen is None:
        print('\nNothing from the robot in 30 s. The app\'s ROS 2 card (or GET /v1/uros) must say '
              'connected; the agent must be running; ROS_DOMAIN_ID must match the card.')
        stop(c, execs)
        return

    c.start_window()
    print('robot heard; measuring for %d s (Ctrl-C stops early)...' % args.seconds)
    t_end = c.t0 + args.seconds
    t_line = c.t0 + 2.0
    try:
        while time.monotonic() < t_end:
            time.sleep(0.05)
            c.expire_pings()
            if time.monotonic() >= t_line:
                t_line += 2.0
                print(c.line())
    except KeyboardInterrupt:
        pass
    report(c, time.monotonic() - c.t0, args.imu_hz, args.joint_hz)
    stop(c, execs)


def stop(c, execs):
    """Executors first, then rclpy: shutting rclpy down under a spinning
    executor thread ends in 'terminate called without an active exception'."""
    for ex in execs:
        ex.shutdown()
    c.pinger.destroy_node()
    c.destroy_node()
    rclpy.try_shutdown()


if __name__ == '__main__':
    main()
