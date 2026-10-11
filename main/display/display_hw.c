/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * ST7789 on SPI3 + LVGL through esp_lvgl_port. See display_hw.h.
 *
 * Every number here is the MP4 ESP32 CORE board as mpv2-claw describes it
 * in boards/mp4_esp32_core (board_devices.yaml, board_peripherals.yaml),
 * which in turn copied the HeySanta board in xiaozhi-esp32 -- the same
 * panel on the same pins, verified on hardware:
 *
 *   SPI3, MOSI 40, SCK 41, CS 9, DC 39, no MISO (3-wire, sio_mode)
 *   spi_mode 2, pclk 80 MHz (drop to 40 if the panel tears)
 *   240x320 native, driven 320x240 landscape:
 *     swap_xy = true, mirror_x = true, mirror_y = false, invert_color = true
 *   RST on the board RESET net (reset_gpio_num = -1: software reset over SPI)
 *   backlight GPIO 42 through Q2, a P-channel FET: the PWM is inverted
 *
 * SPI3, not SPI2: the four servo driver boards sit on SPI2 and GP-SPI2 has
 * the six chip selects; GP-SPI3 has three. With the display on SPI2 the
 * fourth driver board failed to attach (ESP_ERR_NO_MEM from
 * spi_bus_add_device). Everything goes through the GPIO matrix either way.
 *
 * Draw buffers: two of CONFIG_APP_DISPLAY_BUFFER_LINES lines each, in
 * internal DMA RAM, held for good. mpv2-claw learned this the hard way --
 * with the buffers in PSRAM the SPI driver bounces every flush through an
 * internal buffer it allocates on the spot, and once the voice stack is
 * listening that allocation fails and the face freezes.
 */

#include "display_hw.h"

#include "sdkconfig.h"

#if CONFIG_APP_DISPLAY_ENABLE

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

#include "robot/board_config.h"

static const char *TAG = "display_hw";

#define LCD_H_RES            320
#define LCD_V_RES            240
#define LCD_PCLK_HZ          (80 * 1000 * 1000)
#define LCD_SPI_MODE         2
#define LCD_CMD_BITS         8
#define LCD_PARAM_BITS       8
#define LCD_TRANS_QUEUE      10
#define LCD_BASE_SWAP_XY     true
#define LCD_BASE_MIRROR_X    true
#define LCD_BASE_MIRROR_Y    false
#define LCD_INVERT_COLOR     true

#define BL_LEDC_MODE         LEDC_LOW_SPEED_MODE
#define BL_LEDC_TIMER        LEDC_TIMER_1
#define BL_LEDC_CHANNEL      LEDC_CHANNEL_1
#define BL_LEDC_RES          LEDC_TIMER_10_BIT
#define BL_LEDC_FREQ_HZ      5000

#define LVGL_TASK_STACK      (10 * 1024)   /* GIF frames decode on this task */
#define LVGL_TASK_PRIORITY   4
#define LVGL_LOCK_TIMEOUT_MS 1000

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t    s_panel;
static lv_display_t             *s_disp;
static bool                      s_ready;
static bool                      s_backlight_ready;

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode      = BL_LEDC_MODE,
        .duty_resolution = BL_LEDC_RES,
        .timer_num       = BL_LEDC_TIMER,
        .freq_hz         = BL_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer");

    /* duty 0 + output_invert = pin high = FET off = dark. The panel comes
     * out of reset showing random RAM; lighting it now would be the white
     * flash at boot. The face turns it on once the eyes are drawn. */
    const ledc_channel_config_t ch = {
        .gpio_num   = LCD_BACKLIGHT_PIN,
        .speed_mode = BL_LEDC_MODE,
        .channel    = BL_LEDC_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = BL_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
        .flags.output_invert = 1,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch), TAG, "ledc channel");
    s_backlight_ready = true;
    return ESP_OK;
}

static esp_err_t panel_init(void)
{
    const spi_bus_config_t bus = {
        .mosi_io_num     = LCD_SPI_MOSI_PIN,
        .miso_io_num     = -1,
        .sclk_io_num     = LCD_SPI_SCK_PIN,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_H_RES * CONFIG_APP_DISPLAY_BUFFER_LINES * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    const esp_lcd_panel_io_spi_config_t io = {
        .cs_gpio_num       = LCD_SPI_CS_PIN,
        .dc_gpio_num       = LCD_DC_PIN,
        .spi_mode          = LCD_SPI_MODE,
        .pclk_hz           = LCD_PCLK_HZ,
        .trans_queue_depth = LCD_TRANS_QUEUE,
        .lcd_cmd_bits      = LCD_CMD_BITS,
        .lcd_param_bits    = LCD_PARAM_BITS,
        .flags.sio_mode    = 1,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io, &s_io),
                        TAG, "panel io");

    const esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = -1,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian    = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_io, &dev, &s_panel), TAG, "st7789");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, LCD_INVERT_COLOR), TAG, "invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "disp on");
    return ESP_OK;
}

