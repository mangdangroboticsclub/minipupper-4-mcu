cmake_minimum_required(VERSION 3.16)

include("${CMAKE_SOURCE_DIR}/tools/cmake/esp_idf_patch.cmake")
include("${CMAKE_SOURCE_DIR}/tools/cmake/flash_partition_defaults.cmake")

# main's include path list alone passes Windows' 32767-character command-line
# limit, and ninja then fails with "CreateProcess: The parameter is incorrect".
# Response files carry the flags instead. Must be set before project().
set(CMAKE_NINJA_FORCE_RESPONSE_FILE ON CACHE BOOL "" FORCE)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mpv2_uros VERSION 0.1.0)

# esp-sr's model packer prints a box-drawing rule in its report, which raises
# UnicodeEncodeError on a Windows console using the cp1252 codec -- failing the
# build at "Move and Pack models", long before any of our code, with an error
# that looks nothing like its cause. Setting PYTHONIOENCODING fixes it but has
# to be remembered in every shell, which is not a fix.
#
# Tell its stdout to substitute characters it cannot encode instead of raising.
# Patched after project(), because that is when the component manager has
# populated managed_components/, and re-applied on every configure so a
# re-downloaded component does not quietly bring the bug back.
edge_agent_patch_file_replace_literal(
    "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp-sr/model/movemodel.py"
    [=[sys.dont_write_bytecode = True]=]
    [=[sys.dont_write_bytecode = True
if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(errors='replace')]=]
    "esp_sr_movemodel_console_encoding"
    ALLOW_MISSING
)

set(EDGE_AGENT_FATFS_IMAGE_ROOT "${CMAKE_SOURCE_DIR}/fatfs_image")
set(EDGE_AGENT_FATFS_SOURCE_ROOT "${EDGE_AGENT_FATFS_IMAGE_ROOT}/storage")
set(EDGE_AGENT_FATFS_BUILD_ROOT "${CMAKE_BINARY_DIR}/fatfs_image")
set(EDGE_AGENT_FATFS_SYSTEM_SOURCE_ROOT "${EDGE_AGENT_FATFS_IMAGE_ROOT}/system")
set(EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT "${CMAKE_BINARY_DIR}/system_fs_image")
set(EDGE_AGENT_BOARD_GENERATED_CMAKELISTS "${CMAKE_SOURCE_DIR}/components/gen_bmgr_codes/CMakeLists.txt")
set(EDGE_AGENT_FATFS_STORAGE_COPY_COMMANDS "")

set(EDGE_AGENT_BOARD_FATFS_SYSTEM_COPY_COMMANDS "")

if(EXISTS "${EDGE_AGENT_BOARD_GENERATED_CMAKELISTS}")
    file(STRINGS "${EDGE_AGENT_BOARD_GENERATED_CMAKELISTS}" EDGE_AGENT_BOARD_PATH_LINE
        REGEX "^message\\(STATUS \"Board Path: .+\"\\)$"
    )

    if(EDGE_AGENT_BOARD_PATH_LINE)
        list(GET EDGE_AGENT_BOARD_PATH_LINE 0 EDGE_AGENT_BOARD_PATH_LINE)
        string(REGEX REPLACE
            "^message\\(STATUS \"Board Path: (.*)\"\\)$"
            "\\1"
            EDGE_AGENT_BOARD_PATH
            "${EDGE_AGENT_BOARD_PATH_LINE}"
        )

        set(EDGE_AGENT_BOARD_FATFS_SOURCE_ROOT "${EDGE_AGENT_BOARD_PATH}/fatfs_image")

        if(EXISTS "${EDGE_AGENT_BOARD_FATFS_SOURCE_ROOT}")
            list(APPEND EDGE_AGENT_BOARD_FATFS_SYSTEM_COPY_COMMANDS
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "${EDGE_AGENT_BOARD_FATFS_SOURCE_ROOT}"
                        "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}"
            )
        endif()
    endif()
endif()

