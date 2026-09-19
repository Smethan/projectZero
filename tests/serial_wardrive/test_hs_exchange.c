#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../ESP32C5/main/hs_exchange.c"

#define FRAME_LEN 131

static const uint8_t ap1[6] = {0x02, 0, 0, 0, 0, 1};
static const uint8_t ap2[6] = {0x02, 0, 0, 0, 0, 2};
static const uint8_t sta1[6] = {0x0a, 0, 0, 0, 0, 1};
static const uint8_t sta2[6] = {0x0a, 0, 0, 0, 0, 2};

static void make_eapol(uint8_t frame[FRAME_LEN], unsigned message,
                       const uint8_t ap[6], const uint8_t sta[6],
                       unsigned replay) {
    memset(frame, 0, FRAME_LEN);
    frame[0] = 0x08;
    bool from_ap = message == 1 || message == 3;
    frame[1] = from_ap ? 2 : 1;
    if (from_ap) {
        memcpy(frame + 4, sta, 6);
        memcpy(frame + 10, ap, 6);
    } else {
        memcpy(frame + 4, ap, 6);
        memcpy(frame + 10, sta, 6);
    }
    memcpy(frame + 16, ap, 6);
    static const uint8_t llc[8] = {0xaa, 0xaa, 3, 0, 0, 0, 0x88, 0x8e};
    memcpy(frame + 24, llc, sizeof(llc));
    uint8_t *eapol = frame + 32;
    eapol[0] = 2;
    eapol[1] = 3;
    eapol[2] = 0;
    eapol[3] = 95;
    uint8_t *key = eapol + 4;
    key[0] = 2;
    uint16_t info = message == 1 ? 0x008aU : message == 2 ? 0x010aU :
                    message == 3 ? 0x01caU : 0x030aU;
    key[1] = (uint8_t)(info >> 8);
    key[2] = (uint8_t)info;
    key[12] = (uint8_t)replay;
    if (message != 4) {
        for (unsigned i = 0; i < 32; i++) key[13 + i] = (uint8_t)(message * 16 + i + 1);
    }
    if (message != 1) {
        for (unsigned i = 0; i < 16; i++) key[77 + i] = (uint8_t)(0xa0 + i);
    }
}

static hsx_result_t ingest(hsx_state_t *state, unsigned message,
                           const uint8_t ap[6], const uint8_t sta[6],
                           unsigned replay) {
    uint8_t frame[FRAME_LEN];
    make_eapol(frame, message, ap, sta, replay);
    hsx_result_t result;
    assert(hsx_ingest_radio(state, frame, sizeof(frame), message * 1000,
                            6, (int8_t)(-40 - (int)message),
                            (const uint8_t *)"network", 7, &result));
    assert(result.accepted && result.message == message);
    assert(!memcmp(result.bssid, ap, 6) && !memcmp(result.sta, sta, 6));
    return result;
}

static hsx_result_t ingest_frame(hsx_state_t *state, const uint8_t *frame,
                                 size_t len, uint32_t timestamp_us) {
    hsx_result_t result;
    assert(hsx_ingest_radio(state, frame, len, timestamp_us, 6, -42,
                            (const uint8_t *)"network", 7, &result));
    assert(result.accepted);
    return result;
}

static unsigned count_complete(const hsx_state_t *state, const uint8_t ap[6]) {
    unsigned count = 0;
    for (unsigned i = 0; i < HSX_EXCHANGE_LIMIT; i++)
        if (state->entries[i].used && state->entries[i].complete &&
            !memcmp(state->entries[i].bssid, ap, 6)) count++;
    return count;
}

