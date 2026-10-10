/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * See voice_glue.h.
 */

#include "voice_glue.h"

#include <cstdio>
#include <cstring>

#include "agent/agent_config.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "voice_glue";

namespace voice_glue {

bool voice_wanted()
{
    /* Voice is the gateway's in both AI modes; only "off" closes the mic. */
    return agent_config::mode() != agent_config::Mode::Off;
}

}  // namespace voice_glue

#if CONFIG_APP_VOICE_ENABLE

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "cap_music.h"
#include "esp_heap_caps.h"
#include "mpx_audio.h"
#include "mpx_robot.h"
#include "mpx_voice_link.h"
#include "mpx_voice_wake.h"
#include "voice_tools.h"

#if CONFIG_APP_DISPLAY_ENABLE
#include "cap_display.h"
#endif

namespace {

enum Event : uint8_t { EV_MASTER_ON = 1, EV_MASTER_OFF };

QueueHandle_t s_events;
bool          s_started;
bool          s_wake_ready;        /* mpx_voice_wake_init() succeeded */
volatile bool s_voice_resume;      /* hands-free was on when the Pi took over */

bool hands_free_wanted()
{
    return voice_glue::voice_wanted() && s_wake_ready;
}

esp_err_t hands_free_start_locked()
{
    if (!hands_free_wanted()) return ESP_ERR_INVALID_STATE;
    if (mpx_robot_pi_in_control()) {
        /* The Pi has the robot: voice stays off until it lets go. */
        s_voice_resume = true;
        ESP_LOGI(TAG, "hands-free listening waits: the Raspberry Pi is in control");
        return ESP_OK;
    }
    return mpx_voice_wake_auto_start();
}

/* The Pi link task must not block, so the stopping (microphone, display
 * lock inside music/voice teardown) happens here. */
void events_task(void *)
{
    uint8_t ev;
    for (;;) {
        if (xQueueReceive(s_events, &ev, portMAX_DELAY) != pdTRUE) continue;
        switch (ev) {
        case EV_MASTER_ON:
            ESP_LOGW(TAG, "Raspberry Pi (ROS) in control: voice and music off");
            if (cap_music_active()) cap_music_stop();
            if (mpx_voice_wake_auto_active()) {
                s_voice_resume = true;
                mpx_voice_wake_auto_stop();
            }
            break;
        case EV_MASTER_OFF:
            if (s_voice_resume) {
                s_voice_resume = false;
                if (hands_free_wanted() && mpx_voice_wake_auto_start() != ESP_OK) {
                    ESP_LOGW(TAG, "hands-free listening did not restart -- POST /v1/voice {\"hands_free\":true}");
                }
            }
            break;
        default:
            break;
        }
    }
}

}  // namespace

