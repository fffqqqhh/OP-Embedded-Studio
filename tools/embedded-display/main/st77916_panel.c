/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdlib.h>
#include <sys/cdefs.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "st77916_panel.h"

static const char *TAG = "lcd_panel.st77916";

#define ST77916_MAX_INIT_PARAM_BYTES 14

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    gpio_num_t reset_gpio_num;
    bool reset_level;
    int x_gap;
    int y_gap;
    uint8_t madctl_val;
    uint8_t fb_bits_per_pixel;
} st77916_panel_t;

typedef struct {
    uint8_t cmd;
    uint8_t data[ST77916_MAX_INIT_PARAM_BYTES];
    uint8_t data_bytes;
    uint16_t delay_ms;
} st77916_lcd_init_cmd_t;

static esp_err_t panel_st77916_del(esp_lcd_panel_t *panel);
static esp_err_t panel_st77916_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_st77916_init(esp_lcd_panel_t *panel);
static esp_err_t panel_st77916_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end,
                                           const void *color_data);
static esp_err_t panel_st77916_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_st77916_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_st77916_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_st77916_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_st77916_disp_on_off(esp_lcd_panel_t *panel, bool on_off);
static esp_err_t panel_st77916_sleep(esp_lcd_panel_t *panel, bool sleep);

static esp_err_t panel_st77916_select_vendor_page(esp_lcd_panel_io_handle_t io)
{
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xF0, (uint8_t[]) { 0x00 }, 1),
                        TAG, "select page latch failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xF0, (uint8_t[]) { 0x01 }, 1),
                        TAG, "select vendor page failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xF1, (uint8_t[]) { 0x01 }, 1),
                        TAG, "select vendor bank failed");
    return ESP_OK;
}

static esp_err_t panel_st77916_leave_vendor_page(esp_lcd_panel_io_handle_t io)
{
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xF1, (uint8_t[]) { 0x10 }, 1),
                        TAG, "leave vendor bank failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xF0, (uint8_t[]) { 0x00 }, 1),
                        TAG, "leave vendor page failed");
    return ESP_OK;
}

static esp_err_t panel_st77916_set_window(esp_lcd_panel_io_handle_t io, int x_start, int y_start, int x_end, int y_end)
{
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_CASET, (uint8_t[]) {
        (x_start >> 8) & 0xFF, x_start & 0xFF,
        ((x_end - 1) >> 8) & 0xFF, (x_end - 1) & 0xFF,
    }, 4), TAG, "set column address failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_RASET, (uint8_t[]) {
        (y_start >> 8) & 0xFF, y_start & 0xFF,
        ((y_end - 1) >> 8) & 0xFF, (y_end - 1) & 0xFF,
    }, 4), TAG, "set row address failed");
    return ESP_OK;
}

