/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/cdefs.h>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io_interface.h"
#include "esp_log.h"
#include "st77916_qspi_io.h"

#define ST77916_QSPI_WRITE_CMD   0x02
#define ST77916_QSPI_WRITE_COLOR 0x32
#define ST77916_QSPI_PREFIX_BYTES 4

static const char *TAG = "st77916_qspi_io";

typedef struct {
    esp_lcd_panel_io_t base;
    spi_device_handle_t spi_dev;
    spi_host_device_t host;
    esp_lcd_panel_io_color_trans_done_cb_t on_color_trans_done;
    void *user_ctx;
    size_t max_transfer_bytes;
} st77916_qspi_io_t;

static esp_err_t st77916_qspi_io_rx_param(esp_lcd_panel_io_t *io, int lcd_cmd, void *param, size_t param_size);
static esp_err_t st77916_qspi_io_tx_param(esp_lcd_panel_io_t *io, int lcd_cmd, const void *param, size_t param_size);
static esp_err_t st77916_qspi_io_tx_color(esp_lcd_panel_io_t *io, int lcd_cmd, const void *color, size_t color_size);
static esp_err_t st77916_qspi_io_del(esp_lcd_panel_io_t *io);
static esp_err_t st77916_qspi_io_register_event_callbacks(esp_lcd_panel_io_t *io,
                                                          const esp_lcd_panel_io_callbacks_t *cbs,
                                                          void *user_ctx);

static esp_err_t st77916_qspi_write(st77916_qspi_io_t *qspi_io, uint8_t opcode, int lcd_cmd,
                                    const void *data, size_t data_size, bool qio_data)
{
    ESP_RETURN_ON_FALSE(lcd_cmd >= 0 && lcd_cmd <= 0xFF, ESP_ERR_INVALID_ARG, TAG, "invalid LCD command");
    ESP_RETURN_ON_FALSE(data || data_size == 0, ESP_ERR_INVALID_ARG, TAG, "invalid data buffer");
    ESP_RETURN_ON_FALSE(data_size <= SIZE_MAX - ST77916_QSPI_PREFIX_BYTES, ESP_ERR_INVALID_SIZE, TAG, "data buffer too large");

    const size_t tx_size = ST77916_QSPI_PREFIX_BYTES + data_size;
    spi_transaction_t trans = {
        .length = tx_size * 8,
    };

    uint8_t inline_tx[ST77916_QSPI_PREFIX_BYTES + sizeof(trans.tx_data)] = {
        opcode, 0x00, lcd_cmd, 0x00,
    };
    void *dma_tx = NULL;
    if (data_size > 0) {
        memcpy(inline_tx + ST77916_QSPI_PREFIX_BYTES,
               data,
               data_size < sizeof(trans.tx_data) ? data_size : sizeof(trans.tx_data));
    }

    if (tx_size <= sizeof(trans.tx_data)) {
        trans.flags |= SPI_TRANS_USE_TXDATA;
        memcpy(trans.tx_data, inline_tx, tx_size);
    } else {
        dma_tx = heap_caps_malloc(tx_size, MALLOC_CAP_DMA);
        ESP_RETURN_ON_FALSE(dma_tx, ESP_ERR_NO_MEM, TAG, "no memory for SPI transfer");
        memcpy(dma_tx, inline_tx, ST77916_QSPI_PREFIX_BYTES);
        memcpy((uint8_t *)dma_tx + ST77916_QSPI_PREFIX_BYTES, data, data_size);
        trans.tx_buffer = dma_tx;
    }

    (void)qio_data;

    esp_err_t ret = spi_device_polling_transmit(qspi_io->spi_dev, &trans);
    free(dma_tx);
    return ret;
}

static esp_err_t st77916_qspi_write_color_qio(st77916_qspi_io_t *qspi_io, int lcd_cmd,
                                              const void *data, size_t data_size)
{
    ESP_RETURN_ON_FALSE(lcd_cmd >= 0 && lcd_cmd <= 0xFF, ESP_ERR_INVALID_ARG, TAG, "invalid LCD command");
    ESP_RETURN_ON_FALSE(data && data_size > 0, ESP_ERR_INVALID_ARG, TAG, "invalid color buffer");
    ESP_RETURN_ON_FALSE(data_size <= UINT32_MAX / 8, ESP_ERR_INVALID_SIZE, TAG, "color buffer too large");

    spi_transaction_ext_t trans = {
        .base = {
            .flags = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_MODE_QIO,
            .cmd = ST77916_QSPI_WRITE_COLOR,
            .addr = ((uint32_t)lcd_cmd << 8),
            .length = data_size * 8,
            .tx_buffer = data,
        },
        .command_bits = 8,
        .address_bits = 24,
    };

    return spi_device_polling_transmit(qspi_io->spi_dev, (spi_transaction_t *)&trans);
}

