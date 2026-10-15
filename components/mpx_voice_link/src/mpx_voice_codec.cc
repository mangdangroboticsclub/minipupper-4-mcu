/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mpx_voice_link.h"

#include <cinttypes>
#include <cstring>
#include <memory>
#include <vector>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mpx_audio.h"
#include "opus_decoder.h"
#include "opus_encoder.h"

static const char *TAG = "mpx_voice";

namespace {

std::unique_ptr<OpusEncoderWrapper> s_encoder;
std::unique_ptr<OpusDecoderWrapper> s_decoder;

}  // namespace

extern "C" esp_err_t mpx_voice_codec_start(uint32_t encode_rate, uint32_t decode_rate)
{
    if (s_encoder && s_decoder) {
        return ESP_OK;
    }
    s_encoder = std::make_unique<OpusEncoderWrapper>((int)encode_rate, 1, MPX_VOICE_FRAME_MS);
    s_decoder = std::make_unique<OpusDecoderWrapper>((int)decode_rate, 1, MPX_VOICE_FRAME_MS);
    if (!s_encoder || !s_decoder) {
        s_encoder.reset();
        s_decoder.reset();
        return ESP_ERR_NO_MEM;
    }

    /* Complexity 0, as the reference firmware sets. Higher settings buy a
     * little quality for a lot of CPU, and on a device that also runs an
     * acoustic front end and a TLS socket the CPU is worth more. */
    s_encoder->SetComplexity(0);

    ESP_LOGI(TAG, "opus ready: encode %" PRIu32 " Hz, decode %" PRIu32 " Hz, %d ms frames",
             encode_rate, decode_rate, MPX_VOICE_FRAME_MS);
    return ESP_OK;
}

extern "C" void mpx_voice_codec_stop(void)
{
    s_encoder.reset();
    s_decoder.reset();
}

extern "C" bool mpx_voice_codec_running(void)
{
    return s_encoder != nullptr && s_decoder != nullptr;
}

extern "C" int mpx_voice_encode(const int16_t *pcm, size_t frames,
                                uint8_t *out, size_t out_size)
{
    if (!s_encoder || !pcm || !out) {
        return -ESP_ERR_INVALID_STATE;
    }

    /* The wrapper takes ownership of the PCM vector, so this copy is not
     * avoidable without changing the library. One 60 ms frame is 1920 bytes. */
    std::vector<int16_t> in(pcm, pcm + frames);
    std::vector<uint8_t> packet;

    if (!s_encoder->Encode(std::move(in), packet)) {
        return -ESP_FAIL;
    }
    if (packet.size() > out_size) {
        ESP_LOGW(TAG, "opus packet %u B does not fit in %u B",
                 (unsigned)packet.size(), (unsigned)out_size);
        return -ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, packet.data(), packet.size());
    return (int)packet.size();
}

extern "C" int mpx_voice_decode(const uint8_t *packet, size_t len,
                                int16_t *pcm, size_t max_frames)
{
    if (!s_decoder || !packet || !pcm) {
        return -ESP_ERR_INVALID_STATE;
    }

    std::vector<uint8_t> in(packet, packet + len);
    std::vector<int16_t> out;

    if (!s_decoder->Decode(std::move(in), out)) {
        return -ESP_FAIL;
    }
    if (out.size() > max_frames) {
        return -ESP_ERR_INVALID_SIZE;
    }
    memcpy(pcm, out.data(), out.size() * sizeof(int16_t));
    return (int)out.size();
}

/* ── loopback ──────────────────────────────────────────────────────────────
 *
 * Record first, play afterwards. NOT both at once.
 *
 * The obvious version -- capture a frame, encode it, decode it, play it,
 * repeat -- is an acoustic feedback loop. The microphone and the speaker are
 * centimetres apart on the same board, the microphone has 30 dB of gain, and
 * there is no echo cancellation until Phase 4. It howls up to full scale
 * within a fraction of a second, which is unpleasant, tells you nothing about
 * the codec, and on this board pulled the 5 V rail down hard enough to drop
 * the USB serial connection.
 *
 * So: capture the whole clip through Opus into a buffer, stop the microphone,
 * and only then play it. Nothing is listening while the speaker is on, and
 * the test still proves encode and decode end to end by ear.
 *
 * Encode and decode run at the SAME rate here, which the protocol does not --
 * uplink is 16 kHz, downlink 24 kHz. Matching them avoids needing a resampler
 * to test the codec. Rate conversion is the socket's job, in Phase 2b.
 */
#define MPX_VOICE_TASK_STACK  (2048 * 14)

namespace {

struct LoopbackArgs {
    uint32_t seconds;
    esp_err_t result;
    SemaphoreHandle_t done;
};

esp_err_t loopback_body(uint32_t seconds);

void loopback_task(void *arg)
{
    LoopbackArgs *args = (LoopbackArgs *)arg;
    args->result = loopback_body(args->seconds);
    xSemaphoreGive(args->done);
    /* Created with xTaskCreateWithCaps, so it must be destroyed with the
     * matching call -- the plain vTaskDelete would leak the stack. */
    vTaskDeleteWithCaps(nullptr);
}

}  // namespace

