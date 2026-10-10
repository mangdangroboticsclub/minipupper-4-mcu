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
    # The screen: mpv2-claw's cap_display (face, GIFs, orientation, the Pi's
    # camera picture) on a small display_hw service (ST7789 + esp_lvgl_port)
    # in place of ESP-Claw's display_service. Compiled with the display off
    # too (CONFIG_APP_DISPLAY_ENABLE=n): every entry point then says "no panel".
    "display/display_hw.c"
    "display/cap_display.c"
    "display/cap_display_face.c"
    "display/cap_display_orient.c"
    "display/display_assets.cc"
    "display/display_glue.cc"
    "network/http_display.cc"
    # Voice: the AI & Voice setting (/v1/ai) and the voice routes exist in
    # every build; what they can do depends on CONFIG_APP_VOICE_ENABLE.
    "network/http_voice.cc"
    "voice/voice_glue.cc"
    "platform/shared_i2c.cc"
    "util/trace_ring.cc"
    "lua/lua_vm.cc"
    "lua/lua_bindings.cc"
    # The on-device agent (Phase 5). The owner's AI mode and the robot's
    # tool table exist in every build (the voice server's MCP and /v1/ai use
    # them); agent_runtime.cc compiles to stubs when the agent is off.
    "agent/agent_config.cc"
    "agent/robot_tools.cc"
    "agent/agent_runtime.cc"
)

# ESP-Claw itself: the filesystem roots, the tools as capabilities, and the
# link-time override of its event router stack. These include ESP-Claw
# headers and read CONFIG_APP_AGENT_* symbols that only exist when it is on.
if(CONFIG_APP_AGENT_ENABLE)
    list(APPEND sources
        "agent/agent_fs.cc"
        "agent/agent_caps.cc"
        "agent/claw_overrides.c"
    )
endif()

# The voice stack itself (mpv2-claw's mpx_voice_link, mpx_voice_wake and
# cap_music, plus the native MCP bridge) only when it is on: these files
# include esp-sr, Opus and the audio player, and a board with voice off has
# no reason to compile them. Kconfig values are available here (unlike in a
# REQUIRES list, which is expanded before them), so gating SRCS is safe.
if(CONFIG_APP_VOICE_ENABLE)
    list(APPEND sources
        "voice/mpx_voice_codec.cc"
        "voice/mpx_voice_ws.c"
        "voice/mpx_voice_ota.c"
        "voice/mpx_voice_stream.c"
        "voice/mpx_voice_wake.c"
        "voice/cap_music.c"
        "voice/voice_tools.cc"
    )
endif()

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
    INCLUDE_DIRS "." "fs" "wasm" "sdk" "network" "robot" "lua" "platform" "util" "pi" "audio" "display" "voice" "agent"
    EMBED_FILES
        ${common_assets}
        ${design_assets}
        # The Display page's built-in animations, copied to <fs>/display at boot.
        "display/assets/blink.gif"
        "display/assets/smile.gif"
        "display/assets/sleepy.gif"
    REQUIRES
        nvs_flash
        wasm-micro-runtime
        lua
        littlefs
        esp_codec_dev
        esp_lvgl_port
        lvgl
        esp_new_jpeg
        # Voice (main/voice): esp-sr for the wake word, the xiaozhi author's
        # Opus wrappers (esp-opus-encoder keeps esp-opus private, so opus.h
        # has to be asked for by name), the websocket client, the audio
        # player for music and the TLS/HTTP/NVS/app-desc pieces around them.
        esp-sr
        esp-opus-encoder
        esp-opus
        esp_websocket_client
        esp_audio_simple_player
        gmf_core
        gmf_io
        esp_http_client
        esp-tls
        app_update
        driver
        esp_driver_spi
        esp_driver_i2c
        esp_driver_gpio
        esp_driver_uart
        esp_driver_i2s
        esp_driver_ledc
        esp_lcd
        esp_timer
        esp_http_server
        esp_wifi
        esp_netif
        lwip
        esp_event
        mbedtls
        json
        # The on-device agent: ESP-Claw (third-party/esp-claw, pulled in by
        # main/idf_component.yml). app_claw is the shell, claw_core has
        # claw_paths, claw_cap the capability types, cap_im_local the "web"
        # chat channel; claw_event_router for the stack override's header.
        app_claw
        claw_core
        claw_cap
        cap_im_local
        claw_event_router
)

# Lua requires 32-bit mode on Xtensa/RISC-V architectures.
# This must match the Lua component's own definition so that
# including lua.h from our code sees LUA_32BITS=1.
target_compile_definitions(${COMPONENT_LIB} PRIVATE LUA_32BITS=1)

# ESP-Claw starts its event router with an 8 KB stack and has no option for
# it. Rather than edit the vendored tree, the linker hands every call to
# claw_event_router_init() to __wrap_claw_event_router_init() in
# agent/claw_overrides.c, which raises the stack and calls on. The -u keeps
# the wrapper out of the linker's garbage collection (nothing references it
# by name). mpv2-claw's main/CMakeLists.txt does the same.
if(CONFIG_APP_AGENT_ENABLE)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=claw_event_router_init")
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u __wrap_claw_event_router_init")
endif()