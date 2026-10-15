# C++ because the Opus wrappers are C++; the surface exposed to the rest of
# the firmware stays plain C, declared in include/mpx_voice_link.h.
idf_component_register(
    SRCS
        "src/mpx_voice_codec.cc"
        "src/mpx_voice_ws.c"
        "src/mpx_voice_ota.c"
        "src/mpx_voice_stream.c"
        "src/mpx_voice_cmd.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        mpx_audio
        console
        json
        esp_hw_support
        esp_wifi
        esp_http_client
        esp-tls
        nvs_flash
        app_update
        # Managed components are named <namespace>__<name> once they are in
        # the build, so the plain registry name does not resolve:
        #
        #     Failed to resolve component 'esp_websocket_client'
        #     required by component 'mpx_voice_link': unknown name
        #
        # Same convention as 78__esp-opus above.
        espressif__esp_websocket_client
        # 78__esp-opus is NOT redundant.
        #
        # 78/esp-opus-encoder keeps 78/esp-opus in its PRIVATE requirements,
        # so opus.h is on that component's own include path and nobody
        # else's. Its public header opus_decoder.h then does
        # `#include "opus.h"`, and any component that includes it fails with
        #
        #     fatal error: opus.h: No such file or directory
        #
        # The reference firmware never hits this because it uses the wrappers
        # from `main`, which implicitly requires every component in the
        # build. A separate component has to ask.
        #
        # Requiring it here rather than editing the managed component: files
        # under managed_components/ are re-downloaded and would silently lose
        # the change.
        78__esp-opus
)
