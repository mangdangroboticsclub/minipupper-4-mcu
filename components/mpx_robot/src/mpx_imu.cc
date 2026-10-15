/*
 * SPDX-FileCopyrightText: 2026 MangDang
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * IMU for the MangDang MP4 ESP32 CORE: a Bosch BMI270 on the shared I2C bus.
 *
 * This replaces the QMI8658C-over-SPI driver the earlier MangDang board used.
 * That part shared SPI2 with the four servo driver boards, which is why
 * driver_board.h still carries a long note about a bus lock; on this board the
 * IMU has moved to I2C entirely and that hazard is gone.
 *
 * Ownership: this file is the single owner of the BMI270. ESP-Claw ships its
 * own lua_module_imu with a BMI270 backend, and the two must not both call
 * bmi270_sensor_create() on the same chip, so the board defaults leave
 * CONFIG_APP_CLAW_LUA_MODULE_IMU off and Lua reaches the IMU through
 * lua_module_mpx_robot instead.
 *
 * The chip is configured the same way ESP-Claw's backend configures it --
 * +/-16 g, +/-2000 dps, 200 Hz -- so readings are comparable between the two.
 *
 * Magnetometer: the BMM150 (U4) is NOT on the main I2C bus. Its SCL/SDA go to
 * the BMI270's ASCX/ASDX pins -- the accelerometer's own auxiliary I2C master
 * -- so the only way to reach it is through the BMI270. The sequence, which is
 * Bosch's own aux example and what board_devices.yaml spells out:
 *
 *   1. set the aux bus pull-ups (BMI2_AUX_IF_TRIM)
 *   2. configure the BMI2_AUX sensor in MANUAL mode and enable it, so
 *      bmi2_read/write_aux_man_mode() can relay single register accesses
 *   3. run bmm150_init() and the power/preset writes over that relay -- this
 *      reads the chip id and the trim registers the compensation needs
 *   4. flip aux to DATA mode: from then on the BMI270 itself polls the
 *      BMM150's eight data registers at the aux ODR and the bytes arrive in
 *      bmi2_sens_data.aux_data alongside accel and gyro, for free
 *   5. bmm150_aux_mag_data() turns those bytes into compensated uT
 *
 * Accel and gyro are configured and enabled first, on their own, exactly as
 * before; the magnetometer is added afterwards and any failure there is logged
 * and ignored. That ordering is deliberate: a BMM150 problem must not take the
 * accelerometer and gyro down with it.
 *
 * Bosch's bmm150.c lives in src/bmm150/, copied verbatim from the ESP-Claw
 * submodule's lua_module_magnetometer (which must not be edited in place).
 * Only the register map and the compensation maths are wanted from it; that
 * module's own backend talks to the chip on the main bus, which here reaches
 * nothing, and the board defaults keep it off so its copy is never linked.
 */

#include "robot/imu.h"
#include "robot/robot.h"

#include <cstring>

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

extern "C" {
#include "bmi270_api.h"
#include "bmm150.h"
#include "esp_board_device.h"
#include "esp_board_periph.h"
#include "i2c_bus.h"
}

/* The board manager generates this table as C, and only declares it in a
 * private header (private_inc/esp_board_find_utils.h) that components outside
 * it cannot include. So declare it here.
 *
 * The extern "C" is load-bearing: without it the C++ compiler mangles the name
 * and the failure lands at link time rather than compile time. It also has to
 * sit at namespace scope -- a linkage specification inside a function body is
 * not legal C++, which is what the first attempt got wrong.
 *
 * Walking the table rather than calling esp_board_manager_get_device_config()
 * is deliberate: the walk yields cfg_size, and that is the only thing standing
 * between a YAML/struct mismatch and an IMU that silently returns nonsense.
 * ESP-Claw's own lua_module_imu.c does the same for the same reason. */
extern "C" const esp_board_device_desc_t g_esp_board_devices[];

