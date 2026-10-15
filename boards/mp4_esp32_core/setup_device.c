/*
 * SPDX-FileCopyrightText: 2026 MangDang
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MangDang MP4 ESP32 CORE - board-specific device factories.
 *
 * Only the ST7789 panel needs one. ST7789 ships inside ESP-IDF's esp_lcd, so
 * board_devices.yaml declares no component dependency for it; the board
 * manager just needs to be told which constructor to call.
 */

#include <string.h>

#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"

static const char *TAG = "MP4_CORE_SETUP";

/*
 * The 2.0" 240x320 panel on connector J4 shows its full framebuffer, so no
 * offset is applied. If a different panel is fitted and the image is shifted,
 * this is where to correct it -- see waveshare_esp32_s3_geek, whose 135x240
 * panel starts at (52, 40):
 *
 *     esp_lcd_panel_set_gap(*ret_panel, MP4_LCD_OFFSET_X, MP4_LCD_OFFSET_Y);
 */
#define MP4_LCD_OFFSET_X  0
#define MP4_LCD_OFFSET_Y  0

esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel)
{
    esp_lcd_panel_dev_config_t panel_dev_cfg = {0};
    esp_err_t ret;

    memcpy(&panel_dev_cfg, panel_dev_config, sizeof(esp_lcd_panel_dev_config_t));

    ret = esp_lcd_new_panel_st7789(io, &panel_dev_cfg, ret_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_st7789 failed: %s", esp_err_to_name(ret));
        return ret;
    }

#if (MP4_LCD_OFFSET_X != 0) || (MP4_LCD_OFFSET_Y != 0)
    ret = esp_lcd_panel_set_gap(*ret_panel, MP4_LCD_OFFSET_X, MP4_LCD_OFFSET_Y);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_lcd_panel_set_gap failed: %s", esp_err_to_name(ret));
    }
#endif

    /*
     * The panel's RST pin is tied to the board RESET net rather than a GPIO,
     * so board_devices.yaml sets reset_gpio_num to -1 and esp_lcd issues a
     * software reset (command 0x01) over SPI instead. The board manager calls
     * esp_lcd_panel_reset() itself during init, so there is nothing to do
     * here -- this note exists so the -1 does not look like an oversight.
     */
    return ESP_OK;
}
