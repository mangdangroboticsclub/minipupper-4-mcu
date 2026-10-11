/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * The robot's face: two eyes, drawn -- and alive.
 *
 * The reference animation for this is a 320x240 GIF -- two yellow rounded
 * rectangles on black, blinking twice over a 3.5 s loop. It is not stored as a
 * GIF. Every frame of it is two rectangles, so the honest representation is
 * two rectangles: LVGL objects whose size and place a tick drives. That is a
 * few KB of code and no assets, against roughly 150 KB of RAM for a decoded
 * frame buffer plus the file itself, and it buys something the GIF cannot
 * give -- the face becomes programmable, and it can react.
 *
 * Geometry is measured from that GIF rather than guessed:
 *
 *   eye            59 x 114 px, corner radius 8
 *   centres        (82, 120) and (236, 120) on a 320x240 panel
 *   colour         #FFE631 on black
 *   blink          ~110 ms closed, ~110 ms open, every 2.2-5.2 s
 *   squash         the eye widens ~15% as it closes
 *
 * ── Alive ─────────────────────────────────────────────────────────────────
 *
 * Designed first as an interactive prototype ("Pupper Alive Eyes"), then
 * ported here number for number. On top of the expression, every tick:
 *
 *   gaze      the eyes look where the head points: nose up -> eyes up, a side
 *             lowered -> eyes slide to that side. They ride a spring (slightly
 *             under-damped, so they overshoot a touch and settle), stretch a
 *             little along fast moves, and the eye on the far side of the turn
 *             shrinks a few percent -- a cheap 3-D head turn.
 *   glances   every 0.8-3.2 s a small random look-around, sometimes with a
 *             blink, the way real eyes blink on a big gaze shift.
 *   breath    a 1.5% height swell on a 4 s cycle. Invisible on its own, but a
 *             face without it reads as a picture.
 *   lids      looking down lowers the top of the eye (the bottom stays put).
 *
 * and the IMU picks a mood when something happens to the body:
 *
 *   shaken (gyro > 250 dps for 0.3 s)        dizzy: eyes orbit, opposite ways
 *   picked up / dropped (|a| off 1 g > 2 samples)  surprised
 *   a tap (one sharp sample, then still)     happy
 *   tilted past 38 deg (34 nose up/down)     whoa: wide, trembling
 *   upside down                              squeezed flat
 *   still 45 s / 2 min                       sleepy / asleep (dim, "z z")
 *   any real movement while asleep           a start, then awake
 *
 * An expression the agent or a skill asks for (set_emotion) holds until the
 * next one; the body reactions interrupt it briefly and it comes back. Gaze,
 * glances and blinks keep running underneath every expression.
 *
 * ── Shapes beyond a rectangle ────────────────────────────────────────────
 *
 * Two cheap cut-outs in the background colour give the rectangles a brow
 * and a smile (designed in the "Pupper Move Eyes" canvas, ported number for
 * number):
 *
 *   lid       a thick line laid across the top of each eye at a slant: deep
 *             at the outer corner reads worried / scared, deep at the inner
 *             corner reads angry, deep at both is half-lidded (cool, peek).
 *   cut       an ellipse pushed up from below the eye, so the bottom edge
 *             arches and the eye becomes an arc: happy, bliss.
 *
 * Both follow the eye (blinks scale them), and both are hidden at depth 0,
 * so the moods without them draw exactly what they always did.
 *
 * ── Move faces ────────────────────────────────────────────────────────────
 *
 * While the robot plays a move (Scared, Wake up, Push-ups ...), the move has
 * its own face track: a few timed steps of expression plus where to look,
 * started the moment the robot reports the move (via the motion source) and
 * dropped when it ends. A move face outranks everything except upside down;
 * the agent's expression comes back afterwards.
 *
 * The IMU arrives through a callback (cap_display_face_set_motion_source) so
 * this component does not depend on the robot one; main.c wires it. With no
 * source, or with `display eyes off`, the face is the classic one: blinks and
 * expressions, eyes centred.
 *
 * ── Why the face is the home screen ───────────────────────────────────────
 *
 * ESP-Claw's system_ui owns the panel by default: a launcher of tiles and a
 * task panel, both driven by touch. This board has no touch controller, so
 * that UI is not reachable on it. What system_ui did provide that mattered is
 * the network line, so the face carries that line itself, fed from main.c's
 * Wi-Fi state callback.
 */

#include "cap_display_face.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "libs/gif/AnimatedGIF.h"   /* LVGL's GIF decoder, used without lv_gif */
#include "misc/cache/lv_cache.h"      /* lv_image_cache_drop */
#include "nvs.h"

#include "cap_display.h"
#include "display_hw.h"

static const char *TAG = "cap_display_face";

#define FACE_EYE_DX          77      /* eye centre offset from screen centre */
#define FACE_EYE_COLOR       0xFFE631
#define FACE_LOVE_COLOR      0xFF4D8D
#define FACE_EYE_MIN_H       5       /* a fully closed eye is a line, not nothing */
#define FACE_TICK_MS         33      /* ~30 fps; only while the face is on screen */
#define FACE_BLINK_S         0.11f
#define FACE_SLEEPY_BLINK_S  0.32f
#define FACE_SQUASH          0.15f   /* width gain at full close */
#define FACE_STATUS_MAX      64
/* A flush of the 8-line draw buffer is 320 x 8 x 2 = 5 KB of DMA bounce
 * buffer; below this, skip drawing rather than fail every flush. */
#define FACE_MIN_DMA_BLOCK   (6 * 1024)
#define FACE_LID_W           64      /* lid line thickness: the deepest lid it can draw */
#define FACE_LID_EXT         16.0f   /* the lid runs this far past each side of the eye */

/* Gaze */
#define GAZE_X_PX            30.0f   /* full sideways look */
#define GAZE_Y_PX            24.0f   /* full up/down look */
#define GAZE_ROLL_DEG        40.0f   /* tilt that gives the full sideways look */
#define GAZE_PITCH_DEG       35.0f
#define GAZE_DEADBAND_DEG    3.0f    /* a robot standing a little crooked looks ahead */
#define SPRING_K             150.0f  /* "normal" liveliness in the prototype */
#define SPRING_C             13.0f

/* Body reactions */
#define WHOA_ROLL_DEG        38.0f
#define WHOA_PITCH_DEG       34.0f
#define SHAKE_DPS            250.0f
#define SHAKE_HOLD_S         0.30f
#define SPIKE_G              0.25f   /* |a| this far from 1 g: something happened */
#define TAP_G                0.45f   /* ... one sample this big, then still: a tap */
#define STILL_G              0.05f
#define STILL_DPS            8.0f
#define MOVE_G               0.10f   /* this much movement wakes it */
#define MOVE_DPS             20.0f
#define SLEEPY_S             45.0f
#define ASLEEP_S             120.0f

/* ── Moods ─────────────────────────────────────────────────────────────────
 *
 * A mood is the two eyes' size, corner radius and vertical offset, plus how
 * it is coloured and whether it blinks and glances. "Angry" is a low, hard,
 * squared-off eye rather than an angled brow -- a rotated rectangle would be
 * more literal and would cost a transform per frame for a difference nobody
 * standing two metres away can see.
 *
 * The public ones are what the agent, skills and the voice link can ask for;
 * the rest only the body can cause.
 */

typedef struct {
    int16_t w;
    int16_t h;
    int16_t r;
    int16_t dy;
    int16_t dx;     /* toward the middle of the face (both eyes mirror it) */
    int16_t li;     /* lid depth at the inner corner, px from the top */
    int16_t lo;     /* lid depth at the outer corner */
    int16_t cut;    /* how far the arc cut reaches up from the bottom */
} face_eye_t;

typedef struct {
    const char *name;
    face_eye_t  left;
    face_eye_t  right;
    uint32_t    color;      /* 0 = the eye colour (set_colors) */
    uint8_t     dim_pct;    /* 100 = full brightness */
    bool        blinks;
    bool        glances;
    bool        is_public;
    uint8_t     shiver10;   /* tremble, px x10 (0 = still) */
    uint8_t     glance_pct; /* time between glances, % of normal (0 = 100) */
    uint8_t     reach_pct;  /* how far a glance goes, % of normal (0 = 100) */
} face_mood_t;

/*                  w    h   r  dy  dx  li  lo cut */
#define EYE_NEUTRAL { 59, 114,  8,  0 }
#define EYE(w, h, r, dy, dx, li, lo, cut) { w, h, r, dy, dx, li, lo, cut }

enum {
    M_NEUTRAL = 0, M_HAPPY, M_EXCITED, M_SAD, M_SLEEPY, M_ANGRY, M_SURPRISED,
    M_LOVE, M_CONFUSED, M_WINK, M_DIZZY,
    M_SCARED, M_PLEADING, M_DETERMINED, M_BLISS, M_COOL, M_CURIOUS,
    M_PEEK, M_SQUEEZE, M_YAWN,
    M_WHOA, M_UPSIDE, M_ASLEEP, M_COUNT
};

#define EYES2(e) e, e