static const st77916_lcd_init_cmd_t vendor_init_cmds[] = {
    { 0xF0, { 0x08 }, 1, 0 },
    { 0xF2, { 0x08 }, 1, 0 },
    { 0x9B, { 0x51 }, 1, 0 },
    { 0x86, { 0x53 }, 1, 0 },
    { 0xF2, { 0x80 }, 1, 0 },
    { 0xF0, { 0x00 }, 1, 0 },
    { 0xF0, { 0x28 }, 1, 0 },
    { 0xF2, { 0x28 }, 1, 0 },
    { 0x83, { 0xE0 }, 1, 0 },
    { 0x84, { 0x61 }, 1, 0 },
    { 0xF2, { 0x82 }, 1, 0 },
    { 0xF0, { 0x00 }, 1, 0 },
    { 0xF0, { 0x01 }, 1, 0 },
    { 0xF1, { 0x01 }, 1, 0 },
    { 0xB0, { 0x55 }, 1, 0 },
    { 0xB1, { 0x1E }, 1, 0 },
    { 0xB2, { 0x3B }, 1, 0 },
    { 0xB4, { 0x06 }, 1, 0 },
    { 0xB5, { 0x24 }, 1, 0 },
    { 0xB6, { 0xA5 }, 1, 0 },
    { 0xB7, { 0x10 }, 1, 0 },
    { 0xBA, { 0x00 }, 1, 0 },
    { 0xBB, { 0x08 }, 1, 0 },
    { 0xBC, { 0x08 }, 1, 0 },
    { 0xBD, { 0x00 }, 1, 0 },
    { 0xC0, { 0x80 }, 1, 0 },
    { 0xC1, { 0x10 }, 1, 0 },
    { 0xC2, { 0x37 }, 1, 0 },
    { 0xC3, { 0x80 }, 1, 0 },
    { 0xC4, { 0x10 }, 1, 0 },
    { 0xC5, { 0x37 }, 1, 0 },
    { 0xC6, { 0xA9 }, 1, 0 },
    { 0xC7, { 0x41 }, 1, 0 },
    { 0xC8, { 0x51 }, 1, 0 },
    { 0xC9, { 0xA9 }, 1, 0 },
    { 0xCA, { 0x41 }, 1, 0 },
    { 0xCB, { 0x51 }, 1, 0 },
    { 0xD0, { 0x91 }, 1, 0 },
    { 0xD1, { 0x68 }, 1, 0 },
    { 0xD2, { 0x69 }, 1, 0 },
    { 0xF5, { 0x00, 0xA5 }, 2, 0 },
    { 0xDD, { 0x12 }, 1, 0 },
    { 0xDE, { 0x12 }, 1, 0 },
    { 0xF1, { 0x10 }, 1, 0 },
    { 0xF0, { 0x00 }, 1, 0 },
    { 0xF0, { 0x02 }, 1, 0 },
    { 0xE0, { 0xF0, 0x0B, 0x12, 0x0B, 0x0A, 0x06, 0x39, 0x43, 0x4F, 0x07, 0x14, 0x14, 0x2F, 0x34 }, 14, 0 },
    { 0xE1, { 0xF0, 0x0B, 0x11, 0x0A, 0x09, 0x05, 0x32, 0x33, 0x48, 0x07, 0x13, 0x13, 0x2C, 0x33 }, 14, 0 },
    { 0xF0, { 0x10 }, 1, 0 },
    { 0xF3, { 0x10 }, 1, 0 },
    { 0xE0, { 0x0A }, 1, 0 },
    { 0xE1, { 0x00 }, 1, 0 },
    { 0xE2, { 0x00 }, 1, 0 },
    { 0xE3, { 0x00 }, 1, 0 },
    { 0xE4, { 0xE0 }, 1, 0 },
    { 0xE5, { 0x06 }, 1, 0 },
    { 0xE6, { 0x21 }, 1, 0 },
    { 0xE7, { 0x00 }, 1, 0 },
    { 0xE8, { 0x05 }, 1, 0 },
    { 0xE9, { 0xF2 }, 1, 0 },
    { 0xEA, { 0xDF }, 1, 0 },
    { 0xEB, { 0x80 }, 1, 0 },
    { 0xEC, { 0x20 }, 1, 0 },
    { 0xED, { 0x14 }, 1, 0 },
    { 0xEE, { 0xFF }, 1, 0 },
    { 0xEF, { 0x00 }, 1, 0 },
    { 0xF8, { 0xFF }, 1, 0 },
    { 0xF9, { 0x00 }, 1, 0 },
    { 0xFA, { 0x00 }, 1, 0 },
    { 0xFB, { 0x30 }, 1, 0 },
    { 0xFC, { 0x00 }, 1, 0 },
    { 0xFD, { 0x00 }, 1, 0 },
    { 0xFE, { 0x00 }, 1, 0 },
    { 0xFF, { 0x00 }, 1, 0 },
    { 0x60, { 0x42 }, 1, 0 },
    { 0x61, { 0xE0 }, 1, 0 },
    { 0x62, { 0x40 }, 1, 0 },
    { 0x63, { 0x40 }, 1, 0 },
    { 0x64, { 0x02 }, 1, 0 },
    { 0x65, { 0x00 }, 1, 0 },
    { 0x66, { 0x40 }, 1, 0 },
    { 0x67, { 0x03 }, 1, 0 },
    { 0x68, { 0x00 }, 1, 0 },
    { 0x69, { 0x00 }, 1, 0 },
    { 0x6A, { 0x00 }, 1, 0 },
    { 0x6B, { 0x00 }, 1, 0 },
    { 0x70, { 0x42 }, 1, 0 },
    { 0x71, { 0xE0 }, 1, 0 },
    { 0x72, { 0x40 }, 1, 0 },
    { 0x73, { 0x40 }, 1, 0 },
    { 0x74, { 0x02 }, 1, 0 },
    { 0x75, { 0x00 }, 1, 0 },
    { 0x76, { 0x40 }, 1, 0 },
    { 0x77, { 0x03 }, 1, 0 },
    { 0x78, { 0x00 }, 1, 0 },
    { 0x79, { 0x00 }, 1, 0 },
    { 0x7A, { 0x00 }, 1, 0 },
    { 0x7B, { 0x00 }, 1, 0 },
    { 0x80, { 0x48 }, 1, 0 },
    { 0x81, { 0x00 }, 1, 0 },
    { 0x82, { 0x05 }, 1, 0 },
    { 0x83, { 0x02 }, 1, 0 },
    { 0x84, { 0xDD }, 1, 0 },
    { 0x85, { 0x00 }, 1, 0 },
    { 0x86, { 0x00 }, 1, 0 },
    { 0x87, { 0x00 }, 1, 0 },
    { 0x88, { 0x48 }, 1, 0 },
    { 0x89, { 0x00 }, 1, 0 },
    { 0x8A, { 0x07 }, 1, 0 },
    { 0x8B, { 0x02 }, 1, 0 },
    { 0x8C, { 0xDF }, 1, 0 },
    { 0x8D, { 0x00 }, 1, 0 },
    { 0x8E, { 0x00 }, 1, 0 },
    { 0x8F, { 0x00 }, 1, 0 },
    { 0x90, { 0x48 }, 1, 0 },
    { 0x91, { 0x00 }, 1, 0 },
    { 0x92, { 0x09 }, 1, 0 },
    { 0x93, { 0x02 }, 1, 0 },
    { 0x94, { 0xE1 }, 1, 0 },
    { 0x95, { 0x00 }, 1, 0 },
    { 0x96, { 0x00 }, 1, 0 },
    { 0x97, { 0x00 }, 1, 0 },
    { 0x98, { 0x48 }, 1, 0 },
    { 0x99, { 0x00 }, 1, 0 },
    { 0x9A, { 0x0B }, 1, 0 },
    { 0x9B, { 0x02 }, 1, 0 },
    { 0x9C, { 0xE3 }, 1, 0 },
    { 0x9D, { 0x00 }, 1, 0 },
    { 0x9E, { 0x00 }, 1, 0 },
    { 0x9F, { 0x00 }, 1, 0 },
    { 0xA0, { 0x48 }, 1, 0 },
    { 0xA1, { 0x00 }, 1, 0 },
    { 0xA2, { 0x04 }, 1, 0 },
    { 0xA3, { 0x02 }, 1, 0 },
    { 0xA4, { 0xDC }, 1, 0 },
    { 0xA5, { 0x00 }, 1, 0 },
    { 0xA6, { 0x00 }, 1, 0 },
    { 0xA7, { 0x00 }, 1, 0 },
    { 0xA8, { 0x48 }, 1, 0 },
    { 0xA9, { 0x00 }, 1, 0 },
    { 0xAA, { 0x06 }, 1, 0 },
    { 0xAB, { 0x02 }, 1, 0 },
    { 0xAC, { 0xDE }, 1, 0 },
    { 0xAD, { 0x00 }, 1, 0 },
    { 0xAE, { 0x00 }, 1, 0 },
    { 0xAF, { 0x00 }, 1, 0 },
    { 0xB0, { 0x48 }, 1, 0 },
    { 0xB1, { 0x00 }, 1, 0 },
    { 0xB2, { 0x08 }, 1, 0 },
    { 0xB3, { 0x02 }, 1, 0 },
    { 0xB4, { 0xE0 }, 1, 0 },
    { 0xB5, { 0x00 }, 1, 0 },
    { 0xB6, { 0x00 }, 1, 0 },
    { 0xB7, { 0x00 }, 1, 0 },
    { 0xB8, { 0x48 }, 1, 0 },
    { 0xB9, { 0x00 }, 1, 0 },
    { 0xBA, { 0x0A }, 1, 0 },
    { 0xBB, { 0x02 }, 1, 0 },
    { 0xBC, { 0xE2 }, 1, 0 },
    { 0xBD, { 0x00 }, 1, 0 },
    { 0xBE, { 0x00 }, 1, 0 },
    { 0xBF, { 0x00 }, 1, 0 },
    { 0xC0, { 0x22 }, 1, 0 },
    { 0xC1, { 0x98 }, 1, 0 },
    { 0xC2, { 0x65 }, 1, 0 },
    { 0xC3, { 0x74 }, 1, 0 },
    { 0xC4, { 0x47 }, 1, 0 },
    { 0xC5, { 0x56 }, 1, 0 },
    { 0xC6, { 0x00 }, 1, 0 },
    { 0xC7, { 0xBA }, 1, 0 },
    { 0xC8, { 0xAB }, 1, 0 },
    { 0xC9, { 0x33 }, 1, 0 },
    { 0xD0, { 0x11 }, 1, 0 },
    { 0xD1, { 0x98 }, 1, 0 },
    { 0xD2, { 0x65 }, 1, 0 },
    { 0xD3, { 0x74 }, 1, 0 },
    { 0xD4, { 0x47 }, 1, 0 },
    { 0xD5, { 0x56 }, 1, 0 },
    { 0xD6, { 0x00 }, 1, 0 },
    { 0xD7, { 0xBA }, 1, 0 },
    { 0xD8, { 0xAB }, 1, 0 },
    { 0xD9, { 0x33 }, 1, 0 },
    { 0xF3, { 0x01 }, 1, 0 },
    { 0xF0, { 0x00 }, 1, 0 },
    { 0xF0, { 0x01 }, 1, 0 },
    { 0xF1, { 0x01 }, 1, 0 },
    { 0xA0, { 0x0B }, 1, 0 },
    { 0xA3, { 0x2A }, 1, 0 },
    { 0xA5, { 0xC3 }, 1, 1 },
    { 0xA3, { 0x2B }, 1, 0 },
    { 0xA5, { 0xC3 }, 1, 1 },
    { 0xA3, { 0x2C }, 1, 0 },
    { 0xA5, { 0xC3 }, 1, 1 },
    { 0xA3, { 0x2D }, 1, 0 },
    { 0xA5, { 0xC3 }, 1, 1 },
    { 0xA3, { 0x2E }, 1, 0 },
    { 0xA5, { 0xC3 }, 1, 1 },
    { 0xA3, { 0x2F }, 1, 0 },
    { 0xA5, { 0xC3 }, 1, 1 },
    { 0xA3, { 0x30 }, 1, 0 },
    { 0xA5, { 0xC3 }, 1, 1 },
    { 0xA3, { 0x31 }, 1, 0 },
    { 0xA5, { 0xC3 }, 1, 1 },
    { 0xA3, { 0x32 }, 1, 0 },
    { 0xA5, { 0xC3 }, 1, 1 },
    { 0xA3, { 0x33 }, 1, 0 },
    { 0xA5, { 0xC3 }, 1, 1 },
    { 0xA0, { 0x09 }, 1, 0 },
    { 0xF1, { 0x10 }, 1, 0 },
    { 0xF0, { 0x00 }, 1, 0 },
    { 0x2A, { 0x00, 0x00, 0x01, 0x67 }, 4, 0 },
    { 0x2B, { 0x01, 0x68, 0x01, 0x68 }, 4, 0 },
    { 0x4D, { 0x00 }, 1, 0 },
    { 0x4E, { 0x00 }, 1, 0 },
    { 0x4F, { 0x00 }, 1, 0 },
    { 0x4C, { 0x01 }, 1, 10 },
    { 0x4C, { 0x00 }, 1, 0 },
    { 0x2A, { 0x00, 0x00, 0x01, 0x67 }, 4, 0 },
    { 0x2B, { 0x00, 0x00, 0x01, 0x67 }, 4, 0 },
    { 0x21, { }, 0, 0 },
    { 0x11, { }, 0, 120 },
    { 0x29, { }, 0, 0 },
};

