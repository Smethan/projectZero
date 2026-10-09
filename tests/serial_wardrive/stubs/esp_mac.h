#pragma once
#include "platform.h"
typedef enum { ESP_MAC_EFUSE_FACTORY = 0 } esp_mac_type_t;
static inline esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type) {
    (void)type; const uint8_t value[6]={0x02,0,0,0,0,1}; memcpy(mac,value,6); return ESP_OK;
}
