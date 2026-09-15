#pragma once

#include <driver/i2c_master.h>

/*
 * One I2C master bus, shared.
 *
 * NOTE ON THE NAME. This was main/hal/i2c_bus.h, and both halves of that were
 * a mistake:
 *
 *   - `i2c_bus.h` is the header of espressif/i2c_bus, the LEGACY I2C wrapper.
 *     The Bosch BMI270 component's bmi2.h includes it by that exact name, and
 *     because main/hal sat earlier on the include path, bmi2.h got THIS file
 *     instead. The symptom was bmi270.h failing on i2c_bus_handle_t while the
 *     real declaration sat on disk, on the include path, being shadowed.
 *
 *   - `hal/` is ESP-IDF's own component directory; its headers are included as
 *     hal/gpio_hal.h, hal/i2c_types.h and so on. Putting main/hal on the
 *     include path shadowed that whole namespace, so the next collision was
 *     only a matter of time.
 *
 * The directory is no longer on the include path at all — this is reached as
 * "platform/shared_i2c.h" through main/ itself.
 *
 * On this board SDA/SCL (IO1/IO2) are shared by the ES7210 audio ADC and the
 * BMI270 + BMM150 IMU. ESP-IDF's i2c_master driver allows exactly one bus
 * handle per port, so whichever subsystem initialises first would lock the
 * other out with ESP_ERR_INVALID_STATE.
 *
 * In this firmware only the IMU is actually built — the audio codec is not
 * part of md-mpx-dog, which is headless and uses the PWA over Wi-Fi as its
 * UI. The shared handle stays anyway: it is a dozen lines, and the
 * alternative is rediscovering the one-handle-per-port rule the day a codec
 * or a second sensor is added.
 *
 * Callers therefore go through hal::i2c_bus(), which creates the bus on the
 * first call and returns the same handle afterwards. It is safe to call from
 * either subsystem, in either order, and before or after the scheduler
 * starts.
 */
namespace hal {

/**
 * @brief Return the shared I2C master bus handle, creating it on first call.
 * @return The bus handle, or nullptr if the bus could not be created.
 */
i2c_master_bus_handle_t i2c_bus();

}  // namespace hal
