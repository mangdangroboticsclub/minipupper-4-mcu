#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""What Nav2 asks for and what the robot does, once a second: run it in a spare
terminal while a goal is active and paste the output.

    python3 $UROS/pc/nav_check.py

  nav     cmd_vel_nav          the controller (DWB)
  smooth  cmd_vel_smoothed     after the velocity smoother
  out     cmd_vel_navigation2  after the collision monitor (zero: it stopped the robot)
  robot   cmd_vel              what goes to the ESP (uros_relay.py)
  odom    /odom                the robot's measured speed (EKF)
  pose    /amcl_pose           where AMCL thinks it is

A '-' is a topic with nothing in the last second.
"""
import math
import time

import rclpy
from geometry_msgs.msg import PoseWithCovarianceStamped, Twist
from nav_msgs.msg import Odometry
from rclpy.node import Node
from rclpy.qos import DurabilityPolicy, QoSProfile, ReliabilityPolicy

CMDS = [('nav', 'cmd_vel_nav'), ('smooth', 'cmd_vel_smoothed'), ('out', 'cmd_vel_navigation2'), ('robot', 'cmd_vel')]


class Check(Node):
    def __init__(self):
        super().__init__('nav_check')
        self.last = {}
        for name, topic in CMDS:
            self.create_subscription(Twist, topic, lambda m, n=name: self.put(n, (m.linear.x, m.linear.y, m.angular.z)), 10)
        self.create_subscription(Odometry, 'odom', lambda m: self.put('odom', (
            m.twist.twist.linear.x, m.twist.twist.linear.y, m.twist.twist.angular.z)), 10)
        latched = QoSProfile(depth=1, reliability=ReliabilityPolicy.RELIABLE, durability=DurabilityPolicy.TRANSIENT_LOCAL)
        self.create_subscription(PoseWithCovarianceStamped, 'amcl_pose', self.on_pose, latched)
        self.pose = None
        self.t0 = time.monotonic()
        self.create_timer(1.0, self.show)

    def put(self, name, v):
        self.last[name] = (time.monotonic(), v)

    def on_pose(self, m):
        p, q = m.pose.pose.position, m.pose.pose.orientation
        yaw = math.atan2(2 * (q.w * q.z + q.x * q.y), 1 - 2 * (q.y * q.y + q.z * q.z))
        self.pose = (p.x, p.y, math.degrees(yaw))

    def show(self):
        now = time.monotonic()
        parts = ['%5.0fs' % (now - self.t0)]
        for name in [n for n, _ in CMDS] + ['odom']:
            t, v = self.last.get(name, (0.0, None))
            if v is None or now - t > 1.0:
                parts.append('%s -' % name)
            else:
                parts.append('%s %+.3f %+.3f %+.2f' % (name, *v))
        if self.pose:
            parts.append('pose %+.2f %+.2f %+.0fdeg' % self.pose)
        print(' | '.join(parts), flush=True)


def main():
    rclpy.init()
    node = Check()
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    finally:
        node.destroy_node()
        rclpy.try_shutdown()


if __name__ == '__main__':
    main()
