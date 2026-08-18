#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the M5Stack StopWatch power-management IC on an existing I2C bus.
// The PM1 owns the power-key wake path and the 3.3 V rail used by M5IOE1.
esp_err_t openpencil_m5pm1_init(i2c_master_bus_handle_t bus);

#ifdef __cplusplus
}
#endif
