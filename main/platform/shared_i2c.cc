#include "platform/shared_i2c.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "robot/board_config.h"

#define TAG "i2c_bus"

namespace hal {

namespace {

i2c_master_bus_handle_t s_bus = nullptr;
SemaphoreHandle_t s_lock = nullptr;

/* The mutex itself has to be created without a mutex, so do it the one way
 * that needs no lock: a function-local static, which the C++ runtime
 * initialises exactly once and guards for us. */
SemaphoreHandle_t lock() {
    static SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    return mutex;
}

}  // namespace

i2c_master_bus_handle_t i2c_bus() {
    s_lock = lock();
    if (s_lock != nullptr) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }

    if (s_bus == nullptr) {
        i2c_master_bus_config_t cfg = {};
        cfg.i2c_port = (i2c_port_t)IMU_I2C_PORT;
        cfg.sda_io_num = IMU_I2C_SDA_PIN;
        cfg.scl_io_num = IMU_I2C_SCL_PIN;
        cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        cfg.glitch_ignore_cnt = 7;
        cfg.intr_priority = 0;
        cfg.trans_queue_depth = 0;
        /* R15/R16 are 1k externally; the internal pull-ups are left on as a
         * belt-and-braces measure and are far too weak to fight them. */
        cfg.flags.enable_internal_pullup = 1;

        esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
            s_bus = nullptr;
        } else {
            ESP_LOGI(TAG, "I2C%d up on SDA=%d SCL=%d",
                     IMU_I2C_PORT,
                     (int)IMU_I2C_SDA_PIN,
                     (int)IMU_I2C_SCL_PIN);
        }
    }

    if (s_lock != nullptr) {
        xSemaphoreGive(s_lock);
    }
    return s_bus;
}

}  // namespace hal
