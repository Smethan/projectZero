#include "frame_analyzer_parser.h"

#include <stdio.h>
#include <string.h>

#define IEEE80211_DATA_TYPE 2U
#define IEEE80211_QOS_SUBTYPE 8U
#define EAPOL_KEY_TYPE 3U
#define EAPOL_HEADER_SIZE 4U
#define EAPOL_KEY_FIXED_SIZE 95U

static uint16_t read_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

void print_raw_frame(const void *frame) { (void)frame; }
void print_mac_address(const uint8_t *address) { (void)address; }

bool frame_view_parse(const uint8_t *bytes, size_t frame_len, frame_view_t *out) {
    if (!bytes || !out) return false;
    memset(out, 0, sizeof(*out));
    if (frame_len < 24 || ((bytes[0] >> 2) & 3U) != IEEE80211_DATA_TYPE) return false;

    const uint8_t subtype = (uint8_t)((bytes[0] >> 4) & 0x0fU);
    const bool to_ds = (bytes[1] & 0x01U) != 0;
    const bool from_ds = (bytes[1] & 0x02U) != 0;
    const bool protected_frame = (bytes[1] & 0x40U) != 0;
    size_t header_len = 24;
    if (to_ds && from_ds) header_len += 6;
    const bool qos = (subtype & IEEE80211_QOS_SUBTYPE) != 0;
    if (qos) header_len += 2;
    if (qos && (bytes[1] & 0x80U)) header_len += 4; /* HT control */
    if (frame_len < header_len) return false;

    out->bytes = bytes;
    out->frame_len = frame_len;
    out->mac_header = bytes;
    out->mac_header_len = header_len;
    out->addr1 = bytes + 4;
    out->addr2 = bytes + 10;
    out->addr3 = bytes + 16;
    if (to_ds && !from_ds) out->bssid = out->addr1;
    else if (from_ds && !to_ds) out->bssid = out->addr2;
    else if (!to_ds && !from_ds) out->bssid = out->addr3;
    if (protected_frame) return true;

    size_t offset = header_len;
    if (frame_len - offset < 8) return true;
    /* LLC/SNAP: AA AA 03, OUI 00:00:00, EtherType 88:8e. */
    if (bytes[offset] != 0xaa || bytes[offset + 1] != 0xaa ||
        bytes[offset + 2] != 0x03 || bytes[offset + 3] != 0 ||
        bytes[offset + 4] != 0 || bytes[offset + 5] != 0 ||
        read_be16(bytes + offset + 6) != ETHER_TYPE_EAPOL) return true;

    offset += 8;
    const size_t remaining = frame_len - offset;
    if (remaining < EAPOL_HEADER_SIZE) return true;
    const size_t body_len = read_be16(bytes + offset + 2);
    if (body_len > remaining - EAPOL_HEADER_SIZE) return true;
    out->eapol = bytes + offset;
    out->eapol_len = EAPOL_HEADER_SIZE + body_len;
    if (out->eapol[1] != EAPOL_KEY_TYPE || body_len < EAPOL_KEY_FIXED_SIZE) return true;

    const uint8_t *key = out->eapol + EAPOL_HEADER_SIZE;
    const size_t key_len = body_len;
    const size_t key_data_len = read_be16(key + 93);
    if (key_data_len > key_len - EAPOL_KEY_FIXED_SIZE) return true;
    out->eapol_key = key;
    out->eapol_key_len = key_len;
    out->key_info = read_be16(key + 1);
    out->nonce = key + 13;
    out->mic = key + 77;
    out->key_data = key + EAPOL_KEY_FIXED_SIZE;
    out->key_data_len = key_data_len;
    return true;
}

bool frame_view_bssid_matches(const frame_view_t *view, const uint8_t bssid[6]) {
    return view && view->bssid && bssid && memcmp(view->bssid, bssid, 6) == 0;
}

bool frame_view_extract_pmkids(const frame_view_t *view, pmkid_list_t *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!view || !view->eapol_key || !view->key_data ||
        (view->key_info & 0x1000U) || !view->key_data_len) return false;

    size_t offset = 0;
    while (offset < view->key_data_len) {
        const size_t remaining = view->key_data_len - offset;
        if (remaining < 2) { memset(out, 0, sizeof(*out)); return false; }
        const uint8_t *element = view->key_data + offset;
        const size_t element_len = element[1];
        if (element_len > remaining - 2) { memset(out, 0, sizeof(*out)); return false; }

        if (element[0] == 0xdd && element_len >= 4 &&
            element[2] == 0x00 && element[3] == 0x0f && element[4] == 0xac &&
            element[5] == 0x04) {
            if (element_len < 20) { memset(out, 0, sizeof(*out)); return false; }
            if (out->count < FRAME_VIEW_MAX_PMKIDS)
                memcpy(out->pmkids[out->count++], element + 6, 16);
        }
        offset += element_len + 2;
    }
    return out->count != 0;
}

uint8_t frame_view_eapol_message(const frame_view_t *view) {
    if (!view || !view->eapol_key || view->eapol_key_len < EAPOL_KEY_FIXED_SIZE) return 0;
    const uint16_t info = view->key_info;
    const bool ack = (info & 0x0080U) != 0;
    const bool install = (info & 0x0040U) != 0;
    const bool mic = (info & 0x0100U) != 0;
    const bool nonce = memcmp(view->nonce, (uint8_t[32]){0}, 32) != 0;
    if (ack && !install && !mic) return 1;
    if (ack && install && mic) return 3;
    if (!ack && mic && !install) return nonce ? 2 : 4;
    return 0;
}
