/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * See include/mpx_voice_wake.h for what this is, its gate, and what is
 * deliberately not wired up yet.
 *
 * Shape of it: esp-sr's AFE (Audio Front End) does AEC + VAD + WakeNet in
 * one pipeline over a 2-channel [mic, reference] feed. Two tasks, the
 * pattern esp-sr's own examples use and for the reason they use it: feed()
 * enqueues raw audio into the AFE's internal ring buffer and returns
 * quickly, fetch() blocks (its own documented 2000ms timeout) pulling
 * enhanced audio and detection results back out, and those two rates are
 * not the same thing -- combining them into one task risks whichever one
 * is momentarily behind stalling the other.
 */

#include "mpx_voice_wake.h"

#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "mpx_audio.h"
#include "mpx_voice_link.h"
#include "sdkconfig.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* From main/Kconfig.projbuild. The fallback is for a build whose sdkconfig
 * predates the symbol (ninja does not always re-run kconfgen for a new
 * Kconfig entry -- `idf.py reconfigure` does), so a stale sdkconfig costs
 * a default, not a compile error. */
#ifndef CONFIG_MP4_VOICE_AUTO_MAX_SECONDS
#define CONFIG_MP4_VOICE_AUTO_MAX_SECONDS 12
#endif

static const char *TAG = "mpx_voice_wake";

/* Capture layout -- MEASURED, with `wake levels 4`, on the MP4 ESP32 CORE
 * board, 2026-09-04. The ES7210's four inputs are, per the schematic, two
 * microphones on MIC1/MIC2, the amplifier's OUTP/OUTN (the acoustic echo
 * reference) on MIC3, and nothing on MIC4. What the AFE actually receives
 * is a 4-channel capture that esp_codec_dev implements as two 32-bit I2S
 * slots, and the 16-bit samples come out of that in a different order
 * from the one board_devices.yaml lists:
 *
 *   index 0   quiet 204 / tone 4388   electrical, no room noise, x21 with
 *                                     the tone: the ECHO REFERENCE
 *   index 1   quiet 1714 / tone 505   a microphone -- the same one the
 *                                     2-channel `voice talk` capture uses
 *                                     as its channel 0 (same quiet level)
 *   index 2   quiet 202 / tone 1      the unused input
 *   index 3   quiet 2013 / tone 533   the other microphone
 *
 * Two earlier layouts were wrong in instructive ways. 2 channels as "MR"
 * handed AEC the second microphone as the reference, and it subtracted the
 * voice from itself. 4 channels as "MNRN" (the yaml order) handed WakeNet
 * the reference as the microphone: `wake debug` showed a flat -69 dBFS
 * while speaking, i.e. the ~200 RMS of an idle amplifier feed. Hence the
 * insistence on measuring: `wake levels` prints the table above and says
 * whether it still matches this format.
 *
 * One mic, not two ("RMNM"): a second mic turns on the AFE's array
 * processing and a 2-channel WakeNet mode, for more CPU and RAM than this
 * board has to spare, and the single microphone is what the cloud ASR
 * already transcribes cleanly.
 *
 * If the codec will not open 4 channels, fall back to 2 channels as "MN":
 * the wake word still works, but with no reference there is no echo
 * cancellation, so barge-in cannot hear you over the robot's own voice. */
#define WAKE_FORMAT_4CH        "RMNN"
#define WAKE_FORMAT_2CH        "MN"
#define WAKE_REFERENCE_SLOT    0

static uint8_t     s_capture_channels;   /* 4 or 2, decided by init */
static const char *s_input_format;

/* PSRAM-backed, generously sized on purpose -- see mpx_voice_stream.c's own
 * VOICE_STREAM_TASK_STACK comment for why this project no longer inches
 * stack sizes up after a guess turns out wrong. feed() and fetch() run
 * esp-sr's DSP (AEC, VAD, WakeNet inference) on whichever task calls them,
 * not on a stack of the library's own choosing. */
#define WAKE_FEED_TASK_STACK   (2048 * 10)
#define WAKE_FETCH_TASK_STACK  (2048 * 16)

#define WAKE_TASK_WAIT_MS      3000  /* > fetch()'s own 2000ms internal timeout */

static srmodel_list_t *s_models;
static const esp_afe_sr_iface_t *s_afe_handle;
static esp_afe_sr_data_t *s_afe_data;
static bool s_afe_ready;

static volatile bool s_run;
static bool s_active;
static SemaphoreHandle_t s_feed_done;
static SemaphoreHandle_t s_fetch_done;

/* Hands-free mode -- see mpx_voice_wake_auto_start() below. */
#define WAKE_AUTO_TASK_STACK   6144   /* internal RAM: it only waits and orchestrates */
#define WAKE_AUTO_BEEP_HZ      1000
#define WAKE_AUTO_BEEP_MS      120
#define WAKE_AUTO_CONNECT_RETRY_MS 2000

/* The WAKE button (SW3, GPIO 21, momentary to ground -- see board_buttons
 * in boards/mp4_esp32_core/board_devices.yaml) is a second way to say the
 * wake word: a press does exactly what "Hi ESP" does, in every state. Polled
 * from the hands-free task's own waits rather than through an interrupt,
 * because those waits already wake every 250-500 ms and a button does not
 * need better than that. */
