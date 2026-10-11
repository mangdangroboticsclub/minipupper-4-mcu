/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
/* The robot's screen. Ported from mpv2-claw's cap_display component with
 * the ESP-Claw agent-tool layer removed; see cap_display.c for what changed.
 *
 * Boot order (main.cc): cap_display_service_start() brings the panel and
 * LVGL up (display_hw.h), cap_display_face_start() draws the eyes and lights
 * the backlight, cap_display_anim_restore() brings back a looping GIF. */

/* True when the panel is up. False in a build without the display, where
 * every display call is a no-op -- check this before doing work whose only
 * purpose is to put something on the panel. */
bool cap_display_available(void);

/* Panel + LVGL. Safe to call with the display compiled out: it logs and
 * returns the error, which callers ignore. */
esp_err_t cap_display_service_start(void);

/** @brief Put the robot's face on the panel and start it blinking.
 *  A build with no display is not an error; this returns ESP_OK. */
esp_err_t cap_display_face_start(void);

/* ── Operations (mpv2-claw's agent tools, as plain functions) ────────────
 * For the HTTP routes now, and for Lua or an on-device agent later. */

/** A short message, big, for 8 s; then the face comes back. */
esp_err_t cap_display_show_text(const char *text);
/** An expression by name (neutral, happy, ...); ESP_ERR_NOT_FOUND if unknown.
 *  Holds until the next one; "neutral" hands the eyes back to the IMU. */
esp_err_t cap_display_show_emotion(const char *name);
/** Drop a message and go back to the face. */
esp_err_t cap_display_clear(void);
/** Backlight 0..100; 5 and up is remembered in NVS for the next boot. */
esp_err_t cap_display_set_brightness(int percent);
int       cap_display_brightness(void);
/** Face colours as 0xRRGGBB, -1 = leave alone. */
esp_err_t cap_display_set_colors(int32_t background_rgb, int32_t eyes_rgb);
/** "green", "#00ff00", "00FF00" or "0x00ff00" -> 0xRRGGBB; -1 if unreadable. */
int32_t   cap_display_parse_color(const char *text);

/** @brief Update the dim line under the eyes.
 *
 *  In AP mode the panel is the only thing that can tell someone which
 *  network to join, since they have not joined it yet. */
void cap_display_face_set_network(bool sta_connected, const char *ap_ssid, const char *ip);

/** The Raspberry Pi's address, shown after the robot's under the eyes;
 *  NULL or "" when there is no Pi. */
void cap_display_face_set_pi(const char *ip);

/** "ROS" in front of the network line while the Raspberry Pi (ROS) drives
 *  the robot (mpx_pi_link slave mode). Takes the LVGL lock: call from a task
 *  that may block, not from the Pi link task. */
void cap_display_set_ros_mode(bool on);

/** A camera picture from the Raspberry Pi: a baseline JPEG up to 320x240,
 *  decoded and shown full screen; the face comes back 1.5 s after the last
 *  frame. The sink mpx_pi_link_set_video_sink() wants. Call from a task, not
 *  from inside LVGL. ESP_ERR_NOT_SUPPORTED if the JPEG decoder is not in
 *  the build. */
esp_err_t cap_display_show_jpeg(const uint8_t *jpeg, size_t len);

/* ── Free drawing (the agent's display_draw tool) ────────────────────────
 * mpv2-claw lets its agent draw anything ("draw a crab") by writing a Lua
 * script against lua_module_lvgl. This firmware has no agent-side Lua, so
 * the same job is one call with a list of primitives, rendered onto a
 * 320x240 canvas (PSRAM) that replaces the face until hold_ms runs out
 * (0 = until display_clear / an emotion / text). Coordinates are panel
 * pixels, (0,0) top left. */
typedef enum {
    CAP_DRAW_RECT,      /*!< x,y,w,h box; r corner radius */
    CAP_DRAW_CIRCLE,    /*!< centre x,y; w,h = 2*radii (equal for a circle) */
    CAP_DRAW_LINE,      /*!< p[0] -> p[1], width thick */
    CAP_DRAW_TRIANGLE,  /*!< p[0..2], filled */
    CAP_DRAW_ARC,       /*!< centre x,y; r radius; start..end degrees, 0 = 3 o'clock, clockwise */
    CAP_DRAW_TEXT,      /*!< centred on x,y; size 0 small, 1 medium, 2 large */
} cap_draw_kind_t;