static const face_mood_t s_moods[M_COUNT] = {
    /*                                 left / right eye                          colour  dim blink glance public shiver glance% reach% */
    [M_NEUTRAL]    = { "neutral",    EYES2(EYE_NEUTRAL),                             0, 100, true,  true,  true  },
    [M_HAPPY]      = { "happy",      EYES2(EYE(62,  56, 28,  10, 0,  0,  0, 34)),    0, 100, true,  true,  true  },
    [M_EXCITED]    = { "excited",    EYES2(EYE(66, 126, 14,   0, 0,  0,  0,  0)),    0, 100, true,  true,  true  },
    [M_SAD]        = { "sad",        EYES2(EYE(60,  84, 22,  18, 0,  0, 24,  0)),    0, 100, true,  true,  true  },
    [M_SLEEPY]     = { "sleepy",     EYES2(EYE(64,  40, 14,  18, 0,  0,  0,  0)),    0,  75, true,  true,  true  },
    [M_ANGRY]      = { "angry",      EYES2(EYE(62,  92, 10,  -4, 0, 36,  4,  0)),    0, 100, true,  true,  true  },
    [M_SURPRISED]  = { "surprised",  EYES2(EYE(72, 132, 30,  -4, 0,  0,  0,  0)),    0, 100, false, false, true  },
    [M_LOVE]       = { "love",       EYES2(EYE(70, 120, 34,   0, 0,  0,  0,  0)), FACE_LOVE_COLOR, 100, true, true, true },
    [M_CONFUSED]   = { "confused",   EYE_NEUTRAL, EYE(59, 70, 18, 14, 0, 0, 0, 0),   0, 100, true,  true,  true  },
    [M_WINK]       = { "wink",       EYE_NEUTRAL, EYE(64, 12,  6,  2, 0, 0, 0, 0),   0, 100, false, false, true  },
    [M_DIZZY]      = { "dizzy",      EYES2(EYE(54,  96, 24,   0, 0,  0,  0,  0)),    0, 100, false, false, true  },
    /* Small, pulled in, outer corners drooping; trembles, eyes dart. */
    [M_SCARED]     = { "scared",     EYES2(EYE(50, 104, 16,   6, 8,  0, 26,  0)),    0, 100, false, true,  true, 14, 30, 140 },
    [M_PLEADING]   = { "pleading",   EYES2(EYE(68, 122, 30,   4, 0,  0, 18,  0)),    0, 100, true,  true,  true  },
    [M_DETERMINED] = { "determined", EYES2(EYE(62,  84, 10,   4, 0, 26,  8,  0)),    0, 100, true,  false, true  },
    [M_BLISS]      = { "bliss",      EYES2(EYE(64,  44, 22,   8, 0,  0,  0, 30)),    0, 100, false, false, true  },
    [M_COOL]       = { "cool",       EYES2(EYE(64, 110,  8,   4, 0, 50, 50,  0)),    0, 100, true,  true,  true,  0, 180 },
    [M_CURIOUS]    = { "curious",    EYE(64, 122, 12, -2, 0, 0, 0, 0),
                                     EYE(57,  96, 12,  6, 0, 18, 0, 0),              0, 100, true,  true,  true,  0, 0, 160 },
    /* Only the moves use these. */
    [M_PEEK]       = { "peek",       EYES2(EYE(54,  96, 14,   8, 0, 30, 30,  0)),    0, 100, true,  true,  false, 6, 50 },
    [M_SQUEEZE]    = { "squeeze",    EYES2(EYE(66,  12,  6,   6, 0,  0,  0,  0)),    0, 100, false, false, false, 18 },
    [M_YAWN]       = { "yawn",       EYES2(EYE(66,  10,  5,   6, 0,  0,  0,  0)),    0, 100, false, false, false },
    [M_WHOA]       = { "whoa",       EYES2(EYE(68, 128, 16,   0, 0,  0,  0,  0)),    0, 100, false, false, false },
    [M_UPSIDE]     = { "upside",     EYES2(EYE(64,  18,  8,   6, 0,  0,  0,  0)),    0, 100, false, false, false },
    [M_ASLEEP]     = { "asleep",     EYES2(EYE(64,   6,  3,  24, 0,  0,  0,  0)),    0,  45, false, false, false },
};

/* ── Move faces ────────────────────────────────────────────────────────────
 *
 * One step: from `t` seconds into the move, this mood, looking (x, y) px off
 * centre (+x = screen right, +y = down). Times are from the move tables
 * (where each move's sit, shake or tremble actually is). */

typedef struct {
    float  t;
    int8_t mood;
    int8_t lx, ly;
} face_step_t;

typedef struct {
    const char        *name;
    const face_step_t *steps;
    uint8_t            n;
} face_move_t;

#define STEPS(...) (const face_step_t[]){ __VA_ARGS__ }, \
                   sizeof((const face_step_t[]){ __VA_ARGS__ }) / sizeof(face_step_t)

static const face_move_t s_move_faces[] = {
    { "wakeup",     STEPS({ 0, M_ASLEEP, 0, 0 }, { 1.4f, M_SLEEPY, 0, 0 }, { 2.0f, M_YAWN, 0, 0 }, { 3.0f, M_SLEEPY, 0, 0 },
                          { 4.0f, M_YAWN, 0, 0 }, { 5.0f, M_SURPRISED, 0, 0 }, { 5.6f, M_NEUTRAL, 0, 0 },
                          { 7.4f, M_SQUEEZE, 0, 0 }, { 9.0f, M_HAPPY, 0, 0 }) },
    { "look",       STEPS({ 0, M_CURIOUS, 0, 12 }, { 2.3f, M_CURIOUS, -22, 0 }, { 3.7f, M_CURIOUS, 22, 0 },
                          { 5.0f, M_CONFUSED, 0, 0 }, { 6.0f, M_CURIOUS, 0, -10 }, { 7.6f, M_HAPPY, 0, 0 }) },
    { "bow",        STEPS({ 0, M_EXCITED, 0, 0 }, { 1.0f, M_HAPPY, 0, 0 }, { 4.4f, M_EXCITED, 0, 0 }, { 6.0f, M_HAPPY, 0, 0 }) },
    { "trot",       STEPS({ 0, M_HAPPY, 0, 0 }, { 1.5f, M_EXCITED, 0, 0 }, { 5.6f, M_HAPPY, 0, 0 }) },
    { "chase",      STEPS({ 0, M_EXCITED, 24, 0 }, { 1.8f, M_DETERMINED, 26, 4 }, { 5.4f, M_DIZZY, 0, 0 },
                          { 7.4f, M_HAPPY, 0, 0 }) },
    { "dig",        STEPS({ 0, M_CURIOUS, 0, 16 }, { 1.4f, M_DETERMINED, 0, 18 }, { 4.6f, M_CURIOUS, 0, 14 },
                          { 5.6f, M_HAPPY, 0, 0 }) },
    { "scratch",    STEPS({ 0, M_NEUTRAL, 0, 0 }, { 0.9f, M_BLISS, 0, 0 }, { 6.6f, M_HAPPY, 0, 0 }, { 7.9f, M_SQUEEZE, 0, 0 },
                          { 9.0f, M_HAPPY, 0, 0 }) },
    { "shake",      STEPS({ 0, M_NEUTRAL, 0, 0 }, { 1.4f, M_SQUEEZE, 0, 0 }, { 4.5f, M_HAPPY, 0, 0 }, { 5.4f, M_SQUEEZE, 0, 0 },
                          { 7.3f, M_BLISS, 0, 0 }, { 8.1f, M_HAPPY, 0, 0 }) },
    { "cower",      STEPS({ 0, M_SURPRISED, 0, 0 }, { 0.9f, M_SCARED, 0, 0 }, { 3.5f, M_PEEK, 20, 4 }, { 4.8f, M_PEEK, -20, 4 },
                          { 6.0f, M_SCARED, 0, 0 }, { 6.8f, M_SAD, 0, 0 }, { 7.8f, M_NEUTRAL, 0, 0 }) },
    { "paw",        STEPS({ 0, M_HAPPY, 0, 0 }, { 0.9f, M_PLEADING, 0, 0 }, { 2.4f, M_LOVE, 0, 0 }, { 5.8f, M_HAPPY, 0, 0 }) },
    { "tripod",     STEPS({ 0, M_PLEADING, 0, 0 }, { 1.2f, M_SAD, 0, 6 }, { 15.4f, M_HAPPY, 0, 0 }) },
    { "rearclap",   STEPS({ 0, M_EXCITED, 0, 0 }, { 4.6f, M_HAPPY, 0, 0 }) },
    { "situp",      STEPS({ 0, M_DETERMINED, 0, 0 }, { 1.3f, M_HAPPY, 0, 0 }, { 3.5f, M_EXCITED, 0, 0 }, { 6.0f, M_HAPPY, 0, 0 }) },
    { "surrender",  STEPS({ 0, M_SURPRISED, 0, 0 }, { 0.6f, M_SCARED, 0, 0 }, { 4.2f, M_SAD, 0, 0 }, { 6.4f, M_NEUTRAL, 0, 0 }) },
    { "runningman", STEPS({ 0, M_NEUTRAL, 0, 0 }, { 0.9f, M_COOL, 10, 0 }, { 7.9f, M_HAPPY, 0, 0 }) },
    { "moonwalk",   STEPS({ 0, M_NEUTRAL, 0, 0 }, { 0.9f, M_COOL, -10, 0 }, { 7.9f, M_HAPPY, 0, 0 }) },
    { "sixseven",   STEPS({ 0, M_HAPPY, 0, 0 }, { 1.5f, M_COOL, 0, 0 }, { 4.4f, M_EXCITED, 0, 0 }, { 5.4f, M_COOL, 0, 0 },
                          { 8.8f, M_HAPPY, 0, 0 }) },
    { "pushup",     STEPS({ 0, M_DETERMINED, 0, 8 }, { 6.4f, M_SQUEEZE, 0, 0 }, { 8.4f, M_SAD, 0, 0 }, { 9.4f, M_SQUEEZE, 0, 0 },
                          { 11.3f, M_HAPPY, 0, 0 }) },
    { "spin90",     STEPS({ 0, M_DETERMINED, 0, 0 }, { 0.9f, M_SURPRISED, 0, 0 }, { 1.6f, M_HAPPY, 0, 0 }) },
    { "spin90_max", STEPS({ 0, M_DETERMINED, 0, 0 }, { 0.9f, M_SURPRISED, 0, 0 }, { 1.6f, M_HAPPY, 0, 0 }) },
    { "jump",       STEPS({ 0, M_DETERMINED, 0, 0 }, { 0.7f, M_SURPRISED, 0, 0 }, { 1.6f, M_HAPPY, 0, 0 }) },
    { "jumpfwd",    STEPS({ 0, M_DETERMINED, 0, 0 }, { 0.7f, M_SURPRISED, 0, 0 }, { 1.6f, M_HAPPY, 0, 0 }) },
    { "longjump",   STEPS({ 0, M_DETERMINED, 0, 0 }, { 0.7f, M_SURPRISED, 0, 0 }, { 1.6f, M_HAPPY, 0, 0 }) },
    { "farjump",    STEPS({ 0, M_DETERMINED, 0, 0 }, { 0.7f, M_SURPRISED, 0, 0 }, { 1.6f, M_HAPPY, 0, 0 }) },
    { "climb",      STEPS({ 0, M_DETERMINED, 0, 10 }, { 44.0f, M_HAPPY, 0, 0 }) },
    { "twerk",      STEPS({ 0, M_BLISS, 0, 0 }) },
    { "stretch",    STEPS({ 0, M_BLISS, 0, 0 }) },
};

