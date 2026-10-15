/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * `audio` console command -- the Phase 1 gate from docs/voice-plan.md.
 *
 * Deliberately a console command and not an HTTP route or an agent tool.
 * Bringing up audio means recording, listening, changing one parameter and
 * recording again, and a serial prompt is the shortest loop for that. It also
 * works before Wi-Fi is up, which matters when the thing being tested is a
 * microphone rather than a network.
 */

#include "mpx_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"

static const char *TAG = "audio_cmd";

#define AUDIO_DEFAULT_PATH      "audio/rec.wav"
#define AUDIO_DEFAULT_SECONDS   5
#define AUDIO_DEFAULT_RATE      16000
#define AUDIO_DEFAULT_CHANNELS  2

static void audio_usage(void)
{
    printf("\n"
           "  audio info                       what the board has, and the current volume\n"
           "  audio rec [secs] [pick] [chans]  record to /fatfs/%s\n"
           "  audio tone [hz] [secs]           play a generated sine -- speaker test\n"
           "  audio play [path]                play a 16-bit WAV from the data root\n"
           "  audio vol [0-100]                software output volume (saved, default 80)\n"
           "  audio gain [0-60]                microphone gain in dB (default 30)\n"
           "  audio tune                       speaker tuning (see `audio tune help`)\n"
           "\n"
           "Recording keeps ONE channel out of `chans`, chosen by `pick`, so the\n"
           "microphones can be told apart. On the ES7210 boards the channel order\n"
           "declared in board_devices.yaml is [mic4, mic3, mic2, mic1], where mic3\n"
           "is the echo reference wired from the amplifier output -- record it\n"
           "while something is playing and you should hear the playback.\n"
           "\n"
           "If `audio rec` fails to open the codec, the channel count is the thing\n"
           "to change: the ES7210 runs in TDM and the I2S slot configuration may\n"
           "not agree with what was asked for. Try 1, 2 and 4.\n"
           "\n"
           "  audio tone 440 2                 if this is silent, it is not the mic\n"
           "  audio rec 5 0 2                  5 s, first channel of two\n"
           "  audio play audio/rec.wav         hear it back through the speaker\n"
           "\n"
           "Off-device: http://<robot-ip>/files/audio/rec.wav downloads it.\n"
           "\n", AUDIO_DEFAULT_PATH);
}

static void audio_tune_print(void)
{
    mpx_audio_tune_t t;
    mpx_audio_tune_get(&t);
    if (!t.on) {
        printf("  speaker tuning OFF (raw samples, volume %d%%) -- the server shapes speech\n",
               mpx_audio_get_volume());
        return;
    }
    printf("  speaker tuning: high-pass %d Hz, gain %+.1f dB, ceiling %.2f, volume %d%%\n",
           t.hpf_hz, (double)t.gain_db, (double)t.ceiling, mpx_audio_get_volume());
}

/* audio tune [hpf <Hz>] [gain <dB>] [ceiling <0.1-1>] | on | off | reset | help */
static int audio_tune_cmd(int argc, char **argv)
{
    mpx_audio_tune_t t;
    mpx_audio_tune_get(&t);

    if (argc >= 2 && strcmp(argv[1], "help") == 0) {
        printf("\n"
               "  audio tune                    show the speaker tuning\n"
               "  audio tune hpf 700            cut below 700 Hz (4th order). Crackle on\n"
               "                                low/booming words: raise it (700-900)\n"
               "  audio tune gain 3             louder after the cut, in dB (-20..20)\n"
               "  audio tune ceiling 0.7        loudest peak, fraction of full scale. Harsh\n"
               "                                or buzzy when loud: lower it (0.6-0.7)\n"
               "  audio tune off | on           bypass (the server shapes speech again)\n"
               "  audio tune reset              back to 600 Hz, 0 dB, 0.75\n"
               "\n"
               "  Changes apply to the next sound at once and are saved. Voice and\n"
               "  music both go through it. A new voice session picks up on|off.\n"
               "\n");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "reset") == 0) {
        mpx_audio_tune_defaults(&t);
    } else if (argc >= 2 && strcmp(argv[1], "on") == 0) {
        t.on = true;
    } else if (argc >= 2 && strcmp(argv[1], "off") == 0) {
        t.on = false;
    } else {
        for (int i = 1; i + 1 < argc; i += 2) {
            const float v = (float)atof(argv[i + 1]);
            if (strcmp(argv[i], "hpf") == 0) {
                t.hpf_hz = (int)v;
            } else if (strcmp(argv[i], "gain") == 0) {
                t.gain_db = v;
            } else if (strcmp(argv[i], "ceiling") == 0 || strcmp(argv[i], "ceil") == 0) {
                t.ceiling = v;
            } else {
                printf("  unknown setting '%s' -- audio tune help\n", argv[i]);
                return 1;
            }
        }
        if (argc == 2) {
            printf("  audio tune help\n");
            return 1;
        }
    }
    if (argc >= 2) {
        if (mpx_audio_tune_set(&t) != ESP_OK) {
            printf("  out of range -- hpf 0-4000, gain -20..20, ceiling 0.1-1\n");
            return 1;
        }
    }
    audio_tune_print();
    return 0;
}