static const char *TAG = "mpx_imu";

namespace robot {
namespace {

/* Mirror of the `imu_sensor` entry in board_devices.yaml.
 *
 * The board-manager generator emits struct fields in the order they appear in
 * the YAML and silently omits any key that is missing, so this struct and that
 * YAML block have to agree field for field. The cfg_size check below is what
 * turns a disagreement into a clear error instead of garbage readings. Same
 * contract as lua_imu_board_cfg_t in ESP-Claw's lua_module_imu.        */
struct mpx_imu_board_cfg_t {
    const char *name;
    const char *type;
    const char *chip;
    int8_t      i2c_addr;
    int32_t     frequency;
    int8_t      int_gpio_num;
    int8_t      sdo_gpio_num;
    uint8_t     peripheral_count;
    const char *peripheral_name;
};

constexpr const char *kDeviceName = "imu_sensor";

/* Full-scale ranges applied in configure_sensor(). Keep these two constants
 * and that function in step -- they are the only reason the raw LSB counts
 * the chip returns can be turned into g and dps.                          */
constexpr float kAccelRangeG   = 16.0f;
constexpr float kGyroRangeDps  = 2000.0f;
constexpr float kInt16FullScale = 32768.0f;

constexpr int  kPollPeriodMs   = 5;     /* 200 Hz, the ODR: the Pi/ROS link streams it to the EKF,
                                          and the RL link (rl_link.cc) puts it in every
                                          5 ms STATE, where a policy reads each frame */
constexpr int  kTaskStackBytes = 4096;
constexpr int  kTaskPriority   = 3;

/* BMM150 on the BMI270's aux bus: SDO to GND, PS to 3V3, CSB to GND (see
 * board_devices.yaml), i.e. I2C at the default address. The aux ODR only sets
 * how often the BMI270 re-reads the chip; the BMM150's own conversion rate is
 * set separately below and 20 Hz polling here reads whichever is newest.   */
constexpr uint8_t kMagI2cAddr   = BMM150_DEFAULT_I2C_ADDRESS;   /* 0x10 */
constexpr uint8_t kMagAuxOdr    = BMI2_AUX_ODR_50HZ;
constexpr uint8_t kMagDataRate  = BMM150_DATA_RATE_30HZ;
constexpr uint8_t kAuxPullUp    = BMI2_ASDA_PUPSEL_2K;

ImuData          s_latest;
SemaphoreHandle_t s_mutex        = nullptr;
i2c_bus_handle_t s_i2c_bus       = nullptr;
bmi270_handle_t  s_sensor        = nullptr;
bool             s_periph_ref    = false;
const char      *s_periph_name   = nullptr;
TaskHandle_t     s_task          = nullptr;
bool             s_ready         = false;
struct bmm150_dev s_mag          = {};
bool             s_mag_ready     = false;

esp_err_t resolve_board_cfg(const mpx_imu_board_cfg_t **out)
{
    const esp_board_device_desc_t *desc = g_esp_board_devices;

    while (desc != nullptr && desc->name != nullptr) {
        if (strcmp(desc->name, kDeviceName) == 0) {
            if (desc->cfg == nullptr) {
                return ESP_ERR_NOT_FOUND;
            }
            if (desc->cfg_size != sizeof(mpx_imu_board_cfg_t)) {
                ESP_LOGE(TAG,
                         "Board device '%s' cfg_size=%u, expected %u. "
                         "board_devices.yaml is out of sync with "
                         "mpx_imu_board_cfg_t: every field must be present, "
                         "in order, using -1 for unused GPIOs.",
                         kDeviceName, (unsigned)desc->cfg_size,
                         (unsigned)sizeof(mpx_imu_board_cfg_t));
                return ESP_ERR_INVALID_SIZE;
            }
            *out = (const mpx_imu_board_cfg_t *)desc->cfg;
            return ESP_OK;
        }
        desc = desc->next;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t open_i2c_bus(const char *peripheral_name, int frequency)
{
    i2c_master_bus_handle_t master_handle = nullptr;
    i2c_master_bus_config_t *master_cfg = nullptr;

    esp_err_t err = esp_board_periph_ref_handle(peripheral_name, (void **)&master_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot reference board I2C bus '%s': %s",
                 peripheral_name, esp_err_to_name(err));
        return err;
    }
    s_periph_ref  = true;
    s_periph_name = peripheral_name;

    err = esp_board_periph_get_config(peripheral_name, (void **)&master_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot read board I2C config '%s': %s",
                 peripheral_name, esp_err_to_name(err));
        return err;
    }

    /* The BMI270 driver takes an i2c_bus_handle_t from the i2c_bus wrapper
     * rather than the raw master handle, so build one over the same port and
     * pins the board manager already opened. ESP-Claw's lua_module_imu does
     * exactly this.                                                       */
    const i2c_config_t bus_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = master_cfg->sda_io_num,
        .scl_io_num = master_cfg->scl_io_num,
        .sda_pullup_en = master_cfg->flags.enable_internal_pullup != 0,
        .scl_pullup_en = master_cfg->flags.enable_internal_pullup != 0,
        .master = { .clk_speed = (uint32_t)frequency },
        .clk_flags = 0,
    };
    (void)master_handle;

