/* imu.c -- the board's BMI270 (U11) over I2C (pins in board_config.h).
 *
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * The driver is the espressif/bmi270 managed component: the part needs an
 * ~8 KB Bosch configuration blob uploaded over I2C before it produces data.
 */
#include "imu.h"
#include "board_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"
#include "bmi270.h"

static const char *TAG = "imu";

#define IMU_SDA_GPIO  IMU_I2C_SDA_PIN
#define IMU_SCL_GPIO  IMU_I2C_SCL_PIN

static i2c_master_bus_handle_t s_bus;
static bmi270_handle_t        *s_imu;
static bool                    s_ok;

#define IMU_PERIOD_TICKS (pdMS_TO_TICKS(2) > 0 ? pdMS_TO_TICKS(2) : 1)   /* 2 ms poll, faster than the 400 Hz ODR */

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static int16_t      s_acc[3], s_gyr[3];
static int64_t      s_t_us = -1;    /* time of the last good sample */

bool imu_init(void)
{
    i2c_master_bus_config_t bc = {
        .i2c_port          = IMU_I2C_PORT,
        .sda_io_num        = IMU_SDA_GPIO,
        .scl_io_num        = IMU_SCL_GPIO,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bc, &s_bus) != ESP_OK) {
        ESP_LOGW(TAG, "no I2C bus - IMU disabled");
        return false;
    }

    /* The address depends on the SDO strap: probe both. */
    const uint8_t cand[2] = { BMI270_I2C_ADDRESS_L, BMI270_I2C_ADDRESS_H };
    uint8_t addr = 0;
    for (int i = 0; i < 2; i++) {
        if (i2c_master_probe(s_bus, cand[i], 100) == ESP_OK) { addr = cand[i]; break; }
    }
    if (!addr) { ESP_LOGW(TAG, "BMI270 not answering - IMU disabled"); return false; }

    const bmi270_driver_config_t dc = {
        .addr      = addr,
        .interface = BMI270_USE_I2C,
        .i2c_bus   = s_bus,
    };
    /* bmi270_create() soft-resets the part and waits pdMS_TO_TICKS(10); start
     * just after a tick boundary and retry, as Espressif's bmi270 example does. */
    esp_err_t e = ESP_FAIL;
    for (int attempt = 0; attempt < 5 && e != ESP_OK; attempt++) {
        vTaskDelay(1);
        s_imu = NULL;
        e = bmi270_create(&dc, &s_imu);
        if (e != ESP_OK || !s_imu) { e = ESP_FAIL; vTaskDelay(pdMS_TO_TICKS(50)); }
    }
    if (e != ESP_OK) { ESP_LOGW(TAG, "bmi270_create failed - IMU disabled"); return false; }

    const bmi270_config_t mc = {
        .acce_odr   = BMI270_ACC_ODR_400_HZ,
        .acce_range = BMI270_ACC_RANGE_4_G,
        .gyro_odr   = BMI270_GYR_ODR_400_HZ,
        .gyro_range = BMI270_GYR_RANGE_1000_DPS,
    };
    if (bmi270_start(s_imu, &mc) != ESP_OK) {
        ESP_LOGW(TAG, "bmi270_start failed - IMU disabled");
        bmi270_delete(s_imu);
        s_imu = NULL;
        return false;
    }
    s_ok = true;
    return true;
}

static void imu_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, IMU_PERIOD_TICKS);
        float ax, ay, az, gx, gy, gz;
        int64_t t = esp_timer_get_time();
        if (bmi270_get_acce_data(s_imu, &ax, &ay, &az) != ESP_OK) continue;
        if (bmi270_get_gyro_data(s_imu, &gx, &gy, &gz) != ESP_OK) continue;
        portENTER_CRITICAL(&s_mux);
        s_acc[0] = (int16_t)(ax * 1000.0f);
        s_acc[1] = (int16_t)(ay * 1000.0f);
        s_acc[2] = (int16_t)(az * 1000.0f);
        s_gyr[0] = (int16_t)(gx * 10.0f);
        s_gyr[1] = (int16_t)(gy * 10.0f);
        s_gyr[2] = (int16_t)(gz * 10.0f);
        s_t_us = t;
        portEXIT_CRITICAL(&s_mux);
    }
}

void imu_start(void)
{
    if (s_ok) xTaskCreatePinnedToCore(imu_task, "imu", 4096, NULL, 5, NULL, 0);
}

bool imu_latest(int16_t acc_mg[3], int16_t gyr_ddps[3], uint32_t *t_us, int64_t max_age_us)
{
    portENTER_CRITICAL(&s_mux);
    int64_t t = s_t_us;
    for (int i = 0; i < 3; i++) { acc_mg[i] = s_acc[i]; gyr_ddps[i] = s_gyr[i]; }
    portEXIT_CRITICAL(&s_mux);
    *t_us = (uint32_t)(t < 0 ? 0 : t);
    if (t < 0 || esp_timer_get_time() - t > max_age_us) {
        for (int i = 0; i < 3; i++) { acc_mg[i] = 0; gyr_ddps[i] = 0; }
        return false;
    }
    return true;
}
