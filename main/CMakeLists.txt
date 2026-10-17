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
    # The Raspberry Pi link on J-PI: mpv2's ROS protocol, plus mp4-link's RL
    # host protocol on the same UART (pi/RL_LINK.md).
    "pi/pi_link.cc"
    "pi/pi_audio.cc"
    "pi/pi_video.cc"
    "pi/rl_link.cc"
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
    "util/sys_monitor.cc"
    "lua/lua_vm.cc"
    "lua/lua_bindings.cc"
    "lua/lua_display.cc"
    "lua/lua_json.cc"
    "lua/lua_tools.cc"
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
        "agent/agent_lua.cc"
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

# ── RL walk on the ESP32-S3 itself (main/rl/, ported from mpv2-uros) ──
# The policy runner, its /v1/policy routes (the app's RL Walk page) and the
# built-in policy file. rl/policies/getup.mpxp, when it is there, is built in
# too: the get-up policy behind the page's Get up and "never stays down"; and
# rl/policies/terrain.mpxp and rl/policies/sku1.mpxp: more walks the page can
# switch to (the boot picks the first one for APP_POLICY_ROBOT).
set(policy_files "")
if(CONFIG_APP_POLICY_ENABLE)
    list(APPEND sources
        "rl/policy_core.c"
        "rl/policy_run.cc"
    )
    list(APPEND policy_files "rl/policies/default.mpxp")
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/rl/policies/getup.mpxp")
        list(APPEND policy_files "rl/policies/getup.mpxp")
        set(policy_have_getup 1)
    endif()
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/rl/policies/terrain.mpxp")
        list(APPEND policy_files "rl/policies/terrain.mpxp")
        set(policy_have_terrain 1)
    endif()
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/rl/policies/sku1.mpxp")
        list(APPEND policy_files "rl/policies/sku1.mpxp")
        set(policy_have_sku1 1)
    endif()
endif()
list(APPEND sources "network/http_policy.cc")

# ── micro-ROS: the robot as a ROS 2 node on Wi-Fi (main/ros/, from mpv2-uros) ──
# Talks to a micro-ROS agent on a PC (UDP 8888); the client libraries are
# the prebuilt components/micro_ros. Its /v1/uros routes are the RL Walk
# page's ROS 2 card.
if(CONFIG_APP_UROS_ENABLE)
    list(APPEND sources
        "ros/mpx_uros.c"
        "ros/uros_node.c"
        "ros/uros_robot.cc"
        "ros/uros_joint_map.c"
    )
    if(CONFIG_APP_UROS_LIDAR_LD06)
        list(APPEND sources "ros/uros_lidar.c")
    endif()
endif()
list(APPEND sources "network/http_uros.cc")

# ── Phone app assets (pwa-claw/, built into pwa-claw/www) ────
# The mpv2-claw phone app. It carries a three.js model viewer and its 157 KB
# model, so the big files are embedded as .gz ONLY: every browser that can run
# this app accepts gzip, and static_handler() falls back to the .gz even for
# one that does not.
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

# ── App shell ─────────────────────────────────────────────────
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

# ── Register component ───────────────────────────────────────
idf_component_register(
    SRCS        ${sources}
    INCLUDE_DIRS "." "fs" "wasm" "sdk" "network" "robot" "lua" "platform" "util" "pi" "audio" "display" "voice" "agent" "rl" "ros"
    EMBED_FILES
        ${common_assets}
        ${design_assets}
        # The Display page's built-in animations, copied to <fs>/display at boot.
        "display/assets/blink.gif"
        "display/assets/smile.gif"
        "display/assets/sleepy.gif"
        ${policy_files}
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
        # micro-ROS client libraries (prebuilt); only linked when
        # CONFIG_APP_UROS_ENABLE compiles main/ros/ in.
        micro_ros
)

if(policy_have_getup)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MPX_POLICY_HAVE_GETUP=1)
endif()
if(policy_have_terrain)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MPX_POLICY_HAVE_TERRAIN=1)
endif()
if(policy_have_sku1)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MPX_POLICY_HAVE_SKU1=1)
endif()
# Re-run CMake when rl/policies/getup.mpxp appears or goes, so the build picks it up.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/rl/policies")
# The network runs 50 times a second on the policy task: speed over size for
# that one file (the project builds with -Os).
set_source_files_properties("rl/policy_core.c" PROPERTIES COMPILE_OPTIONS "-O2")

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