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
/** @brief Register the "cap_display" capability group. Idempotent. */
esp_err_t cap_display_register_group(void);

/** @brief Put the robot's face on the panel and start it blinking.
 *
 *  Call once after the display service is up (that is, after
 *  app_claw_ui_start()). The face becomes the resting screen: this board
 *  has no touch controller, so ESP-Claw's launcher home screen cannot be
 *  operated on it and holding the panel costs nothing.
 *
 *  A board with no display is not an error; this returns ESP_OK. */
/* True when a panel was found and a display session is open. False on a board
 * with no screen, where every display call is a no-op -- check this before
 * doing work whose only purpose is to put something on the panel. */
bool cap_display_available(void);

/* Bring the display service up with this project's draw-buffer height.
 *
 * Must be called BEFORE app_claw_ui_start(), because the first caller of
 * display_service_start() fixes the buffer size for everyone. Safe to call on
 * a board with no panel: it logs and returns the error, which callers ignore.
 */
esp_err_t cap_display_service_start(void);

esp_err_t cap_display_face_start(void);

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
 * (CONFIG_MP4_DISPLAY_FLIP180_DEFAULT); the choice is kept in NVS. */
esp_err_t cap_display_apply_orientation(void);
esp_err_t cap_display_set_orientation(bool flip180, bool mirror);
void cap_display_get_orientation(bool *flip180, bool *mirror);
/** Registers the `display` console command. */
void cap_display_register_console(void);

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

void        cap_display_anim_set_dir(const char *dir);   /*!< default "/fatfs/display" */
const char *cap_display_anim_dir(void);
bool        cap_display_anim_id_valid(const char *id);
esp_err_t   cap_display_anim_play(const char *id, bool loop);  /*!< ESP_ERR_NOT_FOUND: no such file */
esp_err_t   cap_display_anim_stop(void);
void        cap_display_anim_get(cap_display_anim_state_t *out);
esp_err_t   cap_display_anim_restore(void);              /*!< at boot, after the face starts */
#ifdef __cplusplus
}
#endif
