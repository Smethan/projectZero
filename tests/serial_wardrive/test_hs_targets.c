#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hs_targets.h"

static const uint8_t ap_a[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t ap_b[6] = {0x0a, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
static const uint8_t station[6] = {0x22, 0x33, 0x44, 0x55, 0x66, 0x77};

static void set_addr(uint8_t *frame, unsigned offset, const uint8_t mac[6]) {
    memcpy(frame + offset, mac, 6);
}

static hs_target_set selected(void) {
    hs_target_set set = {0};
    assert(hst_parse(&set, "02:11:22:33:44:55,0A:BB:CC:DD:EE:FF"));
    set.channel[0] = 1;
    set.channel[1] = 149;
    return set;
}

static void test_mac_lists(void) {
    hs_target_set set = {0};
    assert(!hst_parse(&set, NULL));
    assert(!hst_parse(&set, ""));
    assert(!hst_parse(&set, "02:11:22:33:44"));
    assert(!hst_parse(&set, "02:11:22:33:44:5g"));
    assert(!hst_parse(&set, "02-11-22-33-44-55"));
    assert(!hst_parse(&set, "02:11:22:33:44:55,"));
    assert(!hst_parse(&set, ",02:11:22:33:44:55"));
    assert(!hst_parse(&set, "02:11:22:33:44:55;0a:bb:cc:dd:ee:ff"));
    assert(!hst_parse(&set, "02:11:22:33:44:55x"));

    assert(!hst_parse(&set, "00:00:00:00:00:00"));
    assert(!hst_parse(&set, "01:11:22:33:44:55")); /* multicast */
    assert(!hst_parse(&set, "ff:ff:ff:ff:ff:ff")); /* broadcast */
    assert(!hst_parse(&set,
                      "02:11:22:33:44:55,02:11:22:33:44:55"));
    assert(!hst_parse(&set,
                      "02:11:22:33:44:55,02:11:22:33:44:55,"));

    assert(hst_parse(&set, "02:11:22:33:44:55"));
    assert(set.count == 1 && !memcmp(set.mac[0], ap_a, 6));
    assert(hst_contains(&set, ap_a));
    assert(!hst_contains(&set, ap_b));

    assert(hst_parse(&set, "02:11:22:33:44:55,0a:bb:cc:dd:ee:ff"));
    assert(set.count == 2);
    assert(!memcmp(set.mac[0], ap_a, 6));
    assert(!memcmp(set.mac[1], ap_b, 6));

    const char *sixteen =
        "02:00:00:00:00:00,02:00:00:00:00:01,02:00:00:00:00:02,"
        "02:00:00:00:00:03,02:00:00:00:00:04,02:00:00:00:00:05,"
        "02:00:00:00:00:06,02:00:00:00:00:07,02:00:00:00:00:08,"
        "02:00:00:00:00:09,02:00:00:00:00:0a,02:00:00:00:00:0b,"
        "02:00:00:00:00:0c,02:00:00:00:00:0d,02:00:00:00:00:0e,"
        "02:00:00:00:00:0f";
    char seventeen[sizeof(
        "02:00:00:00:00:00,02:00:00:00:00:01,02:00:00:00:00:02,"
        "02:00:00:00:00:03,02:00:00:00:00:04,02:00:00:00:00:05,"
        "02:00:00:00:00:06,02:00:00:00:00:07,02:00:00:00:00:08,"
        "02:00:00:00:00:09,02:00:00:00:00:0a,02:00:00:00:00:0b,"
        "02:00:00:00:00:0c,02:00:00:00:00:0d,02:00:00:00:00:0e,"
        "02:00:00:00:00:0f,02:00:00:00:00:10")];
    snprintf(seventeen, sizeof(seventeen), "%s,02:00:00:00:00:10", sixteen);
    assert(hst_parse(&set, sixteen) && set.count == HS_TARGET_LIMIT);
    assert(!hst_parse(&set, seventeen));
}

static void test_channels(void) {
    hs_target_set all = {0};
    assert(hst_channel(&all, 0));
    assert(hst_channel(&all, 1));
    assert(hst_channel(&all, 196));

    hs_target_set set = selected();
    assert(hst_channel(&set, 1));
    assert(hst_channel(&set, 149));
    assert(!hst_channel(&set, 6));
    assert(!hst_channel(&set, 0));
}

static void test_frames(void) {
    hs_target_set set = selected();
    hs_target_set all = {0};
    uint8_t frame[64] = {0};

    /* Empty means the explicitly selected all-nearby legacy behavior. */
    assert(hst_frame_allowed(&all, NULL, 0));

    assert(!hst_frame_allowed(&set, frame, 23));
    frame[0] = 1; /* unsupported 802.11 version */
    assert(!hst_frame_allowed(&set, frame, sizeof(frame)));

    memset(frame, 0, sizeof(frame));
    frame[0] = 0x80; /* beacon */
    set_addr(frame, 10, ap_a);
    set_addr(frame, 16, ap_a);
    assert(hst_frame_allowed(&set, frame, sizeof(frame)));
    set_addr(frame, 10, station); /* source and BSSID must agree for beacons */
    assert(!hst_frame_allowed(&set, frame, sizeof(frame)));
    set_addr(frame, 10, ap_a);
    set_addr(frame, 16, station);
    assert(!hst_frame_allowed(&set, frame, sizeof(frame)));

    memset(frame, 0, sizeof(frame));
    frame[0] = 0x50; /* probe response has the same source/BSSID invariant */
    set_addr(frame, 10, ap_b);
    set_addr(frame, 16, ap_b);
    assert(hst_frame_allowed(&set, frame, sizeof(frame)));
    set_addr(frame, 10, station);
    assert(!hst_frame_allowed(&set, frame, sizeof(frame)));

    memset(frame, 0, sizeof(frame));
    frame[0] = 0x00; /* other management frame: addr3 is BSSID */
    set_addr(frame, 10, station);
    set_addr(frame, 16, ap_a);
    assert(hst_frame_allowed(&set, frame, sizeof(frame)));
    set_addr(frame, 16, station);
    assert(!hst_frame_allowed(&set, frame, sizeof(frame)));

    memset(frame, 0, sizeof(frame));
    frame[0] = 0x08; /* data, neither DS: addr3 */
    set_addr(frame, 4, station);
    set_addr(frame, 10, station);
    set_addr(frame, 16, ap_a);
    assert(hst_frame_allowed(&set, frame, sizeof(frame)));
    set_addr(frame, 16, station);
    assert(!hst_frame_allowed(&set, frame, sizeof(frame)));

    memset(frame, 0, sizeof(frame));
    frame[0] = 0x08;
    frame[1] = 1; /* to DS: addr1 is the AP/BSSID */
    set_addr(frame, 4, ap_a);
    set_addr(frame, 10, station);
    set_addr(frame, 16, station);
    assert(hst_frame_allowed(&set, frame, sizeof(frame)));
    set_addr(frame, 4, station);
    assert(!hst_frame_allowed(&set, frame, sizeof(frame)));

    memset(frame, 0, sizeof(frame));
    frame[0] = 0x08;
    frame[1] = 2; /* from DS: addr2 is the AP/BSSID */
    set_addr(frame, 4, station);
    set_addr(frame, 10, ap_b);
    set_addr(frame, 16, station);
    assert(hst_frame_allowed(&set, frame, sizeof(frame)));
    set_addr(frame, 10, station);
    assert(!hst_frame_allowed(&set, frame, sizeof(frame)));

    frame[1] = 3; /* WDS has no single BSSID field */
    assert(!hst_frame_allowed(&set, frame, sizeof(frame)));

    memset(frame, 0, sizeof(frame));
    frame[0] = 0x04; /* control */
    set_addr(frame, 16, ap_a);
    assert(!hst_frame_allowed(&set, frame, sizeof(frame)));
    frame[0] = 0x0c; /* extension/reserved */
    assert(!hst_frame_allowed(&set, frame, sizeof(frame)));
}

int main(void) {
    test_mac_lists();
    test_channels();
    test_frames();
    puts("PASS: HS target parsing, count limit, channels and 802.11 BSSID filtering");
}