static const face_move_t *move_face_by_name(const char *name)
{
    if (!name || !name[0]) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(s_move_faces) / sizeof(s_move_faces[0]); i++) {
        if (strcmp(name, s_move_faces[i].name) == 0) {
            return &s_move_faces[i];
        }
    }
    return NULL;
}

/* ── State ─────────────────────────────────────────────────────────────── */

typedef struct {
    float w, h, r, dy, dx, li, lo, cut;
} shape_t;

typedef struct {
    int32_t x, y, w, h, r;
} drawn_t;

static lv_obj_t   *s_screen;
static lv_obj_t   *s_eye[2];
static lv_obj_t   *s_lid[2];                  /* a thick line in the background colour */
static lv_obj_t   *s_cut[2];                  /* an ellipse in the background colour */
static lv_point_precise_t s_lid_pts[2][2];
static lv_obj_t   *s_status;
static lv_obj_t   *s_zz;
static lv_timer_t *s_tick_timer;
static uint32_t    s_background = 0x000000;   /* survives a rebuild of the screen */
static uint32_t    s_eye_color  = FACE_EYE_COLOR;

/* inputs, written from other tasks (plain word-sized stores) */
static volatile int  s_override = M_NEUTRAL;  /* the asked-for expression */
static volatile bool s_alive = true;
static volatile uint8_t s_invert;              /* bit0 pitch, bit1 roll */
static volatile bool s_poke;                   /* someone talked to it: wake up */
static cap_display_motion_fn s_motion_fn;

/* animation, owned by the LVGL task */
static int64_t  s_last_us;
static float    s_t;                 /* seconds the face has been on screen */
static int      s_mood = M_NEUTRAL;
static int      s_event = -1;        /* body reaction playing, or -1 */
static float    s_event_until, s_event_len;
static shape_t  s_cur[2];
static float    s_rgb[3];
static float    s_gx, s_gy, s_vx, s_vy;      /* gaze (px) and its velocity */
static float    s_sx, s_sy, s_next_glance;   /* the current glance offset */
static float    s_blink_t = -1.0f, s_blink_len = FACE_BLINK_S;
static float    s_next_blink = 2.0f, s_double_at;
static bool     s_double;
static drawn_t  s_drawn[2];
static int32_t  s_drawn_lid[2][7];            /* x, y, then both ends relative, shown */
static int32_t  s_drawn_cut[2][5];            /* x, y, w, h, shown */
static uint32_t s_drawn_color = 0xFFFFFFFF;
static bool     s_zz_shown;

/* the move playing now (from the motion source), and where it is */
static const face_move_t *s_move;
static uint32_t s_move_seq;
static float    s_move_t0;
static float    s_look_x, s_look_y;          /* the move's look, px */

static bool anim_tick(void);                 /* the Display page's GIF, below */

