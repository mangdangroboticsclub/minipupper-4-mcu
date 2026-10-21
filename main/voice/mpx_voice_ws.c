/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * The xiaozhi-protocol WebSocket link.
 *
 * Connection and handshake only, for now. Audio streaming is the next step,
 * and keeping them apart matters: if the socket and the codec arrive
 * together, a handshake mismatch and a codec mismatch produce the same
 * symptom -- silence -- and there is no way to tell which you have.
 */

#include "mpx_voice_link.h"
#include "mpx_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "mpx_voice_ws";

#define VOICE_SESSION_ID_MAX   64
#define VOICE_HEADER_MAX      320
#define VOICE_HANDSHAKE_MS   8000

/* The websocket client runs the event handler on its own task, and this
 * parses JSON there. 6 KB rather than the 4 KB default, because cJSON's
 * recursive descent plus a log line is not free and a stack overflow in a
 * callback is a miserable thing to diagnose. */
#define VOICE_WS_TASK_STACK  6144

static esp_websocket_client_handle_t s_client;
static mpx_voice_state_t s_state = MPX_VOICE_IDLE;
static char s_session_id[VOICE_SESSION_ID_MAX];
static uint32_t s_downlink_rate = MPX_VOICE_DOWNLINK_RATE;
static SemaphoreHandle_t s_handshake;
static uint32_t s_audio_frames;
static mpx_voice_mcp_sink_t s_mcp_sink;
static mpx_voice_ui_hooks_t s_ui;   /* zeroed = no hooks */
static bool s_device_brain;
static mpx_voice_stt_sink_t s_stt_sink;

void mpx_voice_set_device_brain(bool on, mpx_voice_stt_sink_t sink)
{
    s_device_brain = on && sink != NULL;
    s_stt_sink = sink;
}

bool mpx_voice_device_brain(void)
{
    return s_device_brain;
}

static void (*s_quiet_hook)(void);

void mpx_voice_set_quiet_hook(void (*hook)(void))
{
    s_quiet_hook = hook;
}

void mpx_voice_set_mcp_sink(mpx_voice_mcp_sink_t sink)
{
    s_mcp_sink = sink;
}

void mpx_voice_set_ui_hooks(const mpx_voice_ui_hooks_t *hooks)
{
    if (hooks) {
        s_ui = *hooks;
    } else {
        memset(&s_ui, 0, sizeof(s_ui));
    }
}

/* Whether the WebSocket upgrade ever completed. The distinction matters when
 * reporting a failure: "the server never accepted the upgrade" and "the
 * server accepted it and then said nothing" have completely different
 * causes, and both look like a timeout from the caller's side. */
static bool s_socket_opened;

/* ── outgoing ────────────────────────────────────────────────────────────── */

/* Send timeouts. These are not "how long to wait before giving up on this
 * message": esp_websocket_client treats a write that times out as a dead
 * socket and ABORTS THE CONNECTION, whatever the caller then does with the
 * error. The first cut used 2 s for everything, and about one wake word in
 * ten ended like this, right after the socket came up:
 *
 *   E transport_ws: Error transport_poll_write(0)
 *   E websocket_client: esp_transport_write() returned 0 ...
 *   E mpx_voice_ws: socket error / W socket closed
 *   E mpx_voice_send_json(78): send failed (0 of 5545)
 *
 * -- a 5.5 KB tools/list reply over fresh TLS, on a Wi-Fi link that had just
 * renegotiated block-ack (the "[ADDBA]RX DELBA" line beside it). Two seconds
 * of silence from the radio is not a dead server. So the timeouts are set by
 * what a stall costs: a control message waits up to 10 s (the caller is the
 * MCP task or the talk task, neither of which minds), an audio frame up to
 * 5 s (the talk task sends one every 60 ms; a stall that long means the
 * link is really gone). While a session is up, Wi-Fi power save is also
 * off -- see the CONNECTED event -- which is what removes most of the
 * stalls in the first place. */
#define VOICE_WS_SEND_JSON_MS    10000
#define VOICE_WS_SEND_AUDIO_MS   5000

esp_err_t mpx_voice_send_json(const char *json)
{
    ESP_RETURN_ON_FALSE(s_client && json, ESP_ERR_INVALID_STATE, TAG, "not connected");

    const int len = (int)strlen(json);
    const int sent = esp_websocket_client_send_text(s_client, json, len,
                                                    pdMS_TO_TICKS(VOICE_WS_SEND_JSON_MS));
    ESP_RETURN_ON_FALSE(sent == len, ESP_FAIL, TAG, "send failed (%d of %d)", sent, len);
    return ESP_OK;
}

