#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#if CONFIG_OPENPENCIL_BOARD_M5STACK_STOPWATCH
// Three buffers allow one buffer to be filled while two full DMA transfers
// are in flight. Keep this paired with the StopWatch SPI queue depth below.
#define OPENPENCIL_CO5300_STREAM_QUEUE_DEPTH 2
#endif

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t example_co5300_new_panel(int max_transfer_sz,
                                   esp_lcd_panel_io_handle_t *ret_io,
                                   esp_lcd_panel_handle_t *ret_panel);

esp_err_t example_co5300_stream_begin(esp_lcd_panel_io_handle_t io,
                                      int x,
                                      int y,
                                      int width,
                                      int height);
esp_err_t example_co5300_stream_color(esp_lcd_panel_io_handle_t io,
                                      const void *pixels,
                                      size_t byte_count,
                                      bool first_chunk);
esp_err_t example_co5300_stream_wait(esp_lcd_panel_io_handle_t io);

#ifdef __cplusplus
}
#endif
