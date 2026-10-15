# mpx_wasm -- the WebAssembly skill runtime.
#
# Ported from main/{wasm,sdk,skills}/ of the MPX-Dog firmware. The ABI it
# exposes (MPX_ABI_VERSION 4, 74 host functions) must stay byte-compatible:
# every skill anyone already owns was compiled against it, and encrypted ones
# cannot be rebuilt by their owners at all.
#
# The dependency on mpx_robot is one-directional by construction. mpx_robot
# does not depend on this component; it asks about a running skill through a
# hook table that mpx_wasm_init() fills in.

idf_component_register(
    SRCS
        "src/wasm_sandbox.cc"
        "src/wasm_decrypt.cc"
        "src/wasm_host_functions.cc"
        "src/registry.cc"
        "src/runner.cc"
        "src/events.cc"
        "src/autorun.cc"
        "src/movement.cc"
        "src/mpx_crypto.cc"
        "src/mpx_skill_fs.cc"
        "src/mpx_wasm_c_api.cc"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "src"
    REQUIRES
        mpx_robot
    PRIV_REQUIRES
        mpx_util
        claw_core
        espressif__wasm-micro-runtime
        espressif__cjson
        mbedtls
        nvs_flash
        esp_timer
        pthread
)

# WAMR sorts its native-symbol table in place with qsort(). If the table lands
# in flash (.rodata) that write raises a cache error on the ESP32-S3, which is
# why NATIVE_SYMBOLS in wasm_host_functions.cc is deliberately non-const.
