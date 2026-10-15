/* pi_audio.h - music from the Raspberry Pi through the robot's speaker.
 *
 * Ported from mpv2 main/pi/pi_audio.h unchanged; the speaker is this
 * firmware's mpx_audio (same calls as mpv2's voice/mpx_audio).
 *
 * The Pi (mini_pupper_music -> esp_bridge) streams 16 kHz mono PCM over the
 * J-PI UART as PI_MSG_AUDIO_DATA frames, paced in real time. This module
 * buffers ~2 s in PSRAM and plays it with mpx_audio's I2S output. Only while
 * the Pi is in control: voice is off then, so the speaker is free.
 *
 * All calls come from the Pi link task; playback runs on its own task.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace pi_audio {

bool start(uint16_t sample_rate, uint8_t volume);   // false: unsupported rate / no speaker
void push(const uint8_t *pcm, size_t len);          // s16le mono
void end();                                         // play what is buffered, then stop
void stop();                                        // stop now
void set_volume(uint8_t volume);                    // 0..100

struct Stats {
    bool     playing;
    uint32_t buffered_ms;
    uint32_t underruns;
    uint32_t dropped_bytes;
};
Stats stats();

}  // namespace pi_audio
