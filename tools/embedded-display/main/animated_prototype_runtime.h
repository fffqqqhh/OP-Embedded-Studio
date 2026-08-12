#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

esp_err_t openpencil_wireless_animated_prototype_run(esp_lcd_panel_handle_t panel,
                                                      uint16_t *frame_buffer);