typedef struct {
    cap_draw_kind_t kind;
    uint32_t color;           /*!< 0xRRGGBB: fill, stroke or text colour */
    int32_t  border_color;    /*!< rect/circle outline 0xRRGGBB, -1 none */
    int16_t  width;           /*!< line/arc thickness, outline width */
    bool     filled;          /*!< rect/circle: false = outline only */
    int16_t  x, y, w, h, r;
    int16_t  p[3][2];
    int16_t  start, end;
    uint8_t  size;
    char     text[64];
} cap_draw_shape_t;

#define CAP_DRAW_MAX_SHAPES 200

esp_err_t cap_display_draw(const cap_draw_shape_t *shapes, size_t count,
                           uint32_t background_rgb, uint32_t hold_ms);

/* Painting the same canvas one primitive at a time, for Lua's `display`
 * module (lua/lua_display.cc): paint it, then show it. Colours are
 * 0xRRGGBB, coordinates are pixels from the top left, anything off the
 * canvas is clipped. Painting does not change what is on the panel;
 * cap_display_draw_show() does. The canvas keeps its contents between
 * drawings (cap_display_draw() repaints all of it), so start one with a
 * fill.
 *
 * Every call: ESP_ERR_INVALID_STATE with no panel, ESP_ERR_TIMEOUT if the
 * LVGL lock could not be had, ESP_ERR_INVALID_ARG for a size <= 0. Call
 * from a task, not from inside LVGL. */
int       cap_display_draw_width(void);    /*!< 0 with no panel */
int       cap_display_draw_height(void);
esp_err_t cap_display_draw_fill(uint32_t rgb);
esp_err_t cap_display_draw_pixel(int x, int y, uint32_t rgb);
/** radius < 0 = fully round ends; border > 0 = outline that thick, 0 = filled. */
esp_err_t cap_display_draw_rect(int x, int y, int w, int h, uint32_t rgb, int radius, int border);
esp_err_t cap_display_draw_circle(int cx, int cy, int r, uint32_t rgb, int border);
esp_err_t cap_display_draw_line(int x1, int y1, int x2, int y2, uint32_t rgb, int width);
/** Degrees, 0 = three o'clock, increasing clockwise; end < start wraps. */
esp_err_t cap_display_draw_arc(int cx, int cy, int r, int start_deg, int end_deg, uint32_t rgb, int width);
/** Text with its box's top-left at x,y, wrapped to max_w (<= 0: to the right
 *  edge). size picks the nearest built-in font: 14, 28 or 48. align is
 *  -1 left, 0 centred, 1 right within the box. */
esp_err_t cap_display_draw_text(int x, int y, const char *text, uint32_t rgb, int size, int align, int max_w);
/** w*h RGB565 pixels, little-endian (2 bytes each, row by row). */
esp_err_t cap_display_draw_blit(int x, int y, int w, int h, const uint8_t *rgb565_le, size_t len);
/** Put the canvas on the panel, as cap_display_draw() does: the face comes
 *  back after hold_ms unless something else has taken the screen by then;
 *  0 holds until something does. Calling it again restarts the hold. */
esp_err_t cap_display_draw_show(uint32_t hold_ms);

/** @brief The conversation on the face -- targets for mpx_voice_set_ui_hooks().
 *
 *  heard/saying put a one-line subtitle under the eyes (the network line
 *  comes back by itself once the reply is over and the robot goes quiet);
 *  emotion maps the server's xiaozhi emotion name onto the face's
 *  expressions. All safe to call from any task and on a board with no panel
 *  (they do nothing there). */
void cap_display_voice_heard(const char *text);
void cap_display_voice_saying(const char *text);
void cap_display_voice_emotion(const char *name);
void cap_display_voice_reply_done(void);
void cap_display_voice_session_ended(void);

/** @brief Draw a bring-up test pattern, hold it, then restore the face.
 *
 *  Corner labels, RGB bars and an edge frame -- enough to judge mirror_x,
 *  swap_xy, invert_color and any panel offset by eye, which is the only way
 *  those can be checked. Blocks for @p hold_ms.
 *
 *  @return ESP_ERR_NOT_SUPPORTED when the board has no panel. */
