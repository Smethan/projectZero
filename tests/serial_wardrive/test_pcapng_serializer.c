#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#include "../../ESP32C5/components/pcap_serializer/pcap_serializer.c"

static unsigned observed;
static void observe(const uint8_t *data, unsigned size) {
    assert(size == 5 && data[0] == 0x80);
    observed++;
}

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int main(void) {
    uint8_t frame[] = {0x80, 1, 2, 3, 4};
    uint8_t *data = pcap_serializer_init();
    assert(data && pcap_serializer_get_size() == 60);
    assert(le32(data) == 0x0a0d0d0aU && le32(data + 4) == 28);
    assert(le32(data + 28) == 1 && le16(data + 36) == 127);
    assert(data[48] == 6);

    pcap_serializer_set_observer(observe);
    pcap_serializer_append_frame_radio(frame, sizeof(frame), 1234567, 6, -42);
    assert(observed == 1);
    data = pcap_serializer_get_buffer();
    assert(le32(data + 60) == 6);
    uint32_t block_len = le32(data + 64);
    assert(block_len == 52 && pcap_serializer_get_size() == 112);
    assert(le32(data + 76) == 1234567);
    assert(le32(data + 80) == 20 && le32(data + 84) == 20);
    const uint8_t *rt = data + 88;
    assert(le16(rt + 2) == 15 && le32(rt + 4) == 0x2a);
    assert(rt[8] == 0 && le16(rt + 10) == 2437 && (int8_t)rt[14] == -42);
    assert(!memcmp(rt + 15, frame, sizeof(frame)));
    assert(le32(data + 60 + block_len - 4) == block_len);

    pcap_serializer_deinit();
    assert(!pcap_serializer_get_buffer() && !pcap_serializer_get_size());
    puts("PASS: reusable PCAPNG serializer header, radiotap and EPB framing");
}
