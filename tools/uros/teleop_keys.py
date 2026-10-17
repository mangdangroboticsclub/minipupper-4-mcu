#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Drive the robot from the keyboard over micro-ROS (phase 1).

Run it on the PC (WSL2) with the micro-ROS agent running and the robot
connected (the app's ROS 2 card is green):

    python3 teleop_keys.py

It publishes cmd_vel (geometry_msgs/Twist) 20 times a second for as long as
it runs; the robot's own gait walks it. Keys change the command, they do not
have to be held:

    w / s     forward / back      (steps of 0.02 m/s)
    a / d     left / right        (steps of 0.02 m/s)
    q / e     turn left / right   (steps of 0.2 rad/s)
    space     stop (all zero: the robot stands after 1 s)
    x         stop and quit       (the robot goes back to the app after 2 s)

The line shows what is sent and what the robot executes (odom/raw). The robot
refuses cmd_vel while the RL walk runs (the app's RL walk page).
"""
import argparse
import math
import os
import select
import sys
import termios
import threading
import time
import tty

import rclpy
from geometry_msgs.msg import Twist
from nav_msgs.msg import Odometry
from rclpy.executors import SingleThreadedExecutor
from rclpy.node import Node
from rclpy.qos import HistoryPolicy, QoSProfile, ReliabilityPolicy

BEST_EFFORT = QoSProfile(reliability=ReliabilityPolicy.BEST_EFFORT,
                         history=HistoryPolicy.KEEP_LAST, depth=5)


def clamp(v, lim):
    return max(-lim, min(lim, v))


class Teleop(Node):

    def __init__(self):
        super().__init__('teleop_keys')
        self.pub = self.create_publisher(Twist, 'cmd_vel', BEST_EFFORT)
        self.create_subscription(Odometry, 'odom/raw', self.on_odom, BEST_EFFORT)
        self.odom = None
        self.odom_t = 0.0

    def on_odom(self, m):
        self.odom = m
        self.odom_t = time.monotonic()

    def send(self, vx, vy, wz):
        t = Twist()
        t.linear.x, t.linear.y, t.angular.z = float(vx), float(vy), float(wz)
        self.pub.publish(t)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--rate', type=float, default=20.0, help='cmd_vel per second')
    ap.add_argument('--max-vx', type=float, default=0.15, help='m/s (the robot allows 0.20)')
    ap.add_argument('--max-vy', type=float, default=0.10, help='m/s')
    ap.add_argument('--max-wz', type=float, default=1.0, help='rad/s')
    args = ap.parse_args()
    if not os.isatty(sys.stdin.fileno()):
        sys.exit('run it in a terminal: it reads single keys')

    rclpy.init()
    node = Teleop()
    ex = SingleThreadedExecutor()
    ex.add_node(node)
    spin = threading.Thread(target=ex.spin, daemon=True)
    spin.start()

    vx = vy = wz = 0.0
    keys = {
        'w': (0.02, 0, 0), 's': (-0.02, 0, 0),
        'a': (0, 0.02, 0), 'd': (0, -0.02, 0),
        'q': (0, 0, 0.2), 'e': (0, 0, -0.2),
    }
    print(next(b for b in __doc__.split('\n\n') if 'w / s' in b))
    old = termios.tcgetattr(sys.stdin)
    period = 1.0 / args.rate
    next_t = time.monotonic()
    try:
        tty.setcbreak(sys.stdin.fileno())
        while True:
            timeout = max(0.0, next_t - time.monotonic())
            r, _, _ = select.select([sys.stdin], [], [], timeout)
            if r:
                k = sys.stdin.read(1).lower()
                if k == 'x' or k == '\x03':
                    break
                if k == ' ':
                    vx = vy = wz = 0.0
                elif k in keys:
                    dx, dy, dz = keys[k]
                    vx = round(clamp(vx + dx, args.max_vx), 3)
                    vy = round(clamp(vy + dy, args.max_vy), 3)
                    wz = round(clamp(wz + dz, args.max_wz), 3)
                continue
            next_t += period
            if time.monotonic() - next_t > period:
                next_t = time.monotonic() + period
            node.send(vx, vy, wz)
            o = node.odom
            if o is not None and time.monotonic() - node.odom_t < 1.0:
                tw = o.twist.twist
                p = o.pose.pose
                yaw = 2.0 * math.atan2(p.orientation.z, p.orientation.w)
                got = 'robot does vx %+.3f vy %+.3f wz %+.2f | odom x %+.2f y %+.2f yaw %+4.0f deg' % (
                    tw.linear.x, tw.linear.y, tw.angular.z, p.position.x, p.position.y,
                    math.degrees(yaw))
            else:
                got = 'no odom/raw from the robot'
            sys.stdout.write('\rsend vx %+.2f vy %+.2f wz %+.1f | %s   ' % (vx, vy, wz, got))
            sys.stdout.flush()
    finally:
        termios.tcsetattr(sys.stdin, termios.TCSADRAIN, old)
        for _ in range(5):
            node.send(0.0, 0.0, 0.0)
            time.sleep(0.05)
        print('\nstopped; the robot goes back to the app in 2 s')
        ex.shutdown()
        spin.join(timeout=1.0)
        node.destroy_node()
        rclpy.try_shutdown()


if __name__ == '__main__':
    main()
