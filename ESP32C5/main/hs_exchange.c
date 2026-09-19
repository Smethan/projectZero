#include "hs_exchange.h"

#include <string.h>

#define HSX_HCCAPX_SIGNATURE 0x58504348U
#define HSX_HCCAPX_VERSION 4U
#define HSX_EAPOL_KEY 3U
#define HSX_EAPOL_MAX 256U

typedef struct {
    uint8_t message;
    uint8_t bssid[6];
    uint8_t sta[6];
    uint8_t replay[8];
    uint8_t nonce[32];
    uint8_t keyver;
    uint8_t mic[16];
    const uint8_t *eapol;
    uint16_t eapol_len;
} hsx_parsed_t;

static bool nonzero(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) if (data[i]) return true;
    return false;
}

static bool replay_normalize(uint8_t replay[8], uint8_t message) {
    if (message < 3) return true;
    for (int i = 7; i >= 0; i--) {
        if (replay[i]) {
            replay[i]--;
            return true;
        }
        replay[i] = 0xff;
    }
    return false;
}

static bool parse_eapol(const uint8_t *frame, size_t len, hsx_parsed_t *out) {
    if (!frame || !out || len < 24 || (frame[0] & 3)) return false;
    unsigned type = (frame[0] >> 2) & 3;
    unsigned subtype = frame[0] >> 4;
    unsigned ds = frame[1] & 3;
    if (type != 2 || ds == 0 || ds == 3 || (frame[1] & 0x40)) return false;

    size_t header = 24;
    if (subtype & 8) {
        if (len < header + 2 || (frame[header] & 0x80)) return false;
        header += 2;
        if (frame[1] & 0x80) header += 4;
    }
    static const uint8_t llc[] = {0xaa, 0xaa, 3, 0, 0, 0, 0x88, 0x8e};
    if (len < header + sizeof(llc) + 4) return false;
    if (memcmp(frame + header, llc, sizeof(llc))) return false;
    const uint8_t *eapol = frame + header + sizeof(llc);
    size_t body_len = ((size_t)eapol[2] << 8) | eapol[3];
    if (eapol[0] < 1 || eapol[0] > 3 || eapol[1] != HSX_EAPOL_KEY ||
        body_len < 95 ||
        body_len > HSX_EAPOL_MAX - 4U ||
        body_len + 4U > len - header - sizeof(llc))
        return false;
    uint16_t eapol_len = (uint16_t)(body_len + 4U);

    const uint8_t *key = eapol + 4;
    uint16_t key_info = ((uint16_t)key[1] << 8) | key[2];
    size_t key_data_len = ((size_t)key[93] << 8) | key[94];
    if ((key[0] != 2 && key[0] != 254) || !(key_info & 0x0008U) ||
        (key_info & (0x0400U | 0x0800U)) ||
        key_data_len != body_len - 95U) return false;
    bool ack = (key_info & 0x0080U) != 0;
    bool install = (key_info & 0x0040U) != 0;
    bool mic = (key_info & 0x0100U) != 0;
    uint8_t message = 0;
    if (ack && !install && !mic) message = 1;
    else if (!ack && !install && mic && nonzero(key + 13, 32)) message = 2;
    else if (ack && install && mic) message = 3;
    else if (!ack && !install && mic && !nonzero(key + 13, 32)) message = 4;
    if (!message) return false;
    if (mic && !nonzero(key + 77, 16)) return false;
    if ((message == 1 || message == 3) != (ds == 2)) return false;
    if ((message == 2 || message == 4) != (ds == 1)) return false;

    memset(out, 0, sizeof(*out));
    out->message = message;
    if (ds == 2) {
        memcpy(out->bssid, frame + 10, 6);
        memcpy(out->sta, frame + 4, 6);
    } else {
        memcpy(out->bssid, frame + 4, 6);
        memcpy(out->sta, frame + 10, 6);
    }
    if ((out->bssid[0] & 1) || (out->sta[0] & 1) ||
        !nonzero(out->bssid, 6) || !nonzero(out->sta, 6)) return false;
    memcpy(out->replay, key + 5, 8);
    if (!replay_normalize(out->replay, message)) return false;
    memcpy(out->nonce, key + 13, 32);
    out->keyver = (uint8_t)(key_info & 7U);
    if (out->keyver == 0 || out->keyver > 3) return false;
    memcpy(out->mic, key + 77, 16);
    out->eapol = eapol;
    out->eapol_len = eapol_len;
    return true;
}

