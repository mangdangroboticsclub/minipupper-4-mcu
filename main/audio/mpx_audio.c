/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 */

/* mpx_audio.c -- the microphone (ES7210) and the speaker (MAX98357A).
 *
 * mpv2-claw's components/mpx_audio/src/mpx_audio.c. The one structural
 * difference: mpv2-claw got its I2S channels and the ES7210 codec device from
 * ESP-Claw's board manager (board_peripherals.yaml / board_devices.yaml). This
 * firmware has no board manager, so audio_hw_init() below creates the same
 * objects by hand from the pins in robot/board_config.h -- a full-duplex I2S0
 * channel pair and an esp_codec_dev ES7210 on the shared I2C bus. Everything
 * after that is the same code, which is the point: the comments in it record
 * a lot of hard-won hardware behaviour. */

#include "audio/mpx_audio.h"
#include "audio/mpx_audio_tune.h"

#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "mpx_audio";

/* The whole implementation is behind this. With CONFIG_APP_AUDIO_ENABLE off
 * the stubs at the bottom of this file stand in and every entry point
 * answers ESP_ERR_NOT_SUPPORTED. */
#if CONFIG_APP_AUDIO_ENABLE

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "platform/shared_i2c.h"
#include "robot/board_config.h"

/* The writable root this firmware mounts (fs/littlefs_manager.cc). */
#define AUDIO_DATA_ROOT "/fs"

/* What mpv2-claw's board manager handed out for the "audio_adc" device:
 * the esp_codec_dev handle and the interfaces behind it. */
typedef struct {
    const audio_codec_data_if_t *data_if;
    const audio_codec_ctrl_if_t *ctrl_if;
    const audio_codec_if_t      *codec_if;
    esp_codec_dev_handle_t       codec_dev;
} dev_audio_codec_handles_t;

/* 4 KB of 16-bit frames. Big enough that the per-read overhead disappears,
 * small enough to sit in PSRAM without anyone noticing. */
#define AUDIO_CHUNK_BYTES   4096

/* claw_paths.h publishes no maximum, so pick one. Deep enough for the data
 * root plus a couple of directories, short enough that the buffer is not a
 * meaningful stack cost. */
#define AUDIO_PATH_MAX      192

static dev_audio_codec_handles_t *s_mic;
static i2s_chan_handle_t s_tx;
/* Software output volume, percent. 80 by default. The MAX98357A has no volume
 * control of its own and, at its default 9 dB gain on 5 V, pushes about
 * 1.4 W into 8 ohms at full scale -- more than the 1 W speaker this robot
 * ships with can take, which is what crackles. 80% keeps full-scale peaks
 * under ~1 W; the voice server shapes the speech for the same speaker.
 * Saved in NVS under the namespace and key mpv2 used ("voice" / "vol"), so a
 * level set on mpv2 carries over and `audio vol N` survives a reboot. */
#define MPX_AUDIO_VOLUME_DEFAULT 80
#define MPX_AUDIO_NVS_NS         "voice"
#define MPX_AUDIO_NVS_KEY        "vol"
static int s_volume = MPX_AUDIO_VOLUME_DEFAULT;

/* Microphone gain in dB, applied AFTER the codec is opened.
 *
 * 30 dB is what the reference firmware for this board uses
 * (AUDIO_CODEC_DEFAULT_MIC_GAIN in SantaTest). Without it the ES7210 runs at
 * its reset default and a normal speaking voice records at a peak of a couple
 * of thousand out of 32767 -- present, but far too quiet to hear on playback,
 * which is easy to mistake for a broken speaker.
 *
 * `adc_init_gain: 30` in board_devices.yaml does NOT do this. That value
 * lands in the board manager's own config struct; esp_codec_dev never reads
 * it, and nothing logs that the gain was left alone.
 *
 * 24 dB now, not 30: on the MP4 ESP32 CORE board, 30 dB clipped a normal
 * speaking voice at half a metre (`audio rec` reported peak 32767 with an
 * RMS of 4767, i.e. -17 dBFS with the tops sheared off). Clipped speech
 * plays back fine to a human ear and is poison to a wake-word model.
 * 24 dB keeps the same voice a few dB under full scale; `audio gain` moves
 * it at runtime. */
static float s_mic_gain_db = 24.0f;

/* ── Output goes straight to I2S, with no codec in between ─────────────────
 *
 * The MAX98357A has no control interface: no I2C, no registers, nothing to
 * configure. esp_codec_dev's "dummy" codec exists to represent exactly that,
 * and going through it produced a TX path that clocked data out for the right
 * length of time and made no sound, while logging
 *
 *     i2s_channel_disable(): the channel has not been enabled yet
 *
 * on every open -- the channel's enable state was not what the driver
 * believed. Rather than keep guessing at that, this follows the firmware
 * already proven on this board (SantaTest's santa_audio_codec.cc), which
 * creates no output codec device at all and calls i2s_channel_write()
 * directly. There is genuinely nothing for a codec layer to do here.
 *
 * The board manager hands out the TX channel handle already enabled, so this
 * only reconfigures the sample rate when it differs from the board default. */
/* The speaker and the microphone are the TX and RX halves of ONE I2S
 * peripheral (board_peripherals.yaml: i2s_audio_in reuses i2s_audio_out's
 * config, "i2s0 works in duplex mode"), and in full duplex the TX side is
 * the clock master. esp_codec_dev knows this -- it enables TX itself when
 * the microphone is opened alone, and refuses to disable TX while RX runs
 * ("Pending out channel for in channel running") -- but this file drives
 * TX directly, underneath esp_codec_dev, so it has to keep the same rule
 * by hand:
 *
 *   never stop or reconfigure the TX channel while a capture is open.
 *
 * Stopping it silently stops the microphone's clock: the next
 * esp_codec_dev_read() times out with rc=-1 and whatever was reading --
 * the wake-word feed -- dies. First seen as `audio tone` killing the
 * hands-free listener; it would have hit at the end of every reply.
 *
 * Two pieces of state make that possible. s_out_cfg_rate is the clock/slot
 * configuration the channel currently carries, which survives disable/
 * enable, so an open at the same rate never reconfigures. s_out_running is
 * whether the channel is enabled, as far as this file knows -- esp_codec_dev
 * may also have enabled it for the microphone's sake, which is why an
 * "already enabled" error on enable is treated as success. */
