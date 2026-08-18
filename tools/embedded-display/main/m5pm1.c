#include "m5pm1.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define M5PM1_I2C_ADDRESS 0x6E
#define M5PM1_I2C_FREQUENCY_HZ 100000
#define M5PM1_TIMEOUT_MS 100

#define M5PM1_REG_DEVICE_ID 0x00
#define M5PM1_REG_DEVICE_MODEL 0x01
#define M5PM1_REG_HW_REV 0x02
#define M5PM1_REG_SW_REV 0x03
#define M5PM1_REG_PWR_CFG 0x06
#define M5PM1_REG_HOLD_CFG 0x07
#define M5PM1_REG_I2C_CFG 0x09
#define M5PM1_REG_WDT_CNT 0x0A
#define M5PM1_REG_BTN_CFG_1 0x49

#define M5PM1_PWR_CFG_CHG_EN (1u << 0)
#define M5PM1_HOLD_CFG_LDO (1u << 5)
#define M5PM1_BTN_SINGLE_CLICK_DELAY_MASK (3u << 1)

static const char *TAG = "m5pm1";
static i2c_master_dev_handle_t s_device;

static esp_err_t read_reg(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_device,
                                        &reg,
                                        1,
                                        value,
                                        1,
                                        pdMS_TO_TICKS(M5PM1_TIMEOUT_MS));
}

static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    const uint8_t packet[2] = {reg, value};
    const esp_err_t result = i2c_master_transmit(s_device,
                                                 packet,
                                                 sizeof(packet),
                                                 pdMS_TO_TICKS(M5PM1_TIMEOUT_MS));
    // PM1 applies configuration writes asynchronously.
    esp_rom_delay_us(500);
    return result;
}

static esp_err_t update_reg(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t current = 0;
    ESP_RETURN_ON_ERROR(read_reg(reg, &current), TAG, "read PM1 register 0x%02x failed", reg);
    current = (uint8_t)((current & (uint8_t)~mask) | (value & mask));
    return write_reg(reg, current);
}

esp_err_t openpencil_m5pm1_init(i2c_master_bus_handle_t bus)
{
    ESP_RETURN_ON_FALSE(bus, ESP_ERR_INVALID_ARG, TAG, "PM1 I2C bus is null");

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = M5PM1_I2C_ADDRESS,
        .scl_speed_hz = M5PM1_I2C_FREQUENCY_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &device_config, &s_device),
                        TAG,
                        "add M5PM1 failed");

    uint8_t id = 0;
    esp_err_t result = ESP_FAIL;
    for (int attempt = 0; attempt < 3; ++attempt) {
        // A PM1 may still be asleep immediately after a hardware wake.
        (void)i2c_master_probe(bus, M5PM1_I2C_ADDRESS, M5PM1_TIMEOUT_MS);
        vTaskDelay(pdMS_TO_TICKS(10));
        result = read_reg(M5PM1_REG_DEVICE_ID, &id);
        if (result == ESP_OK) break;
        ESP_LOGW(TAG, "PM1 probe failed (%s), retry %d/3", esp_err_to_name(result), attempt + 1);
        vTaskDelay(pdMS_TO_TICKS(attempt == 0 ? 100 : 400));
    }
    ESP_RETURN_ON_ERROR(result, TAG, "M5PM1 is not responding");

    uint8_t model = 0;
    uint8_t hw = 0;
    uint8_t sw = 0;
    ESP_RETURN_ON_ERROR(read_reg(M5PM1_REG_DEVICE_MODEL, &model), TAG, "read PM1 model failed");
    ESP_RETURN_ON_ERROR(read_reg(M5PM1_REG_HW_REV, &hw), TAG, "read PM1 hardware revision failed");
    ESP_RETURN_ON_ERROR(read_reg(M5PM1_REG_SW_REV, &sw), TAG, "read PM1 software revision failed");
    ESP_LOGI(TAG, "M5PM1 addr=0x%02x ID=0x%02x MODEL=0x%02x HW=0x%02x SW=0x%02x",
             M5PM1_I2C_ADDRESS, id, model, hw, sw);

    // Match M5Stack's StopWatch startup sequence. In particular, the LDO
    // hold lets the power-key shutdown/wake cycle preserve the 3.3 V domain.
    ESP_RETURN_ON_ERROR(write_reg(M5PM1_REG_I2C_CFG, 0), TAG, "disable PM1 I2C sleep failed");
    ESP_RETURN_ON_ERROR(write_reg(M5PM1_REG_WDT_CNT, 0), TAG, "disable PM1 watchdog failed");
    ESP_RETURN_ON_ERROR(update_reg(M5PM1_REG_HOLD_CFG, M5PM1_HOLD_CFG_LDO, M5PM1_HOLD_CFG_LDO),
                        TAG,
                        "enable PM1 LDO power hold failed");
    ESP_RETURN_ON_ERROR(update_reg(M5PM1_REG_PWR_CFG, M5PM1_PWR_CFG_CHG_EN, M5PM1_PWR_CFG_CHG_EN),
                        TAG,
                        "enable PM1 charging failed");

    // Use the official 1 s single-click debounce while leaving the PM1's
    // single-reset and double-off enable bits untouched.
    ESP_RETURN_ON_ERROR(update_reg(M5PM1_REG_BTN_CFG_1,
                                   M5PM1_BTN_SINGLE_CLICK_DELAY_MASK,
                                   M5PM1_BTN_SINGLE_CLICK_DELAY_MASK),
                        TAG,
                        "configure PM1 power-button delay failed");

    ESP_LOGI(TAG, "M5PM1 power-key wake configuration ready");
    return ESP_OK;
}
