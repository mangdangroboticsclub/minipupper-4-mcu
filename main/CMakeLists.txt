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
    # The Raspberry Pi (ROS 2) link on J-PI, mpv2's protocol.
    "pi/pi_link.cc"
    "pi/pi_audio.cc"
    "pi/pi_video.cc"
    # Microphone + speaker (mpv2-claw's mpx_audio, direct I2S/codec init).
    "audio/mpx_audio.c"
    "audio/mpx_audio_tune.c"
    "platform/shared_i2c.cc"
    "util/trace_ring.cc"
    "lua/lua_vm.cc"
    "lua/lua_bindings.cc"
)

# ── Design-specific assets ───────────────────────────────────
if(CONFIG_PWA_DESIGN_CLAW)
    # The mpv2-claw phone app (pwa-claw/, ported). It is about twice the size
    # of the redesign (three.js model viewer + its 157 KB model), so the big
    # files are embedded as .gz ONLY: every browser that can run this app
    # accepts gzip, and static_handler() falls back to the .gz even for one
    # that does not. Raw+gz of everything would not fit the 2.5 MB app slot.
    message(STATUS "PWA design: claw (mpv2 app)")
    set(pwa_www_dir "../pwa-claw/www")
    set(design_assets
        "../pwa-claw/www/md.svg"
        "../pwa-claw/www/md.svg.gz"
        "../pwa-claw/www/eye-open.svg"
        "../pwa-claw/www/eye-open.svg.gz"
        "../pwa-claw/www/eye-close.svg"
        "../pwa-claw/www/eye-close.svg.gz"
        "../pwa-claw/www/m-pupper3d.js.gz"
        "../pwa-claw/www/pupper_model.json.gz"
    )
    set(big_assets_gz_only TRUE)
elseif(CONFIG_PWA_DESIGN_REDESIGN)
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

# ── Assets shared by all designs (same names, different content) ──
set(common_assets
    "${pwa_www_dir}/index.html"
    "${pwa_www_dir}/index.html.gz"
    "${pwa_www_dir}/index.css.gz"
    "${pwa_www_dir}/m.js.gz"
    "${pwa_www_dir}/manifest.json"
    "${pwa_www_dir}/manifest.json.gz"
    "${pwa_www_dir}/sw.js"
    "${pwa_www_dir}/sw.js.gz"
)
if(NOT big_assets_gz_only)
    list(APPEND common_assets
        "${pwa_www_dir}/index.css"
        "${pwa_www_dir}/m.js"
    )
endif()

# ── Register component ───────────────────────────────────────
idf_component_register(
    SRCS        ${sources}
    INCLUDE_DIRS "." "fs" "wasm" "sdk" "network" "robot" "lua" "platform" "util" "pi" "audio"
    EMBED_FILES
        ${common_assets}
        ${design_assets}
    REQUIRES
        nvs_flash
        wasm-micro-runtime
        lua
        littlefs
        esp_codec_dev
        driver
        esp_driver_spi
        esp_driver_i2c
        esp_driver_gpio
        esp_driver_uart
        esp_driver_i2s
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