# Requirements are listed unconditionally, with no `if(CONFIG_...)` around
# them.
#
# IDF collects a component's REQUIRES during an early expansion pass, and
# CONFIG_ values are not dependably set that early -- so a conditional
# requirement can be silently dropped from the dependency graph while the
# source that needs it still compiles, which surfaces as "not in the
# requirements list of" for a header that is plainly right there.
#
# The condition was redundant regardless: main/idf_component.yml only pulls
# this component in when MP4_ROBOT_ENABLE is set, so by the time anything here
# is built, mpx_robot is in the build too.
idf_component_register(
    SRCS "mpx_selftest.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        console
        cap_display
        claw_core
        esp_board_manager
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_ledc
        esp_partition
        fatfs
        mpx_robot
)
