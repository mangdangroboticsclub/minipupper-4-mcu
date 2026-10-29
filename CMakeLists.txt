# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(MPX-Dog)

# ── Phone app (pwa-claw): `npm run build` inside the IDF build ───────────────
#
# The phone app is a Vite/Svelte bundle whose output, pwa-claw/www/, is
# embedded into the firmware by main/CMakeLists.txt (EMBED_FILES). Without a
# build step here, `idf.py build` ships whatever www/ happened to contain.
#
# Two different moments matter, and handling only the second is what makes
# this look broken:
#
#   configure time  EMBED_FILES is resolved into one ninja edge per file by
#                   IDF's target_add_binary_data(), which takes the path as a
#                   MAIN_DEPENDENCY without checking that it exists. A tree
#                   with no www/ -- fresh clone (www/ is gitignored), a wiped
#                   build volume, `git clean -xdf` -- therefore dies at build
#                   time with "missing and no known rule to make it", before
#                   npm could ever create the files. So bootstrap the bundle
#                   here, at configure time, when it is absent.
#
#   build time      mpx_build_pwa runs npm only when the phone app sources
#                   changed since the last run (tools/pwa_build.cmake decides),
#                   and __idf_main depends on it -- that is what orders it
#                   before the *.gz.S embed objects and relinks libmain with
#                   the fresh assets. Skipping npm matters twice: it saves the
#                   Vite build, and it leaves the www/ mtimes alone, so ninja
#                   keeps the existing embed objects instead of re-assembling
#                   ~5 MB of assets and relinking the app. `idf.py flash` builds
#                   the app first, so without that check every flash paid for
#                   both, for no change at all.
#                   `idf.py` users get the same check *before* ninja starts
#                   (idf_ext.py, project-level idf.py extension) -- ninja
#                   decides what is dirty once at startup, so a web build run
#                   from inside the graph can only be relinked by the next
#                   build, which would flash the previous web app.
#
# -DMPX_BUILD_PWA=OFF builds against the www/ that is already on disk (no
# node/npm needed, e.g. CI that only checks the C side). For a one-off skip
# without reconfiguring use MPX_SKIP_PWA=1, and MPX_FORCE_PWA=1 rebuilds the
# phone app even when nothing changed.
option(MPX_BUILD_PWA
    "Build the phone app (pwa-claw) with npm as part of the firmware build" ON)

set(MPX_PWA_DIR "${CMAKE_SOURCE_DIR}/pwa-claw")
set(MPX_PWA_INDEX "${MPX_PWA_DIR}/www/index.html")
# Stamp holding the newest pwa-claw source mtime that www/ was built from, plus
# the script that compares them -- shared by the configure-time bootstrap below
# and the mpx_build_pwa target.
set(MPX_PWA_STAMP "${CMAKE_BINARY_DIR}/mpx_pwa.stamp")
set(MPX_PWA_SCRIPT "${CMAKE_SOURCE_DIR}/tools/pwa_build.cmake")

# npm is npm.cmd on Windows, next to node.exe.
find_program(MPX_NPM_EXECUTABLE NAMES npm npm.cmd npm.exe)

if(MPX_BUILD_PWA AND NOT MPX_NPM_EXECUTABLE)
    if(EXISTS "${MPX_PWA_INDEX}")
        message(WARNING
            "pwa-claw: npm not found -- the firmware will embed the existing "
            "${MPX_PWA_DIR}/www unchanged. Install Node.js to rebuild it.")
    else()
        message(FATAL_ERROR
            "pwa-claw: www/ is not built and npm was not found.\n"
            "Install Node.js (https://nodejs.org), or build the app by hand "
            "(cd pwa-claw && npm ci && npm run build) and configure with "
            "-DMPX_BUILD_PWA=OFF.")
    endif()
    set(MPX_BUILD_PWA OFF)
endif()

if(MPX_BUILD_PWA AND NOT EXISTS "${MPX_PWA_INDEX}")
    message(STATUS "pwa-claw: www/ is missing, building it now (it is needed at configure time)")
    # Same script the mpx_build_pwa target runs, so `npm ci` / `npm run build`
    # and the stamp live in one place. It runs npm here because www/ is absent.
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
                "-DMPX_PWA_DIR=${MPX_PWA_DIR}"
                "-DMPX_NPM_EXECUTABLE=${MPX_NPM_EXECUTABLE}"
                "-DMPX_PWA_STAMP=${MPX_PWA_STAMP}"
                -P "${MPX_PWA_SCRIPT}"
        RESULT_VARIABLE mpx_pwa_result)
    if(NOT mpx_pwa_result EQUAL 0)
        message(FATAL_ERROR "pwa-claw: building www/ failed (${mpx_pwa_result})")
    endif()
endif()

if(NOT EXISTS "${MPX_PWA_INDEX}")
    message(FATAL_ERROR
        "pwa-claw: ${MPX_PWA_INDEX} is missing and MPX_BUILD_PWA=OFF, so the "
        "firmware has no web UI to embed.\n"
        "Build it with: cd pwa-claw && npm ci && npm run build")
endif()

if(MPX_BUILD_PWA)
    # No-op unless a pwa-claw source, config or the lockfile is newer than the
    # stamp, so `idf.py build` and `idf.py flash` only pay for Vite when the
    # phone app actually changed.
    add_custom_target(mpx_build_pwa ALL
        COMMAND "${CMAKE_COMMAND}"
                "-DMPX_PWA_DIR=${MPX_PWA_DIR}"
                "-DMPX_NPM_EXECUTABLE=${MPX_NPM_EXECUTABLE}"
                "-DMPX_PWA_STAMP=${MPX_PWA_STAMP}"
                -P "${MPX_PWA_SCRIPT}"
        COMMENT "pwa-claw: npm run build only if the phone app changed"
        USES_TERMINAL
        VERBATIM
    )
    # __idf_main embeds pwa-claw/www, so the app must be rebuilt before it.
    add_dependencies(__idf_main mpx_build_pwa)
endif()

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