namespace voice_glue {

bool available()
{
    return mpx_audio_have_mic();
}

esp_err_t apply_mode()
{
    ESP_LOGW(TAG, "AI mode -> %s", agent_config::mode_name(agent_config::mode()));
    if (!s_started) return ESP_OK;
    if (!voice_wanted()) {
        if (cap_music_active()) cap_music_stop();
        mpx_voice_wake_auto_stop();
        mpx_voice_disconnect();
        s_voice_resume = false;
        return ESP_OK;
    }
    if (!available()) return ESP_ERR_NOT_SUPPORTED;
    /* Off -> on: the front end was never built if the robot booted with
     * voice off, so this goes through the init-if-needed path. */
    if (mpx_voice_wake_auto_active()) return ESP_OK;
    return hands_free_start();
}

void start()
{
    if (s_started) return;
    s_started = true;

    if (!available()) {
        ESP_LOGW(TAG, "no microphone: voice unavailable");
        return;
    }

    /* The Pi events queue first: a Pi taking control during boot must be
     * seen even before the front end is up. */
    s_events = xQueueCreate(4, sizeof(uint8_t));
    if (!s_events || xTaskCreate(events_task, "voice_ev", 4096, nullptr, 3, nullptr) != pdPASS) {
        ESP_LOGW(TAG, "voice events task did not start");
    }

#if CONFIG_APP_DISPLAY_ENABLE
    /* The conversation on the face: what the server heard, says and feels.
     * Registered here so neither voice nor display knows the other exists. */
    static const mpx_voice_ui_hooks_t face_hooks = {
        .heard         = cap_display_voice_heard,
        .saying        = cap_display_voice_saying,
        .emotion       = cap_display_voice_emotion,
        .reply_done    = cap_display_voice_reply_done,
        .session_ended = cap_display_voice_session_ended,
    };
    mpx_voice_set_ui_hooks(&face_hooks);
#endif

    /* Music works in every mode -- with voice off it is the app's
     * (POST /v1/voice {"music":...}). */
    cap_music_init();

    /* The tool bridge, so the server's model can move the robot. Nothing
     * network-related happens until a session opens. */
    if (voice_tools::start() != ESP_OK) {
        ESP_LOGW(TAG, "voice tools did not start -- the server cannot move the robot");
    }

    if (!voice_wanted()) {
        ESP_LOGW(TAG, "AI is off: voice disabled, the microphone stays closed");
        return;
    }

    /* Load esp-sr's models and build the AFE now, at boot, rather than on
     * first use: it is the last big consumer of internal RAM to come up,
     * and better to see it fail in the boot log than mid-conversation. */
    if (mpx_voice_wake_init() != ESP_OK) {
        ESP_LOGW(TAG, "wake word init did not complete -- voice unavailable this boot");
        return;
    }
    s_wake_ready = true;
    ESP_LOGI(TAG, "after wake init: internal RAM %u free, %u largest",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

#if CONFIG_APP_VOICE_AUTO_LISTEN
    /* Hands-free from here on. The socket opens on the first wake word, by
     * which time Wi-Fi has had every chance to come up. */
    if (hands_free_start_locked() != ESP_OK) {
        ESP_LOGW(TAG, "hands-free listening did not start -- POST /v1/voice {\"hands_free\":true} to retry");
    }
#endif
}

void on_master_changed(bool active)
{
    const uint8_t ev = active ? EV_MASTER_ON : EV_MASTER_OFF;
    if (s_events) (void)xQueueSend(s_events, &ev, 0);
}

esp_err_t hands_free_start()
{
    if (!s_started || !available()) return ESP_ERR_NOT_SUPPORTED;
    if (!voice_wanted()) return ESP_ERR_INVALID_STATE;
    if (!s_wake_ready) {
        if (mpx_voice_wake_init() != ESP_OK) return ESP_FAIL;
        s_wake_ready = true;
    }
    return hands_free_start_locked();
}

void hands_free_stop()
{
    s_voice_resume = false;
    mpx_voice_wake_auto_stop();
}

esp_err_t provision()
{
    if (!available()) return ESP_ERR_NOT_SUPPORTED;
    return mpx_voice_provision(nullptr);
}

std::string status_json()
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "available", available());
    cJSON_AddStringToObject(o, "mode", agent_config::mode_name(agent_config::mode()));
    cJSON_AddBoolToObject(o, "mic", mpx_audio_have_mic());
    cJSON_AddBoolToObject(o, "speaker", mpx_audio_have_speaker());
    cJSON_AddNumberToObject(o, "volume", mpx_audio_get_volume());
    cJSON_AddBoolToObject(o, "provisioned", mpx_voice_stored_url()[0] != '\0');
    cJSON_AddStringToObject(o, "gateway", MPX_VOICE_DEFAULT_OTA_URL);
    cJSON_AddStringToObject(o, "link", mpx_voice_state_name());
    cJSON_AddStringToObject(o, "hands_free", mpx_voice_wake_state_name());
    cJSON_AddBoolToObject(o, "listening", mpx_voice_wake_active());
    cJSON_AddBoolToObject(o, "talking", mpx_voice_talk_active());
    cJSON_AddBoolToObject(o, "playing_reply", mpx_voice_stream_is_playing());
    cJSON_AddBoolToObject(o, "pi_in_control", mpx_robot_pi_in_control());
    {
        float level = 0, ring = 0;
        int vad = 0;
        mpx_voice_wake_last_seen(&level, &vad, &ring);
        cJSON *afe = cJSON_AddObjectToObject(o, "afe");
        cJSON_AddBoolToObject(afe, "ready", s_wake_ready);
        cJSON_AddNumberToObject(afe, "level_dbfs", (double)level);
        cJSON_AddBoolToObject(afe, "speech", vad != 0);
        cJSON_AddNumberToObject(afe, "ringbuf_free", (double)ring);
    }
    {
        char line[200];
        cap_music_describe(line, sizeof(line));
        cJSON *music = cJSON_AddObjectToObject(o, "music");
        cJSON_AddBoolToObject(music, "active", cap_music_active());
        cJSON_AddStringToObject(music, "title", cap_music_title());
        cJSON_AddNumberToObject(music, "level", cap_music_level());
        cJSON_AddStringToObject(music, "state", line);
    }
    char *s = cJSON_PrintUnformatted(o);
    std::string out = s ? s : "{}";
    cJSON_free(s);
    cJSON_Delete(o);
    return out;
}

}  // namespace voice_glue

#else  /* !CONFIG_APP_VOICE_ENABLE */

namespace voice_glue {

bool available() { return false; }
esp_err_t apply_mode() { return voice_wanted() ? ESP_ERR_NOT_SUPPORTED : ESP_OK; }
void start() { ESP_LOGI(TAG, "voice not compiled in (CONFIG_APP_VOICE_ENABLE=n)"); }
void on_master_changed(bool) {}
esp_err_t hands_free_start() { return ESP_ERR_NOT_SUPPORTED; }
void hands_free_stop() {}
esp_err_t provision() { return ESP_ERR_NOT_SUPPORTED; }
std::string status_json() { return "{\"available\":false,\"mode\":\"off\"}"; }

}  // namespace voice_glue

#endif
