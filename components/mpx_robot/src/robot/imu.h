#pragma once

#include <cstdint>

namespace robot {

/**
 * @brief IMU data snapshot.
 *
 * Accelerometer in g, gyroscope in degrees per second, magnetometer in
 * microtesla. The magnetometer fields are only meaningful when mag_valid is
 * true — see imu_init() for when that happens.
 */
struct ImuData {
    float ax = 0.0f;  ///< Accel X (g)
    float ay = 0.0f;  ///< Accel Y (g)
    float az = 0.0f;  ///< Accel Z (g)
    float gx = 0.0f;  ///< Gyro X (dps)
    float gy = 0.0f;  ///< Gyro Y (dps)
    float gz = 0.0f;  ///< Gyro Z (dps)
    float mx = 0.0f;  ///< Mag X (uT), valid only if mag_valid
    float my = 0.0f;  ///< Mag Y (uT), valid only if mag_valid
    float mz = 0.0f;  ///< Mag Z (uT), valid only if mag_valid
    bool  mag_valid = false;  ///< True once the BMM150 has been read at least once
    uint32_t seq  = 0;        ///< +1 per new sample (0 = never sampled): "is this one fresh?"
    int64_t  t_us = 0;        ///< esp_timer time of the sample
};

/**
 * @brief Initialise the IMU.
 *
 * MP4-ESP32-CORE carries a BMI270 (accel + gyro) at 0x68 on the shared host
 * I2C bus, with a BMM150 magnetometer hanging off the BMI270's auxiliary I2C
 * interface (ASDX/ASCX) rather than off the host bus — the host reaches the
 * BMM150 only through the BMI270. This replaces the Mini Pupper 2's QMI8658C,
 * which was on SPI.
 *
 * Steps performed:
 *  - Attaches to the shared I2C bus from hal::i2c_bus()
 *  - Runs bmi270_init(), which uploads the 8 KB Bosch configuration blob
 *  - Enables accel + gyro at 100 Hz, and the AUX interface if the BMM150
 *    answers
 *  - Starts a background FreeRTOS task that polls at 20 Hz and caches the
 *    latest sample
 *
 * If the BMM150 does not answer, accel and gyro still come up and mag_valid
 * stays false — a dead magnetometer must not cost the robot its gait.
 *
 * @return true if accel + gyro are running, false if the BMI270 itself failed.
 */
bool imu_init();

/**
 * @brief Return a copy of the latest IMU sample (thread-safe).
 */
ImuData imu_read();

/** BMM150 found behind the BMI270 and sampled. */
bool imu_mag_ready();

/**
 * @brief Print the latest IMU data via ESP_LOGI.
 */
void imu_print();

}  // namespace robot
