/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * Phase 2 of docs/voice-plan.md: Opus, then the socket.
 *
 * This file currently covers the codec half only. The gate for it is
 * `voice loopback`: capture from the microphone, encode to Opus, decode
 * straight back, and play. If that sounds like you, the codec, the frame
 * sizes and the buffer handling are all correct -- and none of it depends on
 * a server existing, which is the point of doing it first.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 16 kHz up, 24 kHz down, 60 ms frames -- the xiaozhi protocol's numbers, and
 * what the server will negotiate in its hello reply. Fixed here so the frame
 * arithmetic elsewhere has one place to read them from. */
#define MPX_VOICE_UPLINK_RATE     16000
#define MPX_VOICE_DOWNLINK_RATE   24000
#define MPX_VOICE_FRAME_MS        60
#define MPX_VOICE_UPLINK_FRAME    ((MPX_VOICE_UPLINK_RATE / 1000) * MPX_VOICE_FRAME_MS)
#define MPX_VOICE_DOWNLINK_FRAME  ((MPX_VOICE_DOWNLINK_RATE / 1000) * MPX_VOICE_FRAME_MS)

/* An Opus frame at these rates is a few hundred bytes; this is generous. */
#define MPX_VOICE_PACKET_MAX      1500

/* The rate the SPEAKER runs at, which is not the downlink rate. The server
 * encodes TTS at 24 kHz, but Opus decodes to whatever rate the decoder was
 * opened with, and the decoder here is opened at 16 kHz on purpose: the
 * ES7210 (mic) and the MAX98357A (speaker) sit on the same I2S peripheral,
 * and esp_codec_dev refuses to open the second direction at a different
 * sample rate from the first ("conflict sample_rate ... with peer mode").
 * The wake-word front end keeps the microphone open at 16 kHz while a reply
 * plays -- that is how barge-in hears you -- so the reply has to play at
 * 16 kHz too. Wideband speech loses nothing audible in the process. */
#define MPX_VOICE_PLAYBACK_RATE   16000

esp_err_t mpx_voice_codec_start(uint32_t encode_rate, uint32_t decode_rate);
void      mpx_voice_codec_stop(void);
bool      mpx_voice_codec_running(void);

/* Returns the encoded length, or a negative esp_err_t on failure. */
int mpx_voice_encode(const int16_t *pcm, size_t frames, uint8_t *out, size_t out_size);

/* Returns the number of PCM frames produced, or negative on failure. */
int mpx_voice_decode(const uint8_t *packet, size_t len, int16_t *pcm, size_t max_frames);

/* Microphone to Opus to speaker, with nothing in between. */
esp_err_t mpx_voice_loopback(uint32_t seconds);

/* ── The link ──────────────────────────────────────────────────────────────
 *
 * A xiaozhi-protocol WebSocket. This half is the connection and the
 * handshake; audio streaming follows once a server answers.
 *
 * The gate for it is `voice connect ws://<host>:<port>/xiaozhi/v1/` printing
 * the server's hello reply and a session id. That proves the URL, the
 * headers, the JSON shape and the protocol version all agree, which is worth
 * establishing before audio is in the picture -- a handshake mismatch and a
 * codec mismatch look identical from the outside. */
typedef enum {
    MPX_VOICE_IDLE = 0,      /* no socket */
    MPX_VOICE_CONNECTING,    /* socket opening */
    MPX_VOICE_HANDSHAKING,   /* our hello sent, waiting for theirs */
    MPX_VOICE_READY,         /* session established */
} mpx_voice_state_t;

/* ── Provisioning ──────────────────────────────────────────────────────────
 *
 * The websocket URL is not something you configure. The device asks an OTA
 * endpoint for it, and the reply carries the address and the token -- plus,
 * the first time, a six-digit code to type into the Xiaozhi console to bind
 * the device to an agent.
 *
 * So the order is: provision, read the code, bind it in the console,
 * provision again, then connect. */
/* mpv2-claw: the MangDang voice gateway. It needs no activation code, so a
 * robot provisions itself the first time the wake word is heard. */
#ifdef CONFIG_MP4_VOICE_OTA_URL
#define MPX_VOICE_DEFAULT_OTA_URL  CONFIG_MP4_VOICE_OTA_URL
#else
#define MPX_VOICE_DEFAULT_OTA_URL  "http://35.220.215.160:8000/xiaozhi/ota/"
#endif

