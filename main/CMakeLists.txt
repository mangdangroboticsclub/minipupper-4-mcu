# ── Source files (common to all designs) ─────────────────────
set(sources
    "main.cc"
    "fs/littlefs_manager.cc"
    "wasm/wasm_sandbox.cc"
    "wasm/wasm_decrypt.cc"
    "wasm/registry.cc"
    "sdk/wasm_host_functions.cc"
    "network/wifi_ap.cc"
    "network/wifi_sta.cc"
    "network/http_server.cc"
    "network/www_assets.cc"
    "network/crypto.cc"
    "network/chat_ws.cc"
    "network/marketplace_proxy.cc"
    # The robot: mpv2's gait, kinematics, moves and servo driver (local-claw
    # branch: these are mpv2-claw's components/mpx_robot/src/robot files, see
    # .cursor/PORT_NOTES.md). imu.cc stays this firmware's (shared I2C bus).
    "robot/robot.cc"
    "robot/driver_board.c"
    "robot/stanford_gait.cc"
    "robot/stanford_kinematics.cc"
    "robot/alive.c"
    "robot/imu.cc"
    "robot/mpx_robot_c_api.cc"
    "platform/shared_i2c.cc"
    "util/trace_ring.cc"
    "lua/lua_vm.cc"
    "lua/lua_bindings.cc"
)

# ── Design-specific assets ───────────────────────────────────
if(CONFIG_PWA_DESIGN_REDESIGN)
    message(STATUS "PWA design: redesign (light theme)")
    set(pwa_www_dir "../pwa-redesign/www")
    set(design_assets
        "../pwa-redesign/www/md.svg"
        "../pwa-redesign/www/md.svg.gz"
        "../pwa-redesign/www/eye-open.svg"
        "../pwa-redesign/www/eye-open.svg.gz"
        "../pwa-redesign/www/eye-close.svg"
        "../pwa-redesign/www/eye-close.svg.gz"
    )
else()
    message(STATUS "PWA design: original (dark theme)")
    set(pwa_www_dir "../pwa/www")
    set(design_assets
        "../pwa/www/icon.svg"
        "../pwa/www/icon.svg.gz"
    )
endif()

# ── Assets shared by both designs (same names, different content) ──
set(common_assets
    "${pwa_www_dir}/index.html"
    "${pwa_www_dir}/index.html.gz"
    "${pwa_www_dir}/index.css"
    "${pwa_www_dir}/index.css.gz"
    "${pwa_www_dir}/m.js"
    "${pwa_www_dir}/m.js.gz"
    "${pwa_www_dir}/manifest.json"
    "${pwa_www_dir}/manifest.json.gz"
    "${pwa_www_dir}/sw.js"
    "${pwa_www_dir}/sw.js.gz"
)

# ── Register component ───────────────────────────────────────
idf_component_register(
    SRCS        ${sources}
    INCLUDE_DIRS "." "fs" "wasm" "sdk" "network" "robot" "lua" "platform" "util"
    EMBED_FILES
        ${common_assets}
        ${design_assets}
    REQUIRES
        nvs_flash
        wasm-micro-runtime
        lua
        littlefs
        driver
        esp_driver_spi
        esp_driver_i2c
        esp_driver_gpio
        esp_timer
        esp_http_server
        esp_wifi
        esp_netif
        lwip
        esp_event
        mbedtls
        json
)

# Lua requires 32-bit mode on Xtensa/RISC-V architectures.
# This must match the Lua component's own definition so that
# including lua.h from our code sees LUA_32BITS=1.
target_compile_definitions(${COMPONENT_LIB} PRIVATE LUA_32BITS=1)