static uint32_t s_out_cfg_rate;
static bool     s_out_running;
static bool     s_cap_open;   /* defined here so the output side can see it */

/* Width of the TX channel's I2S slots RIGHT NOW. 16 when this file
 * configured the channel. But when a capture opens with more than two
 * channels, esp_codec_dev implements it as two 32-bit slots -- the ES7210
 * packs its four 16-bit ADC words into a 64-bit frame -- and, because the
 * two directions share one clock, it reconfigures the paired TX channel to
 * 32-bit slots as well ("Peer mode playback need extend bits 32 to 64").
 * Every 16-bit stereo frame this file writes then has to be widened to two
 * 32-bit words or the amplifier plays it at the wrong width: heard as a
 * garbled, muffled reply during hands-free (mic open, 4 channels) that was
 * perfectly clear after `voice talk` (mic closed, 2 channels). */
static uint8_t  s_out_slot_bits = 16;
static int32_t *s_out_wide;   /* conversion buffer for the 32-bit case */
#define AUDIO_OUT_WIDE_FRAMES  512
static void *audio_alloc(size_t size);   /* defined with the streaming code below */

#define AUDIO_OUT_PARK_RATE    16000u   /* what voice runs at, both directions */
/* Roughly the driver's DMA ring at 16 kHz stereo 16-bit, with margin: the
 * last thing written keeps circulating on a channel that stays enabled, so
 * the last thing written had better be silence. */
#define AUDIO_OUT_FLUSH_BYTES  (16u * 1024u)   /* > 6 x 240 frames x 8 B at 32-bit slots */

static esp_err_t audio_out_configure(uint32_t rate)
{
    /* Reconfiguring requires the channel to be stopped. It may already be
     * stopped, and that is not an error worth reporting. */
    (void)i2s_channel_disable(s_tx);
    s_out_running = false;
    s_out_cfg_rate = 0;

    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    clk.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(s_tx, &clk), TAG,
                        "cannot set %" PRIu32 " Hz", rate);

    /* Philips framing, 16-bit, both slots -- what the amplifier expects and
     * what the working firmware sends. */
    i2s_std_slot_config_t slot =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                            I2S_SLOT_MODE_STEREO);
    ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_slot(s_tx, &slot), TAG,
                        "cannot set slot format");
    s_out_cfg_rate = rate;
    s_out_slot_bits = 16;

    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "cannot enable TX");
    s_out_running = true;
    return ESP_OK;
}

static esp_err_t audio_out_open(uint32_t rate)
{
    if (!s_tx) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (s_out_cfg_rate != rate) {
        if (s_cap_open) {
            /* Only a WAV at an odd rate gets here; voice is all 16 kHz. */
            ESP_LOGW(TAG, "reconfiguring the shared I2S clock %" PRIu32 " -> %" PRIu32
                          " Hz while the microphone is open -- expect a capture hiccup",
                     s_out_cfg_rate, rate);
        }
        return audio_out_configure(rate);
    }

    if (!s_out_running) {
        const esp_err_t err = i2s_channel_enable(s_tx);
        /* ESP_ERR_INVALID_STATE: already enabled -- esp_codec_dev did it
         * when the microphone opened. That is the state we want. */
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
        s_out_running = true;
    }
    return ESP_OK;
}

static esp_err_t audio_out_write(const void *buf, size_t bytes)
{
    size_t written = 0;
    return i2s_channel_write(s_tx, buf, bytes, &written, portMAX_DELAY);
}

/* Every producer in this file makes 16-bit interleaved stereo. This is the
 * one place that knows what the channel currently expects (see
 * s_out_slot_bits) and widens the frames when it has to. */
static esp_err_t audio_out_write_stereo16(const int16_t *stereo, size_t frames)
{
    if (s_out_slot_bits == 16) {
        return audio_out_write(stereo, frames * 2u * sizeof(int16_t));
    }
    if (!s_out_wide) {
        s_out_wide = audio_alloc(AUDIO_OUT_WIDE_FRAMES * 2u * sizeof(int32_t));
        if (!s_out_wide) {
            return ESP_ERR_NO_MEM;
        }
    }
    for (size_t done = 0; done < frames;) {
        const size_t n = (frames - done) > AUDIO_OUT_WIDE_FRAMES ? AUDIO_OUT_WIDE_FRAMES : (frames - done);
        /* Interleaved stereo: frame `done` starts at sample `done * 2`.
         * (Indexing this as `done + i` once re-read the middle of every 60 ms
         * reply frame in place of its end -- heard as garbled-but-legible
         * speech, only ever in hands-free mode, where the slots are wide.) */
        const int16_t *src = stereo + done * 2u;
        for (size_t i = 0; i < n * 2u; i++) {
            /* The sample in the top 16 bits of the 32-bit slot: what a
             * 16-bit DAC word looks like to an amplifier reading MSB first. */
            s_out_wide[i] = (int32_t)src[i] << 16;
        }
        ESP_RETURN_ON_ERROR(audio_out_write(s_out_wide, n * 2u * sizeof(int32_t)), TAG, "i2s write");
        done += n;
    }
    return ESP_OK;
}

static void audio_out_close(void)
{
    if (!s_tx) {
        return;
    }
    /* ALWAYS leave silence in the DMA ring, whether or not the channel is
     * about to be stopped. The I2S driver does not clear its buffers on
     * disable, reconfigure or enable, so whatever was written last -- the
     * tail of the beep, the end of a reply -- is still there the next time
     * the channel runs, and the channel is run behind this file's back:
     * esp_codec_dev enables TX whenever the microphone opens, at whatever
     * slot width the capture needs. A 16-bit beep tail looping through
     * 32-bit slots while the wake-word front end listens is the burst of
     * hash heard at the start of every hands-free reply. */
    static const uint8_t zeros[1024] = {0};
    for (size_t left = AUDIO_OUT_FLUSH_BYTES; left > 0;) {
        const size_t n = left < sizeof(zeros) ? left : sizeof(zeros);
        if (audio_out_write(zeros, n) != ESP_OK) {
            break;
        }
        left -= n;
    }
    if (s_cap_open) {
        /* The microphone needs this clock: leave the channel running.
         * capture_stop() finishes the shutdown once nothing needs it. */
        return;
    }
    (void)i2s_channel_disable(s_tx);
    s_out_running = false;
    if (s_out_slot_bits != 16) {
        s_out_cfg_rate = 0;   /* a capture widened the slots; reconfigure on the next open */
    }
}