#define WAKE_BUTTON_GPIO       21
#define WAKE_BUTTON_POLL_MS    50

static volatile bool s_auto_run;
static bool s_auto_active;
static volatile bool s_feed_failed;    /* capture read died under feed_task */

/* `wake debug on`: one line a second from the fetch loop with what the AFE
 * is actually seeing -- input level in dBFS, VAD state, ring buffer load.
 * The first question when the wake word is not heard is whether the audio
 * reaches the model at all, and this answers it without a recording. */
static volatile bool s_debug;
static float s_last_volume_dbfs;
static int   s_last_vad;
static float s_last_ringbuf_free;
static SemaphoreHandle_t s_detected;   /* fetch_task -> auto task: wake word heard */
static SemaphoreHandle_t s_auto_done;

/* ── init: load models, build the AFE instance once ─────────────────────── */

esp_err_t mpx_voice_wake_init(void)
{
    if (s_afe_ready) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(mpx_audio_have_mic(), ESP_ERR_NOT_SUPPORTED, TAG,
                        "no microphone on this board");

    s_models = esp_srmodel_init("model");
    ESP_RETURN_ON_FALSE(s_models, ESP_FAIL, TAG,
                        "esp_srmodel_init failed -- is the `model` partition "
                        "flashed? (CONFIG_MP4_VOICE_ENABLE needs a full flash, "
                        "not just an OTA, the first time it's turned on)");

    /* Probe the capture layout (see the comment above WAKE_FORMAT_4CH).
     * Opening and closing the codec once at boot is cheap; building the AFE
     * for a layout the codec then refuses is not. */
    if (mpx_audio_capture_active()) {
        ESP_LOGW(TAG, "microphone busy during init -- assuming a 4-channel capture");
        s_capture_channels = 4;
        s_input_format = WAKE_FORMAT_4CH;
    } else if (mpx_audio_capture_start(MPX_VOICE_UPLINK_RATE, 4, 0) == ESP_OK) {
        mpx_audio_capture_stop();
        s_capture_channels = 4;
        s_input_format = WAKE_FORMAT_4CH;
    } else {
        s_capture_channels = 2;
        s_input_format = WAKE_FORMAT_2CH;
        ESP_LOGW(TAG, "codec will not open 4 channels -- no echo reference, so "
                      "barge-in cannot hear you while the robot is talking");
    }
    ESP_LOGI(TAG, "capture: %u channels, AFE input format \"%s\"",
             (unsigned)s_capture_channels, s_input_format);

    afe_config_t *afe_config = afe_config_init(s_input_format, s_models,
                                               AFE_TYPE_SR, AFE_MODE_LOW_COST);
    ESP_RETURN_ON_FALSE(afe_config, ESP_FAIL, TAG, "afe_config_init failed");

    /* Internal RAM is the scarce resource on this board (see the
     * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL comment in
     * boards/santy_control/sdkconfig.defaults.board) -- AFE's own ring
     * buffers and per-algorithm state are exactly the kind of large,
     * long-lived allocation that has no business competing with the LCD's
     * DMA buffer for it. */
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    /* Core 1, as xiaozhi-esp32 does: core 0 carries the main task, LVGL,
     * Wi-Fi and the agent, and a starved AFE task drops audio silently
     * (ringbuff_free_pct in the fetch result is the tell). */
    afe_config->afe_perferred_core = 1;

    s_afe_handle = esp_afe_handle_from_config(afe_config);
    if (!s_afe_handle) {
        ESP_LOGE(TAG, "esp_afe_handle_from_config failed");
        afe_config_free(afe_config);
        return ESP_FAIL;
    }

    s_afe_data = s_afe_handle->create_from_config(afe_config);
    /* Deliberately not calling afe_config_free() here: create_from_config's
     * header comment doesn't say whether AFE copies the config or keeps
     * the pointer, and freeing it if AFE kept it would be a use-after-free
     * that would take a lot longer to notice than the ~200 bytes this
     * leaks once, at boot, for the life of the firmware. */
    if (!s_afe_data) {
        ESP_LOGE(TAG, "create_from_config failed");
        return ESP_FAIL;
    }

    s_afe_ready = true;
    ESP_LOGI(TAG, "AFE ready: %d Hz, %d feed channel(s), %d samples/channel "
                  "per feed, %d samples/channel per fetch",
             s_afe_handle->get_samp_rate(s_afe_data),
             s_afe_handle->get_feed_channel_num(s_afe_data),
             s_afe_handle->get_feed_chunksize(s_afe_data),
             s_afe_handle->get_fetch_chunksize(s_afe_data));
    s_afe_handle->print_pipeline(s_afe_data);
    return ESP_OK;
}

/* ── feed: mic -> raw capture -> afe->feed() ─────────────────────────────── */

