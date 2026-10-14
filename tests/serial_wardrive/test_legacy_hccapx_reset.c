#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#include "frame_analyzer_parser.h"
#include "hccapx_serializer.h"
#include "../../ESP32C5/components/hccapx_serializer/hccapx_serializer.c"

static void be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static size_t key_frame(uint8_t *out, uint16_t info, bool from_ap, uint8_t replay,
                        bool has_nonce) {
    memset(out, 0, 160); out[0] = 0x08; out[1] = from_ap ? 0x02 : 0x01;
    const uint8_t ap[6] = {2,1,2,3,4,5}, sta[6] = {2,6,7,8,9,10};
    memcpy(out + 4, from_ap ? sta : ap, 6);
    memcpy(out + 10, from_ap ? ap : sta, 6);
    memset(out + 16, 0x77, 6);
    const uint8_t llc[] = {0xaa,0xaa,0x03,0,0,0,0x88,0x8e};
    memcpy(out + 24, llc, sizeof(llc));
    size_t p = 32; out[p] = 2; out[p + 1] = 3; be16(out + p + 2, 95); p += 4;
    out[p] = 2; be16(out + p + 1, info); p += 3; p += 2;
    out[p + 7] = replay; p += 8;
    if (has_nonce) memset(out + p, replay + 1, 32);
    p += 32 + 16 + 8 + 8 + 16; be16(out + p, 0); p += 2;
    return p;
}

int main(void) {
    uint8_t raw[160], ssid[] = {'t','e','s','t'};
    frame_view_t view;
    hccapx_serializer_init(ssid, sizeof(ssid));
    assert(!hccapx_serializer_get());
    size_t n = key_frame(raw, 0x010a, false, 1, true); /* STA M2 replay 1 */
    assert(frame_view_parse(raw, n, &view) && view.eapol_key);
    hccapx_serializer_add_frame(&view);
    assert(!hccapx_serializer_get());
    n = key_frame(raw, 0x01ca, true, 2, true); /* AP M3 replay 2; no M1 observed */
    assert(frame_view_parse(raw, n, &view) && view.eapol_key);
    hccapx_serializer_add_frame(&view);
    hccapx_t *result = hccapx_serializer_get();
    assert(result && result->message_pair == 2);
    assert(result->nonce_ap[0] == 3); /* M3 supplies the missing ANonce. */
    assert(result->nonce_sta[0] == 2 && result->essid_len == sizeof(ssid));

    hccapx_serializer_init(ssid, sizeof(ssid));
    assert(!hccapx_serializer_get());
    n = key_frame(raw, 0x01ca, true, 2, true); /* M3 can precede M2 */
    assert(frame_view_parse(raw, n, &view)); hccapx_serializer_add_frame(&view);
    assert(!hccapx_serializer_get());
    n = key_frame(raw, 0x010a, false, 1, true);
    assert(frame_view_parse(raw, n, &view)); hccapx_serializer_add_frame(&view);
    result = hccapx_serializer_get();
    assert(result && result->message_pair == 3 && result->nonce_ap[0] == 3);

    hccapx_serializer_init(ssid, sizeof(ssid));
    assert(!hccapx_serializer_get());
    n = key_frame(raw, 0x01ca, true, 2, true); /* stale state must not complete alone */
    assert(frame_view_parse(raw, n, &view)); hccapx_serializer_add_frame(&view);
    assert(!hccapx_serializer_get());
    hccapx_serializer_init(ssid, 33);
    assert(!hccapx_serializer_get());
    puts("PASS: complete HCCAPX session reset and M3 ANonce fallback");
}
