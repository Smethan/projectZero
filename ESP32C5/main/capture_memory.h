#pragma once
#include "esp_heap_caps.h"
#include "esp_log.h"

/* Startup/stop diagnostics only: never log from a packet callback. Largest
 * contiguous block matters as well as total free memory for task/pool creation. */
static void capture_memory_log(const char *stage) {
    ESP_LOGI("capture_memory", "%s: internal_free=%u internal_largest=%u psram_free=%u",
             stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}
