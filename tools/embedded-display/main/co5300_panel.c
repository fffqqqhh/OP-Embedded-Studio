#include "co5300_panel.h"

#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_vendor.h"
#include "display_presenter.h"

#define CO5300_SPI_HOST SPI2_HOST
#if CONFIG_OPENPENCIL_BOARD_M5STACK_STOPWATCH
#define CO5300_PCLK_HZ (80 * 1000 * 1000)
#elif CONFIG_OPENPENCIL_WIFI_LIVE_PREVIEW
// Realtime preview prioritizes a complete TE-synchronized frame over peak
// throughput. The lower clock gives Wi-Fi and PSRAM DMA more scheduling margin.
#define CO5300_PCLK_HZ (10 * 1000 * 1000)
#else
#define CO5300_PCLK_HZ (30 * 1000 * 1000)
#endif
#define CO5300_CS_GPIO CONFIG_EXAMPLE_PIN_NUM_LCD_CS
#define CO5300_PCLK_GPIO CONFIG_EXAMPLE_PIN_NUM_QSPI_PCLK
#define CO5300_DATA0_GPIO CONFIG_EXAMPLE_PIN_NUM_QSPI_DATA0
#define CO5300_DATA1_GPIO CONFIG_EXAMPLE_PIN_NUM_QSPI_DATA1
#define CO5300_DATA2_GPIO CONFIG_EXAMPLE_PIN_NUM_QSPI_DATA2
#define CO5300_DATA3_GPIO CONFIG_EXAMPLE_PIN_NUM_QSPI_DATA3
#define CO5300_RESET_GPIO CONFIG_EXAMPLE_PIN_NUM_LCD_RST
#define CO5300_WRITE_CMD_OPCODE 0x02U
#define CO5300_WRITE_COLOR_OPCODE 0x32U
#define CO5300_CMD_CASET 0x2AU
#define CO5300_CMD_RASET 0x2BU
#define CO5300_CMD_RAMWR 0x2CU
#define CO5300_CMD_RAMWRC 0x3CU

#if CONFIG_OPENPENCIL_BOARD_M5STACK_STOPWATCH
// M5Stack StopWatch uses a different CO5300 command table from the Waveshare
// 1.75C panel. In particular, TE is enabled with 0x35/0x80 and scanline 466.
static const co5300_lcd_init_cmd_t co5300_init_cmds[] = {
    {0x11, NULL, 0, 150},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x35, (uint8_t[]){0x80}, 1, 0},
    {0x44, (uint8_t[]){0x01, 0xD2}, 2, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x20, NULL, 0, 0},
    {0x36, (uint8_t[]){0x00}, 1, 0},
    {0x51, (uint8_t[]){0xA0}, 1, 0},
    {0x29, NULL, 0, 0},
};
#else
static const co5300_lcd_init_cmd_t co5300_init_cmds[] = {
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 600},
    {0x11, NULL, 0, 600},
    {0x29, NULL, 0, 0},
};
#endif

esp_err_t example_co5300_new_panel(int max_transfer_sz,
                                   esp_lcd_panel_io_handle_t *ret_io,
                                   esp_lcd_panel_handle_t *ret_panel)
{
    const spi_bus_config_t bus_config = CO5300_PANEL_BUS_QSPI_CONFIG(
        CO5300_PCLK_GPIO,
        CO5300_DATA0_GPIO,
        CO5300_DATA1_GPIO,
        CO5300_DATA2_GPIO,
        CO5300_DATA3_GPIO,
        max_transfer_sz);
    ESP_ERROR_CHECK(spi_bus_initialize(CO5300_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config =
        CO5300_PANEL_IO_QSPI_CONFIG(CO5300_CS_GPIO, NULL, NULL);
    io_config.pclk_hz = CO5300_PCLK_HZ;
#if CONFIG_OPENPENCIL_BOARD_M5STACK_STOPWATCH
    io_config.trans_queue_depth = OPENPENCIL_CO5300_STREAM_QUEUE_DEPTH;
#else
    io_config.trans_queue_depth = 10;
#endif
    io_config.on_color_trans_done = openpencil_display_presenter_on_color_done;

    co5300_vendor_config_t vendor_config = {
        .init_cmds = co5300_init_cmds,
        .init_cmds_size = sizeof(co5300_init_cmds) / sizeof(co5300_init_cmds[0]),
        .flags = {
            .use_qspi_interface = 1,
        },
    };

    esp_lcd_panel_io_handle_t io_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)CO5300_SPI_HOST,
        &io_config,
        &io_handle));

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = CO5300_RESET_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };

    esp_lcd_panel_handle_t panel_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 6, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    if (ret_io) *ret_io = io_handle;
    if (ret_panel) *ret_panel = panel_handle;
    return ESP_OK;
}

static int co5300_qspi_command(uint8_t opcode, uint8_t command)
{
    return ((int)opcode << 24) | ((int)command << 8);
}

esp_err_t example_co5300_stream_begin(esp_lcd_panel_io_handle_t io,
                                      int x,
                                      int y,
                                      int width,
                                      int height)
{
    ESP_RETURN_ON_FALSE(io && x >= 0 && y >= 0 && width > 0 && height > 0,
                        ESP_ERR_INVALID_ARG,
                        "co5300",
                        "invalid streamed window");

    const int x_start = x + CONFIG_EXAMPLE_LCD_X_GAP;
    const int y_start = y + CONFIG_EXAMPLE_LCD_Y_GAP;
    const int x_end = x_start + width - 1;
    const int y_end = y_start + height - 1;
    const uint8_t column[] = {
        (uint8_t)(x_start >> 8),
        (uint8_t)x_start,
        (uint8_t)(x_end >> 8),
        (uint8_t)x_end,
    };
    const uint8_t row[] = {
        (uint8_t)(y_start >> 8),
        (uint8_t)y_start,
        (uint8_t)(y_end >> 8),
        (uint8_t)y_end,
    };

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io,
                                                   co5300_qspi_command(CO5300_WRITE_CMD_OPCODE, CO5300_CMD_CASET),
                                                   column,
                                                   sizeof(column)),
                        "co5300",
                        "set streamed column window failed");
    return esp_lcd_panel_io_tx_param(io,
                                     co5300_qspi_command(CO5300_WRITE_CMD_OPCODE, CO5300_CMD_RASET),
                                     row,
                                     sizeof(row));
}

esp_err_t example_co5300_stream_color(esp_lcd_panel_io_handle_t io,
                                      const void *pixels,
                                      size_t byte_count,
                                      bool first_chunk)
{
    ESP_RETURN_ON_FALSE(io && pixels && byte_count > 0,
                        ESP_ERR_INVALID_ARG,
                        "co5300",
                        "invalid streamed color chunk");

    const int command = co5300_qspi_command(
        CO5300_WRITE_COLOR_OPCODE,
        first_chunk ? CO5300_CMD_RAMWR : CO5300_CMD_RAMWRC);
    return esp_lcd_panel_io_tx_color(io, command, pixels, byte_count);
}

esp_err_t example_co5300_stream_wait(esp_lcd_panel_io_handle_t io)
{
    ESP_RETURN_ON_FALSE(io, ESP_ERR_INVALID_ARG, "co5300", "invalid streamed IO handle");
    // tx_param() does not recycle queued color transactions. A color command
    // does, before it is issued, which makes this a transport-safe frame
    // boundary without relying on private panel-IO internals.
    return esp_lcd_panel_io_tx_color(io,
                                     co5300_qspi_command(CO5300_WRITE_COLOR_OPCODE, CO5300_CMD_RAMWRC),
                                     NULL,
                                     0);
}
