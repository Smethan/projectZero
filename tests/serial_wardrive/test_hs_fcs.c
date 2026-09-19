/* Exercise the actual active-HS callback and task-owned worker with the
 * four-byte FCS layout delivered by ESP-IDF promiscuous mode. */
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "capture_pool.h"
#include "hs_capture.h"
#include "hs_targets.h"
#include "../../ESP32C5/main/hs_exchange.c"

#define HS_MAX_APS 64
#define HS_MAX_CLIENTS 128
#define HS_FRAME_POOL_COUNT 8
#define HS_CLIENT_HINT_COUNT 64
#define TAG "test"
#define MY_LOG_INFO(...) ((void)0)

typedef enum {
    WIFI_AUTH_OPEN = 0,
    WIFI_AUTH_WEP = 1,
    WIFI_AUTH_WPA_PSK = 2,
    WIFI_AUTH_WPA2_PSK = 3,
} wifi_auth_mode_t;

typedef struct {
    uint8_t bssid[6];
    char ssid[33];
    uint8_t ssid_len;
    char display_ssid[33];
    uint8_t channel;
    wifi_auth_mode_t authmode;
    int rssi;
    bool captured_m1, captured_m2, captured_m3, captured_m4;
    bool complete;
    bool completion_reported;
    bool beacon_captured;
    bool has_existing_file;
    bool partial_saved;
    bool association_has_pmkid;
    int64_t complete_at_us;
    int64_t last_deauth_us;
    hsx_frame_t beacon;
    hsx_frame_t authentication;
    hsx_frame_t association;
} hs_ap_target_t;

typedef struct {
    uint8_t mac[6];
    int hs_ap_index;
    int rssi;
    int64_t last_seen_us;
    int64_t last_deauth_us;
    bool deauthed;
} hs_client_entry_t;

typedef enum {
    HS_FRAME_AP_CONTEXT = 1,
    HS_FRAME_ASSOCIATION = 2,
    HS_FRAME_EAPOL = 3,
    HS_FRAME_AUTHENTICATION = 4,
} hs_frame_kind_t;

typedef struct {
    uint32_t timestamp_us;
    uint16_t len;
    uint8_t kind;
    uint8_t channel;
    int8_t rssi;
    uint8_t data[HSX_FRAME_MAX];
} hs_queued_frame_t;

typedef struct {
    uint8_t bssid[6];
    uint8_t sta[6];
    int8_t rssi;
} hs_client_hint_t;

typedef enum {
    HS_ARTIFACT_NONE = 0,
    HS_ARTIFACT_PARTIAL,
    HS_ARTIFACT_PMKID,
    HS_ARTIFACT_VALID,
} hs_artifact_kind_t;

static hs_ap_target_t ap_storage[HS_MAX_APS];
static hs_client_entry_t client_storage[HS_MAX_CLIENTS];
static hsx_state_t exchange_storage;
static uint8_t artifact_storage[HSX_ARTIFACT_MAX];
static hs_ap_target_t *hs_ap_targets = ap_storage;
static hs_client_entry_t *hs_clients = client_storage;
static hsx_state_t *hs_exchange_state = &exchange_storage;
static uint8_t *hs_artifact_buffer = artifact_storage;
static int hs_ap_count;
static int hs_client_count;
static volatile int hs_dwell_new_clients;
static volatile int hs_dwell_eapol_frames;
static bool handshake_serial_mode = true;
static volatile bool handshake_attack_active = true;
static hs_target_set handshake_scope;
static capture_pool_t hs_frame_pool;
static StaticQueue_t hs_hint_queue_storage;
static uint8_t hs_hint_queue_bytes[HS_CLIENT_HINT_COUNT * sizeof(hs_client_hint_t)];
static QueueHandle_t hs_hint_queue;
static atomic_bool hs_capture_accepting;
static atomic_uint hs_capture_producers;
static atomic_uint hs_capture_drops;
static uint8_t hs_seen_context[HS_MAX_APS * 3][7];
static unsigned hs_seen_context_count;
static portMUX_TYPE hs_seen_context_lock = portMUX_INITIALIZER_UNLOCKED;

