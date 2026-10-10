/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * The screen: the face, a text message, the Pi's camera picture.
 *
 * Ported from mpv2-claw's cap_display component. There the same code was
 * also a set of agent tools (display_show_text, display_show_emotion, ...)
 * on ESP-Claw's claw_cap; the tool descriptors are gone and the operations
 * are plain functions (cap_display_show_text, cap_display_set_brightness,
 * cap_display_set_colors) for the HTTP routes and, later, Lua or an agent.
 * The function names keep the cap_display_ prefix on purpose: every file in
 * this folder diffs cleanly against mpv2-claw's components/cap_display.
 *
 * Emotions are a drawn face, not bitmaps and not an emoji font. Both of those
 * cost flash for something whose every frame is two rounded rectangles; see
 * cap_display_face.c, which owns the geometry and the animation. The face is
 * this robot's home screen: showing a message borrows the screen and gives it
 * back, and clearing returns to the face rather than to a blank panel that
 * looks like a crash.
 *
 * mpv2-claw drew through ESP-Claw's display_service (sessions, a default
 * screen, Lua drawings holding the panel exclusively). mpx-dog has one
 * client for the panel, so display_hw.h is the whole service: a lock and
 * the panel handle. Everywhere the original asked "does someone else hold
 * the panel", the answer here is no.
 */

#include "cap_display.h"
#include "cap_display_face.h"
#include "display_hw.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cap_display";

#define CAP_DISPLAY_TEXT_MAX 128

#define CAP_DISPLAY_TEXT_HOLD_MS 8000

static bool        s_backlight_on;
static int         s_brightness = 80; /* percent; NVS "display"/"bright" */
static bool        s_talk_face;       /* NVS "display"/"talk", default off */
static lv_obj_t   *s_text_screen;
static lv_obj_t   *s_label;
static lv_timer_t *s_text_timer;

static void cancel_text_locked(void);

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
    return display_hw_available() ? ESP_OK : ESP_ERR_INVALID_STATE;
}

/* Put `screen` on the panel. Caller holds the LVGL lock. */
static esp_err_t load_screen_locked(lv_obj_t *screen)
{
    if (!screen) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!display_hw_available()) {
        return ESP_ERR_INVALID_STATE;
    }
    lv_screen_load(screen);
    return ESP_OK;
}

/* ── Backlight ─────────────────────────────────────────────────────────────
 *
 * display_hw powers the backlight up at 0%: the ST7789 comes out of reset
 * with random RAM that reads as white, and lighting it then is the white
 * flash at boot. It goes on once the eyes are on the panel, at the
 * brightness last chosen (cap_display_set_brightness, kept in NVS). */
