# mpx_robot -- the MangDang quadruped: servo driver boards, gait, IK, IMU.
#
# Ported from main/robot/ of the MPX-Dog firmware. The C++ is deliberate: this
# is proven gait and kinematics code and rewriting it in C would trade a lot of
# risk for a little house style. Everything outside this component talks to it
# through the C facade in include/mpx_robot.h.

idf_component_register(
    SRCS
        # mpv2-claw: the gait, kinematics, moves and servo driver are mpv2's
        # (minipupperesp movement), verbatim under src/robot/. Only the IMU
        # (board-manager I2C, shared with the ES7210) and the C facade are
        # this component's own.
        "src/robot/driver_board.c"
        "src/robot/robot.cc"
        "src/robot/stanford_gait.cc"
        "src/robot/stanford_kinematics.cc"
        "src/robot/alive.c"
        "src/mpx_imu.cc"
        # Bosch's BMM150 driver, verbatim from the ESP-Claw submodule's
        # lua_module_magnetometer (third-party/ is never edited in place).
        # mpx_imu.cc drives it through the BMI270's aux master; the lua
        # module's own backend is off in the board defaults
        # (CONFIG_APP_CLAW_LUA_MODULE_MAGNETOMETER=n), and must stay off --
        # both compiled in would be two copies of every bmm150_* symbol.
        "src/bmm150/bmm150.c"
        "src/mpx_robot_c_api.cc"
    INCLUDE_DIRS
        # "src" is public, not private, and that is a deliberate exception.
        #
        # mpx_wasm's host functions are the skill ABI: 48 distinct robot::
        # symbols including the per-leg IK, the raw servo reads and the bus
        # lock. Wrapping all of that in C to keep robot.h private would be a
        # few hundred lines of pure name translation protecting a boundary
        # that the ABI has already made permanent. Everything that is NOT the
        # skill runtime should still include only mpx_robot.h.
        "include"
        "src"
        # mpx_wasm includes "robot.h" / "imu.h" unqualified; mpv2's own files
        # include "robot/robot.h". Both resolve with these two directories.
        "src/robot"
    PRIV_INCLUDE_DIRS
        "src/bmm150"
    REQUIRES
        esp_board_manager
    PRIV_REQUIRES
        driver
        esp_driver_spi
        esp_driver_gpio
        esp_timer
        nvs_flash
        espressif__i2c_bus
)

# The BMI270 driver is only pulled in when the board selects it. Optional so a
# board without an IMU still links.
idf_component_optional_requires(PRIVATE espressif__bmi270_sensor)
