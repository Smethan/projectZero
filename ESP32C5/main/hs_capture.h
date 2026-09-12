#pragma once
#include <stddef.h>
#include <stdint.h>

/* Classification only: retain unencrypted EAPOL and the management context
 * needed to interpret handshakes/RSN PMKIDs. No packet transmission. */
static int hs_capture_kind(const uint8_t *p, size_t n) {
    if (n < 24 || (p[0] & 3)) return 0;
    unsigned type = (p[0] >> 2) & 3, subtype = p[0] >> 4;
    if (type == 0) {
        unsigned fixed;
        switch (subtype) {
            case 0: fixed = 4; break;  /* association request */
            case 1: case 3: fixed = 6; break;
            case 2: fixed = 10; break; /* reassociation request */
            case 5: case 8: fixed = 12; break; /* probe response / beacon */
            default: return 0;
        }
        return n >= 24 + fixed ? 1 : 0;
    }
    if (type != 2 || (p[1] & 0x40)) return 0; /* protected data */
    size_t hdr = 24 + ((p[1] & 3) == 3 ? 6 : 0);
    if (subtype & 8) {
        if (n < hdr + 2 || (p[hdr] & 0x80)) return 0; /* A-MSDU */
        hdr += 2;
        if (p[1] & 0x80) hdr += 4; /* HT control */
    }
    static const uint8_t llc[] = {0xaa,0xaa,3,0,0,0,0x88,0x8e};
    if (n < hdr + 12) return 0;
    for (unsigned i = 0; i < sizeof(llc); i++) if (p[hdr+i] != llc[i]) return 0;
    size_t payload = ((size_t)p[hdr+10] << 8) | p[hdr+11];
    return n >= hdr + 12 + payload ? 2 : 0;
}
