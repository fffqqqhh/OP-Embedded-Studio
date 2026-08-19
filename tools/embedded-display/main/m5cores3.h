/* SPDX-License-Identifier: CC0-1.0 */
#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

esp_err_t openpencil_m5cores3_display_init(void);
esp_err_t openpencil_m5cores3_lcd_reset(void);
esp_err_t openpencil_m5cores3_touch_reset(void);
esp_err_t openpencil_m5cores3_read_power_register(uint8_t reg, uint8_t *value);
i2c_master_bus_handle_t openpencil_m5cores3_i2c_bus(void);
