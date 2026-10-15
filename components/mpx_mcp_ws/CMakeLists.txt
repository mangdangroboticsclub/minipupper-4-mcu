idf_component_register(
    SRCS
        "src/mpx_mcp_ws.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        mpx_voice_link
        claw_cap
        claw_manager
        claw_core
        console
        json
        esp_hw_support
        # Same managed-component naming trap as mpx_voice_link's CMakeLists:
        # the plain registry name does not resolve once it is in the build.
        espressif__mcp-c-sdk
)
