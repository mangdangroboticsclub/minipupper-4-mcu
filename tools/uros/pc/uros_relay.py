#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""PC side of the micro-ROS robot: its best-effort topics, republished reliable.

The robot sends imu/data_raw, joint_states and odom/raw best effort (a late
sample is worth nothing over Wi-Fi). A best-effort publisher does not match a
reliable subscriber, and robot_state_publisher and the EKF subscribe reliable,
so this node republishes them:

    imu/data_raw  -> pc/imu           (gyro bias removed, covariances filled in)
    odom/raw      -> pc/odom          (velocity x linear / angular scale)
    joint_states  -> pc/joint_states

and it removes the robot's own body from the lidar:

    scan          -> scan_filtered    (points inside the robot's outline: NaN)

The LD06 sits on the robot's back and sees its own legs a few cm away; Nav2
marks them as obstacles all around the robot ("stuck in a wall") and SLAM puts
them in the map. The outline is a box in base_link (the URDF's footprint,
-0.11..0.12 x -0.07..0.07 m, plus a margin for the legs) and the LD06's pose is
the URDF's lidar_joint (centred on the body, yaw -90 deg: measured).

and it scales Nav2's commands the other way:

    cmd_vel_navigation2 -> cmd_vel    (x 1 / scale)

The scales: the gait walks slower than it is told. ros2jazzympv2 measured
0.624 for this gait (mini_pupper_bringup/config/mini_pupper_mp4.yaml,
odometry.linear_scale; its bridge and nav_vel_scaler use it the same way), so
odom/raw (the command) is scaled down to what the robot really does, and Nav2,
which plans in real m/s, is scaled up to what the robot must be told.

The gyro bias is measured over the first 2 s: keep the robot still when this
starts (uros_slam.launch.py starts it).
"""
import argparse
import math
import sys

import rclpy
from geometry_msgs.msg import Twist
from nav_msgs.msg import Odometry
from rclpy.node import Node
from rclpy.qos import QoSProfile, qos_profile_sensor_data
from sensor_msgs.msg import Imu, JointState, LaserScan

# The robot's outline in base_link (m) and the LD06's pose in it (the URDF).
BODY_X = (-0.15, 0.16)
BODY_Y = (-0.11, 0.11)
LIDAR_X, LIDAR_Y, LIDAR_YAW = 0.0, 0.0, -math.pi / 2

BIAS_SAMPLES = 200        # 2 s at 100 Hz
GYRO_VAR = 0.02 ** 2      # (rad/s)^2
ACC_VAR = 0.2 ** 2        # (m/s^2)^2


class Relay(Node):

    def __init__(self, lin, ang):
        super().__init__('uros_relay')
        self.lin, self.ang = lin, ang
        rel = QoSProfile(depth=10)
        be = qos_profile_sensor_data
        self.pub_imu = self.create_publisher(Imu, 'pc/imu', rel)
        self.pub_odom = self.create_publisher(Odometry, 'pc/odom', rel)
        self.pub_js = self.create_publisher(JointState, 'pc/joint_states', rel)
        self.create_subscription(Imu, 'imu/data_raw', self.on_imu, be)
        self.create_subscription(Odometry, 'odom/raw', self.on_odom, be)
        self.pub_cmd = self.create_publisher(Twist, 'cmd_vel', rel)
        self.create_subscription(Twist, 'cmd_vel_navigation2', self.on_nav_cmd, rel)
        self.create_subscription(JointState, 'joint_states', self.pub_js.publish, be)
        self.pub_scan = self.create_publisher(LaserScan, 'scan_filtered', be)
        self.create_subscription(LaserScan, 'scan', self.on_scan, be)
        self.mask = None          # beams that hit the robot, per (angle_min, increment, n)
        self.mask_key = None
        self.bias = None
        self.sum = [0.0, 0.0, 0.0]
        self.n = 0
        self.get_logger().info('measuring the gyro bias: keep the robot still for 2 s '
                               '(odometry scale %.3f linear, %.3f angular)' % (lin, ang))

    def on_odom(self, m):
        t = m.twist.twist
        t.linear.x *= self.lin
        t.linear.y *= self.lin
        t.angular.z *= self.ang
        self.pub_odom.publish(m)

    def body_mask(self, m):
        """For each beam, the range (m) below which its point is inside the
        robot's outline (0 = never): the beam leaves the outline there."""
        key = (m.angle_min, m.angle_increment, len(m.ranges))
        if key == self.mask_key:
            return self.mask
        mask = []
        for i in range(len(m.ranges)):
            a = LIDAR_YAW + m.angle_min + i * m.angle_increment
            dx, dy = math.cos(a), math.sin(a)
            r, step = 0.0, 0.005
            while r < 0.3:
                x, y = LIDAR_X + r * dx, LIDAR_Y + r * dy
                if not (BODY_X[0] <= x <= BODY_X[1] and BODY_Y[0] <= y <= BODY_Y[1]):
                    break
                r += step
            mask.append(r)
        self.mask, self.mask_key = mask, key
        return mask

    def on_scan(self, m):
        mask = self.body_mask(m)
        m.ranges = [float('nan') if r <= cut else r for r, cut in zip(m.ranges, mask)]
        self.pub_scan.publish(m)

    def on_nav_cmd(self, m):
        m.linear.x /= self.lin
        m.linear.y /= self.lin
        m.angular.z /= self.ang
        self.pub_cmd.publish(m)

    def on_imu(self, m):
        w = m.angular_velocity
        if self.bias is None:
            self.sum[0] += w.x
            self.sum[1] += w.y
            self.sum[2] += w.z
            self.n += 1
            if self.n >= BIAS_SAMPLES:
                self.bias = [s / self.n for s in self.sum]
                self.get_logger().info('gyro bias %.4f %.4f %.4f rad/s: relaying' % tuple(self.bias))
            return
        w.x -= self.bias[0]
        w.y -= self.bias[1]
        w.z -= self.bias[2]
        for i in (0, 4, 8):
            if m.angular_velocity_covariance[i] == 0.0:
                m.angular_velocity_covariance[i] = GYRO_VAR
            if m.linear_acceleration_covariance[i] == 0.0:
                m.linear_acceleration_covariance[i] = ACC_VAR
        self.pub_imu.publish(m)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--linear-scale', type=float, default=0.624)
    ap.add_argument('--angular-scale', type=float, default=1.0)
    args, ros_args = ap.parse_known_args()
    rclpy.init(args=[sys.argv[0]] + ros_args)
    node = Relay(args.linear_scale, args.angular_scale)
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    node.destroy_node()
    rclpy.try_shutdown()


if __name__ == '__main__':
    main()
