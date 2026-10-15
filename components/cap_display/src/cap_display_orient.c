/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * Screen orientation: turned 180 degrees by default, and the `display`
 * console command.
 *
 * The board YAML says mirror_x = true, mirror_y = false (with swap_xy), and
 * the board manager programs the ST7789 that way at boot. On the robot as
 * assembled that reads upside down, so -- as in mpv2 -- the panel is flipped
 * 180 degrees on every boot unless the owner turned that off. The choice
 * lives in NVS (namespace "display", keys "flip" and "mirror") so it survives
 * a reflash, and is applied straight to the panel controller: display_service
 * leaves SPI panels at LVGL rotation 0, so the panel's own MADCTL is the only
 * place orientation is decided.
 */

#include "cap_display.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dev_display_lcd.h"
#include "display_service.h"
#include "esp_board_device.h"
#include "esp_console.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "lvgl.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "cap_display";

#ifndef CONFIG_MP4_DISPLAY_FLIP180_DEFAULT
#define CONFIG_MP4_DISPLAY_FLIP180_DEFAULT 0
#endif

#define ORIENT_NVS_NS   "display"
#define LCD_DEVICE      "display_lcd"

static bool s_flip180 = CONFIG_MP4_DISPLAY_FLIP180_DEFAULT;
static bool s_mirror;
static bool s_loaded;
/* Picture shift in pixels (+x right, +y down). Turning the panel 180
 * degrees with MADCTL mirrors the controller's RAM window, and on a panel
 * whose glass does not sit exactly on that window the picture lands a few
 * pixels off: a strip of never-drawn RAM shows as a black band on one edge.
 * esp_lcd_panel_set_gap() moves the window back. NVS "gap_x" / "gap_y". */
#define GAP_MAX 40
static int8_t s_gap_x, s_gap_y;

static bool nvs_get_flag(nvs_handle_t h, const char *key, bool def)
{
    uint8_t v = 0;
    return nvs_get_u8(h, key, &v) == ESP_OK ? v != 0 : def;
}

static void load_once(void)
{
    if (s_loaded) {
        return;
    }
    s_loaded = true;
    nvs_handle_t h;
    if (nvs_open(ORIENT_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        s_flip180 = nvs_get_flag(h, "flip", s_flip180);
        s_mirror = nvs_get_flag(h, "mirror", false);
        {
            int8_t v;
            if (nvs_get_i8(h, "gap_x", &v) == ESP_OK && v >= -GAP_MAX && v <= GAP_MAX) {
                s_gap_x = v;
            }
            if (nvs_get_i8(h, "gap_y", &v) == ESP_OK && v >= -GAP_MAX && v <= GAP_MAX) {
                s_gap_y = v;
            }
        }
        nvs_close(h);
    }
}

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open(ORIENT_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "orientation not saved: NVS unavailable");
        return;
    }
    nvs_set_u8(h, "flip", s_flip180 ? 1 : 0);
    nvs_set_u8(h, "mirror", s_mirror ? 1 : 0);
    nvs_set_i8(h, "gap_x", s_gap_x);
    nvs_set_i8(h, "gap_y", s_gap_y);
    nvs_commit(h);
    nvs_close(h);
}