static void test_same_exchange_rules(void) {
    hsx_state_t state;
    hsx_reset(&state);
    hsx_result_t m1 = ingest(&state, 1, ap1, sta1, 7);
    assert(m1.new_message && !m1.became_complete);
    hsx_result_t duplicate = ingest(&state, 1, ap1, sta1, 7);
    assert(!duplicate.new_message && !duplicate.became_complete);
    hsx_result_t m2 = ingest(&state, 2, ap1, sta1, 7);
    assert(m2.new_message && m2.became_complete && m2.entry->complete);
    assert(m2.entry->hccapx.signature == 0x58504348U);
    assert(m2.entry->hccapx.version == 4 && m2.entry->hccapx.message_pair == 0);
    assert(m2.entry->hccapx.keyver == 2 && m2.entry->hccapx.essid_len == 7);
    assert(!memcmp(m2.entry->hccapx.mac_ap, ap1, 6));
    assert(!memcmp(m2.entry->hccapx.mac_sta, sta1, 6));
    for (unsigned i = 81; i < 97; i++) assert(m2.entry->hccapx.eapol[i] == 0);
    assert(m2.entry->hccapx.eapol_len == 99);

    uint8_t pcap[HSX_PCAP_MAX];
    size_t size = hsx_build_pcap(m2.entry, NULL, NULL, pcap, sizeof(pcap));
    assert(size == 24 + 2 * (16 + FRAME_LEN));
    assert(!memcmp(pcap, "\xd4\xc3\xb2\xa1", 4));
    assert(!hsx_build_pcap(m2.entry, NULL, NULL, pcap, size - 1));

    hsx_reset(&state);
    ingest(&state, 1, ap1, sta1, 9);
    ingest(&state, 2, ap1, sta2, 9);
    assert(count_complete(&state, ap1) == 0); /* two stations never combine */

    hsx_reset(&state);
    ingest(&state, 1, ap1, sta1, 9);
    ingest(&state, 2, ap1, sta1, 10);
    assert(count_complete(&state, ap1) == 0); /* two replay epochs never combine */

    hsx_reset(&state);
    ingest(&state, 2, ap1, sta1, 20);
    hsx_result_t m3 = ingest(&state, 3, ap1, sta1, 21);
    assert(m3.became_complete && m3.entry->hccapx.message_pair == 2);

    hsx_reset(&state);
    ingest(&state, 3, ap1, sta1, 21);
    ingest(&state, 4, ap1, sta1, 21);
    assert(count_complete(&state, ap1) == 0); /* M4 carries no SNonce */
}

