/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "esp_lcd_panel_vendor.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t esp_lcd_new_panel_st77916(const esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel);

esp_err_t esp_lcd_panel_st77916_set_vcom(esp_lcd_panel_handle_t panel, uint8_t vcom);
esp_err_t esp_lcd_panel_st77916_set_power_b2(esp_lcd_panel_handle_t panel, uint8_t value);

#ifdef __cplusplus
}
#endif
