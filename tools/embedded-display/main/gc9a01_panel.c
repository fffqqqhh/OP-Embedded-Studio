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
#include "gc9a01_panel.h"

static const char *TAG = "lcd_panel.gc9a01";

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    gpio_num_t reset_gpio_num;
    bool reset_level;
    int x_gap;
    int y_gap;
    uint8_t madctl_val;
    uint8_t colmod_val;
    uint8_t fb_bits_per_pixel;
} gc9a01_panel_t;

static esp_err_t panel_gc9a01_del(esp_lcd_panel_t *panel);
static esp_err_t panel_gc9a01_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_gc9a01_init(esp_lcd_panel_t *panel);
static esp_err_t panel_gc9a01_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end,
                                          const void *color_data);
static esp_err_t panel_gc9a01_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_gc9a01_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_gc9a01_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_gc9a01_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_gc9a01_disp_on_off(esp_lcd_panel_t *panel, bool on_off);
static esp_err_t panel_gc9a01_sleep(esp_lcd_panel_t *panel, bool sleep);

#define GC9A01_SEND_CMD(io, cmd) \
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param((io), (cmd), NULL, 0), TAG, "command 0x%02X failed", (cmd))

#define GC9A01_SEND_PARAM(io, cmd, ...) do { \
        const uint8_t data[] = { __VA_ARGS__ }; \
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param((io), (cmd), data, sizeof(data)), TAG, "command 0x%02X failed", (cmd)); \
    } while (0)

esp_err_t esp_lcd_new_panel_gc9a01(const esp_lcd_panel_io_handle_t io,
                                   const esp_lcd_panel_dev_config_t *panel_dev_config,
                                   esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = ESP_OK;
    gc9a01_panel_t *gc9a01 = NULL;
    ESP_GOTO_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");

    gc9a01 = calloc(1, sizeof(gc9a01_panel_t));
    ESP_GOTO_ON_FALSE(gc9a01, ESP_ERR_NO_MEM, err, TAG, "no mem for gc9a01 panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure reset GPIO failed");
    }

    switch (panel_dev_config->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        gc9a01->madctl_val = 0;
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        gc9a01->madctl_val = LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported RGB element order");
        break;
    }

    switch (panel_dev_config->bits_per_pixel) {
    case 16:
        gc9a01->colmod_val = 0x05;
        gc9a01->fb_bits_per_pixel = 16;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported pixel width");
        break;
    }

    gc9a01->io = io;
    gc9a01->reset_gpio_num = panel_dev_config->reset_gpio_num;
    gc9a01->reset_level = panel_dev_config->flags.reset_active_high;
    gc9a01->base.del = panel_gc9a01_del;
    gc9a01->base.reset = panel_gc9a01_reset;
    gc9a01->base.init = panel_gc9a01_init;
    gc9a01->base.draw_bitmap = panel_gc9a01_draw_bitmap;
    gc9a01->base.invert_color = panel_gc9a01_invert_color;
    gc9a01->base.mirror = panel_gc9a01_mirror;
    gc9a01->base.swap_xy = panel_gc9a01_swap_xy;
    gc9a01->base.set_gap = panel_gc9a01_set_gap;
    gc9a01->base.disp_on_off = panel_gc9a01_disp_on_off;
    gc9a01->base.disp_sleep = panel_gc9a01_sleep;

    *ret_panel = &gc9a01->base;
    return ESP_OK;

err:
    if (gc9a01) {
        if (panel_dev_config && panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(gc9a01);
    }
    return ret;
}

static esp_err_t panel_gc9a01_del(esp_lcd_panel_t *panel)
{
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);

    if (gc9a01->reset_gpio_num >= 0) {
        gpio_reset_pin(gc9a01->reset_gpio_num);
    }
    free(gc9a01);
    return ESP_OK;
}

static esp_err_t panel_gc9a01_reset(esp_lcd_panel_t *panel)
{
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);

    if (gc9a01->reset_gpio_num >= 0) {
        gpio_set_level(gc9a01->reset_gpio_num, gc9a01->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(gc9a01->reset_gpio_num, !gc9a01->reset_level);
        vTaskDelay(pdMS_TO_TICKS(120));
    } else {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(gc9a01->io, LCD_CMD_SWRESET, NULL, 0), TAG, "software reset failed");
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    return ESP_OK;
}

