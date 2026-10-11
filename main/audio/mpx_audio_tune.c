/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * Speaker tuning: the last stage before the amplifier, on the robot.
 *
 * The speaker (FUET FS1510MS0810, 15 x 10 mm, 8 ohm, 1 W rated / 1.5 W max)
 * plays 750 Hz - 20 kHz and resonates at 1250 Hz. The MAX98357A in front of
 * it has no volume control and pushes ~1.4 W into 8 ohms at full scale.
 * Three things make that crackle, and each has a knob here:
 *
 *   hpf      sound below the speaker's range makes no sound, it only throws
 *            the cone against its stops -- the crackle. A 4th-order (24 dB per
 *            octave) Butterworth high-pass at `hpf` Hz removes it.
 *   gain     what is left can be made louder ...
 *   ceiling  ... up to this fraction of full scale, approached smoothly (a
 *            tanh knee from 70% of it), so peaks round off instead of
 *            clipping. Clipping is a crackle of its own.
 *
 * Then the volume setting scales the result, so `ceiling` is what the
 * speaker gets at 100% and the volume only ever turns it down.
 *
 * This used to happen on the voice server, which meant tuning a speaker by
 * redeploying a server. Now the robot does it -- for voice replies and music
 * alike -- tells the server so in its hello ("speaker_dsp"), and the server
 * sends its speech unshaped. `audio tune` changes it live and saves it.
 *
 * All of it runs at the voice rate (16 kHz), the only rate voice and music
 * play at on this board.
 */

#include "audio/mpx_audio_tune.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "mpx_audio_tune";

#define TUNE_RATE          16000.0f
#define TUNE_NVS_NS        "voice"
#define TUNE_DEFAULT_HPF   600
#define TUNE_DEFAULT_GAIN  0.0f
#define TUNE_DEFAULT_CEIL  0.75f

static mpx_audio_tune_t s_tune = {
    .on = true, .hpf_hz = TUNE_DEFAULT_HPF, .gain_db = TUNE_DEFAULT_GAIN,
    .ceiling = TUNE_DEFAULT_CEIL,
};
static float    s_bq[2][5];          /* b0 b1 b2 a1 a2 per section */
static int      s_sections;
static float    s_gain = 1.0f, s_ceil = TUNE_DEFAULT_CEIL, s_knee = TUNE_DEFAULT_CEIL * 0.7f;
static volatile uint32_t s_gen = 1;   /* bumped on every change: streams reset their state */
static bool     s_loaded;

static void design(void)
{
    static const float qs[2] = { 0.5412f, 1.3066f };
    s_sections = 0;
    if (s_tune.on && s_tune.hpf_hz > 0 && s_tune.hpf_hz < (int)(TUNE_RATE / 2)) {
        const float w0 = 2.0f * (float)M_PI * (float)s_tune.hpf_hz / TUNE_RATE;
        const float cw = cosf(w0), sw = sinf(w0);
        for (int i = 0; i < 2; i++) {
            const float alpha = sw / (2.0f * qs[i]);
            const float a0 = 1.0f + alpha;
            s_bq[i][0] = ((1.0f + cw) / 2.0f) / a0;
            s_bq[i][1] = (-(1.0f + cw)) / a0;
            s_bq[i][2] = ((1.0f + cw) / 2.0f) / a0;
            s_bq[i][3] = (-2.0f * cw) / a0;
            s_bq[i][4] = (1.0f - alpha) / a0;
        }
        s_sections = 2;
    }
    s_gain = s_tune.on ? powf(10.0f, s_tune.gain_db / 20.0f) : 1.0f;
    s_ceil = s_tune.on ? s_tune.ceiling : 1.0f;
    s_knee = s_ceil * 0.7f;
    s_gen++;
}