esp_err_t cap_display_apply_orientation(void)
{
    load_once();
    dev_display_lcd_handles_t *lcd = NULL;
    dev_display_lcd_config_t *cfg = NULL;
    if (esp_board_device_get_handle(LCD_DEVICE, (void **)&lcd) != ESP_OK || !lcd ||
        !lcd->panel_handle) {
        return ESP_ERR_NOT_FOUND;
    }
    bool base_mx = true, base_my = false;
    if (esp_board_device_get_config(LCD_DEVICE, (void **)&cfg) == ESP_OK && cfg) {
        base_mx = cfg->mirror_x;
        base_my = cfg->mirror_y;
    }
    /* 180 degrees = both axes inverted; "mirror" is a left-right mirror. */
    const bool mx = base_mx ^ s_flip180 ^ s_mirror;
    const bool my = base_my ^ s_flip180;
    /* The panel's SPI link is not thread-safe: esp_lcd keeps one in-flight
     * counter per panel and every call drains it with portMAX_DELAY. If this
     * MADCTL write lands while the LVGL task is pushing a frame, both sides
     * wait on the same transaction and the LVGL task hangs forever holding
     * the LVGL lock -- black screen, "Failed to acquire LVGL lock" from then
     * on. Every frame is pushed with that lock held, so taking it here keeps
     * the two apart. No lock (service not running) means nobody else is
     * drawing; the setting is saved and applied when the service starts. */
    if (display_service_lock() != ESP_OK) {
        ESP_LOGW(TAG, "orientation saved; the panel is not running, applied at next start");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_lcd_panel_mirror(lcd->panel_handle, mx, my);
    if (err == ESP_OK) {
        err = esp_lcd_panel_set_gap(lcd->panel_handle, s_gap_x, s_gap_y);
        /* Redraw everything at the new place. */
        lv_obj_invalidate(lv_screen_active());
    }
    display_service_unlock();
    ESP_LOGI(TAG, "orientation: %s%s (mirror_x=%d mirror_y=%d, shift %+d,%+d) -> %s",
             s_flip180 ? "turned 180" : "as wired", s_mirror ? ", mirrored" : "",
             mx, my, s_gap_x, s_gap_y, esp_err_to_name(err));
    return err;
}

esp_err_t cap_display_set_orientation(bool flip180, bool mirror)
{
    load_once();
    s_flip180 = flip180;
    s_mirror = mirror;
    save();
    return cap_display_apply_orientation();
}

void cap_display_get_orientation(bool *flip180, bool *mirror)
{
    load_once();
    if (flip180) {
        *flip180 = s_flip180;
    }
    if (mirror) {
        *mirror = s_mirror;
    }
}

/* ── `display` console command ─────────────────────────────────────────── */

static int cmd_display(int argc, char **argv)
{
    const char *op = argc >= 2 ? argv[1] : "info";
    bool flip, mirror;
    cap_display_get_orientation(&flip, &mirror);

    if (strcmp(op, "flip") == 0) {
        cap_display_set_orientation(!flip, mirror);
    } else if (strcmp(op, "mirror") == 0) {
        cap_display_set_orientation(flip, !mirror);
    } else if (strcmp(op, "reset") == 0) {
        cap_display_set_orientation(CONFIG_MP4_DISPLAY_FLIP180_DEFAULT, false);
    } else if (strcmp(op, "test") == 0) {
        return cap_display_show_test_pattern(argc >= 3 ? (uint32_t)atoi(argv[2]) * 1000u : 5000u) == ESP_OK ? 0 : 1;
    } else if (strcmp(op, "shift") == 0) {
        load_once();
        if (argc >= 4) {
            const int x = atoi(argv[2]), y = atoi(argv[3]);
            if (x < -GAP_MAX || x > GAP_MAX || y < -GAP_MAX || y > GAP_MAX) {
                printf("shift: each value between %d and %d\n", -GAP_MAX, GAP_MAX);
                return 1;
            }
            s_gap_x = (int8_t)x;
            s_gap_y = (int8_t)y;
            save();
            cap_display_apply_orientation();
        } else if (argc != 2) {
            printf("usage: display shift <x> <y>   (pixels, +x right, +y down; 0 0 = none)\n");
            return 1;
        }
        printf("display shift: %+d %+d (saved)\n", s_gap_x, s_gap_y);
        return 0;
    } else if (strcmp(op, "talk") == 0) {
        if (argc >= 3) {
            cap_display_set_talk_face(strcmp(argv[2], "on") == 0);
        }
        printf("display talk: %s (conversation %s the face)\n",
               cap_display_talk_face() ? "on" : "off",
               cap_display_talk_face() ? "shows subtitles and expressions on" : "leaves");
        return 0;
    } else if (strcmp(op, "eyes") == 0) {
        char line[200];
        const char *arg = argc >= 3 ? argv[2] : "";
        uint8_t inv = cap_display_face_invert();

        if (strcmp(arg, "on") == 0 || strcmp(arg, "alive") == 0) {
            cap_display_set_eyes_alive(true);
        } else if (strcmp(arg, "off") == 0 || strcmp(arg, "classic") == 0) {
            cap_display_set_eyes_alive(false);
        } else if (strcmp(arg, "invert") == 0 && argc >= 4) {
            if (strcmp(argv[3], "pitch") == 0) {
                inv ^= 1;
            } else if (strcmp(argv[3], "roll") == 0) {
                inv ^= 2;
            } else if (strcmp(argv[3], "none") == 0) {
                inv = 0;
            }
            cap_display_set_eyes_invert(inv);
        } else if (arg[0] != '\0') {
            printf("usage: display eyes [on|off|invert pitch|roll|none]\n");
            return 1;
        }
        cap_display_face_describe(line, sizeof(line));
        printf("%s%s%s\n", line,
               cap_display_face_invert() & 1 ? ", pitch inverted" : "",
               cap_display_face_invert() & 2 ? ", roll inverted" : "");
        return 0;
    } else if (strcmp(op, "info") != 0) {
        printf("usage: display [info|flip|mirror|reset|test|talk on|off|eyes ...]\n"
               "  flip    turn the picture 180 degrees (saved)\n"
               "  mirror  mirror it left-right (saved)\n"
               "  reset   back to the default (turned 180, not mirrored)\n"
               "  test [s] show the test pattern (5 s, or s seconds)\n"
               "  shift x y  move the picture x px right, y px down if an edge shows a\n"
               "          black strip (saved); 'display shift 0 0' undoes it\n"
               "  talk    on: subtitles + expressions while talking; off: eyes stay as they are\n"
               "  eyes    on: eyes follow the head and react to the IMU (default); off: classic\n"
               "          invert pitch|roll|none: if they look the wrong way (saved)\n"
               "          no argument: live mood / pitch / roll\n");
        return 0;
    }
    cap_display_get_orientation(&flip, &mirror);
    printf("display: %s, %s, shift %+d %+d, talk face %s, eyes %s\n", flip ? "turned 180" : "as wired",
           mirror ? "mirrored" : "not mirrored", s_gap_x, s_gap_y, cap_display_talk_face() ? "on" : "off",
           cap_display_face_alive() ? "alive" : "classic");
    return 0;
}

void cap_display_register_console(void)
{
    const esp_console_cmd_t cmd = {
        .command = "display",
        .help = "Screen: display [info|flip|mirror|reset|test [s]|shift x y|talk on|off|eyes on|off|invert]",
        .hint = NULL,
        .func = &cmd_display,
    };
    if (esp_console_cmd_register(&cmd) != ESP_OK) {
        ESP_LOGW(TAG, "`display` console command not registered");
    }
}
