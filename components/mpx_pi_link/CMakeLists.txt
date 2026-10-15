# The Raspberry Pi (ROS 2) link on the J-PI header: mpv2's protocol, so the
# ROS bridge that drives mpv2 drives this firmware too. See
# include/mpx_pi_link.h.
#
# Gated at the manifest level (main/idf_component.yml pulls this in under
# MP4_ROBOT_ENABLE) and at the call site (main.c under MP4_PI_LINK_ENABLE);
# the sources themselves always build, so a Kconfig change does not need a
# reconfigure to take effect on this component.
#
# mpx_robot is a dependency in one direction only: the link calls
# robot::master::* (src/robot/robot.h, exported by mpx_robot on purpose) and
# reports back through mpx_robot_set_master_observer() in main.c. The camera
# picture goes to a sink main.c registers, so cap_display is not needed here.
idf_component_register(
    SRCS
        "src/pi_link.cc"
        "src/pi_audio.cc"
        "src/pi_video.cc"
        "src/mpx_pi_link.c"
        "src/rl_link.cc"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "src"
    REQUIRES
        esp_driver_uart
    PRIV_REQUIRES
        console
        esp_driver_gpio
        esp_timer
        mpx_robot
        mpx_wasm
        mpx_audio
)
