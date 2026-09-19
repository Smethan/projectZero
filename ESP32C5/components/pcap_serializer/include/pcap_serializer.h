/**
 * @file pcap_serializer.h
 * @brief In-memory PCAPNG serialization of raw 802.11 frames with radiotap.
 */
#ifndef PCAP_SERIALIZER_H
#define PCAP_SERIALIZER_H

#include <stdint.h>

/* Optional nonblocking observer of frames successfully appended to PCAPNG. */
typedef void (*pcap_frame_observer_t)(const uint8_t *, unsigned);
void pcap_serializer_set_observer(pcap_frame_observer_t observer);

/** Reset the buffer and write PCAPNG section/interface blocks. */
uint8_t *pcap_serializer_init(void);

/** Append a frame when radio metadata is unavailable. */
void pcap_serializer_append_frame(const uint8_t *buffer, unsigned size,
                                  unsigned ts_usec);

/** Append an FCS-stripped frame with radiotap channel and RSSI metadata. */
void pcap_serializer_append_frame_radio(const uint8_t *buffer, unsigned size,
                                        unsigned ts_usec, uint8_t channel,
                                        int8_t rssi);

void pcap_serializer_deinit(void);
unsigned pcap_serializer_get_size(void);
uint8_t *pcap_serializer_get_buffer(void);

#endif