/* ── WAV ───────────────────────────────────────────────────────────────────
 *
 * Written by hand rather than pulled from a library: it is 44 bytes, it is
 * the only container this layer needs, and a dependency here would be a
 * dependency in every later phase too. */
#pragma pack(push, 1)
typedef struct {
    char     riff[4];
    uint32_t file_size;        /* everything after this field */
    char     wave[4];
    char     fmt[4];
    uint32_t fmt_size;
    uint16_t format;           /* 1 = PCM */
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char     data[4];
    uint32_t data_size;
} wav_header_t;
#pragma pack(pop)

static void wav_header_fill(wav_header_t *h, uint32_t rate, uint16_t channels,
                            uint32_t data_bytes)
{
    memcpy(h->riff, "RIFF", 4);
    memcpy(h->wave, "WAVE", 4);
    memcpy(h->fmt,  "fmt ", 4);
    memcpy(h->data, "data", 4);
    h->fmt_size        = 16;
    h->format          = 1;
    h->channels        = channels;
    h->sample_rate     = rate;
    h->bits_per_sample = 16;
    h->block_align     = (uint16_t)(channels * 2);
    h->byte_rate       = rate * h->block_align;
    h->data_size       = data_bytes;
    h->file_size       = data_bytes + sizeof(wav_header_t) - 8;
}

/* ── helpers ─────────────────────────────────────────────────────────────── */

/* Resolve a data-root-relative path and create its parent directories.
 *
 * mkdir() per component rather than a recursive helper, because FATFS has no
 * mkdir -p and the paths here are two levels deep at most. EEXIST is success. */
static esp_err_t audio_prepare_path(const char *rel, char *out, size_t out_size)
{
    ESP_RETURN_ON_FALSE(rel && rel[0], ESP_ERR_INVALID_ARG, TAG, "empty path");
    ESP_RETURN_ON_FALSE(!strstr(rel, ".."), ESP_ERR_INVALID_ARG, TAG, "path escapes root");
    {
        const int n = snprintf(out, out_size, "%s/%s", AUDIO_DATA_ROOT,
                               rel[0] == '/' ? rel + 1 : rel);
        ESP_RETURN_ON_FALSE(n > 0 && (size_t)n < out_size, ESP_ERR_INVALID_SIZE,
                            TAG, "path too long");
    }

    /* Skip the mount point itself: mkdir("/fs") is not ours to do. */
    for (char *slash = strchr(out + sizeof(AUDIO_DATA_ROOT), '/'); slash;
         slash = strchr(slash + 1, '/')) {
        *slash = '\0';
        if (mkdir(out, 0777) != 0 && errno != EEXIST) {
            ESP_LOGW(TAG, "mkdir %s failed: %s", out, strerror(errno));
        }
        *slash = '/';
    }
    return ESP_OK;
}

/* Integer square root, for the RMS report. sqrt() from libm would work, but
 * it drags float formatting into a logging path that runs on the console
 * task, and this is exact enough for a level meter. */
static uint32_t isqrt64(uint64_t value)
{
    uint64_t root = 0;
    uint64_t bit = 1ULL << 62;

    while (bit > value) {
        bit >>= 2;
    }
    while (bit) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)root;
}

static void *audio_alloc(size_t size)
{
    /* PSRAM first: these buffers are large and nothing DMAs straight out of
     * them -- esp_codec_dev copies through the I2S driver either way -- and
     * internal RAM is the scarce resource on this firmware. */
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(size, MALLOC_CAP_DEFAULT);
}

/* ── init ────────────────────────────────────────────────────────────────── */

static void volume_load(void)
{
    nvs_handle_t h;
    int32_t v = 0;
    if (nvs_open(MPX_AUDIO_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    if (nvs_get_i32(h, MPX_AUDIO_NVS_KEY, &v) == ESP_OK && v >= 0 && v <= 100) {
        s_volume = (int)v;
    }
    nvs_close(h);
}

static void volume_save(void)
{
    nvs_handle_t h;
    if (nvs_open(MPX_AUDIO_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "volume not saved: NVS unavailable");
        return;
    }
    nvs_set_i32(h, MPX_AUDIO_NVS_KEY, s_volume);
    nvs_commit(h);
    nvs_close(h);
}

/* ── hardware bring-up (what the board manager did for mpv2-claw) ──────────
 *
 * One I2S0 channel pair in full duplex: TX to the MAX98357A, RX from the
 * ES7210, sharing MCLK/BCLK/WS. Philips 16-bit stereo at the voice rate;
 * esp_codec_dev reconfigures both halves when a capture opens with another
 * layout (four channels -> two 32-bit slots), and the TX-side bookkeeping
 * above follows it. The ES7210 is an esp_codec_dev ADC-only device driven
 * over the shared I2C bus (hal_shared_i2c_bus(), the one the BMI270 is on). */
static dev_audio_codec_handles_t s_mic_handles;
static i2s_chan_handle_t         s_rx;

static esp_err_t audio_hw_init_i2s(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, &s_rx), TAG, "i2s_new_channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_OUT_PARK_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = AUDIO_I2S_MCLK_PIN,
            .bclk = AUDIO_I2S_BCLK_PIN,
            .ws   = AUDIO_I2S_WS_PIN,
            .dout = AUDIO_I2S_DOUT_PIN,
            .din  = AUDIO_I2S_DIN_PIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "init TX");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG, "init RX");
    return ESP_OK;
}

