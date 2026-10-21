# SPDX-License-Identifier: Apache-2.0
"""Navigate the micro-ROS robot with Nav2: everything on the PC, nothing on a Pi.

    # on a map saved with map_saver_cli (AMCL localisation):
    ros2 launch $UROS/pc/uros_nav.launch.py \\
        map:=/mnt/c/esp/maps/room.yaml
    # no saved map: map and navigate at once (slam_toolbox)
    ros2 launch $UROS/pc/uros_nav.launch.py slam:=True

Needs the micro-ROS agent running and the robot connected, with its LD06.
Keep the robot still for the first 2 s (gyro bias). Starts:

  uros_relay.py          best effort -> reliable (pc/imu, pc/odom, pc/joint_states),
                         odometry scaled to real m/s, and Nav2's commands
                         cmd_vel_navigation2 -> cmd_vel scaled back (0.624, as
                         ros2jazzympv2's nav_vel_scaler)
  robot_state_publisher  the SKU1 (mini_pupper_mp4) URDF (mini_pupper_description/)
  ekf_node               odom -> base_link, published as /odom
  Nav2                   mp4_room.yaml (config/): map_server + AMCL + planner,
                         controller, smoother, collision monitor, behaviours
  slam_toolbox           only with slam:=true (then it provides map -> odom)
  rviz2                  navigation.rviz (config/) (rviz:=false to leave out;
                         look:=film for film.rviz: clean, for screen recording)

A small room or careful moves: margin:=0.12 (the clearance kept from walls, m; the
default 0.25 / 0.30 keeps the path in the middle of the room) and speed:=0.7 (every
speed limit times 0.7), e.g.
    ros2 launch $UROS/pc/uros_nav.launch.py map:=/mnt/c/esp/maps/room_5.yaml look:=film

In RViz: "2D Pose Estimate" where the robot really is (AMCL), then "Nav2 Goal".
"""
import os

from launch import LaunchDescription
from launch.actions import (DeclareLaunchArgument, ExecuteProcess, IncludeLaunchDescription,
                            OpaqueFunction)
from launch.conditions import IfCondition
from launch.launch_description_sources import PythonLaunchDescriptionSource
from launch.substitutions import LaunchConfiguration, PathJoinSubstitution
from launch_ros.actions import Node
from launch_ros.substitutions import FindPackageShare

HERE = os.path.dirname(os.path.realpath(__file__))
# Copied from ros2jazzympv2 into this folder, so nothing else is needed:
# the SKU1 robot model (mini_pupper_description/) and the slam_toolbox, Nav2
# and RViz settings (config/).
DESC = os.path.join(HERE, 'mini_pupper_description')
CFG = os.path.join(HERE, 'config')


def robot_state_publisher(context):
    """The URDF with its package:// meshes as file:// paths (see uros_slam.launch.py)."""
    import xacro
    path = LaunchConfiguration('urdf').perform(context)
    pkg = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(path))))
    urdf = xacro.process_file(path).toxml().replace(
        'package://mini_pupper_description/', 'file://' + pkg + '/')
    return [Node(package='robot_state_publisher', executable='robot_state_publisher',
                 output='screen', parameters=[{'robot_description': urdf}],
                 remappings=[('joint_states', 'pc/joint_states')])]


def scan_filtered(node):
    """Every scan topic in a parameter tree -> /scan_filtered (uros_relay.py:
    the scan without the robot's own legs and body)."""
    if isinstance(node, dict):
        for k, v in node.items():
            if k in ('topic', 'scan_topic') and v in ('scan', '/scan'):
                node[k] = '/scan_filtered'
            else:
                scan_filtered(v)
    elif isinstance(node, list):
        for v in node:
            scan_filtered(v)