esp_err_t esp_lcd_new_panel_st77916(const esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = ESP_OK;
    st77916_panel_t *st77916 = NULL;
    ESP_GOTO_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");

    st77916 = calloc(1, sizeof(st77916_panel_t));
    ESP_GOTO_ON_FALSE(st77916, ESP_ERR_NO_MEM, err, TAG, "no mem for st77916 panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure reset GPIO failed");
    }

    switch (panel_dev_config->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        st77916->madctl_val = 0;
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        st77916->madctl_val = LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported RGB element order");
        break;
    }

    switch (panel_dev_config->bits_per_pixel) {
    case 16:
        st77916->fb_bits_per_pixel = 16;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported pixel width");
        break;
    }

    st77916->io = io;
    st77916->reset_gpio_num = panel_dev_config->reset_gpio_num;
    st77916->reset_level = panel_dev_config->flags.reset_active_high;
    st77916->base.del = panel_st77916_del;
    st77916->base.reset = panel_st77916_reset;
    st77916->base.init = panel_st77916_init;
    st77916->base.draw_bitmap = panel_st77916_draw_bitmap;
    st77916->base.invert_color = panel_st77916_invert_color;
    st77916->base.mirror = panel_st77916_mirror;
    st77916->base.swap_xy = panel_st77916_swap_xy;
    st77916->base.set_gap = panel_st77916_set_gap;
    st77916->base.disp_on_off = panel_st77916_disp_on_off;
    st77916->base.disp_sleep = panel_st77916_sleep;

    *ret_panel = &st77916->base;
    return ESP_OK;

