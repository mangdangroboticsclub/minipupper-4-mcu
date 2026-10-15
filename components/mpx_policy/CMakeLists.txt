# An RL walk policy running on the robot (mpx_policy.h). The arithmetic is
# src/policy_core.c, which tools/policy/host_test checks against omniRL's
# numpy runner; policies/default.mpxp is built in (tools/policy/export_mpxp.py).
# policies/getup.mpxp, when it is there, is built in too: the get-up policy
# (omniRL docs/GETUP.md) behind `policy getup` and "never stays down".
set(MPX_POLICY_EMBED "policies/default.mpxp")
set(MPX_POLICY_GETUP_FILE "${CMAKE_CURRENT_LIST_DIR}/policies/getup.mpxp")
if(EXISTS "${MPX_POLICY_GETUP_FILE}")
    list(APPEND MPX_POLICY_EMBED "policies/getup.mpxp")
endif()

idf_component_register(
    SRCS
        "src/policy_core.c"
        "src/policy_run.cc"
        "src/policy_console.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "src"
    PRIV_REQUIRES
        console
        esp_timer
        nvs_flash
        mpx_robot
    EMBED_FILES
        ${MPX_POLICY_EMBED}
)

if(EXISTS "${MPX_POLICY_GETUP_FILE}")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MPX_POLICY_HAVE_GETUP=1)
endif()
# Re-run CMake when getup.mpxp appears or goes, so the build picks it up.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/policies")

# The network runs 50 times a second on the policy task: speed over size
# for that one file (the project builds with -Os).
set_source_files_properties("src/policy_core.c" PROPERTIES COMPILE_OPTIONS "-O2")