def tune(p, margin: str, speed: str):
    """margin:=M: the clearance Nav2 keeps from walls, metres (the costmaps' inflation radius; 0.25 local and
    0.30 global in mp4_room.yaml): lower for a small room, at least 0.10 (the robot is 23 x 14 cm). speed:=F: every
    speed and acceleration limit times F (0.5 = half: careful moves)."""
    if margin:
        m = max(0.10, float(margin))
        for cm in ('local_costmap', 'global_costmap'):
            layer = p.setdefault(cm, {}).setdefault(cm, {}).setdefault('ros__parameters', {}) \
                .setdefault('inflation_layer', {})
            layer['inflation_radius'] = m
            layer['cost_scaling_factor'] = 4.0 if m >= 0.25 else 10.0     # the cost falls off faster in a tight space
    if speed:
        f = min(1.0, max(0.1, float(speed)))
        fp = p.setdefault('controller_server', {}).setdefault('ros__parameters', {}).setdefault('FollowPath', {})
        for k in ('min_vel_x', 'max_vel_x', 'min_vel_y', 'max_vel_y', 'max_vel_theta', 'max_speed_xy', 'acc_lim_x',
                  'acc_lim_y', 'acc_lim_theta', 'decel_lim_x', 'decel_lim_y', 'decel_lim_theta'):
            if k in fp:
                fp[k] = round(fp[k] * f, 4)
        vs = p.setdefault('velocity_smoother', {}).setdefault('ros__parameters', {})
        for k in ('max_velocity', 'min_velocity', 'max_accel', 'max_decel'):
            if k in vs:
                vs[k] = [round(v * f, 4) for v in vs[k]]
        bs = p.setdefault('behavior_server', {}).setdefault('ros__parameters', {})
        if 'max_rotational_vel' in bs:
            bs['max_rotational_vel'] = round(bs['max_rotational_vel'] * f, 4)
            bs['min_rotational_vel'] = min(bs.get('min_rotational_vel', 0.3), bs['max_rotational_vel'])


def nav2_params(context):
    """Nav2's settings with the map filled in. ros2jazzympv2's mp4_room.yaml has
    map_server.yaml_filename: "" and, with this Nav2, that empty value wins over
    bringup's map:= (map_server: "yaml-filename parameter is empty"). So the
    map goes into a copy of the file, and Nav2 starts from the copy."""
    import tempfile
    import yaml
    src = LaunchConfiguration('params').perform(context)
    map_yaml = os.path.abspath(LaunchConfiguration('map').perform(context))
    with open(src) as f:
        p = yaml.safe_load(f)
    if LaunchConfiguration('slam').perform(context).lower() != 'true':
        if not os.path.isfile(map_yaml):
            raise RuntimeError('no map at %s (map:=...yaml from map_saver_cli)' % map_yaml)
        p.setdefault('map_server', {}).setdefault('ros__parameters', {})['yaml_filename'] = map_yaml
    scan_filtered(p)
    tune(p, LaunchConfiguration('margin').perform(context), LaunchConfiguration('speed').perform(context))
    out = os.path.join(tempfile.gettempdir(), 'uros_nav2_params.yaml')
    with open(out, 'w') as f:
        yaml.safe_dump(p, f, sort_keys=False)
    return out


def slam_toolbox(context):
    """slam_toolbox on /scan_filtered, from a copy of its settings (as uros_slam.launch.py)."""
    import tempfile
    import yaml
    with open(LaunchConfiguration('slam_params').perform(context)) as f:
        p = yaml.safe_load(f)
    for node in p.values():
        node.setdefault('ros__parameters', {})['scan_topic'] = '/scan_filtered'
    out = os.path.join(tempfile.gettempdir(), 'uros_slam_params.yaml')
    with open(out, 'w') as f:
        yaml.safe_dump(p, f, sort_keys=False)
    return [IncludeLaunchDescription(
        PythonLaunchDescriptionSource(PathJoinSubstitution(
            [FindPackageShare('slam_toolbox'), 'launch', 'online_async_launch.py'])),
        launch_arguments={'slam_params_file': out, 'use_sim_time': 'false'}.items())]


