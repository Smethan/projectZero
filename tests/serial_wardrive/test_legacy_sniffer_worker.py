from pathlib import Path

source = (Path(__file__).parents[2] / "ESP32C5/components/sniffer/sniffer.c").read_text()
callback = source.split("static void frame_callback", 1)[1].split("esp_err_t sniffer_init", 1)[0]
worker = source.split("static void post_slot", 1)[1].split("static void sniffer_worker", 1)[0]
stop = source.split("esp_err_t sniffer_stop", 1)[1]

assert "SNIFFER_POOL_SLOTS 8" in source
assert "SNIFFER_FRAME_BYTES 2304" in source
assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in source
assert "MALLOC_CAP_INTERNAL" not in source
assert "#if !CONFIG_SPIRAM" in source and "ESP_ERR_NOT_SUPPORTED" in source
assert "memcpy(dst, frame, length)" in callback
assert "xQueueReceive(free_slots, &slot, 0)" in callback
assert "xQueueSend(pending_slots, &slot, 0)" in callback
assert "esp_event_post" not in callback
assert "handlers[type](pkt, handler_contexts[type])" in worker
assert "esp_event_post" not in worker
assert "while (atomic_load(&producers) != 0" in stop
assert "if (xSemaphoreTake(worker_done, remaining) != pdTRUE) return ESP_ERR_TIMEOUT" in stop
assert stop.index("return ESP_ERR_TIMEOUT") < stop.index("heap_caps_free(pool)")
print("PASS: legacy sniffer uses bounded PSRAM slots and cooperative retirement")
