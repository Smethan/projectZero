#ifndef SNIFFER_H
#define SNIFFER_H

#include "esp_err.h"
#include "esp_event.h"
#include "esp_wifi_types.h"
#include <stdint.h>

ESP_EVENT_DECLARE_BASE(SNIFFER_EVENTS);
enum { SNIFFER_EVENT_CAPTURED_DATA, SNIFFER_EVENT_CAPTURED_MGMT, SNIFFER_EVENT_CAPTURED_CTRL };
typedef void (*sniffer_frame_handler_t)(const wifi_promiscuous_pkt_t *frame, void *ctx);

/* Handlers run serially on the bounded sniffer worker, never in Wi-Fi/event-loop callbacks. */
esp_err_t sniffer_set_frame_handler(wifi_promiscuous_pkt_type_t type,
                                    sniffer_frame_handler_t handler, void *ctx);

/* This legacy manual capture path requires PSRAM; the Zero returns NOT_SUPPORTED. */
esp_err_t sniffer_init(void);
/* Disables intake, drains and retires the worker, then releases its PSRAM pool. */
esp_err_t sniffer_stop(uint32_t timeout_ms);
uint32_t sniffer_get_drop_count(void);

#endif