def nav2(context):
    """map_server + AMCL + Nav2 on the saved map, or Nav2 alone with slam:=True."""
    nav2_dir = FindPackageShare('nav2_bringup').perform(context)
    params = nav2_params(context)
    if LaunchConfiguration('slam').perform(context).lower() == 'true':
        return [IncludeLaunchDescription(
            PythonLaunchDescriptionSource(os.path.join(nav2_dir, 'launch', 'navigation_launch.py')),
            launch_arguments={'params_file': params, 'use_sim_time': 'False',
                              'use_composition': 'False', 'autostart': 'True'}.items())]
    map_yaml = os.path.abspath(LaunchConfiguration('map').perform(context))
    return [IncludeLaunchDescription(
        PythonLaunchDescriptionSource(os.path.join(nav2_dir, 'launch', 'bringup_launch.py')),
        launch_arguments={'map': map_yaml, 'params_file': params, 'use_sim_time': 'False',
                          'slam': 'False', 'use_composition': 'False',
                          'autostart': 'True'}.items())]


def rviz_node(context, default_cfg):
    """rviz2 with its usual settings, or config/film.rviz with look:=film."""
    film = LaunchConfiguration('look').perform(context) == 'film'
    cfg = os.path.join(CFG, 'film.rviz') if film else default_cfg
    return [Node(package='rviz2', executable='rviz2', output='log', arguments=['-d', cfg])]


def generate_launch_description():
    slam = LaunchConfiguration('slam')
    rviz = LaunchConfiguration('rviz')
    scale = LaunchConfiguration('linear_scale')
    return LaunchDescription([
        DeclareLaunchArgument('map', default_value='/mnt/c/esp/maps/room.yaml',
                              description='map yaml from map_saver_cli'),
        # 'False', not 'false': nav2_bringup reads this same 'slam' and puts it
        # through a Python expression ('not ' + slam).
        DeclareLaunchArgument('slam', default_value='False',
                              description='True: no saved map, slam_toolbox maps as you go'),
        DeclareLaunchArgument(
            'params', default_value=os.path.join(CFG, 'mp4_room.yaml'),
            description='Nav2 settings'),
        DeclareLaunchArgument(
            'urdf', default_value=os.path.join(
                DESC, 'urdf/mini_pupper_mp4/mini_pupper_description.urdf.xacro')),
        DeclareLaunchArgument(
            'slam_params', default_value=os.path.join(CFG, 'slam_toolbox_room.yaml')),
        DeclareLaunchArgument('linear_scale', default_value='0.624',
                              description='real speed / commanded speed of the gait'),
        DeclareLaunchArgument('margin', default_value='',
                              description='clearance from walls, m (default 0.25/0.30; small room: 0.12)'),
        DeclareLaunchArgument('speed', default_value='',
                              description='speed limits times this (0.5 = half speed)'),
        DeclareLaunchArgument('rviz', default_value='true'),
        DeclareLaunchArgument('look', default_value='', choices=['', 'film'],
                              description='film: the clean RViz for screen recording (config/film.rviz)'),

        ExecuteProcess(cmd=['python3', os.path.join(HERE, 'uros_relay.py'),
                            '--linear-scale', scale], output='screen'),

        OpaqueFunction(function=robot_state_publisher),

        Node(package='robot_localization', executable='ekf_node', name='uros_ekf',
             output='screen', parameters=[os.path.join(HERE, 'ekf_uros.yaml')],
             remappings=[('odometry/filtered', 'odom')]),

        # Saved map: map_server + AMCL + navigation; slam:=True: navigation only.
        OpaqueFunction(function=nav2),
        # No map yet: slam_toolbox (on /scan_filtered) for map -> odom.
        OpaqueFunction(function=slam_toolbox, condition=IfCondition(slam)),

        OpaqueFunction(function=lambda c: rviz_node(c, os.path.join(CFG, 'navigation.rviz')),
                       condition=IfCondition(rviz)),
    ])