esp_err_t mpx_voice_send_binary(const uint8_t *data, size_t len)
{
    ESP_RETURN_ON_FALSE(s_client && data && len, ESP_ERR_INVALID_STATE, TAG, "not connected");

    /* Same call as send_text -- esp_websocket_client picks the frame opcode
     * (binary vs. text) from which of the two functions you call, not from
     * the bytes. Uplink Opus packets go out as 0x02 this way, matching what
     * the server sends us and what voice_ws_event's op_code check expects. */
    const int sent = esp_websocket_client_send_bin(s_client, (const char *)data, (int)len,
                                                    pdMS_TO_TICKS(VOICE_WS_SEND_AUDIO_MS));
    ESP_RETURN_ON_FALSE(sent == (int)len, ESP_FAIL, TAG, "binary send failed (%d of %u)",
                        sent, (unsigned)len);
    return ESP_OK;
}

esp_err_t mpx_voice_send_speak(const char *text, bool last)
{
    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root, ESP_ERR_NO_MEM, TAG, "out of memory");
    cJSON_AddStringToObject(root, "type", "speak");
    if (s_session_id[0]) {
        cJSON_AddStringToObject(root, "session_id", s_session_id);
    }
    cJSON_AddStringToObject(root, "text", text ? text : "");
    cJSON_AddBoolToObject(root, "last", last);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    ESP_RETURN_ON_FALSE(json, ESP_ERR_NO_MEM, TAG, "out of memory");
    const esp_err_t err = mpx_voice_send_json(json);
    free(json);
    return err;
}

static esp_err_t voice_send_hello(void)
{
    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root, ESP_ERR_NO_MEM, TAG, "out of memory");

    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddStringToObject(root, "transport", "websocket");

    /* mcp:true is the flag that lets the server call back into this device.
     * It is what Phase 3 needs -- the server's model asking the robot to
     * change its face -- and it costs nothing to advertise now. */
    cJSON *features = cJSON_AddObjectToObject(root, "features");
    if (features) {
        cJSON_AddBoolToObject(features, "mcp", true);
        /* The robot shapes its own speaker (mpx_audio `audio tune`): the
         * server then sends speech as the TTS made it. */
        if (mpx_audio_speaker_dsp()) {
            cJSON_AddBoolToObject(features, "speaker_dsp", true);
        }
        if (s_device_brain) {
            /* The server transcribes and speaks; this robot's agent answers. */
            cJSON_AddStringToObject(features, "brain", "device");
        }
    }

    cJSON *audio = cJSON_AddObjectToObject(root, "audio_params");
    if (audio) {
        cJSON_AddStringToObject(audio, "format", "opus");
        cJSON_AddNumberToObject(audio, "sample_rate", MPX_VOICE_UPLINK_RATE);
        cJSON_AddNumberToObject(audio, "channels", 1);
        cJSON_AddNumberToObject(audio, "frame_duration", MPX_VOICE_FRAME_MS);
    }

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    ESP_RETURN_ON_FALSE(text, ESP_ERR_NO_MEM, TAG, "out of memory");

    ESP_LOGI(TAG, "-> %s", text);
    const esp_err_t err = mpx_voice_send_json(text);
    free(text);
    return err;
}

/* ── incoming ────────────────────────────────────────────────────────────── */

static void voice_handle_hello(const cJSON *root)
{
    const cJSON *session = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session) && session->valuestring) {
        strlcpy(s_session_id, session->valuestring, sizeof(s_session_id));
    }

    /* The server chooses the downlink rate, and it is not always the 24 kHz
     * the protocol documents -- so read it rather than assume it. Getting
     * this wrong plays speech at the wrong pitch, which sounds like a broken
     * decoder. */
    const cJSON *audio = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio)) {
        const cJSON *rate = cJSON_GetObjectItem(audio, "sample_rate");
        if (cJSON_IsNumber(rate) && rate->valueint > 0) {
            s_downlink_rate = (uint32_t)rate->valueint;
        }
    }

    s_state = MPX_VOICE_READY;
    ESP_LOGI(TAG, "session ready: id=%s, downlink %" PRIu32 " Hz",
             s_session_id[0] ? s_session_id : "(none)", s_downlink_rate);
    ESP_LOGI(TAG, "internal RAM after connect: %u free, %u largest",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    if (s_handshake) {
        xSemaphoreGive(s_handshake);
    }
}

