idf_component_register(
    SRCS "src/cap_display.c" "src/cap_display_face.c" "src/cap_display_orient.c"
    INCLUDE_DIRS "include"
    REQUIRES
        claw_cap
        display_service
        lvgl
    PRIV_REQUIRES
        espressif__cjson
        esp_board_manager
        esp_driver_ledc
        esp_hw_support
        esp_timer
        esp_lcd
        console
        nvs_flash
)

# The Pi's camera picture (cap_display_show_jpeg) decodes with Espressif's
# esp_new_jpeg. It is already in the build through esp-dl / esp_lv_decoder,
# so it is attached optionally rather than declared: if it ever drops out,
# cap_display_show_jpeg() answers ESP_ERR_NOT_SUPPORTED instead of the build
# failing (src/cap_display.c checks __has_include).
idf_component_optional_requires(PRIVATE espressif__esp_new_jpeg)
