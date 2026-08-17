#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*openpencil_sequence_ready_callback_t)(void);

typedef struct {
    bool active;
    uint32_t fps_milli;
    uint32_t transfer_us;
    uint32_t present_us;
    uint32_t dropped_frames;
    uint16_t target_delay_ms;
} openpencil_sequence_player_metrics_t;

esp_err_t openpencil_sequence_player_run(esp_lcd_panel_handle_t panel,
                                      uint16_t *primary_frame_buffer,
                                      size_t frame_pixels,
                                      int width,
                                      int height,
                                      openpencil_sequence_ready_callback_t on_first_frame);
bool openpencil_sequence_player_get_metrics(openpencil_sequence_player_metrics_t *metrics);

#ifdef __cplusplus
}
#endif