err:
    if (st77916) {
        if (panel_dev_config && panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(st77916);
    }
    return ret;
}

esp_err_t esp_lcd_panel_st77916_set_vcom(esp_lcd_panel_handle_t panel, uint8_t vcom)
{
    st77916_panel_t *st77916 = __containerof(panel, st77916_panel_t, base);
    esp_lcd_panel_io_handle_t io = st77916->io;

    ESP_RETURN_ON_ERROR(panel_st77916_select_vendor_page(io), TAG, "select vendor page failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xDD, &vcom, 1), TAG, "set VCOMN failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xDE, &vcom, 1), TAG, "set VCOMP failed");
    ESP_RETURN_ON_ERROR(panel_st77916_leave_vendor_page(io), TAG, "leave vendor page failed");

    return ESP_OK;
}

esp_err_t esp_lcd_panel_st77916_set_power_b2(esp_lcd_panel_handle_t panel, uint8_t value)
{
    st77916_panel_t *st77916 = __containerof(panel, st77916_panel_t, base);
    esp_lcd_panel_io_handle_t io = st77916->io;

    ESP_RETURN_ON_ERROR(panel_st77916_select_vendor_page(io), TAG, "select vendor page failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xB2, &value, 1), TAG, "set power B2 failed");
    ESP_RETURN_ON_ERROR(panel_st77916_leave_vendor_page(io), TAG, "leave vendor page failed");

    return ESP_OK;
}

static esp_err_t panel_st77916_del(esp_lcd_panel_t *panel)
{
    st77916_panel_t *st77916 = __containerof(panel, st77916_panel_t, base);

    if (st77916->reset_gpio_num >= 0) {
        gpio_reset_pin(st77916->reset_gpio_num);
    }
    free(st77916);
    return ESP_OK;
}

static esp_err_t panel_st77916_reset(esp_lcd_panel_t *panel)
{
    st77916_panel_t *st77916 = __containerof(panel, st77916_panel_t, base);

    if (st77916->reset_gpio_num >= 0) {
        gpio_set_level(st77916->reset_gpio_num, st77916->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(st77916->reset_gpio_num, !st77916->reset_level);
        vTaskDelay(pdMS_TO_TICKS(120));
    } else {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(st77916->io, LCD_CMD_SWRESET, NULL, 0), TAG, "software reset failed");
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    return ESP_OK;
}

static esp_err_t panel_st77916_init(esp_lcd_panel_t *panel)
{
    st77916_panel_t *st77916 = __containerof(panel, st77916_panel_t, base);
    esp_lcd_panel_io_handle_t io = st77916->io;

    ESP_LOGI(TAG, "Initialize ST77916 with vendor command table");
    for (size_t i = 0; i < sizeof(vendor_init_cmds) / sizeof(vendor_init_cmds[0]); i++) {
        const st77916_lcd_init_cmd_t *init_cmd = &vendor_init_cmds[i];
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, init_cmd->cmd, init_cmd->data, init_cmd->data_bytes),
                            TAG, "command 0x%02X failed", init_cmd->cmd);
        if (init_cmd->delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(init_cmd->delay_ms));
        }
    }

    uint8_t colmod = 0x55;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_COLMOD, &colmod, 1),
                        TAG, "set RGB565 pixel format failed");

    return ESP_OK;
}