static void test_pair_choice_and_arrival_orders(void) {
    static const unsigned orders[][3] = {
        {0, 1, 2}, {0, 2, 1}, {1, 0, 2},
        {1, 2, 0}, {2, 0, 1}, {2, 1, 0},
    };
    uint8_t frames[3][FRAME_LEN];
    make_eapol(frames[0], 1, ap1, sta1, 40);
    make_eapol(frames[1], 2, ap1, sta1, 40);
    make_eapol(frames[2], 3, ap1, sta1, 41);

    for (unsigned order = 0; order < sizeof(orders) / sizeof(orders[0]); order++) {
        hsx_state_t state;
        hsx_reset(&state);
        hsx_result_t result = {0};
        unsigned completions = 0;
        for (unsigned i = 0; i < 3; i++) {
            unsigned which = orders[order][i];
            result = ingest_frame(&state, frames[which], FRAME_LEN,
                                  1000U + which);
            if (result.became_complete) completions++;
        }
        assert(result.entry && result.entry->complete && completions == 1);

        uint8_t pcap[HSX_PCAP_MAX];
        size_t size = hsx_build_pcap(result.entry, NULL, NULL, pcap,
                                     sizeof(pcap));
        assert(size == 24 + 3 * (16 + FRAME_LEN));
        const uint8_t *first = pcap + 24 + 16;
        const uint8_t *second = first + FRAME_LEN + 16;
        const uint8_t *third = second + FRAME_LEN + 16;
        assert(!memcmp(first, frames[0], FRAME_LEN));
        assert(!memcmp(second, frames[1], FRAME_LEN));
        assert(!memcmp(third, frames[2], FRAME_LEN));
    }
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t get16(const uint8_t *p) {
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static void test_rich_pcapng_and_m4_retention(void) {
    hsx_state_t state;
    hsx_reset(&state);
    ingest(&state, 1, ap1, sta1, 70);
    hsx_result_t result = ingest(&state, 2, ap1, sta1, 70);
    assert(result.entry && result.entry->complete);
    ingest(&state, 3, ap1, sta1, 71);
    result = ingest(&state, 4, ap1, sta1, 71);
    assert(result.entry && result.entry->message_mask == 0x0f);
    assert(result.entry->messages[3].len == FRAME_LEN);

    uint8_t pcap[HSX_PCAP_MAX];
    size_t pcap_size = hsx_build_pcap(result.entry, NULL, NULL, pcap,
                                      sizeof(pcap));
    assert(pcap_size == 24 + 4 * (16 + FRAME_LEN));

    uint8_t pcapng[HSX_PCAPNG_MAX];
    size_t size = hsx_build_pcapng(result.entry, NULL, NULL, pcapng,
                                   sizeof(pcapng));
    assert(size > pcap_size && get32(pcapng) == 0x0a0d0d0aU);
    assert(get32(pcapng + 4) == 28 && get32(pcapng + 24) == 28);
    assert(get32(pcapng + 28) == 1 && get16(pcapng + 36) == 127);
    unsigned packets = 0;
    for (size_t offset = 60; offset < size;) {
        assert(get32(pcapng + offset) == 6);
        uint32_t block_size = get32(pcapng + offset + 4);
        assert(block_size >= 32 && !(block_size & 3));
        assert(get32(pcapng + offset + block_size - 4) == block_size);
        uint32_t captured = get32(pcapng + offset + 20);
        const uint8_t *packet = pcapng + offset + 28;
        assert(captured == FRAME_LEN + 15 && get16(packet + 2) == 15);
        assert(get16(packet + 10) == 2437 && get16(packet + 12) == 0x0080);
        assert((int8_t)packet[14] < 0);
        assert(!memcmp(packet + 15, result.entry->messages[packets].data,
                       FRAME_LEN));
        packets++;
        offset += block_size;
    }
    assert(packets == 4);
    assert(!hsx_build_pcapng(result.entry, NULL, NULL, pcapng, size - 1));
}

static void test_poisoned_candidate_and_retransmission(void) {
    uint8_t poisoned_m1[FRAME_LEN], valid_m1[FRAME_LEN];
    uint8_t valid_m2[FRAME_LEN], valid_m3[FRAME_LEN];
    make_eapol(poisoned_m1, 1, ap1, sta1, 51);
    memset(poisoned_m1 + 36 + 13, 0, 32); /* parseable, unusable ANonce */
    make_eapol(valid_m1, 1, ap1, sta1, 51);
    make_eapol(valid_m2, 2, ap1, sta1, 51);
    make_eapol(valid_m3, 3, ap1, sta1, 52);

    hsx_state_t state;
    hsx_reset(&state);
    assert(ingest_frame(&state, poisoned_m1, FRAME_LEN, 1).new_message);
    hsx_result_t m2 = ingest_frame(&state, valid_m2, FRAME_LEN, 2);
    assert(m2.entry && !m2.entry->complete);
    hsx_result_t m3 = ingest_frame(&state, valid_m3, FRAME_LEN, 3);
    assert(m3.became_complete && m3.entry->hccapx.message_pair == 2);

    /* Without M3, a distinct valid M1 retransmission must replace the poisoned
     * first copy and make the already-stored M2 usable. */
    hsx_reset(&state);
    ingest_frame(&state, poisoned_m1, FRAME_LEN, 1);
    m2 = ingest_frame(&state, valid_m2, FRAME_LEN, 2);
    assert(m2.entry && !m2.entry->complete);
    hsx_result_t repaired = ingest_frame(&state, valid_m1, FRAME_LEN, 3);
    assert(repaired.new_message && repaired.became_complete);
    assert(repaired.entry->hccapx.message_pair == 0);
    assert(!memcmp(repaired.entry->messages[0].data, valid_m1, FRAME_LEN));
}

static void test_independent_aps_and_reset(void) {
    hsx_state_t state;
    hsx_reset(&state);
    ingest(&state, 1, ap1, sta1, 1);
    hsx_result_t first = ingest(&state, 2, ap1, sta1, 1);
    ingest(&state, 1, ap2, sta2, 4);
    hsx_result_t second = ingest(&state, 2, ap2, sta2, 4);
    assert(first.entry != second.entry);
    assert(count_complete(&state, ap1) == 1 && count_complete(&state, ap2) == 1);

    hsx_entry_t snapshot = *first.entry;
    hsx_remove_ap(&state, ap1);
    assert(count_complete(&state, ap1) == 0 && count_complete(&state, ap2) == 1);
    assert(snapshot.complete && !memcmp(snapshot.hccapx.mac_ap, ap1, 6));
    uint8_t artifact[HSX_PCAP_MAX];
    assert(hsx_build_pcap(&snapshot, NULL, NULL, artifact, sizeof(artifact)) > 24);
    hsx_reset(&state);
    assert(snapshot.complete); /* output snapshot does not alias reset state */
}

static void test_bounds_and_validation(void) {
    hsx_state_t state;
    hsx_reset(&state);
    uint8_t frame[FRAME_LEN];
    hsx_result_t result;
    make_eapol(frame, 1, ap1, sta1, 1);
    frame[1] = 1; /* AP message in STA direction */
    assert(!hsx_ingest(&state, frame, sizeof(frame), 0,
                       (const uint8_t *)"x", 1, &result));
    make_eapol(frame, 2, ap1, sta1, 1);
    frame[1] |= 0x40;
    assert(!hsx_ingest(&state, frame, sizeof(frame), 0,
                       (const uint8_t *)"x", 1, &result));
    make_eapol(frame, 2, ap1, sta1, 1);
    frame[34] = 1; frame[35] = 0; /* EAPOL length exceeds both frame and HCCAPX */
    assert(!hsx_ingest(&state, frame, sizeof(frame), 0,
                       (const uint8_t *)"x", 1, &result));
    make_eapol(frame, 2, ap1, sta1, 1);
    frame[34] = 0xff; frame[35] = 0xff; /* must not wrap the 16-bit length */
    assert(!hsx_ingest(&state, frame, sizeof(frame), 0,
                       (const uint8_t *)"x", 1, &result));
    make_eapol(frame, 1, ap1, sta1, 1);
    frame[32] = 0; /* unsupported EAPOL protocol version */
    assert(!hsx_ingest(&state, frame, sizeof(frame), 0,
                       (const uint8_t *)"x", 1, &result));
    make_eapol(frame, 1, ap1, sta1, 1);
    frame[32] = 4;
    assert(!hsx_ingest(&state, frame, sizeof(frame), 0,
                       (const uint8_t *)"x", 1, &result));
    make_eapol(frame, 1, ap1, sta1, 1);
    frame[32] = 1;
    assert(hsx_ingest(&state, frame, sizeof(frame), 0,
                      (const uint8_t *)"x", 1, &result));
    make_eapol(frame, 1, ap1, sta1, 2);
    frame[32] = 3;
    assert(hsx_ingest(&state, frame, sizeof(frame), 0,
                      (const uint8_t *)"x", 1, &result));
    make_eapol(frame, 1, ap1, sta1, 3);
    frame[36] = 254; /* WPA descriptor is supported */
    assert(hsx_ingest(&state, frame, sizeof(frame), 0,
                      (const uint8_t *)"x", 1, &result));
    make_eapol(frame, 1, ap1, sta1, 4);
    frame[36 + 94] = 1; /* key-data length disagrees with EAPOL body */
    assert(!hsx_ingest(&state, frame, sizeof(frame), 0,
                       (const uint8_t *)"x", 1, &result));
    uint8_t extended[FRAME_LEN + 1];
    make_eapol(extended, 1, ap1, sta1, 5);
    extended[35] = 96;       /* 95-byte fixed body + one key-data byte */
    extended[36 + 94] = 1;
    extended[FRAME_LEN] = 0x5a;
    assert(hsx_ingest(&state, extended, sizeof(extended), 0,
                      (const uint8_t *)"x", 1, &result));
    make_eapol(frame, 1, ap1, sta1, 0); /* M1 replay zero is valid */
    assert(hsx_ingest(&state, frame, sizeof(frame), 0,
                      (const uint8_t *)"x", 1, &result));
    make_eapol(frame, 3, ap1, sta1, 0); /* normalization would underflow */
    assert(!hsx_ingest(&state, frame, sizeof(frame), 0,
                       (const uint8_t *)"x", 1, &result));
    assert(!hsx_ingest(&state, frame, HSX_FRAME_MAX + 1U, 0,
                       (const uint8_t *)"x", 1, &result));
    make_eapol(frame, 2, ap1, sta1, 1);
    frame[36] = 1; /* unsupported key descriptor */
    assert(!hsx_ingest(&state, frame, sizeof(frame), 0,
                       (const uint8_t *)"x", 1, &result));
    make_eapol(frame, 2, ap1, sta1, 1);
    frame[38] &= (uint8_t)~0x08U; /* not a pairwise key */
    assert(!hsx_ingest(&state, frame, sizeof(frame), 0,
                       (const uint8_t *)"x", 1, &result));
    make_eapol(frame, 2, ap1, sta1, 1);
    frame[37] |= 0x0cU; /* request/error */
    assert(!hsx_ingest(&state, frame, sizeof(frame), 0,
                       (const uint8_t *)"x", 1, &result));
    make_eapol(frame, 2, ap1, sta1, 1);
    memset(frame + 36 + 77, 0, 16); /* MIC bit without a MIC */
    assert(!hsx_ingest(&state, frame, sizeof(frame), 0,
                       (const uint8_t *)"x", 1, &result));
    make_eapol(frame, 2, ap1, sta1, 1);
    memset(frame + 4, 0, 6); /* zero AP identity */
    assert(!hsx_ingest(&state, frame, sizeof(frame), 0,
                       (const uint8_t *)"x", 1, &result));
    make_eapol(frame, 2, ap1, sta1, 1);
    memset(frame + 10, 0, 6); /* zero STA identity */
    assert(!hsx_ingest(&state, frame, sizeof(frame), 0,
                       (const uint8_t *)"x", 1, &result));
    make_eapol(frame, 2, ap1, sta1, 1);
    assert(!hsx_ingest(&state, frame, sizeof(frame), 0, NULL, 1, &result));

    hsx_reset(&state);
    uint8_t key_m1[FRAME_LEN], key_m2[FRAME_LEN];
    make_eapol(key_m1, 1, ap1, sta1, 8);
    make_eapol(key_m2, 2, ap1, sta1, 8);
    key_m2[38] = (uint8_t)((key_m2[38] & ~7U) | 1U);
    assert(hsx_ingest(&state, key_m1, sizeof(key_m1), 0,
                      (const uint8_t *)"x", 1, &result));
    assert(hsx_ingest(&state, key_m2, sizeof(key_m2), 0,
                      (const uint8_t *)"x", 1, &result));
    assert(result.entry && !result.entry->complete); /* key versions must agree */

    hsx_reset(&state);
    uint8_t m1[FRAME_LEN], m2[FRAME_LEN];
    make_eapol(m1, 1, ap1, sta1, 3);
    make_eapol(m2, 2, ap1, sta1, 3);
    assert(hsx_ingest(&state, m1, sizeof(m1), 0, NULL, 0, &result));
    assert(hsx_ingest(&state, m2, sizeof(m2), 0, NULL, 0, &result));
    assert(result.entry && !result.entry->complete); /* hidden SSID is not crackable */
    hsx_entry_t *hidden = result.entry;
    hsx_set_ap_ssid(&state, ap2, (const uint8_t *)"wrong", 5);
    hsx_set_ap_ssid(&state, ap1, NULL, 4);
    assert(!hidden->complete);
    hsx_set_ap_ssid(&state, ap1, (const uint8_t *)"late", 4);
    assert(hidden->complete && hidden->hccapx.essid_len == 4);
    assert(sizeof(hsx_hccapx_t) == 393);
}

static void make_assoc(uint8_t *frame, size_t *len, bool pmkid) {
    memset(frame, 0, HSX_FRAME_MAX);
    frame[0] = 0x00;
    memcpy(frame + 4, ap1, 6);
    memcpy(frame + 10, sta1, 6);
    memcpy(frame + 16, ap1, 6);
    size_t pos = 28;
    frame[pos++] = 48;
    frame[pos++] = pmkid ? 38 : 20;
    uint8_t *rsn = frame + pos;
    rsn[0] = 1;
    rsn[2] = 0x00; rsn[3] = 0x0f; rsn[4] = 0xac; rsn[5] = 4;
    rsn[6] = 1;
    rsn[8] = 0x00; rsn[9] = 0x0f; rsn[10] = 0xac; rsn[11] = 4;
    rsn[12] = 1;
    rsn[14] = 0x00; rsn[15] = 0x0f; rsn[16] = 0xac; rsn[17] = 2;
    if (pmkid) {
        rsn[20] = 1;
        for (unsigned i = 0; i < 16; i++) rsn[22 + i] = (uint8_t)(i + 1);
    }
    pos += frame[29];
    *len = pos;
}

static void test_pmkid_context(void) {
    uint8_t assoc_data[HSX_FRAME_MAX], beacon_data[64] = {0};
    size_t assoc_len;
    make_assoc(assoc_data, &assoc_len, true);
    assert(hsx_assoc_has_pmkid(assoc_data, assoc_len));
    assoc_data[1] = 0x40;
    assert(!hsx_assoc_has_pmkid(assoc_data, assoc_len));
    assoc_data[1] = 0;
    memset(assoc_data + 30 + 22, 0, 16);
    assert(!hsx_assoc_has_pmkid(assoc_data, assoc_len));
    make_assoc(assoc_data, &assoc_len, true);
    assoc_data[30] = 2; /* unsupported RSN version */
    assert(!hsx_assoc_has_pmkid(assoc_data, assoc_len));
    assoc_data[30] = 1;
    assoc_data[29]--;
    assert(!hsx_assoc_has_pmkid(assoc_data, assoc_len - 1));
    make_assoc(assoc_data, &assoc_len, false);
    assert(!hsx_assoc_has_pmkid(assoc_data, assoc_len));
    make_assoc(assoc_data, &assoc_len, true);

    hsx_frame_t beacon = {.len = sizeof(beacon_data), .timestamp_us = 100};
    hsx_frame_t assoc = {.len = (uint16_t)assoc_len, .timestamp_us = 200};
    memcpy(beacon.data, beacon_data, sizeof(beacon_data));
    memcpy(assoc.data, assoc_data, assoc_len);
    uint8_t artifact[HSX_PCAP_MAX];
    size_t size = hsx_build_pcap(NULL, &beacon, &assoc, artifact, sizeof(artifact));
    assert(size == 24 + 16 + sizeof(beacon_data) + 16 + assoc_len);
}

int main(void) {
    test_same_exchange_rules();
    test_pair_choice_and_arrival_orders();
    test_rich_pcapng_and_m4_retention();
    test_poisoned_candidate_and_retransmission();
    test_independent_aps_and_reset();
    test_bounds_and_validation();
    test_pmkid_context();
    puts("PASS: per-exchange HS validation, independent APs, snapshots and PMKID context");
}