add_custom_target(edge_agent_prepare_fatfs_image ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${EDGE_AGENT_FATFS_BUILD_ROOT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${EDGE_AGENT_FATFS_BUILD_ROOT}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${EDGE_AGENT_FATFS_SOURCE_ROOT}"
            "${EDGE_AGENT_FATFS_BUILD_ROOT}"
    ${EDGE_AGENT_FATFS_STORAGE_COPY_COMMANDS}
    VERBATIM
)

# ── Web bundles -> the system partition ───────────────────────────────────
#
# Both UIs are streamed off the read-only system partition rather than embedded
# in the app image with EMBED_FILES. The PWA's bundle is ~126 KB gzipped and
# ESP-Claw's SolidJS one ~73 KB; as .rodata that is 199 KB of a 5 MB app slot
# spent on files that never execute -- and paid twice, because ota_1 is the
# same size. On the system partition (2600 KB, roughly half of it free) they
# cost the app image nothing, and a UI change reflashes in seconds without
# rebuilding firmware.
#
# Staged into the build-side staging tree, not into fatfs_image/ itself, so a
# build never writes into the source tree.
set(EDGE_AGENT_WEB_ASSET_COPY_COMMANDS "")
# The web app is mpv2's own (MPX-Dog pwa-redesign), copied into
# frontend_pwa_mpv2/ so mpv2 robots look and work the same on this firmware.
# mp4-claw's app is still in frontend_pwa/: point this back at it to switch.
# Rebuild after a UI change:  cd components/http_server/frontend_pwa_mpv2 &&
#                             npm ci && npm run build
set(EDGE_AGENT_PWA_WWW_DIR "${CMAKE_SOURCE_DIR}/components/http_server/frontend_pwa_mpv2/www")
set(EDGE_AGENT_SETTINGS_HTML_GZ
    "${CMAKE_SOURCE_DIR}/components/http_server/frontend_source/dist/index.html.gz")

if(EXISTS "${EDGE_AGENT_PWA_WWW_DIR}")
    # Only what the robot serves: the .gz of every file that has one (the web
    # server sends those with Content-Encoding: gzip), plus anything without a
    # .gz, e.g. fonts. The uncompressed twins the npm build leaves next to them
    # would double the space the app takes on the system partition.
    file(GLOB_RECURSE _pwa_files RELATIVE "${EDGE_AGENT_PWA_WWW_DIR}"
         CONFIGURE_DEPENDS "${EDGE_AGENT_PWA_WWW_DIR}/*")
    list(APPEND EDGE_AGENT_WEB_ASSET_COPY_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E make_directory "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}/www")
    foreach(_f IN LISTS _pwa_files)
        if(NOT _f MATCHES "\\.gz$" AND EXISTS "${EDGE_AGENT_PWA_WWW_DIR}/${_f}.gz")
            continue()
        endif()
        get_filename_component(_d "${_f}" DIRECTORY)
        list(APPEND EDGE_AGENT_WEB_ASSET_COPY_COMMANDS
            COMMAND ${CMAKE_COMMAND} -E make_directory "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}/www/${_d}"
            COMMAND ${CMAKE_COMMAND} -E copy "${EDGE_AGENT_PWA_WWW_DIR}/${_f}"
                    "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}/www/${_f}")
    endforeach()
else()
    message(WARNING
        "PWA bundle not found at ${EDGE_AGENT_PWA_WWW_DIR}\n"
        "The robot will boot and serve /v1, but the web UI will be missing.\n"
        "Build it with:  cd components/http_server/frontend_pwa_mpv2 && npm ci && npm run build")
endif()

if(EXISTS "${EDGE_AGENT_SETTINGS_HTML_GZ}")
    list(APPEND EDGE_AGENT_WEB_ASSET_COPY_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy
                "${EDGE_AGENT_SETTINGS_HTML_GZ}"
                "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}/settings/index.html.gz"
    )
endif()

# Prepare the read-only system partition build tree: mirror the source
# system_fs_image/ into a build-side staging dir that skill/lua syncs
# can also write into before fatfsgen packs it.
add_custom_target(edge_agent_prepare_system_fs_image ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${EDGE_AGENT_FATFS_SYSTEM_SOURCE_ROOT}"
            "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}"
    ${EDGE_AGENT_BOARD_FATFS_SYSTEM_COPY_COMMANDS}
    ${EDGE_AGENT_WEB_ASSET_COPY_COMMANDS}
    VERBATIM
)

# PRESERVE_TIME: the web server's ETag is size + mtime. Without real
# timestamps every file is dated 1980-01-01, so a rebuilt m.js that happens
# to keep the same size gets the same ETag, the browser's If-None-Match is
# answered 304, and it keeps showing the old UI after a flash.
fatfs_create_rawflash_image(system "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}" FLASH_IN_PROJECT PRESERVE_TIME)
fatfs_create_spiflash_image(storage "${EDGE_AGENT_FATFS_BUILD_ROOT}" FLASH_IN_PROJECT)

# Skill / Lua resources are now baked into the read-only system partition
# so storage can be reformatted at runtime without losing them.
skill_builder_configure_skill_sync(
    TARGET fatfs_system_bin
    SKILL_OUTPUT_DIR "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}/skills"
)

lua_module_builder_configure_builtin_lua_sync(
    TARGET fatfs_system_bin
    BUILTIN_OUTPUT_DIR "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}/scripts/builtin"
    DOCS_OUTPUT_DIR "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}/scripts/docs"
)

add_dependencies(skill_builder_sync_skills edge_agent_prepare_system_fs_image)
add_dependencies(lua_module_builder_sync_builtin_scripts edge_agent_prepare_system_fs_image)
add_dependencies(lua_module_builder_sync_docs edge_agent_prepare_system_fs_image)
add_dependencies(fatfs_system_bin edge_agent_prepare_system_fs_image)
add_dependencies(fatfs_storage_bin edge_agent_prepare_fatfs_image)
