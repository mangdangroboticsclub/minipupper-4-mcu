/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * Private: the speaker-tuning stage mpx_audio.c runs every voice and music
 * block through. The public knobs are in mpx_audio.h.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "audio/mpx_audio.h"

typedef struct {
    float    z[2][2];
    uint32_t gen;
} mpx_spk_state_t;

void mpx_spk_reset(mpx_spk_state_t *st);

/* mono in -> tuned, volume-scaled, duplicated to both I2S slots */
void mpx_spk_block(mpx_spk_state_t *st, const int16_t *in, int16_t *stereo, size_t n, int volume);
