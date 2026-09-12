#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_console.h"
bool sw_active(void);
bool sw_hs_mode(void);
bool sw_stop(void);
void sw_ble(const uint8_t *addr, uint8_t type, int rssi, uint8_t event, const uint8_t *data, size_t len);
void sw_register(void);
esp_err_t sw_register_command(const esp_console_cmd_t *cmd);
/* Implemented by main.c; invoked only by the serial session owner. */
bool sw_radio_start(void);
void sw_radio_stop(void);
void sw_radio_hop(unsigned index);

bool sw_prepare(void);
