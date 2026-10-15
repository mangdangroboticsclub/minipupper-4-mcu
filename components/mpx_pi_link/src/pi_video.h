/* pi_video.h - the Pi's camera / tracking picture on the robot's screen.
 *
 * The bridge sends each frame as a small baseline JPEG (<= 320x240) split into
 * PI_MSG_VIDEO_DATA chunks. This module puts the chunks back together (on the
 * Pi link task) and hands complete frames to its own task, which passes them
 * to the sink main.c registered (cap_display_show_jpeg: decode + show).
 * Newest frame wins: if the screen is still busy with the previous one, a
 * new frame is dropped rather than queued.
 *
 * Ported from mpv2 main/pi/pi_video.{h,cc}; mpv2 called display::show_jpeg()
 * directly, here the display is a sink so this component does not depend on
 * cap_display.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace pi_video {

void chunk(const uint8_t *payload, uint8_t len);   // one PI_MSG_VIDEO_DATA payload

// Where complete JPEGs go; nullptr (the default) drops them unassembled.
void set_sink(esp_err_t (*sink)(const uint8_t *jpeg, size_t len));

struct Stats {
    uint32_t frames_shown;
    uint32_t frames_dropped;     // gap in the chunks, or the screen was busy
    uint32_t decode_errors;
};
Stats stats();

}  // namespace pi_video