/* IMU estimate (robot frame: x forward, y left, z up) */
static float    s_up[3] = { 0, 0, 1 };
static float    s_g0 = 1.0f;
static float    s_pitch, s_roll;     /* deg; pitch + = nose up, roll + = left side up */
static float    s_still_t, s_idle_t, s_shake_t;
static bool     s_spike_pending;
static float    s_spike_peak;
static float    s_last_raw[3];
static bool     s_imu_seen;

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static float smooth01(float x)
{
    x = clampf(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

static float frand(float lo, float hi)
{
    return lo + (hi - lo) * (float)(esp_random() & 0xFFFF) / 65535.0f;
}

static float deadband(float v, float band)
{
    if (v > band) {
        return v - band;
    }
    if (v < -band) {
        return v + band;
    }
    return 0.0f;
}

static void start_event(int mood, float seconds)
{
    s_event = mood;
    s_event_len = seconds;
    s_event_until = s_t + seconds;
    s_idle_t = 0.0f;
}

static void start_blink(float seconds)
{
    if (s_blink_t >= 0.0f) {
        return;
    }
    s_blink_t = 0.0f;
    s_blink_len = seconds;
    s_double = (esp_random() % 100) < 20;
}

/* ── The body ──────────────────────────────────────────────────────────── */

/* One tick of the estimator. Sensor frame as mounted (see the robot's
 * alive.h, checked with recordings): +x = LEFT, +y = FORWARD, +z = DOWN;
 * standing still reads az = -1 g. */
static void body_update(const cap_display_motion_t *m, float dt)
{
    const float a[3] = { m->ay, m->ax, -m->az };              /* fwd, left, up */
    const float wn = sqrtf(m->gx * m->gx + m->gy * m->gy + m->gz * m->gz);
    const float an = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    const bool fresh = m->ax != s_last_raw[0] || m->ay != s_last_raw[1] || m->az != s_last_raw[2];
    const int was = s_mood;

    if (an < 0.05f) {
        return;                                               /* no sample yet */
    }
    s_last_raw[0] = m->ax;
    s_last_raw[1] = m->ay;
    s_last_raw[2] = m->az;

    /* Which way is up: the accelerometer, trusted fast when it reads ~1 g
     * (not being shaken), slowly when it does not. */
    {
        const float tau = fabsf(an - s_g0) < 0.1f ? 0.15f : 1.0f;
        const float k = s_imu_seen ? clampf(dt / tau, 0.0f, 1.0f) : 1.0f;
        float un;

        for (int i = 0; i < 3; i++) {
            s_up[i] += (a[i] / an - s_up[i]) * k;
        }
        un = sqrtf(s_up[0] * s_up[0] + s_up[1] * s_up[1] + s_up[2] * s_up[2]);
        if (un < 1e-3f) {
            s_up[0] = s_up[1] = 0.0f;
            s_up[2] = un = 1.0f;
        }
        for (int i = 0; i < 3; i++) {
            s_up[i] /= un;
        }
        s_imu_seen = true;
    }
    s_pitch = atan2f(s_up[0], s_up[2]) * 57.29578f;
    s_roll  = atan2f(-s_up[1], s_up[2]) * 57.29578f;
    if (s_invert & 1) {
        s_pitch = -s_pitch;
    }
    if (s_invert & 2) {
        s_roll = -s_roll;
    }

    const float dev = an - s_g0;
    const bool still = fabsf(dev) < STILL_G && wn < STILL_DPS;

    /* Shaken: a lot of rotation for a while, not one bump. */
    if (wn > SHAKE_DPS) {
        s_shake_t += dt;
        if (s_shake_t > SHAKE_HOLD_S) {
            s_shake_t = 0.0f;
            start_event(M_DIZZY, 2.6f);
        }
    } else {
        s_shake_t = fmaxf(0.0f, s_shake_t - dt * 0.5f);
    }

    /* Lifted, dropped or tapped -- only when the robot is not making its own
     * bumps (walking, a dance), and judged once per new IMU sample (20 Hz). */
    if (fresh && !m->busy) {
        const bool big = fabsf(dev) > SPIKE_G;

        if (s_spike_pending) {
            if (big) {
                start_event(M_SURPRISED, 1.6f);          /* it kept going: held */
            } else if (s_spike_peak > TAP_G) {
                start_event(M_HAPPY, 1.8f);              /* one sharp knock: a pat */
            }
            s_spike_pending = false;
        } else if (big && s_still_t > 0.4f) {
            s_spike_pending = true;
            s_spike_peak = fabsf(dev);
        }
    }
    if (m->busy) {
        s_spike_pending = false;
    }

    s_still_t = still ? s_still_t + dt : 0.0f;
    if (still && fresh) {
        s_g0 += (an - s_g0) * clampf(dt / 2.0f, 0.0f, 1.0f);   /* this IMU's own 1 g */
    }

    /* Sleep: counts only true stillness; real movement wakes it. */
    if (m->busy) {
        s_idle_t = 0.0f;
    } else if (still) {
        s_idle_t += dt;
    } else if (fabsf(dev) > MOVE_G || wn > MOVE_DPS) {
        if ((was == M_ASLEEP || was == M_SLEEPY) && s_event < 0) {
            start_event(M_SURPRISED, 0.9f);              /* "oh! I'm up" */
        }
        s_idle_t = 0.0f;
    }
}

static int pick_mood(void)
{
    const int ov = s_override;

    if (s_event >= 0 && s_t >= s_event_until) {
        s_event = -1;
    }
    s_look_x = s_look_y = 0.0f;
    if (s_move) {
        /* The move's own face. It knows what it is doing to the body, so no
         * body reaction starts meanwhile and it never counts as idle; only
         * landing upside down beats it. */
        const float el = s_t - s_move_t0;
        const face_step_t *st = &s_move->steps[0];

        for (uint8_t i = 1; i < s_move->n; i++) {
            if (s_move->steps[i].t <= el) {
                st = &s_move->steps[i];
            }
        }
        s_event = -1;
        s_idle_t = 0.0f;
        if (!(s_alive && s_imu_seen && s_up[2] < -0.5f)) {
            s_look_x = st->lx;
            s_look_y = st->ly;
            return st->mood;
        }
    }
    if (!s_alive || !s_imu_seen) {
        return ov;
    }
    if (s_event >= 0) {
        return s_event;
    }
    if (s_up[2] < -0.5f) {
        return M_UPSIDE;
    }
    if (fabsf(s_roll) > WHOA_ROLL_DEG || fabsf(s_pitch) > WHOA_PITCH_DEG) {
        return M_WHOA;
    }
    if (ov != M_NEUTRAL) {
        return ov;
    }
    if (s_idle_t > ASLEEP_S) {
        return M_ASLEEP;
    }
    if (s_idle_t > SLEEPY_S) {
        return M_SLEEPY;
    }
    return M_NEUTRAL;
}

/* ── Drawing ───────────────────────────────────────────────────────────── */

static void draw_eye(int i, float x, float y, float w, float h, float r)
{
    drawn_t d = {
        .x = (int32_t)lroundf(x), .y = (int32_t)lroundf(y),
        .w = (int32_t)lroundf(w), .h = (int32_t)lroundf(h), .r = (int32_t)lroundf(r),
    };

    if (!s_eye[i]) {
        return;
    }
    if (d.w != s_drawn[i].w || d.h != s_drawn[i].h) {
        lv_obj_set_size(s_eye[i], d.w, d.h);
    }
    if (d.r != s_drawn[i].r) {
        lv_obj_set_style_radius(s_eye[i], d.r, LV_PART_MAIN);
    }
    if (d.x != s_drawn[i].x || d.y != s_drawn[i].y) {
        lv_obj_set_pos(s_eye[i], d.x, d.y);
    }
    s_drawn[i] = d;
}

static void show_obj(lv_obj_t *o, bool show)
{
    if (show) {
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

/* The lid: its lower edge runs from `dl` px below the eye's top at the left
 * to `dr` px at the right, carried on past both sides so the slanted ends
 * never leave a corner of the eye showing. A line of FACE_LID_W covers half
 * its width either side of its centre, so the centre sits that far above
 * the edge. The object sits at the line's own corner, so moving it only
 * redraws the small area around it. */
static void draw_lid(int i, float x, float y, float w, float dl, float dr)
{
    int32_t v[7] = { 0 };

    if (!s_lid[i]) {
        return;
    }
    v[6] = fmaxf(dl, dr) > 0.5f;
    if (v[6]) {
        const float slope = (dr - dl) / w, half = FACE_LID_W / 2.0f;
        float ax = x - FACE_LID_EXT, ay = y + dl - FACE_LID_EXT * slope;
        float bx = x + w + FACE_LID_EXT, by = y + dr + FACE_LID_EXT * slope;
        const float len = hypotf(bx - ax, by - ay);
        const float ux = (bx - ax) / len, uy = (by - ay) / len;

        ax += uy * half;                 /* (uy, -ux) points up, away from the eye */
        ay -= ux * half;
        bx += uy * half;
        by -= ux * half;
        v[0] = (int32_t)floorf(fminf(ax, bx));
        v[1] = (int32_t)floorf(fminf(ay, by));
        v[2] = (int32_t)lroundf(ax) - v[0];
        v[3] = (int32_t)lroundf(ay) - v[1];
        v[4] = (int32_t)lroundf(bx) - v[0];
        v[5] = (int32_t)lroundf(by) - v[1];
    }
    if (memcmp(v, s_drawn_lid[i], sizeof(v)) == 0) {
        return;
    }
    if (v[6] != s_drawn_lid[i][6]) {
        show_obj(s_lid[i], v[6]);
    }
    if (v[6]) {
        s_lid_pts[i][0].x = v[2];
        s_lid_pts[i][0].y = v[3];
        s_lid_pts[i][1].x = v[4];
        s_lid_pts[i][1].y = v[5];
        lv_obj_set_pos(s_lid[i], v[0], v[1]);
        lv_line_set_points(s_lid[i], s_lid_pts[i], 2);
    }
    memcpy(s_drawn_lid[i], v, sizeof(v));
}

/* The arc cut: an ellipse a little wider than the eye, its top `cut` px up
 * from the eye's bottom, so the bottom edge arches like ^. */
static void draw_cut(int i, float x, float y, float w, float h, float cut)
{
    int32_t v[5] = { 0 };

    if (!s_cut[i]) {
        return;
    }
    v[4] = cut > 0.5f;
    if (v[4]) {
        const float cw = w * 1.25f, ch = 2.0f * cut + 24.0f;

        v[0] = (int32_t)lroundf(x + w / 2.0f - cw / 2.0f);
        v[1] = (int32_t)lroundf(y + h - cut);
        v[2] = (int32_t)lroundf(cw);
        v[3] = (int32_t)lroundf(ch);
    }
    if (memcmp(v, s_drawn_cut[i], sizeof(v)) == 0) {
        return;
    }
    if (v[4] != s_drawn_cut[i][4]) {
        show_obj(s_cut[i], v[4]);
    }
    if (v[4]) {
        lv_obj_set_size(s_cut[i], v[2], v[3]);
        lv_obj_set_pos(s_cut[i], v[0], v[1]);
    }
    memcpy(s_drawn_cut[i], v, sizeof(v));
}

static void face_frame(float dt)
{
    const int mood = pick_mood();
    const face_mood_t *fm = &s_moods[mood];
    const bool alive = s_alive && s_imu_seen;

    s_mood = mood;

    /* Shape: ease toward the mood from wherever the eyes are now, so an
     * interruption half way through never snaps. */
    {
        const float k = 1.0f - expf(-dt * 14.0f);
        const face_eye_t *tg[2] = { &fm->left, &fm->right };

        for (int i = 0; i < 2; i++) {
            s_cur[i].w  += ((float)tg[i]->w  - s_cur[i].w)  * k;
            s_cur[i].h  += ((float)tg[i]->h  - s_cur[i].h)  * k;
            s_cur[i].r  += ((float)tg[i]->r  - s_cur[i].r)  * k;
            s_cur[i].dy += ((float)tg[i]->dy - s_cur[i].dy) * k;
            s_cur[i].dx += ((float)tg[i]->dx - s_cur[i].dx) * k;
            s_cur[i].li += ((float)tg[i]->li - s_cur[i].li) * k;
            s_cur[i].lo += ((float)tg[i]->lo - s_cur[i].lo) * k;
            s_cur[i].cut += ((float)tg[i]->cut - s_cur[i].cut) * k;
        }
    }

    /* Colour: the mood's own, or the eye colour, dimmed for sleep. */
    {
        const uint32_t base = fm->color ? fm->color : s_eye_color;
        const float dim = (float)fm->dim_pct / 100.0f;
        const float tgt[3] = {
            (float)((base >> 16) & 0xFF) * dim, (float)((base >> 8) & 0xFF) * dim,
            (float)(base & 0xFF) * dim,
        };
        const float k = 1.0f - expf(-dt * 8.0f);
        uint32_t c;

        for (int i = 0; i < 3; i++) {
            s_rgb[i] += (tgt[i] - s_rgb[i]) * k;
        }
        c = ((uint32_t)lroundf(s_rgb[0]) << 16) | ((uint32_t)lroundf(s_rgb[1]) << 8) |
            (uint32_t)lroundf(s_rgb[2]);
        if (c != s_drawn_color) {
            for (int i = 0; i < 2; i++) {
                if (s_eye[i]) {
                    lv_obj_set_style_bg_color(s_eye[i], lv_color_hex(c), LV_PART_MAIN);
                }
            }
            s_drawn_color = c;
        }
    }

    /* Gaze target: where the head points, plus the current glance. Seen
     * from in front, the robot's right is the viewer's left: right side down
     * (roll > 0, "left side up") -> the eyes slide to screen left. */
    {
        float range = 1.0f, tx = 0.0f, ty = 0.0f;

        /* Glances: also while a move plays without an IMU, since a scared
         * dart is part of the expression. Some moods glance more often
         * (glance_pct < 100) or further (reach_pct > 100). */
        if (alive || s_move) {
            if (s_t > s_next_glance) {
                if (fm->glances) {
                    const float reach = fm->reach_pct ? (float)fm->reach_pct / 100.0f : 1.0f;

                    if (esp_random() % 100 < 30) {
                        s_sx = s_sy = 0.0f;
                    } else {
                        s_sx = frand(-12.0f, 12.0f) * reach;
                        s_sy = frand(-7.0f, 7.0f) * reach;
                    }
                    if (fm->blinks && esp_random() % 100 < 22) {
                        start_blink(FACE_BLINK_S);
                    }
                } else {
                    s_sx = s_sy = 0.0f;
                }
                s_next_glance = s_t + frand(0.8f, 3.2f) *
                                (fm->glance_pct ? (float)fm->glance_pct / 100.0f : 1.0f);
            }
            range = mood == M_ASLEEP ? 0.1f : mood == M_SLEEPY ? 0.4f : mood == M_WHOA ? 1.25f : 1.0f;
            tx = s_sx;
            ty = s_sy;
        }
        if (alive) {
            tx += clampf(-deadband(s_roll, GAZE_DEADBAND_DEG) / GAZE_ROLL_DEG, -1, 1) * GAZE_X_PX;
            ty += -clampf(deadband(s_pitch, GAZE_DEADBAND_DEG) / GAZE_PITCH_DEG, -1, 1) * GAZE_Y_PX;
        }
        tx = (tx + s_look_x) * range;   /* where the move looks */
        ty = (ty + s_look_y) * range;
        s_vx += (SPRING_K * (tx - s_gx) - SPRING_C * s_vx) * dt;
        s_gx += s_vx * dt;
        s_vy += (SPRING_K * (ty - s_gy) - SPRING_C * s_vy) * dt;
        s_gy += s_vy * dt;
    }

    /* Blinks: a fresh random gap every time (a fixed period reads as a
     * metronome), one in five a double. */
    float open = 1.0f;
    if (s_t > s_next_blink) {
        if (fm->blinks) {
            start_blink(mood == M_SLEEPY ? FACE_SLEEPY_BLINK_S : FACE_BLINK_S);
        }
        s_next_blink = s_t + frand(2.2f, 5.2f) * (mood == M_SLEEPY ? 0.7f : 1.0f);
    }
    if (s_double_at > 0.0f && s_t > s_double_at) {
        s_double_at = 0.0f;
        start_blink(FACE_BLINK_S);
        s_double = false;
    }
    if (s_blink_t >= 0.0f) {
        const float ph = (s_blink_t += dt) / s_blink_len;

        if (ph < 1.0f) {
            open = 1.0f - smooth01(ph);
        } else if (ph < 2.0f) {
            open = smooth01(ph - 1.0f);
        } else {
            s_blink_t = -1.0f;
            if (s_double) {
                s_double = false;
                s_double_at = s_t + 0.12f;
            }
        }
    }

    /* Put it together. */
    {
        const int32_t sw = lv_obj_get_width(s_screen);
        const int32_t sh = lv_obj_get_height(s_screen);
        const float gn = clampf(s_gx / GAZE_X_PX, -1, 1);
        const float gyn = clampf(s_gy / GAZE_Y_PX, -1, 1);
        const float breath = alive ? 1.0f + 0.015f * sinf(s_t * 6.2831853f / 4.0f) : 1.0f;
        const float stx = fminf(fabsf(s_vx) / 900.0f, 0.15f);
        const float sty = fminf(fabsf(s_vy) / 900.0f, 0.15f);
        const float shiver = fmaxf((mood == M_WHOA || mood == M_UPSIDE) ? 1.2f : 0.0f,
                                   (float)fm->shiver10 / 10.0f);
        float orbit = 0.0f;

        if (mood == M_DIZZY && s_event == M_DIZZY && s_event_len > 0.0f) {
            orbit = 2.0f + 11.0f * clampf((s_event_until - s_t) / s_event_len, 0, 1);
        } else if (mood == M_DIZZY && s_move) {
            orbit = 8.0f;                         /* a move's dizzy spell: steady circles */
        }
        for (int i = 0; i < 2; i++) {
            const shape_t *c = &s_cur[i];
            const float persp = 1.0f + (i == 0 ? 1.0f : -1.0f) * 0.07f * gn;
            float w = c->w * persp * (1.0f + stx - sty * 0.5f);
            float h = c->h * persp * breath * (1.0f + sty - stx * 0.5f);
            float lid = 0.0f, ox = 0.0f, oy = 0.0f, r;

            if (gyn > 0.0f) {
                lid = h * 0.2f * gyn;           /* looking down: the lid comes down */
                h -= lid;
            } else {
                h *= 1.0f - 0.05f * gyn;        /* looking up: a touch wider open */
            }
            h *= open;
            if (h < FACE_EYE_MIN_H) {
                h = FACE_EYE_MIN_H;
            }
            w *= 1.0f + FACE_SQUASH * (1.0f - open);
            r = fminf(c->r, h / 2.0f);
            if (orbit > 0.0f) {
                const float ang = i == 0 ? s_t * 10.0f : -s_t * 10.0f + 3.14159f;
                ox = cosf(ang) * orbit;
                oy = sinf(ang) * orbit;
            }
            if (shiver > 0.0f) {
                ox += frand(-shiver, shiver);
                oy += frand(-shiver, shiver);
            }
            {
                /* dx pulls both eyes toward the middle; the left eye's inner
                 * corner is its right-hand side, so its lid depths swap. */
                const float side = i == 0 ? -1.0f : 1.0f;
                const float cx = (float)sw / 2.0f + side * (FACE_EYE_DX - c->dx) + s_gx + ox;
                const float cy = (float)sh / 2.0f + c->dy + s_gy + oy + lid * 0.5f;
                const float x = cx - w / 2.0f, y = cy - h / 2.0f;
                const float dl = (i == 0 ? c->lo : c->li) * open;
                const float dr = (i == 0 ? c->li : c->lo) * open;

                draw_eye(i, x, y, w, h, r);
                draw_cut(i, x, y, w, h, c->cut * open);
                draw_lid(i, x, y, w, dl, dr);
            }
        }
    }

    /* "z z" while asleep, breathing in and out. */
    if (s_zz) {
        const bool show = mood == M_ASLEEP;
        if (show != s_zz_shown) {
            if (show) {
                lv_obj_remove_flag(s_zz, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_zz, LV_OBJ_FLAG_HIDDEN);
            }
            s_zz_shown = show;
        }
        if (show) {
            lv_obj_set_style_opa(s_zz, (lv_opa_t)(130 + 100 * sinf(s_t * 1.6f)), LV_PART_MAIN);
        }
    }
}

static void face_tick_cb(lv_timer_t *timer)
{
    cap_display_motion_t m;
    int64_t now;
    float dt;

    (void)timer;
    /* Nothing to do while something else owns the panel (a drawing, the
     * test pattern, a Lua app): the face picks up where it was. */
    if (!s_screen || lv_screen_active() != s_screen) {
        s_last_us = 0;
        return;
    }
    /* An animation from the Display page covers the face: the eyes stop. */
    if (anim_tick()) {
        s_last_us = 0;
        return;
    }
    now = esp_timer_get_time();
    dt = s_last_us ? (float)(now - s_last_us) / 1e6f : (float)FACE_TICK_MS / 1000.0f;
    s_last_us = now;
    dt = clampf(dt, 0.001f, 0.1f);
    s_t += dt;

    if (s_poke) {
        s_poke = false;
        s_idle_t = 0.0f;                    /* talked to: awake, no start */
    }
    if (s_motion_fn) {
        memset(&m, 0, sizeof(m));
        if (s_motion_fn(&m)) {
            if (s_alive && m.ok) {
                body_update(&m, dt);
            }
            /* A new move (or the same one again) starts its face track from
             * now; a move without one, or none, drops it. */
            if (m.move_seq != s_move_seq) {
                s_move_seq = m.move_seq;
                s_move = move_face_by_name(m.move);
                s_move_t0 = s_t;
            }
        }
    }
    /* Every moved pixel goes to the panel through a DMA bounce buffer the
     * SPI driver allocates per flush, in internal RAM (the draw buffers are
     * in PSRAM). When internal RAM runs short -- opening the voice socket,
     * the agent thinking -- that allocation fails, and the driver says so
     * once per flush: "Draw bitmap failed: ESP_ERR_NO_MEM", a screenful of
     * them at 30 fps. Nothing is drawn wrong, but nothing should be drawn
     * at all then: hold the eyes still until there is room again.
     * (Only with PSRAM draw buffers: once cap_display.c has given LVGL its
     * own internal ones, a flush needs no RAM at all and this never trips.) */
    if (!cap_display_draw_bufs_internal()) {
        const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        static bool held;
        if (largest < FACE_MIN_DMA_BLOCK) {
            if (!held) {
                ESP_LOGW(TAG, "internal RAM low (%u bytes largest) -- eyes hold still",
                         (unsigned)largest);
                held = true;
            }
            return;
        }
        if (held) {
            ESP_LOGI(TAG, "internal RAM back (%u bytes largest) -- eyes moving",
                     (unsigned)largest);
            held = false;
        }
    }
    face_frame(dt);
}

/* ── Animations (the app's Display page) ───────────────────────────────────
 *
 * A GIF from the storage partition, full screen on the face screen, in place
 * of the eyes: while it shows, the face does not tick at all. LVGL's GIF
 * decoder (AnimatedGIF) streams the file through the stdio file system
 * ("S:" + an absolute path) into one RGB565 frame buffer shown by an
 * lv_image -- about 150 KB of PSRAM for 320x240, plus ~25 KB of decoder
 * state, freed again when it stops.
 *
 * NOT lv_gif: LVGL 9.5's widget paints every transparent pixel of a frame
 * with the GIF's background colour. Most GIFs from the web are "optimized":
 * each frame stores only what changed and marks the rest transparent, so on
 * the robot everything that did not move vanished every frame (Nyan Cat lost
 * its body and rainbow). The player below follows the GIF rules instead: a
 * transparent pixel keeps what is already there, and the previous frame's
 * disposal (2 = clear its rectangle, 3 = put back what was under it) runs
 * before the next frame is drawn.
 *
 * Loop plays until told otherwise; once plays the file one time and hands
 * the screen back to the eyes. The choice is saved (NVS "display": anim_on,
 * anim_id, anim_loop) so a looping animation comes back after a reboot. */

#define ANIM_ID_MAX     32
#define ANIM_MIN_PSRAM  (200 * 1024)

static lv_obj_t *s_anim_layer;               /* full-screen backdrop + the GIF */
static lv_obj_t *s_gif;                      /* lv_image showing s_pl->buf */

typedef struct {
    GIFIMAGE       gif;                      /* decoder state, ~25 KB */
    lv_draw_buf_t *buf;                      /* the canvas, RGB565 */
    uint16_t      *keep;                     /* what a disposal-3 frame covered, kw x kh */
    lv_timer_t    *timer;
    uint16_t       backdrop;                 /* RGB565 used to clear (disposal 2) */
    int            fx, fy, fw, fh, fdisp;    /* the frame on screen, disposed before the next */
    int            kx, ky, kw, kh;           /* the area saved in keep */
    bool           fresh;                    /* no line of the frame drawn yet */
    bool           saved;                    /* keep holds what this frame covers */
    bool           loop;
    bool           ending;                   /* play-once: last frame up, stop next tick */
} anim_player_t;
static anim_player_t *s_pl;
static volatile bool s_anim_on;
static bool      s_anim_loop = true;
static bool      s_anim_paused;              /* held for lack of internal RAM */
static char      s_anim_id[ANIM_ID_MAX];
static char      s_anim_src[160];            /* "S:/fs/display/x.gif"; the widget keeps the pointer */
static char      s_anim_dir[96] = "/fs/display";
static char      s_anim_pending[ANIM_ID_MAX]; /* restored at boot, started by the LVGL task */
static bool      s_anim_pending_loop;

static void anim_save(bool on, const char *id, bool loop)
{
    nvs_handle_t h;

    if (nvs_open("display", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, "anim_on", on ? 1 : 0);
    nvs_set_u8(h, "anim_loop", loop ? 1 : 0);
    if (id) {
        nvs_set_str(h, "anim_id", id);
    }
    nvs_commit(h);
    nvs_close(h);
}

/* One decoded line of the current frame into the canvas. Transparent
 * pixels are skipped: they mean "keep what the earlier frames left here". */
static void anim_keep_area(anim_player_t *pl, const GIFDRAW *d);

static void anim_draw_cb(GIFDRAW *d)
{
    anim_player_t *pl = (anim_player_t *)d->pUser;
    lv_draw_buf_t *b = pl ? pl->buf : NULL;
    const int yy = d->iY + d->y;
    int w = d->iWidth;

    /* First line of a frame: nothing of it is drawn yet, so this is the
     * moment to save what a "restore to previous" frame is about to cover. */
    if (pl && pl->fresh) {
        pl->fresh = false;
        if (d->ucDisposalMethod == 3) {
            anim_keep_area(pl, d);
        }
    }
    if (!b || yy < 0 || yy >= (int)b->header.h || d->iX < 0 || d->iX >= (int)b->header.w) {
        return;
    }
    if (d->iX + w > (int)b->header.w) {
        w = (int)b->header.w - d->iX;
    }
    uint16_t *dst = (uint16_t *)(b->data + (size_t)yy * b->header.stride) + d->iX;
    const uint16_t *pal = d->pPalette;
    const uint8_t *src = d->pPixels;

    if (d->ucHasTransparency) {
        const uint8_t t = d->ucTransparent;
        for (int i = 0; i < w; i++) {
            if (src[i] != t) {
                dst[i] = pal[src[i]];
            }
        }
    } else {
        for (int i = 0; i < w; i++) {
            dst[i] = pal[src[i]];
        }
    }
}

/* Save the canvas under the frame described by d into pl->keep. The buffer
 * is allocated once, canvas-sized, and only for GIFs that use disposal 3. */
static void anim_keep_area(anim_player_t *pl, const GIFDRAW *d)
{
    lv_draw_buf_t *b = pl->buf;
    const int cw = (int)b->header.w, ch = (int)b->header.h;
    const int x = d->iX < 0 ? 0 : d->iX, y = d->iY < 0 ? 0 : d->iY;
    const int w = (d->iX + d->iWidth > cw ? cw : d->iX + d->iWidth) - x;
    const int h = (d->iY + d->iHeight > ch ? ch : d->iY + d->iHeight) - y;

    if (w <= 0 || h <= 0) {
        return;
    }
    if (!pl->keep) {
        pl->keep = heap_caps_malloc((size_t)cw * ch * 2, MALLOC_CAP_SPIRAM);
        if (!pl->keep) {
            return;                        /* falls back to clearing the area */
        }
    }
    for (int r = 0; r < h; r++) {
        memcpy(pl->keep + (size_t)r * w, b->data + (size_t)(y + r) * b->header.stride + (size_t)x * 2, (size_t)w * 2);
    }
    pl->kx = x; pl->ky = y; pl->kw = w; pl->kh = h;
    pl->saved = true;
}

/* Clip the last frame's rectangle to the canvas. False if nothing is left. */
static bool anim_frame_rect(const anim_player_t *pl, int *x, int *y, int *w, int *h)
{
    const int cw = (int)pl->buf->header.w, ch = (int)pl->buf->header.h;

    *x = pl->fx < 0 ? 0 : pl->fx;
    *y = pl->fy < 0 ? 0 : pl->fy;
    *w = (pl->fx + pl->fw > cw ? cw : pl->fx + pl->fw) - *x;
    *h = (pl->fy + pl->fh > ch ? ch : pl->fy + pl->fh) - *y;
    return *w > 0 && *h > 0;
}

/* Decode the next frame. Returns >0 if more frames follow, 0 after the last
 * one (the next call starts from the top again). */
static int anim_next_frame(anim_player_t *pl, int *delay_ms)
{
    lv_draw_buf_t *b = pl->buf;
    const uint32_t stride = b->header.stride;
    int x, y, w, h, more;

    /* Dispose of the frame that is on screen now. 0/1 leave it in place;
     * 3 puts back what was under it; 2 (or a 3 we could not save) clears
     * its area to the backdrop. */
    if (pl->fdisp == 3 && pl->saved) {
        for (int r = 0; r < pl->kh; r++) {
            memcpy(b->data + (size_t)(pl->ky + r) * stride + (size_t)pl->kx * 2,
                   pl->keep + (size_t)r * pl->kw, (size_t)pl->kw * 2);
        }
    } else if ((pl->fdisp == 2 || pl->fdisp == 3) && anim_frame_rect(pl, &x, &y, &w, &h)) {
        for (int r = y; r < y + h; r++) {
            uint16_t *row = (uint16_t *)(b->data + (size_t)r * stride) + x;
            for (int i = 0; i < w; i++) {
                row[i] = pl->backdrop;
            }
        }
    }

    *delay_ms = 0;
    pl->fresh = true;
    pl->saved = false;
    more = GIF_playFrame(&pl->gif, delay_ms, pl);
    pl->fresh = false;
    pl->fx = pl->gif.iX;
    pl->fy = pl->gif.iY;
    pl->fw = pl->gif.iWidth;
    pl->fh = pl->gif.iHeight;
    pl->fdisp = (pl->gif.ucGIFBits & 0x1c) >> 2;
    if (*delay_ms < 20) {
        *delay_ms = 100;                   /* browsers treat 0-10 ms as 100 ms too */
    }
    lv_draw_buf_flush_cache(b, NULL);
    lv_image_cache_drop(b);
    if (s_gif) {
        lv_obj_invalidate(s_gif);
    }
    return more;
}

/* Caller holds the LVGL lock. Safe from inside the player's own timer. */
static void anim_player_free(void)
{
    anim_player_t *pl = s_pl;

    if (!pl) {
        return;
    }
    s_pl = NULL;
    if (pl->timer) {
        lv_timer_delete(pl->timer);
    }
    GIF_close(&pl->gif);
    if (pl->buf) {
        lv_image_cache_drop(pl->buf);
        lv_draw_buf_destroy(pl->buf);
    }
    heap_caps_free(pl->keep);
    heap_caps_free(pl);
}

static void anim_finish_once(void);

static void anim_timer_cb(lv_timer_t *t)
{
    anim_player_t *pl = (anim_player_t *)lv_timer_get_user_data(t);
    int ms = 100;

    if (pl->ending) {
        anim_finish_once();                /* frees pl and this timer */
        return;
    }
    /* Play-once: the last frame stays up for its own delay, then the eyes. */
    if (anim_next_frame(pl, &ms) == 0 && !pl->loop) {
        pl->ending = true;
    }
    lv_timer_set_period(t, (uint32_t)ms);
}

/* Caller holds the LVGL lock. */
static void anim_close_locked(void)
{
    if (s_anim_layer) {
        lv_obj_delete(s_anim_layer);       /* the image goes first, then its buffer */
        s_anim_layer = NULL;
        s_gif = NULL;
    }
    anim_player_free();
    s_anim_on = false;
    s_anim_paused = false;
    s_last_us = 0;
    if (s_status) {
        lv_obj_remove_flag(s_status, LV_OBJ_FLAG_HIDDEN);
    }
}

/* A play-once GIF reached its end: give the
 * screen back to the eyes. Runs inside the player's timer; LVGL allows a
 * timer to delete itself there. */
static void anim_finish_once(void)
{
    anim_close_locked();
    ESP_LOGI(TAG, "animation '%s' finished -- back to the eyes", s_anim_id);
}

/* From face_tick_cb, LVGL lock held. True while an animation owns the
 * screen. Every frame of a GIF is a full-screen redraw, and each flush needs
 * a DMA bounce buffer in internal RAM; when that runs short (the voice socket
 * opening, the agent thinking) hold the frame rather than fail every flush. */
static esp_err_t anim_play_locked(const char *id, bool loop);

static bool anim_tick(void)
{
    if (s_anim_pending[0]) {
        /* The boot-time restore: started here, in the LVGL task, so the
         * first frame decodes on its stack and not on app_main's. */
        char id[ANIM_ID_MAX];
        strlcpy(id, s_anim_pending, sizeof(id));
        s_anim_pending[0] = '\0';
        if (anim_play_locked(id, s_anim_pending_loop) == ESP_OK) {
            ESP_LOGI(TAG, "animation '%s' restored", id);
        }
    }
    if (!s_anim_on || !s_pl) {
        return false;
    }
    if (!cap_display_draw_bufs_internal()) {
        const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

        if (largest < FACE_MIN_DMA_BLOCK && !s_anim_paused) {
            lv_timer_pause(s_pl->timer);
            s_anim_paused = true;
        } else if (largest >= FACE_MIN_DMA_BLOCK && s_anim_paused) {
            lv_timer_resume(s_pl->timer);
            s_anim_paused = false;
        }
    }
    return true;
}

static esp_err_t face_build_locked(void);

/* Caller holds the LVGL lock. */
static esp_err_t anim_play_locked(const char *id, bool loop)
{
    struct { uint16_t w, h; } sz = { 0, 0 };

    if (face_build_locked() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    anim_close_locked();
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < ANIM_MIN_PSRAM) {
        ESP_LOGW(TAG, "animation: not enough PSRAM free to decode a frame");
        return ESP_ERR_NO_MEM;
    }
    snprintf(s_anim_src, sizeof(s_anim_src), "S:%s/%s.gif", s_anim_dir, id);
    /* Check the file with a 10-byte header read. NOT lv_gif_get_size(): it
     * builds a whole GIFIMAGE decoder on the stack -- 24 KB -- and every
     * caller here (the HTTP worker, app_main at boot) has less than that.
     * It overflowed the stack and the robot crashed seconds later. */
    {
        uint8_t head[10];
        FILE *f = fopen(s_anim_src + 2, "rb");
        const size_t n = f ? fread(head, 1, sizeof(head), f) : 0;

        if (f) {
            fclose(f);
        }
        if (n != sizeof(head) || memcmp(head, "GIF8", 4) != 0) {
            ESP_LOGW(TAG, "animation: cannot open %s", s_anim_src + 2);
            return ESP_ERR_NOT_FOUND;
        }
        sz.w = (uint16_t)(head[6] | (head[7] << 8));
        sz.h = (uint16_t)(head[8] | (head[9] << 8));
    }

    /* A backdrop the colour of the first pixel would be nicer for small
     * GIFs; black is the face's own background and never looks broken. */
    s_anim_layer = lv_obj_create(s_screen);
    if (!s_anim_layer) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(s_anim_layer);
    lv_obj_set_size(s_anim_layer, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_anim_layer, lv_color_hex(s_background), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_anim_layer, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(s_anim_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_anim_layer, LV_OBJ_FLAG_CLICKABLE);

    {
        anim_player_t *pl = heap_caps_calloc(1, sizeof(*pl), MALLOC_CAP_SPIRAM);
        int ms = 100, cw, ch, more;

        if (!pl) {
            anim_close_locked();
            return ESP_ERR_NO_MEM;
        }
        s_pl = pl;
        pl->loop = loop;
        GIF_begin(&pl->gif, GIF_PALETTE_RGB565_LE);
        if (!GIF_openFile(&pl->gif, s_anim_src, anim_draw_cb)) {
            ESP_LOGW(TAG, "animation: %s did not decode", s_anim_src + 2);
            anim_close_locked();
            return ESP_FAIL;
        }
        pl->gif.ucDrawType = GIF_DRAW_RAW;
        cw = GIF_getCanvasWidth(&pl->gif);
        ch = GIF_getCanvasHeight(&pl->gif);
        if (cw <= 0 || ch <= 0 || cw > MAX_WIDTH) {
            ESP_LOGW(TAG, "animation: %s is %dx%d, cannot show it", s_anim_src + 2, cw, ch);
            anim_close_locked();
            return ESP_ERR_INVALID_SIZE;
        }
        pl->buf = lv_draw_buf_create((uint32_t)cw, (uint32_t)ch, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
        if (!pl->buf) {
            ESP_LOGW(TAG, "animation: no memory for a %dx%d frame", cw, ch);
            anim_close_locked();
            return ESP_ERR_NO_MEM;
        }
        /* Start from the GIF's background colour (global palette), so
         * pixels the first frame leaves transparent are not garbage. */
        pl->backdrop = pl->gif.pPalette[pl->gif.ucBackground];
        for (int r = 0; r < ch; r++) {
            uint16_t *row = (uint16_t *)(pl->buf->data + (size_t)r * pl->buf->header.stride);
            for (int i = 0; i < cw; i++) {
                row[i] = pl->backdrop;
            }
        }
        pl->fdisp = 0;
        more = anim_next_frame(pl, &ms);
        pl->ending = (more == 0 && !loop);   /* a one-frame GIF shown once */
        /* The first frame's corner is the picture's real background: use it
         * for the fill around a smaller GIF and for disposal-2 clears, which
         * browsers show as "see-through to the page" anyway. */
        pl->backdrop = *(const uint16_t *)pl->buf->data;

        s_gif = lv_image_create(s_anim_layer);
        if (!s_gif) {
            anim_close_locked();
            return ESP_ERR_NO_MEM;
        }
        lv_image_set_src(s_gif, pl->buf);
        lv_obj_center(s_gif);
        lv_obj_set_style_bg_color(s_anim_layer,
                                  lv_color_make((uint8_t)(((pl->backdrop >> 11) & 0x1F) << 3),
                                                (uint8_t)(((pl->backdrop >> 5) & 0x3F) << 2),
                                                (uint8_t)((pl->backdrop & 0x1F) << 3)),
                                  LV_PART_MAIN);
        pl->timer = lv_timer_create(anim_timer_cb, (uint32_t)ms, pl);
        if (!pl->timer) {
            anim_close_locked();
            return ESP_ERR_NO_MEM;
        }
    }

    /* The animation has the whole screen: no IP line over it. */
    if (s_status) {
        lv_obj_add_flag(s_status, LV_OBJ_FLAG_HIDDEN);
    }
    strlcpy(s_anim_id, id, sizeof(s_anim_id));
    s_anim_loop = loop;
    s_anim_on = true;
    ESP_LOGI(TAG, "animation '%s' (%ux%u, %s) -- eyes paused", id, sz.w, sz.h, loop ? "loop" : "once");
    return ESP_OK;
}

static bool anim_id_ok(const char *id)
{
    if (!id || !id[0] || strlen(id) >= ANIM_ID_MAX) {
        return false;
    }
    for (const char *c = id; *c; c++) {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
              (*c >= '0' && *c <= '9') || *c == '_' || *c == '-')) {
            return false;
        }
    }
    return true;
}

void cap_display_anim_set_dir(const char *dir)
{
    if (dir && dir[0]) {
        strlcpy(s_anim_dir, dir, sizeof(s_anim_dir));
    }
}

const char *cap_display_anim_dir(void)
{
    return s_anim_dir;
}

bool cap_display_anim_id_valid(const char *id)
{
    return anim_id_ok(id);
}

esp_err_t cap_display_anim_play(const char *id, bool loop)
{
    esp_err_t err;

    if (!anim_id_ok(id)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!cap_display_available()) {
        return ESP_ERR_INVALID_STATE;          /* no panel on this board */
    }
    if (display_hw_lock() != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    s_anim_pending[0] = '\0';
    err = anim_play_locked(id, loop);
    display_hw_unlock();
    if (err == ESP_OK) {
        /* A looping choice is the screen's new resting state; a play-once
         * is a moment, after which the eyes are what should come back. */
        anim_save(loop, id, loop);
    }
    return err;
}

esp_err_t cap_display_anim_stop(void)
{
    if (display_hw_lock() != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    s_anim_pending[0] = '\0';
    anim_close_locked();
    display_hw_unlock();
    anim_save(false, NULL, s_anim_loop);
    ESP_LOGI(TAG, "animation off -- eyes back");
    return ESP_OK;
}

void cap_display_anim_get(cap_display_anim_state_t *out)
{
    nvs_handle_t h;

    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->on = s_anim_on;
    out->loop = s_anim_loop;
    strlcpy(out->id, s_anim_id, sizeof(out->id));
    /* Nothing played yet this boot: report the saved choice, so the app
     * shows what will be picked. */
    if (!out->id[0] && nvs_open("display", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(out->id);
        uint8_t v;

        if (nvs_get_str(h, "anim_id", out->id, &len) != ESP_OK) {
            out->id[0] = '\0';
        }
        if (nvs_get_u8(h, "anim_loop", &v) == ESP_OK) {
            out->loop = v != 0;
        }
        nvs_close(h);
    }
}

esp_err_t cap_display_anim_restore(void)
{
    nvs_handle_t h;
    uint8_t on = 0, loop = 1;
    char id[ANIM_ID_MAX] = { 0 };
    size_t len = sizeof(id);

    if (nvs_open("display", NVS_READONLY, &h) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    nvs_get_u8(h, "anim_on", &on);
    nvs_get_u8(h, "anim_loop", &loop);
    if (nvs_get_str(h, "anim_id", id, &len) != ESP_OK) {
        id[0] = '\0';
    }
    nvs_close(h);
    if (!on || !anim_id_ok(id) || !cap_display_available()) {
        if (id[0]) {
            strlcpy(s_anim_id, id, sizeof(s_anim_id));
            s_anim_loop = loop != 0;
        }
        return ESP_ERR_NOT_FOUND;
    }
    /* Picked up by the face tick within ~33 ms. */
    s_anim_pending_loop = loop != 0;
    strlcpy(s_anim_pending, id, sizeof(s_anim_pending));
    return ESP_OK;
}

/* ── Building the screen ───────────────────────────────────────────────── */

/* Caller holds the LVGL lock. */
static esp_err_t face_build_locked(void)
{
    if (s_screen) {
        return ESP_OK;
    }

    s_screen = lv_obj_create(NULL);
    if (!s_screen) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(s_background), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_screen, 0, LV_PART_MAIN);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 2; i++) {
        s_eye[i] = lv_obj_create(s_screen);
        if (!s_eye[i]) {
            return ESP_ERR_NO_MEM;
        }
        lv_obj_set_style_bg_opa(s_eye[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_eye[i], 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(s_eye[i], 0, LV_PART_MAIN);
        lv_obj_remove_flag(s_eye[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(s_eye[i], LV_OBJ_FLAG_CLICKABLE);
        s_drawn[i] = (drawn_t){ -1, -1, -1, -1, -1 };
    }
    s_drawn_color = 0xFFFFFFFF;

    /* The cut-outs, above the eyes and below the text. */
    for (int i = 0; i < 2; i++) {
        s_cut[i] = lv_obj_create(s_screen);
        if (s_cut[i]) {
            lv_obj_set_style_bg_color(s_cut[i], lv_color_hex(s_background), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(s_cut[i], LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_border_width(s_cut[i], 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(s_cut[i], 0, LV_PART_MAIN);
            lv_obj_set_style_radius(s_cut[i], LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_obj_remove_flag(s_cut[i], LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(s_cut[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(s_cut[i], LV_OBJ_FLAG_HIDDEN);
        }
        s_lid[i] = lv_line_create(s_screen);
        if (s_lid[i]) {
            lv_obj_set_style_line_width(s_lid[i], FACE_LID_W, LV_PART_MAIN);
            lv_obj_set_style_line_color(s_lid[i], lv_color_hex(s_background), LV_PART_MAIN);
            lv_obj_set_style_line_rounded(s_lid[i], false, LV_PART_MAIN);
            lv_obj_remove_flag(s_lid[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(s_lid[i], LV_OBJ_FLAG_HIDDEN);
        }
        memset(s_drawn_lid[i], 0, sizeof(s_drawn_lid[i]));
        memset(s_drawn_cut[i], 0, sizeof(s_drawn_cut[i]));
    }

    s_status = lv_label_create(s_screen);
    if (s_status) {
        /* Dim on purpose: it is a thing to read when you need it, not part of
         * the face. */
        lv_obj_set_style_text_color(s_status, lv_color_hex(0x6B6B6B), LV_PART_MAIN);
        lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_status, lv_pct(96));
        lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -6);
        lv_label_set_text(s_status, "");
    }

    s_zz = lv_label_create(s_screen);
    if (s_zz) {
        lv_obj_set_style_text_color(s_zz, lv_color_hex(0x8A7A1E), LV_PART_MAIN);
        lv_label_set_text(s_zz, "z z");
        lv_obj_align(s_zz, LV_ALIGN_TOP_RIGHT, -46, 30);
        lv_obj_add_flag(s_zz, LV_OBJ_FLAG_HIDDEN);
        s_zz_shown = false;
    }

    for (int i = 0; i < 2; i++) {
        const face_eye_t *e = i == 0 ? &s_moods[M_NEUTRAL].left : &s_moods[M_NEUTRAL].right;
        s_cur[i] = (shape_t){ e->w, e->h, e->r, e->dy, e->dx, e->li, e->lo, e->cut };
    }
    s_rgb[0] = (float)((s_eye_color >> 16) & 0xFF);
    s_rgb[1] = (float)((s_eye_color >> 8) & 0xFF);
    s_rgb[2] = (float)(s_eye_color & 0xFF);
    s_gx = s_gy = s_vx = s_vy = 0.0f;
    s_last_us = 0;
    face_frame(0.0f);

    if (!s_tick_timer) {
        s_tick_timer = lv_timer_create(face_tick_cb, FACE_TICK_MS, NULL);
    }
    return ESP_OK;
}

lv_obj_t *cap_display_face_screen_locked(void)
{
    if (face_build_locked() != ESP_OK) {
        return NULL;
    }
    return s_screen;
}

/* ── Public API ────────────────────────────────────────────────────────── */

static int mood_by_name(const char *name)
{
    if (!name) {
        return -1;
    }
    for (int i = 0; i < M_COUNT; i++) {
        if (s_moods[i].is_public && strcasecmp(name, s_moods[i].name) == 0) {
            return i;
        }
    }
    return -1;
}

bool cap_display_face_is_emotion(const char *name)
{
    return mood_by_name(name) >= 0;
}

size_t cap_display_face_emotion_names(char *out, size_t out_size)
{
    size_t at = 0;
    bool first = true;

    for (int i = 0; i < M_COUNT && at < out_size; i++) {
        if (!s_moods[i].is_public) {
            continue;
        }
        at += (size_t)snprintf(out + at, out_size - at, "%s%s", first ? "" : ", ", s_moods[i].name);
        first = false;
    }
    return at;
}

esp_err_t cap_display_face_set_emotion(const char *name)
{
    const int idx = mood_by_name(name);

    if (idx < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (display_hw_lock() != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    if (face_build_locked() != ESP_OK) {
        display_hw_unlock();
        return ESP_ERR_NO_MEM;
    }
    /* Holds until the next one; "neutral" hands the face back to the body.
     * Asking for an expression also counts as being awake. */
    s_override = idx;
    s_idle_t = 0.0f;
    display_hw_unlock();
    ESP_LOGI(TAG, "expression: %s", s_moods[idx].name);
    return ESP_OK;
}

esp_err_t cap_display_face_set_status(const char *text)
{
    char buf[FACE_STATUS_MAX];

    snprintf(buf, sizeof(buf), "%s", text ? text : "");
    if (display_hw_lock() != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    if (face_build_locked() == ESP_OK && s_status) {
        lv_label_set_text(s_status, buf);
    }
    display_hw_unlock();
    return ESP_OK;
}

esp_err_t cap_display_face_set_colors(int32_t background_rgb, int32_t eyes_rgb)
{
    if (display_hw_lock() != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    if (face_build_locked() != ESP_OK) {
        display_hw_unlock();
        return ESP_ERR_NO_MEM;
    }
    if (background_rgb >= 0) {
        s_background = (uint32_t)background_rgb & 0xFFFFFF;
        lv_obj_set_style_bg_color(s_screen, lv_color_hex(s_background), LV_PART_MAIN);
        for (int i = 0; i < 2; i++) {           /* the cut-outs are the background */
            if (s_cut[i]) {
                lv_obj_set_style_bg_color(s_cut[i], lv_color_hex(s_background), LV_PART_MAIN);
            }
            if (s_lid[i]) {
                lv_obj_set_style_line_color(s_lid[i], lv_color_hex(s_background), LV_PART_MAIN);
            }
        }
    }
    if (eyes_rgb >= 0) {
        /* Jump straight there rather than fading: a colour request reads as
         * "now". Moods with their own colour (love) and sleep dimming still
         * apply on top. */
        s_eye_color = (uint32_t)eyes_rgb & 0xFFFFFF;
        s_rgb[0] = (float)((s_eye_color >> 16) & 0xFF);
        s_rgb[1] = (float)((s_eye_color >> 8) & 0xFF);
        s_rgb[2] = (float)(s_eye_color & 0xFF);
    }
    display_hw_unlock();
    ESP_LOGI(TAG, "colours: background #%06X eyes %s", (unsigned)s_background,
             eyes_rgb >= 0 ? "set" : "unchanged");
    return ESP_OK;
}

void cap_display_face_set_motion_source(cap_display_motion_fn fn)
{
    s_motion_fn = fn;
}

void cap_display_face_set_alive(bool on)
{
    s_alive = on;
    if (!on) {
        s_event = -1;
        s_idle_t = 0.0f;
    }
}

bool cap_display_face_alive(void)
{
    return s_alive;
}

void cap_display_face_set_invert(uint8_t bits)
{
    s_invert = bits & 3;
}

uint8_t cap_display_face_invert(void)
{
    return s_invert;
}

void cap_display_face_poke(void)
{
    s_poke = true;
}

size_t cap_display_face_describe(char *out, size_t out_size)
{
    return (size_t)snprintf(out, out_size,
                            "eyes %s, mood %s%s%s, pitch %+.0f (nose up +), roll %+.0f (left side up +), "
                            "still %.0f s%s",
                            s_alive ? "alive" : "classic", s_moods[s_mood].name,
                            s_override != M_NEUTRAL ? ", asked for " : "",
                            s_override != M_NEUTRAL ? s_moods[s_override].name : "",
                            (double)s_pitch, (double)s_roll, (double)s_idle_t,
                            !s_motion_fn ? " (no IMU source)" : !s_imu_seen ? " (no IMU data yet)" : "");
}