static esp_err_t audio_hw_init_codec(void)
{
    dev_audio_codec_handles_t *h = &s_mic_handles;

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port      = AUDIO_I2S_PORT,
        .rx_handle = s_rx,
        .tx_handle = s_tx,     /* the clock master: esp_codec_dev needs it for RX */
    };
    h->data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(h->data_if, ESP_FAIL, TAG, "i2s data interface");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port           = 0,
        .addr           = AUDIO_ES7210_ADDR_8BIT,
        .bus_handle     = hal_shared_i2c_bus(),
        .clock_speed_hz = 400000,
    };
    ESP_RETURN_ON_FALSE(i2c_cfg.bus_handle, ESP_FAIL, TAG, "shared I2C bus unavailable");
    h->ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(h->ctrl_if, ESP_FAIL, TAG, "i2c ctrl interface");

    /* MIC1, MIC2 and MIC3 (the echo reference): the esp_box_3 mask, as
     * mpv2-claw's board_devices.yaml declares it ("0111"). */
    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if      = h->ctrl_if,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3,
    };
    h->codec_if = es7210_codec_new(&es7210_cfg);
    ESP_RETURN_ON_FALSE(h->codec_if, ESP_FAIL, TAG, "ES7210 did not answer at 0x%02x",
                        AUDIO_ES7210_ADDR_8BIT);

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = h->codec_if,
        .data_if  = h->data_if,
    };
    h->codec_dev = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(h->codec_dev, ESP_FAIL, TAG, "esp_codec_dev_new");
    s_mic = h;
    return ESP_OK;
}

esp_err_t mpx_audio_init(void)
{
    volume_load();

    if (audio_hw_init_i2s() != ESP_OK) {
        ESP_LOGE(TAG, "I2S%d would not come up -- no microphone, no speaker",
                 (int)AUDIO_I2S_PORT);
        s_tx = NULL;
        s_rx = NULL;
    } else if (audio_hw_init_codec() != ESP_OK) {
        /* The speaker needs only the TX channel; keep it. */
        ESP_LOGW(TAG, "ES7210 setup failed -- speaker only");
    }

    ESP_LOGI(TAG, "microphone %s, speaker %s (volume %d%%)",
             mpx_audio_have_mic() ? "ready" : "absent",
             mpx_audio_have_speaker() ? "ready" : "absent", s_volume);

    /* Configure TX for the voice rate now, while nothing is capturing, so
     * the first reply that arrives while the wake-word front end holds the
     * microphone finds the clock already right and never has to stop it
     * (see the comment above audio_out_configure()). Left running:
     * esp_codec_dev bounces it when the microphone opens either way, and a
     * running channel spares the driver's "has not been enabled yet"
     * complaint when it does. */
    if (s_tx) {
        (void)audio_out_configure(AUDIO_OUT_PARK_RATE);
    }
    return ESP_OK;
}

bool mpx_audio_have_mic(void)     { return s_mic && s_mic->codec_dev; }
bool mpx_audio_have_speaker(void) { return s_tx != NULL; }

void mpx_audio_set_volume(int percent)
{
    const int v = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    if (v == s_volume) {
        return;
    }
    s_volume = v;
    /* Not on the audio path: called from the console and settings, on a task
     * with an internal-RAM stack (NVS writes must not run on a PSRAM stack). */
    volume_save();
}

void mpx_audio_set_mic_gain(int db)
{
    s_mic_gain_db = (float)(db < 0 ? 0 : (db > 60 ? 60 : db));
}

int mpx_audio_get_mic_gain(void) { return (int)s_mic_gain_db; }

int mpx_audio_get_volume(void) { return s_volume; }

void mpx_audio_set_volume_transient(int percent)
{
    s_volume = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
}

/* ── streaming ─────────────────────────────────────────────────────────────
 *
 * Same codec, same I2S channel as the file calls, but held open across calls
 * so a caller can push frames through Opus and a socket without paying an
 * open/close per block. */

static uint8_t  s_cap_channels = 1;
static uint8_t  s_cap_pick;
static int16_t *s_cap_scratch;      /* interleaved, straight off the codec */
static int16_t *s_out_scratch;      /* stereo, after volume */

esp_err_t mpx_audio_capture_start(uint32_t sample_rate, uint8_t channels, uint8_t pick)
{
    int rc;

    ESP_RETURN_ON_FALSE(mpx_audio_have_mic(), ESP_ERR_NOT_SUPPORTED, TAG,
                        "no microphone on this board");
    ESP_RETURN_ON_FALSE(channels >= 1 && channels <= 4 && pick < channels,
                        ESP_ERR_INVALID_ARG, TAG, "bad channel selection");
    if (s_cap_open) {
        return ESP_OK;
    }

    /* Non-zero mask, for the reason spelled out in mpx_audio_record_wav. */
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = channels,
        .channel_mask    = (uint16_t)((1u << channels) - 1u),
        .sample_rate     = sample_rate,
        .mclk_multiple   = 0,
    };

    rc = esp_codec_dev_open(s_mic->codec_dev, &fs);
    ESP_RETURN_ON_FALSE(rc == 0, ESP_FAIL, TAG, "codec open failed: rc=%d", rc);

    if (esp_codec_dev_set_in_channel_gain(s_mic->codec_dev, fs.channel_mask,
                                          s_mic_gain_db) != 0) {
        (void)esp_codec_dev_set_in_gain(s_mic->codec_dev, s_mic_gain_db);
    }

    s_cap_scratch = audio_alloc(MPX_AUDIO_MAX_FRAMES * channels * sizeof(int16_t));
    if (!s_cap_scratch) {
        esp_codec_dev_close(s_mic->codec_dev);
        return ESP_ERR_NO_MEM;
    }

    s_cap_channels = channels;
    s_cap_pick = pick;
    s_cap_open = true;

    /* esp_codec_dev has just reconfigured and enabled the paired TX channel
     * to match this capture (see s_out_slot_bits): the clock is now this
     * capture's rate, the slots are 32-bit when there are more than two
     * channels, and the channel is running. Record that, so playback that
     * starts while the mic is open neither reconfigures the channel (which
     * would stop the mic's clock) nor writes 16-bit frames into 32-bit
     * slots. */
    s_out_cfg_rate = sample_rate;
    s_out_slot_bits = (channels > 2) ? 32 : 16;
    s_out_running = true;
    return ESP_OK;
}

