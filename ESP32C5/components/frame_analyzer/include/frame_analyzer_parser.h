#ifndef FRAME_ANALYZER_PARSER_H
#define FRAME_ANALYZER_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "frame_analyzer_types.h"

/* A view never owns storage. Every pointer is bounded by frame_len. */
typedef struct {
    const uint8_t *bytes;
    size_t frame_len;
    const uint8_t *mac_header;
    size_t mac_header_len;
    const uint8_t *addr1;
    const uint8_t *addr2;
    const uint8_t *addr3;
    const uint8_t *bssid;
    const uint8_t *eapol;
    size_t eapol_len;
    const uint8_t *eapol_key;
    size_t eapol_key_len;
    const uint8_t *key_data;
    size_t key_data_len;
    uint16_t key_info;
    const uint8_t *nonce;
    const uint8_t *mic;
} frame_view_t;

#define FRAME_VIEW_MAX_PMKIDS 8
typedef struct {
    uint8_t count;
    uint8_t pmkids[FRAME_VIEW_MAX_PMKIDS][16];
} pmkid_list_t;

bool frame_view_parse(const uint8_t *bytes, size_t frame_len, frame_view_t *out);
bool frame_view_bssid_matches(const frame_view_t *view, const uint8_t bssid[6]);
bool frame_view_extract_pmkids(const frame_view_t *view, pmkid_list_t *out);
uint8_t frame_view_eapol_message(const frame_view_t *view);

/* Kept for source compatibility only; bounded APIs above should be used. */
void print_raw_frame(const void *frame);
void print_mac_address(const uint8_t *address);

#endif