static void feed_task(void *arg)
{
    (void)arg;
    const int chunk = s_afe_handle->get_feed_chunksize(s_afe_data);
    const int channels = s_afe_handle->get_feed_channel_num(s_afe_data);

    if (chunk <= 0 || chunk > MPX_AUDIO_MAX_FRAMES || channels != (int)s_capture_channels) {
        ESP_LOGE(TAG, "feed chunk/channel mismatch (chunk=%d, channels=%d, "
                      "expected <= %d frames, %u channels) -- not feeding",
                 chunk, channels, MPX_AUDIO_MAX_FRAMES, (unsigned)s_capture_channels);
        xSemaphoreGive(s_feed_done);
        vTaskDeleteWithCaps(NULL);
        return;
    }

    int16_t *buf = heap_caps_malloc((size_t)chunk * channels * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = heap_caps_malloc((size_t)chunk * channels * sizeof(int16_t), MALLOC_CAP_DEFAULT);
    }
    if (!buf) {
        ESP_LOGE(TAG, "no memory for the feed buffer -- not feeding");
        xSemaphoreGive(s_feed_done);
        vTaskDeleteWithCaps(NULL);
        return;
    }

    while (s_run) {
        if (mpx_audio_capture_read_raw(buf, (size_t)chunk) != ESP_OK) {
            ESP_LOGW(TAG, "raw capture read failed -- stopping feed");
            /* Take fetch_task down with us rather than leaving it polling
             * an AFE nobody feeds ("Ringbuffer of AFE is empty" every
             * 200 ms, forever). The hands-free task sees s_feed_failed
             * and restarts the front end; `wake info` shows it otherwise. */
            s_feed_failed = true;
            s_run = false;
            break;
        }
        s_afe_handle->feed(s_afe_data, buf);
    }

    free(buf);
    xSemaphoreGive(s_feed_done);
    vTaskDeleteWithCaps(NULL);
}

/* ── fetch: afe->fetch() -> wake/VAD result -> barge-in ──────────────────── */

static void send_abort_wake_word(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    const char *sid = mpx_voice_session_id();
    if (sid && sid[0]) {
        cJSON_AddStringToObject(root, "session_id", sid);
    }
    cJSON_AddStringToObject(root, "type", "abort");
    cJSON_AddStringToObject(root, "reason", "wake_word_detected");

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        return;
    }
    if (mpx_voice_send_json(text) != ESP_OK) {
        ESP_LOGW(TAG, "could not send abort -- socket down?");
    }
    free(text);
}

static void fetch_task(void *arg)
{
    (void)arg;

    while (s_run) {
        afe_fetch_result_t *res = s_afe_handle->fetch(s_afe_data);
        if (!res) {
            continue;  /* fetch()'s own 2000ms timeout re-checks s_run for us */
        }
        if (res->ret_value != 0) {
            /* Negative/nonzero ret_value is esp-sr's own "something is
             * wrong with this frame" signal -- not fatal to the pipeline,
             * just skip acting on this result. */
            continue;
        }

        s_last_volume_dbfs = res->data_volume;
        s_last_vad = (int)res->vad_state;
        s_last_ringbuf_free = res->ringbuff_free_pct;
        if (s_debug) {
            static TickType_t last;
            const TickType_t now = xTaskGetTickCount();
            if (now - last >= pdMS_TO_TICKS(1000)) {
                last = now;
                ESP_LOGI(TAG, "afe: level %.0f dBFS  vad %s  ringbuf free %.0f%%  wake %d",
                         (double)res->data_volume,
                         res->vad_state == VAD_SPEECH ? "SPEECH " : "silence",
                         (double)res->ringbuff_free_pct * 100.0, (int)res->wakeup_state);
            }
        }

        if (res->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "wake word heard (word index %d)", res->wake_word_index);
            if (mpx_voice_stream_is_playing()) {
                ESP_LOGI(TAG, "barge-in: robot was talking -- stopping playback "
                              "and telling the server");
                mpx_voice_stream_abort_playback();
                send_abort_wake_word();
            }
            if (s_auto_active) {
                /* Hands-free: hand the microphone over. This task cannot call
                 * mpx_voice_wake_stop() itself (it would be waiting for its
                 * own exit), so it leaves the loop right here -- rather than
                 * going back into fetch(), which would otherwise block for
                 * its full 2 s timeout once the feed stops -- and lets the
                 * auto task do the stop/talk/restart sequence. */
                s_run = false;
                xSemaphoreGive(s_detected);
                break;
            }
        }
    }

    xSemaphoreGive(s_fetch_done);
    vTaskDeleteWithCaps(NULL);
}

/* ── start/stop ──────────────────────────────────────────────────────────── */