static esp_err_t panel_st77916_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end,
                                           const void *color_data)
{
    st77916_panel_t *st77916 = __containerof(panel, st77916_panel_t, base);
    esp_lcd_panel_io_handle_t io = st77916->io;

    x_start += st77916->x_gap;
    x_end += st77916->x_gap;
    y_start += st77916->y_gap;
    y_end += st77916->y_gap;

    const int width = x_end - x_start;
    const int height = y_end - y_start;
    const size_t bytes_per_pixel = st77916->fb_bits_per_pixel / 8;
    const size_t bytes_per_line = width * bytes_per_pixel;
    const size_t max_chunk_bytes = 32 * 1024;
    int lines_per_chunk = max_chunk_bytes / bytes_per_line;
    if (lines_per_chunk < 1) {
        lines_per_chunk = 1;
    }

    const uint8_t *color_bytes = (const uint8_t *)color_data;
    for (int y = 0; y < height; y += lines_per_chunk) {
        const int chunk_lines = (height - y) > lines_per_chunk ? lines_per_chunk : (height - y);
        const int chunk_y_start = y_start + y;
        const int chunk_y_end = chunk_y_start + chunk_lines;
        const size_t chunk_bytes = bytes_per_line * chunk_lines;

        ESP_RETURN_ON_ERROR(panel_st77916_set_window(io, x_start, chunk_y_start, x_end, chunk_y_end),
                            TAG, "set draw window failed");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(io, LCD_CMD_RAMWR, color_bytes, chunk_bytes),
                            TAG, "write color data failed");
        color_bytes += chunk_bytes;
    }

    return ESP_OK;
}

