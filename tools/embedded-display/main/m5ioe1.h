#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t openpencil_m5ioe1_display_init(void);
esp_err_t openpencil_m5ioe1_display_power_down(void);
esp_err_t openpencil_m5ioe1_touch_reset(void);
i2c_master_bus_handle_t openpencil_m5ioe1_i2c_bus(void);

#ifdef __cplusplus
}
#endif