esp_err_t mpx_voice_wake_start(void)
{
    ESP_RETURN_ON_FALSE(s_afe_ready, ESP_ERR_INVALID_STATE, TAG,
                        "call mpx_voice_wake_init() first");
    if (s_active) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(!mpx_audio_capture_active(), ESP_ERR_INVALID_STATE, TAG,
                        "microphone busy -- stop `voice talk` first");

    ESP_RETURN_ON_ERROR(mpx_audio_capture_start(MPX_VOICE_UPLINK_RATE,
                                                s_capture_channels, 0),
                        TAG, "capture start failed");

    /* Whatever the AFE still holds from before the microphone was handed
     * away (the tail of the wake word, the beep, the start of a reply) is
     * stale now; a second detection off old audio would loop straight back
     * into another talk. Optional in the interface, hence the NULL check. */
    if (s_afe_handle->reset_buffer) {
        s_afe_handle->reset_buffer(s_afe_data);
    }

    if (!s_feed_done) {
        s_feed_done = xSemaphoreCreateBinary();
    }
    if (!s_fetch_done) {
        s_fetch_done = xSemaphoreCreateBinary();
    }
    if (!s_feed_done || !s_fetch_done) {
        mpx_audio_capture_stop();
        return ESP_ERR_NO_MEM;
    }

    s_run = true;
    s_feed_failed = false;

    BaseType_t ok = xTaskCreateWithCaps(feed_task, "wake_feed", WAKE_FEED_TASK_STACK,
                                        NULL, 5, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ok = xTaskCreateWithCaps(feed_task, "wake_feed", WAKE_FEED_TASK_STACK,
                                 NULL, 5, NULL, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (ok != pdPASS) {
        s_run = false;
        mpx_audio_capture_stop();
        ESP_LOGE(TAG, "could not create the feed task");
        return ESP_ERR_NO_MEM;
    }

    ok = xTaskCreateWithCaps(fetch_task, "wake_fetch", WAKE_FETCH_TASK_STACK,
                             NULL, 5, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ok = xTaskCreateWithCaps(fetch_task, "wake_fetch", WAKE_FETCH_TASK_STACK,
                                 NULL, 5, NULL, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (ok != pdPASS) {
        /* feed_task is already running -- stop it the normal way rather
         * than leaving it orphaned. */
        s_run = false;
        xSemaphoreTake(s_feed_done, pdMS_TO_TICKS(WAKE_TASK_WAIT_MS));
        mpx_audio_capture_stop();
        ESP_LOGE(TAG, "could not create the fetch task");
        return ESP_ERR_NO_MEM;
    }

    s_active = true;
    ESP_LOGI(TAG, "listening (feed/fetch tasks up)");
    return ESP_OK;
}

void mpx_voice_wake_stop(void)
{
    if (!s_active) {
        return;
    }
    s_run = false;

    if (xSemaphoreTake(s_feed_done, pdMS_TO_TICKS(WAKE_TASK_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "feed task did not confirm stop within %d ms", WAKE_TASK_WAIT_MS);
    }
    if (xSemaphoreTake(s_fetch_done, pdMS_TO_TICKS(WAKE_TASK_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "fetch task did not confirm stop within %d ms", WAKE_TASK_WAIT_MS);
    }

    mpx_audio_capture_stop();
    s_active = false;
    ESP_LOGI(TAG, "stopped");
}

bool mpx_voice_wake_active(void)
{
    return s_active;
}

/* ── hands-free: wake word -> talk -> reply -> listen again ──────────────── */

/* A short tone, so you know it heard you and is listening. Generated here
 * rather than through mpx_audio_play_tone(), whose resolution is whole
 * seconds. Played with the microphone closed, so no echo path exists. */
static void wake_auto_beep(void)
{
    if (!mpx_audio_have_speaker()) {
        return;
    }
    const size_t frames = (size_t)MPX_VOICE_PLAYBACK_RATE * WAKE_AUTO_BEEP_MS / 1000;
    int16_t *buf = malloc(frames * sizeof(int16_t));
    if (!buf) {
        return;
    }
    const size_t ramp = frames / 8;
    for (size_t i = 0; i < frames; i++) {
        double env = 1.0;
        if (i < ramp) {
            env = (double)i / (double)ramp;
        } else if (i >= frames - ramp) {
            env = (double)(frames - 1 - i) / (double)ramp;
        }
        buf[i] = (int16_t)(8000.0 * env *
                           sin(2.0 * M_PI * WAKE_AUTO_BEEP_HZ * (double)i / MPX_VOICE_PLAYBACK_RATE));
    }
    if (mpx_audio_output_start(MPX_VOICE_PLAYBACK_RATE) == ESP_OK) {
        size_t off = 0;
        while (off < frames) {
            const size_t n = (frames - off) > MPX_AUDIO_MAX_FRAMES ? MPX_AUDIO_MAX_FRAMES : (frames - off);
            if (mpx_audio_output_write(buf + off, n) != ESP_OK) {
                break;
            }
            off += n;
        }
        mpx_audio_output_stop();
    }
    free(buf);
}

/* Seconds to keep listening for a follow-up after a reply, with no wake
 * word. The server's VAD ends it the moment you speak, and the server ends
 * the whole session (says goodbye, closes the socket) after ITS idle limit,
 * which also ends this listen -- so in practice this is only a backstop.
 * Long on purpose: xiaozhi-esp32 keeps listening after a reply until the
 * server says goodbye, and a short window here (it was 8 s) meant that a
 * pause to think became "the robot stopped listening". */
#define WAKE_FOLLOWUP_SECONDS   90
/* Upper bound on waiting for a reply to arrive and finish. */
#define WAKE_REPLY_TIMEOUT_MS   60000

static void wake_button_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << WAKE_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "WAKE button on GPIO %d could not be configured", WAKE_BUTTON_GPIO);
    }
}

/* Wait up to `ms` for the wake word OR a WAKE button press. A press is
 * turned into the same thing a detection is -- including the barge-in
 * abort if a reply is playing, which fetch_task does for the wake word --
 * so everything downstream is identical. Returns true if either happened. */
static bool wake_auto_wait_trigger(uint32_t ms)
{
    static bool was_down;
    for (uint32_t waited = 0; waited < ms; waited += WAKE_BUTTON_POLL_MS) {
        if (xSemaphoreTake(s_detected, pdMS_TO_TICKS(WAKE_BUTTON_POLL_MS)) == pdTRUE) {
            return true;
        }
        const bool down = gpio_get_level(WAKE_BUTTON_GPIO) == 0;
        if (down && !was_down) {
            was_down = true;
            ESP_LOGI(TAG, "WAKE button pressed -- same as the wake word");
            if (mpx_voice_stream_is_playing()) {
                mpx_voice_stream_abort_playback();
                send_abort_wake_word();
            }
            /* The front end, if it is up, must leave its loop the way it
             * does for a detection, or wake_stop() waits 2 s for fetch(). */
            s_run = false;
            return true;
        }
        if (!down) {
            was_down = false;
        }
    }
    return false;
}

/* Stop the front end, waiting first for a barge-in's playback teardown --
 * see the note in the REPLY state for why the order matters. */
static void wake_auto_release_mic(void)
{
    for (int i = 0; i < 40 && mpx_voice_stream_is_playing(); i++) {
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    mpx_voice_wake_stop();
}

typedef enum {
    WAKE_ST_LISTEN,   /* front end up, waiting for the wake word */
    WAKE_ST_TALK,     /* microphone streaming to the server */
    WAKE_ST_REPLY,    /* reply arriving/playing; front end up for barge-in */
} wake_auto_state_t;

/* The loop's state, for mpx_voice_wake_conversing(), and the music
 * player's say on follow-up turns (mpx_voice_wake_set_followup_gate()). */
static volatile wake_auto_state_t s_auto_state = WAKE_ST_LISTEN;
static mpx_voice_wake_gate_fn s_followup_gate;

bool mpx_voice_wake_conversing(void)
{
    return s_auto_active && s_auto_state != WAKE_ST_LISTEN;
}

void mpx_voice_wake_set_followup_gate(mpx_voice_wake_gate_fn skip_followup)
{
    s_followup_gate = skip_followup;
}

static void wake_auto_task(void *arg)
{
    (void)arg;
    const uint32_t max_secs = CONFIG_MP4_VOICE_AUTO_MAX_SECONDS;
    wake_auto_state_t state = WAKE_ST_LISTEN;
    bool follow_up = false;   /* this TALK needs no wake word and no beep */

    while (s_auto_run) {
        s_auto_state = state;
        switch (state) {

        case WAKE_ST_LISTEN: {
            /* Listen. If the mic could not be opened (someone ran `voice
             * talk` by hand, say), try again shortly rather than give up. */
            if (!s_active) {
                const esp_err_t err = mpx_voice_wake_start();
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "hands-free: cannot listen (%s) -- retrying",
                             esp_err_to_name(err));
                    vTaskDelay(pdMS_TO_TICKS(WAKE_AUTO_CONNECT_RETRY_MS));
                    break;
                }
            }
            /* Short timeout only so `wake auto off` and a dead feed are
             * noticed promptly. */
            if (!wake_auto_wait_trigger(500)) {
                if (s_feed_failed) {
                    ESP_LOGW(TAG, "hands-free: microphone read died -- restarting the front end");
                    mpx_voice_wake_stop();
                }
                break;
            }
            wake_auto_release_mic();
            follow_up = false;
            state = WAKE_ST_TALK;
            break;
        }

        case WAKE_ST_TALK: {
            /* A session. The server drops idle sockets after a minute or
             * so, so reconnecting here is the normal case. */
            if (mpx_voice_state() != MPX_VOICE_READY) {
                /* Zero setup: the MangDang gateway hands out a URL and a
                 * token with no activation code, so provision on first use
                 * -- and again if the stored token stopped working (the
                 * server's secret changed). */
                if (!mpx_voice_stored_url()[0]) {
                    ESP_LOGI(TAG, "hands-free: first use -- provisioning against %s",
                             MPX_VOICE_DEFAULT_OTA_URL);
                    (void)mpx_voice_provision(NULL);
                }
                if (!mpx_voice_stored_url()[0]) {
                    ESP_LOGW(TAG, "hands-free: not provisioned (no network?) -- say the "
                                  "wake word again later, or run `voice provision`");
                    state = WAKE_ST_LISTEN;
                    break;
                }
                ESP_LOGI(TAG, "hands-free: connecting");
                esp_err_t err = mpx_voice_connect(NULL, NULL);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "hands-free: connect failed (%s) -- provisioning again",
                             esp_err_to_name(err));
                    if (mpx_voice_provision(NULL) == ESP_OK) {
                        err = mpx_voice_connect(NULL, NULL);
                    }
                }
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "hands-free: connect failed (%s) -- back to listening",
                             esp_err_to_name(err));
                    state = WAKE_ST_LISTEN;
                    break;
                }
            }

            ESP_LOGI(TAG, "hands-free: turn (%s), internal RAM %u free / %u largest",
                     follow_up ? "follow-up" : "wake word",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            if (!follow_up) {
                wake_auto_beep();
            } else {
                ESP_LOGI(TAG, "hands-free: your turn -- no wake word needed until the server ends the session");
            }
            const uint32_t secs = follow_up ? WAKE_FOLLOWUP_SECONDS : max_secs;
            const esp_err_t err = mpx_voice_talk_start_auto(secs);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "hands-free: talk failed (%s) -- back to listening",
                         esp_err_to_name(err));
                state = WAKE_ST_LISTEN;
                break;
            }
            /* Wait for the talk to release the mic, in short slices so that
             * `wake auto off` mid-sentence is honoured within a beat. */
            for (uint32_t waited = 0; mpx_voice_talk_active() && waited < secs * 1000u + 3000u; waited += 250) {
                if (mpx_voice_talk_wait(250) == ESP_OK) {
                    break;
                }
                if (!s_auto_run) {
                    mpx_voice_talk_stop();
                    break;
                }
            }
            if (!mpx_voice_talk_server_took_turn()) {
                /* Nothing was said, or nothing the server could use. */
                ESP_LOGI(TAG, "hands-free: nothing heard -- listening for the wake word");
                state = WAKE_ST_LISTEN;
                break;
            }
            state = WAKE_ST_REPLY;
            break;
        }

        case WAKE_ST_REPLY: {
            /* A reply is on its way. Put the front end back up NOW, while
             * it arrives and plays, so the wake word can interrupt it. Then
             * wait for one of: the wake word (barge-in), the reply finishing
             * (follow-up turn), or nothing happening for a long time. */
            const uint32_t stops_before = mpx_voice_stream_tts_stop_count();
            if (!s_active && mpx_voice_wake_start() != ESP_OK) {
                ESP_LOGW(TAG, "hands-free: front end did not restart -- no barge-in this reply");
            }
            bool detected = false;
            bool finished = false;
            bool ended = false;
            for (uint32_t waited = 0; s_auto_run && waited < WAKE_REPLY_TIMEOUT_MS; waited += 250) {
                if (wake_auto_wait_trigger(250)) {
                    detected = true;
                    break;
                }
                if (mpx_voice_stream_tts_stop_count() != stops_before &&
                        !mpx_voice_stream_is_playing()) {
                    finished = true;
                    break;
                }
                /* Session gone -- the server said goodbye (after its idle
                 * timeout, or after you did) or the socket dropped. Do not
                 * sit here for a reply that is not coming, and do not offer
                 * a follow-up turn on a dead session: let whatever is still
                 * playing finish, then go back to the wake word. */
                if (mpx_voice_state() != MPX_VOICE_READY && !mpx_voice_stream_is_playing()) {
                    ended = true;
                    break;
                }
                if (s_feed_failed) {
                    ESP_LOGW(TAG, "hands-free: microphone read died -- restarting the front end");
                    mpx_voice_wake_stop();
                    (void)mpx_voice_wake_start();
                }
            }
            if (!s_auto_run) {
                break;
            }
            if (detected) {
                /* Barge-in: fetch_task already aborted playback and told the
                 * server. Let the speaker finish closing BEFORE the capture
                 * is closed -- on this I2S peripheral stopping RX with TX
                 * mid-write can stall TX ("if disable RX, TX also not work"
                 * in esp_codec_dev), and a stalled write with no timeout is
                 * a hung task. */
                wake_auto_release_mic();
                follow_up = false;
                state = WAKE_ST_TALK;
            } else if (finished && s_followup_gate && s_followup_gate()) {
                /* Music is waiting for the speaker (it was just asked for,
                 * or this question interrupted it): no follow-up turn --
                 * back to the wake word, so the music can play. */
                ESP_LOGI(TAG, "hands-free: music waiting -- listening for the wake word");
                state = WAKE_ST_LISTEN;
            } else if (finished && mpx_voice_state() == MPX_VOICE_READY) {
                /* Reply done: hand the mic straight back for a follow-up. */
                wake_auto_release_mic();
                follow_up = true;
                state = WAKE_ST_TALK;
            } else if (finished || ended) {
                ESP_LOGI(TAG, "hands-free: session ended -- listening for the wake word");
                state = WAKE_ST_LISTEN;
            } else {
                ESP_LOGW(TAG, "hands-free: no reply within %d s -- listening for the wake word",
                         WAKE_REPLY_TIMEOUT_MS / 1000);
                state = WAKE_ST_LISTEN;
            }
            break;
        }
        }
    }

    if (s_active) {
        mpx_voice_wake_stop();
    }
    s_auto_state = WAKE_ST_LISTEN;
    s_auto_active = false;
    xSemaphoreGive(s_auto_done);
    vTaskDelete(NULL);
}

