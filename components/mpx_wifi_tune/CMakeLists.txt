# Station-side Wi-Fi tuning on top of the submodule's wifi_manager.
# See include/mpx_wifi_tune.h for what and why.
idf_component_register(
    SRCS
        "src/mpx_wifi_tune.c"
        "src/mpx_captive_dns.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_wifi
        esp_event
        esp_netif
        lwip
)