static hsx_entry_t *find_entry(hsx_state_t *state, const hsx_parsed_t *p) {
    for (unsigned i = 0; i < HSX_EXCHANGE_LIMIT; i++) {
        hsx_entry_t *entry = &state->entries[i];
        if (entry->used && !memcmp(entry->bssid, p->bssid, 6) &&
            !memcmp(entry->sta, p->sta, 6) && !memcmp(entry->replay, p->replay, 8))
            return entry;
    }
    return NULL;
}

static hsx_entry_t *allocate_entry(hsx_state_t *state) {
    hsx_entry_t *oldest = NULL;
    for (unsigned i = 0; i < HSX_EXCHANGE_LIMIT; i++) {
        hsx_entry_t *entry = &state->entries[i];
        if (!entry->used) return entry;
        if (!entry->complete && (!oldest || entry->age < oldest->age)) oldest = entry;
    }
    return oldest;
}

static bool build_hccapx_candidate(hsx_entry_t *entry, bool m12) {
    uint8_t required = m12 ? 0x03U : 0x06U;
    if (!entry->ssid_len || (entry->message_mask & required) != required)
        return false;
    const hsx_frame_t *ap_frame = &entry->messages[m12 ? 0 : 2];
    const hsx_frame_t *sta_frame = &entry->messages[1];
    hsx_parsed_t ap = {0}, sta = {0};
    if (!parse_eapol(ap_frame->data, ap_frame->len, &ap) ||
        !parse_eapol(sta_frame->data, sta_frame->len, &sta) ||
        ap.message != (m12 ? 1 : 3) || sta.message != 2)
        return false;
    if (memcmp(ap.bssid, sta.bssid, 6) || memcmp(ap.sta, sta.sta, 6) ||
        memcmp(ap.replay, sta.replay, 8) || ap.keyver != sta.keyver ||
        !nonzero(ap.nonce, sizeof(ap.nonce)) ||
        !nonzero(sta.nonce, sizeof(sta.nonce)) ||
        !nonzero(sta.mic, sizeof(sta.mic))) return false;

    hsx_hccapx_t *h = &entry->hccapx;
    memset(h, 0, sizeof(*h));
    h->signature = HSX_HCCAPX_SIGNATURE;
    h->version = HSX_HCCAPX_VERSION;
    h->message_pair = m12 ? 0 : 2;
    h->essid_len = entry->ssid_len;
    memcpy(h->essid, entry->ssid, entry->ssid_len);
    h->keyver = sta.keyver;
    memcpy(h->keymic, sta.mic, sizeof(h->keymic));
    memcpy(h->mac_ap, entry->bssid, 6);
    memcpy(h->nonce_ap, ap.nonce, sizeof(h->nonce_ap));
    memcpy(h->mac_sta, entry->sta, 6);
    memcpy(h->nonce_sta, sta.nonce, sizeof(h->nonce_sta));
    h->eapol_len = sta.eapol_len;
    memcpy(h->eapol, sta.eapol, sta.eapol_len);
    if (sta.eapol_len >= 97) memset(h->eapol + 81, 0, 16);
    entry->complete = true;
    return true;
}

static void build_hccapx(hsx_entry_t *entry) {
    if (!entry || entry->complete) return;
    /* M1/M2 is preferred when both candidates are valid.  A malformed or
     * incompatible stored M1 must never mask an independently valid M2/M3. */
    if (!build_hccapx_candidate(entry, true))
        (void)build_hccapx_candidate(entry, false);
}

