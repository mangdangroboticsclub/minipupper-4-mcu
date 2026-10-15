# Requirements are listed unconditionally (see components/mpx_audio); whether
# this component is in the build at all is decided in main/idf_component.yml.
idf_component_register(
    SRCS
        "src/cap_music.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        claw_cap
    PRIV_REQUIRES
        console
        esp_timer
        mbedtls
        nvs_flash
        espressif__cjson
        esp_audio_simple_player
        gmf_core
        gmf_io
        mpx_audio
        mpx_voice_link
        mpx_voice_wake
)