static void voice_handle_text(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, (size_t)len);
    if (!root) {
        ESP_LOGW(TAG, "unparseable message (%d bytes)", len);
        return;
    }

    const cJSON *type = cJSON_GetObjectItem(root, "type");
    const char *kind = cJSON_IsString(type) ? type->valuestring : "";

    if (strcmp(kind, "hello") == 0) {
        voice_handle_hello(root);
    } else if (strcmp(kind, "stt") == 0) {
        const cJSON *text = cJSON_GetObjectItem(root, "text");
        ESP_LOGI(TAG, "heard: %s",
                 cJSON_IsString(text) ? text->valuestring : "(no text)");
        if (s_ui.heard && cJSON_IsString(text)) {
            s_ui.heard(text->valuestring);
        }
        if (s_device_brain && s_stt_sink && cJSON_IsString(text) && text->valuestring[0]) {
            char *copy = strdup(text->valuestring);
            if (copy) {
                s_stt_sink(copy);   /* owns it now */
            }
        }
        /* A transcript means the server's VAD has closed the utterance --
         * an auto-mode talk should stop streaming now, not keep feeding it
         * the room (and, in a moment, its own reply). */
        mpx_voice_stream_on_server_turn();
    } else if (strcmp(kind, "tts") == 0) {
        const cJSON *state = cJSON_GetObjectItem(root, "state");
        const cJSON *text = cJSON_GetObjectItem(root, "text");
        const char *state_str = cJSON_IsString(state) ? state->valuestring : "?";
        ESP_LOGI(TAG, "tts %s%s%s", state_str,
                 cJSON_IsString(text) ? ": " : "",
                 cJSON_IsString(text) ? text->valuestring : "");
        /* "stop" means the server is done sending this reply's audio -- tell
         * the streaming layer so it closes the speaker instead of leaving
         * I2S open with nothing feeding it until the next reply. Anything
         * else ("start", "sentence_start") means the reply has begun, which
         * is the other way an auto-mode talk learns the server took its
         * turn -- some servers send tts start before, or instead of, stt. */
        if (strcmp(state_str, "stop") == 0) {
            mpx_voice_stream_on_tts_stop();
            if (s_ui.reply_done) {
                s_ui.reply_done();
            }
        } else {
            mpx_voice_stream_on_server_turn();
            if (s_ui.saying && strcmp(state_str, "sentence_start") == 0 && cJSON_IsString(text)) {
                s_ui.saying(text->valuestring);
            }
        }
    } else if (strcmp(kind, "llm") == 0) {
        /* Emotion hints -- Phase 5 puts them on the face through the hook. */
        const cJSON *emotion = cJSON_GetObjectItem(root, "emotion");
        ESP_LOGI(TAG, "emotion: %s",
                 cJSON_IsString(emotion) ? emotion->valuestring : "(none)");
        if (s_ui.emotion && cJSON_IsString(emotion)) {
            s_ui.emotion(emotion->valuestring);
        }
    } else if (strcmp(kind, "mcp") == 0) {
        /* Phase 3. `payload` is the raw JSON-RPC message; mpx_mcp_ws (if it
         * has registered itself) owns everything past this point, including
         * the reply -- see mpx_voice_set_mcp_sink() in the header for why
         * this component does not call into it directly. */
        const cJSON *payload = cJSON_GetObjectItem(root, "payload");
        if (!cJSON_IsObject(payload)) {
            ESP_LOGW(TAG, "mcp message with no payload object (%d bytes)", len);
        } else if (!s_mcp_sink) {
            ESP_LOGW(TAG, "mcp message received but no MCP bridge is registered "
                          "-- mpx_mcp_ws_init() not called?");
        } else {
            char *json = cJSON_PrintUnformatted(payload);
            if (json) {
                s_mcp_sink(json);
            }
        }
    } else if (strcmp(kind, "goodbye") == 0) {
        /* The server is ending the conversation -- after you said goodbye,
         * or after its own idle timeout (it says "bye" out loud first, then
         * sends this, then closes the socket). Nothing to send back; the
         * DISCONNECTED event that follows is what moves the state. Logged
         * loudly because from the outside it looks like the robot stopped
         * listening for no reason. */
        ESP_LOGW(TAG, "server ended the session (goodbye) -- the next turn needs the wake word");
        {
            /* "reason":"stop": you told it to be quiet ("stop talking", "be
             * quiet") -- not an idle timeout. Quiet means the music too. */
            const cJSON *reason = cJSON_GetObjectItem(root, "reason");
            if (cJSON_IsString(reason) && strcmp(reason->valuestring, "stop") == 0 && s_quiet_hook) {
                s_quiet_hook();
            }
        }
        if (s_ui.session_ended) {
            s_ui.session_ended();
        }
        s_state = MPX_VOICE_IDLE;
    } else {
        ESP_LOGI(TAG, "<- %.*s", len > 200 ? 200 : len, data);
    }

    cJSON_Delete(root);
}

