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
#define AW9523B_REG_OUTPUT0 0x02
#define AW9523B_REG_OUTPUT1 0x03
#define AW9523B_REG_CONFIG0 0x04
#define AW9523B_REG_CONFIG1 0x05
#define AW9523B_REG_GCR 0x11
#define AW9523B_REG_LEDMODE0 0x12
#define AW9523B_REG_LEDMODE1 0x13
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

static esp_err_t read_reg(i2c_master_dev_handle_t device, uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(device, &reg, 1, value, 1, pdMS_TO_TICKS(100));
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

    // Follow the CoreS3 factory stack's two-stage power-up order. M5GFX first
    // enables only the rails needed for board discovery and configures the IO
    // expander without starting the external boost converter.
    ESP_RETURN_ON_ERROR(write_reg(s_axp, 0x90, 0xBF), TAG, "enable CoreS3 LDO rails failed");
    ESP_RETURN_ON_ERROR(write_reg(s_axp, 0x95, 28), TAG, "set CoreS3 ALDO4 voltage failed");
    ESP_RETURN_ON_ERROR(update_reg(s_aw, AW9523B_REG_OUTPUT0, 0x05, 0x05), TAG,
                        "enable CoreS3 port 0 outputs failed");
    ESP_RETURN_ON_ERROR(update_reg(s_aw, AW9523B_REG_OUTPUT1, 0x03, 0x03), TAG,
                        "enable CoreS3 port 1 outputs failed");
    ESP_RETURN_ON_ERROR(write_reg(s_aw, AW9523B_REG_CONFIG0, 0x18), TAG,
                        "configure CoreS3 port 0 failed");
    ESP_RETURN_ON_ERROR(write_reg(s_aw, AW9523B_REG_CONFIG1, 0x0C), TAG,
                        "configure CoreS3 port 1 failed");
    ESP_RETURN_ON_ERROR(write_reg(s_aw, AW9523B_REG_GCR, 0x10), TAG,
                        "configure CoreS3 push-pull outputs failed");
    ESP_RETURN_ON_ERROR(write_reg(s_aw, AW9523B_REG_LEDMODE0, 0xFF), TAG,
                        "configure CoreS3 port 0 GPIO mode failed");
    ESP_RETURN_ON_ERROR(write_reg(s_aw, AW9523B_REG_LEDMODE1, 0xFF), TAG,
                        "configure CoreS3 port 1 GPIO mode failed");

    // M5Unified 0.1.6 then starts BOOST_EN and applies the factory CoreS3 PMIC
    // values. Keep this sequence exact; unrelated AXP2101 defaults can affect
    // the handover from VBUS to the battery-backed system rail.
    ESP_RETURN_ON_ERROR(update_reg(s_aw, AW9523B_REG_OUTPUT1, 0x80, 0x80), TAG,
                        "enable CoreS3 boost converter failed");
    ESP_RETURN_ON_ERROR(write_reg(s_axp, 0x90, 0xBF), TAG, "enable CoreS3 LDO rails failed");
    ESP_RETURN_ON_ERROR(write_reg(s_axp, 0x92, 13), TAG, "set CoreS3 ALDO1 voltage failed");
    ESP_RETURN_ON_ERROR(write_reg(s_axp, 0x93, 28), TAG, "set CoreS3 ALDO2 voltage failed");
    ESP_RETURN_ON_ERROR(write_reg(s_axp, 0x94, 28), TAG, "set CoreS3 ALDO3 voltage failed");
    ESP_RETURN_ON_ERROR(write_reg(s_axp, 0x95, 28), TAG, "set CoreS3 ALDO4 voltage failed");
    // Match the official CoreS3 AXP2101 startup policy. In particular, keep
    // the PWRON timing and PMIC common configuration at the factory values;
    // these are applied before the ESP32 can rely on battery-only startup.
    ESP_RETURN_ON_ERROR(write_reg(s_axp, 0x27, 0x00), TAG, "set CoreS3 power-key timing failed");
    ESP_RETURN_ON_ERROR(write_reg(s_axp, 0x10, 0x30), TAG, "set CoreS3 PMIC common config failed");
    ESP_RETURN_ON_ERROR(write_reg(s_axp, 0x30, 0x0F), TAG, "enable CoreS3 power ADC failed");
    ESP_RETURN_ON_ERROR(write_reg(s_axp, 0x69, 0x11), TAG, "configure CoreS3 charge LED failed");
    ESP_RETURN_ON_ERROR(update_reg(s_aw, AW9523B_REG_OUTPUT0, 0x02, 0x02), TAG,
                        "enable CoreS3 external power output failed");
    ESP_RETURN_ON_ERROR(write_reg(s_axp, 0x99, 28), TAG, "set CoreS3 backlight voltage failed");

    uint8_t status = 0;
    uint8_t battery_voltage_high = 0;
    uint8_t battery_voltage_low = 0;
    if (read_reg(s_axp, 0x00, &status) == ESP_OK &&
        read_reg(s_axp, 0x34, &battery_voltage_high) == ESP_OK &&
        read_reg(s_axp, 0x35, &battery_voltage_low) == ESP_OK) {
        const uint16_t battery_mv = (uint16_t)(((battery_voltage_high & 0x3F) << 8) |
                                               battery_voltage_low);
        ESP_LOGI(TAG, "CoreS3 power: VBUS=%s battery=%s voltage=%u mV (status=0x%02x)",
                 (status & 0x20) ? "present" : "absent",
                 (status & 0x08) ? "present" : "absent",
                 battery_mv,
                 status);
    } else {
        ESP_LOGW(TAG, "could not read CoreS3 power status");
    }
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

esp_err_t openpencil_m5cores3_read_power_register(uint8_t reg, uint8_t *value)
{
    ESP_RETURN_ON_FALSE(s_axp && value, ESP_ERR_INVALID_STATE, TAG,
                        "AXP2101 is not initialized");
    return read_reg(s_axp, reg, value);
}

i2c_master_bus_handle_t openpencil_m5cores3_i2c_bus(void)
{
    return s_bus;
}
