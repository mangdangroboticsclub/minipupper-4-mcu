/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * The screen, as tools the agent can call.
 *
 * Two decisions worth explaining.
 *
 * Emotions are a drawn face, not bitmaps and not an emoji font. Both of those
 * cost flash for something whose every frame is two rounded rectangles; see
 * cap_display_face.c, which owns the geometry and the animation. What matters
 * here is that the face is this robot's home screen, so the tools below move
 * it rather than take the panel over: showing a message borrows the screen and
 * gives it back, and clearing returns to the face rather than to a blank
 * panel that looks like a crash.
 *
 * The session is SHARED_LVGL, not EXCLUSIVE. Exclusive would fight the system
 * UI and any Lua skill that wants to draw; shared means the last writer wins
 * and nothing gets locked out. A tool the model calls casually should not be
 * able to take the screen away from a running skill.
 */

#include "cap_display.h"
#include "cap_display_face.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "driver/ledc.h"
#include "esp_board_device.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
/* The backlight is a LEDC channel, and periph_ledc.h only lands on the include
 * path when the selected board declares a `ledc` peripheral. A board without
 * one -- boards/generic_esp32s3, say -- must still build: this file already
 * copes with the brightness DEVICE being absent at runtime, so it should cope
 * with the PERIPHERAL being absent at compile time the same way. */
#if defined(__has_include)
#  if __has_include("periph_ledc.h")
#    include "periph_ledc.h"
#    define CAP_DISPLAY_HAS_LEDC 1
#  endif
#endif

#include "display_service.h"

#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cap_display";

#define CAP_DISPLAY_TEXT_MAX 128
#define CAP_DISPLAY_BRIGHTNESS_DEVICE "lcd_brightness"

#define CAP_DISPLAY_TEXT_HOLD_MS 8000

static display_service_session_handle_t s_session;
static bool        s_own_load;        /* our lv_screen_load in progress */
static bool        s_guard_pending;   /* face restore already queued   */
static bool        s_guard_installed;
static bool        s_backlight_on;
static int         s_brightness = 80; /* percent; NVS "display"/"bright" */
static bool        s_talk_face;       /* NVS "display"/"talk", default off */
static lv_obj_t   *s_text_screen;
static lv_obj_t   *s_label;
static lv_timer_t *s_text_timer;

/* The network line normally shown under the eyes; the conversation
 * subtitles (further down) borrow the line and hand it back to this. */
static char s_network_line[96] = "";
static char s_net_esp[64] = "";   /* the robot's own line: IP, or "join <AP>" */
static char s_net_pi[16] = "";    /* the Pi's IP, "" without one */
static volatile bool s_ros;       /* the Pi (ROS) is driving the robot */

/* Compose and show "<robot>   Pi <ip>". Remembered so the conversation
 * subtitles can hand the line back once the robot has been quiet. */
static void network_line_refresh(void)
{
    /* "ROS" in front while the Pi drives the robot (mpv2's ROS badge), so
     * anyone wondering why the app and voice do nothing can see why. */
    const char *ros = s_ros ? "ROS  " : "";

    if (s_net_pi[0]) {
        snprintf(s_network_line, sizeof(s_network_line), "%s%s   Pi %s", ros, s_net_esp, s_net_pi);
    } else {
        snprintf(s_network_line, sizeof(s_network_line), "%s%s", ros, s_net_esp);
    }
    (void)cap_display_face_set_status(s_network_line);
}

/* ── The screen ────────────────────────────────────────────────────────── */

