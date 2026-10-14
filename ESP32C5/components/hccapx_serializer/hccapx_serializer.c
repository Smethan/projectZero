#include "hccapx_serializer.h"

#include <string.h>
#include "esp_log.h"
#include "frame_analyzer_parser.h"

#define HCCAPX_SIGNATURE 0x58504348U
#define HCCAPX_VERSION 4U
#define HCCAPX_KEYVER_WPA2 2U
#define HCCAPX_MAX_EAPOL_SIZE 256U

static const char *TAG = "hccapx_serializer";
static hccapx_t hccapx;
static bool have_m1, have_m2, have_m3;
static bool eapol_saved;
static uint8_t eapol_source;
static uint8_t replay_m1[8], replay_m2[8], replay_m3[8];

static bool all_zero(const uint8_t *data, size_t len) {
    uint8_t accum = 0;
    for (size_t i = 0; i < len; ++i) accum |= data[i];
    return accum == 0;
}

static void clear_session(void) {
    memset(&hccapx, 0, sizeof(hccapx));
    hccapx.signature = HCCAPX_SIGNATURE;
    hccapx.version = HCCAPX_VERSION;
    hccapx.message_pair = 255;
    hccapx.keyver = HCCAPX_KEYVER_WPA2;
    have_m1 = have_m2 = have_m3 = false;
    eapol_saved = false;
    eapol_source = 0;
    memset(replay_m1, 0, sizeof(replay_m1));
    memset(replay_m2, 0, sizeof(replay_m2));
    memset(replay_m3, 0, sizeof(replay_m3));
}

void hccapx_serializer_init(const uint8_t *ssid, unsigned size) {
    clear_session();
    if (!ssid || size == 0 || size > sizeof(hccapx.essid)) {
        ESP_LOGW(TAG, "Ignoring invalid SSID length %u", size);
        return;
    }
    hccapx.essid_len = (uint8_t)size;
    memcpy(hccapx.essid, ssid, size);
}

hccapx_t *hccapx_serializer_get(void) {
    return hccapx.message_pair == 255 ? NULL : &hccapx;
}

static void counter_increment(const uint8_t in[8], uint8_t out[8]) {
    memcpy(out, in, 8);
    for (int i = 7; i >= 0; --i) if (++out[i] != 0) break;
}

static bool counter_matches_next(const uint8_t base[8], const uint8_t candidate[8]) {
    uint8_t next[8];
    counter_increment(base, next);
    return memcmp(next, candidate, 8) == 0;
}

static bool save_eapol(const frame_view_t *view, uint8_t source) {
    if (!view || !view->eapol || !view->mic || view->eapol_len < 99 ||
        view->eapol_len > HCCAPX_MAX_EAPOL_SIZE) return false;
    memset(hccapx.eapol, 0, sizeof(hccapx.eapol));
    memcpy(hccapx.eapol, view->eapol, view->eapol_len);
    hccapx.eapol_len = (uint16_t)view->eapol_len;
    memcpy(hccapx.keymic, view->mic, sizeof(hccapx.keymic));
    memset(hccapx.eapol + 81, 0, 16);
    hccapx.keyver = (uint8_t)(view->key_info & 7U);
    eapol_source = source;
    eapol_saved = true;
    return true;
}

void hccapx_serializer_add_frame(const frame_view_t *view) {
    if (!view || !view->eapol_key || !view->addr1 || !view->addr2 || !view->bssid ||
        hccapx.essid_len == 0 || view->eapol_key_len < 95) return;
    uint8_t msg = frame_view_eapol_message(view);
    if (msg < 1 || msg > 4) return;

    const bool from_ap = memcmp(view->addr2, view->bssid, 6) == 0;
    const bool to_ap = memcmp(view->addr1, view->bssid, 6) == 0;
    if (from_ap == to_ap) return;
    if ((msg == 1 || msg == 3) != from_ap) return;

    const uint8_t *ap = view->bssid;
    const uint8_t *sta = from_ap ? view->addr1 : view->addr2;
    if (!all_zero(hccapx.mac_ap, 6) && memcmp(hccapx.mac_ap, ap, 6) != 0) return;
    if (!all_zero(hccapx.mac_sta, 6) && memcmp(hccapx.mac_sta, sta, 6) != 0) return;

    const uint8_t *replay = view->eapol_key + 5;
    switch (msg) {
        case 1:
            if (all_zero(view->nonce, 32) ||
                (have_m2 && memcmp(replay, replay_m2, 8) != 0) ||
                (have_m3 && !counter_matches_next(replay, replay_m3))) return;
            memcpy(hccapx.mac_ap, ap, 6);
            memcpy(hccapx.nonce_ap, view->nonce, 32);
            memcpy(replay_m1, replay, 8);
            have_m1 = true;
            if (have_m2 && memcmp(replay_m1, replay_m2, 8) != 0) return;
            break;
        case 2:
            if (all_zero(view->nonce, 32)) return;
            if (have_m1 && memcmp(replay_m1, replay, 8) != 0) return;
            if (have_m3 && !counter_matches_next(replay, replay_m3)) return;
            memcpy(hccapx.mac_ap, ap, 6);
            memcpy(hccapx.mac_sta, sta, 6);
            memcpy(hccapx.nonce_sta, view->nonce, 32);
            memcpy(replay_m2, replay, 8);
            have_m2 = true;
            if (!eapol_saved && !save_eapol(view, 2)) return;
            if (have_m1) hccapx.message_pair = 0;
            else if (have_m3 && eapol_source == 3) hccapx.message_pair = 3;
            break;
        case 3:
            if (all_zero(view->nonce, 32)) return;
            if (have_m1 && !counter_matches_next(replay_m1, replay)) return;
            if (!have_m1 && have_m2 && !counter_matches_next(replay_m2, replay)) return;
            memcpy(hccapx.mac_ap, ap, 6);
            memcpy(hccapx.nonce_ap, view->nonce, 32); /* M3 fallback when M1 was missed. */
            memcpy(replay_m3, replay, 8);
            have_m3 = true;
            if (!eapol_saved && !save_eapol(view, 3)) return;
            if (eapol_source == 2) hccapx.message_pair = 2;
            else if (have_m2) hccapx.message_pair = 3;
            break;
        case 4:
            if (!have_m1 && !have_m3) return;
            if (have_m3 && memcmp(replay_m3, replay, 8) != 0) return;
            if (have_m1 && !have_m3 && !counter_matches_next(replay_m1, replay)) return;
            if (!have_m2) return;
            memcpy(hccapx.mac_ap, ap, 6);
            memcpy(hccapx.mac_sta, sta, 6);
            if (eapol_source == 3) hccapx.message_pair = 4;
            else if (!eapol_saved && save_eapol(view, 4)) hccapx.message_pair = have_m3 ? 5 : 1;
            break;
    }
    ESP_LOGD(TAG, "Processed EAPOL M%u, pair=%u", msg, hccapx.message_pair);
}