esp_err_t mpx_audio_capture_read(int16_t *mono, size_t frames)
{
    ESP_RETURN_ON_FALSE(s_cap_open, ESP_ERR_INVALID_STATE, TAG, "capture not started");
    ESP_RETURN_ON_FALSE(mono && frames && frames <= MPX_AUDIO_MAX_FRAMES,
                        ESP_ERR_INVALID_ARG, TAG, "bad frame count");

    const int bytes = (int)(frames * s_cap_channels * sizeof(int16_t));
    const int rc = esp_codec_dev_read(s_mic->codec_dev, s_cap_scratch, bytes);
    ESP_RETURN_ON_FALSE(rc == 0, ESP_FAIL, TAG, "read failed: rc=%d", rc);

    for (size_t i = 0; i < frames; i++) {
        mono[i] = s_cap_scratch[i * s_cap_channels + s_cap_pick];
    }
    return ESP_OK;
}

void mpx_audio_capture_stop(void)
{
    if (!s_cap_open) {
        return;
    }
    esp_codec_dev_close(s_mic->codec_dev);
    free(s_cap_scratch);
    s_cap_scratch = NULL;
    s_cap_open = false;

    /* A playback that ended while this capture was open left TX running
     * (see audio_out_close()); nothing needs the clock any more, so stop
     * it the way that close would have. Not while an output stream is
     * still open, though -- that one closes itself. */
    if (s_tx && s_out_running && !s_out_scratch) {
        (void)i2s_channel_disable(s_tx);
        s_out_running = false;
        /* The codec left TX in whatever width this capture needed; make the
         * next open (with nothing capturing) put it back to 16-bit. If an
         * output stream is open right now it keeps the current width until
         * it closes, and the same reset happens on its next open. */
        s_out_cfg_rate = 0;
    }
}

bool mpx_audio_capture_active(void) { return s_cap_open; }

uint8_t mpx_audio_capture_channels(void) { return s_cap_channels; }

esp_err_t mpx_audio_capture_read_raw(int16_t *interleaved, size_t frames)
{
    ESP_RETURN_ON_FALSE(s_cap_open, ESP_ERR_INVALID_STATE, TAG, "capture not started");
    ESP_RETURN_ON_FALSE(interleaved && frames && frames <= MPX_AUDIO_MAX_FRAMES,
                        ESP_ERR_INVALID_ARG, TAG, "bad frame count");

    /* Same read as mpx_audio_capture_read(), straight into the caller's
     * buffer instead of s_cap_scratch -- there is no picking to do, so no
     * reason to bounce through the scratch buffer first. */
    const int bytes = (int)(frames * s_cap_channels * sizeof(int16_t));
    const int rc = esp_codec_dev_read(s_mic->codec_dev, interleaved, bytes);
    ESP_RETURN_ON_FALSE(rc == 0, ESP_FAIL, TAG, "read failed: rc=%d", rc);
    return ESP_OK;
}

/* ── who has the speaker ────────────────────────────────────────────────────
 *
 * Two producers share the one TX channel: voice (replies, the wake beep --
 * everything that goes through mpx_audio_output_start/stop) and music
 * (mpx_audio_music_write). Voice always wins. While a voice output is open,
 * music writes are refused, and the music player holds off; the lock makes
 * the hand-over clean -- a music block already being written finishes
 * (tens of ms) before the voice open touches the channel. */
static SemaphoreHandle_t s_out_lock;
static mpx_spk_state_t   s_voice_spk;    /* filter state, one per producer */
static mpx_spk_state_t   s_music_spk;
static volatile bool     s_voice_out;
static int16_t          *s_music_scratch;
static bool              s_music_open;

static bool out_lock(TickType_t wait)
{
    if (!s_out_lock) {
        static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
        SemaphoreHandle_t m = xSemaphoreCreateMutex();
        portENTER_CRITICAL(&mux);
        if (!s_out_lock) {
            s_out_lock = m;
            m = NULL;
        }
        portEXIT_CRITICAL(&mux);
        if (m) {
            vSemaphoreDelete(m);
        }
        if (!s_out_lock) {
            return false;
        }
    }
    return xSemaphoreTake(s_out_lock, wait) == pdTRUE;
}

static void out_unlock(void)
{
    xSemaphoreGive(s_out_lock);
}

bool mpx_audio_voice_output_active(void)
{
    return s_voice_out;
}

esp_err_t mpx_audio_output_start(uint32_t sample_rate)
{
    const bool locked = out_lock(pdMS_TO_TICKS(500));
    esp_err_t err;

    s_voice_out = true;
    mpx_spk_reset(&s_voice_spk);        /* each reply starts from silence */
    err = audio_out_open(sample_rate);
    if (err != ESP_OK) {
        s_voice_out = false;
        if (locked) {
            out_unlock();
        }
        ESP_LOGE(TAG, "cannot open I2S output");
        return err;
    }

    if (!s_out_scratch) {
        s_out_scratch = audio_alloc(MPX_AUDIO_MAX_FRAMES * 2u * sizeof(int16_t));
        if (!s_out_scratch) {
            audio_out_close();
            s_voice_out = false;
            if (locked) {
                out_unlock();
            }
            return ESP_ERR_NO_MEM;
        }
    }
    if (locked) {
        out_unlock();
    }
    return ESP_OK;
}

esp_err_t mpx_audio_music_write(const int16_t *mono, size_t frames)
{
    esp_err_t err = ESP_OK;

    ESP_RETURN_ON_FALSE(mono && frames, ESP_ERR_INVALID_ARG, TAG, "bad music block");
    if (s_voice_out || !out_lock(pdMS_TO_TICKS(100))) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_voice_out) {
        out_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_music_scratch) {
        s_music_scratch = audio_alloc(MPX_AUDIO_MAX_FRAMES * 2u * sizeof(int16_t));
        if (!s_music_scratch) {
            out_unlock();
            return ESP_ERR_NO_MEM;
        }
    }
    /* Every block: a voice reply may have closed (and stopped) the channel
     * since the last one. At the voice rate this is a no-op otherwise. */
    err = audio_out_open(MPX_AUDIO_MUSIC_RATE);
    s_music_open = err == ESP_OK;
    while (err == ESP_OK && frames > 0) {
        const size_t n = frames > MPX_AUDIO_MAX_FRAMES ? MPX_AUDIO_MAX_FRAMES : frames;
        mpx_spk_block(&s_music_spk, mono, s_music_scratch, n, s_volume);
        err = audio_out_write_stereo16(s_music_scratch, n);
        mono += n;
        frames -= n;
    }
    out_unlock();
    return err;
}

