/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Minimal ILI9342C panel driver for the M5Stack CoreS3 display.
 * The initialization sequence follows M5GFX's Panel_ILI9342 configuration.
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
#include "ili9342_panel.h"

static const char *TAG = "lcd_panel.ili9342";

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    gpio_num_t reset_gpio_num;
    bool reset_level;
    int x_gap;
    int y_gap;
    uint8_t madctl_val;
    uint8_t colmod_val;
} ili9342_panel_t;

static esp_err_t panel_del(esp_lcd_panel_t *panel);
static esp_err_t panel_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_init(esp_lcd_panel_t *panel);
static esp_err_t panel_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start,
                                   int x_end, int y_end, const void *color_data);
static esp_err_t panel_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_disp_on_off(esp_lcd_panel_t *panel, bool on_off);
static esp_err_t panel_sleep(esp_lcd_panel_t *panel, bool sleep);

esp_err_t esp_lcd_new_panel_ili9342(const esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = ESP_OK;
    ili9342_panel_t *ili = NULL;
    ESP_GOTO_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");

    ili = calloc(1, sizeof(*ili));
    ESP_GOTO_ON_FALSE(ili, ESP_ERR_NO_MEM, err, TAG, "no memory for ILI9342 panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        const gpio_config_t reset_config = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&reset_config), err, TAG, "configure reset GPIO failed");
    }

    ili->madctl_val = panel_dev_config->rgb_ele_order == LCD_RGB_ELEMENT_ORDER_BGR ? LCD_CMD_BGR_BIT : 0;
    ESP_GOTO_ON_FALSE(panel_dev_config->rgb_ele_order == LCD_RGB_ELEMENT_ORDER_RGB ||
                     panel_dev_config->rgb_ele_order == LCD_RGB_ELEMENT_ORDER_BGR,
                     ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported RGB element order");
    ESP_GOTO_ON_FALSE(panel_dev_config->bits_per_pixel == 16,
                     ESP_ERR_NOT_SUPPORTED, err, TAG, "ILI9342 requires RGB565");

    ili->io = io;
    ili->reset_gpio_num = panel_dev_config->reset_gpio_num;
    ili->reset_level = panel_dev_config->flags.reset_active_high;
    ili->colmod_val = 0x55;
    ili->base.del = panel_del;
    ili->base.reset = panel_reset;
    ili->base.init = panel_init;
    ili->base.draw_bitmap = panel_draw_bitmap;
    ili->base.invert_color = panel_invert_color;
    ili->base.mirror = panel_mirror;
    ili->base.swap_xy = panel_swap_xy;
    ili->base.set_gap = panel_set_gap;
    ili->base.disp_on_off = panel_disp_on_off;
    ili->base.disp_sleep = panel_sleep;
    *ret_panel = &ili->base;
    return ESP_OK;

err:
    if (ili) {
        if (panel_dev_config && panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(ili);
    }
    return ret;
}

static esp_err_t panel_del(esp_lcd_panel_t *panel)
{
    ili9342_panel_t *ili = __containerof(panel, ili9342_panel_t, base);
    if (ili->reset_gpio_num >= 0) gpio_reset_pin(ili->reset_gpio_num);
    free(ili);
    return ESP_OK;
}

static esp_err_t panel_reset(esp_lcd_panel_t *panel)
{
    ili9342_panel_t *ili = __containerof(panel, ili9342_panel_t, base);
    if (ili->reset_gpio_num >= 0) {
        gpio_set_level(ili->reset_gpio_num, ili->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(ili->reset_gpio_num, !ili->reset_level);
    } else {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(ili->io, LCD_CMD_SWRESET, NULL, 0), TAG, "software reset failed");
    }
    vTaskDelay(pdMS_TO_TICKS(120));
    return ESP_OK;
}

static esp_err_t panel_init(esp_lcd_panel_t *panel)
{
    ili9342_panel_t *ili = __containerof(panel, ili9342_panel_t, base);
    esp_lcd_panel_io_handle_t io = ili->io;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xC8, (uint8_t[]){0xFF, 0x93, 0x42}, 3), TAG, "set external command failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xC0, (uint8_t[]){0x12, 0x12}, 2), TAG, "set power control 1 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xC1, (uint8_t[]){0x03}, 1), TAG, "set power control 2 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xC5, (uint8_t[]){0xF2}, 1), TAG, "set VCOM failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xB0, (uint8_t[]){0xE0}, 1), TAG, "set display function control failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xF6, (uint8_t[]){0x01, 0x00, 0x00}, 3), TAG, "set interface control failed");
    static const uint8_t gamma_pos[] = {0x00,0x0C,0x11,0x04,0x11,0x08,0x37,0x89,0x4C,0x06,0x0C,0x0A,0x2E,0x34,0x0F};
    static const uint8_t gamma_neg[] = {0x00,0x0B,0x11,0x05,0x13,0x09,0x33,0x67,0x48,0x07,0x0E,0x0B,0x2E,0x33,0x0F};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xE0, gamma_pos, sizeof(gamma_pos)), TAG, "set positive gamma failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xE1, gamma_neg, sizeof(gamma_neg)), TAG, "set negative gamma failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xB6, (uint8_t[]){0x08,0x82,0x1D,0x04}, 4), TAG, "set display function failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_COLMOD, &ili->colmod_val, 1), TAG, "set pixel format failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, &ili->madctl_val, 1), TAG, "set memory access control failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SLPOUT, NULL, 0), TAG, "exit sleep failed");
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_NORON, NULL, 0), TAG, "normal display mode failed");
    return ESP_OK;
}