static esp_err_t panel_gc9a01_init(esp_lcd_panel_t *panel)
{
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9a01->io;

    GC9A01_SEND_CMD(io, 0xFE);
    GC9A01_SEND_CMD(io, 0xEF);
    GC9A01_SEND_PARAM(io, 0xEB, 0x14);
    GC9A01_SEND_PARAM(io, 0x84, 0x40);
    GC9A01_SEND_PARAM(io, 0x86, 0xFF);
    GC9A01_SEND_PARAM(io, 0xC0, 0x1A);
    GC9A01_SEND_PARAM(io, 0x88, 0x0A);
    GC9A01_SEND_PARAM(io, 0x89, 0x21);
    GC9A01_SEND_PARAM(io, 0x8A, 0x00);
    GC9A01_SEND_PARAM(io, 0x8B, 0x80);
    GC9A01_SEND_PARAM(io, 0x8C, 0x01);
    GC9A01_SEND_PARAM(io, 0x8D, 0x01);
    GC9A01_SEND_PARAM(io, 0x8F, 0xFF);
    GC9A01_SEND_PARAM(io, 0xB6, 0x20);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, &gc9a01->madctl_val, 1), TAG, "set MADCTL failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_COLMOD, &gc9a01->colmod_val, 1), TAG, "set color mode failed");
    GC9A01_SEND_PARAM(io, 0x90, 0x08, 0x08, 0x08, 0x08);
    GC9A01_SEND_PARAM(io, 0xBD, 0x06);
    GC9A01_SEND_PARAM(io, 0xBC, 0x00);
    GC9A01_SEND_PARAM(io, 0xFF, 0x60, 0x01, 0x04);
    GC9A01_SEND_PARAM(io, 0xC9, 0x22);
    GC9A01_SEND_PARAM(io, 0xBE, 0x11);
    GC9A01_SEND_PARAM(io, 0xE1, 0x10, 0x0E);
    GC9A01_SEND_PARAM(io, 0xDF, 0x21, 0x0C, 0x02);
    GC9A01_SEND_PARAM(io, 0xF0, 0x45, 0x09, 0x08, 0x08, 0x26, 0x2A);
    GC9A01_SEND_PARAM(io, 0xF1, 0x43, 0x70, 0x72, 0x36, 0x37, 0x6F);
    GC9A01_SEND_PARAM(io, 0xF2, 0x45, 0x09, 0x08, 0x08, 0x26, 0x2A);
    GC9A01_SEND_PARAM(io, 0xF3, 0x43, 0x70, 0x72, 0x36, 0x37, 0x6F);
    GC9A01_SEND_PARAM(io, 0xED, 0x1B, 0x8B);
    GC9A01_SEND_PARAM(io, 0xAE, 0x77);
    GC9A01_SEND_PARAM(io, 0xCD, 0x63);
    GC9A01_SEND_PARAM(io, 0xAC, 0x27);
    GC9A01_SEND_PARAM(io, 0x70, 0x07, 0x07, 0x04, 0x06, 0x0F, 0x09, 0x07, 0x08, 0x03);
    GC9A01_SEND_PARAM(io, 0xE8, 0x24);
    GC9A01_SEND_PARAM(io, 0x62, 0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70);
    GC9A01_SEND_PARAM(io, 0x63, 0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70);
    GC9A01_SEND_PARAM(io, 0x64, 0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07);
    GC9A01_SEND_PARAM(io, 0x66, 0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00);
    GC9A01_SEND_PARAM(io, 0x67, 0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98);
    GC9A01_SEND_PARAM(io, 0x74, 0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00);
    GC9A01_SEND_PARAM(io, 0x98, 0x3E, 0x07);
    GC9A01_SEND_CMD(io, 0x35);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_INVON, NULL, 0), TAG, "enable inversion failed");
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SLPOUT, NULL, 0), TAG, "exit sleep failed");
    vTaskDelay(pdMS_TO_TICKS(320));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_DISPON, NULL, 0), TAG, "display on failed");
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_RAMWR, NULL, 0), TAG, "start memory write failed");

    return ESP_OK;
}

static esp_err_t panel_gc9a01_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end,
                                          const void *color_data)
{
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9a01->io;

    x_start += gc9a01->x_gap;
    x_end += gc9a01->x_gap;
    y_start += gc9a01->y_gap;
    y_end += gc9a01->y_gap;

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_CASET, (uint8_t[]) {
        (x_start >> 8) & 0xFF, x_start & 0xFF,
        ((x_end - 1) >> 8) & 0xFF, (x_end - 1) & 0xFF,
    }, 4), TAG, "set column address failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_RASET, (uint8_t[]) {
        (y_start >> 8) & 0xFF, y_start & 0xFF,
        ((y_end - 1) >> 8) & 0xFF, (y_end - 1) & 0xFF,
    }, 4), TAG, "set row address failed");

    size_t len = (x_end - x_start) * (y_end - y_start) * gc9a01->fb_bits_per_pixel / 8;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(io, LCD_CMD_RAMWR, color_data, len), TAG, "write color data failed");

    return ESP_OK;
}

static esp_err_t panel_gc9a01_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);
    int command = invert_color_data ? LCD_CMD_INVON : LCD_CMD_INVOFF;
    return esp_lcd_panel_io_tx_param(gc9a01->io, command, NULL, 0);
}

static esp_err_t panel_gc9a01_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);

    if (mirror_x) {
        gc9a01->madctl_val |= LCD_CMD_MX_BIT;
    } else {
        gc9a01->madctl_val &= ~LCD_CMD_MX_BIT;
    }
    if (mirror_y) {
        gc9a01->madctl_val |= LCD_CMD_MY_BIT;
    } else {
        gc9a01->madctl_val &= ~LCD_CMD_MY_BIT;
    }

    return esp_lcd_panel_io_tx_param(gc9a01->io, LCD_CMD_MADCTL, &gc9a01->madctl_val, 1);
}

static esp_err_t panel_gc9a01_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);

    if (swap_axes) {
        gc9a01->madctl_val |= LCD_CMD_MV_BIT;
    } else {
        gc9a01->madctl_val &= ~LCD_CMD_MV_BIT;
    }

    return esp_lcd_panel_io_tx_param(gc9a01->io, LCD_CMD_MADCTL, &gc9a01->madctl_val, 1);
}

static esp_err_t panel_gc9a01_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);

    gc9a01->x_gap = x_gap;
    gc9a01->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_gc9a01_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);
    int command = on_off ? LCD_CMD_DISPON : LCD_CMD_DISPOFF;
    return esp_lcd_panel_io_tx_param(gc9a01->io, command, NULL, 0);
}

static esp_err_t panel_gc9a01_sleep(esp_lcd_panel_t *panel, bool sleep)
{
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);
    int command = sleep ? LCD_CMD_SLPIN : LCD_CMD_SLPOUT;
    esp_err_t ret = esp_lcd_panel_io_tx_param(gc9a01->io, command, NULL, 0);
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    return ret;
}