void mpx_audio_music_end(void)
{
    if (!out_lock(pdMS_TO_TICKS(500))) {
        return;
    }
    if (s_music_open && !s_voice_out) {
        audio_out_close();      /* leaves silence in the DMA ring */
    }
    s_music_open = false;
    mpx_spk_reset(&s_music_spk);
    free(s_music_scratch);
    s_music_scratch = NULL;
    out_unlock();
}

esp_err_t mpx_audio_output_write(const int16_t *mono, size_t frames)
{
    ESP_RETURN_ON_FALSE(s_out_scratch, ESP_ERR_INVALID_STATE, TAG, "output not started");
    ESP_RETURN_ON_FALSE(mono && frames && frames <= MPX_AUDIO_MAX_FRAMES,
                        ESP_ERR_INVALID_ARG, TAG, "bad frame count");

    mpx_spk_block(&s_voice_spk, mono, s_out_scratch, frames, s_volume);
    return audio_out_write_stereo16(s_out_scratch, frames);
}

void mpx_audio_output_stop(void)
{
    const bool locked = out_lock(pdMS_TO_TICKS(500));

    audio_out_close();
    free(s_out_scratch);
    s_out_scratch = NULL;
    s_voice_out = false;
    if (locked) {
        out_unlock();
    }
}

/* ── record ──────────────────────────────────────────────────────────────── */

esp_err_t mpx_audio_record_wav(const char *rel_path, uint32_t seconds,
                               uint32_t sample_rate, uint8_t channels,
                               uint8_t pick)
{
    char full[AUDIO_PATH_MAX];
    wav_header_t header = {0};
    uint8_t *chunk = NULL;
    int16_t *mono = NULL;
    FILE *f = NULL;
    esp_err_t ret = ESP_OK;
    uint32_t written_bytes = 0;
    int32_t peak = 0;
    uint64_t energy = 0;
    uint32_t energy_count = 0;
    int rc;

    ESP_RETURN_ON_FALSE(mpx_audio_have_mic(), ESP_ERR_NOT_SUPPORTED, TAG,
                        "no microphone on this board");
    /* The streaming capture (wake word, `voice talk`) is the same hardware
     * stream; opening the codec a second time underneath it interleaves two
     * readers, which is not an error anyone gets told about. */
    ESP_RETURN_ON_FALSE(!s_cap_open, ESP_ERR_INVALID_STATE, TAG,
                        "microphone busy -- `wake auto off` (or `wake stop`) first");
    ESP_RETURN_ON_FALSE(channels >= 1 && channels <= 4, ESP_ERR_INVALID_ARG,
                        TAG, "channels must be 1-4");
    ESP_RETURN_ON_FALSE(pick < channels, ESP_ERR_INVALID_ARG, TAG,
                        "pick must be below channels");
    ESP_RETURN_ON_ERROR(audio_prepare_path(rel_path, full, sizeof(full)), TAG, "bad path");

    /* channel_mask MUST be non-zero. This is not a preference.
     *
     * es7210_config_fs() contains:
     *
     *     if (es7210_is_tdm_mode(codec) && fs->channel <= 2 &&
     *             fs->channel_mask == 0) {
     *         bits >>= 1;
     *     }
     *
     * With the ES7210 in TDM and a request for two channels and no mask, the
     * driver silently HALVES the sample width -- the log line is
     * "ES7210: Bits 8" -- so it can pack two 8-bit TDM channels into each
     * 16-bit I2S slot. That is a deliberate trick for reading four
     * microphones through a two-slot I2S port, but the buffer that comes back
     * is not 16-bit PCM: each word holds two different channels, one per
     * byte. Read it as int16 and you get noise at very low level, which
     * sounds exactly like a broken microphone or a dead speaker.
     *
     * Passing a real mask skips that branch, keeps the ADC at 16 bits, and
     * gives ordinary interleaved PCM. (1 << channels) - 1 selects the first
     * `channels` channels, which is what the de-interleave below assumes. */
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = channels,
        .channel_mask    = (uint16_t)((1u << channels) - 1u),
        .sample_rate     = sample_rate,
        .mclk_multiple   = 0,          /* 0 means 256x, which is what the board wires */
    };

    rc = esp_codec_dev_open(s_mic->codec_dev, &fs);
    if (rc != 0) {
        /* Not necessarily fatal for the user: the ES7210 is in TDM and the
         * I2S slot config may not agree with the channel count asked for.
         * Saying so beats a bare error code, because the fix is to pass a
         * different channel count on the command line. */
        ESP_LOGE(TAG, "codec open failed (rc=%d) at %" PRIu32 " Hz, %u channel(s)",
                 rc, sample_rate, (unsigned)channels);
        ESP_LOGE(TAG, "try a different channel count: `audio rec 5 0 2`");
        return ESP_FAIL;
    }

    chunk = audio_alloc(AUDIO_CHUNK_BYTES);
    mono  = audio_alloc(AUDIO_CHUNK_BYTES / channels);
    ESP_GOTO_ON_FALSE(chunk && mono, ESP_ERR_NO_MEM, cleanup, TAG, "out of memory");

    f = fopen(full, "wb");
    ESP_GOTO_ON_FALSE(f, ESP_FAIL, cleanup, TAG, "cannot write %s", full);

    /* Header first with a zero length, patched once the size is known. */
    wav_header_fill(&header, sample_rate, 1, 0);
    ESP_GOTO_ON_FALSE(fwrite(&header, 1, sizeof(header), f) == sizeof(header),
                      ESP_FAIL, cleanup, TAG, "header write failed");

    const uint32_t frames_per_chunk = AUDIO_CHUNK_BYTES / (channels * 2u);
    const uint32_t frames_total     = sample_rate * seconds;

    /* Gain must be set after open: before that the codec is unconfigured and
     * there is nothing to write the register to. */
    rc = esp_codec_dev_set_in_channel_gain(s_mic->codec_dev, fs.channel_mask,
                                           s_mic_gain_db);
    if (rc != 0) {
        rc = esp_codec_dev_set_in_gain(s_mic->codec_dev, s_mic_gain_db);
    }
    if (rc != 0) {
        ESP_LOGW(TAG, "could not set microphone gain (rc=%d); recording will "
                      "be quiet", rc);
    }

    ESP_LOGI(TAG, "recording %" PRIu32 " s at %" PRIu32 " Hz, %u channel(s), "
                  "keeping channel %u, gain %d dB",
             seconds, sample_rate, (unsigned)channels, (unsigned)pick,
             (int)s_mic_gain_db);

    for (uint32_t done = 0; done < frames_total; done += frames_per_chunk) {
        rc = esp_codec_dev_read(s_mic->codec_dev, chunk, AUDIO_CHUNK_BYTES);
        if (rc != 0) {
            ESP_LOGE(TAG, "read failed: rc=%d", rc);
            ret = ESP_FAIL;
            goto cleanup;
        }

        /* De-interleave: keep one channel out of every frame. */
        const int16_t *src = (const int16_t *)chunk;
        for (uint32_t i = 0; i < frames_per_chunk; i++) {
            mono[i] = src[i * channels + pick];
        }

        for (uint32_t i = 0; i < frames_per_chunk; i++) {
            const int32_t v = mono[i] < 0 ? -(int32_t)mono[i] : mono[i];
            if (v > peak) {
                peak = v;
            }
            energy += (uint64_t)((int32_t)mono[i] * (int32_t)mono[i]);
            energy_count++;
        }

        const size_t bytes = frames_per_chunk * sizeof(int16_t);
        if (fwrite(mono, 1, bytes, f) != bytes) {
            ESP_LOGE(TAG, "short write -- storage full?");
            ret = ESP_FAIL;
            goto cleanup;
        }
        written_bytes += (uint32_t)bytes;
    }

    /* Patch the two length fields now that the size is known. */
    wav_header_fill(&header, sample_rate, 1, written_bytes);
    if (fseek(f, 0, SEEK_SET) != 0 ||
            fwrite(&header, 1, sizeof(header), f) != sizeof(header)) {
        ESP_LOGE(TAG, "could not patch WAV header");
        ret = ESP_FAIL;
        goto cleanup;
    }

    /* The single most useful number here, because it answers "did the
     * microphone hear anything" without leaving the console. Full scale is
     * 32767; a silent channel sits in the low tens. */
    {
        const uint32_t rms = energy_count
                             ? (uint32_t)isqrt64(energy / energy_count) : 0;
        ESP_LOGI(TAG, "level: peak %" PRId32 " / 32767, rms %" PRIu32,
                 peak, rms);
        if (peak < 200) {
            ESP_LOGW(TAG, "that is silence -- wrong channel, or the "
                          "microphone is not being reached");
        }
    }

    ESP_LOGI(TAG, "wrote %s (%" PRIu32 " bytes)", full, written_bytes);
    ESP_LOGI(TAG, "download it: http://<robot-ip>/files/%s", rel_path);