static esp_err_t panel_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start,
                                   int x_end, int y_end, const void *color_data)
{
    ili9342_panel_t *ili = __containerof(panel, ili9342_panel_t, base);
    x_start += ili->x_gap; x_end += ili->x_gap;
    y_start += ili->y_gap; y_end += ili->y_gap;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(ili->io, LCD_CMD_CASET,
        (uint8_t[]){x_start >> 8, x_start, (x_end - 1) >> 8, x_end - 1}, 4), TAG, "set column address failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(ili->io, LCD_CMD_RASET,
        (uint8_t[]){y_start >> 8, y_start, (y_end - 1) >> 8, y_end - 1}, 4), TAG, "set row address failed");
    return esp_lcd_panel_io_tx_color(ili->io, LCD_CMD_RAMWR, color_data,
                                     (size_t)(x_end - x_start) * (size_t)(y_end - y_start) * 2);
}

static esp_err_t panel_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    ili9342_panel_t *ili = __containerof(panel, ili9342_panel_t, base);
    return esp_lcd_panel_io_tx_param(ili->io, invert_color_data ? LCD_CMD_INVON : LCD_CMD_INVOFF, NULL, 0);
}

static esp_err_t panel_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    ili9342_panel_t *ili = __containerof(panel, ili9342_panel_t, base);
    if (mirror_x) ili->madctl_val |= LCD_CMD_MX_BIT; else ili->madctl_val &= ~LCD_CMD_MX_BIT;
    if (mirror_y) ili->madctl_val |= LCD_CMD_MY_BIT; else ili->madctl_val &= ~LCD_CMD_MY_BIT;
    return esp_lcd_panel_io_tx_param(ili->io, LCD_CMD_MADCTL, &ili->madctl_val, 1);
}

static esp_err_t panel_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    ili9342_panel_t *ili = __containerof(panel, ili9342_panel_t, base);
    if (swap_axes) ili->madctl_val |= LCD_CMD_MV_BIT; else ili->madctl_val &= ~LCD_CMD_MV_BIT;
    return esp_lcd_panel_io_tx_param(ili->io, LCD_CMD_MADCTL, &ili->madctl_val, 1);
}

static esp_err_t panel_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    ili9342_panel_t *ili = __containerof(panel, ili9342_panel_t, base);
    ili->x_gap = x_gap; ili->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    ili9342_panel_t *ili = __containerof(panel, ili9342_panel_t, base);
    return esp_lcd_panel_io_tx_param(ili->io, on_off ? LCD_CMD_DISPON : LCD_CMD_DISPOFF, NULL, 0);
}

static esp_err_t panel_sleep(esp_lcd_panel_t *panel, bool sleep)
{
    ili9342_panel_t *ili = __containerof(panel, ili9342_panel_t, base);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(ili->io, sleep ? LCD_CMD_SLPIN : LCD_CMD_SLPOUT, NULL, 0), TAG, "set sleep mode failed");
    vTaskDelay(pdMS_TO_TICKS(120));
    return ESP_OK;
}
