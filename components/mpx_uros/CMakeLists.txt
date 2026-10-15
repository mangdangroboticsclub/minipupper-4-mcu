# micro-ROS on the robot itself: the ESP32-S3 is a ROS 2 node on Wi-Fi,
# talking to a micro-ROS agent on the PC (UDP, port 8888 by default).
# No Raspberry Pi. See README_UROS.md at the project root.
#
# The ROS client libraries come prebuilt from components/micro_ros (built on
# Linux from micro_ros_espidf_component, jazzy, for this exact IDF and
# sdkconfig) so that a Windows `idf.py build` needs no colcon.
idf_component_register(
    SRCS
        "src/mpx_uros.c"
        "src/uros_node.c"
        "src/uros_robot.cc"
        "src/uros_joint_map.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "src"
    REQUIRES
        micro_ros
    PRIV_REQUIRES
        console
        esp_netif
        esp_timer
        esp_wifi
        nvs_flash
        mpx_robot
)
