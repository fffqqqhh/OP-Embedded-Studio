#include "m5ioe1.h"

#include <stdbool.h>
#include <stddef.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "m5pm1.h"

#define M5IOE1_I2C_ADDRESS_PRIMARY 0x4F
#define M5IOE1_I2C_ADDRESS_SECONDARY 0x6F
#define M5IOE1_I2C_SCL_GPIO 48
#define M5IOE1_I2C_SDA_GPIO 47
#define M5IOE1_I2C_FREQUENCY_HZ 100000

#define M5IOE1_REG_UID_L 0x00
#define M5IOE1_REG_GPIO_MODE_L 0x03
#define M5IOE1_REG_GPIO_OUT_L 0x05
#define M5IOE1_REG_GPIO_PU_L 0x09
#define M5IOE1_REG_GPIO_PD_L 0x0B
#define M5IOE1_REG_GPIO_DRV_L 0x13
#define M5IOE1_REG_I2C_CFG 0x23

// M5Stack StopWatch routes AMOLED enable to IO8 (L3B_EN) and panel reset to IO5.
#define M5IOE1_DISPLAY_POWER_PIN 7
#define M5IOE1_DISPLAY_RESET_PIN 4
#define M5IOE1_TRANSACTION_TIMEOUT_MS 100

static const char *TAG = "m5ioe1";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_device;
static uint8_t s_address;

static esp_err_t read_register(uint8_t reg, uint8_t *data, size_t length)
{
    return i2c_master_transmit_receive(s_device, &reg, 1, data, length,
                                        pdMS_TO_TICKS(M5IOE1_TRANSACTION_TIMEOUT_MS));
}

static esp_err_t write_register(uint8_t reg, const uint8_t *data, size_t length)
{
    uint8_t buffer[3];
    ESP_RETURN_ON_FALSE(length <= 2, ESP_ERR_INVALID_SIZE, TAG, "M5IOE1 write too large");
    buffer[0] = reg;
    for (size_t index = 0; index < length; index++) buffer[index + 1] = data[index];
    const esp_err_t result = i2c_master_transmit(s_device, buffer, length + 1,
                                                  pdMS_TO_TICKS(M5IOE1_TRANSACTION_TIMEOUT_MS));
    // M5IOE1 needs a short processing interval between register writes.
    esp_rom_delay_us(500);
    return result;
}

static esp_err_t write_register16(uint8_t reg, uint16_t value)
{
    const uint8_t data[2] = {
        (uint8_t)(value & 0xFF),
        (uint8_t)((value >> 8) & 0xFF),
    };
    return write_register(reg, data, sizeof(data));
}

static esp_err_t read_register16(uint8_t reg, uint16_t *value)
{
    uint8_t data[2] = {0};
    const esp_err_t result = read_register(reg, data, sizeof(data));
    esp_rom_delay_us(500);
    if (result == ESP_OK) {
        *value = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    }
    return result;
}

static esp_err_t configure_display_outputs(void)
{
    // GPIO8 (bit 7) drives the AMOLED rail; GPIO5 (bit 4) drives panel reset.
    const uint16_t display_gpio_mask = (uint16_t)((1u << M5IOE1_DISPLAY_POWER_PIN) |
                                                  (1u << M5IOE1_DISPLAY_RESET_PIN));
    const uint16_t power_only = (uint16_t)(1u << M5IOE1_DISPLAY_POWER_PIN);

    // M5IOE1 GPIO registers are 16-bit little-endian registers. Configure
    // the complete register, as the stable USB baseline does.
    ESP_RETURN_ON_ERROR(write_register16(M5IOE1_REG_GPIO_PU_L, 0), TAG, "clear GPIO pull-up failed");
    ESP_RETURN_ON_ERROR(write_register16(M5IOE1_REG_GPIO_PD_L, 0), TAG, "clear GPIO pull-down failed");
    // GPIO_DRV uses 0 for push-pull and 1 for open-drain.
    ESP_RETURN_ON_ERROR(write_register16(M5IOE1_REG_GPIO_DRV_L, 0), TAG, "configure display drive failed");
    ESP_RETURN_ON_ERROR(write_register16(M5IOE1_REG_GPIO_MODE_L, display_gpio_mask), TAG, "configure display output mode failed");

    uint16_t mode = 0;
    uint16_t drive = 0;
    if (read_register16(M5IOE1_REG_GPIO_MODE_L, &mode) != ESP_OK ||
        read_register16(M5IOE1_REG_GPIO_DRV_L, &drive) != ESP_OK) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "M5IOE1 addr=0x%02x GPIO_MODE=0x%04x GPIO_DRV=0x%04x", s_address, mode, drive);
    if ((mode & display_gpio_mask) != display_gpio_mask || (drive & display_gpio_mask) != 0) {
        ESP_LOGE(TAG, "M5IOE1 display GPIO configuration readback mismatch");
        return ESP_FAIL;
    }