esp_err_t mpx_voice_wake_auto_start(void)
{
    if (s_auto_active) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(mpx_voice_wake_init(), TAG, "init failed");

    if (!s_detected) {
        s_detected = xSemaphoreCreateBinary();
    }
    if (!s_auto_done) {
        s_auto_done = xSemaphoreCreateBinary();
    }
    ESP_RETURN_ON_FALSE(s_detected && s_auto_done, ESP_ERR_NO_MEM, TAG, "out of memory");
    /* Drain anything left from a previous run. */
    (void)xSemaphoreTake(s_detected, 0);
    (void)xSemaphoreTake(s_auto_done, 0);

    wake_button_init();

    s_auto_run = true;
    s_auto_active = true;
    /* Internal stack on purpose: this task only orchestrates, and a task
     * whose stack lives in PSRAM cannot be the one that ends up in a flash
     * write (NVS, from provisioning) with the cache disabled. */
    if (xTaskCreate(wake_auto_task, "wake_auto", WAKE_AUTO_TASK_STACK, NULL, 4, NULL) != pdPASS) {
        s_auto_run = false;
        s_auto_active = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "hands-free: say the wake word to talk");
    return ESP_OK;
}

void mpx_voice_wake_auto_stop(void)
{
    if (!s_auto_active) {
        return;
    }
    s_auto_run = false;
    /* If it is mid-talk, cut that short so the task can leave the loop. */
    mpx_voice_talk_stop();
    if (xSemaphoreTake(s_auto_done, pdMS_TO_TICKS(WAKE_TASK_WAIT_MS + 5000)) != pdTRUE) {
        ESP_LOGW(TAG, "hands-free task did not confirm stop");
    }
}