static esp_err_t st77916_qspi_io_rx_param(esp_lcd_panel_io_t *io, int lcd_cmd, void *param, size_t param_size)
{
    (void)io;
    (void)lcd_cmd;
    (void)param;
    (void)param_size;
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t st77916_qspi_io_tx_param(esp_lcd_panel_io_t *io, int lcd_cmd, const void *param, size_t param_size)
{
    st77916_qspi_io_t *qspi_io = __containerof(io, st77916_qspi_io_t, base);
    return st77916_qspi_write(qspi_io, ST77916_QSPI_WRITE_CMD, lcd_cmd, param, param_size, false);
}

static esp_err_t st77916_qspi_io_tx_color(esp_lcd_panel_io_t *io, int lcd_cmd, const void *color, size_t color_size)
{
    st77916_qspi_io_t *qspi_io = __containerof(io, st77916_qspi_io_t, base);
    ESP_RETURN_ON_FALSE(color && color_size > 0, ESP_ERR_INVALID_ARG, TAG, "invalid color buffer");

    const uint8_t *color_bytes = color;
    size_t bytes_left = color_size;
    size_t host_max_transfer_bytes = 0;
    esp_err_t ret = spi_bus_get_max_transaction_len(qspi_io->host, &host_max_transfer_bytes);
    if (ret != ESP_OK || host_max_transfer_bytes == 0) {
        host_max_transfer_bytes = qspi_io->max_transfer_bytes;
    }
    const size_t configured_max_bytes = qspi_io->max_transfer_bytes ? qspi_io->max_transfer_bytes : color_size;
    const size_t max_transfer_bytes = host_max_transfer_bytes < configured_max_bytes ?
                                      host_max_transfer_bytes : configured_max_bytes;
    ESP_RETURN_ON_FALSE(max_transfer_bytes > 0, ESP_ERR_INVALID_ARG, TAG, "invalid max transfer length");

    while (bytes_left > 0) {
        size_t chunk_size = bytes_left > max_transfer_bytes ? max_transfer_bytes : bytes_left;
        ESP_RETURN_ON_ERROR(st77916_qspi_write_color_qio(qspi_io, lcd_cmd, color_bytes, chunk_size),
                            TAG, "write color failed");
        color_bytes += chunk_size;
        bytes_left -= chunk_size;
    }

    if (qspi_io->on_color_trans_done) {
        qspi_io->on_color_trans_done(&qspi_io->base, NULL, qspi_io->user_ctx);
    }
    return ESP_OK;
}

static esp_err_t st77916_qspi_io_del(esp_lcd_panel_io_t *io)
{
    st77916_qspi_io_t *qspi_io = __containerof(io, st77916_qspi_io_t, base);
    ESP_RETURN_ON_ERROR(spi_bus_remove_device(qspi_io->spi_dev), TAG, "remove SPI device failed");
    free(qspi_io);
    return ESP_OK;
}

static esp_err_t st77916_qspi_io_register_event_callbacks(esp_lcd_panel_io_t *io,
                                                          const esp_lcd_panel_io_callbacks_t *cbs,
                                                          void *user_ctx)
{
    ESP_RETURN_ON_FALSE(cbs, ESP_ERR_INVALID_ARG, TAG, "invalid callbacks");
    st77916_qspi_io_t *qspi_io = __containerof(io, st77916_qspi_io_t, base);
    if (qspi_io->on_color_trans_done) {
        ESP_LOGW(TAG, "Callback on_color_trans_done was already set and now it was overwritten");
    }
    qspi_io->on_color_trans_done = cbs->on_color_trans_done;
    qspi_io->user_ctx = user_ctx;
    return ESP_OK;
}

esp_err_t example_lcd_new_panel_io_st77916_qspi(spi_host_device_t host,
                                                const example_lcd_st77916_qspi_io_config_t *io_config,
                                                esp_lcd_panel_io_handle_t *ret_io)
{
    ESP_RETURN_ON_FALSE(io_config && ret_io, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    st77916_qspi_io_t *qspi_io = calloc(1, sizeof(st77916_qspi_io_t));
    ESP_RETURN_ON_FALSE(qspi_io, ESP_ERR_NO_MEM, TAG, "no memory for ST77916 QSPI IO");

    qspi_io->base.rx_param = st77916_qspi_io_rx_param;
    qspi_io->base.tx_param = st77916_qspi_io_tx_param;
    qspi_io->base.tx_color = st77916_qspi_io_tx_color;
    qspi_io->base.del = st77916_qspi_io_del;
    qspi_io->base.register_event_callbacks = st77916_qspi_io_register_event_callbacks;
    qspi_io->host = host;
    qspi_io->max_transfer_bytes = io_config->max_transfer_bytes;

    spi_device_interface_config_t devcfg = {
        .mode = io_config->spi_mode,
        .clock_speed_hz = io_config->pclk_hz,
        .spics_io_num = io_config->cs_gpio_num,
        .queue_size = io_config->trans_queue_depth,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };

    esp_err_t ret = spi_bus_add_device(host, &devcfg, &qspi_io->spi_dev);
    if (ret != ESP_OK) {
        free(qspi_io);
        ESP_LOGE(TAG, "add ST77916 QSPI device failed: %s", esp_err_to_name(ret));
        return ret;
    }

    *ret_io = &qspi_io->base;
    return ESP_OK;
}
