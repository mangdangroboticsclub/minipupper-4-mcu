/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * Phase 1 of docs/voice-plan.md: get sound in and out, and nothing else.
 *
 * No Opus, no WebSocket, no wake word, no AI. The point of this layer is to
 * answer one question on real hardware -- do the microphones and the speaker
 * work, and which channel is which -- because every later phase is miserable
 * to debug if that answer is uncertain.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up I2S0 (full duplex) and the ES7210 on the shared I2C bus.
 *
 * Never fatal. With CONFIG_APP_AUDIO_ENABLE off, or hardware that does not
 * answer, mpx_audio_have_mic()/have_speaker() stay false and every call
 * below returns ESP_ERR_NOT_SUPPORTED. Call after the IMU is up: the two
 * share one I2C bus and the ES7210 init writes a few dozen registers. */
esp_err_t mpx_audio_init(void);

bool mpx_audio_have_mic(void);
bool mpx_audio_have_speaker(void);

/* Record to a mono 16-bit WAV under the data root.
 *
 * rel_path is relative to the LittleFS root (/fs), e.g. "audio/rec.wav"; parent
 * directories are created. `channels` is how many the codec is opened with
 * and `pick` selects which one is written, so a four-microphone ES7210 can be
 * examined one channel at a time -- including the echo-cancellation
 * reference, which should carry whatever the speaker just played. */
esp_err_t mpx_audio_record_wav(const char *rel_path,
                               uint32_t seconds,
                               uint32_t sample_rate,
                               uint8_t channels,
                               uint8_t pick);

/* Play a 16-bit PCM WAV from the data root. Mono files are duplicated to both
 * I2S slots; the sample rate comes from the file. */
esp_err_t mpx_audio_play_wav(const char *rel_path);

/* Play a sine wave, generated on the fly.
 *
 * The point of this is diagnosis: it takes the microphone, the filesystem and
 * the recording out of the question entirely. If a tone is audible then the
 * amplifier, the speaker, the I2S wiring and the volume are all fine, and
 * anything still wrong is on the capture side. If it is not audible, nothing
 * on the capture side can be blamed. */
esp_err_t mpx_audio_play_tone(uint32_t hz, uint32_t seconds);

/* Software volume, 0-100.
 *
 * Software because the MAX98357A on these boards has no control interface at
 * all: SD_MODE is strapped and GAIN goes to a test point, so
 * esp_codec_dev_set_out_vol() is a no-op. Applied to the samples before they
 * reach I2S. */
void mpx_audio_set_volume(int percent);
int  mpx_audio_get_volume(void);
/* Like mpx_audio_set_volume() but NOT saved to NVS: for a caller that
 * borrows the output level for a while and puts it back (the Pi's music,
 * mpx_pi_link, runs the output at 100 % and scales its own samples). Safe
 * from a PSRAM-stack task, which mpx_audio_set_volume() is not. */
void mpx_audio_set_volume_transient(int percent);

/* ── Streaming ─────────────────────────────────────────────────────────────
 *
 * The file-based calls above are for bring-up. Everything after Phase 1 wants
 * frames: capture a block, encode it, send it; receive a block, decode it,
 * play it. These keep the codec open across calls so that is possible.
 *
 * Capture yields MONO -- one channel picked out of `channels`, as with
 * mpx_audio_record_wav -- and playback takes mono and duplicates it to both
 * I2S slots. Mono is what Opus and every speech model want.
 *
 * At most MPX_AUDIO_MAX_FRAMES per call. A 60 ms Opus frame at 16 kHz is 960,
 * so this is two of them. */
#define MPX_AUDIO_MAX_FRAMES  1920

esp_err_t mpx_audio_capture_start(uint32_t sample_rate, uint8_t channels, uint8_t pick);
esp_err_t mpx_audio_capture_read(int16_t *mono, size_t frames);
void      mpx_audio_capture_stop(void);

/* True between a successful mpx_audio_capture_start() and the matching
 * mpx_audio_capture_stop() -- the codec is a single hardware stream, so
 * whoever calls capture_start() next while this is true does NOT get an
 * error (capture_start() is intentionally idempotent, for a caller
 * resuming its own already-open capture) but WOULD be reading the same
 * stream as whoever opened it, silently interleaving with them. Callers
 * that do not own the capture themselves -- mpx_voice_wake's `wake start`,
 * mpx_voice_link's `voice talk` -- check this first and refuse instead of
 * risking that. */
bool mpx_audio_capture_active(void);

/* How many channels the current (or most recent) capture_start() opened.
 * Needed by a caller reading the interleaved raw stream below, since it
 * has to size its own buffer -- mpx_audio_capture_read()'s mono callers
 * never needed to know this. */
uint8_t mpx_audio_capture_channels(void);

/* Same capture, without the de-interleave-to-mono step: `interleaved` gets
 * `frames * mpx_audio_capture_channels()` samples, straight off the codec,
 * in whatever order the ES7210's TDM slots deliver them (see
 * mpx_audio_cmd.c's usage text for what is documented about that order,
 * and mpx_voice_wake's comments for what is actually verified). For a
 * multi-mic consumer like an AFE front end that needs every channel, not
 * just one picked out of it. */
esp_err_t mpx_audio_capture_read_raw(int16_t *interleaved, size_t frames);

esp_err_t mpx_audio_output_start(uint32_t sample_rate);
esp_err_t mpx_audio_output_write(const int16_t *mono, size_t frames);
void      mpx_audio_output_stop(void);

/* Music shares the speaker with voice, and voice always wins: between
 * mpx_audio_output_start() and _stop() (a reply, the wake beep) music
 * writes are refused with ESP_ERR_INVALID_STATE, and the player waits.
 * Music is mono at MPX_AUDIO_MUSIC_RATE -- the voice rate, because the
 * microphone shares the I2S clock and the wake word listens at 16 kHz. The
 * volume setting applies. Blocks while the samples go out (it paces the
 * player). mpx_audio_music_end() leaves silence and lets the channel go. */
#define MPX_AUDIO_MUSIC_RATE  16000u

/* Speaker tuning (mpx_audio_tune.c): every voice reply and every song goes
 * through high-pass -> gain -> soft ceiling -> volume before the amplifier.
 * `audio tune` on the console; saved in NVS. While `on`, the robot tells the
 * voice server ("speaker_dsp" in its hello) and gets unshaped speech. */
typedef struct {
    bool  on;        /* false: samples go out as they come (only the volume) */
    int   hpf_hz;    /* 4th-order high-pass corner, 0 = none (default 600) */
    float gain_db;   /* after the high-pass, -20..+20 (default 0) */
    float ceiling;   /* soft limit, fraction of full scale, 0.1..1 (default 0.75) */
} mpx_audio_tune_t;

void      mpx_audio_tune_get(mpx_audio_tune_t *out);
esp_err_t mpx_audio_tune_set(const mpx_audio_tune_t *t);     /* applies live, saves */
void      mpx_audio_tune_defaults(mpx_audio_tune_t *out);
bool      mpx_audio_speaker_dsp(void);
bool      mpx_audio_voice_output_active(void);
esp_err_t mpx_audio_music_write(const int16_t *mono, size_t frames);
void      mpx_audio_music_end(void);

/* Microphone gain in dB, 0-60, applied on the next recording. */
void mpx_audio_set_mic_gain(int db);
int  mpx_audio_get_mic_gain(void);

#ifdef __cplusplus
}
#endif
