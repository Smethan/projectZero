#pragma once
#include "platform.h"
static inline esp_err_t esp_flash_get_size(void *chip, uint32_t *size) {
    (void)chip;
#if CONFIG_PROJECTZERO_BOARD_WAVESHARE_ZERO
    *size=4u*1024u*1024u;
#else
    *size=8u*1024u*1024u;
#endif
    return ESP_OK;
}
