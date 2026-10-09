# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(MPX-Dog)

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
