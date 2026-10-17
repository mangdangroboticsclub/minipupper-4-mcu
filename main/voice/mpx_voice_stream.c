/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * Phase 2's missing half. `voice loopback` proved the codec by buffering a
 * whole clip locally; this streams frames over the real socket instead --
 * mic -> Opus -> binary WebSocket frame outbound, binary frame -> Opus ->
 * speaker inbound.
 *
 * Two ways in. `voice talk` is push-to-talk: the mic is opened, closed
 * again before any reply audio exists, and "listen" is manual-mode -- the
 * server does not run ASR until it sees {"type":"listen","state":"stop"}.
 * mpx_voice_talk_start_auto(), which mpx_voice_wake calls after the wake
 * word, is listen-mode "auto" instead: the server's VAD decides when you
 * have finished, and the first sign it has taken its turn ("stt", or "tts"
 * start) is what closes the microphone here. Either way capture and
 * playback never overlap on THIS component's account -- the reply plays
 * after the mic is closed. What may hold the mic open during a reply is
 * mpx_voice_wake's echo-cancelled front end, listening for barge-in.
 *
 * TX runs inline on its own task -- reading, encoding and sending share one
 * loop, since each blocking capture read already paces it at ~60ms/frame.
 * RX is two stages: the WebSocket event handler (mpx_voice_ws.c) just
 * copies each binary frame into this component's queue and returns
 * immediately, and a separate task here decodes and plays it. That split
 * exists because opus decode, like opus encode, wants real stack -- doing
 * it inline in the WebSocket client's own event task is how the Opus
 * loopback crash happened earlier in this project, on a task nowhere near
 * this hungry.
 */

#include "mpx_voice_link.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mpx_audio.h"

static const char *TAG = "mpx_voice_stream";

/* Started at the same size as MPX_VOICE_TASK_STACK in mpx_voice_codec.cc
 * (28672) on the theory that talk_task and playback_task call into the same
 * Opus wrappers on their own stack, so they'd need the same headroom
 * loopback does. That theory was wrong for talk_task specifically: unlike
 * loopback, which only ever encodes and decodes locally, talk_task also
 * calls mpx_voice_send_binary() -- a synchronous TLS write, all the way
 * through esp_websocket_client into mbedtls's record-layer encryption --
 * from the very same stack frame each time around the loop, right after an
 * Opus encode call that alone was already sized to need most of a 28 KB
 * stack. Measured on real hardware: 28672 overflows within the first
 * `voice talk`, every time. Doubled with real margin left over rather than
 * inched up, because the failure mode of guessing wrong here again is a
 * silent memory-corruption crash, not a clean error return. PSRAM-backed
 * (see the allocation below), so the extra 28 KB costs nothing that
 * matters. */
#define VOICE_STREAM_TASK_STACK  (2048 * 28)
/* Downlink frames waiting to be played. The server does not pace TTS
 * audio: it sends each sentence as one burst, and a long sentence is far
 * more than the 24 frames (1.4 s) the first cut held --
 *
 *   W mpx_voice_stream: RX queue full -- dropping a downlink frame   (x30)
 *
 * -- which is what a reply that stutters and skips words sounds like.
 * 300 frames is 18 s of audio; the queue is pointers only (the packets
 * themselves are malloc'd, i.e. PSRAM), so the depth is nearly free. */
#define VOICE_STREAM_RX_DEPTH    300
/* Frames (60 ms each) to have in hand before a reply starts playing. */
#define VOICE_STREAM_PREBUFFER   7

/* What cap_display's ES7210 recording has been validated with all through
 * Phase 1 (see mpx_audio_cmd.c's `audio rec` defaults) -- 2 channels, pick
 * channel 0. Reusing known-good numbers rather than picking new ones. */
#define TALK_CAPTURE_CHANNELS  2
#define TALK_CAPTURE_PICK      0

typedef struct {
    uint8_t *data;  /* NULL is the "tts finished" sentinel */
    size_t len;
} voice_rx_frame_t;