esp_err_t mpx_voice_provision(const char *ota_url);
const char *mpx_voice_stored_url(void);
const char *mpx_voice_stored_token(void);

/* Passing NULL for either uses whatever provisioning stored. */
esp_err_t mpx_voice_connect(const char *url, const char *token);
void      mpx_voice_disconnect(void);

mpx_voice_state_t mpx_voice_state(void);
const char *mpx_voice_state_name(void);
const char *mpx_voice_session_id(void);
uint32_t    mpx_voice_downlink_rate(void);

/* Send one control message. `json` is sent verbatim, so the caller owns the
 * shape -- this is deliberately thin while the protocol is being brought up. */
esp_err_t mpx_voice_send_json(const char *json);

/* Send one binary WebSocket frame -- an Opus packet, uplink. */
esp_err_t mpx_voice_send_binary(const uint8_t *data, size_t len);

/* ── Streaming (Phase 2b) ──────────────────────────────────────────────────
 *
 * mpx_voice_ws.c owns the socket and knows a frame arrived; it does not know
 * Opus or the audio hardware, so it hands binary frames off here rather than
 * decoding them itself -- same reasoning as the MCP sink above, one layer
 * down. mpx_voice_stream_init() is safe to call more than once (the second
 * call is a no-op) and mpx_voice_talk_start() calls it, so nothing else
 * needs to call it directly except mpx_voice_ws.c wiring the RX side up
 * before a talk ever happens. */
esp_err_t mpx_voice_stream_init(void);

/* RX: mpx_voice_ws.c calls this from the websocket event handler for every
 * binary (op_code 0x02) frame. Copies and queues -- never decodes inline,
 * see mpx_voice_stream.c's file header for why. */
void mpx_voice_stream_rx_frame(const uint8_t *data, size_t len);

/* RX: mpx_voice_ws.c calls this when a {"type":"tts","state":"stop"}
 * message arrives, so playback closes the speaker instead of leaving I2S
 * running with nothing feeding it. */
void mpx_voice_stream_on_tts_stop(void);

/* Push-to-talk: open the mic, stream Opus uplink frames until told to stop
 * or `max_seconds` elapses (0 = no limit, use `mpx_voice_talk_stop`), then
 * close the mic and send listen/stop. Requires MPX_VOICE_READY. Refuses
 * with ESP_ERR_INVALID_STATE if something else already holds the
 * microphone open (mpx_audio_capture_active()) -- see mpx_voice_wake for
 * the component that's usually why. */
esp_err_t mpx_voice_talk_start(uint32_t max_seconds);
void      mpx_voice_talk_stop(void);
bool      mpx_voice_talk_active(void);

/* Hands-free variant, used by mpx_voice_wake after the wake word. Sends
 * listen/start in "auto" mode, so the SERVER decides when you have stopped
 * speaking (its VAD, not a fixed window): the microphone streams until the
 * server takes its turn -- an "stt" transcript or the first "tts" message
 * arrives (see mpx_voice_stream_on_server_turn()) -- or `max_seconds` passes
 * with nothing heard, whichever is first. Same preconditions as
 * mpx_voice_talk_start(). Returns as soon as the talk task is up; use
 * mpx_voice_talk_wait() to block until it has released the microphone. */
esp_err_t mpx_voice_talk_start_auto(uint32_t max_seconds);

/* Block until the current talk (either kind) has finished and closed the
 * microphone, or `timeout_ms` passes. ESP_OK when not talking. */
esp_err_t mpx_voice_talk_wait(uint32_t timeout_ms);

/* After a talk has ended: did the server take its turn (a reply is on its
 * way), or did the talk end on its own -- timeout, or `voice talk stop`?
 * The hands-free loop uses this to tell "wait for the reply" from "nothing
 * was said, go back to the wake word". */
bool mpx_voice_talk_server_took_turn(void);

/* Counts "tts stop" messages, i.e. replies the server has finished
 * sending. Compare before and after to know a reply has ended; combine
 * with mpx_voice_stream_is_playing() to know it has finished playing. */
uint32_t mpx_voice_stream_tts_stop_count(void);

/* mpx_voice_ws.c calls this when the server has clearly taken its turn in
 * the conversation -- an "stt" result, or "tts" start/sentence_start -- so
 * an auto-mode talk stops streaming instead of feeding the server its own
 * reply through the microphone. Harmless when nothing is talking. */