#if CONFIG_OPENPENCIL_BLE_SERVER
    // Web flashing resets the ESP32-S3 but leaves the external M5IOE1 and
    // AMOLED rail running. Give BLE firmware a deterministic cold panel start.
    ESP_RETURN_ON_ERROR(write_register16(M5IOE1_REG_GPIO_OUT_L, 0),
                        TAG,
                        "turn off display before BLE cold start failed");
    vTaskDelay(pdMS_TO_TICKS(1000));
#endif
    ESP_RETURN_ON_ERROR(write_register16(M5IOE1_REG_GPIO_OUT_L, power_only), TAG, "enable display power and assert reset failed");
    vTaskDelay(pdMS_TO_TICKS(80));
    ESP_RETURN_ON_ERROR(write_register16(M5IOE1_REG_GPIO_OUT_L, display_gpio_mask), TAG, "release display reset failed");
    vTaskDelay(pdMS_TO_TICKS(50));
    uint16_t output = 0;
    ESP_RETURN_ON_ERROR(read_register16(M5IOE1_REG_GPIO_OUT_L, &output), TAG, "read display output failed");
    ESP_LOGI(TAG, "M5IOE1 GPIO_OUT=0x%04x (power=%d reset=%d)", output,
             (output & (1u << M5IOE1_DISPLAY_POWER_PIN)) != 0,
             (output & (1u << M5IOE1_DISPLAY_RESET_PIN)) != 0);
    if ((output & display_gpio_mask) != display_gpio_mask) {
        ESP_LOGE(TAG, "M5IOE1 display output readback mismatch");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t openpencil_m5ioe1_display_init(void)
{
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = M5IOE1_I2C_SDA_GPIO,
        .scl_io_num = M5IOE1_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &s_bus), TAG, "create M5IOE1 I2C bus failed");

    // The PM1 controls the power-key shutdown/wake cycle and the 3.3 V rail.
    // Initialize it on the same bus before touching the IO expander.
    const esp_err_t pm1_result = openpencil_m5pm1_init(s_bus);
    if (pm1_result != ESP_OK) {
        ESP_LOGW(TAG, "M5PM1 initialization failed: %s; continuing with display-only mode",
                 esp_err_to_name(pm1_result));
    }

    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = M5IOE1_I2C_ADDRESS_PRIMARY,
        .scl_speed_hz = M5IOE1_I2C_FREQUENCY_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &device_config, &s_device), TAG, "add M5IOE1 failed");

    uint8_t uid[2];
    esp_err_t response = read_register(M5IOE1_REG_UID_L, uid, sizeof(uid));
    if (response != ESP_OK) {
        ESP_LOGW(TAG, "M5IOE1 primary address 0x%02x unavailable; trying 0x%02x",
                 M5IOE1_I2C_ADDRESS_PRIMARY, M5IOE1_I2C_ADDRESS_SECONDARY);
        ESP_RETURN_ON_ERROR(i2c_master_bus_rm_device(s_device), TAG, "remove primary M5IOE1 failed");
        s_device = NULL;
        device_config.device_address = M5IOE1_I2C_ADDRESS_SECONDARY;
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &device_config, &s_device), TAG, "add secondary M5IOE1 failed");
        response = read_register(M5IOE1_REG_UID_L, uid, sizeof(uid));
        ESP_RETURN_ON_ERROR(response, TAG, "M5IOE1 is not responding");
        s_address = M5IOE1_I2C_ADDRESS_SECONDARY;
    } else {
        s_address = M5IOE1_I2C_ADDRESS_PRIMARY;
    }
    ESP_LOGI(TAG, "M5IOE1 UID %02x%02x", uid[1], uid[0]);
    const uint8_t i2c_config = 0;
    ESP_RETURN_ON_ERROR(write_register(M5IOE1_REG_I2C_CFG, &i2c_config, 1), TAG, "configure M5IOE1 I2C failed");

    return configure_display_outputs();
}

esp_err_t openpencil_m5ioe1_display_power_down(void)
{
    ESP_RETURN_ON_FALSE(s_device, ESP_ERR_INVALID_STATE, TAG, "M5IOE1 is not initialized");

    // esp_restart() resets the ESP32-S3 but leaves the M5IOE1 and AMOLED rail
    // powered. Force the panel into the same known-off state used by a real
    // board reset before the next boot reinitializes it.
    ESP_RETURN_ON_ERROR(write_register16(M5IOE1_REG_GPIO_OUT_L, 0),
                        TAG,
                        "turn off display power failed");
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_LOGI(TAG, "AMOLED power and reset held low before restart");
    return ESP_OK;
}