static void load_once(void)
{
    nvs_handle_t h;
    int32_t v;

    if (s_loaded) {
        return;
    }
    s_loaded = true;
    if (nvs_open(TUNE_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_i32(h, "t_on", &v) == ESP_OK) {
            s_tune.on = v != 0;
        }
        if (nvs_get_i32(h, "t_hpf", &v) == ESP_OK && v >= 0 && v <= 4000) {
            s_tune.hpf_hz = (int)v;
        }
        if (nvs_get_i32(h, "t_gain", &v) == ESP_OK && v >= -200 && v <= 200) {
            s_tune.gain_db = (float)v / 10.0f;
        }
        if (nvs_get_i32(h, "t_ceil", &v) == ESP_OK && v >= 10 && v <= 100) {
            s_tune.ceiling = (float)v / 100.0f;
        }
        nvs_close(h);
    }
    design();
}

void mpx_audio_tune_get(mpx_audio_tune_t *out)
{
    load_once();
    if (out) {
        *out = s_tune;
    }
}

esp_err_t mpx_audio_tune_set(const mpx_audio_tune_t *t)
{
    nvs_handle_t h;

    if (!t || t->hpf_hz < 0 || t->hpf_hz > 4000 || t->gain_db < -20.0f || t->gain_db > 20.0f ||
            t->ceiling < 0.1f || t->ceiling > 1.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    load_once();
    s_tune = *t;
    design();
    /* Callers are the console and settings tasks (internal-RAM stacks). */
    if (nvs_open(TUNE_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "t_on", s_tune.on ? 1 : 0);
        nvs_set_i32(h, "t_hpf", s_tune.hpf_hz);
        nvs_set_i32(h, "t_gain", (int32_t)lroundf(s_tune.gain_db * 10.0f));
        nvs_set_i32(h, "t_ceil", (int32_t)lroundf(s_tune.ceiling * 100.0f));
        nvs_commit(h);
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "tuning not saved: NVS unavailable");
    }
    ESP_LOGI(TAG, "speaker: %s, high-pass %d Hz, gain %+.1f dB, ceiling %.2f",
             s_tune.on ? "tuned" : "raw", s_tune.hpf_hz, (double)s_tune.gain_db,
             (double)s_tune.ceiling);
    return ESP_OK;
}

void mpx_audio_tune_defaults(mpx_audio_tune_t *out)
{
    out->on = true;
    out->hpf_hz = TUNE_DEFAULT_HPF;
    out->gain_db = TUNE_DEFAULT_GAIN;
    out->ceiling = TUNE_DEFAULT_CEIL;
}

bool mpx_audio_speaker_dsp(void)
{
    load_once();
    return s_tune.on;
}

void mpx_spk_reset(mpx_spk_state_t *st)
{
    memset(st, 0, sizeof(*st));
}

void mpx_spk_block(mpx_spk_state_t *st, const int16_t *in, int16_t *stereo, size_t n, int volume)
{
    const float vol = (float)volume / 100.0f;

    load_once();
    if (st->gen != s_gen) {
        memset(st->z, 0, sizeof(st->z));     /* settings changed: start clean */
        st->gen = s_gen;
    }
    for (size_t i = 0; i < n; i++) {
        float x = (float)in[i] / 32768.0f;
        for (int s = 0; s < s_sections; s++) {   /* transposed direct form II */
            const float *b = s_bq[s];
            const float y = b[0] * x + st->z[s][0];
            st->z[s][0] = b[1] * x - b[3] * y + st->z[s][1];
            st->z[s][1] = b[2] * x - b[4] * y;
            x = y;
        }
        x *= s_gain;
        const float a = fabsf(x);
        if (a > s_knee) {
            const float span = s_ceil - s_knee;
            const float y = span > 0.0f ? s_knee + span * tanhf((a - s_knee) / span) : s_ceil;
            x = x < 0.0f ? -y : y;
        }
        x *= vol;
        const int32_t v = (int32_t)lrintf(x * 32767.0f);
        const int16_t o = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        stereo[i * 2] = o;
        stereo[i * 2 + 1] = o;
    }
}