static unsigned progress_count;
static unsigned progress_lengths[8];
static unsigned progress_size;
static uint8_t progress_dummy;
static uint8_t *pcap_serializer_init(void) {
    progress_size = 24;
    return &progress_dummy;
}
static void pcap_serializer_append_frame(const uint8_t *data, unsigned size,
                                         unsigned timestamp_us) {
    (void)data;
    (void)timestamp_us;
    assert(progress_count < 8);
    progress_lengths[progress_count++] = size;
    progress_size += 16 + size;
}
static void pcap_serializer_append_frame_radio(const uint8_t *data,
                                               unsigned size,
                                               unsigned timestamp_us,
                                               uint8_t channel,
                                               int8_t rssi) {
    assert(channel == 6 && rssi == -42);
    pcap_serializer_append_frame(data, size, timestamp_us);
}
static unsigned pcap_serializer_get_size(void) { return progress_size; }

static bool check_handshake_file_exists(const char *ssid) {
    (void)ssid;
    return false;
}
static bool check_handshake_file_exists_by_bssid(const uint8_t bssid[6]) {
    (void)bssid;
    return false;
}

#include "hs_fcs_under_test.inc"

static const uint8_t ap[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t sta[6] = {0x0a, 0x11, 0x22, 0x33, 0x44, 0x66};
static const uint8_t fcs[4] = {0xde, 0xad, 0xbe, 0xef};

static size_t make_beacon(uint8_t *frame) {
    memset(frame, 0, HSX_FRAME_MAX);
    frame[0] = 0x80;
    memset(frame + 4, 0xff, 6);
    memcpy(frame + 10, ap, 6);
    memcpy(frame + 16, ap, 6);
    frame[34] = 0x10;
    size_t pos = 36;
    frame[pos++] = 0;
    frame[pos++] = 7;
    memcpy(frame + pos, "network", 7);
    pos += 7;
    frame[pos++] = 3;
    frame[pos++] = 1;
    frame[pos++] = 6;
    frame[pos++] = 48;
    frame[pos++] = 2;
    frame[pos++] = 1;
    frame[pos++] = 0;
    return pos;
}

static size_t make_authentication(uint8_t *frame) {
    memset(frame, 0, HSX_FRAME_MAX);
    frame[0] = 0xb0;
    memcpy(frame + 4, ap, 6);
    memcpy(frame + 10, sta, 6);
    memcpy(frame + 16, ap, 6);
    frame[26] = 1; /* authentication transaction 1 */
    return 30;
}

#define EAPOL_FRAME_LEN 131
static void make_eapol(uint8_t *frame, unsigned message, unsigned replay) {
    memset(frame, 0, HSX_FRAME_MAX);
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
    static const uint8_t llc[] = {0xaa, 0xaa, 3, 0, 0, 0, 0x88, 0x8e};
    memcpy(frame + 24, llc, sizeof(llc));
    uint8_t *eapol = frame + 32;
    eapol[0] = 2;
    eapol[1] = 3;
    eapol[3] = 95;
    uint8_t *key = eapol + 4;
    key[0] = 2;
    uint16_t info = message == 1 ? 0x008aU : message == 2 ? 0x010aU :
                    message == 3 ? 0x01caU : 0x030aU;
    key[1] = (uint8_t)(info >> 8);
    key[2] = (uint8_t)info;
    key[12] = (uint8_t)replay;
    if (message != 4)
        for (unsigned i = 0; i < 32; i++) key[13 + i] = (uint8_t)(message * 16 + i + 1);
    if (message != 1)
        for (unsigned i = 0; i < 16; i++) key[77 + i] = (uint8_t)(0xa0 + i);
}

static void offer_with_fcs(const uint8_t *frame, size_t frame_len,
                           wifi_promiscuous_pkt_type_t type, uint32_t timestamp) {
    wifi_promiscuous_pkt_t packet = {0};
    assert(frame_len + sizeof(fcs) <= sizeof(packet.payload));
    packet.rx_ctrl.sig_len = (int)(frame_len + sizeof(fcs));
    packet.rx_ctrl.channel = 6;
    packet.rx_ctrl.rssi = -42;
    packet.rx_ctrl.timestamp = timestamp;
    memcpy(packet.payload, frame, frame_len);
    memcpy(packet.payload + frame_len, fcs, sizeof(fcs));
    hs_sniffer_promiscuous_cb(&packet, type);
}

static uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int main(void) {
    hsx_reset(hs_exchange_state);
    memcpy(handshake_scope.mac[0], ap, 6);
    handshake_scope.channel[0] = 6;
    handshake_scope.count = 1;
    assert(pcap_serializer_init());
    assert(hs_capture_open());
    atomic_store(&hs_capture_accepting, true);

    wifi_promiscuous_pkt_t too_short = {.rx_ctrl = {.sig_len = 3}};
    hs_sniffer_promiscuous_cb(&too_short, WIFI_PKT_MGMT);
    assert(!uxQueueMessagesWaiting(hs_frame_pool.ready));

    uint8_t frame[HSX_FRAME_MAX];
    size_t beacon_len = make_beacon(frame);
    offer_with_fcs(frame, beacon_len, WIFI_PKT_MGMT, 100);
    assert(uxQueueMessagesWaiting(hs_frame_pool.ready) == 1);
    assert(hs_capture_drain() == 1);
    assert(hs_ap_count == 1 && hs_ap_targets[0].beacon_captured);
    assert(hs_ap_targets[0].beacon.len == beacon_len);
    assert(!memcmp(hs_ap_targets[0].beacon.data, frame, beacon_len));
    assert(progress_count == 1 && progress_lengths[0] == beacon_len);

    size_t authentication_len = make_authentication(frame);
    offer_with_fcs(frame, authentication_len, WIFI_PKT_MGMT, 150);
    assert(hs_capture_drain() == 1);
    assert(hs_ap_targets[0].authentication.len == authentication_len);
    assert(!memcmp(hs_ap_targets[0].authentication.data, frame,
                   authentication_len));

    make_eapol(frame, 1, 70);
    offer_with_fcs(frame, EAPOL_FRAME_LEN, WIFI_PKT_DATA, 200);
    assert(hs_capture_drain() == 1);
    make_eapol(frame, 2, 70);
    offer_with_fcs(frame, EAPOL_FRAME_LEN, WIFI_PKT_DATA, 300);
    assert(hs_capture_drain() == 1);
    assert(hs_ap_targets[0].complete);
    assert(progress_count == 3);
    assert(progress_lengths[1] == EAPOL_FRAME_LEN);
    assert(progress_lengths[2] == EAPOL_FRAME_LEN);

    hsx_entry_t *exchange = NULL;
    size_t pcapng_size = 0;
    assert(hs_build_ap_artifact(0, true, &exchange, &pcapng_size) ==
           HS_ARTIFACT_VALID);
    assert(exchange && exchange->hccapx.message_pair == 0);
    assert(get_le32(artifact_storage) == 0x0a0d0d0aU);
    assert(get_le32(artifact_storage + 28) == 1U);
    assert(artifact_storage[36] == 127 && artifact_storage[37] == 0);

    size_t pos = 60;
    const size_t expected_lengths[] = {beacon_len, authentication_len,
                                       EAPOL_FRAME_LEN, EAPOL_FRAME_LEN};
    for (unsigned i = 0; i < 4; i++) {
        assert(get_le32(artifact_storage + pos) == 6U);
        uint32_t block_len = get_le32(artifact_storage + pos + 4);
        uint32_t packet_len = get_le32(artifact_storage + pos + 20);
        assert(packet_len == 15 + expected_lengths[i]);
        assert(get_le32(artifact_storage + pos + block_len - 4) == block_len);
        const uint8_t *radiotap = artifact_storage + pos + 28;
        assert(radiotap[2] == 15 && radiotap[3] == 0);
        assert(radiotap[8] == 0); /* FCS was stripped before serialization. */
        assert(radiotap[14] == (uint8_t)-42);
        assert(radiotap[10] == 0x85 && radiotap[11] == 0x09); /* 2437 MHz */
        const uint8_t *saved = radiotap + 15;
        assert(expected_lengths[i] >= 4 &&
               memcmp(saved + expected_lengths[i] - 4, fcs, 4));
        pos += block_len;
    }
    assert(pos == pcapng_size);

    hs_capture_close();
    assert(!heap_live && !hs_frame_pool.frames && !hs_hint_queue);
    puts("PASS: active HS FCS stripping and PCAPNG radiotap records");
}