void hsx_reset(hsx_state_t *state) {
    if (state) memset(state, 0, sizeof(*state));
}

bool hsx_ingest_radio(hsx_state_t *state, const uint8_t *frame, size_t len,
                      uint32_t timestamp_us, uint8_t channel, int8_t rssi,
                const uint8_t *ssid, size_t ssid_len,
                hsx_result_t *result) {
    if (!state || !result || len > HSX_FRAME_MAX || ssid_len > 32 ||
        (ssid_len && !ssid)) return false;
    memset(result, 0, sizeof(*result));
    hsx_parsed_t parsed;
    if (!parse_eapol(frame, len, &parsed)) return false;
    result->accepted = true;
    result->message = parsed.message;
    memcpy(result->bssid, parsed.bssid, 6);
    memcpy(result->sta, parsed.sta, 6);

    hsx_entry_t *entry = find_entry(state, &parsed);
    if (!entry) {
        entry = allocate_entry(state);
        if (!entry) return true; /* Parsed for status, but bounded state is full. */
        memset(entry, 0, sizeof(*entry));
        entry->used = true;
        memcpy(entry->bssid, parsed.bssid, 6);
        memcpy(entry->sta, parsed.sta, 6);
        memcpy(entry->replay, parsed.replay, 8);
        entry->ssid_len = (uint8_t)ssid_len;
        if (ssid_len) memcpy(entry->ssid, ssid, ssid_len);
    }
    entry->age = ++state->clock;
    result->entry = entry;
    if (!entry->ssid_len && ssid_len) {
        entry->ssid_len = (uint8_t)ssid_len;
        memcpy(entry->ssid, ssid, ssid_len);
    }
    bool was_complete = entry->complete;
    uint8_t bit = (uint8_t)(1U << (parsed.message - 1U));
    if (entry->message_mask & bit) {
        /* Until a pair has been committed, a distinct, well-formed
         * retransmission replaces the first copy.  This lets a later valid
         * M1/M2/M3 repair an initially incompatible frame for the same
         * station and replay epoch. */
        if (!entry->complete && parsed.message <= 3) {
            hsx_frame_t *saved = &entry->messages[parsed.message - 1U];
            if (saved->len != len || memcmp(saved->data, frame, len)) {
                saved->len = (uint16_t)len;
                saved->channel = channel;
                saved->rssi = rssi;
                saved->timestamp_us = timestamp_us;
                memcpy(saved->data, frame, len);
                result->new_message = true;
            }
            build_hccapx(entry);
            result->became_complete = !was_complete && entry->complete;
        }
        return true;
    }
    entry->message_mask |= bit;
    result->new_message = true;
    if (parsed.message <= 4) {
        hsx_frame_t *saved = &entry->messages[parsed.message - 1U];
        saved->len = (uint16_t)len;
        saved->channel = channel;
        saved->rssi = rssi;
        saved->timestamp_us = timestamp_us;
        memcpy(saved->data, frame, len);
    }
    build_hccapx(entry);
    result->became_complete = !was_complete && entry->complete;
    return true;
}

bool hsx_ingest(hsx_state_t *state, const uint8_t *frame, size_t len,
                uint32_t timestamp_us, const uint8_t *ssid, size_t ssid_len,
                hsx_result_t *result) {
    return hsx_ingest_radio(state, frame, len, timestamp_us, 0, 0,
                            ssid, ssid_len, result);
}

void hsx_set_ap_ssid(hsx_state_t *state, const uint8_t bssid[6],
                     const uint8_t *ssid, size_t ssid_len) {
    if (!state || !bssid || !ssid || !ssid_len || ssid_len > 32) return;
    for (unsigned i = 0; i < HSX_EXCHANGE_LIMIT; i++) {
        hsx_entry_t *entry = &state->entries[i];
        if (!entry->used || memcmp(entry->bssid, bssid, 6)) continue;
        entry->ssid_len = (uint8_t)ssid_len;
        memcpy(entry->ssid, ssid, ssid_len);
        build_hccapx(entry);
    }
}

