#include "sniffer.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define SNIFFER_POOL_SLOTS 8
#define SNIFFER_FRAME_BYTES 2304
#define SNIFFER_STOP_SLOT UINT8_MAX

ESP_EVENT_DEFINE_BASE(SNIFFER_EVENTS);
static const char *TAG = "sniffer";
static uint8_t *pool;
static uint8_t slot_types[SNIFFER_POOL_SLOTS];
static QueueHandle_t free_slots;
static QueueHandle_t pending_slots;
static SemaphoreHandle_t worker_done;
static TaskHandle_t worker_task;
static atomic_bool accepting;
static atomic_uint producers;
static atomic_uint drops;
static sniffer_frame_handler_t handlers[3];
static void *handler_contexts[3];

static void post_slot(uint8_t slot) {
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)(pool + (size_t)slot * SNIFFER_FRAME_BYTES);
    unsigned type = slot_types[slot];
    if (type <= WIFI_PKT_CTRL && handlers[type]) handlers[type](pkt, handler_contexts[type]);
    xQueueSend(free_slots, &slot, 0);
}

static void sniffer_worker(void *arg) {
    (void)arg;
    uint8_t slot;
    while (xQueueReceive(pending_slots, &slot, portMAX_DELAY) == pdTRUE) {
        if (slot == SNIFFER_STOP_SLOT) break;
        post_slot(slot);
    }
    /* No pool access follows the completion signal. */
    worker_task = NULL;
    xSemaphoreGive(worker_done);
    vTaskDelete(NULL);
}

static void frame_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    atomic_fetch_add(&producers, 1);
    if (!atomic_load(&accepting)) {
        atomic_fetch_sub(&producers, 1);
        return;
    }
    if (!buf || (type != WIFI_PKT_DATA && type != WIFI_PKT_MGMT && type != WIFI_PKT_CTRL)) {
        atomic_fetch_add(&drops, 1);
        atomic_fetch_sub(&producers, 1);
        return;
    }
    wifi_promiscuous_pkt_t *frame = buf;
    size_t length = sizeof(*frame) + frame->rx_ctrl.sig_len;
    if (length > SNIFFER_FRAME_BYTES) {
        atomic_fetch_add(&drops, 1);
        atomic_fetch_sub(&producers, 1);
        return;
    }
    uint8_t slot;
    if (xQueueReceive(free_slots, &slot, 0) != pdTRUE) {
        atomic_fetch_add(&drops, 1);
        atomic_fetch_sub(&producers, 1);
        return;
    }
    uint8_t *dst = pool + (size_t)slot * SNIFFER_FRAME_BYTES;
    slot_types[slot] = (uint8_t)type;
    memcpy(dst, frame, length);
    if (xQueueSend(pending_slots, &slot, 0) != pdTRUE) {
        xQueueSend(free_slots, &slot, 0);
        atomic_fetch_add(&drops, 1);
    }
    atomic_fetch_sub(&producers, 1);
}

esp_err_t sniffer_set_frame_handler(wifi_promiscuous_pkt_type_t type,
                                    sniffer_frame_handler_t handler, void *ctx) {
    if (type > WIFI_PKT_CTRL || worker_task || atomic_load(&accepting) || atomic_load(&producers))
        return ESP_ERR_INVALID_STATE;
    handlers[type] = handler;
    handler_contexts[type] = handler ? ctx : NULL;
    return ESP_OK;
}

esp_err_t sniffer_init(void) {
#if !CONFIG_SPIRAM
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (worker_task || pool) return ESP_ERR_INVALID_STATE;
    if (!esp_psram_is_initialized()) return ESP_ERR_NOT_SUPPORTED;
    pool = heap_caps_malloc(SNIFFER_POOL_SLOTS * SNIFFER_FRAME_BYTES,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    free_slots = xQueueCreate(SNIFFER_POOL_SLOTS, sizeof(uint8_t));
    pending_slots = xQueueCreate(SNIFFER_POOL_SLOTS, sizeof(uint8_t));
    worker_done = xSemaphoreCreateBinary();
    if (!pool || !free_slots || !pending_slots || !worker_done) goto fail;
    for (uint8_t i = 0; i < SNIFFER_POOL_SLOTS; ++i)
        if (xQueueSend(free_slots, &i, 0) != pdTRUE) goto fail;
    atomic_store(&drops, 0);
    atomic_store(&producers, 0);
    atomic_store(&accepting, false);
    if (xTaskCreate(sniffer_worker, "legacy_sniff", 4096, NULL, 5,
                    &worker_task) != pdPASS) goto fail;

    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_DATA | WIFI_PROMIS_FILTER_MASK_MGMT
    };
    esp_err_t err = esp_wifi_set_promiscuous_filter(&filter);
    if (err != ESP_OK) goto fail_wifi;
    err = esp_wifi_set_promiscuous_rx_cb(frame_callback);
    if (err != ESP_OK) goto fail_wifi;
    atomic_store(&accepting, true);
    err = esp_wifi_set_promiscuous(true);
    if (err != ESP_OK) {
        atomic_store(&accepting, false);
        esp_wifi_set_promiscuous_rx_cb(NULL);
        goto fail_wifi;
    }
    return ESP_OK;

fail_wifi:
    if (worker_task) {
        uint8_t stop = SNIFFER_STOP_SLOT;
        xQueueSend(pending_slots, &stop, 0);
        if (xSemaphoreTake(worker_done, pdMS_TO_TICKS(1000)) != pdTRUE)
            return ESP_ERR_TIMEOUT; /* retain resources while the worker may be live */
    }
fail:
    if (worker_done) vSemaphoreDelete(worker_done);
    if (pending_slots) vQueueDelete(pending_slots);
    if (free_slots) vQueueDelete(free_slots);
    if (pool) heap_caps_free(pool);
    worker_done = NULL; pending_slots = NULL; free_slots = NULL; pool = NULL;
    worker_task = NULL;
    return ESP_FAIL;
#endif
}

esp_err_t sniffer_stop(uint32_t timeout_ms) {
    if (!pool && !worker_task) return ESP_OK;
    atomic_store(&accepting, false);
    esp_err_t err = esp_wifi_set_promiscuous(false);
    esp_err_t cb_err = esp_wifi_set_promiscuous_rx_cb(NULL);
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (atomic_load(&producers) != 0 && xTaskGetTickCount() < deadline) taskYIELD();
    if (atomic_load(&producers) != 0) return ESP_ERR_TIMEOUT;
    TickType_t now = xTaskGetTickCount();
    TickType_t remaining = now < deadline ? deadline - now : 0;
    if (worker_task) {
        uint8_t stop = SNIFFER_STOP_SLOT;
        if (xQueueSend(pending_slots, &stop, remaining) != pdTRUE) return ESP_ERR_TIMEOUT;
        now = xTaskGetTickCount();
        remaining = now < deadline ? deadline - now : 0;
    }
    if (xSemaphoreTake(worker_done, remaining) != pdTRUE) return ESP_ERR_TIMEOUT;
    vSemaphoreDelete(worker_done); vQueueDelete(pending_slots); vQueueDelete(free_slots);
    heap_caps_free(pool);
    worker_done = NULL; pending_slots = NULL; free_slots = NULL; pool = NULL;
    if (err != ESP_OK) return err;
    return cb_err == ESP_OK ? ESP_OK : cb_err;
}

uint32_t sniffer_get_drop_count(void) { return atomic_load(&drops); }