extern "C" esp_err_t mpx_voice_loopback(uint32_t seconds)
{
    LoopbackArgs args = { seconds, ESP_FAIL, nullptr };

    args.done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(args.done, ESP_ERR_NO_MEM, TAG, "no memory for semaphore");

    /* Stack in PSRAM.
     *
     * 28 KB contiguous is more than internal RAM has spare on this firmware --
     * the measured largest free block is around 63 KB at boot and much less
     * once Wi-Fi, the web server and the agent are running, so this failed
     * outright with ESP_ERR_NO_MEM.
     *
     * External RAM is the normal answer here rather than a workaround:
     * ESP-Claw already puts task stacks there by default
     * (claw_task_memory_caps returns MALLOC_CAP_SPIRAM for every policy but
     * INTERNAL_ONLY), and CONFIG_SPIRAM_XIP_FROM_PSRAM is on, which is what
     * makes a PSRAM stack safe around flash access. See the note in
     * AGENTS.md -- that setting is load-bearing, and this is one of the
     * things leaning on it.
     *
     * Internal is the fallback for a build without PSRAM, where Opus would be
     * the least of the problems. */
    BaseType_t ok = xTaskCreateWithCaps(loopback_task, "opus_loop",
                                        MPX_VOICE_TASK_STACK, &args, 5, nullptr,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ok = xTaskCreateWithCaps(loopback_task, "opus_loop",
                                 MPX_VOICE_TASK_STACK, &args, 5, nullptr,
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (ok != pdPASS) {
        vSemaphoreDelete(args.done);
        ESP_LOGE(TAG, "could not create the codec task: %d bytes of stack is "
                      "more than either heap has left", MPX_VOICE_TASK_STACK);
        return ESP_ERR_NO_MEM;
    }

    /* Wait for it rather than returning immediately, so the console prompt
     * comes back when the loopback is actually finished. */
    xSemaphoreTake(args.done, portMAX_DELAY);
    vSemaphoreDelete(args.done);
    return args.result;
}

namespace {

esp_err_t loopback_body(uint32_t seconds)
{
    const uint32_t rate = MPX_VOICE_UPLINK_RATE;
    const size_t frame = MPX_VOICE_UPLINK_FRAME;
    esp_err_t ret = ESP_OK;
    uint32_t packets = 0;
    uint32_t bytes = 0;

    ESP_RETURN_ON_ERROR(mpx_voice_codec_start(rate, rate), TAG, "codec start failed");

    const uint32_t total_frames = (seconds * 1000u) / MPX_VOICE_FRAME_MS;
    const size_t   total_samples = total_frames * frame;

    /* The decoded clip, held whole so playback happens after capture. 8 s of
     * 16 kHz mono is 256 KB -- far too much for internal RAM, and nothing
     * here DMAs out of it, so PSRAM is the right home. */
    int16_t *clip = (int16_t *)heap_caps_malloc(total_samples * sizeof(int16_t),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(clip, ESP_ERR_NO_MEM, TAG,
                        "no room for a %" PRIu32 " s clip", seconds);

    std::vector<int16_t> pcm(frame);
    std::vector<int16_t> back(frame * 2);
    std::vector<uint8_t> packet(MPX_VOICE_PACKET_MAX);
    size_t filled = 0;

    ret = mpx_audio_capture_start(rate, 2, 0);
    if (ret != ESP_OK) {
        free(clip);
        return ret;
    }

    ESP_LOGI(TAG, "recording %" PRIu32 " s through Opus -- speak now", seconds);

    for (uint32_t i = 0; i < total_frames; i++) {
        ret = mpx_audio_capture_read(pcm.data(), frame);
        if (ret != ESP_OK) {
            break;
        }

        const int len = mpx_voice_encode(pcm.data(), frame, packet.data(), packet.size());
        if (len < 0) {
            ESP_LOGE(TAG, "encode failed");
            ret = ESP_FAIL;
            break;
        }
        packets++;
        bytes += (uint32_t)len;

        const int got = mpx_voice_decode(packet.data(), (size_t)len,
                                         back.data(), back.size());
        if (got < 0) {
            ESP_LOGE(TAG, "decode failed");
            ret = ESP_FAIL;
            break;
        }
        if (filled + (size_t)got <= total_samples) {
            memcpy(clip + filled, back.data(), (size_t)got * sizeof(int16_t));
            filled += (size_t)got;
        }
    }

    /* Microphone off BEFORE the speaker comes on. This ordering is the whole
     * point of the exercise. */
    mpx_audio_capture_stop();

    if (packets) {
        ESP_LOGI(TAG, "%" PRIu32 " packets, %" PRIu32 " bytes, average %" PRIu32
                      " B/frame, about %" PRIu32 " kbit/s",
                 packets, bytes, bytes / packets,
                 (bytes * 8u) / (packets * MPX_VOICE_FRAME_MS));
    }

    if (ret == ESP_OK && filled) {
        ESP_LOGI(TAG, "playing it back (%u samples)", (unsigned)filled);
        ret = mpx_audio_output_start(rate);
        if (ret == ESP_OK) {
            for (size_t off = 0; off < filled; off += MPX_AUDIO_MAX_FRAMES) {
                const size_t n = (filled - off < MPX_AUDIO_MAX_FRAMES)
                                 ? (filled - off) : MPX_AUDIO_MAX_FRAMES;
                if (mpx_audio_output_write(clip + off, n) != ESP_OK) {
                    break;
                }
            }
            mpx_audio_output_stop();
        }
    }

    free(clip);
    return ret;
}

}  // namespace