void hsx_remove_ap(hsx_state_t *state, const uint8_t bssid[6]) {
    if (!state || !bssid) return;
    for (unsigned i = 0; i < HSX_EXCHANGE_LIMIT; i++)
        if (state->entries[i].used && !memcmp(state->entries[i].bssid, bssid, 6))
            memset(&state->entries[i], 0, sizeof(state->entries[i]));
}

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

bool hsx_assoc_has_pmkid(const uint8_t *frame, size_t len) {
    if (!frame || len < 28 || (frame[0] & 3) || (frame[1] & 0x40) ||
        ((frame[0] >> 2) & 3) != 0)
        return false;
    unsigned subtype = frame[0] >> 4;
    size_t pos = 24 + (subtype == 0 ? 4U : subtype == 2 ? 10U : 0U);
    if (pos == 24 || pos > len) return false;
    while (pos + 2 <= len) {
        uint8_t id = frame[pos], size = frame[pos + 1];
        pos += 2;
        if (pos + size > len) return false;
        if (id == 48 && size >= 18) {
            const uint8_t *rsn = frame + pos;
            size_t remain = size, off = 0;
            if (remain < 8) return false;
            if (read_le16(rsn) != 1) return false;
            off = 2 + 4;
            uint16_t pairwise = read_le16(rsn + off); off += 2;
            if (pairwise > 64 || off + (size_t)pairwise * 4 + 2 > remain) return false;
            off += (size_t)pairwise * 4;
            uint16_t akm = read_le16(rsn + off); off += 2;
            if (akm > 64 || off + (size_t)akm * 4 + 4 > remain) return false;
            off += (size_t)akm * 4;
            off += 2; /* RSN capabilities */
            uint16_t count = read_le16(rsn + off); off += 2;
            if (!count || count > 16 || off + (size_t)count * 16 > remain) return false;
            for (uint16_t i = 0; i < count; i++)
                if (nonzero(rsn + off + (size_t)i * 16, 16)) return true;
            return false;
        }
        pos += size;
    }
    return false;
}

static void put16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
}
static void put32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16); p[3] = (uint8_t)(value >> 24);
}

static bool pcap_frame(uint8_t *out, size_t capacity, size_t *used,
                       const hsx_frame_t *frame) {
    if (!frame || !frame->len) return true;
    if (frame->len > HSX_FRAME_MAX || *used + 16U + frame->len > capacity) return false;
    uint8_t *record = out + *used;
    put32(record, frame->timestamp_us / 1000000U);
    put32(record + 4, frame->timestamp_us % 1000000U);
    put32(record + 8, frame->len);
    put32(record + 12, frame->len);
    memcpy(record + 16, frame->data, frame->len);
    *used += 16U + frame->len;
    return true;
}

size_t hsx_build_pcap(const hsx_entry_t *entry, const hsx_frame_t *beacon,
                      const hsx_frame_t *association, uint8_t *out,
                      size_t capacity) {
    if (!out || capacity < 24) return 0;
    memset(out, 0, 24);
    put32(out, 0xa1b2c3d4U);
    put16(out + 4, 2);
    put16(out + 6, 4);
    put32(out + 16, 65535U);
    put32(out + 20, 105U);
    size_t used = 24;
    if (!pcap_frame(out, capacity, &used, beacon) ||
        !pcap_frame(out, capacity, &used, association)) return 0;
    if (entry) {
        if (!entry->complete) return 0;
        for (unsigned i = 0; i < 4; i++)
            if (!pcap_frame(out, capacity, &used, &entry->messages[i])) return 0;
    }
    return used > 24 ? used : 0;
}

static size_t align4(size_t value) {
    return (value + 3U) & ~3U;
}

