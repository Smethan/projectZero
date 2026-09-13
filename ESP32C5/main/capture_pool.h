#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* Fixed ownership: available -> producer -> ready -> consumer -> available.
 * Only pointers go through queues; callbacks neither allocate nor put a full
 * frame on the Wi-Fi task's stack. Close only after producers/consumer stop. */
#define CAPTURE_POOL_MAX 8
typedef struct {
    void *frames;
    QueueHandle_t available, ready;
    StaticQueue_t available_storage, ready_storage;
    uint8_t available_bytes[CAPTURE_POOL_MAX * sizeof(void *)];
    uint8_t ready_bytes[CAPTURE_POOL_MAX * sizeof(void *)];
} capture_pool_t;

static void capture_pool_close(capture_pool_t *p) {
    if (p->available) vQueueDelete(p->available);
    if (p->ready) vQueueDelete(p->ready);
    heap_caps_free(p->frames);
    p->available = p->ready = NULL;
    p->frames = NULL;
}

static bool capture_pool_open(capture_pool_t *p, unsigned count, size_t size) {
    if (p->frames || !count || count > CAPTURE_POOL_MAX || !size || size > SIZE_MAX/count) return false;
    p->frames = heap_caps_malloc(count * size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p->frames) p->frames = heap_caps_malloc(count * size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!p->frames) return false;
    p->available = xQueueCreateStatic(count, sizeof(void *), p->available_bytes, &p->available_storage);
    p->ready = xQueueCreateStatic(count, sizeof(void *), p->ready_bytes, &p->ready_storage);
    if (!p->available || !p->ready) { capture_pool_close(p); return false; }
    for (unsigned i = 0; i < count; ++i) {
        void *frame = (uint8_t *)p->frames + i * size;
        xQueueSend(p->available, &frame, 0);
    }
    return true;
}

static void *capture_pool_acquire(capture_pool_t *p) {
    void *frame = NULL;
    xQueueReceive(p->available, &frame, 0);
    return frame;
}

static void capture_pool_release(capture_pool_t *p, void *frame) {
    xQueueSend(p->available, &frame, 0);
}

static bool capture_pool_publish(capture_pool_t *p, void *frame) {
    if (xQueueSend(p->ready, &frame, 0) == pdTRUE) return true;
    capture_pool_release(p, frame);
    return false;
}
