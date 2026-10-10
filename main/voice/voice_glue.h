/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * Voice on this firmware: what mpv2-claw's main.c does around mpx_voice_link,
 * mpx_voice_wake, cap_music and the MCP bridge, in one place.
 *
 *   - The owner's choice, "AI & Voice" in the app (GET/POST /v1/ai), which
 *     agent_config owns since Phase 5: mangdang and own (voice goes to the
 *     MangDang voice gateway; the wake word works) or off (no voice, the
 *     microphone never opens). In "own" the on-device agent uses the
 *     owner's LLM for chat, but a voice turn is still answered by the
 *     gateway -- the voice -> local agent handoff is later work. Saved in
 *     NVS; applied live, no restart.
 *   - Boot: the face hooks (what was heard / said / felt goes on the screen),
 *     the music player, the tool bridge, then esp-sr's front end and the
 *     hands-free loop when the mode allows.
 *   - The Raspberry Pi taking control: voice and music off while it drives,
 *     hands-free back when it lets go (the Pi may use the speaker itself).
 *
 * Compiled with voice off too (CONFIG_APP_VOICE_ENABLE=n): everything here
 * then reports "not available".
 */
#pragma once

#include <string>

#include "esp_err.h"

namespace voice_glue {

/** Is voice compiled in and does the board have a microphone? */
bool available();

/** Does the saved AI mode (agent_config) allow voice at all? */
bool voice_wanted();

/** The AI mode changed (POST /v1/ai, already saved by agent_config): start
 *  or stop hands-free listening to match. ESP_ERR_NOT_SUPPORTED when voice
 *  is wanted but unavailable (no microphone); the mode stays saved. */
esp_err_t apply_mode();

/** Boot wiring. After mpx_audio_init(), the display and the HTTP server
 *  (the tools start skills through network::skill_start). Never fatal. */
void start();

/** The Raspberry Pi took (true) or released (false) the robot. Runs on the
 *  Pi link task: this only posts; a small task does the stopping. */
void on_master_changed(bool active);

/** Start hands-free listening now (mode must be MangDang). For a retry from
 *  /v1/voice after "could not start". */
esp_err_t hands_free_start();
void      hands_free_stop();

/** Provision against the gateway now (normally automatic on first use). */
esp_err_t provision();

/** Status for GET /v1/voice, as a JSON object string. */
std::string status_json();

}  // namespace voice_glue
