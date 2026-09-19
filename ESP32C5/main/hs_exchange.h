#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HSX_EXCHANGE_LIMIT 32
#define HSX_FRAME_MAX 512
#define HSX_CAPTURE_FRAME_COUNT 7 /* beacon, auth, association and M1-M4 */
#define HSX_PCAP_MAX (24 + HSX_CAPTURE_FRAME_COUNT * (16 + HSX_FRAME_MAX))
#define HSX_PCAPNG_MAX 4096
#define HSX_ARTIFACT_MAX HSX_PCAPNG_MAX

typedef struct {
    uint16_t len;
    uint8_t channel;
    int8_t rssi;
    uint32_t timestamp_us;
    uint8_t data[HSX_FRAME_MAX];
} hsx_frame_t;

typedef struct __attribute__((packed)) {
    uint32_t signature;
    uint32_t version;
    uint8_t message_pair;
    uint8_t essid_len;
    uint8_t essid[32];
    uint8_t keyver;
    uint8_t keymic[16];
    uint8_t mac_ap[6];
    uint8_t nonce_ap[32];
    uint8_t mac_sta[6];
    uint8_t nonce_sta[32];
    uint16_t eapol_len;
    uint8_t eapol[256];
} hsx_hccapx_t;

typedef struct {
    bool used;
    bool complete;
    uint8_t bssid[6];
    uint8_t sta[6];
    uint8_t replay[8];
    uint8_t message_mask;
    uint8_t ssid_len;
    uint8_t ssid[32];
    uint64_t age;
    hsx_frame_t messages[4]; /* Retain every observed M1-M4 packet. */
    hsx_hccapx_t hccapx;
} hsx_entry_t;

typedef struct {
    uint64_t clock;
    hsx_entry_t entries[HSX_EXCHANGE_LIMIT];
} hsx_state_t;

typedef struct {
    bool accepted;
    bool new_message;
    bool became_complete;
    uint8_t message;
    uint8_t bssid[6];
    uint8_t sta[6];
    hsx_entry_t *entry;
} hsx_result_t;

void hsx_reset(hsx_state_t *state);
bool hsx_ingest(hsx_state_t *state, const uint8_t *frame, size_t len,
                uint32_t timestamp_us, const uint8_t *ssid, size_t ssid_len,
                hsx_result_t *result);
bool hsx_ingest_radio(hsx_state_t *state, const uint8_t *frame, size_t len,
                      uint32_t timestamp_us, uint8_t channel, int8_t rssi,
                      const uint8_t *ssid, size_t ssid_len,
                      hsx_result_t *result);
void hsx_set_ap_ssid(hsx_state_t *state, const uint8_t bssid[6],
                     const uint8_t *ssid, size_t ssid_len);
void hsx_remove_ap(hsx_state_t *state, const uint8_t bssid[6]);
bool hsx_assoc_has_pmkid(const uint8_t *frame, size_t len);
size_t hsx_build_pcap(const hsx_entry_t *entry, const hsx_frame_t *beacon,
                      const hsx_frame_t *association, uint8_t *out,
                      size_t capacity);
size_t hsx_build_pcapng(const hsx_entry_t *entry, const hsx_frame_t *beacon,
                        const hsx_frame_t *association, uint8_t *out,
                        size_t capacity);
size_t hsx_build_pcapng_with_context(const hsx_entry_t *entry,
                                     const hsx_frame_t *beacon,
                                     const hsx_frame_t *authentication,
                                     const hsx_frame_t *association,
                                     uint8_t *out, size_t capacity);
