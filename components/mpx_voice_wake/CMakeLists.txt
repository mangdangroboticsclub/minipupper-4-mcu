# esp-sr is deliberately NOT in REQUIRES.
#
# main/idf_component.yml pulls espressif/esp-sr in under
# `rules: $CONFIG{MP4_VOICE_ENABLE}`. The component manager evaluates those
# rules only from build/config/sdkconfig.json, and on the first pass of every
# configure it deliberately does not load that file (it may be stale, or not
# exist yet). So on that pass every `rules:`-gated dependency is dropped, the
# manager exits 10, and ESP-IDF's project.cmake finishes the pass far enough
# to generate sdkconfig.json and then runs idf_build_process() again. That
# second pass is where esp-sr actually arrives.
#
# The first pass still runs requirement expansion, though, and this component
# is auto-discovered from components/ whether or not the manifest lists it.
# A hard `REQUIRES espressif__esp-sr` here therefore fails on pass one, every
# time, before the retry that would have fixed it ever happens:
#
#     Failed to resolve component 'espressif__esp-sr'
#     required by component 'mpx_voice_wake': unknown name
#
# Nothing about the board, the sdkconfig or dependencies.lock is wrong when
# that appears; it is purely the ordering of pass one. So the dependency is
# attached after registration with idf_component_optional_requires(), the
# way ESP-IDF's own components (esp_system among others) attach dependencies
# that may or may not be in the build. Code after idf_component_register()
# does not run during the early expansion pass, and the real pass only
# happens once the component manager is done, so the check sees the final
# component list.
#
# The source list is gated on CONFIG_MP4_VOICE_ENABLE for the same reason:
# mpx_voice_wake.c includes esp-sr headers, and on a board with voice off
# there is no esp-sr to include them from. Kconfig values are available at
# this point of the real pass (they are missing only during early expansion,
# where SRCS is not used), so this is safe -- unlike gating REQUIRES on
# CONFIG_..., which is not (see the note in mpx_audio's CMakeLists.txt).
if(CONFIG_MP4_VOICE_ENABLE)
    set(srcs "src/mpx_voice_wake.c")
else()
    set(srcs "")
endif()

idf_component_register(
    SRCS
        ${srcs}
    INCLUDE_DIRS
        "include"
    REQUIRES
        mpx_audio
        mpx_voice_link
        console
        json
        esp_hw_support
        esp_driver_gpio
)

# Managed components are named <namespace>__<name> once they are in the
# build -- see mpx_voice_link's CMakeLists.txt for the same note about
# espressif__esp_websocket_client.
idf_component_optional_requires(PRIVATE espressif__esp-sr)

# Voice on but esp-sr absent means main/idf_component.yml no longer gates
# espressif/esp-sr on MP4_VOICE_ENABLE, or the two have drifted apart. Say so
# here, where the cause is visible, rather than failing later with
# "esp_afe_sr_iface.h: No such file or directory".
if(CONFIG_MP4_VOICE_ENABLE)
    idf_build_get_property(build_components BUILD_COMPONENTS)
    if(NOT "espressif__esp-sr" IN_LIST build_components)
        message(FATAL_ERROR
            "CONFIG_MP4_VOICE_ENABLE=y but espressif__esp-sr is not in the "
            "build. main/idf_component.yml is expected to pull it in with "
            "`rules: - if: $CONFIG{MP4_VOICE_ENABLE} == True`; check that "
            "entry is still there and still gated on the same symbol.")
    endif()
endif()