static QueueHandle_t s_rx_queue;
static bool s_playback_open;
static bool s_stream_ready;

static volatile bool s_talk_active;
static volatile bool s_talk_stop_requested;
static volatile bool s_talk_auto;        /* this talk is listen-mode "auto" */
static volatile bool s_server_turn;      /* the server answered -- stop streaming */
static volatile uint32_t s_tts_stops;    /* replies finished, see the header */
static volatile bool s_stop_pending;     /* a "tts stop" arrived; close once the queue drains */
static SemaphoreHandle_t s_talk_done;

/* ── setup ───────────────────────────────────────────────────────────────*/

static void playback_task(void *arg);

esp_err_t mpx_voice_stream_init(void)
{
    if (s_stream_ready) {
        return ESP_OK;
    }

    /* Queue storage in PSRAM: 300 slots of 8 bytes is not much, but internal
     * RAM is the one thing this board is short of (see the boot log's heap
     * lines) and a queue does not need to be fast memory. */
    s_rx_queue = xQueueCreateWithCaps(VOICE_STREAM_RX_DEPTH, sizeof(voice_rx_frame_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rx_queue) {
        s_rx_queue = xQueueCreateWithCaps(VOICE_STREAM_RX_DEPTH, sizeof(voice_rx_frame_t),
                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    ESP_RETURN_ON_FALSE(s_rx_queue, ESP_ERR_NO_MEM, TAG, "queue alloc failed");

    s_talk_done = xSemaphoreCreateBinary();
    if (!s_talk_done) {
        vQueueDeleteWithCaps(s_rx_queue);
        s_rx_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreateWithCaps(playback_task, "voice_rx", VOICE_STREAM_TASK_STACK,
                                        NULL, 5, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ok = xTaskCreateWithCaps(playback_task, "voice_rx", VOICE_STREAM_TASK_STACK,
                                 NULL, 5, NULL, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (ok != pdPASS) {
        vSemaphoreDelete(s_talk_done);
        vQueueDeleteWithCaps(s_rx_queue);
        s_talk_done = NULL;
        s_rx_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_stream_ready = true;
    ESP_LOGI(TAG, "streaming ready (talk/playback tasks up)");
    return ESP_OK;
}

/* ── RX: socket -> queue -> decode -> speaker ───────────────────────────── */

void mpx_voice_stream_rx_frame(const uint8_t *data, size_t len)
{
    if (!s_rx_queue || !data || !len) {
        return;
    }
    /* While we are the one talking and the server has not yet answered,
     * any audio it sends is the tail of an earlier reply -- typically the
     * frames still in flight after a barge-in abort. Playing them would
     * put the speaker on during capture with no echo path to cancel it. */
    if (s_talk_active && !s_server_turn) {
        return;
    }
    uint8_t *copy = malloc(len);
    if (!copy) {
        ESP_LOGW(TAG, "OOM copying a %u-byte downlink frame -- dropped", (unsigned)len);
        return;
    }
    memcpy(copy, data, len);

    const voice_rx_frame_t frame = { .data = copy, .len = len };
    if (xQueueSend(s_rx_queue, &frame, 0) != pdTRUE) {
        ESP_LOGW(TAG, "RX queue full -- dropping a downlink frame");
        free(copy);
    }
}

void mpx_voice_stream_on_tts_stop(void)
{
    s_tts_stops++;
    if (!s_rx_queue) {
        return;
    }
    /* Two ways to tell the playback task, because one was not enough: the
     * sentinel below is dropped if the queue happens to be full (the server
     * sends the start of a reply in a burst), and a dropped sentinel left
     * the speaker "open" forever -- mpx_voice_stream_is_playing() stayed
     * true, the hands-free loop waited its full minute for a reply that had
     * already ended, and the server, hearing nothing, said goodbye. The
     * flag cannot be dropped: playback_task closes the speaker when it is
     * set and the queue has drained. */
    s_stop_pending = true;
    const voice_rx_frame_t sentinel = { .data = NULL, .len = 0 };
    (void)xQueueSend(s_rx_queue, &sentinel, 0);
}

bool mpx_voice_stream_is_playing(void)
{
    return s_playback_open;
}

void mpx_voice_stream_on_server_turn(void)
{
    s_server_turn = true;
}

bool mpx_voice_talk_server_took_turn(void)
{
    return s_server_turn;
}

uint32_t mpx_voice_stream_tts_stop_count(void)
{
    return s_tts_stops;
}

void mpx_voice_stream_abort_playback(void)
{
    if (!s_rx_queue) {
        return;
    }
    /* Drop whatever is still queued -- barge-in wants silence now, not
     * "finish the sentence that's already buffered." xQueueReset() is safe
     * to call against a queue another task is blocked on receiving from;
     * the one frame playback_task may already have dequeued and be mid-
     * decode plays out regardless (at most ~60ms), which is an acceptable
     * barge-in latency. The sentinel behind it is what actually closes the
     * speaker, same as a normal end-of-reply. */
    (void)xQueueReset(s_rx_queue);
    s_stop_pending = true;
    const voice_rx_frame_t sentinel = { .data = NULL, .len = 0 };
    (void)xQueueSend(s_rx_queue, &sentinel, 0);
}

static void playback_task(void *arg)
{
    (void)arg;
    voice_rx_frame_t frame;
    int16_t pcm[MPX_VOICE_DOWNLINK_FRAME];

    for (;;) {
        if (xQueueReceive(s_rx_queue, &frame, pdMS_TO_TICKS(100)) != pdTRUE) {
            /* Nothing queued. If a "tts stop" has arrived, the reply is
             * over whether or not its sentinel made it into the queue. */
            if (s_stop_pending && s_playback_open) {
                mpx_audio_output_stop();
                s_playback_open = false;
            }
            if (s_stop_pending) {
                s_stop_pending = false;
            }
            continue;
        }

        if (!frame.data) {
            /* The "tts stop" sentinel: close the output stream rather than
             * leaving I2S running with nothing feeding it. */
            if (s_playback_open) {
                mpx_audio_output_stop();
                s_playback_open = false;
            }
            s_stop_pending = false;
            continue;
        }

        if (!s_playback_open) {
            s_stop_pending = false;
            /* First frame of a reply. Let a few more arrive before the
             * speaker starts: the server sends the opening of a reply in a
             * burst and then paces to real time, and with the I2S driver
             * clearing each DMA buffer as it goes (auto_clear), every gap
             * between frames is a gap in the audio -- the crackle heard at
             * the start of hands-free replies. ~250 ms of buffer absorbs
             * the jitter and is not a delay anyone notices. Raised to ~400 ms
             * (VOICE_STREAM_PREBUFFER, waiting at most 800 ms): over the
             * internet a 180 ms cushion still ran dry mid-word now and then,
             * and a sound cut off mid-wave is a click -- crackle. The server
             * sends a 1.2 s head start, so this fills at once. */
            for (int i = 0; i < 32 && uxQueueMessagesWaiting(s_rx_queue) < VOICE_STREAM_PREBUFFER; i++) {
                vTaskDelay(pdMS_TO_TICKS(25));
            }
            /* MPX_VOICE_PLAYBACK_RATE, not mpx_voice_downlink_rate(): the
             * decoder was opened at the playback rate in mpx_voice_talk_start(),
             * so this is what the PCM actually is -- and it is the only rate
             * the I2S peripheral will accept while the wake-word front end
             * holds the microphone open (see the header for the full story). */
            if (mpx_audio_output_start(MPX_VOICE_PLAYBACK_RATE) == ESP_OK) {
                s_playback_open = true;
            } else {
                ESP_LOGW(TAG, "cannot open speaker for playback");
            }
        }

        if (s_playback_open) {
            const int n = mpx_voice_decode(frame.data, frame.len, pcm, MPX_VOICE_DOWNLINK_FRAME);
            if (n > 0) {
                (void)mpx_audio_output_write(pcm, (size_t)n);
            } else if (n < 0) {
                ESP_LOGW(TAG, "decode failed: %d", n);
            }
        }

        free(frame.data);
    }
}

/* ── TX: mic -> encode -> binary frame -> socket ────────────────────────── */

static esp_err_t send_listen_state(const char *state, const char *mode)
{
    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root, ESP_ERR_NO_MEM, TAG, "out of memory");

    const char *sid = mpx_voice_session_id();
    if (sid && sid[0]) {
        cJSON_AddStringToObject(root, "session_id", sid);
    }
    cJSON_AddStringToObject(root, "type", "listen");
    cJSON_AddStringToObject(root, "state", state);
    if (mode) {
        cJSON_AddStringToObject(root, "mode", mode);
    }

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    ESP_RETURN_ON_FALSE(text, ESP_ERR_NO_MEM, TAG, "out of memory");

    const esp_err_t err = mpx_voice_send_json(text);
    free(text);
    return err;
}

static void talk_task(void *arg)
{
    const uint32_t max_seconds = (uint32_t)(uintptr_t)arg;
    const uint32_t max_frames = max_seconds ? (max_seconds * 1000u / MPX_VOICE_FRAME_MS) : 0;
    int16_t pcm[MPX_VOICE_UPLINK_FRAME];
    uint8_t packet[MPX_VOICE_PACKET_MAX];
    uint32_t frames_sent = 0;

    const esp_err_t open_err = mpx_audio_capture_start(MPX_VOICE_UPLINK_RATE,
                                                        TALK_CAPTURE_CHANNELS,
                                                        TALK_CAPTURE_PICK);
    if (open_err != ESP_OK) {
        ESP_LOGE(TAG, "capture start failed: %s", esp_err_to_name(open_err));
        s_talk_active = false;
        xSemaphoreGive(s_talk_done);
        vTaskDeleteWithCaps(NULL);
        return;
    }

    const bool auto_mode = s_talk_auto;
    (void)send_listen_state("start", auto_mode ? "auto" : "manual");
    ESP_LOGI(TAG, "listening -- speak now (%s)",
             auto_mode      ? "the server decides when you are done" :
             max_seconds    ? "will stop automatically" :
                              "run `voice talk stop` when done");

    while (!s_talk_stop_requested &&
           !(auto_mode && s_server_turn) &&
           (max_frames == 0 || frames_sent < max_frames)) {
        if (mpx_audio_capture_read(pcm, MPX_VOICE_UPLINK_FRAME) != ESP_OK) {
            ESP_LOGW(TAG, "capture read failed -- stopping");
            break;
        }
        const int packet_len = mpx_voice_encode(pcm, MPX_VOICE_UPLINK_FRAME, packet, sizeof(packet));
        if (packet_len > 0) {
            if (mpx_voice_send_binary(packet, (size_t)packet_len) != ESP_OK) {
                ESP_LOGW(TAG, "send failed -- socket down? stopping");
                break;
            }
        }
        frames_sent++;
    }

    mpx_audio_capture_stop();
    /* In auto mode the server has already decided the utterance is over by
     * the time it answers; telling it "stop" on top of that is at best
     * redundant and on some servers cancels the reply it is producing. Only
     * send it when WE ended the listen -- timeout, or an explicit stop. */
    if (!(auto_mode && s_server_turn)) {
        (void)send_listen_state("stop", NULL);
    }
    ESP_LOGI(TAG, "stopped listening (%" PRIu32 " frames sent, %.1fs%s)",
             frames_sent, (double)frames_sent * MPX_VOICE_FRAME_MS / 1000.0,
             (auto_mode && s_server_turn) ? ", server took its turn" : "");

    s_talk_active = false;
    xSemaphoreGive(s_talk_done);
    vTaskDeleteWithCaps(NULL);
}

static esp_err_t talk_start_common(uint32_t max_seconds, bool auto_mode)
{
    ESP_RETURN_ON_FALSE(mpx_voice_state() == MPX_VOICE_READY, ESP_ERR_INVALID_STATE, TAG,
                        "not connected -- run `voice connect` first");
    ESP_RETURN_ON_FALSE(!s_talk_active, ESP_ERR_INVALID_STATE, TAG, "already talking");
    /* The microphone is a single hardware stream (see mpx_audio_capture_active()'s
     * doc comment) -- if mpx_voice_wake's `wake start` already has it open,
     * talk_task's own capture_start() below would not fail, it would just
     * silently interleave reads with wake's, which is worse than an error
     * here. */
    ESP_RETURN_ON_FALSE(!mpx_audio_capture_active(), ESP_ERR_INVALID_STATE, TAG,
                        "microphone busy -- stop `wake` first (`wake stop`)");
    ESP_RETURN_ON_ERROR(mpx_voice_stream_init(), TAG, "stream init failed");

    /* Decode at MPX_VOICE_PLAYBACK_RATE regardless of what rate the server
     * said it encodes at (see the header): Opus resamples on the way out,
     * and the I2S peripheral will not run the speaker at a different rate
     * from the microphone the wake-word front end keeps open. The loopback
     * test opens the codec at the same rates, so "already running" is the
     * same configuration either way. */
    if (!mpx_voice_codec_running()) {
        ESP_RETURN_ON_ERROR(mpx_voice_codec_start(MPX_VOICE_UPLINK_RATE, MPX_VOICE_PLAYBACK_RATE),
                            TAG, "codec start failed");
    }

    /* A previous talk that nobody waited for leaves its "done" token in the
     * semaphore; take it now so mpx_voice_talk_wait() below waits for THIS
     * talk and not a stale one. */
    (void)xSemaphoreTake(s_talk_done, 0);

    s_talk_stop_requested = false;
    s_talk_auto = auto_mode;
    s_server_turn = false;
    s_talk_active = true;

    BaseType_t ok = xTaskCreateWithCaps(talk_task, "voice_talk", VOICE_STREAM_TASK_STACK,
                                        (void *)(uintptr_t)max_seconds, 5, NULL,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ok = xTaskCreateWithCaps(talk_task, "voice_talk", VOICE_STREAM_TASK_STACK,
                                 (void *)(uintptr_t)max_seconds, 5, NULL,
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (ok != pdPASS) {
        s_talk_active = false;
        ESP_LOGE(TAG, "could not create the talk task: %d bytes of stack is "
                      "more than either heap has left", VOICE_STREAM_TASK_STACK);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t mpx_voice_talk_start(uint32_t max_seconds)
{
    return talk_start_common(max_seconds, false);
}

esp_err_t mpx_voice_talk_start_auto(uint32_t max_seconds)
{
    return talk_start_common(max_seconds, true);
}

esp_err_t mpx_voice_talk_wait(uint32_t timeout_ms)
{
    if (!s_talk_active || !s_talk_done) {
        return ESP_OK;
    }
    if (xSemaphoreTake(s_talk_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    /* Put it back for anyone else waiting (mpx_voice_talk_stop() takes it
     * too); the next talk_start clears it. */
    (void)xSemaphoreGive(s_talk_done);
    return ESP_OK;
}

void mpx_voice_talk_stop(void)
{
    if (!s_talk_active) {
        return;
    }
    s_talk_stop_requested = true;
    /* The task notices within one capture read -- ~60ms -- so this is a
     * short wait, not a real block; the timeout is only so a wedged capture
     * driver cannot hang the console. */
    if (xSemaphoreTake(s_talk_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG, "talk task did not confirm stop within 2s");
    }
}

bool mpx_voice_talk_active(void)
{
    return s_talk_active;
}
