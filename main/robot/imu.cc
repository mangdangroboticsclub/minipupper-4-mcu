/* imu.cc — BMI270 (accel + gyro) with a BMM150 magnetometer on its AUX bus.
 *
 * Why this file changed shape from the Mini Pupper 2 version
 * ---------------------------------------------------------
 * That board had a QMI8658C on SPI2, sharing the servo bus, and the driver
 * was a hundred lines of register pokes because the QMI8658C needs nothing
 * else. The BMI270 is not like that: it will not produce a single sample
 * until an 8 KB configuration image has been uploaded into its feature
 * engine, and that image is a Bosch binary blob. So instead of open-coding
 * registers we drive the two Bosch Sensor APIs:
 *
 *   espressif2022/bmi270          -> bmi2.c / bmi270.c, carries the blob
 *   tangyumei3535/bmm150_sensorapi -> BMM150 driven through the BMI270 AUX
 *
 * Both are platform-independent C: they call out through function pointers
 * for register read, register write and microsecond delay. Those three
 * callbacks are the only ESP32-specific code here, and they go through the
 * NEW i2c_master driver — the same bus handle the ES7210 audio codec uses.
 * That last point is not incidental. The obvious off-the-shelf choice,
 * lbuque/bmi270_bmm150_sensor, wraps the LEGACY i2c_bus component; putting
 * the legacy and new I2C drivers on one port is unsupported, and the codec
 * would have been the casualty.
 *
 * The BMM150 sits on the BMI270's own auxiliary I2C pins (ASDX/ASCX), not on
 * the host bus, so there is no way to talk to it directly — every mag read is
 * a host->BMI270->BMM150 hop. It is therefore treated as optional: if it does
 * not answer, accel and gyro still run and mag_valid stays false.
 */

#include "robot/imu.h"

#include <cstring>
#include <cmath>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "platform/shared_i2c.h"
#include "robot/board_config.h"

extern "C" {
#include "bmi2.h"
#include "bmi270.h"
#include "bmm150.h"
}

static const char *TAG = "IMU";

/* ── Bus parameters ───────────────────────────────────────────
 * 400 kHz is the BMI270's fast-mode limit. The blob upload is ~8 KB of burst
 * writes, so the difference between 100 and 400 kHz here is the difference
 * between a 700 ms and a 180 ms boot delay. The frequency itself comes from
 * IMU_I2C_FREQ_HZ in robot/board_config.h, so the bus and the device cannot
 * disagree about it.
 *
 * BMI2_READ_WRITE_LEN caps how much of the config image goes out per
 * transaction. Bosch's own examples use 128 for I2C; larger is not better
 * because every chunk still has to fit the I2C driver's transfer buffer. */
#define BMI2_READ_WRITE_LEN 128