bool mpx_voice_wake_auto_active(void)
{
    return s_auto_active;
}

/* ── console command ─────────────────────────────────────────────────────── */

static void wake_usage(void)
{
    printf("\n"
           "  wake auto on|off      hands-free: wake word -> listen -> reply -> repeat\n"
           "  wake start           listen for the wake word (barge-in only)\n"
           "  wake stop             stop listening, release the microphone\n"
           "  wake info              status, including what the AFE last saw\n"
           "  wake debug on|off     log level / VAD / ring buffer once a second\n"
           "  wake levels [secs]    raw per-channel RMS, %d s default -- see below\n"
           "\n"
           "The wake word is \"Hi ESP\" -- said as the letters, \"Hi, E-S-P\".\n"
           "The WAKE button (GPIO 21) does the same thing as saying it.\n"
           "\n"
           "`wake auto on` (the boot default when CONFIG_MP4_VOICE_AUTO_LISTEN\n"
           "is set) is the whole conversation loop: it listens for the wake\n"
           "word, beeps, streams what you say until the server decides you\n"
           "have finished, plays the reply -- with the front end running, so\n"
           "the wake word interrupts it -- and then keeps listening with NO\n"
           "wake word, like xiaozhi, until the server ends the session (it\n"
           "says goodbye after a long silence). Then the wake word is needed\n"
           "again. It connects (or\n"
           "reconnects: the server drops idle sockets) by itself; `voice\n"
           "provision` still has to have been run once. `wake auto off`\n"
           "returns to the manual commands.\n"
           "\n"
           "`wake`, hands-free or not, and `voice talk` share one microphone\n"
           "and cannot both hold it open -- each refuses to start while the\n"
           "other is running. `wake start` alone is the manual, barge-in-only\n"
           "mode: the wake word interrupts a reply but does not start a talk.\n"
           "\n"
           "`wake levels` does not need `wake start`: it opens all four ES7210\n"
           "slots by itself, stays quiet for the first half, plays a tone for\n"
           "the second half, and prints each channel's level in both. The\n"
           "channel that jumps with the tone is the echo reference; the front\n"
           "end expects it at index 0 (\"" WAKE_FORMAT_4CH "\").\n"
           "\n", 3);
}

