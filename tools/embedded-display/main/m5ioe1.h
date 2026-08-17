#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t openpencil_m5ioe1_display_init(void);
esp_err_t openpencil_m5ioe1_display_power_down(void);

#ifdef __cplusplus
}
#endif