namespace {

i2c_master_dev_handle_t s_bmi_dev = nullptr;
struct bmi2_dev s_bmi = {};
struct bmm150_dev s_bmm = {};
bool s_mag_ready = false;

TaskHandle_t s_task = nullptr;
robot::ImuData s_data = {};
SemaphoreHandle_t s_mutex = nullptr;

/* ── Bosch platform callbacks ─────────────────────────────────
 * Signatures are fixed by bmi2_defs.h: BMI2_INTF_RETURN_TYPE is int8_t and
 * intf_ptr is whatever we stashed in dev.intf_ptr — here the I2C device
 * handle. Returning BMI2_INTF_RET_SUCCESS (0) means OK; anything else aborts
 * the Bosch call with a comms error, which is what we want on a NACK. */

BMI2_INTF_RETURN_TYPE bmi2_i2c_read(uint8_t reg_addr, uint8_t *reg_data,
                                    uint32_t len, void *intf_ptr) {
    auto dev = static_cast<i2c_master_dev_handle_t>(intf_ptr);
    esp_err_t err = i2c_master_transmit_receive(dev, &reg_addr, 1,
                                                reg_data, len,
                                                pdMS_TO_TICKS(100));
    return (err == ESP_OK) ? BMI2_INTF_RET_SUCCESS : -1;
}

BMI2_INTF_RETURN_TYPE bmi2_i2c_write(uint8_t reg_addr, const uint8_t *reg_data,
                                     uint32_t len, void *intf_ptr) {
    auto dev = static_cast<i2c_master_dev_handle_t>(intf_ptr);

    /* i2c_master_transmit() takes one contiguous buffer, so the register
     * address has to be prepended. BMI2_READ_WRITE_LEN bounds len, so a
     * stack buffer is safe and avoids a malloc on every config chunk. */
    uint8_t buf[BMI2_READ_WRITE_LEN + 1];
    if (len > BMI2_READ_WRITE_LEN) {
        return -1;
    }
    buf[0] = reg_addr;
    std::memcpy(&buf[1], reg_data, len);

    esp_err_t err = i2c_master_transmit(dev, buf, len + 1, pdMS_TO_TICKS(100));
    return (err == ESP_OK) ? BMI2_INTF_RET_SUCCESS : -1;
}

void bmi2_delay_us(uint32_t period, void *intf_ptr) {
    (void)intf_ptr;
    /* Sub-millisecond waits must not yield — the config upload has 450 us
     * settling requirements that a vTaskDelay would round up to a tick. */
    if (period < 1000) {
        esp_rom_delay_us(period);
    } else {
        vTaskDelay(pdMS_TO_TICKS(period / 1000));
    }
}

/* ── BMM150 transport ─────────────────────────────────────────
 * The magnetometer has no path to the host bus — its SDI/SCK go to the
 * BMI270's ASDX/ASCX. So every register access is tunnelled: the Bosch
 * BMM150 driver calls these, and they hand the transaction to the BMI270's
 * auxiliary master in manual mode. */

int8_t aux_i2c_read(uint8_t reg_addr, uint8_t *reg_data, uint32_t len,
                    void *intf_ptr) {
    (void)intf_ptr;
    return bmi2_read_aux_man_mode(reg_addr, reg_data,
                                  static_cast<uint16_t>(len), &s_bmi);
}

int8_t aux_i2c_write(uint8_t reg_addr, const uint8_t *reg_data, uint32_t len,
                     void *intf_ptr) {
    (void)intf_ptr;
    return bmi2_write_aux_man_mode(reg_addr, reg_data,
                                   static_cast<uint16_t>(len), &s_bmi);
}

/* ── Raw -> physical conversion ───────────────────────────────
 * The BMI270 is a 16-bit signed sensor, so full scale maps to +/-32768 LSB.
 * width is passed in rather than hard-coded so the same helper serves both
 * the accel (range in g) and the gyro (range in dps). */
inline float lsb_to_units(int16_t raw, float full_scale) {
    return (full_scale * static_cast<float>(raw)) / 32768.0f;
}

constexpr float ACCEL_RANGE_G   = 2.0f;      /* BMI2_ACC_RANGE_2G   */
constexpr float GYRO_RANGE_DPS  = 2000.0f;   /* BMI2_GYR_RANGE_2000 */

/*
 * Configure accelerometer, gyroscope and the auxiliary interface together.
 *
 * They go in one call because the AUX interface is a BMI270 sensor like the
 * other two, and Bosch's API wants the whole configuration set before any of
 * it is enabled. Splitting it was the first version of this file's mistake.
 */
bool configure_sensors() {
    /* Internal pull-ups on the auxiliary bus.
     *
     * This single register write is the difference between a working
     * magnetometer and a silent one. ASDX/ASCX run straight from the BMI270
     * to the BMM150 with no external pull-ups anywhere on the schematic, so
     * without enabling the BMI270's own 2k pull-ups the aux bus never idles
     * high and every transaction NAKs. Nothing in the datasheet's register
     * map makes this obvious; it is in Bosch's reference code and in every
     * working BMI270+BMM150 design.
     */
    uint8_t pupsel = BMI2_ASDA_PUPSEL_2K;
    if (bmi2_set_regs(BMI2_AUX_IF_TRIM, &pupsel, 1, &s_bmi) != BMI2_OK) {
        ESP_LOGW(TAG, "could not set aux bus pull-ups — magnetometer will "
                      "likely not answer");
    }

    struct bmi2_sens_config cfg[3] = {};
    cfg[0].type = BMI2_ACCEL;
    cfg[1].type = BMI2_GYRO;
    cfg[2].type = BMI2_AUX;

    if (bmi270_get_sensor_config(cfg, 3, &s_bmi) != BMI2_OK) {
        ESP_LOGE(TAG, "bmi270_get_sensor_config failed");
        return false;
    }

    /* 100 Hz suits a 20 Hz poll with room to spare and keeps the gyro out of
     * its high-power mode. Performance-optimised filtering is the low-noise
     * setting; the alternative saves ~200 uA, which is not a trade worth
     * making on a robot driving four servo boards. */
    cfg[0].cfg.acc.odr         = BMI2_ACC_ODR_100HZ;
    cfg[0].cfg.acc.range       = BMI2_ACC_RANGE_2G;
    cfg[0].cfg.acc.bwp         = BMI2_ACC_NORMAL_AVG4;
    cfg[0].cfg.acc.filter_perf = BMI2_PERF_OPT_MODE;

    cfg[1].cfg.gyr.odr         = BMI2_GYR_ODR_100HZ;
    cfg[1].cfg.gyr.range       = BMI2_GYR_RANGE_2000;
    cfg[1].cfg.gyr.ois_range   = BMI2_GYR_OIS_2000;
    cfg[1].cfg.gyr.bwp         = BMI2_GYR_NORMAL_MODE;
    cfg[1].cfg.gyr.noise_perf  = BMI2_PERF_OPT_MODE;
    cfg[1].cfg.gyr.filter_perf = BMI2_PERF_OPT_MODE;

    /* Manual mode to begin with: bmm150_init() and the power-mode writes
     * below are host-driven register accesses, and the BMI270 will not pass
     * those through once it owns the bus. Data mode comes after. */
    cfg[2].cfg.aux.aux_en          = BMI2_ENABLE;
    cfg[2].cfg.aux.manual_en       = BMI2_ENABLE;
    cfg[2].cfg.aux.fcu_write_en    = BMI2_ENABLE;
    cfg[2].cfg.aux.i2c_device_addr = IMU_BMM150_I2C_ADDR;
    cfg[2].cfg.aux.man_rd_burst    = BMI2_AUX_READ_LEN_3;
    cfg[2].cfg.aux.aux_rd_burst    = BMI2_AUX_READ_LEN_3;
    cfg[2].cfg.aux.read_addr       = BMM150_REG_DATA_X_LSB;
    cfg[2].cfg.aux.odr             = BMI2_AUX_ODR_100HZ;

    if (bmi270_set_sensor_config(cfg, 3, &s_bmi) != BMI2_OK) {
        ESP_LOGE(TAG, "bmi270_set_sensor_config failed");
        return false;
    }

    /* Enable only after configuring — the order is load-bearing. */
    uint8_t sens_list[3] = { BMI2_ACCEL, BMI2_GYRO, BMI2_AUX };
    if (bmi270_sensor_enable(sens_list, 3, &s_bmi) != BMI2_OK) {
        ESP_LOGE(TAG, "bmi270_sensor_enable failed");
        return false;
    }
    return true;
}

/*
 * Bring up the BMM150 behind the BMI270 and hand the bus over.
 *
 * Optional by design: a board built without the magnetometer populated, or
 * with a dead one, still walks. Accel and gyro are already running by the
 * time this is called, and a failure here leaves the aux interface in manual
 * mode, which is the quietest possible failure state.
 */
bool configure_magnetometer() {
    s_bmm.intf     = BMM150_I2C_INTF;
    s_bmm.read     = aux_i2c_read;
    s_bmm.write    = aux_i2c_write;
    s_bmm.delay_us = bmi2_delay_us;
    static uint8_t bmm_addr = IMU_BMM150_I2C_ADDR;
    s_bmm.intf_ptr = &bmm_addr;

    int8_t rslt = bmm150_init(&s_bmm);
    if (rslt != BMM150_OK || s_bmm.chip_id != BMM150_CHIP_ID) {
        ESP_LOGW(TAG, "BMM150 did not answer on the aux bus (rslt=%d, "
                      "chip_id=0x%02x) — continuing without it",
                 rslt, s_bmm.chip_id);
        return false;
    }

    struct bmm150_settings settings = {};
    settings.preset_mode = BMM150_PRESETMODE_REGULAR;
    if (bmm150_set_presetmode(&settings, &s_bmm) != BMM150_OK) {
        ESP_LOGW(TAG, "BMM150 preset mode failed");
        return false;
    }
    settings.pwr_mode = BMM150_POWERMODE_NORMAL;
    if (bmm150_set_op_mode(&settings, &s_bmm) != BMM150_OK) {
        ESP_LOGW(TAG, "BMM150 power mode failed");
        return false;
    }

    /* Hand the aux bus to the BMI270. From here it polls the BMM150 itself
     * at the configured ODR and drops the result into sens_data.aux_data, so
     * a mag sample costs nothing beyond the accel/gyro read we already do —
     * no extra host transactions per sample. */
    struct bmi2_sens_config aux = {};
    aux.type = BMI2_AUX;
    if (bmi270_get_sensor_config(&aux, 1, &s_bmi) != BMI2_OK) {
        return false;
    }
    aux.cfg.aux.manual_en = BMI2_DISABLE;
    if (bmi270_set_sensor_config(&aux, 1, &s_bmi) != BMI2_OK) {
        ESP_LOGW(TAG, "could not switch the aux bus to data mode");
        return false;
    }

    ESP_LOGI(TAG, "BMM150 ready on the aux bus (chip id 0x%02x)", s_bmm.chip_id);
    return true;
}

void imu_task(void *arg) {
    (void)arg;
#if CONFIG_APP_IMU_LOG_ENABLE
    int since_log = 0;
#endif

    for (;;) {
        struct bmi2_sens_data raw = {};
        robot::ImuData sample = {};

        if (bmi2_get_sensor_data(&raw, &s_bmi) == BMI2_OK) {
            sample.ax = lsb_to_units(raw.acc.x, ACCEL_RANGE_G);
            sample.ay = lsb_to_units(raw.acc.y, ACCEL_RANGE_G);
            sample.az = lsb_to_units(raw.acc.z, ACCEL_RANGE_G);
            sample.gx = lsb_to_units(raw.gyr.x, GYRO_RANGE_DPS);
            sample.gy = lsb_to_units(raw.gyr.y, GYRO_RANGE_DPS);
            sample.gz = lsb_to_units(raw.gyr.z, GYRO_RANGE_DPS);

            if (s_mag_ready) {
                /* No extra bus traffic: the BMI270 already polled the BMM150
                 * and shipped the eight raw bytes alongside the accel and
                 * gyro data. All that is left is Bosch's compensation, which
                 * needs the trim registers bmm150_init() read at startup. */
                struct bmm150_mag_data mag = {};
                if (bmm150_aux_mag_data(raw.aux_data, &mag, &s_bmm) == BMM150_OK) {
                    sample.mx = mag.x;
                    sample.my = mag.y;
                    sample.mz = mag.z;
                    sample.mag_valid = true;
                }
            }

            if (s_mutex) {
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_data = sample;
                xSemaphoreGive(s_mutex);
            }
        }

#if CONFIG_APP_IMU_LOG_ENABLE
        if (++since_log >= CONFIG_APP_IMU_LOG_INTERVAL) {
            since_log = 0;
            robot::imu_print();
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(50));   /* 20 Hz */
    }
}

}  // namespace

