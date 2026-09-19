/**
 * @file pcap_serializer.c
 * @brief In-memory PCAPNG serializer for raw 802.11 captures.
 */
#include "pcap_serializer.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include "esp_log.h"

#define PCAPNG_SECTION_HEADER_BLOCK 0x0a0d0d0aU
#define PCAPNG_INTERFACE_DESCRIPTION_BLOCK 1U
#define PCAPNG_ENHANCED_PACKET_BLOCK 6U
#define PCAPNG_BYTE_ORDER_MAGIC 0x1a2b3c4dU
#define PCAPNG_LINKTYPE_IEEE802_11_RADIOTAP 127U
#define PCAPNG_SNAPLEN 65535U
#define PCAPNG_RADIOTAP_LEN 15U

static const char *TAG = "pcap_serializer";
static unsigned pcapng_size;
static uint8_t *pcapng_buffer;
static _Atomic(pcap_frame_observer_t) frame_observer;

static void put_le16(uint8_t *dest, uint16_t value) {
    dest[0] = (uint8_t)value;
    dest[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *dest, uint32_t value) {
    dest[0] = (uint8_t)value;
    dest[1] = (uint8_t)(value >> 8);
    dest[2] = (uint8_t)(value >> 16);
    dest[3] = (uint8_t)(value >> 24);
}

static uint16_t channel_frequency(uint8_t channel) {
    if (channel >= 1 && channel <= 13) return (uint16_t)(2407 + 5 * channel);
    if (channel == 14) return 2484;
    if (channel >= 15) return (uint16_t)(5000 + 5 * channel);
    return 0;
}

static uint16_t channel_flags(uint8_t channel) {
    if (!channel) return 0;
    return channel <= 14 ? 0x00c0U : 0x0140U;
}

static unsigned align4(unsigned value) {
    return (value + 3U) & ~3U;
}

void pcap_serializer_set_observer(pcap_frame_observer_t observer) {
    atomic_store(&frame_observer, observer);
}

uint8_t *pcap_serializer_init(void) {
    free(pcapng_buffer);
    pcapng_buffer = calloc(1, 60);
    pcapng_size = pcapng_buffer ? 60U : 0U;
    if (!pcapng_buffer) return NULL;

    /* Section Header Block, little-endian, unknown section length. */
    put_le32(pcapng_buffer, PCAPNG_SECTION_HEADER_BLOCK);
    put_le32(pcapng_buffer + 4, 28);
    put_le32(pcapng_buffer + 8, PCAPNG_BYTE_ORDER_MAGIC);
    put_le16(pcapng_buffer + 12, 1);
    put_le16(pcapng_buffer + 14, 0);
    memset(pcapng_buffer + 16, 0xff, 8);
    put_le32(pcapng_buffer + 24, 28);

    /* One radiotap interface with explicit microsecond timestamp resolution. */
    uint8_t *idb = pcapng_buffer + 28;
    put_le32(idb, PCAPNG_INTERFACE_DESCRIPTION_BLOCK);
    put_le32(idb + 4, 32);
    put_le16(idb + 8, PCAPNG_LINKTYPE_IEEE802_11_RADIOTAP);
    put_le32(idb + 12, PCAPNG_SNAPLEN);
    put_le16(idb + 16, 9); /* if_tsresol */
    put_le16(idb + 18, 1);
    idb[20] = 6;           /* 10^-6 seconds */
    put_le16(idb + 24, 0); /* end of options */
    put_le16(idb + 26, 0);
    put_le32(idb + 28, 32);
    return pcapng_buffer;
}

void pcap_serializer_append_frame_radio(const uint8_t *buffer, unsigned size,
                                        unsigned ts_usec, uint8_t channel,
                                        int8_t rssi) {
    if (!pcapng_buffer || !buffer || !size) {
        ESP_LOGD(TAG, "Frame or PCAPNG buffer unavailable; skipping frame.");
        return;
    }
    if (size > PCAPNG_SNAPLEN - PCAPNG_RADIOTAP_LEN)
        size = PCAPNG_SNAPLEN - PCAPNG_RADIOTAP_LEN;

    const unsigned packet_size = PCAPNG_RADIOTAP_LEN + size;
    const unsigned block_size = 32U + align4(packet_size);
    uint8_t *grown = realloc(pcapng_buffer, pcapng_size + block_size);
    if (!grown) {
        ESP_LOGE(TAG, "Error growing PCAPNG buffer; capture may be incomplete.");
        return;
    }
    pcapng_buffer = grown;
    uint8_t *block = pcapng_buffer + pcapng_size;
    memset(block, 0, block_size);
    put_le32(block, PCAPNG_ENHANCED_PACKET_BLOCK);
    put_le32(block + 4, block_size);
    put_le32(block + 8, 0);  /* interface id */
    put_le32(block + 12, 0); /* timestamp high */
    put_le32(block + 16, ts_usec);
    put_le32(block + 20, packet_size);
    put_le32(block + 24, packet_size);

    uint8_t *radiotap = block + 28;
    put_le16(radiotap + 2, PCAPNG_RADIOTAP_LEN);
    put_le32(radiotap + 4, 0x2aU); /* flags, channel, dBm antenna signal */
    radiotap[8] = 0;              /* capture has already stripped the FCS */
    put_le16(radiotap + 10, channel_frequency(channel));
    put_le16(radiotap + 12, channel_flags(channel));
    radiotap[14] = (uint8_t)rssi;
    memcpy(radiotap + PCAPNG_RADIOTAP_LEN, buffer, size);
    put_le32(block + block_size - 4, block_size);
    pcapng_size += block_size;

    pcap_frame_observer_t observer = atomic_load(&frame_observer);
    if (observer) observer(buffer, size);
}

void pcap_serializer_append_frame(const uint8_t *buffer, unsigned size,
                                  unsigned ts_usec) {
    pcap_serializer_append_frame_radio(buffer, size, ts_usec, 0, 0);
}

void pcap_serializer_deinit(void) {
    free(pcapng_buffer);
    pcapng_buffer = NULL;
    pcapng_size = 0;
}

unsigned pcap_serializer_get_size(void) {
    return pcapng_size;
}

uint8_t *pcap_serializer_get_buffer(void) {
    return pcapng_buffer;
}
