/* SPDX-License-Identifier: CC0-1.0 */

#include "m5cores3.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CORES3_I2C_SDA 12
#define CORES3_I2C_SCL 11
#define CORES3_I2C_FREQ 400000
#define AXP2101_ADDR 0x34
#define AW9523B_ADDR 0x58
#define AW9523B_REG_OUTPUT1 0x03
#define AW9523B_REG_CONFIG1 0x05
#define LCD_RESET_MASK (1u << 1) /* AW9523B P1_1 */

static const char *TAG = "m5cores3";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_axp;
static i2c_master_dev_handle_t s_aw;

static esp_err_t write_reg(i2c_master_dev_handle_t device, uint8_t reg, uint8_t value)
{
    const uint8_t packet[2] = {reg, value};
    return i2c_master_transmit(device, packet, sizeof(packet), pdMS_TO_TICKS(100));
}

static esp_err_t update_reg(i2c_master_dev_handle_t device, uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t current = 0;
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(device, &reg, 1, &current, 1, pdMS_TO_TICKS(100)), TAG, "read I2C register failed");
    current = (uint8_t)((current & (uint8_t)~mask) | (value & mask));
    return write_reg(device, reg, current);
}

static esp_err_t add_device(uint8_t address, i2c_master_dev_handle_t *device)
{
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = CORES3_I2C_FREQ,
    };
    return i2c_master_bus_add_device(s_bus, &config, device);
}

esp_err_t openpencil_m5cores3_display_init(void)
{
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = CORES3_I2C_SDA,
        .scl_io_num = CORES3_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &s_bus), TAG, "create CoreS3 I2C bus failed");
    ESP_RETURN_ON_ERROR(add_device(AXP2101_ADDR, &s_axp), TAG, "add AXP2101 failed");
    ESP_RETURN_ON_ERROR(add_device(AW9523B_ADDR, &s_aw), TAG, "add AW9523B failed");

    // AW9523B P1_1 is the LCD reset output. A zero in CONFIG means output.
    ESP_RETURN_ON_ERROR(update_reg(s_aw, AW9523B_REG_CONFIG1, LCD_RESET_MASK, 0), TAG, "configure LCD reset pin failed");
    ESP_RETURN_ON_ERROR(write_reg(s_aw, AW9523B_REG_OUTPUT1, LCD_RESET_MASK), TAG, "release LCD reset pin failed");

    // CoreS3 drives the display/backlight rail from AXP2101 DLDO1.
    ESP_RETURN_ON_ERROR(update_reg(s_axp, 0x90, 0x80, 0x80), TAG, "enable LCD DLDO1 failed");
    ESP_RETURN_ON_ERROR(write_reg(s_axp, 0x99, 28), TAG, "set LCD DLDO1 voltage failed");
    ESP_LOGI(TAG, "CoreS3 I2C ready (SDA=%d SCL=%d, AXP2101=0x%02x, AW9523B=0x%02x)",
             CORES3_I2C_SDA, CORES3_I2C_SCL, AXP2101_ADDR, AW9523B_ADDR);
    return openpencil_m5cores3_lcd_reset();
}

esp_err_t openpencil_m5cores3_lcd_reset(void)
{
    ESP_RETURN_ON_FALSE(s_aw, ESP_ERR_INVALID_STATE, TAG, "AW9523B is not initialized");
    ESP_RETURN_ON_ERROR(update_reg(s_aw, AW9523B_REG_OUTPUT1, LCD_RESET_MASK, 0), TAG, "assert LCD reset failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(update_reg(s_aw, AW9523B_REG_OUTPUT1, LCD_RESET_MASK, LCD_RESET_MASK), TAG, "release LCD reset failed");
    vTaskDelay(pdMS_TO_TICKS(120));
    return ESP_OK;
}

esp_err_t openpencil_m5cores3_touch_reset(void)
{
    // FT6336U reset is not required for normal CoreS3 boot. Keep this API for
    // the input layer so a future board revision can add an expander reset.
    return s_bus ? ESP_OK : ESP_ERR_INVALID_STATE;
}

i2c_master_bus_handle_t openpencil_m5cores3_i2c_bus(void)
{
    return s_bus;
}
