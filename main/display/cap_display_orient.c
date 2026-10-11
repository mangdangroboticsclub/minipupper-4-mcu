/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * Screen orientation: turned 180 degrees by default.
 *
 * display_hw programs the ST7789 as wired (mirror_x = true, mirror_y = false,
 * with swap_xy). On the robot as assembled that reads upside down, so -- as
 * in mpv2 -- the panel is flipped 180 degrees on every boot unless the owner
 * turned that off. The choice lives in NVS (namespace "display", keys "flip"
 * and "mirror") so it survives a reflash, and is applied straight to the
 * panel controller: LVGL stays at rotation 0, so the panel's own MADCTL is
 * the only place orientation is decided.
 *
 * mpv2-claw also had the `display` console command here. mpx-dog has no
 * console; the same knobs are on the app's /v1/display route.
 */

#include "cap_display.h"
#include "display_hw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "lvgl.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "cap_display";

#ifndef CONFIG_APP_DISPLAY_FLIP180_DEFAULT
#define CONFIG_APP_DISPLAY_FLIP180_DEFAULT 0
#endif

#define ORIENT_NVS_NS   "display"

static bool s_flip180 = CONFIG_APP_DISPLAY_FLIP180_DEFAULT;
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
    esp_lcd_panel_handle_t panel = display_hw_panel();
    if (!display_hw_available() || !panel) {
        return ESP_ERR_NOT_FOUND;
    }
    bool base_mx = true, base_my = false;
    display_hw_base_mirror(&base_mx, &base_my);
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
    if (display_hw_lock() != ESP_OK) {
        ESP_LOGW(TAG, "orientation saved; the panel is not running, applied at next start");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_lcd_panel_mirror(panel, mx, my);
    if (err == ESP_OK) {
        err = esp_lcd_panel_set_gap(panel, s_gap_x, s_gap_y);
        /* Redraw everything at the new place. */
        lv_obj_invalidate(lv_screen_active());
    }
    display_hw_unlock();
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

esp_err_t cap_display_set_shift(int x, int y)
{
    load_once();
    if (x < -GAP_MAX || x > GAP_MAX || y < -GAP_MAX || y > GAP_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    s_gap_x = (int8_t)x;
    s_gap_y = (int8_t)y;
    save();
    return cap_display_apply_orientation();
}

void cap_display_get_shift(int *x, int *y)
{
    load_once();
    if (x) {
        *x = s_gap_x;
    }
    if (y) {
        *y = s_gap_y;
    }
}