cleanup:
    if (f) {
        fclose(f);
    }
    free(chunk);
    free(mono);
    esp_codec_dev_close(s_mic->codec_dev);
    return ret;
}

/* ── tone ──────────────────────────────────────────────────────────────── */

esp_err_t mpx_audio_play_tone(uint32_t hz, uint32_t seconds)
{
    const uint32_t rate = 16000;
    int16_t *buf = NULL;
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(mpx_audio_have_speaker(), ESP_ERR_NOT_SUPPORTED, TAG,
                        "no speaker on this board");
    ESP_RETURN_ON_FALSE(hz >= 20 && hz <= rate / 2, ESP_ERR_INVALID_ARG, TAG,
                        "frequency must be 20 Hz to %" PRIu32 " Hz", rate / 2);

    ESP_RETURN_ON_ERROR(audio_out_open(rate), TAG, "cannot open I2S output");

    const uint32_t frames_per_chunk = AUDIO_CHUNK_BYTES / 4u;
    buf = audio_alloc(frames_per_chunk * 2u * sizeof(int16_t));
    ESP_GOTO_ON_FALSE(buf, ESP_ERR_NO_MEM, cleanup, TAG, "out of memory");

    ESP_LOGI(TAG, "playing %" PRIu32 " Hz for %" PRIu32 " s at volume %d%%",
             hz, seconds, s_volume);

    /* A third of full scale on purpose: a full-scale tone into a small
     * speaker is unpleasant and tells you nothing extra. */
    const float amplitude = 10000.0f * (float)s_volume / 100.0f;
    uint32_t phase = 0;
    const uint32_t frames_total = rate * seconds;

    for (uint32_t done = 0; done < frames_total; done += frames_per_chunk) {
        for (uint32_t i = 0; i < frames_per_chunk; i++) {
            const float t = (float)phase / (float)rate;
            const int16_t v =
                (int16_t)(amplitude * sinf(2.0f * (float)M_PI * (float)hz * t));
            buf[i * 2]     = v;
            buf[i * 2 + 1] = v;
            /* Wrap on a whole second so the phase stays continuous and the
             * float keeps its precision over a long tone. */
            if (++phase >= rate) {
                phase = 0;
            }
        }
        ret = audio_out_write_stereo16(buf, frames_per_chunk);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "i2s write failed: %s", esp_err_to_name(ret));
            goto cleanup;
        }
    }
    ESP_LOGI(TAG, "tone done");

cleanup:
    free(buf);
    audio_out_close();
    return ret;
}

/* ── play ────────────────────────────────────────────────────────────────── */