static esp_err_t ensure_session(void)
{
    display_service_session_config_t cfg = {
        .owner_name = "cap_display",
        .mode = DISPLAY_SERVICE_MODE_SHARED_LVGL,
        .flags = DISPLAY_SERVICE_SESSION_FLAG_RESTORE_DEFAULT_ON_RELEASE,
    };
    esp_err_t err;

    if (!display_service_is_started()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_session != NULL && display_service_session_is_valid(s_session)) {
        return ESP_OK;
    }

    err = display_service_open(&cfg, &s_session);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display_service_open failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

/* Put `screen` on the panel. Caller holds the LVGL lock.
 *
 * NOT display_service_session_load_screen_locked(). That call is only for
 * sessions opened in DISPLAY_SERVICE_MODE_EXCLUSIVE_LVGL and refuses every
 * other kind with "session is not exclusive LVGL" -- which is what this
 * component used to hit on every call, so the face never came up at boot
 * ("Face not started: ESP_ERR_INVALID_STATE") and display_show_text from
 * the agent failed the same way while the log said the tool call arrived.
 *
 * This session is SHARED on purpose: an exclusive session would hold the
 * panel's scene lock permanently, and lua_module_lvgl (the agent's own
 * drawing) opens an exclusive session of its own, which would then fail.
 * A shared client draws the way ESP-Claw's system_ui does it: check that
 * nobody holds the panel exclusively, then lv_screen_load() under the lock
 * (system_ui_load_screen_locked() is exactly this). When Lua does hold it,
 * this refuses and the face comes back on its own when Lua releases --
 * that is what registering the face as the default screen is for. */
static esp_err_t load_screen_locked(lv_obj_t *screen)
{
    if (!screen) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!display_service_session_is_valid(s_session)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (display_service_has_exclusive_session()) {
        /* Someone else -- a Lua drawing -- owns the panel right now. */
        return ESP_ERR_INVALID_STATE;
    }
    s_own_load = true;
    lv_screen_load(screen);
    s_own_load = false;
    return ESP_OK;
}

/* ── Backlight ─────────────────────────────────────────────────────────────
 *
 * The board powers the backlight up at 0% (board_devices.yaml): the ST7789
 * comes out of reset with random RAM that reads as white, and lighting it
 * then is the white flash at boot. It goes on once the eyes are on the
 * panel, at the brightness last chosen (display_set_brightness, kept in
 * NVS). */
static esp_err_t backlight_apply(int percent)
{
#if !CAP_DISPLAY_HAS_LEDC
    (void)percent;
    return ESP_ERR_NOT_SUPPORTED;
#else
    void *handle = NULL;
    if (esp_board_device_get_handle(CAP_DISPLAY_BRIGHTNESS_DEVICE, &handle) != ESP_OK ||
            handle == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (percent < 0)   { percent = 0; }
    if (percent > 100) { percent = 100; }
    /* 10-bit LEDC channel; output_invert in the board YAML handles the
     * P-channel FET, so 100 here always means bright. */
    periph_ledc_handle_t *ledc = (periph_ledc_handle_t *)handle;
    const uint32_t duty = (uint32_t)(((1U << LEDC_TIMER_10_BIT) - 1U) * percent / 100);
    if (ledc_set_duty(ledc->speed_mode, ledc->channel, duty) != ESP_OK ||
            ledc_update_duty(ledc->speed_mode, ledc->channel) != ESP_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
#endif
}

static void display_prefs_load(void)
{
    nvs_handle_t h;
    uint8_t v;
    if (nvs_open("display", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    if (nvs_get_u8(h, "bright", &v) == ESP_OK && v >= 5 && v <= 100) {
        s_brightness = v;
    }
    if (nvs_get_u8(h, "talk", &v) == ESP_OK) {
        s_talk_face = v != 0;
    }
    if (nvs_get_u8(h, "alive", &v) == ESP_OK) {
        cap_display_face_set_alive(v != 0);
    }
    if (nvs_get_u8(h, "eyes_inv", &v) == ESP_OK) {
        cap_display_face_set_invert(v);
    }
    nvs_close(h);
}

static void display_pref_save(const char *key, uint8_t value)
{
    nvs_handle_t h;
    if (nvs_open("display", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, key, value);
    nvs_commit(h);
    nvs_close(h);
}

/* The brightness tool runs on whatever task called it -- the agent's or the
 * voice bridge's, both with PSRAM stacks, where an NVS write (flash, cache
 * off) is not safe. So it is saved from the esp_timer task instead. */
static esp_timer_handle_t s_bright_save_timer;

static void bright_save_cb(void *arg)
{
    (void)arg;
    display_pref_save("bright", (uint8_t)s_brightness);
}

static void bright_save_later(void)
{
    if (s_bright_save_timer == NULL) {
        const esp_timer_create_args_t args = { .callback = bright_save_cb, .name = "bright_save" };
        if (esp_timer_create(&args, &s_bright_save_timer) != ESP_OK) {
            return;
        }
    }
    (void)esp_timer_stop(s_bright_save_timer);
    (void)esp_timer_start_once(s_bright_save_timer, 500 * 1000);
}

bool cap_display_talk_face(void)
{
    return s_talk_face;
}

void cap_display_set_talk_face(bool on)
{
    s_talk_face = on;
    display_pref_save("talk", on ? 1 : 0);
}

void cap_display_set_eyes_alive(bool on)
{
    cap_display_face_set_alive(on);
    display_pref_save("alive", on ? 1 : 0);
}

void cap_display_set_eyes_invert(uint8_t bits)
{
    cap_display_face_set_invert(bits);
    display_pref_save("eyes_inv", (uint8_t)(bits & 3));
}

/* Put the face back on the panel. Caller holds the LVGL lock. */
static esp_err_t show_face_locked(void)
{
    lv_obj_t *face = cap_display_face_screen_locked();

    if (!face) {
        return ESP_ERR_NO_MEM;
    }
    return load_screen_locked(face);
}

static void text_expired_cb(lv_timer_t *timer)
{
    (void)timer;
    /* Already inside LVGL, so the lock is held. */
    (void)show_face_locked();
    s_text_timer = NULL;
}

/* Show `text` scaled to fill the panel, then hand the screen back to the face.
 *
 * The hand-back is the point. The face is the robot's resting state, so a
 * message is something it says and stops saying -- without the timer, one
 * display_show_text call would leave a wall of words up until the next tool
 * call, which on a quiet robot could be hours.
 *
 * Caller must NOT hold the LVGL lock: this takes it. */
static esp_err_t paint_text(const char *text)
{
    esp_err_t err = ensure_session();

    if (err != ESP_OK) {
        return err;
    }
    if (display_service_lock() != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_text_screen == NULL) {
        s_text_screen = lv_obj_create(NULL);
        if (s_text_screen == NULL) {
            display_service_unlock();
            return ESP_ERR_NO_MEM;
        }
        lv_obj_set_style_bg_color(s_text_screen, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_text_screen, LV_OPA_COVER, LV_PART_MAIN);

        s_label = lv_label_create(s_text_screen);
        lv_obj_set_style_text_color(s_label, lv_color_white(), LV_PART_MAIN);
        lv_label_set_long_mode(s_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s_label, lv_pct(90));
        lv_obj_set_style_text_align(s_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_center(s_label);
    }

    lv_label_set_text(s_label, text);
    err = load_screen_locked(s_text_screen);

    if (s_text_timer) {
        lv_timer_reset(s_text_timer);
    } else {
        s_text_timer = lv_timer_create(text_expired_cb, CAP_DISPLAY_TEXT_HOLD_MS, NULL);
        if (s_text_timer) {
            lv_timer_set_repeat_count(s_text_timer, 1);
        }
    }
    display_service_unlock();
    return err;
}

/* ── Bring-up test pattern ─────────────────────────────────────────────────
 *
 * Three values in board_devices.yaml cannot be settled from a schematic --
 * mirror_x, swap_xy and invert_color describe how a panel is physically
 * mounted, not how it is wired. The only way to check them is to draw
 * something whose correct appearance is unambiguous and look at it.
 *
 * Hence corner labels rather than a pretty pattern: "TL" in the wrong corner
 * names the wrong flag directly. The border frame catches a row/column offset,
 * which otherwise hides as a thin band of noise at one edge.
 */

esp_err_t cap_display_show_test_pattern(uint32_t hold_ms)
{
    static const struct { const char *text; lv_align_t align; int dx, dy; } corners[] = {
        { "TL", LV_ALIGN_TOP_LEFT,      6,   6 },
        { "TR", LV_ALIGN_TOP_RIGHT,    -6,   6 },
        { "BL", LV_ALIGN_BOTTOM_LEFT,   6,  -6 },
        { "BR", LV_ALIGN_BOTTOM_RIGHT, -6,  -6 },
    };
    static const uint32_t bars[3] = { 0xFF0000, 0x00FF00, 0x0000FF };

    esp_err_t err = ensure_session();
    lv_obj_t *screen;

    if (err != ESP_OK) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (display_service_lock() != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }

    screen = lv_obj_create(NULL);
    if (!screen) {
        display_service_unlock();
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    /* A frame on the outermost pixels. If the panel is offset, one edge of
     * this is missing or doubled. */
    lv_obj_set_style_border_color(screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 2, LV_PART_MAIN);

    for (size_t i = 0; i < sizeof(corners) / sizeof(corners[0]); i++) {
        lv_obj_t *label = lv_label_create(screen);
        if (!label) {
            continue;
        }
        lv_label_set_text(label, corners[i].text);
        lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
        lv_obj_align(label, corners[i].align, corners[i].dx, corners[i].dy);
    }

    /* Red, green, blue, left to right. Any other order or hue means the
     * colour format or invert_color is wrong. */
    for (int i = 0; i < 3; i++) {
        lv_obj_t *bar = lv_obj_create(screen);
        if (!bar) {
            continue;
        }
        lv_obj_set_size(bar, 60, 40);
        lv_obj_set_style_bg_color(bar, lv_color_hex(bars[i]), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(bar, LV_ALIGN_CENTER, (i - 1) * 66, 34);
    }

    {
        lv_obj_t *size_label = lv_label_create(screen);
        if (size_label) {
            lv_label_set_text_fmt(size_label, "%d x %d",
                                  (int)lv_display_get_horizontal_resolution(NULL),
                                  (int)lv_display_get_vertical_resolution(NULL));
            lv_obj_set_style_text_color(size_label, lv_color_hex(0xFFE631), LV_PART_MAIN);
            lv_obj_align(size_label, LV_ALIGN_CENTER, 0, -30);
        }
    }

    err = load_screen_locked(screen);
    display_service_unlock();
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(hold_ms));

    /* Put the face back and drop the pattern. Deleting the screen only after
     * another one is loaded, because deleting the active screen is what LVGL
     * documents as undefined. */
    if (display_service_lock() == ESP_OK) {
        (void)show_face_locked();
        lv_obj_delete(screen);
        display_service_unlock();
    }
    return ESP_OK;
}

/* ── The Raspberry Pi's camera picture ─────────────────────────────────────
 *
 * mpx_pi_link reassembles the Pi's VIDEO_DATA chunks into one baseline JPEG
 * (<= 320x240) and hands it here from its own task. Ported from mpv2's
 * display::show_jpeg(): decode outside the display lock (10-20 ms) into the
 * buffer LVGL is not showing, then swap it in under the lock. The camera
 * screen is one of this component's own screens, so the face guard below
 * leaves it alone, and the face comes back 1.5 s after the last frame. */
#if defined(__has_include)
#if __has_include("esp_jpeg_dec.h")
#include "esp_jpeg_dec.h"
#define CAP_DISPLAY_HAVE_JPEG 1
#endif
#endif

#define CAP_DISPLAY_VIDEO_W        320
#define CAP_DISPLAY_VIDEO_H        240
#define CAP_DISPLAY_VIDEO_HOLD_MS  1500

static lv_obj_t   *s_video_screen;
static lv_obj_t   *s_video_canvas;
static lv_obj_t   *s_video_label;
static lv_timer_t *s_video_timer;

#if CAP_DISPLAY_HAVE_JPEG
static uint16_t   *s_video_buf[2];         /* PSRAM, RGB565, 320x240 each */
static int         s_video_back;           /* the one LVGL is NOT showing */
static uint32_t    s_video_frames;
static int64_t     s_video_fps_t0;

static void video_expired_cb(lv_timer_t *timer)
{
    (void)timer;
    /* Inside LVGL: the lock is held. A one-shot timer deletes itself. */
    s_video_timer = NULL;
    if (lv_screen_active() == s_video_screen) {
        (void)show_face_locked();
    }
}

static esp_err_t video_screen_locked(void)
{
    if (s_video_screen) {
        return ESP_OK;
    }
    s_video_screen = lv_obj_create(NULL);
    if (!s_video_screen) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_style_bg_color(s_video_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_video_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_video_screen, 0, LV_PART_MAIN);
    lv_obj_remove_flag(s_video_screen, LV_OBJ_FLAG_SCROLLABLE);
    s_video_canvas = lv_canvas_create(s_video_screen);
    s_video_label = lv_label_create(s_video_screen);
    lv_obj_set_style_text_color(s_video_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_video_label, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_video_label, LV_OPA_50, LV_PART_MAIN);
    lv_obj_align(s_video_label, LV_ALIGN_BOTTOM_LEFT, 4, -4);
    lv_label_set_text(s_video_label, "Pi camera");
    return ESP_OK;
}
#endif

esp_err_t cap_display_show_jpeg(const uint8_t *jpeg, size_t len)
{
#if !CAP_DISPLAY_HAVE_JPEG
    (void)jpeg; (void)len;
    return ESP_ERR_NOT_SUPPORTED;
#else
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    jpeg_dec_handle_t dec = NULL;
    jpeg_dec_io_t io = { 0 };
    jpeg_dec_header_info_t info = { 0 };
    esp_err_t err = ESP_OK;

    if (!cap_display_available()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!jpeg || len < 4) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_video_buf[0]) {
        for (int i = 0; i < 2; ++i) {
            s_video_buf[i] = heap_caps_aligned_calloc(16, 1, CAP_DISPLAY_VIDEO_W * CAP_DISPLAY_VIDEO_H * 2,
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!s_video_buf[i]) {
                heap_caps_free(s_video_buf[0]);
                s_video_buf[0] = NULL;
                return ESP_ERR_NO_MEM;
            }
        }
    }

    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;      /* LVGL's own RGB565 */
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        return ESP_ERR_NO_MEM;
    }
    io.inbuf = (uint8_t *)jpeg;
    io.inbuf_len = (int)len;
    if (jpeg_dec_parse_header(dec, &io, &info) != JPEG_ERR_OK) {
        err = ESP_ERR_INVALID_ARG;
    } else if (info.width == 0 || info.height == 0 ||
               info.width > CAP_DISPLAY_VIDEO_W || info.height > CAP_DISPLAY_VIDEO_H) {
        err = ESP_ERR_INVALID_SIZE;
    } else {
        io.outbuf = (uint8_t *)s_video_buf[s_video_back];
        if (jpeg_dec_process(dec, &io) != JPEG_ERR_OK) {
            err = ESP_FAIL;
        }
    }
    jpeg_dec_close(dec);
    if (err != ESP_OK) {
        return err;
    }

    if (display_service_lock() != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    err = video_screen_locked();
    if (err == ESP_OK) {
        lv_canvas_set_buffer(s_video_canvas, s_video_buf[s_video_back], info.width, info.height,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_center(s_video_canvas);
        lv_obj_invalidate(s_video_canvas);
        s_video_back ^= 1;

        const int64_t now = esp_timer_get_time();
        if (++s_video_frames >= 10 || now - s_video_fps_t0 > 3000000) {
            if (s_video_fps_t0) {
                char t[32];
                snprintf(t, sizeof(t), "Pi camera  %.1f fps",
                         s_video_frames * 1e6 / (double)(now - s_video_fps_t0));
                lv_label_set_text(s_video_label, t);
            }
            s_video_frames = 0;
            s_video_fps_t0 = now;
        }
        if (lv_screen_active() != s_video_screen) {
            err = load_screen_locked(s_video_screen);   /* refused while Lua owns the panel */
        }
        if (s_video_timer) {
            lv_timer_reset(s_video_timer);
        } else {
            s_video_timer = lv_timer_create(video_expired_cb, CAP_DISPLAY_VIDEO_HOLD_MS, NULL);
            if (s_video_timer) {
                lv_timer_set_repeat_count(s_video_timer, 1);
            }
        }
    }
    display_service_unlock();
    return err;
#endif
}

void cap_display_set_ros_mode(bool on)
{
    s_ros = on;
    if (!cap_display_available()) {
        return;
    }
    network_line_refresh();
}

/* ── Boot ──────────────────────────────────────────────────────────────── */

bool cap_display_available(void)
{
    return s_session != NULL;
}

/* ── Draw buffers the SPI can send straight from ───────────────────────────
 *
 * The "SPI with PSRAM" profile puts LVGL's draw buffers in PSRAM, which the
 * SPI DMA cannot read. So spi_master copies every flush into a bounce buffer
 * it allocates from internal RAM on the spot, and frees again. Once the voice
 * wake word is listening, the largest free internal block is ~3.5 KB -- less
 * than one 8-line flush (5 KB) -- and every flush failed: "Draw bitmap
 * failed: ESP_ERR_NO_MEM", a frozen face, a GIF stuck on its first frame.
 *
 * Fix: right after the display starts (early boot, ~180 KB internal free),
 * hand LVGL two buffers of its own in internal DMA RAM -- 2 x 5 KB, held for
 * good. The flush then goes out straight from them, and nothing is ever
 * allocated per frame again. Rendering into internal RAM is faster too. */
static bool s_internal_draw_bufs;

bool cap_display_draw_bufs_internal(void)
{
    return s_internal_draw_bufs;
}

static void use_internal_draw_buffers(void)
{
    lv_display_t *disp;
    void *b1 = NULL, *b2 = NULL;
    size_t bytes = 0;

    if (display_service_lock() != ESP_OK) {
        return;
    }
    disp = lv_display_get_default();
    if (disp) {
        const uint32_t w = (uint32_t)lv_display_get_horizontal_resolution(disp);
        const lv_color_format_t cf = lv_display_get_color_format(disp);

        bytes = (size_t)lv_draw_buf_width_to_stride(w, cf) * CONFIG_MP4_DISPLAY_BUFFER_LINES;
        b1 = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        b2 = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (b1 && b2) {
            lv_display_set_buffers(disp, b1, b2, (uint32_t)bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
            s_internal_draw_bufs = true;
        }
    }
    display_service_unlock();
    if (s_internal_draw_bufs) {
        ESP_LOGI(TAG, "draw buffers: 2 x %u bytes in internal DMA RAM (no per-flush bounce buffer)",
                 (unsigned)bytes);
    } else {
        heap_caps_free(b1);
        heap_caps_free(b2);
        ESP_LOGW(TAG, "draw buffers: could not move them to internal RAM, keeping PSRAM ones");
    }
}

esp_err_t cap_display_service_start(void)
{
    /* Start the display service ourselves, before anything else opens a
     * session, purely to choose the draw-buffer height.
     *
     * display_service_start() takes the buffer height from the FIRST caller
     * and ignores every later one -- it returns ESP_OK immediately once
     * started. system_ui opens a session with a NULL config during
     * app_claw_ui_start(), which lands on the upstream default of 40 lines,
     * and by the time cap_display opens its own session the size is fixed.
     * So this has to run first or not at all.
     *
     * Why it matters: with PSRAM enabled, display_service selects the "SPI
     * with PSRAM" profile, the draw buffer lives in external RAM, and
     * spi_master has to bounce every flush through an internal DMA buffer of
     * the same size. 40 lines is 25 KB of internal RAM churned several times
     * a second; on a board with little to spare that allocation fails and
     * every flush is dropped. */
    const display_service_config_t cfg = {
        .buffer_lines = CONFIG_MP4_DISPLAY_BUFFER_LINES,
    };

    const esp_err_t err = display_service_start(&cfg);
    if (err != ESP_OK) {
        /* Not fatal: a board with no panel reports one of these, and the
         * face already treats "no display" as a normal configuration. */
        ESP_LOGI(TAG, "display service not started: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "display service started, draw buffer %d lines",
             (int)CONFIG_MP4_DISPLAY_BUFFER_LINES);
    use_internal_draw_buffers();
    (void)cap_display_apply_orientation();
    return ESP_OK;
}

/* ── The face stays the home screen ────────────────────────────────────────
 *
 * ESP-Claw's system_ui loads its clock/launcher home twice during boot (when
 * it starts, and again from app_claw_start() once the skill registry is up)
 * and makes it the display's default screen. On this robot -- no touch panel
 * to operate a launcher -- that clock is never wanted: any screen loaded that
 * is not ours, while nobody holds the panel exclusively, is put back to the
 * face straight away. Exclusive sessions (a Lua drawing the agent made, the
 * "draw a crab" case) are left alone, and so are this component's own
 * screens (text, test pattern), which load through load_screen_locked(). */
static void restore_face_async(void *arg)
{
    (void)arg;
    s_guard_pending = false;
    if (!display_service_session_is_valid(s_session) || display_service_has_exclusive_session()) {
        return;
    }
    lv_obj_t *face = cap_display_face_screen_locked();
    if (face && lv_screen_active() != face && lv_screen_active() != s_text_screen &&
        lv_screen_active() != s_video_screen) {
        (void)load_screen_locked(face);
    }
    if (face) {
        display_service_set_default_screen_locked(face);
    }
}

static void screen_loaded_cb(lv_event_t *e)
{
    lv_obj_t *scr = (lv_obj_t *)lv_event_get_param(e);
    if (s_own_load || s_guard_pending || scr == NULL) {
        return;
    }
    if (scr == cap_display_face_screen_locked() || scr == s_text_screen || scr == s_video_screen) {
        return;
    }
    if (display_service_has_exclusive_session()) {
        return;   /* a Lua drawing: the agent asked for this */
    }
    s_guard_pending = true;
    lv_async_call(restore_face_async, NULL);
}

esp_err_t cap_display_face_start(void)
{
    static bool prefs_loaded;
    if (!prefs_loaded) {
        display_prefs_load();
        prefs_loaded = true;
    }
    esp_err_t err = ensure_session();

    if (err != ESP_OK) {
        /* A board with no panel is a normal configuration, not a failure. */
        ESP_LOGI(TAG, "no display on this board; face not started");
        return ESP_OK;
    }
    if (display_service_lock() != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    err = show_face_locked();

    /* Also make the face the display service's default screen -- what the
     * panel falls back to when no session owns it.
     *
     * This is what happens after a Lua drawing finishes. The agent can write
     * Lua that takes the display (lua_module_lvgl for objects,
     * lua_module_display for the raw framebuffer) and draws whatever it
     * invents; when that script calls display.deinit() the session is
     * released, and without this line the panel would fall back to
     * system_ui's launcher home -- a screen this board has no touch
     * controller to operate. With it, the robot always returns to its face. */
    if (err == ESP_OK) {
        lv_obj_t *face = cap_display_face_screen_locked();
        if (face) {
            display_service_set_default_screen_locked(face);
        }
        if (!s_guard_installed && lv_display_get_default()) {
            lv_display_add_event_cb(lv_display_get_default(), screen_loaded_cb,
                                    LV_EVENT_SCREEN_LOADED, NULL);
            s_guard_installed = true;
        }
        if (!s_backlight_on) {
            /* Draw the eyes now, then light the panel: no white flash. */
            lv_refr_now(NULL);
        }
    }
    display_service_unlock();

    /* Lit even if the face could not be shown (a Lua drawing holding the
     * panel, say): a dark screen would look like a dead robot. */
    if (!s_backlight_on) {
        s_backlight_on = backlight_apply(s_brightness) == ESP_OK;
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "face is up, and is the fallback screen");
    }
    return err;
}

void cap_display_face_set_network(bool sta_connected, const char *ap_ssid, const char *ip)
{
    char line[64];

    /* Nothing to draw on, so do not reach for the lock. display_service_lock()
     * logs an ESP_LOGE from esp_lv_adapter every time it is called without a
     * panel, and this runs on every Wi-Fi state change -- so a display-less
     * board printed a red error line next to each one, which looks like a
     * fault and is just this board not having a screen. */
    if (!cap_display_available()) {
        return;
    }

    /* What someone standing in front of the robot needs, in the order they
     * need it: before joining, the AP name; after, the address to type. */
    if (sta_connected && ip && ip[0]) {
        snprintf(line, sizeof(line), "%s", ip);
    } else if (ap_ssid && ap_ssid[0]) {
        snprintf(line, sizeof(line), "join %s", ap_ssid);
    } else {
        snprintf(line, sizeof(line), "no network");
    }
    snprintf(s_net_esp, sizeof(s_net_esp), "%s", line);
    network_line_refresh();
}

/* A Raspberry Pi on the J-PI header reports its address over the link
 * (mpx_pi_link); it goes after the robot's own, "" when the Pi goes away. */
void cap_display_face_set_pi(const char *ip)
{
    snprintf(s_net_pi, sizeof(s_net_pi), "%s", ip ? ip : "");
    if (!cap_display_available()) {
        return;
    }
    network_line_refresh();
}

/* ── The conversation, on the face (Phase 5) ──────────────────────────────
 *
 * mpx_voice_link tells us what the server heard, what it is about to say,
 * and which emotion it picked, through the hooks main.c registers with
 * mpx_voice_set_ui_hooks(). Two things happen here:
 *
 *   the eyes take the emotion -- the server's names (xiaozhi's list, about
 *   twenty of them) are folded onto the ten expressions the face can draw;
 *
 *   the dim line under the eyes becomes a subtitle -- "you: ..." for the
 *   transcript, then each sentence of the reply as it starts. When the
 *   reply is over and nothing else happens for a while, the line goes back
 *   to the network address it normally shows, so the panel is still useful
 *   to someone who walks up to the robot an hour later. The expression is
 *   left as it is: a robot that just laughed should not snap back to
 *   neutral on a timer.
 *
 * These run on the websocket task. cap_display_face_set_*() take the LVGL
 * lock themselves; the only thing done here without it is the esp_timer,
 * which is what makes the hand-back safe -- an lv_timer callback runs inside
 * LVGL with the lock held, and would deadlock calling set_status().         */

#define CAP_DISPLAY_VOICE_IDLE_MS   12000
#define CAP_DISPLAY_SUBTITLE_MAX    96

static esp_timer_handle_t s_voice_idle_timer;

static const struct { const char *server; const char *face; } s_emotion_map[] = {
    { "neutral",     "neutral"   },
    { "happy",       "happy"     },
    { "laughing",    "excited"   },
    { "funny",       "happy"     },
    { "silly",       "happy"     },
    { "delicious",   "happy"     },
    { "confident",   "happy"     },
    { "cool",        "cool"      },
    { "winking",     "wink"      },
    { "relaxed",     "neutral"   },
    { "thinking",    "confused"  },
    { "confused",    "confused"  },
    { "embarrassed", "confused"  },
    { "sad",         "sad"       },
    { "crying",      "sad"       },
    { "angry",       "angry"     },
    { "surprised",   "surprised" },
    { "shocked",     "surprised" },
    { "loving",      "love"      },
    { "kissy",       "love"      },
    { "sleepy",      "sleepy"    },
    { "scared",      "scared"    },
    { "afraid",      "scared"    },
    { "nervous",     "scared"    },
    { "determined",  "determined" },
    { "curious",     "curious"   },
    { "pleading",    "pleading"  },
    { "content",     "bliss"     },
};

static void voice_idle_cb(void *arg)
{
    (void)arg;
    (void)cap_display_face_set_status(s_network_line);
}

static void voice_idle_arm(bool arm)
{
    if (s_voice_idle_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = voice_idle_cb,
            .name = "face_idle",
        };
        if (esp_timer_create(&args, &s_voice_idle_timer) != ESP_OK) {
            return;
        }
    }
    (void)esp_timer_stop(s_voice_idle_timer);
    if (arm) {
        (void)esp_timer_start_once(s_voice_idle_timer, (uint64_t)CAP_DISPLAY_VOICE_IDLE_MS * 1000);
    }
}

static void voice_subtitle(const char *prefix, const char *text)
{
    char line[CAP_DISPLAY_SUBTITLE_MAX];

    if (!cap_display_available() || !text) {
        return;
    }
    /* The server's sentences arrive with their own leading newlines and
     * spaces now and then; one line is all there is room for. */
    while (*text == ' ' || *text == '\n' || *text == '\r') {
        text++;
    }
    snprintf(line, sizeof(line), "%s%s", prefix ? prefix : "", text);
    for (char *c = line; *c; c++) {
        if (*c == '\n' || *c == '\r') {
            *c = ' ';
        }
    }
    voice_idle_arm(false);              /* a live conversation: keep the subtitle */
    (void)cap_display_face_set_status(line);
}

/* By default a conversation leaves the face alone -- the eyes stay as they
 * are and the IP stays under them; the screen changes only when someone
 * asks for it (the display tools, an ESP-Claw drawing). `display talk on`
 * brings back the subtitles and the per-reply expressions. */
void cap_display_voice_heard(const char *text)
{
    cap_display_face_poke();            /* talked to: wake up if dozing */
    if (s_talk_face) {
        voice_subtitle("you: ", text);
    }
}

void cap_display_voice_saying(const char *text)
{
    if (s_talk_face) {
        voice_subtitle(NULL, text);
    }
}

void cap_display_voice_emotion(const char *name)
{
    const char *face = NULL;

    if (!s_talk_face || !cap_display_available() || !name) {
        return;
    }
    for (size_t i = 0; i < sizeof(s_emotion_map) / sizeof(s_emotion_map[0]); i++) {
        if (strcasecmp(name, s_emotion_map[i].server) == 0) {
            face = s_emotion_map[i].face;
            break;
        }
    }
    if (face == NULL) {
        /* Not in the table: maybe it is already one of ours ("wink"). */
        face = cap_display_face_is_emotion(name) ? name : "neutral";
    }
    (void)cap_display_face_set_emotion(face);
}

void cap_display_voice_reply_done(void)
{
    if (!s_talk_face || !cap_display_available()) {
        return;
    }
    voice_idle_arm(true);
}

void cap_display_voice_session_ended(void)
{
    if (!cap_display_available()) {
        return;
    }
    voice_idle_arm(false);
    (void)cap_display_face_set_status(s_network_line);
    (void)cap_display_face_set_emotion("neutral");
}

/* ── Tools ─────────────────────────────────────────────────────────────── */

static esp_err_t execute_show_text(const char *input_json,
                                   const claw_cap_call_context_t *ctx,
                                   char *output, size_t output_size)
{
    (void)ctx;
    cJSON *root;
    const cJSON *text;
    char buf[CAP_DISPLAY_TEXT_MAX];

    if (!input_json || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: input must be a JSON object");
        return ESP_ERR_INVALID_ARG;
    }

    text = cJSON_GetObjectItem(root, "text");
    if (!cJSON_IsString(text) || !text->valuestring[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: 'text' is required");
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(buf, sizeof(buf), "%s", text->valuestring);
    cJSON_Delete(root);

    if (paint_text(buf) != ESP_OK) {
        /* Two different situations, and the agent should be told which:
         * no panel at all, or a Lua drawing currently holding it. */
        snprintf(output, output_size, cap_display_available()
                 ? "Error: the display is busy (another drawing owns it right now)."
                 : "Error: the display is not available on this device.");
        return ESP_ERR_INVALID_STATE;
    }
    snprintf(output, output_size, "{\"ok\":true,\"showing\":\"%s\"}", buf);
    return ESP_OK;
}

static esp_err_t execute_show_emotion(const char *input_json,
                                      const claw_cap_call_context_t *ctx,
                                      char *output, size_t output_size)
{
    (void)ctx;
    cJSON *root;
    const cJSON *emotion;
    char name[24];
    esp_err_t err;

    if (!input_json || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: input must be a JSON object");
        return ESP_ERR_INVALID_ARG;
    }

    emotion = cJSON_GetObjectItem(root, "emotion");
    if (!cJSON_IsString(emotion) || !emotion->valuestring[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: 'emotion' is required");
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(name, sizeof(name), "%s", emotion->valuestring);
    cJSON_Delete(root);

    if (!cap_display_face_is_emotion(name)) {
        size_t at = (size_t)snprintf(output, output_size,
                                     "Error: no such emotion. Try one of: ");
        if (at < output_size) {
            cap_display_face_emotion_names(output + at, output_size - at);
        }
        return ESP_OK;
    }

    if (ensure_session() != ESP_OK || cap_display_face_set_emotion(name) != ESP_OK) {
        snprintf(output, output_size,
                 "Error: the display is not available on this device.");
        return ESP_ERR_INVALID_STATE;
    }

    /* An expression change also cancels any message still on screen: the robot
     * reacting to what was just said should be visible immediately, not in
     * eight seconds when the previous text times out. */
    if (display_service_lock() == ESP_OK) {
        if (s_text_timer) {
            lv_timer_delete(s_text_timer);
            s_text_timer = NULL;
        }
        err = show_face_locked();
        display_service_unlock();
        if (err != ESP_OK) {
            snprintf(output, output_size, "Error: could not show the face.");
            return err;
        }
    }
    snprintf(output, output_size, "{\"ok\":true,\"emotion\":\"%s\"}", name);
    return ESP_OK;
}

static esp_err_t execute_clear(const char *input_json,
                               const claw_cap_call_context_t *ctx,
                               char *output, size_t output_size)
{
    (void)input_json;
    (void)ctx;

    /* Back to the face rather than closing the session. The face is this
     * robot's home screen -- releasing the session would restore ESP-Claw's
     * launcher, which on a board with no touch controller is a screen nobody
     * can do anything with. */
    if (s_session != NULL && display_service_lock() == ESP_OK) {
        if (s_text_timer) {
            lv_timer_delete(s_text_timer);
            s_text_timer = NULL;
        }
        (void)show_face_locked();
        display_service_unlock();
    }
    snprintf(output, output_size, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t execute_set_brightness(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char *output, size_t output_size)
{
    (void)ctx;
    cJSON *root;
    const cJSON *percent_json;
    void *handle = NULL;
    int percent;

    if (!input_json || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
#if !CAP_DISPLAY_HAS_LEDC
    /* No LEDC peripheral on this board, so no dimmable backlight to reach.
     * Same answer the runtime path below gives when the device is missing --
     * the tool stays callable and says why rather than not existing. */
    snprintf(output, output_size, "Error: this board has no controllable backlight.");
    return ESP_ERR_NOT_SUPPORTED;
#else
    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: input must be a JSON object");
        return ESP_ERR_INVALID_ARG;
    }
    percent_json = cJSON_GetObjectItem(root, "percent");
    if (!cJSON_IsNumber(percent_json)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: 'percent' (0-100) is required");
        return ESP_ERR_INVALID_ARG;
    }
    percent = (int)percent_json->valuedouble;
    cJSON_Delete(root);

    if (percent < 0)   { percent = 0; }
    if (percent > 100) { percent = 100; }

    (void)handle;
    {
        const esp_err_t berr = backlight_apply(percent);
        if (berr == ESP_ERR_NOT_FOUND) {
            snprintf(output, output_size, "Error: this board has no controllable backlight.");
            return berr;
        }
        if (berr != ESP_OK) {
            snprintf(output, output_size, "Error: could not set the backlight duty.");
            return berr;
        }
    }
    /* Remembered, so the robot wakes up at the brightness it was given. A
     * request for (near) darkness is not, or it would boot to a black screen. */
    if (percent >= 5) {
        s_brightness = percent;
        bright_save_later();
    }
    s_backlight_on = true;

    snprintf(output, output_size, "{\"ok\":true,\"percent\":%d}", percent);
    return ESP_OK;
#endif /* CAP_DISPLAY_HAS_LEDC */
}

/* "green", "#00ff00", "00FF00" or "0x00ff00" -> 0xRRGGBB; -1 if unreadable. */
static int32_t parse_color(const char *text)
{
    static const struct { const char *name; uint32_t rgb; } named[] = {
        { "black",  0x000000 }, { "white",  0xFFFFFF }, { "red",    0xFF0000 },
        { "green",  0x00C000 }, { "blue",   0x0040FF }, { "yellow", 0xFFE631 },
        { "orange", 0xFF8000 }, { "purple", 0x8000FF }, { "pink",   0xFF4D8D },
        { "cyan",   0x00E0E0 }, { "grey",   0x808080 }, { "gray",   0x808080 },
    };
    if (!text || !text[0]) {
        return -1;
    }
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
        if (strcasecmp(text, named[i].name) == 0) {
            return (int32_t)named[i].rgb;
        }
    }
    if (text[0] == '#') {
        text++;
    } else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text += 2;
    }
    if (strlen(text) != 6) {
        return -1;
    }
    char *end = NULL;
    const long v = strtol(text, &end, 16);
    return (end && *end == '\0') ? (int32_t)v : -1;
}

static esp_err_t execute_set_colors(const char *input_json,
                                    const claw_cap_call_context_t *ctx,
                                    char *output, size_t output_size)
{
    (void)ctx;
    if (!input_json || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: input must be a JSON object");
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *bg = cJSON_GetObjectItem(root, "background");
    const cJSON *eyes = cJSON_GetObjectItem(root, "eyes");
    const int32_t bg_rgb = cJSON_IsString(bg) ? parse_color(bg->valuestring) : -1;
    const int32_t eye_rgb = cJSON_IsString(eyes) ? parse_color(eyes->valuestring) : -1;
    const bool bg_bad = cJSON_IsString(bg) && bg->valuestring[0] && bg_rgb < 0;
    const bool eye_bad = cJSON_IsString(eyes) && eyes->valuestring[0] && eye_rgb < 0;
    cJSON_Delete(root);

    if (bg_bad || eye_bad) {
        snprintf(output, output_size,
                 "Error: colours are a name (black, white, red, green, blue, yellow, "
                 "orange, purple, pink, cyan, grey) or hex like #00ff00.");
        return ESP_ERR_INVALID_ARG;
    }
    if (bg_rgb < 0 && eye_rgb < 0) {
        snprintf(output, output_size, "Error: give 'background' and/or 'eyes'.");
        return ESP_ERR_INVALID_ARG;
    }
    if (ensure_session() != ESP_OK || cap_display_face_set_colors(bg_rgb, eye_rgb) != ESP_OK) {
        snprintf(output, output_size, "Error: the display is not available on this device.");
        return ESP_ERR_INVALID_STATE;
    }
    /* Make sure the face is what is on screen, so the change is visible. */
    if (display_service_lock() == ESP_OK) {
        if (s_text_timer) {
            lv_timer_delete(s_text_timer);
            s_text_timer = NULL;
        }
        (void)show_face_locked();
        display_service_unlock();
    }
    snprintf(output, output_size, "{\"ok\":true}");
    return ESP_OK;
}

/* ── Descriptors ───────────────────────────────────────────────────────── */

static const claw_cap_descriptor_t s_descriptors[] = {
    {
        .id = "display_show_text",
        .name = "display_show_text",
        .family = "display",
        .description =
            "Show a short TEXT message on the robot's screen, a few words, big. "
            "Text only: for a picture, drawing, shape or animation on the screen "
            "use ask_robot_agent instead.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{"
            "\"text\":{\"type\":\"string\",\"maxLength\":120}},"
            "\"required\":[\"text\"]}",
        .execute = execute_show_text,
    },
    {
        .id = "display_show_emotion",
        .name = "display_show_emotion",
        .family = "display",
        .description =
            "Show a face on the robot's screen: neutral, happy, excited, sad, "
            "sleepy, angry, surprised, love, confused, wink, dizzy, scared, pleading, "
            "determined, bliss, cool or curious. It holds until "
            "the next one; neutral hands the eyes back to the IMU. A reaction to what "
            "was said. For pictures or drawings use ask_robot_agent instead.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{"
            "\"emotion\":{\"type\":\"string\",\"enum\":[\"neutral\",\"happy\","
            "\"excited\",\"sad\",\"sleepy\",\"angry\",\"surprised\",\"love\","
            "\"confused\",\"wink\",\"dizzy\",\"scared\",\"pleading\",\"determined\","
            "\"bliss\",\"cool\",\"curious\"]}},\"required\":[\"emotion\"]}",
        .execute = execute_show_emotion,
    },
    {
        .id = "display_clear",
        .name = "display_clear",
        .family = "display",
        .description =
            "Stop showing a message and go back to the robot's resting face.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = execute_clear,
    },
    {
        .id = "display_set_colors",
        .name = "display_set_colors",
        .family = "display",
        .description =
            "Change the colours of the robot's face: the screen background "
            "and/or the eyes. Use this when someone asks to make the screen or "
            "background a colour, or to change the eye colour. Colour names "
            "(black, white, red, green, blue, yellow, orange, purple, pink, "
            "cyan, grey) or hex like #00ff00. The default is yellow eyes on "
            "black.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{"
            "\"background\":{\"type\":\"string\",\"description\":\"screen background colour\"},"
            "\"eyes\":{\"type\":\"string\",\"description\":\"eye colour\"}}}",
        .execute = execute_set_colors,
    },
    {
        .id = "display_set_brightness",
        .name = "display_set_brightness",
        .family = "display",
        .description =
            "Set the screen backlight from 0 to 100 percent. 0 turns it off, "
            "which is the polite thing to do when someone says the robot should "
            "sleep.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{"
            "\"percent\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100}},"
            "\"required\":[\"percent\"]}",
        .execute = execute_set_brightness,
    },
};

static const claw_cap_group_t s_group = {
    .group_id = "cap_display",
    .plugin_name = "Robot display",
    .version = "1",
    .descriptors = s_descriptors,
    .descriptor_count = sizeof(s_descriptors) / sizeof(s_descriptors[0]),
};

esp_err_t cap_display_register_group(void)
{
    if (claw_cap_group_exists(s_group.group_id)) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "registering %u display tools", (unsigned)s_group.descriptor_count);
    return claw_cap_register_group(&s_group);
}
