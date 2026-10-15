/*
 * SPDX-FileCopyrightText: 2026 MangDang
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Santy Control - board-specific device factories.
 *
 * Only the ST7789 panel needs one. ST7789 ships inside ESP-IDF's esp_lcd, so
 * board_devices.yaml declares no component dependency for it; the board
 * manager just needs to be told which constructor to call.
 *
 * Every board with a `display_lcd` device MUST provide this file. Without it
 * the build gets all the way to the final link and then fails with
 *
 *     undefined reference to `lcd_panel_factory_entry_t'
 *
 * from dev_display_lcd_sub_spi.c, which declares the symbol `extern` and
 * leaves the board to define it. Nothing in the YAML hints that a C file is
 * required, so the error arrives with no obvious connection to the board
 * definition that caused it.
 */

#include <string.h>

#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"

static const char *TAG = "SANTY_SETUP";

/*
 * No offset: the panel on J4 is assumed to show its full 240x320 framebuffer,
 * as on the MP4 board. If the image comes up shifted by a fixed number of
 * pixels, correct it here rather than in the YAML -- a 135x240 panel, for
 * example, starts at (52, 40).
 */
#define SANTY_LCD_OFFSET_X  0
#define SANTY_LCD_OFFSET_Y  0

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

#if (SANTY_LCD_OFFSET_X != 0) || (SANTY_LCD_OFFSET_Y != 0)
    ret = esp_lcd_panel_set_gap(*ret_panel, SANTY_LCD_OFFSET_X, SANTY_LCD_OFFSET_Y);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_lcd_panel_set_gap failed: %s", esp_err_to_name(ret));
    }
#endif

    /*
     * J4 pin 3 (RESET) is on the board RESET net rather than a GPIO, so
     * board_devices.yaml sets reset_gpio_num to -1 and esp_lcd issues a
     * software reset (command 0x01) over SPI instead. The board manager
     * calls esp_lcd_panel_reset() itself, so there is nothing to do here.
     */
    return ESP_OK;
}