esp_err_t mpx_audio_play_wav(const char *rel_path)
{
    char full[AUDIO_PATH_MAX];
    wav_header_t header = {0};
    int16_t *file_buf = NULL;
    int16_t *out_buf = NULL;
    FILE *f = NULL;
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(mpx_audio_have_speaker(), ESP_ERR_NOT_SUPPORTED, TAG,
                        "no speaker on this board");
    ESP_RETURN_ON_ERROR(audio_prepare_path(rel_path, full, sizeof(full)), TAG, "bad path");

    f = fopen(full, "rb");
    ESP_RETURN_ON_FALSE(f, ESP_ERR_NOT_FOUND, TAG, "cannot open %s", full);

    if (fread(&header, 1, sizeof(header), f) != sizeof(header) ||
            memcmp(header.riff, "RIFF", 4) != 0 ||
            memcmp(header.wave, "WAVE", 4) != 0) {
        fclose(f);
        ESP_LOGE(TAG, "%s is not a WAV file", full);
        return ESP_ERR_INVALID_ARG;
    }
    if (header.format != 1 || header.bits_per_sample != 16 ||
            header.channels < 1 || header.channels > 2) {
        fclose(f);
        ESP_LOGE(TAG, "need 16-bit PCM, 1 or 2 channels; got format=%u bits=%u ch=%u",
                 header.format, header.bits_per_sample, header.channels);
        return ESP_ERR_INVALID_ARG;
    }

    /* The amplifier takes two I2S slots regardless of what the file holds, so
     * a mono file is written to both. */
    ret = audio_out_open(header.sample_rate);
    if (ret != ESP_OK) {
        fclose(f);
        ESP_LOGE(TAG, "cannot open I2S output: %s", esp_err_to_name(ret));
        return ret;
    }

    const uint32_t frames_per_chunk = AUDIO_CHUNK_BYTES / 4u;   /* stereo out */
    file_buf = audio_alloc(frames_per_chunk * header.channels * sizeof(int16_t));
    out_buf  = audio_alloc(frames_per_chunk * 2u * sizeof(int16_t));
    ESP_GOTO_ON_FALSE(file_buf && out_buf, ESP_ERR_NO_MEM, cleanup, TAG, "out of memory");

    ESP_LOGI(TAG, "playing %s: %" PRIu32 " Hz, %u channel(s), volume %d%%",
             full, header.sample_rate, header.channels, s_volume);

    for (;;) {
        const size_t want = frames_per_chunk * header.channels;
        const size_t got = fread(file_buf, sizeof(int16_t), want, f);
        if (got == 0) {
            break;
        }
        const size_t frames = got / header.channels;

        for (size_t i = 0; i < frames; i++) {
            const int32_t l = file_buf[i * header.channels];
            const int32_t r = (header.channels == 2) ? file_buf[i * 2 + 1] : l;
            /* Volume in the samples: the MAX98357A cannot do it in hardware.
             * int32 intermediate because a full-scale sample times 100 does
             * not fit in 16 bits. */
            out_buf[i * 2]     = (int16_t)(l * s_volume / 100);
            out_buf[i * 2 + 1] = (int16_t)(r * s_volume / 100);
        }

        ret = audio_out_write_stereo16(out_buf, frames);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "i2s write failed: %s", esp_err_to_name(ret));
            goto cleanup;
        }
        if (got < want) {
            break;                      /* short read means end of file */
        }
    }
    ESP_LOGI(TAG, "playback done");

cleanup:
    if (f) {
        fclose(f);
    }
    free(file_buf);
    free(out_buf);
    audio_out_close();
    return ret;
}

#else  /* CONFIG_APP_AUDIO_ENABLE */

/* Audio compiled out. Everything answers honestly rather than failing to
 * link, so the callers (the Pi's music, voice) degrade to "no speaker". */

esp_err_t mpx_audio_init(void)
{
    ESP_LOGI(TAG, "audio disabled (CONFIG_APP_AUDIO_ENABLE=n)");
    return ESP_OK;
}

bool mpx_audio_have_mic(void)     { return false; }
bool mpx_audio_have_speaker(void) { return false; }

esp_err_t mpx_audio_record_wav(const char *rel_path, uint32_t seconds,
                               uint32_t sample_rate, uint8_t channels,
                               uint8_t pick)
{
    (void)rel_path; (void)seconds; (void)sample_rate; (void)channels; (void)pick;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t mpx_audio_play_wav(const char *rel_path)
{
    (void)rel_path;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t mpx_audio_play_tone(uint32_t hz, uint32_t seconds)
{
    (void)hz; (void)seconds;
    return ESP_ERR_NOT_SUPPORTED;
}

static int s_volume_stub = 70;
void mpx_audio_set_volume(int percent) { s_volume_stub = percent; }
int  mpx_audio_get_volume(void)        { return s_volume_stub; }
void mpx_audio_set_volume_transient(int percent) { s_volume_stub = percent; }

static int s_gain_stub = 30;
void mpx_audio_set_mic_gain(int db) { s_gain_stub = db; }
int  mpx_audio_get_mic_gain(void)   { return s_gain_stub; }

esp_err_t mpx_audio_capture_start(uint32_t sample_rate, uint8_t channels, uint8_t pick)
{
    (void)sample_rate; (void)channels; (void)pick;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t mpx_audio_capture_read(int16_t *mono, size_t frames)
{
    (void)mono; (void)frames;
    return ESP_ERR_NOT_SUPPORTED;
}

void mpx_audio_capture_stop(void) { }

bool mpx_audio_capture_active(void) { return false; }
uint8_t mpx_audio_capture_channels(void) { return 0; }

esp_err_t mpx_audio_capture_read_raw(int16_t *interleaved, size_t frames)
{
    (void)interleaved; (void)frames;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t mpx_audio_output_start(uint32_t sample_rate)
{
    (void)sample_rate;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t mpx_audio_output_write(const int16_t *mono, size_t frames)
{
    (void)mono; (void)frames;
    return ESP_ERR_NOT_SUPPORTED;
}

void mpx_audio_output_stop(void) { }

bool mpx_audio_voice_output_active(void) { return false; }

esp_err_t mpx_audio_music_write(const int16_t *mono, size_t frames)
{
    (void)mono; (void)frames;
    return ESP_ERR_NOT_SUPPORTED;
}

void mpx_audio_music_end(void) { }

#endif  /* CONFIG_APP_AUDIO_ENABLE */
