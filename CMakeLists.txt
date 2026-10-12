# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(MPX-Dog)

add_custom_target(mpx_build_pwa ALL
    COMMAND npm run build
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/pwa-claw"
    VERBATIM
)
add_dependencies(__idf_main mpx_build_pwa)

# ── esp-sr on Windows (local-claw) ───────────────────────────────────────────
#
# esp-sr's model packer (model/movemodel.py) prints a box-drawing rule in its
# report, which raises UnicodeEncodeError on a Windows console using the
# cp1252 codec -- failing the build at "Move and Pack models", long before
# any of our code, with an error that looks nothing like its cause. Setting
# PYTHONIOENCODING fixes it but has to be remembered in every shell.
#
# Tell its stdout to substitute characters it cannot encode instead of
# raising. Patched after project(), because that is when the component
# manager has populated managed_components/, and re-applied on every
# configure so a re-downloaded component does not quietly bring the bug
# back. Lifted from mpv2-claw's top-level CMakeLists.txt.
function(mpx_patch_file_replace_literal FILE_PATH OLD_TEXT NEW_TEXT PATCH_NAME)
    if(NOT EXISTS "${FILE_PATH}")
        message(STATUS "patch '${PATCH_NAME}' skipped: ${FILE_PATH} not present")
        return()
    endif()
    file(READ "${FILE_PATH}" FILE_CONTENT)
    string(FIND "${FILE_CONTENT}" "${NEW_TEXT}" FIXED_OFFSET)
    if(NOT FIXED_OFFSET EQUAL -1)
        return()    # already applied
    endif()
    string(FIND "${FILE_CONTENT}" "${OLD_TEXT}" OLD_OFFSET)
    if(OLD_OFFSET EQUAL -1)
        message(WARNING "patch '${PATCH_NAME}' skipped: source pattern not found in ${FILE_PATH}")
        return()
    endif()
    string(REPLACE "${OLD_TEXT}" "${NEW_TEXT}" FILE_CONTENT "${FILE_CONTENT}")
    file(WRITE "${FILE_PATH}" "${FILE_CONTENT}")
    message(STATUS "Applied patch '${PATCH_NAME}'")
endfunction()

mpx_patch_file_replace_literal(
    "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp-sr/model/movemodel.py"
    [=[sys.dont_write_bytecode = True]=]
    [=[sys.dont_write_bytecode = True
if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(errors='replace')]=]
    "esp_sr_movemodel_console_encoding"
)

# ── The agent's read-only `system` partition (local-claw, Phase 5) ──────────
#
# ESP-Claw keeps two filesystem roots: DATA (writable; here <fs>/agent on the
# `storage` LittleFS) and SYSTEM (read-only; the `system` partition, mounted
# at /system). SYSTEM carries what the agent needs at boot and must not be
# able to lose or corrupt:
#
#   skills/<id>/SKILL.md   every skill shipped by a component in the build,
#                          collected by ESP-Claw's skill_builder from each
#                          component's skills/ directory (main/skills/ has
#                          the robot's: mpx_robot, mpx_skills, mpx_display,
#                          mpx_music; ESP-Claw's own ride along).
#   .recovery/             seeds for memory/, router_rules/ and scheduler/,
#                          copied into DATA at boot when a file is missing
#                          (main/agent/agent_fs.cc). agent_fs/system/ is the
#                          source.
#
# Staged into build/system_fs/ -- never into the source tree -- and packed by
# joltwallet/littlefs's image tool, which `idf.py flash` writes along with
# the app (FLASH_IN_PROJECT). The image is rebuilt on every build (the tool
# cannot watch a directory), which costs a second.
set(MPX_SYSTEM_FS_SOURCE "${CMAKE_SOURCE_DIR}/agent_fs/system")
set(MPX_SYSTEM_FS_STAGING "${CMAKE_BINARY_DIR}/system_fs")

add_custom_target(mpx_prepare_system_fs ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${MPX_SYSTEM_FS_STAGING}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MPX_SYSTEM_FS_STAGING}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${MPX_SYSTEM_FS_SOURCE}" "${MPX_SYSTEM_FS_STAGING}"
    VERBATIM
)

littlefs_create_partition_image(system "${MPX_SYSTEM_FS_STAGING}" FLASH_IN_PROJECT
    DEPENDS mpx_prepare_system_fs)

skill_builder_configure_skill_sync(
    TARGET littlefs_system_bin
    SKILL_OUTPUT_DIR "${MPX_SYSTEM_FS_STAGING}/skills"
)
# The sync writes into the staging tree, so it has to run after the tree is
# (re)created, and the image has to wait for both.
add_dependencies(skill_builder_sync_skills mpx_prepare_system_fs)

# skill_builder ships every skill of every component in the build, and its own
# three are written for ESP-Claw's Lua runtime, which this firmware leaves out
# (CONFIG_APP_CLAW_CAP_LUA=n, see sdkconfig.defaults). skill_creator teaches
# the agent to write `require("arg_schema")` scripts nothing here can run, and
# the Skills Lab pair installs such scripts from the hub. Dropped from the
# image after the sync; main/skills/mpx_skill_creator replaces skill_creator.
set(MPX_SYSTEM_SKILLS_EXCLUDED skill_creator skills_lab_downloader skills_lab_search)
set(mpx_excluded_skill_dirs "")
foreach(skill_id IN LISTS MPX_SYSTEM_SKILLS_EXCLUDED)
    list(APPEND mpx_excluded_skill_dirs "${MPX_SYSTEM_FS_STAGING}/skills/${skill_id}")
endforeach()
add_custom_target(mpx_prune_system_skills ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${mpx_excluded_skill_dirs}
    VERBATIM
)
add_dependencies(mpx_prune_system_skills skill_builder_sync_skills)
add_dependencies(littlefs_system_bin mpx_prune_system_skills)