static esp_err_t backlight_apply(int percent)
{
    return display_hw_backlight(percent);
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
    if (display_hw_lock() != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_text_screen == NULL) {
        s_text_screen = lv_obj_create(NULL);
        if (s_text_screen == NULL) {
            display_hw_unlock();
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
    display_hw_unlock();
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
    if (display_hw_lock() != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }

    screen = lv_obj_create(NULL);
    if (!screen) {
        display_hw_unlock();
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
    display_hw_unlock();
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(hold_ms));

    /* Put the face back and drop the pattern. Deleting the screen only after
     * another one is loaded, because deleting the active screen is what LVGL
     * documents as undefined. */
    if (display_hw_lock() == ESP_OK) {
        (void)show_face_locked();
        lv_obj_delete(screen);
        display_hw_unlock();
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

    if (display_hw_lock() != ESP_OK) {
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
    display_hw_unlock();
    return err;
#endif
}

/* ── Free drawing (see cap_display.h) ──────────────────────────────────────
 *
 * One screen with one canvas whose 150 KB RGB565 buffer is taken from PSRAM
 * the first time something is drawn and kept. Every call repaints the whole
 * canvas: the agent describes a picture, not edits to the last one. */

#define CAP_DRAW_W 320
#define CAP_DRAW_H 240

static lv_obj_t   *s_draw_screen;
static lv_obj_t   *s_draw_canvas;
static uint16_t   *s_draw_buf;
static lv_timer_t *s_draw_timer;

static void draw_expired_cb(lv_timer_t *timer)
{
    (void)timer;
    /* Inside LVGL: the lock is held. A one-shot timer deletes itself. */
    s_draw_timer = NULL;
    if (lv_screen_active() == s_draw_screen) {
        (void)show_face_locked();
    }
}

static const lv_font_t *draw_font(uint8_t size)
{
#if LV_FONT_MONTSERRAT_48
    if (size >= 2) return &lv_font_montserrat_48;
#endif
#if LV_FONT_MONTSERRAT_28
    if (size >= 1) return &lv_font_montserrat_28;
#endif
    (void)size;
    return LV_FONT_DEFAULT;
}

static void draw_shape_locked(lv_layer_t *layer, const cap_draw_shape_t *sh)
{
    const lv_color_t c = lv_color_hex(sh->color);

    switch (sh->kind) {
    case CAP_DRAW_RECT:
    case CAP_DRAW_CIRCLE: {
        lv_draw_rect_dsc_t d;
        lv_area_t a;
        lv_draw_rect_dsc_init(&d);
        if (sh->kind == CAP_DRAW_RECT) {
            a.x1 = sh->x;
            a.y1 = sh->y;
            a.x2 = sh->x + (sh->w > 0 ? sh->w : 1) - 1;
            a.y2 = sh->y + (sh->h > 0 ? sh->h : 1) - 1;
            d.radius = sh->r > 0 ? sh->r : 0;
        } else {
            const int rx = sh->w > 1 ? sh->w / 2 : 1, ry = sh->h > 1 ? sh->h / 2 : 1;
            a.x1 = sh->x - rx;
            a.y1 = sh->y - ry;
            a.x2 = sh->x + rx;
            a.y2 = sh->y + ry;
            d.radius = LV_RADIUS_CIRCLE;
        }
        d.bg_color = c;
        d.bg_opa = sh->filled ? LV_OPA_COVER : LV_OPA_TRANSP;
        if (sh->border_color >= 0 || !sh->filled) {
            d.border_color = sh->border_color >= 0 ? lv_color_hex((uint32_t)sh->border_color) : c;
            d.border_width = sh->width > 0 ? sh->width : 2;
            d.border_opa = LV_OPA_COVER;
        } else {
            d.border_width = 0;
        }
        lv_draw_rect(layer, &d, &a);
        break;
    }
    case CAP_DRAW_LINE: {
        lv_draw_line_dsc_t d;
        lv_draw_line_dsc_init(&d);
        d.p1.x = sh->p[0][0];
        d.p1.y = sh->p[0][1];
        d.p2.x = sh->p[1][0];
        d.p2.y = sh->p[1][1];
        d.color = c;
        d.width = sh->width > 0 ? sh->width : 3;
        d.opa = LV_OPA_COVER;
        d.round_start = 1;
        d.round_end = 1;
        lv_draw_line(layer, &d);
        break;
    }
    case CAP_DRAW_TRIANGLE: {
        lv_draw_triangle_dsc_t d;
        lv_draw_triangle_dsc_init(&d);
        for (int i = 0; i < 3; i++) {
            d.p[i].x = sh->p[i][0];
            d.p[i].y = sh->p[i][1];
        }
        d.color = c;
        d.opa = LV_OPA_COVER;
        lv_draw_triangle(layer, &d);
        break;
    }
    case CAP_DRAW_ARC: {
        lv_draw_arc_dsc_t d;
        lv_draw_arc_dsc_init(&d);
        d.center.x = sh->x;
        d.center.y = sh->y;
        d.radius = (uint16_t)(sh->r > 0 ? sh->r : 1);
        d.start_angle = sh->start;
        d.end_angle = sh->end;
        d.width = sh->width > 0 ? sh->width : 3;
        d.color = c;
        d.opa = LV_OPA_COVER;
        d.rounded = 1;
        lv_draw_arc(layer, &d);
        break;
    }
    case CAP_DRAW_TEXT: {
        lv_draw_label_dsc_t d;
        lv_area_t a;
        lv_draw_label_dsc_init(&d);
        d.font = draw_font(sh->size);
        d.color = c;
        d.opa = LV_OPA_COVER;
        d.text = sh->text;
        d.text_local = 1;
        d.align = LV_TEXT_ALIGN_CENTER;
        const int32_t lh = lv_font_get_line_height(d.font);
        a.x1 = sh->x - CAP_DRAW_W / 2;
        a.x2 = sh->x + CAP_DRAW_W / 2 - 1;
        a.y1 = sh->y - lh / 2;
        a.y2 = sh->y + lh;
        lv_draw_label(layer, &d, &a);
        break;
    }
    }
}

esp_err_t cap_display_draw(const cap_draw_shape_t *shapes, size_t count,
                           uint32_t background_rgb, uint32_t hold_ms)
{
    if (ensure_session() != ESP_OK) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (count > CAP_DRAW_MAX_SHAPES || (count && !shapes)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_draw_buf) {
        s_draw_buf = heap_caps_malloc(CAP_DRAW_W * CAP_DRAW_H * sizeof(uint16_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_draw_buf) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (display_hw_lock() != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_draw_screen) {
        s_draw_screen = lv_obj_create(NULL);
        if (!s_draw_screen) {
            display_hw_unlock();
            return ESP_ERR_NO_MEM;
        }
        lv_obj_set_style_bg_color(s_draw_screen, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_draw_screen, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_pad_all(s_draw_screen, 0, LV_PART_MAIN);
        lv_obj_remove_flag(s_draw_screen, LV_OBJ_FLAG_SCROLLABLE);
        s_draw_canvas = lv_canvas_create(s_draw_screen);
        lv_canvas_set_buffer(s_draw_canvas, s_draw_buf, CAP_DRAW_W, CAP_DRAW_H,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(s_draw_canvas, 0, 0);
    }

    lv_canvas_fill_bg(s_draw_canvas, lv_color_hex(background_rgb), LV_OPA_COVER);
    lv_layer_t layer;
    lv_canvas_init_layer(s_draw_canvas, &layer);
    for (size_t i = 0; i < count; i++) {
        draw_shape_locked(&layer, &shapes[i]);
    }
    lv_canvas_finish_layer(s_draw_canvas, &layer);

    cancel_text_locked();
    if (s_draw_timer) {
        lv_timer_delete(s_draw_timer);
        s_draw_timer = NULL;
    }
    const esp_err_t err = load_screen_locked(s_draw_screen);
    if (err == ESP_OK && hold_ms) {
        s_draw_timer = lv_timer_create(draw_expired_cb, hold_ms, NULL);
        if (s_draw_timer) {
            lv_timer_set_repeat_count(s_draw_timer, 1);
        }
    }
    display_hw_unlock();
    return err;
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
    return display_hw_available();
}

/* Always: display_hw gives LVGL its two draw buffers in internal DMA RAM
 * (esp_lvgl_port, buff_dma), so a flush never allocates. The face code
 * keeps asking because on mpv2-claw the answer depended on the board. */
bool cap_display_draw_bufs_internal(void)
{
    return true;
}

esp_err_t cap_display_service_start(void)
{
    const esp_err_t err = display_hw_init();
    if (err != ESP_OK) {
        /* Not fatal: a build without the panel reports ESP_ERR_NOT_SUPPORTED,
         * and the face already treats "no display" as a normal configuration. */
        ESP_LOGI(TAG, "display not started: %s", esp_err_to_name(err));
        return err;
    }
    (void)cap_display_apply_orientation();
    return ESP_OK;
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
    if (display_hw_lock() != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    err = show_face_locked();
    if (err == ESP_OK && !s_backlight_on) {
        /* Draw the eyes now, then light the panel: no white flash. */
        lv_refr_now(NULL);
    }
    display_hw_unlock();

    /* Lit even if the face could not be shown: a dark screen would look
     * like a dead robot. */
    if (!s_backlight_on) {
        s_backlight_on = backlight_apply(s_brightness) == ESP_OK;
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "face is up");
    }
    return err;
}
void cap_display_face_set_network(bool sta_connected, const char *ap_ssid, const char *ip)
{
    char line[64];

    /* Nothing to draw on, so do not reach for the lock. display_hw_lock()
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

/* ── Operations (mpv2-claw's agent tools, as plain functions) ──────────── */

esp_err_t cap_display_show_text(const char *text)
{
    char buf[CAP_DISPLAY_TEXT_MAX];

    if (!text || !text[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(buf, sizeof(buf), "%s", text);
    return paint_text(buf);
}

/* Drop a message still on screen: a reaction to what was just said should
 * be visible now, not in eight seconds when the text times out. Caller
 * holds the LVGL lock. */
static void cancel_text_locked(void)
{
    if (s_text_timer) {
        lv_timer_delete(s_text_timer);
        s_text_timer = NULL;
    }
}

esp_err_t cap_display_show_emotion(const char *name)
{
    esp_err_t err;

    if (!cap_display_face_is_emotion(name)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (ensure_session() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    err = cap_display_face_set_emotion(name);
    if (err != ESP_OK) {
        return err;
    }
    if (display_hw_lock() == ESP_OK) {
        cancel_text_locked();
        err = show_face_locked();
        display_hw_unlock();
    }
    return err;
}

esp_err_t cap_display_clear(void)
{
    /* Back to the face, which is this robot's home screen. */
    if (display_hw_lock() == ESP_OK) {
        cancel_text_locked();
        (void)show_face_locked();
        display_hw_unlock();
    }
    return ESP_OK;
}

esp_err_t cap_display_set_brightness(int percent)
{
    esp_err_t err;

    if (percent < 0)   { percent = 0; }
    if (percent > 100) { percent = 100; }
    err = backlight_apply(percent);
    if (err != ESP_OK) {
        return err;
    }
    /* Remembered, so the robot wakes up at the brightness it was given. A
     * request for (near) darkness is not, or it would boot to a black screen. */
    if (percent >= 5) {
        s_brightness = percent;
        bright_save_later();
    }
    s_backlight_on = true;
    return ESP_OK;
}

int cap_display_brightness(void)
{
    return s_brightness;
}

/* "green", "#00ff00", "00FF00" or "0x00ff00" -> 0xRRGGBB; -1 if unreadable. */
int32_t cap_display_parse_color(const char *text)
{
    static const struct { const char *name; uint32_t rgb; } named[] = {
        { "black",  0x000000 }, { "white",  0xFFFFFF }, { "red",    0xFF0000 },
        { "green",  0x00C000 }, { "blue",   0x0040FF }, { "yellow", 0xFFE631 },
        { "orange", 0xFF8000 }, { "purple", 0x8000FF }, { "pink",   0xFF4D8D },
        { "cyan",   0x00E0E0 }, { "grey",   0x808080 }, { "gray",   0x808080 },
        { "brown",  0x8B4513 }, { "navy",   0x000080 }, { "lime",   0x80FF00 },
        { "magenta",0xFF00FF }, { "gold",   0xFFC000 }, { "silver", 0xC0C0C0 },
        { "darkred",0x8B0000 }, { "skyblue",0x87CEEB }, { "beige",  0xF5DEB3 },
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

esp_err_t cap_display_set_colors(int32_t background_rgb, int32_t eyes_rgb)
{
    if (background_rgb < 0 && eyes_rgb < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ensure_session() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = cap_display_face_set_colors(background_rgb, eyes_rgb);
    if (err != ESP_OK) {
        return err;
    }
    /* Make sure the face is what is on screen, so the change is visible. */
    if (display_hw_lock() == ESP_OK) {
        cancel_text_locked();
        (void)show_face_locked();
        display_hw_unlock();
    }
    return ESP_OK;
}