static esp_err_t panel_st77916_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    st77916_panel_t *st77916 = __containerof(panel, st77916_panel_t, base);
    int command = invert_color_data ? LCD_CMD_INVON : LCD_CMD_INVOFF;
    return esp_lcd_panel_io_tx_param(st77916->io, command, NULL, 0);
}

static esp_err_t panel_st77916_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    st77916_panel_t *st77916 = __containerof(panel, st77916_panel_t, base);

    if (mirror_x) {
        st77916->madctl_val |= LCD_CMD_MX_BIT;
    } else {
        st77916->madctl_val &= ~LCD_CMD_MX_BIT;
    }
    if (mirror_y) {
        st77916->madctl_val |= LCD_CMD_MY_BIT;
    } else {
        st77916->madctl_val &= ~LCD_CMD_MY_BIT;
    }

    return esp_lcd_panel_io_tx_param(st77916->io, LCD_CMD_MADCTL, &st77916->madctl_val, 1);
}

static esp_err_t panel_st77916_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    st77916_panel_t *st77916 = __containerof(panel, st77916_panel_t, base);

    if (swap_axes) {
        st77916->madctl_val |= LCD_CMD_MV_BIT;
    } else {
        st77916->madctl_val &= ~LCD_CMD_MV_BIT;
    }

    return esp_lcd_panel_io_tx_param(st77916->io, LCD_CMD_MADCTL, &st77916->madctl_val, 1);
}

static esp_err_t panel_st77916_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    st77916_panel_t *st77916 = __containerof(panel, st77916_panel_t, base);

    st77916->x_gap = x_gap;
    st77916->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_st77916_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    st77916_panel_t *st77916 = __containerof(panel, st77916_panel_t, base);
    int command = on_off ? LCD_CMD_DISPON : LCD_CMD_DISPOFF;
    return esp_lcd_panel_io_tx_param(st77916->io, command, NULL, 0);
}

static esp_err_t panel_st77916_sleep(esp_lcd_panel_t *panel, bool sleep)
{
    st77916_panel_t *st77916 = __containerof(panel, st77916_panel_t, base);
    int command = sleep ? LCD_CMD_SLPIN : LCD_CMD_SLPOUT;
    esp_err_t ret = esp_lcd_panel_io_tx_param(st77916->io, command, NULL, 0);
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    return ret;
}