esp_err_t cap_display_show_test_pattern(uint32_t hold_ms);

/* Orientation (cap_display_orient.c). Turned 180 degrees by default
 * (CONFIG_APP_DISPLAY_FLIP180_DEFAULT); the choice is kept in NVS. mpv2-claw
 * also had a `display` console command here; mpx-dog has no console, so the
 * app's /v1/display route carries flip180/mirror/shift instead. */
esp_err_t cap_display_apply_orientation(void);
esp_err_t cap_display_set_orientation(bool flip180, bool mirror);
void cap_display_get_orientation(bool *flip180, bool *mirror);
/** Picture shift in pixels (+x right, +y down), for a panel whose glass does
 *  not sit exactly on the controller's RAM window. Saved in NVS. */
esp_err_t cap_display_set_shift(int x, int y);
void cap_display_get_shift(int *x, int *y);

/** Does a conversation change the face (subtitles, expressions)? Off by
 *  default; `display talk on|off`, saved in NVS. */
bool cap_display_talk_face(void);
void cap_display_set_talk_face(bool on);

/* ── Alive eyes (cap_display_face.c) ──────────────────────────────────────
 * The eyes follow the head and react to the body: they need the IMU, handed
 * over as a callback so this component does not depend on the robot one. */

/** One IMU sample in the sensor frame as mounted: +x left, +y forward,
 *  +z down (standing still reads az = -1 g). */
typedef struct {
    float ax, ay, az;   /*!< g   */
    float gx, gy, gz;   /*!< dps */
    bool  busy;         /*!< the robot is moving on its own (walking, a dance):
                             its bumps are not someone picking it up */
    bool  ok;           /*!< a real sample */
    const char *move;   /*!< the move playing now ("cower", "wakeup", ...) or
                             NULL: a move with a face track plays it */
    uint32_t move_seq;  /*!< changes every time a move starts, so the same
                             move twice in a row restarts its face */
} cap_display_motion_t;

typedef bool (*cap_display_motion_fn)(cap_display_motion_t *out);

/** Called ~30 times a second from the LVGL task, with the display lock held:
 *  keep it quick and non-blocking. NULL = no IMU (classic eyes). */
void cap_display_face_set_motion_source(cap_display_motion_fn fn);

/** On (default): gaze follows the head, glances, IMU moods, sleep.
 *  Off: the classic face -- blinks and expressions only. `display eyes`. */
void cap_display_face_set_alive(bool on);
bool cap_display_face_alive(void);
void cap_display_set_eyes_alive(bool on);           /*!< ... and save it */

/** Flip the gaze if the IMU is mounted the other way: bit0 pitch, bit1 roll. */
void    cap_display_face_set_invert(uint8_t bits);
uint8_t cap_display_face_invert(void);
void    cap_display_set_eyes_invert(uint8_t bits);  /*!< ... and save it */

/** Someone is talking to the robot: wake up (quietly) if it was dozing. */
void cap_display_face_poke(void);

/** One line: mood, pitch, roll, how long it has been still. */
size_t cap_display_face_describe(char *out, size_t out_size);

/* ── Animations (the app's Display page) ─────────────────────────────────
 * A GIF from <storage>/display/<id>.gif shown full screen instead of the
 * eyes. Loop keeps it up (and brings it back after a reboot); once plays it
 * one time, then the eyes return. Ids are [A-Za-z0-9_-], under 32 chars. */
typedef struct {
    bool on;            /*!< an animation owns the screen right now */
    bool loop;
    char id[32];        /*!< playing, or the last one chosen */
} cap_display_anim_state_t;

void        cap_display_anim_set_dir(const char *dir);   /*!< default "/fs/display" */
const char *cap_display_anim_dir(void);
bool        cap_display_anim_id_valid(const char *id);
esp_err_t   cap_display_anim_play(const char *id, bool loop);  /*!< ESP_ERR_NOT_FOUND: no such file */
esp_err_t   cap_display_anim_stop(void);
void        cap_display_anim_get(cap_display_anim_state_t *out);
esp_err_t   cap_display_anim_restore(void);              /*!< at boot, after the face starts */
#ifdef __cplusplus
}
#endif
