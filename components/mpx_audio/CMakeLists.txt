# Requirements are listed unconditionally.
#
# Wrapping them in if(CONFIG_...) does not work the way it looks: component
# CMakeLists are processed during early expansion, before Kconfig values are
# available, so the condition silently evaluates false and the REQUIRES list
# comes out empty. Gating happens in main/idf_component.yml instead, which is
# what decides whether this component is in the build at all.
idf_component_register(
    SRCS
        "src/mpx_audio.c"
        "src/mpx_audio_cmd.c"
        "src/mpx_audio_tune.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_board_manager
        esp_driver_i2s
        claw_core
        console
        vfs
        nvs_flash
)
