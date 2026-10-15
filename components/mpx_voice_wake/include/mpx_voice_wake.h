/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * Phase 4 of docs/voice-plan.md: esp-sr's acoustic front end (AEC + VAD +
 * WakeNet) over the microphone, and the hands-free conversation loop built
 * on it.
 *
 * Two things are wired to the wake word ("Hi ESP"):
 *
 *   Barge-in. Hearing it while the robot is mid-reply sends
 *   {"type":"abort","reason":"wake_word_detected"} and stops local playback
 *   immediately. Available in both modes below.
 *
 *   Hands-free (mpx_voice_wake_auto_start(), `wake auto on`, and the boot
 *   default under CONFIG_MP4_VOICE_AUTO_LISTEN). Hearing it from idle hands
 *   the microphone to mpx_voice_talk_start_auto(): beep, stream until the
 *   server's VAD says you are done, play the reply, and go back to
 *   listening -- listening resumes while the reply is still playing, so
 *   the wake word interrupts it and starts the next turn. The socket is
 *   opened, or reopened after the server's idle timeout, on demand.
 *
 * The microphone handoff that made this "deliberately not wired" in the
 * first cut is done the simple way: mpx_audio's capture is still a single
 * stream with no owner tracking, so the hands-free task is the only thing
 * that ever moves it -- it stops the wake front end, waits for that to
 * confirm, starts the talk, waits for the talk to confirm, and starts the
 * front end again. `wake` and `voice talk` still refuse to start over each
 * other, which is what keeps a manual command from breaking the loop.
 *
 * Playback runs at 16 kHz (MPX_VOICE_PLAYBACK_RATE), not the 24 kHz the
 * server encodes at, because the speaker and the microphone share one I2S
 * peripheral and it will not run the two directions at different rates --
 * and barge-in needs the microphone open during the reply.
 *
 * Still unverified on real hardware, and worth reading before trusting
 * barge-in's AEC quality: which raw capture channel actually carries the
 * amplifier's echo-reference feedback. `wake levels` reports both channels'
 * RMS so that can be checked directly -- see its usage text.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Loads esp-sr's models from the "model" partition and builds the AFE
 * instance. Expensive (model load off flash) and done once; does NOT open
 * the microphone or start listening -- that is `wake start` (or
 * `mpx_voice_wake_start()`), so the mic stays free for `voice talk` until
 * a caller actually asks for wake-word detection. Idempotent. */
esp_err_t mpx_voice_wake_init(void);

/* Opens the microphone and starts the feed/fetch tasks. Refuses with
 * ESP_ERR_INVALID_STATE if mpx_audio_capture_active() is already true
 * (something else -- normally `voice talk` -- holds the mic) or if
 * mpx_voice_wake_init() has not run yet. */
esp_err_t mpx_voice_wake_start(void);

/* Stops the feed/fetch tasks and releases the microphone. Waits for both
 * tasks to actually exit before returning, so it is safe to call
 * mpx_voice_talk_start() immediately afterwards. Safe to call when not
 * running. */
void mpx_voice_wake_stop(void);

bool mpx_voice_wake_active(void);

/* Hands-free conversation loop, described in the file header. Runs
 * mpx_voice_wake_init() itself if needed. Idempotent. Needs `voice
 * provision` to have been run once on this device; it reports (and keeps
 * listening) rather than failing if it has not. */
esp_err_t mpx_voice_wake_auto_start(void);

/* Leave hands-free mode: cuts short any talk in progress, stops listening,
 * releases the microphone. Safe to call when not running. */
void mpx_voice_wake_auto_stop(void);

bool mpx_voice_wake_auto_active(void);

/* True while the hands-free loop is in a conversation turn: after the wake
 * word, while the microphone streams to the server, and while the reply
 * arrives and plays. False while it just listens for the wake word. */
bool mpx_voice_wake_conversing(void);

/* Asked when a reply has finished: return true to skip the follow-up turn
 * and go straight back to the wake word (cap_music: a song is waiting for
 * the speaker). NULL = always offer the follow-up. */
typedef bool (*mpx_voice_wake_gate_fn)(void);
void mpx_voice_wake_set_followup_gate(mpx_voice_wake_gate_fn skip_followup);

/* Console command: wake auto|start|stop|info|levels */
void register_wake_command(void);

#ifdef __cplusplus
}
#endif