static void voice_ws_event(void *arg, esp_event_base_t base,
                           int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "socket open");
            s_socket_opened = true;
            s_state = MPX_VOICE_HANDSHAKING;
            s_audio_frames = 0;
            /* No modem sleep while a conversation is live. The default
             * WIFI_PS_MIN_MODEM wakes the radio once per DTIM, which on this
             * router is enough added latency to make a TLS write stall past
             * its timeout now and then -- and a timed-out write is a dropped
             * session (see VOICE_WS_SEND_JSON_MS). xiaozhi's own firmware
             * does the same. Restored when the socket closes, so the idle
             * robot still sleeps between wake words. */
            (void)esp_wifi_set_ps(WIFI_PS_NONE);
            if (voice_send_hello() != ESP_OK) {
                ESP_LOGE(TAG, "could not send hello");
            }
            break;

        case WEBSOCKET_EVENT_DATA:
            if (ev->op_code == 0x01 && ev->data_len > 0) {
                voice_handle_text(ev->data_ptr, ev->data_len);
            } else if (ev->op_code == 0x02 && ev->data_len > 0) {
                /* Opus from the server. Copied onto mpx_voice_stream's own
                 * queue and decoded on its own task, never here -- decoding
                 * inline would run on the websocket client's task stack,
                 * nowhere near the ~26 KB Opus wants (see the file header of
                 * mpx_voice_stream.c; this is the same crash the loopback
                 * codec test was built to avoid). */
                s_audio_frames++;
                if ((s_audio_frames % 25u) == 0) {
                    ESP_LOGI(TAG, "%" PRIu32 " audio frames received",
                             s_audio_frames);
                }
                mpx_voice_stream_rx_frame((const uint8_t *)ev->data_ptr, (size_t)ev->data_len);
            }
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "socket closed");
            s_state = MPX_VOICE_IDLE;
            s_session_id[0] = '\0';
            (void)esp_wifi_set_ps(WIFI_PS_NONE);      /* was MIN_MODEM: it made the AP (192.168.2.1) laggy */
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "socket error");
            break;

        default:
            break;
    }
}

/* ── connect ─────────────────────────────────────────────────────────────── */

