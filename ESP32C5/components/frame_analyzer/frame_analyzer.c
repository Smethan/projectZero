#include "frame_analyzer.h"

#include <string.h>
#include "esp_log.h"
#include "esp_wifi_types.h"
#include "frame_analyzer_parser.h"
#include "sniffer.h"

static const char *TAG = "frame_analyzer";
static uint8_t target_bssid[6];
static search_type_t search_type = SEARCH_HANDSHAKE;
static bool registered;

static void data_frame_handler(const wifi_promiscuous_pkt_t *frame, void *ctx) {
    (void)ctx;
    if (!frame || frame->rx_ctrl.sig_len < 24) return;

    frame_view_t view;
    if (!frame_view_parse(frame->payload, frame->rx_ctrl.sig_len, &view) ||
        !view.eapol_key || !frame_view_bssid_matches(&view, target_bssid)) return;

    if (search_type == SEARCH_HANDSHAKE) {
        const size_t event_size = sizeof(*frame) + frame->rx_ctrl.sig_len;
        if (esp_event_post(FRAME_ANALYZER_EVENTS, DATA_FRAME_EVENT_EAPOLKEY_FRAME,
                           frame, event_size, 0) != ESP_OK)
            ESP_LOGW(TAG, "Dropped EAPOL event because the event queue is full");
        return;
    }

    if (search_type == SEARCH_PMKID) {
        frame_analyzer_pmkid_event_t result = {0};
        if (!frame_view_extract_pmkids(&view, &result.matches)) return;
        memcpy(result.bssid, target_bssid, sizeof(result.bssid));
        if (esp_event_post(FRAME_ANALYZER_EVENTS, DATA_FRAME_EVENT_PMKID,
                           &result, sizeof(result), 0) != ESP_OK)
            ESP_LOGW(TAG, "Dropped PMKID event because the event queue is full");
    }
}

esp_err_t frame_analyzer_capture_start(search_type_t requested, const uint8_t *bssid) {
    if (!bssid) return ESP_ERR_INVALID_ARG;
    if (registered) frame_analyzer_capture_stop();
    memcpy(target_bssid, bssid, sizeof(target_bssid));
    search_type = requested;
    esp_err_t err = sniffer_set_frame_handler(WIFI_PKT_DATA, data_frame_handler, NULL);
    registered = err == ESP_OK;
    if (!registered) ESP_LOGE(TAG, "Could not register frame handler: %s", esp_err_to_name(err));
    return err;
}

void frame_analyzer_capture_stop(void) {
    if (!registered) return;
    esp_err_t err = sniffer_set_frame_handler(WIFI_PKT_DATA, NULL, NULL);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND)
        ESP_LOGW(TAG, "Could not unregister frame handler: %s", esp_err_to_name(err));
    registered = false;
}