static int audio_cmd(int argc, char **argv)
{
    if (argc < 2) {
        audio_usage();
        return 0;
    }

    if (strcmp(argv[1], "info") == 0) {
        printf("  microphone : %s\n", mpx_audio_have_mic() ? "present" : "absent");
        printf("  speaker    : %s\n", mpx_audio_have_speaker() ? "present" : "absent");
        printf("  volume     : %d%%\n", mpx_audio_get_volume());
        printf("  mic gain   : %d dB\n", mpx_audio_get_mic_gain());
        audio_tune_print();
        return 0;
    }

    if (strcmp(argv[1], "vol") == 0) {
        if (argc >= 3) {
            mpx_audio_set_volume(atoi(argv[2]));
        }
        printf("  volume %d%%\n", mpx_audio_get_volume());
        return 0;
    }

    if (strcmp(argv[1], "rec") == 0) {
        const uint32_t secs  = (argc >= 3) ? (uint32_t)atoi(argv[2]) : AUDIO_DEFAULT_SECONDS;
        const uint8_t  pick  = (argc >= 4) ? (uint8_t)atoi(argv[3]) : 0;
        const uint8_t  chans = (argc >= 5) ? (uint8_t)atoi(argv[4]) : AUDIO_DEFAULT_CHANNELS;
        const esp_err_t err = mpx_audio_record_wav(AUDIO_DEFAULT_PATH, secs,
                                                   AUDIO_DEFAULT_RATE, chans, pick);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "record failed: %s", esp_err_to_name(err));
            return 1;
        }
        return 0;
    }

    if (strcmp(argv[1], "gain") == 0) {
        if (argc >= 3) {
            mpx_audio_set_mic_gain(atoi(argv[2]));
        }
        printf("  mic gain %d dB\n", mpx_audio_get_mic_gain());
        return 0;
    }

    if (strcmp(argv[1], "tune") == 0) {
        return audio_tune_cmd(argc - 1, argv + 1);
    }

    if (strcmp(argv[1], "tone") == 0) {
        const uint32_t hz   = (argc >= 3) ? (uint32_t)atoi(argv[2]) : 440;
        const uint32_t secs = (argc >= 4) ? (uint32_t)atoi(argv[3]) : 2;
        const esp_err_t err = mpx_audio_play_tone(hz, secs);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "tone failed: %s", esp_err_to_name(err));
            return 1;
        }
        return 0;
    }

    if (strcmp(argv[1], "play") == 0) {
        const char *path = (argc >= 3) ? argv[2] : AUDIO_DEFAULT_PATH;
        const esp_err_t err = mpx_audio_play_wav(path);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "playback failed: %s", esp_err_to_name(err));
            return 1;
        }
        return 0;
    }

    audio_usage();
    return 0;
}

void register_audio_command(void)
{
    const esp_console_cmd_t cmd = {
        .command = "audio",
        .help = "Record and play audio (see `audio` with no arguments)",
        .hint = NULL,
        .func = audio_cmd,
    };
    if (esp_console_cmd_register(&cmd) == ESP_OK) {
        ESP_LOGI(TAG, "'audio' console command registered");
    }
}
