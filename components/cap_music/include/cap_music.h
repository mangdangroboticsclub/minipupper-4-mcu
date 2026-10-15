/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * Music on the robot's speaker (internet radio by genre, or an audio URL),
 * sharing the speaker with voice. See cap_music.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the player task, the `music` console command and the voice hook. */
void cap_music_init(void);

/** The "cap_music" capability group: music_play, music_stop. */
esp_err_t cap_music_register_group(void);

/** Play a station for a genre/mood (NULL or no match = a random one), or an
 *  http(s) URL. `what` gets the station or file name. Starts once nobody is
 *  talking to the robot. */
esp_err_t cap_music_play(const char *genre_or_url, char *what, size_t what_size);
void      cap_music_stop(void);
bool      cap_music_active(void);
void      cap_music_set_level(int percent);
size_t    cap_music_describe(char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
