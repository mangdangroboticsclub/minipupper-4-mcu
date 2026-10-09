/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * Voice on this firmware: what mpv2-claw's main.c does around mpx_voice_link,
 * mpx_voice_wake, cap_music and the MCP bridge, in one place.
 *
 *   - The owner's choice, "AI & Voice" in the app (GET/POST /v1/ai): mangdang
 *     (MangDang's cloud does speech and answers; the wake word works) or off
 *     (no voice, the microphone never opens). mpv2-claw's third mode, "own"
 *     (the robot's own agent with the owner's LLM key), is phase 5 and not
 *     offered here. Saved in NVS; applied live, no restart.
 *   - Boot: the face hooks (what was heard / said / felt goes on the screen),
 *     the music player, the tool bridge, then esp-sr's front end and the
 *     hands-free loop when the mode allows.
 *   - The Raspberry Pi taking control: voice and music off while it drives,
 *     hands-free back when it lets go (the Pi may use the speaker itself).
 *
 * Compiled with voice off too (CONFIG_APP_VOICE_ENABLE=n): everything here
 * then reports "not available" and /v1/ai offers only "off".
 */
#pragma once

#include <string>

#include "esp_err.h"

namespace voice_glue {

enum class Mode { MangDang, Off };

/** Is voice compiled in and does the board have a microphone? */
bool available();

Mode        mode();                     /*!< the saved choice (Off when voice is unavailable) */
const char *mode_name(Mode m);          /*!< "mangdang" | "off" */
bool        parse_mode(const char *name, Mode *out);

/** Save the mode and apply it now: start or stop hands-free listening.
 *  ESP_ERR_NOT_SUPPORTED for MangDang when voice is unavailable. */
esp_err_t set_mode(Mode m);

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