void mpx_voice_stream_on_server_turn(void);

/* True while a TTS reply is actively being decoded and played -- i.e.
 * between the first downlink audio frame of a reply and its "tts stop".
 * mpx_voice_wake uses this to decide whether a wake word heard mid-reply
 * means barge-in (see mpx_voice_stream_abort_playback() below) or just an
 * ordinary wake from idle. */
bool mpx_voice_stream_is_playing(void);

/* Barge-in: stop playback immediately, discarding any already-queued
 * downlink frames, rather than the graceful drain mpx_voice_stream_on_tts_stop()
 * does for a normal end-of-reply. The caller is still responsible for
 * telling the server -- this only silences the speaker on this end. */
void mpx_voice_stream_abort_playback(void);

/* ── MCP bridge hook (Phase 3) ─────────────────────────────────────────────
 *
 * This component knows the xiaozhi wire format -- {"type":"mcp","payload":
 * {...}} -- but nothing about MCP itself; that lives in mpx_mcp_ws, which
 * would have to depend back on this component to send replies, so the
 * dependency only runs one direction: mpx_mcp_ws registers a sink here at
 * its own init time, and voice_handle_text() calls it when an "mcp" message
 * arrives, instead of this component including mpx_mcp_ws.h.
 *
 * `sink` receives ownership of a heap-allocated, NUL-terminated JSON-RPC
 * string (the "payload" object, re-serialized) and must free it -- there is
 * exactly one message in flight per call, no batching. Before a sink is
 * registered, "mcp" messages are logged and dropped. */
typedef void (*mpx_voice_mcp_sink_t)(const char *jsonrpc_owned);
void mpx_voice_set_mcp_sink(mpx_voice_mcp_sink_t sink);

/* ── Face / screen hooks (Phase 5) ─────────────────────────────────────────
 *
 * The server narrates the conversation as it goes: an "stt" transcript of
 * what you said, "tts sentence_start" for each sentence it is about to say,
 * an "llm" emotion hint per reply, and "tts stop" / "goodbye" when it is
 * done. This component only knows the wire format; what to do with them on
 * a panel is cap_display's business, and cap_display is only in the build on
 * boards with an LCD -- so, same shape as the MCP sink above, main.c
 * registers a set of callbacks here at boot instead of this component
 * including cap_display.h.
 *
 * All callbacks run on the websocket task, so they must be quick and must
 * not call back into this component. Any may be NULL. Strings are only valid
 * for the duration of the call. */
typedef struct {
    void (*heard)(const char *text);        /* "stt": what the server understood */
    void (*saying)(const char *text);       /* "tts sentence_start": the next sentence */
    void (*emotion)(const char *name);      /* "llm": xiaozhi emotion name, e.g. "happy" */
    void (*reply_done)(void);               /* "tts stop": the reply has been sent */
    void (*session_ended)(void);            /* "goodbye": the conversation is over */
} mpx_voice_ui_hooks_t;

void mpx_voice_set_ui_hooks(const mpx_voice_ui_hooks_t *hooks);

/* Called (on the websocket task -- keep it quick) when the server ends the
 * session because you asked for quiet ("stop talking", "be quiet"):
 * {"type":"goodbye","reason":"stop"}. cap_music stops the music. */
void mpx_voice_set_quiet_hook(void (*hook)(void));

/* ── Device brain ("own" AI mode) ──────────────────────────────────────────
 *
 * Normally the server answers: it transcribes, asks its own LLM, speaks the
 * reply. With the device brain on, the hello advertises features.brain =
 * "device" and the server stops after transcribing: it sends the "stt" text
 * here and waits for the robot to say what to speak, as
 * {"type":"speak","text":...,"last":true}. The robot's own agent -- with the
 * owner's own LLM key, which never leaves the robot -- writes that answer.
 *
 * The sink receives each transcript as a heap copy it must free(). It runs on
 * the websocket task, so it must only queue the text, never block. */
typedef void (*mpx_voice_stt_sink_t)(char *text_owned);

void mpx_voice_set_device_brain(bool on, mpx_voice_stt_sink_t sink);
bool mpx_voice_device_brain(void);
/* Send what the robot's brain wants spoken; `last` ends the reply. */
esp_err_t mpx_voice_send_speak(const char *text, bool last);

void register_voice_command(void);

#ifdef __cplusplus
}
#endif
