#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "frame_analyzer_parser.h"

static void be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static size_t make_key_frame(uint8_t *out, uint16_t key_info, const uint8_t replay[8],
                             bool ap_to_sta, bool nonce, const uint8_t *key_data,
                             uint16_t key_data_len) {
    memset(out, 0, 256);
    out[0] = 0x08; out[1] = ap_to_sta ? 0x02 : 0x01;
    const uint8_t ap[6] = {2,1,2,3,4,5}, sta[6] = {2,6,7,8,9,10};
    memcpy(out + 4, ap_to_sta ? sta : ap, 6);
    memcpy(out + 10, ap_to_sta ? ap : sta, 6);
    memset(out + 16, 0x77, 6);
    size_t p = 24;
    const uint8_t llc[] = {0xaa,0xaa,0x03,0,0,0,0x88,0x8e};
    memcpy(out + p, llc, sizeof(llc)); p += sizeof(llc);
    out[p] = 2; out[p + 1] = 3; be16(out + p + 2, 95 + key_data_len); p += 4;
    out[p] = 2; be16(out + p + 1, key_info); p += 3;
    p += 2; memcpy(out + p, replay, 8); p += 8;
    if (nonce) memset(out + p, 0x44, 32);
    p += 32 + 16 + 8 + 8 + 16;
    be16(out + p, key_data_len); p += 2;
    if (key_data_len) memcpy(out + p, key_data, key_data_len);
    return p + key_data_len;
}

int main(void) {
    uint8_t frame[512], replay[8] = {0};
    size_t n = make_key_frame(frame, 0x008a, replay, true, true, NULL, 0);
    frame_view_t view;
    for (size_t i = 0; i < n; ++i) {
        frame_view_t truncated;
        if (frame_view_parse(frame, i, &truncated)) assert(!truncated.eapol_key);
    }
    assert(frame_view_parse(frame, n, &view));
    assert(view.eapol_key && view.eapol_key_len == 95);
    assert(frame_view_eapol_message(&view) == 1);
    const uint8_t ap[] = {2,1,2,3,4,5};
    assert(frame_view_bssid_matches(&view, ap));
    frame[1] |= 0x40;
    assert(frame_view_parse(frame, n, &view) && !view.eapol_key);
    frame[1] &= (uint8_t)~0x40;

    uint8_t kde[8 * 22 + 1];
    for (unsigned i = 0; i < 8; ++i) {
        uint8_t *e = kde + i * 22;
        e[0] = 0xdd; e[1] = 20; e[2] = 0; e[3] = 0x0f; e[4] = 0xac; e[5] = 4;
        memset(e + 6, (int)(i + 1), 16);
    }
    n = make_key_frame(frame, 0x010a, replay, false, true, kde, 8 * 22);
    assert(frame_view_parse(frame, n, &view));
    pmkid_list_t list;
    assert(frame_view_extract_pmkids(&view, &list) && list.count == 8);
    assert(list.pmkids[7][0] == 8);

    /* Truncated or malformed KDEs reject the complete result, never read past it. */
    n = make_key_frame(frame, 0x010a, replay, false, true, kde, 8 * 22 - 1);
    assert(frame_view_parse(frame, n, &view));
    assert(!frame_view_extract_pmkids(&view, &list) && list.count == 0);
    n = make_key_frame(frame, 0x110a, replay, false, true, kde, 22);
    assert(frame_view_parse(frame, n, &view));
    assert(!frame_view_extract_pmkids(&view, &list));

    n = make_key_frame(frame, 0x010a, replay, false, true, NULL, 0);
    assert(frame_view_parse(frame, n, &view) && frame_view_eapol_message(&view) == 2);
    n = make_key_frame(frame, 0x030a, replay, false, false, NULL, 0);
    assert(frame_view_parse(frame, n, &view) && frame_view_eapol_message(&view) == 4);

    /* Deterministic fuzz smoke: arbitrary bytes and all short lengths stay bounded. */
    uint32_t rng = 0x6d2b79f5U;
    for (unsigned trial = 0; trial < 10000; ++trial) {
        uint8_t fuzz[256];
        for (size_t i = 0; i < sizeof(fuzz); ++i) {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            fuzz[i] = (uint8_t)rng;
        }
        size_t len = trial % sizeof(fuzz);
        frame_view_t fuzz_view;
        if (frame_view_parse(fuzz, len, &fuzz_view)) {
            pmkid_list_t fuzz_pmks;
            (void)frame_view_extract_pmkids(&fuzz_view, &fuzz_pmks);
            (void)frame_view_eapol_message(&fuzz_view);
        }
    }
    puts("PASS: bounded legacy frame, EAPOL, and repeated PMKID parsing");
}
