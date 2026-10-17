/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * The panel and LVGL, as one small service.
 *
 * mpv2-claw's display code was written against ESP-Claw's display_service
 * (board-manager panel + esp_lvgl_adapter + sessions). mpx-dog has none of
 * that, so this is the subset the face, the GIF player and the orientation
 * code actually use: bring the ST7789 up on SPI3, start LVGL through
 * esp_lvgl_port, hand out the LVGL lock, own the backlight. There is one
 * client (the face), so there are no sessions: the face is the screen.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Panel + LVGL up, backlight still off (the face turns it on once drawn).
 *  ESP_ERR_NOT_SUPPORTED when the display is compiled out. */
esp_err_t display_hw_init(void);

/** True once display_hw_init() succeeded. Every other call is a no-op
 *  (or returns ESP_ERR_INVALID_STATE) before that. */
bool display_hw_available(void);

/** The LVGL lock. Hold it around every lv_* call made from a task other
 *  than LVGL's own; never take it from inside an lv_timer callback. */
esp_err_t display_hw_lock(void);
void      display_hw_unlock(void);

/** The esp_lcd panel, for MADCTL writes (orientation). Caller holds the lock. */
esp_lcd_panel_handle_t display_hw_panel(void);

/** How the panel is mirrored as wired, before any user orientation. */
void display_hw_base_mirror(bool *mirror_x, bool *mirror_y);

/** Backlight 0..100 percent; the P-FET inversion is handled here. */
esp_err_t display_hw_backlight(int percent);

#ifdef __cplusplus
}
#endif