namespace robot {

bool imu_init() {
    if (s_task != nullptr) {
        ESP_LOGW(TAG, "imu_init() called twice — ignoring");
        return true;
    }

    i2c_master_bus_handle_t bus = hal::i2c_bus();
    if (bus == nullptr) {
        ESP_LOGE(TAG, "shared I2C bus unavailable");
        return false;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = IMU_BMI270_I2C_ADDR;
    dev_cfg.scl_speed_hz = IMU_I2C_FREQ_HZ;
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_bmi_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device(0x%02x) failed: %s",
                 IMU_BMI270_I2C_ADDR, esp_err_to_name(err));
        return false;
    }

    /* INT1 is wired to IO10 but nothing subscribes to it yet: the polling
     * task is well inside the 100 Hz ODR, so data-ready interrupts would buy
     * nothing today. Configure it as a plain input so the pin is in a defined
     * state and a future data-ready path only has to attach a handler. */
    gpio_config_t int_io = {};
    int_io.pin_bit_mask = 1ULL << IMU_INT1_PIN;
    int_io.mode = GPIO_MODE_INPUT;
    int_io.pull_up_en = GPIO_PULLUP_DISABLE;
    int_io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    int_io.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&int_io);

    s_bmi.intf = BMI2_I2C_INTF;
    s_bmi.intf_ptr = s_bmi_dev;
    s_bmi.read = bmi2_i2c_read;
    s_bmi.write = bmi2_i2c_write;
    s_bmi.delay_us = bmi2_delay_us;
    s_bmi.read_write_len = BMI2_READ_WRITE_LEN;
    s_bmi.config_file_ptr = nullptr;   /* use the driver's built-in blob */

    /* This is the slow call: it uploads ~8 KB and then polls
     * INTERNAL_STATUS until the feature engine reports ready. */
    int8_t rslt = bmi270_init(&s_bmi);
    if (rslt != BMI2_OK) {
        ESP_LOGE(TAG, "bmi270_init failed (rslt=%d) — no IMU. Check that the "
                      "BMI270 answers at 0x%02x on SDA=%d/SCL=%d.",
                 rslt, IMU_BMI270_I2C_ADDR,
                 (int)IMU_I2C_SDA_PIN, (int)IMU_I2C_SCL_PIN);
        i2c_master_bus_rm_device(s_bmi_dev);
        s_bmi_dev = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "BMI270 found, chip id 0x%02x", s_bmi.chip_id);

    if (!configure_sensors()) {
        return false;
    }

    s_mag_ready = configure_magnetometer();
    ESP_LOGI(TAG, "BMM150 on AUX: %s", s_mag_ready ? "ready" : "not available");

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == nullptr) {
        ESP_LOGE(TAG, "mutex alloc failed");
        return false;
    }

    /* Core 0: the gait task owns core 1 and a 20 Hz sensor poll has no
     * business competing with it for cycles. */
    if (xTaskCreatePinnedToCore(imu_task, "imu", 4096, nullptr, 4, &s_task, 0)
        != pdPASS) {
        ESP_LOGE(TAG, "imu task create failed");
        return false;
    }

    ESP_LOGI(TAG, "IMU ready (accel+gyro @100Hz, poll 20Hz)");
    return true;
}

ImuData imu_read() {
    ImuData copy;
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        copy = s_data;
        xSemaphoreGive(s_mutex);
    }
    return copy;
}

void imu_print() {
    ImuData d = imu_read();
    if (d.mag_valid) {
        ESP_LOGI(TAG, "acc[g] %.3f %.3f %.3f | gyr[dps] %.2f %.2f %.2f | "
                      "mag[uT] %.1f %.1f %.1f",
                 d.ax, d.ay, d.az, d.gx, d.gy, d.gz, d.mx, d.my, d.mz);
    } else {
        ESP_LOGI(TAG, "acc[g] %.3f %.3f %.3f | gyr[dps] %.2f %.2f %.2f",
                 d.ax, d.ay, d.az, d.gx, d.gy, d.gz);
    }
}

}  // namespace robot
