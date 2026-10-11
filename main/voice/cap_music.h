/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * Music on the robot's speaker (internet radio by genre, or an audio URL),
 * sharing the speaker with voice. See cap_music.c.
 *
 * mpx-dog (local-claw): mpv2-claw's cap_music without the claw_cap tool
 * descriptors (the voice server reaches these through voice_tools.cc) and
 * without the `music` console command (/v1/voice does that).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the player task and the voice hooks (follow-up gate, "be quiet"). */
void cap_music_init(void);

/** Play a station for a genre/mood (NULL or no match = a random one), or an
 *  http(s) URL. `what` gets the station or file name. Starts once nobody is
 *  talking to the robot. */
esp_err_t cap_music_play(const char *genre_or_url, char *what, size_t what_size);
void      cap_music_stop(void);
bool      cap_music_active(void);
void      cap_music_set_level(int percent);     /*!< 5..100, saved in NVS */
int       cap_music_level(void);
size_t    cap_music_describe(char *out, size_t out_size);

/** Does this genre/mood pick a known station (true), or would it fall back
 *  to a random one (false)? For the tool reply's wording. */
bool cap_music_matches_station(const char *genre);

/** Station catalogue, for /v1/voice: the i-th title/keywords, or NULL. */
int         cap_music_station_count(void);
const char *cap_music_station_title(int index);
const char *cap_music_station_keywords(int index);

/** What is playing now: title, or "" when off. */
const char *cap_music_title(void);

#ifdef __cplusplus
}
#endif
