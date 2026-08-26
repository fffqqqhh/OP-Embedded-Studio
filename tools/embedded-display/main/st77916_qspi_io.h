/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int cs_gpio_num;
    int pclk_hz;
    int spi_mode;
    int trans_queue_depth;
    size_t max_transfer_bytes;
} example_lcd_st77916_qspi_io_config_t;

esp_err_t example_lcd_new_panel_io_st77916_qspi(spi_host_device_t host,
                                                const example_lcd_st77916_qspi_io_config_t *io_config,
                                                esp_lcd_panel_io_handle_t *ret_io);

#ifdef __cplusplus
}
#endif