static uint16_t channel_frequency(uint8_t channel, uint16_t *flags) {
    if (flags) *flags = 0;
    if (channel == 14) {
        if (flags) *flags = 0x0080U;
        return 2484;
    }
    if (channel >= 1 && channel <= 13) {
        if (flags) *flags = 0x0080U;
        return (uint16_t)(2407U + channel * 5U);
    }
    if (channel >= 15 && channel <= 233) {
        if (flags) *flags = 0x0100U;
        return (uint16_t)(5000U + channel * 5U);
    }
    return 0;
}

static bool pcapng_frame(uint8_t *out, size_t capacity, size_t *used,
                         const hsx_frame_t *frame) {
    if (!frame || !frame->len) return true;
    if (frame->len > HSX_FRAME_MAX) return false;
    const size_t radiotap_len = 15;
    size_t packet_len = radiotap_len + frame->len;
    size_t block_len = 32U + align4(packet_len);
    if (*used + block_len > capacity) return false;
    uint8_t *block = out + *used;
    memset(block, 0, block_len);
    put32(block, 6U);                  /* Enhanced Packet Block */
    put32(block + 4, (uint32_t)block_len);
    put32(block + 8, 0U);             /* interface 0 */
    put32(block + 12, 0U);            /* timestamp high */
    put32(block + 16, frame->timestamp_us);
    put32(block + 20, (uint32_t)packet_len);
    put32(block + 24, (uint32_t)packet_len);
    uint8_t *packet = block + 28;
    put16(packet + 2, (uint16_t)radiotap_len);
    put32(packet + 4, 0x0000002aU);    /* flags, channel, dBm signal */
    packet[8] = 0;                     /* FCS was stripped */
    uint16_t channel_flags = 0;
    put16(packet + 10, channel_frequency(frame->channel, &channel_flags));
    put16(packet + 12, channel_flags);
    packet[14] = (uint8_t)frame->rssi;
    memcpy(packet + radiotap_len, frame->data, frame->len);
    put32(block + block_len - 4, (uint32_t)block_len);
    *used += block_len;
    return true;
}

size_t hsx_build_pcapng_with_context(const hsx_entry_t *entry,
                                     const hsx_frame_t *beacon,
                                     const hsx_frame_t *authentication,
                                     const hsx_frame_t *association,
                                     uint8_t *out, size_t capacity) {
    if (!out || capacity < 60) return 0;
    memset(out, 0, 60);
    /* Section Header Block: little endian, version 1.0, unknown length. */
    put32(out, 0x0a0d0d0aU); put32(out + 4, 28U);
    put32(out + 8, 0x1a2b3c4dU); put16(out + 12, 1U);
    memset(out + 16, 0xff, 8); put32(out + 24, 28U);
    /* Interface Description Block: radiotap, explicit microsecond resolution. */
    uint8_t *idb = out + 28;
    put32(idb, 1U); put32(idb + 4, 32U); put16(idb + 8, 127U);
    put32(idb + 12, HSX_FRAME_MAX + 15U);
    put16(idb + 16, 9U); put16(idb + 18, 1U); idb[20] = 6U;
    put32(idb + 24, 0U); put32(idb + 28, 32U);
    size_t used = 60;
    if (!pcapng_frame(out, capacity, &used, beacon) ||
        !pcapng_frame(out, capacity, &used, authentication) ||
        !pcapng_frame(out, capacity, &used, association)) return 0;
    if (entry) {
        if (!entry->complete) return 0;
        for (unsigned i = 0; i < 4; i++)
            if (!pcapng_frame(out, capacity, &used, &entry->messages[i])) return 0;
    }
    return used > 60 ? used : 0;
}

size_t hsx_build_pcapng(const hsx_entry_t *entry, const hsx_frame_t *beacon,
                        const hsx_frame_t *association, uint8_t *out,
                        size_t capacity) {
    return hsx_build_pcapng_with_context(entry, beacon, NULL, association,
                                         out, capacity);
}