esp_err_t mpx_voice_connect(const char *url, const char *token)
{
    char headers[VOICE_HEADER_MAX];
    uint8_t mac[6] = {0};

    /* Fall back to whatever `voice provision` discovered, which is the normal
     * case -- the cloud tells the device where to connect, so a hand-typed
     * URL is the exception. */
    if (!url || !url[0]) {
        url = mpx_voice_stored_url();
    }
    if (!token || !token[0]) {
        token = mpx_voice_stored_token();
    }
    ESP_RETURN_ON_FALSE(url && url[0], ESP_ERR_INVALID_ARG, TAG,
                        "no url -- run `voice provision` first");
    if (s_client) {
        mpx_voice_disconnect();
    }

    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    /* Device-Id is the MAC and Client-Id identifies this installation. The
     * xiaozhi server uses Device-Id to tell devices apart, so it has to be
     * stable -- deriving both from the MAC gives that for free. */
    const int written = snprintf(headers, sizeof(headers),
                                 "Authorization: Bearer %s\r\n"
                                 "Protocol-Version: 1\r\n"
                                 "Device-Id: %02x:%02x:%02x:%02x:%02x:%02x\r\n"
                                 "Client-Id: %02x%02x%02x%02x%02x%02x\r\n",
                                 (token && token[0]) ? token : "none",
                                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_RETURN_ON_FALSE(written > 0 && written < (int)sizeof(headers),
                        ESP_ERR_INVALID_SIZE, TAG, "token too long for headers");

    if (!s_handshake) {
        s_handshake = xSemaphoreCreateBinary();
        ESP_RETURN_ON_FALSE(s_handshake, ESP_ERR_NO_MEM, TAG, "out of memory");
    }

    const esp_websocket_client_config_t cfg = {
        .uri = url,
        .headers = headers,
        .task_stack = VOICE_WS_TASK_STACK,
        .buffer_size = 2048,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        /* Manual reconnect. An automatic one re-runs the handshake behind the
         * caller's back, and during bring-up a connection that silently comes
         * back hides the reason it dropped. */
        .disable_auto_reconnect = true,
        /* wss:// needs a server-verification option or esp-tls refuses to set
         * up the TLS session at all (ESP_ERR_MBEDTLS_SSL_SETUP_FAILED, before
         * a single byte goes on the wire). mpx_voice_ota.c already attaches
         * the IDF cert bundle for the OTA HTTPS call; the websocket needs the
         * same thing for the same reason -- it's a different esp-tls caller,
         * not a different problem. */
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    s_client = esp_websocket_client_init(&cfg);
    ESP_RETURN_ON_FALSE(s_client, ESP_FAIL, TAG, "client init failed");

    ESP_RETURN_ON_ERROR(esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY,
                                                      voice_ws_event, NULL),
                        TAG, "cannot register events");

    s_state = MPX_VOICE_CONNECTING;
    s_socket_opened = false;
    ESP_LOGI(TAG, "connecting to %s", url);

    /* Internal RAM before and after, because the websocket client takes its
     * buffers from it and this firmware has very little to give. When the
     * panel starts logging ESP_ERR_NO_MEM the moment a socket opens, these
     * two numbers are the evidence. */
    ESP_LOGI(TAG, "internal RAM before connect: %u free, %u largest",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    const esp_err_t err = esp_websocket_client_start(s_client);
    if (err != ESP_OK) {
        mpx_voice_disconnect();
        return err;
    }

    /* Wait for the server's hello rather than returning on socket-open. A
     * socket that opens and then says nothing is the common failure -- wrong
     * path, wrong token, wrong protocol version -- and reporting success on
     * TCP alone would hide every one of them. */
    if (xSemaphoreTake(s_handshake, pdMS_TO_TICKS(VOICE_HANDSHAKE_MS)) != pdTRUE) {
        if (!s_socket_opened) {
            /* The WebSocket upgrade never completed. If the log above shows
             * "Connection reset by peer" and "Error read response for Upgrade
             * header", TCP reached the server and it hung up without even
             * sending an HTTP response -- which is what a server does when
             * the PATH is not one it serves. A firewall or wrong address
             * fails earlier and differently, with a refusal or a timeout. */
            ESP_LOGE(TAG, "the server closed the connection during the "
                          "WebSocket upgrade");
            ESP_LOGE(TAG, "the address and port are reachable, so the PATH is "
                          "the likely problem -- check what the server prints "
                          "at startup, it names the websocket URL it serves");
        } else {
            ESP_LOGE(TAG, "socket opened but no hello within %d ms -- the token "
                          "or the protocol version is the likely problem",
                     VOICE_HANDSHAKE_MS);
        }
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void mpx_voice_disconnect(void)
{
    if (!s_client) {
        return;
    }
    esp_websocket_client_close(s_client, pdMS_TO_TICKS(1000));
    esp_websocket_client_destroy(s_client);
    s_client = NULL;
    s_state = MPX_VOICE_IDLE;
    s_session_id[0] = '\0';
    (void)esp_wifi_set_ps(WIFI_PS_NONE);      /* was MIN_MODEM: it made the AP (192.168.2.1) laggy */
    ESP_LOGI(TAG, "disconnected");
}

mpx_voice_state_t mpx_voice_state(void) { return s_state; }
const char *mpx_voice_session_id(void) { return s_session_id; }
uint32_t mpx_voice_downlink_rate(void) { return s_downlink_rate; }

const char *mpx_voice_state_name(void)
{
    switch (s_state) {
        case MPX_VOICE_IDLE:        return "idle";
        case MPX_VOICE_CONNECTING:  return "connecting";
        case MPX_VOICE_HANDSHAKING: return "handshaking";
        case MPX_VOICE_READY:       return "ready";
        default:                    return "?";
    }
}
