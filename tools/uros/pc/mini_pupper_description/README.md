# mini_pupper_mp4 robot model (SKU1)

Copied, unchanged, from ros2jazzympv2 (github.com/Bariest/ros2jazzympv2,
commit 4edd939), `mini_pupper_description/`: the `mini_pupper_mp4` URDF and its
meshes. Apache License 2.0, see `LICENSE` here.

`../config/` holds, from the same commit and also unchanged:

| File | From |
|---|---|
| `mp4_room.yaml` | `mini_pupper_navigation/param/` (Nav2) |
| `slam_toolbox_room.yaml` | `mini_pupper_slam/config/` |
| `navigation.rviz` | `mini_pupper_navigation/rviz/` |

The launch files never change these; they write changed copies to `/tmp`
(the map path, `/scan_filtered`). To tune Nav2 or slam_toolbox, edit them here.
The LD06 mount is `lidar_joint` in the URDF (xyz -0.0612 0.0005 0.0685, yaw +90 deg).