static esp_err_t lvgl_init(void)
{
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack    = LVGL_TASK_STACK;
    port_cfg.task_priority = LVGL_TASK_PRIORITY;
    /* PSRAM stack, as mpv2-claw (ESP_LVGL_ADAPTER_LVGL_THREAD_STACK_IN_PSRAM).
     * This task reads GIFs off LittleFS; that used to need an internal stack
     * (a flash read from a PSRAM stack asserted in cache_utils.c), but since
     * Phase 5 the firmware runs with CONFIG_SPIRAM_XIP_FROM_PSRAM, where it
     * no longer does. 10 KB of internal RAM back for the microphone's DMA. */
    port_cfg.task_stack_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl_port_init");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = s_io,
        .panel_handle  = s_panel,
        .buffer_size   = LCD_H_RES * CONFIG_APP_DISPLAY_BUFFER_LINES,
        .double_buffer = true,
        .hres          = LCD_H_RES,
        .vres          = LCD_V_RES,
        .monochrome    = false,
        .rotation = {
            .swap_xy  = LCD_BASE_SWAP_XY,
            .mirror_x = LCD_BASE_MIRROR_X,
            .mirror_y = LCD_BASE_MIRROR_Y,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma    = true,
            .buff_spiram = false,
            .swap_bytes  = true,   /* the panel wants big-endian RGB565 */
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(s_disp != NULL, ESP_FAIL, TAG, "lvgl_port_add_disp");
    return ESP_OK;
}

esp_err_t display_hw_init(void)
{
    esp_err_t err;

    if (s_ready) {
        return ESP_OK;
    }
    err = backlight_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "backlight not available: %s", esp_err_to_name(err));
    }
    err = panel_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel did not come up: %s", esp_err_to_name(err));
        return err;
    }
    err = lvgl_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LVGL did not start: %s", esp_err_to_name(err));
        return err;
    }
    s_ready = true;
    ESP_LOGI(TAG, "ST7789 %dx%d on SPI3, LVGL draw buffers 2 x %d lines (internal DMA RAM)",
             LCD_H_RES, LCD_V_RES, CONFIG_APP_DISPLAY_BUFFER_LINES);
    return ESP_OK;
}

bool display_hw_available(void)
{
    return s_ready;
}

esp_err_t display_hw_lock(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    return lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS) ? ESP_OK : ESP_ERR_TIMEOUT;
}

void display_hw_unlock(void)
{
    if (s_ready) {
        lvgl_port_unlock();
    }
}

esp_lcd_panel_handle_t display_hw_panel(void)
{
    return s_panel;
}

void display_hw_base_mirror(bool *mirror_x, bool *mirror_y)
{
    if (mirror_x) {
        *mirror_x = LCD_BASE_MIRROR_X;
    }
    if (mirror_y) {
        *mirror_y = LCD_BASE_MIRROR_Y;
    }
}

esp_err_t display_hw_backlight(int percent)
{
    if (!s_backlight_ready) {
        return ESP_ERR_NOT_FOUND;
    }
    if (percent < 0)   { percent = 0; }
    if (percent > 100) { percent = 100; }
    const uint32_t duty = (uint32_t)(((1U << BL_LEDC_RES) - 1U) * (uint32_t)percent / 100U);
    if (ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty) != ESP_OK ||
            ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL) != ESP_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

#else /* !CONFIG_APP_DISPLAY_ENABLE */

esp_err_t display_hw_init(void)                      { return ESP_ERR_NOT_SUPPORTED; }
bool display_hw_available(void)                      { return false; }
esp_err_t display_hw_lock(void)                      { return ESP_ERR_INVALID_STATE; }
void display_hw_unlock(void)                         { }
esp_lcd_panel_handle_t display_hw_panel(void)        { return NULL; }
void display_hw_base_mirror(bool *mx, bool *my)      { if (mx) { *mx = false; } if (my) { *my = false; } }
esp_err_t display_hw_backlight(int percent)          { (void)percent; return ESP_ERR_NOT_SUPPORTED; }

#endif /* CONFIG_APP_DISPLAY_ENABLE */
