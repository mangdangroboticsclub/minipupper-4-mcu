/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * Music on the robot's speaker: internet radio by genre, or any audio URL.
 *
 * ── Why not ESP-Claw's Lua `audio.player` ─────────────────────────────────
 *
 * On ESP-Claw's own boards the agent plays a song by writing a Lua script:
 * audio.new_output("audio_dac") + audio.player():play(url). On this robot
 * that path cannot work, for two reasons in the hardware:
 *
 *   - The speaker (TX) and the microphones (RX) are the two halves of ONE
 *     I2S peripheral and share its clock. The wake word listens at 16 kHz
 *     all the time; the Lua player opens the output at its own rate
 *     (48 kHz by default) through esp_codec_dev, underneath mpx_audio,
 *     which reclocks the microphone and either kills the wake word or plays
 *     at the wrong speed -- or fails to open at all.
 *   - The amplifier (MAX98357A) has no volume control; esp_codec_dev's
 *     volume is a no-op here. A full-scale mastered song straight into the
 *     1 W speaker is the crackling we already fought for speech.
 *
 * So music goes through mpx_audio, like voice does: decoded by Espressif's
 * esp_audio_simple_player (HTTP/HTTPS, MP3/AAC/M4A/FLAC/WAV), resampled to
 * 16 kHz mono by its own converters (sdkconfig), turned down to the music
 * level, and written with mpx_audio_music_write() -- which runs it through
 * the same speaker tuning as voice (`audio tune`) and yields to voice: a
 * reply or the wake beep always wins the speaker.
 *
 * ── Sharing with the conversation ──────────────────────────────────────────
 *
 *   - Music waits while a conversation turn is going (after the wake word,
 *     while you speak, while the reply plays), then starts. Asking for music
 *     by voice ends that conversation after the reply (no follow-up turn;
 *     mpx_voice_wake's follow-up gate), so the song starts straight away.
 *   - "Hi ESP" during a song: the song holds (a live station stops and
 *     reconnects; a file pauses and resumes) while you talk, then comes back.
 *   - "Stop the music" goes through music_stop. "Be quiet" / "stop talking"
 *     (the gateway's stop phrases, answered with goodbye reason "stop")
 *     stop it too.
 *
 * ── Where the music comes from ─────────────────────────────────────────────
 *
 * SomaFM: listener-supported, commercial-free internet radio with public
 * direct MP3 stream links (somafm.com/<channel>/directstreamlinks.html). A
 * genre picks a station; nothing picks one at random. It is radio, so "a
 * random song" is whatever that station is playing. A specific song needs
 * a direct URL to an audio file -- the agent can find one and pass it.
 */

#include "cap_music.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "esp_audio_simple_player.h"
#include "esp_audio_simple_player_advance.h"
#include "esp_crt_bundle.h"
#include "esp_gmf_io_http.h"
#include "esp_gmf_obj.h"
#include "esp_gmf_pipeline.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "mpx_audio.h"
#include "mpx_voice_link.h"
#include "mpx_voice_wake.h"

static const char *TAG = "cap_music";

#if !defined(CONFIG_AUDIO_SIMPLE_PLAYER_RESAMPLE_DEST_RATE) || \
    CONFIG_AUDIO_SIMPLE_PLAYER_RESAMPLE_DEST_RATE != 16000
#error "cap_music needs CONFIG_AUDIO_SIMPLE_PLAYER_RESAMPLE_DEST_RATE=16000 (the voice rate)"
#endif
#if defined(CONFIG_ESP_AUDIO_SIMPLE_PLAYER_CH_CVT_EN) && CONFIG_AUDIO_SIMPLE_PLAYER_CH_CVT_DEST == 1
#define MUSIC_CHANNELS_FIXED 1
#else
#define MUSIC_CHANNELS_FIXED 0
#endif

#define MUSIC_URL_MAX        300
#define MUSIC_TITLE_MAX      48
#define MUSIC_TICK_MS        100
#define MUSIC_QUIET_MS       700     /* voice idle this long before music (re)starts */
#define MUSIC_RETRIES        3
#define MUSIC_BLOCK          480     /* frames processed per write (30 ms) */
#define MUSIC_LEVEL_DEFAULT  55      /* % of full scale before the volume setting */

/* ── Stations ──────────────────────────────────────────────────────────── */

typedef struct {
    const char *slug;       /* SomaFM channel id */
    const char *title;
    const char *keywords;   /* space separated, lower case */
    bool        in_random;  /* picked for "play some music" */
} station_t;

static const station_t s_stations[] = {
    { "groovesalad",  "Groove Salad",           "chill chillout downtempo relax relaxing electronic beats study", true  },
    { "lush",         "Lush",                   "chill mellow vocal female sensual relax",                         true  },
    { "indiepop",     "Indie Pop Rocks!",       "pop indie rock happy upbeat song songs",                          true  },
    { "poptron",      "PopTron",                "pop electropop synthpop dance",                                   true  },
    { "u80s",         "Underground 80s",        "80s eighties new wave synth retro",                               true  },
    { "seventies",    "Left Coast 70s",         "70s seventies classic rock retro oldies",                         true  },
    { "secretagent",  "Secret Agent",           "lounge spy cool cocktail",                                        true  },
    { "sonicuniverse","Sonic Universe",         "jazz",                                                            false },
    { "illstreet",    "Illinois Street Lounge", "lounge exotica 50s 60s oldies",                                   false },
    { "bootliquor",   "Boot Liquor",            "country americana western",                                       false },
    { "folkfwd",      "Folk Forward",           "folk acoustic singer songwriter",                                 false },
    { "dronezone",    "Drone Zone",             "ambient sleep calm meditation quiet space",                       false },
    { "deepspaceone", "Deep Space One",         "ambient space electronic",                                        false },
    { "beatblender",  "Beat Blender",           "house deep electronic dance party",                               true  },
    { "fluid",        "Fluid",                  "hip hop hiphop rap instrumental beats",                           true  },
    { "7soul",        "Seven Inch Soul",        "soul funk motown rnb r&b",                                        true  },
    { "reggae",       "Heavyweight Reggae",     "reggae ska dub",                                                  false },
    { "metal",        "Metal Detector",         "metal heavy hard rock",                                           false },
    { "covers",       "Covers",                 "covers cover versions songs",                                     true  },
    { "thetrip",      "The Trip",               "trance progressive edm",                                          false },
};
#define STATION_COUNT (sizeof(s_stations) / sizeof(s_stations[0]))

static void station_url(const station_t *st, int server, char *out, size_t n)
{
    /* Two of SomaFM's servers; the second is the retry. */
    snprintf(out, n, "https://ice%d.somafm.com/%s-128-mp3", server ? 5 : 2, st->slug);
}

static int station_find(const char *query)
{
    char q[96];
    int best = -1, best_score = 0;
    size_t i;

    if (!query || !query[0]) {
        return -1;
    }
    for (i = 0; query[i] && i < sizeof(q) - 1; i++) {
        q[i] = (char)tolower((unsigned char)query[i]);
    }
    q[i] = '\0';

    for (size_t k = 0; k < STATION_COUNT; k++) {
        const station_t *st = &s_stations[k];
        char title[MUSIC_TITLE_MAX];
        char words[128];
        int score = 0;
        size_t j;

        for (j = 0; st->title[j] && j < sizeof(title) - 1; j++) {
            title[j] = (char)tolower((unsigned char)st->title[j]);
        }
        title[j] = '\0';
        if (strstr(q, st->slug) || strstr(title, q) || strstr(q, title)) {
            score += 10;
        }
        snprintf(words, sizeof(words), "%s", st->keywords);
        for (char *save = NULL, *w = strtok_r(words, " ", &save); w; w = strtok_r(NULL, " ", &save)) {
            if (strstr(q, w)) {
                score += (int)strlen(w);          /* longer word, surer match */
            }
        }
        if (score > best_score) {
            best_score = score;
            best = (int)k;
        }
    }
    return best;
}

static int station_random(int not_this)
{
    int pool[STATION_COUNT];
    int n = 0;

    for (int k = 0; k < (int)STATION_COUNT; k++) {
        if (s_stations[k].in_random && k != not_this) {
            pool[n++] = k;
        }
    }
    return n ? pool[esp_random() % (uint32_t)n] : 0;
}

/* ── State ─────────────────────────────────────────────────────────────── */

typedef enum { REQ_NONE, REQ_PLAY, REQ_STOP } req_t;

static SemaphoreHandle_t s_lock;          /* guards the request fields */
static TaskHandle_t      s_task;

/* request (written by callers under s_lock, consumed by the task) */
static volatile req_t s_req;
static char           s_req_url[MUSIC_URL_MAX];
static char           s_req_title[MUSIC_TITLE_MAX];
static int            s_req_station = -1;

/* what is wanted / playing (task-owned; read for status) */
static volatile bool  s_want;
static char           s_url[MUSIC_URL_MAX];
static char           s_title[MUSIC_TITLE_MAX];
static int            s_station = -1;
static bool           s_live;
static int            s_server;
static int            s_retries;
static char           s_last_error[80];

static esp_asp_handle_t        s_asp;
static volatile esp_asp_state_t s_state = ESP_ASP_STATE_NONE;
static bool           s_started;           /* run() issued for the current item */
static bool           s_held;              /* held for a conversation */
static bool           s_https;
static volatile bool  s_hold_output;       /* out_cb drops instead of writing */
static int64_t        s_quiet_since;
static int64_t        s_retry_at;          /* a dropped stream reconnects at this time */
static int            s_level = MUSIC_LEVEL_DEFAULT;
static int            s_channels = 2;      /* decoder output when not fixed */

static int16_t       *s_mono;              /* one processed block */

/* ── DSP ───────────────────────────────────────────────────────────────── */

static inline float shape(float x)
{
    /* Only the music level here: the high-pass, the soft ceiling and the
     * volume are mpx_audio's speaker tuning (`audio tune`), shared with
     * voice. Mastered music is far louder than speech, hence the level. */
    return x * (float)s_level / 100.0f;
}

/* ── Player callbacks (player task) ────────────────────────────────────── */

static int music_out_cb(uint8_t *data, int data_size, void *ctx)
{
    (void)ctx;
    const int ch = MUSIC_CHANNELS_FIXED ? 1 : (s_channels > 0 ? s_channels : 2);
    const int16_t *in = (const int16_t *)data;
    size_t frames = (size_t)data_size / (sizeof(int16_t) * (size_t)ch);

    if (!s_want || s_hold_output || !s_mono) {
        return data_size;               /* dropped: stopping, or voice has the speaker */
    }
    while (frames > 0) {
        const size_t n = frames > MUSIC_BLOCK ? MUSIC_BLOCK : frames;
        for (size_t i = 0; i < n; i++) {
            float x = 0.0f;
            for (int c = 0; c < ch; c++) {
                x += (float)in[i * ch + c];
            }
            x = shape(x / ((float)ch * 32768.0f));
            s_mono[i] = (int16_t)lrintf(x * 32767.0f);
        }
        if (mpx_audio_music_write(s_mono, n) != ESP_OK) {
            return data_size;           /* voice took the speaker mid-block */
        }
        in += n * (size_t)ch;
        frames -= n;
    }
    return data_size;
}

static int music_event_cb(esp_asp_event_pkt_t *pkt, void *ctx)
{
    (void)ctx;
    if (!pkt) {
        return 0;
    }
    if (pkt->type == ESP_ASP_EVENT_TYPE_STATE && pkt->payload_size == sizeof(esp_asp_state_t)) {
        s_state = *(esp_asp_state_t *)pkt->payload;
        ESP_LOGI(TAG, "player: %s", esp_audio_simple_player_state_to_str(s_state));
    } else if (pkt->type == ESP_ASP_EVENT_TYPE_MUSIC_INFO &&
               pkt->payload_size == sizeof(esp_asp_music_info_t)) {
        const esp_asp_music_info_t *mi = (const esp_asp_music_info_t *)pkt->payload;
        if (!MUSIC_CHANNELS_FIXED && mi->channels >= 1 && mi->channels <= 2) {
            s_channels = mi->channels;
        }
        ESP_LOGI(TAG, "stream: %d Hz, %d ch, %d bit, %d kbps", mi->sample_rate,
                 mi->channels, mi->bits, mi->bitrate / 1000);
    }
    return 0;
}

/* HTTPS: give the HTTP reader the certificate bundle before the pipeline
 * opens (the same thing ESP-Claw's Lua player does). */
static int music_prev_cb(esp_asp_handle_t *handle, void *ctx)
{
    (void)ctx;
    esp_gmf_pipeline_handle_t pipe = NULL;
    esp_gmf_io_handle_t in_io = NULL;

    if (!s_https) {
        return ESP_GMF_ERR_OK;
    }
    if (esp_audio_simple_player_get_pipeline((esp_asp_handle_t)handle, &pipe) != ESP_GMF_ERR_OK || !pipe ||
            esp_gmf_pipeline_get_in(pipe, &in_io) != ESP_GMF_ERR_OK || !in_io ||
            strcmp(OBJ_GET_TAG(in_io), "io_http") != 0) {
        return ESP_GMF_ERR_FAIL;
    }
    http_io_cfg_t *http_cfg = (http_io_cfg_t *)OBJ_GET_CFG(in_io);
    if (!http_cfg) {
        return ESP_GMF_ERR_FAIL;
    }
    http_cfg->crt_bundle_attach = esp_crt_bundle_attach;
    return ESP_GMF_ERR_OK;
}

/* ── The player task ───────────────────────────────────────────────────── */

static bool voice_busy(void)
{
    return mpx_voice_wake_conversing() || mpx_voice_talk_active() ||
           mpx_voice_stream_is_playing() || mpx_audio_voice_output_active();
}

static bool player_create(void)
{
    if (s_asp) {
        return true;
    }
    if (!s_mono) {
        s_mono = heap_caps_malloc(MUSIC_BLOCK * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_mono) {
            s_mono = malloc(MUSIC_BLOCK * sizeof(int16_t));
        }
        if (!s_mono) {
            return false;
        }
    }
    esp_asp_cfg_t cfg = {
        .out = { .cb = music_out_cb, .user_ctx = NULL },
        .task_prio = 5,
        .task_stack = 6 * 1024,
        .task_core = 1,
        .task_stack_in_ext = true,      /* internal RAM is the scarce thing */
        .prev = music_prev_cb,
        .prev_ctx = NULL,
    };
    const size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (esp_audio_simple_player_new(&cfg, &s_asp) != ESP_GMF_ERR_OK) {
        s_asp = NULL;
        snprintf(s_last_error, sizeof(s_last_error), "player create failed (out of memory?)");
        return false;
    }
    esp_audio_simple_player_set_event(s_asp, music_event_cb, NULL);
    ESP_LOGI(TAG, "player up: internal RAM %u -> %u free", (unsigned)before,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return true;
}

static void player_destroy(void)
{
    if (s_asp) {
        s_hold_output = true;
        (void)esp_audio_simple_player_stop(s_asp);
        (void)esp_audio_simple_player_destroy(s_asp);
        s_asp = NULL;
        s_hold_output = false;
    }
    mpx_audio_music_end();
    free(s_mono);
    s_mono = NULL;
    s_state = ESP_ASP_STATE_NONE;
}

static bool player_run(void)
{
    if (!player_create()) {
        s_started = true;
        s_state = ESP_ASP_STATE_ERROR;
        return false;
    }
    s_https = strncasecmp(s_url, "https://", 8) == 0;
    s_state = ESP_ASP_STATE_NONE;
    s_hold_output = false;
    s_started = true;       /* from here on a failure is the retry logic's */
    ESP_LOGI(TAG, "playing %s (%s)", s_title, s_url);
    if (esp_audio_simple_player_run(s_asp, s_url, NULL) != ESP_GMF_ERR_OK) {
        snprintf(s_last_error, sizeof(s_last_error), "could not open %.60s", s_url);
        ESP_LOGW(TAG, "%s", s_last_error);
        s_state = ESP_ASP_STATE_ERROR;
        return false;
    }
    return true;
}

static void player_halt(void)
{
    if (s_asp && s_started) {
        s_hold_output = true;
        (void)esp_audio_simple_player_stop(s_asp);
        s_hold_output = false;
    }
    s_started = false;
}

static void take_request(void)
{
    req_t req;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    req = s_req;
    s_req = REQ_NONE;
    if (req == REQ_PLAY) {
        snprintf(s_url, sizeof(s_url), "%s", s_req_url);
        snprintf(s_title, sizeof(s_title), "%s", s_req_title);
        s_station = s_req_station;
    }
    xSemaphoreGive(s_lock);

    if (req == REQ_STOP) {
        player_halt();
        player_destroy();
        s_want = false;
        s_held = false;
        ESP_LOGI(TAG, "stopped");
    } else if (req == REQ_PLAY) {
        player_halt();
        s_live = s_station >= 0 ||
                 !(strstr(s_url, ".mp3") || strstr(s_url, ".m4a") || strstr(s_url, ".aac") ||
                   strstr(s_url, ".flac") || strstr(s_url, ".wav"));
        s_server = 0;
        s_retries = 0;
        s_retry_at = 0;
        s_held = false;
        s_last_error[0] = '\0';
        s_want = true;
    }
}

static void music_task(void *arg)
{
    (void)arg;

    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(s_want ? MUSIC_TICK_MS : 2000));
        take_request();
        if (!s_want) {
            continue;
        }

        const int64_t now = esp_timer_get_time();
        const bool busy = voice_busy();
        if (busy) {
            s_quiet_since = 0;
        } else if (!s_quiet_since) {
            s_quiet_since = now;
        }
        const bool quiet = !busy && now - s_quiet_since >= (int64_t)MUSIC_QUIET_MS * 1000;

        /* A conversation: hold. A live station is stopped (it would only
         * pile up behind us and get cut off by the server); a file pauses. */
        if (busy) {
            if (s_started && !s_held) {
                s_hold_output = true;
                if (s_live) {
                    player_halt();
                } else {
                    (void)esp_audio_simple_player_pause(s_asp);
                }
                s_held = true;
                ESP_LOGI(TAG, "holding for the conversation");
            }
            continue;
        }
        if (!quiet) {
            continue;
        }

        if (s_held) {
            s_held = false;
            if (s_live || !s_started) {
                (void)player_run();
            } else {
                s_hold_output = false;
                (void)esp_audio_simple_player_resume(s_asp);
            }
            ESP_LOGI(TAG, "back to the music");
            continue;
        }
        if (s_retry_at) {
            if (now >= s_retry_at) {
                s_retry_at = 0;
                (void)player_run();
            }
            continue;
        }
        if (!s_started) {
            (void)player_run();
            continue;
        }

        if (s_state == ESP_ASP_STATE_FINISHED && !s_live) {
            ESP_LOGI(TAG, "finished: %s", s_title);
            player_destroy();
            s_started = false;
            s_want = false;
            continue;
        }
        if (s_state == ESP_ASP_STATE_ERROR || (s_state == ESP_ASP_STATE_FINISHED && s_live)) {
            if (s_retries >= MUSIC_RETRIES) {
                if (!s_last_error[0]) {
                    snprintf(s_last_error, sizeof(s_last_error), "the stream stopped working");
                }
                ESP_LOGW(TAG, "giving up on %s: %s", s_title, s_last_error);
                player_destroy();
                s_started = false;
                s_want = false;
                continue;
            }
            s_retries++;
            player_halt();
            s_state = ESP_ASP_STATE_NONE;
            if (s_station >= 0) {
                s_server ^= 1;          /* the other SomaFM server */
                station_url(&s_stations[s_station], s_server, s_url, sizeof(s_url));
            }
            s_retry_at = now + 2000000;
            ESP_LOGW(TAG, "stream dropped -- retry %d/%d in 2 s", s_retries, MUSIC_RETRIES);
        } else if (s_state == ESP_ASP_STATE_RUNNING) {
            s_retries = 0;
        }
    }
}

/* ── Public API ────────────────────────────────────────────────────────── */

static esp_err_t ensure_task(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_task) {
        /* PSRAM stack: it only builds and drives the player (heap and
         * network, never flash), and internal RAM is scarce. */
        if (xTaskCreatePinnedToCoreWithCaps(music_task, "music", 6144, NULL, 4, &s_task, 1,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            s_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static void level_load(void)
{
    nvs_handle_t h;
    uint8_t v;
    if (nvs_open("music", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "level", &v) == ESP_OK && v >= 5 && v <= 100) {
            s_level = v;
        }
        nvs_close(h);
    }
}

esp_err_t cap_music_play(const char *genre_or_url, char *what, size_t what_size)
{
    char url[MUSIC_URL_MAX];
    char title[MUSIC_TITLE_MAX];
    int station = -1;
    esp_err_t err = ensure_task();

    if (err != ESP_OK) {
        return err;
    }
    if (!mpx_audio_have_speaker()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (genre_or_url && (strncasecmp(genre_or_url, "http://", 7) == 0 ||
                         strncasecmp(genre_or_url, "https://", 8) == 0)) {
        if (strlen(genre_or_url) >= sizeof(url)) {
            return ESP_ERR_INVALID_SIZE;
        }
        snprintf(url, sizeof(url), "%s", genre_or_url);
        {
            const char *slash = strrchr(url, '/');
            strlcpy(title, slash && slash[1] ? slash + 1 : url, sizeof(title));
            for (char *c = title; *c; c++) {
                if (*c == '"' || *c == '\\' || (unsigned char)*c < 0x20) {
                    *c = '_';               /* it goes into JSON replies */
                }
            }
        }
    } else {
        station = station_find(genre_or_url);
        if (station < 0) {
            station = station_random(s_want ? s_station : -1);
        }
        station_url(&s_stations[station], 0, url, sizeof(url));
        snprintf(title, sizeof(title), "%s", s_stations[station].title);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_req_url, sizeof(s_req_url), "%s", url);
    snprintf(s_req_title, sizeof(s_req_title), "%s", title);
    s_req_station = station;
    s_req = REQ_PLAY;
    xSemaphoreGive(s_lock);
    s_want = true;                      /* so the follow-up gate sees it now */
    xTaskNotifyGive(s_task);

    if (what && what_size) {
        snprintf(what, what_size, "%s", title);
    }
    return ESP_OK;
}

void cap_music_stop(void)
{
    if (ensure_task() != ESP_OK) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_req = REQ_STOP;
    xSemaphoreGive(s_lock);
    s_want = false;
    xTaskNotifyGive(s_task);
}

bool cap_music_active(void)
{
    return s_want;
}

size_t cap_music_describe(char *out, size_t out_size)
{
    const char *state = !s_want ? "off"
                        : s_held ? "held for a conversation"
                        : s_state == ESP_ASP_STATE_RUNNING ? "playing"
                        : s_state == ESP_ASP_STATE_PAUSED ? "paused"
                        : "starting";
    return (size_t)snprintf(out, out_size, "music %s%s%s, level %d%%%s%s", state,
                            s_want ? ": " : "", s_want ? s_title : "", s_level,
                            s_last_error[0] ? ", last problem: " : "", s_last_error);
}

void cap_music_set_level(int percent)
{
    nvs_handle_t h;
    s_level = percent < 5 ? 5 : percent > 100 ? 100 : percent;
    if (nvs_open("music", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "level", (uint8_t)s_level);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* A song is waiting for the speaker: tell the hands-free loop to skip the
 * follow-up turn after this reply. */
static bool skip_followup(void)
{
    return s_want;
}

int cap_music_level(void)
{
    return s_level;
}

bool cap_music_matches_station(const char *genre)
{
    return station_find(genre) >= 0;
}

int cap_music_station_count(void)
{
    return (int)STATION_COUNT;
}

const char *cap_music_station_title(int index)
{
    return (index >= 0 && index < (int)STATION_COUNT) ? s_stations[index].title : NULL;
}

const char *cap_music_station_keywords(int index)
{
    return (index >= 0 && index < (int)STATION_COUNT) ? s_stations[index].keywords : NULL;
}

const char *cap_music_title(void)
{
    return s_want ? s_title : "";
}

/* ── Setup ─────────────────────────────────────────────────────────────── */

void cap_music_init(void)
{
    level_load();
    (void)ensure_task();
    mpx_voice_wake_set_followup_gate(skip_followup);
    mpx_voice_set_quiet_hook(cap_music_stop);       /* "be quiet" means the music too */
}

