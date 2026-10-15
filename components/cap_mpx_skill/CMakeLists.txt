idf_component_register(
    SRCS "src/cap_mpx_skill.c"
    INCLUDE_DIRS "include"
    REQUIRES claw_cap mpx_wasm
    PRIV_REQUIRES espressif__cjson
)