static void wake_cmd_levels(uint32_t seconds)
{
    if (mpx_audio_capture_active()) {
        printf("  microphone busy -- `wake auto off` or `wake stop` first\n");
        return;
    }
    /* Always all four slots here, whatever the front end settled on: the
     * point of this command is to see the layout. */
    const uint8_t channels = 4;
    esp_err_t err = mpx_audio_capture_start(MPX_VOICE_UPLINK_RATE, channels, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "capture start (4 ch) failed: %s", esp_err_to_name(err));
        return;
    }

    const uint32_t frames_per_read = 320;  /* 20 ms at 16 kHz */
    int16_t *buf = heap_caps_malloc(frames_per_read * channels * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *tone = heap_caps_malloc(frames_per_read * sizeof(int16_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf || !tone) {
        ESP_LOGE(TAG, "out of memory");
        mpx_audio_capture_stop();
        free(buf);
        free(tone);
        return;
    }
    /* 440 Hz: 320 frames is exactly 8.8 cycles, so generate from a running
     * phase rather than per-block to avoid a click at every block edge. */
    double phase = 0.0;
    const double step = 2.0 * M_PI * 440.0 / MPX_VOICE_UPLINK_RATE;

    double energy[2][4] = {{0}};   /* [silent|tone][channel] */
    uint64_t count[2] = {0, 0};
    const uint32_t reads = (MPX_VOICE_UPLINK_RATE * seconds) / frames_per_read;
    const uint32_t tone_from = reads / 2;
    bool speaker = false;

    printf("  %" PRIu32 " s: first half quiet, second half playing a 440 Hz tone\n", seconds);
    for (uint32_t r = 0; r < reads; r++) {
        const int half = (r >= tone_from) ? 1 : 0;
        if (half == 1) {
            if (!speaker && mpx_audio_have_speaker() &&
                mpx_audio_output_start(MPX_VOICE_PLAYBACK_RATE) == ESP_OK) {
                speaker = true;
            }
            if (speaker) {
                for (uint32_t i = 0; i < frames_per_read; i++) {
                    tone[i] = (int16_t)(6000.0 * sin(phase));
                    phase += step;
                }
                (void)mpx_audio_output_write(tone, frames_per_read);
            }
        }
        if (mpx_audio_capture_read_raw(buf, frames_per_read) != ESP_OK) {
            break;
        }
        for (uint32_t i = 0; i < frames_per_read; i++) {
            for (int c = 0; c < channels; c++) {
                const double v = buf[i * channels + c];
                energy[half][c] += v * v;
            }
        }
        count[half]++;
    }
    if (speaker) {
        mpx_audio_output_stop();
    }
    mpx_audio_capture_stop();
    free(buf);
    free(tone);

    if (count[0] == 0 || count[1] == 0) {
        printf("  not enough frames read\n");
        return;
    }
    /* Names by MEASURED position (see the comment above WAKE_FORMAT_4CH),
     * not by the ES7210 pin they come from. */
    static const char *const names[4] = {
        "echo reference (AFE 'R')", "microphone (AFE 'M')",
        "unused input", "second microphone (unused)",
    };
    printf("  channel   quiet rms   tone rms   (of 32767)\n");
    int best = -1;
    double best_ratio = 0.0;
    for (int c = 0; c < channels; c++) {
        const double q = sqrt(energy[0][c] / (double)(count[0] * frames_per_read));
        const double t = sqrt(energy[1][c] / (double)(count[1] * frames_per_read));
        const double ratio = t / (q + 1.0);
        printf("  %d %-32s %8.0f   %8.0f\n", c, names[c], q, t);
        if (ratio > best_ratio) {
            best_ratio = ratio;
            best = c;
        }
    }
    printf("  the reference is the channel whose level rises most with the tone: "
           "channel %d (x%.1f)%s\n", best, best_ratio,
           best == WAKE_REFERENCE_SLOT ? " -- matches the AFE's \"" WAKE_FORMAT_4CH "\"" :
                                         " -- does NOT match the AFE's \"" WAKE_FORMAT_4CH "\", tell Claude");
    printf("  (mic channels also rise a little during the tone: that is the "
           "speaker heard through the air, which is what AEC is for)\n");
}

static int wake_cmd(int argc, char **argv)
{
    if (argc < 2) {
        wake_usage();
        return 0;
    }

    if (strcmp(argv[1], "start") == 0) {
        esp_err_t err = mpx_voice_wake_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "init failed: %s", esp_err_to_name(err));
            return 1;
        }
        err = mpx_voice_wake_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err));
            return 1;
        }
        return 0;
    }

    if (strcmp(argv[1], "stop") == 0) {
        /* Stopping while hands-free is on would just be undone by the auto
         * task a moment later; turn that off first. */
        mpx_voice_wake_auto_stop();
        mpx_voice_wake_stop();
        return 0;
    }

    if (strcmp(argv[1], "auto") == 0) {
        const bool on = (argc < 3) || strcmp(argv[2], "on") == 0;
        if (on) {
            const esp_err_t err = mpx_voice_wake_auto_start();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "auto start failed: %s", esp_err_to_name(err));
                return 1;
            }
            printf("  hands-free on -- say \"Hi ESP\"\n");
        } else {
            mpx_voice_wake_auto_stop();
            printf("  hands-free off\n");
        }
        return 0;
    }

    if (strcmp(argv[1], "debug") == 0) {
        s_debug = (argc < 3) || strcmp(argv[2], "on") == 0;
        printf("  afe debug %s\n", s_debug ? "on -- one line a second while listening" : "off");
        return 0;
    }

    if (strcmp(argv[1], "info") == 0) {
        printf("  afe    : %s\n", s_afe_ready ? "ready" : "not initialized");
        printf("  input  : %u channels, format \"%s\"\n",
               (unsigned)s_capture_channels, s_input_format ? s_input_format : "?");
        printf("  last   : level %.0f dBFS, vad %s, ringbuf free %.0f%%\n",
               (double)s_last_volume_dbfs, s_last_vad == (int)VAD_SPEECH ? "speech" : "silence",
               (double)s_last_ringbuf_free * 100.0);
        printf("  auto   : %s\n", mpx_voice_wake_auto_active() ? "on (hands-free)" : "off");
        printf("  wake   : %s\n", mpx_voice_wake_active() ?
               (s_feed_failed ? "STALLED (mic read died -- `wake stop`, `wake start`)" : "listening") :
               "stopped");
        printf("  mic    : %s\n", mpx_audio_capture_active() ? "busy" : "free");
        printf("  link   : %s\n", mpx_voice_state_name());
        return 0;
    }

    if (strcmp(argv[1], "levels") == 0) {
        const uint32_t secs = (argc >= 3) ? (uint32_t)atoi(argv[2]) : 3;
        wake_cmd_levels(secs ? secs : 3);
        return 0;
    }

    wake_usage();
    return 0;
}

void register_wake_command(void)
{
    const esp_console_cmd_t cmd = {
        .command = "wake",
        .help = "Wake word: hands-free conversation, barge-in (see `wake` with no arguments)",
        .hint = NULL,
        .func = wake_cmd,
    };
    if (esp_console_cmd_register(&cmd) == ESP_OK) {
        ESP_LOGI(TAG, "'wake' console command registered");
    }
}