    s_i2c_bus = i2c_bus_create((i2c_port_t)master_cfg->i2c_port, &bus_cfg);
    if (s_i2c_bus == nullptr) {
        ESP_LOGE(TAG, "i2c_bus_create failed on port %d", (int)master_cfg->i2c_port);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t configure_sensor()
{
    uint8_t sens_list[2] = { BMI2_ACCEL, BMI2_GYRO };
    struct bmi2_sens_config config[2] = {};

    config[BMI2_ACCEL].type = BMI2_ACCEL;
    config[BMI2_GYRO].type  = BMI2_GYRO;

    int8_t rslt = bmi2_set_adv_power_save(BMI2_DISABLE, s_sensor);
    if (rslt != BMI2_OK) {
        ESP_LOGE(TAG, "bmi2_set_adv_power_save failed: %d", rslt);
        return ESP_FAIL;
    }

    rslt = bmi2_get_sensor_config(config, 2, s_sensor);
    if (rslt != BMI2_OK) {
        ESP_LOGE(TAG, "bmi2_get_sensor_config failed: %d", rslt);
        return ESP_FAIL;
    }

    config[BMI2_ACCEL].cfg.acc.odr         = BMI2_ACC_ODR_200HZ;
    config[BMI2_ACCEL].cfg.acc.range       = BMI2_ACC_RANGE_16G;
    config[BMI2_ACCEL].cfg.acc.bwp         = BMI2_ACC_NORMAL_AVG4;
    config[BMI2_ACCEL].cfg.acc.filter_perf = BMI2_PERF_OPT_MODE;

    config[BMI2_GYRO].cfg.gyr.odr          = BMI2_GYR_ODR_200HZ;
    config[BMI2_GYRO].cfg.gyr.range        = BMI2_GYR_RANGE_2000;
    config[BMI2_GYRO].cfg.gyr.bwp          = BMI2_GYR_NORMAL_MODE;
    config[BMI2_GYRO].cfg.gyr.noise_perf   = BMI2_PERF_OPT_MODE;
    config[BMI2_GYRO].cfg.gyr.filter_perf  = BMI2_PERF_OPT_MODE;

    rslt = bmi2_set_sensor_config(config, 2, s_sensor);
    if (rslt != BMI2_OK) {
        ESP_LOGE(TAG, "bmi2_set_sensor_config failed: %d", rslt);
        return ESP_FAIL;
    }

    rslt = bmi270_sensor_enable(sens_list, 2, s_sensor);
    if (rslt != BMI2_OK) {
        ESP_LOGE(TAG, "bmi270_sensor_enable failed: %d", rslt);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ── BMM150 through the BMI270 aux master ─────────────────────────────────
 *
 * Bosch's bmm150.c talks to "a bus" through three function pointers. Here
 * the bus is the BMI270's auxiliary master in manual mode: each call becomes
 * a write of the BMM150 register address into BMI2_AUX_RD_ADDR / WR_ADDR and
 * a wait for the aux-busy flag, which bmi2_read_aux_man_mode() and
 * bmi2_write_aux_man_mode() already implement, chunked to the manual-mode
 * burst length. bmi270_handle_t IS a `struct bmi2_dev *` (bmi270_api.h says
 * so), so it goes straight through as intf_ptr.                            */

int8_t mag_aux_read(uint8_t reg_addr, uint8_t *reg_data, uint32_t length, void *intf_ptr)
{
    struct bmi2_dev *dev = (struct bmi2_dev *)intf_ptr;
    if (dev == nullptr) {
        return BMM150_E_COM_FAIL;
    }
    return bmi2_read_aux_man_mode(reg_addr, reg_data, (uint16_t)length, dev) == BMI2_OK
               ? BMM150_INTF_RET_SUCCESS : BMM150_E_COM_FAIL;
}

int8_t mag_aux_write(uint8_t reg_addr, const uint8_t *reg_data, uint32_t length, void *intf_ptr)
{
    struct bmi2_dev *dev = (struct bmi2_dev *)intf_ptr;
    if (dev == nullptr) {
        return BMM150_E_COM_FAIL;
    }
    return bmi2_write_aux_man_mode(reg_addr, reg_data, (uint16_t)length, dev) == BMI2_OK
               ? BMM150_INTF_RET_SUCCESS : BMM150_E_COM_FAIL;
}

void mag_delay_us(uint32_t period_us, void *)
{
    /* Init-time only (start-up and mode-change waits of 1-3 ms). Anything
     * a tick or longer yields; the short ones busy-wait, same as the
     * BMI270 wrapper's own delay.                                        */
    if (period_us >= 1000u * portTICK_PERIOD_MS) {
        vTaskDelay(pdMS_TO_TICKS((period_us + 999) / 1000));
    } else {
        esp_rom_delay_us(period_us);
    }
}

/* One aux configuration for both phases: identical except for manual_en.
 * read_addr / aux_rd_burst describe what the BMI270 fetches by itself once
 * manual mode is off -- the BMM150's eight data registers starting at
 * DATA_X_LSB, which is exactly the buffer bmm150_aux_mag_data() decodes.   */
void mag_aux_config(struct bmi2_sens_config *cfg, bool manual)
{
    cfg->type                    = BMI2_AUX;
    cfg->cfg.aux.aux_en          = BMI2_ENABLE;
    cfg->cfg.aux.manual_en       = manual ? BMI2_ENABLE : BMI2_DISABLE;
    cfg->cfg.aux.fcu_write_en    = BMI2_ENABLE;
    cfg->cfg.aux.man_rd_burst    = BMI2_AUX_READ_LEN_3;   /* 8 bytes */
    cfg->cfg.aux.aux_rd_burst    = BMI2_AUX_READ_LEN_3;   /* 8 bytes */
    cfg->cfg.aux.odr             = kMagAuxOdr;
    cfg->cfg.aux.offset          = 0;
    cfg->cfg.aux.i2c_device_addr = kMagI2cAddr;
    cfg->cfg.aux.read_addr       = BMM150_REG_DATA_X_LSB;
}

esp_err_t configure_magnetometer()
{
    /* 1. Pull-ups on the aux SDA line. The BMM150 footprint has none of its
     *    own on this board; without these every aux transfer reads 0xFF.  */
    uint8_t trim = kAuxPullUp;
    int8_t rslt = bmi2_set_regs(BMI2_AUX_IF_TRIM, &trim, 1, s_sensor);
    if (rslt != BMI2_OK) {
        ESP_LOGW(TAG, "BMM150: aux pull-up write failed: %d", rslt);
        return ESP_FAIL;
    }

    /* 2. Aux in manual mode, and switched on. Accel and gyro were enabled in
     *    configure_sensor(); bmi270_sensor_enable() only ORs the aux bit in,
     *    so they stay running.                                             */
    struct bmi2_sens_config cfg = {};
    mag_aux_config(&cfg, true);
    rslt = bmi2_set_sensor_config(&cfg, 1, s_sensor);
    if (rslt != BMI2_OK) {
        ESP_LOGW(TAG, "BMM150: aux (manual) config failed: %d", rslt);
        return ESP_FAIL;
    }
    uint8_t aux_list[1] = { BMI2_AUX };
    rslt = bmi270_sensor_enable(aux_list, 1, s_sensor);
    if (rslt != BMI2_OK) {
        ESP_LOGW(TAG, "BMM150: aux enable failed: %d", rslt);
        return ESP_FAIL;
    }

    /* 3. The BMM150 over the relay. It powers up in suspend, where every
     *    register but POWER_CONTROL reads as zero; bmm150_init() sets that
     *    bit first, waits 3 ms, then reads the chip id and the trim data.
     *    A chip id of 0x00 after that means the aux bus is not reaching it
     *    (pull-ups, address, or no chip); 0xFF means SDA is floating high. */
    s_mag = {};
    s_mag.intf     = BMM150_I2C_INTF;
    s_mag.intf_ptr = s_sensor;
    s_mag.read     = mag_aux_read;
    s_mag.write    = mag_aux_write;
    s_mag.delay_us = mag_delay_us;

    rslt = bmm150_init(&s_mag);
    if (rslt != BMM150_OK || s_mag.chip_id != BMM150_CHIP_ID) {
        /* Once more after a soft reset -- the ESP-Claw backend needed this
         * on a chip that had been left in an odd state by a previous boot. */
        (void)bmm150_soft_reset(&s_mag);
        mag_delay_us(BMM150_START_UP_TIME, nullptr);
        rslt = bmm150_init(&s_mag);
    }
    if (rslt != BMM150_OK || s_mag.chip_id != BMM150_CHIP_ID) {
        ESP_LOGW(TAG, "BMM150 not found on the BMI270 aux bus (init %d, chip id 0x%02X, "
                      "expected 0x%02X) -- magnetometer disabled",
                 rslt, (unsigned)s_mag.chip_id, (unsigned)BMM150_CHIP_ID);
        return ESP_ERR_NOT_FOUND;
    }

    struct bmm150_settings settings = {};
    settings.pwr_mode = BMM150_POWERMODE_NORMAL;
    rslt = bmm150_set_op_mode(&settings, &s_mag);
    if (rslt == BMM150_OK) {
        /* REGULAR preset = 9/15 repetitions, 10 Hz; then lift the rate so a
         * 20 Hz poll never sees the same conversion twice.                 */
        settings.preset_mode = BMM150_PRESETMODE_REGULAR;
        rslt = bmm150_set_presetmode(&settings, &s_mag);
    }
    if (rslt == BMM150_OK) {
        settings.data_rate = kMagDataRate;
        rslt = bmm150_set_sensor_settings(BMM150_SEL_DATA_RATE, &settings, &s_mag);
    }
    if (rslt != BMM150_OK) {
        ESP_LOGW(TAG, "BMM150: power/preset setup failed: %d -- magnetometer disabled", rslt);
        return ESP_FAIL;
    }

    /* 4. Hand the bus to the BMI270: manual off, and it polls DATA_X_LSB..
     *    RHALL_MSB at the aux ODR into the aux data registers.             */
    mag_aux_config(&cfg, false);
    rslt = bmi2_set_sensor_config(&cfg, 1, s_sensor);
    if (rslt != BMI2_OK) {
        ESP_LOGW(TAG, "BMM150: aux (data mode) config failed: %d -- magnetometer disabled", rslt);
        return ESP_FAIL;
    }

    s_mag_ready = true;
    ESP_LOGI(TAG, "BMM150 ready behind the BMI270 aux bus (addr 0x%02X, chip id 0x%02X, "
                  "%d Hz conversions)",
             (unsigned)kMagI2cAddr, (unsigned)s_mag.chip_id, 30);
    return ESP_OK;
}

void poll_task(void *)
{
    for (;;) {
        struct bmi2_sens_data data = {};
        if (bmi2_get_sensor_data(&data, s_sensor) == BMI2_OK) {
            ImuData sample;
            sample.ax = (float)data.acc.x / kInt16FullScale * kAccelRangeG;
            sample.ay = (float)data.acc.y / kInt16FullScale * kAccelRangeG;
            sample.az = (float)data.acc.z / kInt16FullScale * kAccelRangeG;
            sample.gx = (float)data.gyr.x / kInt16FullScale * kGyroRangeDps;
            sample.gy = (float)data.gyr.y / kInt16FullScale * kGyroRangeDps;
            sample.gz = (float)data.gyr.z / kInt16FullScale * kGyroRangeDps;

            if (s_mag_ready) {
                /* 5. Eight raw bytes -> compensated uT (int16 build of the
                 *    Bosch driver: 1 uT resolution, plenty for a heading).
                 *    The call masks the buffer in place, hence the copy.  */
                uint8_t raw[BMI2_AUX_NUM_BYTES];
                memcpy(raw, data.aux_data, sizeof(raw));
                struct bmm150_mag_data mag = {};
                if (bmm150_aux_mag_data(raw, &mag, &s_mag) == BMM150_OK) {
                    sample.mx = (float)mag.x;
                    sample.my = (float)mag.y;
                    sample.mz = (float)mag.z;
                    sample.mag_valid = true;
                }
            }

            sample.t_us = esp_timer_get_time();
            if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
                sample.seq = s_latest.seq + 1;
                s_latest = sample;
                xSemaphoreGive(s_mutex);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(kPollPeriodMs));
    }
}

void cleanup()
{
    if (s_sensor != nullptr) {
        bmi270_sensor_del(&s_sensor);
        s_sensor = nullptr;
    }
    if (s_i2c_bus != nullptr) {
        i2c_bus_delete(&s_i2c_bus);
        s_i2c_bus = nullptr;
    }
    if (s_periph_ref && s_periph_name != nullptr) {
        esp_board_periph_unref_handle(s_periph_name);
        s_periph_ref = false;
    }
}

/* Say which address the chip is actually on, before trying to talk to it.
 *
 * bmi270_sensor_create() takes no address -- the driver picks one, and its own
 * header calls 0x68 "typical". This board straps SDO high, so the chip is on
 * 0x69. If those disagree, create() fails with a bare ESP_FAIL and no hint
 * that addressing is the problem, which is a genuinely horrible thing to debug
 * on a bench with a scope.
 *
 * A bus scan costs a few milliseconds once at boot and turns that into a
 * sentence. It also prints every other device on the bus, which is the first
 * thing anyone wants when a board is new.
 */
void probe_i2c(int8_t declared_addr)
{
    uint8_t found[16] = {0};
    char list[96];
    size_t at = 0;
    bool at_declared = false;
    uint8_t n = i2c_bus_scan(s_i2c_bus, found, sizeof(found));

    for (uint8_t i = 0; i < n && at + 6 < sizeof(list); i++) {
        at += (size_t)snprintf(list + at, sizeof(list) - at, "%s0x%02X",
                               i ? " " : "", found[i]);
        if (found[i] == (uint8_t)declared_addr) {
            at_declared = true;
        }
    }
    ESP_LOGI(TAG, "I2C scan: %u device(s) [%s]", (unsigned)n, n ? list : "none");

    if (!at_declared) {
        ESP_LOGW(TAG, "Nothing answered at 0x%02X, the address board_devices.yaml "
                      "declares for the BMI270 -- check the SDO strap",
                 (unsigned)(uint8_t)declared_addr);
    }
}

}  // namespace

bool imu_init()
{
    if (s_ready) {
        ESP_LOGW(TAG, "imu_init() called twice — ignoring");
        return true;
    }

    const mpx_imu_board_cfg_t *cfg = nullptr;
    esp_err_t err = resolve_board_cfg(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Board device '%s' not usable: %s",
                 kDeviceName, esp_err_to_name(err));
        return false;
    }
    if (cfg->peripheral_count == 0 || cfg->peripheral_name == nullptr) {
        ESP_LOGE(TAG, "Board device '%s' declares no I2C peripheral", kDeviceName);
        return false;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == nullptr) {
        ESP_LOGE(TAG, "mutex alloc failed");
        return false;
    }

    if (open_i2c_bus(cfg->peripheral_name, cfg->frequency) != ESP_OK) {
        cleanup();
        return false;
    }

    probe_i2c(cfg->i2c_addr);

    /* bmi270_sensor_create() uploads the ~8 KB configuration blob, which is
     * why board_devices.yaml marks this device init_skip: true and we do it
     * here rather than on the board manager's boot path.                  */
    err = bmi270_sensor_create(s_i2c_bus, &s_sensor, bmi270_config_file,
                               BMI2_GYRO_CROSS_SENS_ENABLE | BMI2_CRT_RTOSK_ENABLE);
    if (err != ESP_OK || s_sensor == nullptr) {
        ESP_LOGE(TAG, "bmi270_sensor_create failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "If the scan above found the chip at an address other "
                      "than the one this driver uses, that is the reason: "
                      "bmi270_sensor_create() takes no address argument.");
        cleanup();
        return false;
    }

    if (configure_sensor() != ESP_OK) {
        cleanup();
        return false;
    }

    /* Accel and gyro are up. The magnetometer is optional from here on:
     * whatever goes wrong inside is already logged, and the poll task simply
     * never sets mag_valid.                                                */
    (void)configure_magnetometer();

    if (xTaskCreate(poll_task, "mpx_imu", kTaskStackBytes, nullptr,
                    kTaskPriority, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "poll task create failed");
        cleanup();
        return false;
    }

    s_ready = true;
    ESP_LOGI(TAG, "BMI270 ready on I2C '%s' addr 0x%02X (+/-%.0fg, +/-%.0fdps, %d Hz poll)",
             cfg->peripheral_name, (unsigned)(uint8_t)cfg->i2c_addr,
             (double)kAccelRangeG, (double)kGyroRangeDps, 1000 / kPollPeriodMs);
    return true;
}

ImuData imu_read()
{
    ImuData copy;
    if (s_mutex != nullptr && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        copy = s_latest;
        xSemaphoreGive(s_mutex);
    }
    return copy;
}

bool imu_mag_ready()
{
    return s_mag_ready;
}

void imu_print()
{
    const ImuData d = imu_read();
    if (d.mag_valid) {
        ESP_LOGI(TAG, "accel %.3f %.3f %.3f g   gyro %.2f %.2f %.2f dps   mag %.0f %.0f %.0f uT",
                 (double)d.ax, (double)d.ay, (double)d.az,
                 (double)d.gx, (double)d.gy, (double)d.gz,
                 (double)d.mx, (double)d.my, (double)d.mz);
    } else {
        ESP_LOGI(TAG, "accel %.3f %.3f %.3f g   gyro %.2f %.2f %.2f dps   (no magnetometer)",
                 (double)d.ax, (double)d.ay, (double)d.az,
                 (double)d.gx, (double)d.gy, (double)d.gz);
    }
}

}  // namespace robot
