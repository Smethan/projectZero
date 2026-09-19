#include <assert.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hs_exchange.h"

typedef enum {
    HS_ARTIFACT_NONE = 0,
    HS_ARTIFACT_PARTIAL,
    HS_ARTIFACT_PMKID,
    HS_ARTIFACT_VALID,
} hs_artifact_kind_t;

typedef struct {
    uint8_t bssid[6];
    char ssid[33];
    uint8_t ssid_len;
} hs_ap_target_t;

static hs_ap_target_t hs_ap_targets[1];
static uint8_t artifact[96];
static uint8_t *hs_artifact_buffer = artifact;
static hsx_entry_t entry;
static hs_artifact_kind_t next_kind;
static size_t next_pcapng_size;

static char wire[8192];
static size_t wire_len;
static unsigned lock_depth, begin_calls, end_calls, write_calls;
static unsigned fail_write_call;
static bool fail_begin;

static hs_artifact_kind_t hs_build_ap_artifact(int ap_idx, bool allow_partial,
                                                hsx_entry_t **exchange,
                                                size_t *pcapng_size) {
    assert(ap_idx == 0 && allow_partial);
    *exchange = next_kind == HS_ARTIFACT_VALID ? &entry : NULL;
    *pcapng_size = next_pcapng_size;
    memset(artifact, 0x5a, next_pcapng_size);
    return next_kind;
}

static bool serial_output_begin(unsigned timeout_ms) {
    assert(timeout_ms == 1000 && lock_depth == 0);
    begin_calls++;
    if (fail_begin) return false;
    lock_depth = 1;
    return true;
}

static bool serial_output_locked(const char *data, unsigned length,
                                 unsigned timeout_ms) {
    assert(timeout_ms == 1000 && lock_depth == 1);
    write_calls++;
    if (write_calls == fail_write_call) return false;
    assert(wire_len + length < sizeof(wire));
    memcpy(wire + wire_len, data, length);
    wire_len += length;
    wire[wire_len] = 0;
    return true;
}

static void serial_output_end(void) {
    assert(lock_depth == 1);
    lock_depth = 0;
    end_calls++;
}

static int mbedtls_base64_encode(unsigned char *dest, size_t capacity,
                                 size_t *written, const unsigned char *src,
                                 size_t length) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = ((length + 2) / 3) * 4;
    if (!dest || capacity < need) {
        *written = need;
        return -1;
    }
    size_t out = 0;
    for (size_t i = 0; i < length; i += 3) {
        uint32_t value = (uint32_t)src[i] << 16;
        if (i + 1 < length) value |= (uint32_t)src[i + 1] << 8;
        if (i + 2 < length) value |= src[i + 2];
        dest[out++] = alphabet[value >> 18];
        dest[out++] = alphabet[(value >> 12) & 63];
        dest[out++] = i + 1 < length ? alphabet[(value >> 6) & 63] : '=';
        dest[out++] = i + 2 < length ? alphabet[value & 63] : '=';
    }
    *written = out;
    return 0;
}

#include "hs_artifact_under_test.inc"

static void reset(hs_artifact_kind_t kind) {
    memset(&hs_ap_targets, 0, sizeof(hs_ap_targets));
    const uint8_t hostile[] = {'E','v','i','l','\n',0,'A','P',':','X'};
    memcpy(hs_ap_targets[0].ssid, hostile, sizeof(hostile));
    hs_ap_targets[0].ssid_len = sizeof(hostile);
    const uint8_t bssid[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    memcpy(hs_ap_targets[0].bssid, bssid, 6);
    memset(&entry, 0, sizeof(entry));
    memset(&entry.hccapx, 0x33, sizeof(entry.hccapx));
    next_kind = kind;
    next_pcapng_size = 80;
    wire_len = lock_depth = begin_calls = end_calls = write_calls = 0;
    fail_write_call = 0;
    fail_begin = false;
    wire[0] = 0;
}

static void assert_order(const char *first, const char *second) {
    const char *a = strstr(wire, first), *b = strstr(wire, second);
    assert(a && b && a < b);
}

int main(void) {
    char safe[33];
    const uint8_t hostile[] = {'A','\r','\n',0,'A','P',':','Z'};
    hs_sanitize_ssid(safe, hostile, sizeof(hostile), sizeof(safe));
    assert(!strcmp(safe, "A___AP_Z"));

    reset(HS_ARTIFACT_VALID);
    assert(hs_dump_ap_serial(0));
    assert(lock_depth == 0 && begin_calls == 1 && end_calls == 1);
    assert_order("CAPTURE_KIND: VALID", "CAPTURE_FORMAT: PCAPNG");
    assert_order("CAPTURE_FORMAT: PCAPNG", "--- PCAPNG BEGIN ---");
    assert_order("--- PCAPNG END ---", "PCAPNG_SIZE: 80");
    assert_order("PCAPNG_SIZE: 80", "--- HCCAPX BEGIN ---");
    assert(!strstr(wire, "--- PCAP BEGIN ---"));
    assert(!strstr(wire, "PCAP_SIZE:"));
    assert_order("--- HCCAPX END ---", "SSID: Evil__AP_X  AP: 02:11:22:33:44:55");
    assert(strstr(wire, "SSID: Evil__AP_X  AP: 02:11:22:33:44:55\n") +
           strlen("SSID: Evil__AP_X  AP: 02:11:22:33:44:55\n") == wire + wire_len);

    reset(HS_ARTIFACT_PMKID);
    assert(hs_dump_ap_serial(0));
    assert(strstr(wire, "CAPTURE_KIND: PMKID"));
    assert(!strstr(wire, "HCCAPX"));
    assert(strstr(wire, "SSID: Evil__AP_X"));

    reset(HS_ARTIFACT_PARTIAL);
    assert(hs_dump_ap_serial(0));
    assert(strstr(wire, "CAPTURE_KIND: PARTIAL"));
    assert(!strstr(wire, "HCCAPX"));

    reset(HS_ARTIFACT_VALID);
    fail_write_call = 3;
    assert(!hs_dump_ap_serial(0));
    assert(lock_depth == 0 && begin_calls == 1 && end_calls == 1);
    assert(!strstr(wire, "SSID:")); /* no commit after a partial transfer */

    reset(HS_ARTIFACT_VALID);
    fail_begin = true;
    assert(!hs_dump_ap_serial(0));
    assert(lock_depth == 0 && begin_calls == 1 && end_calls == 0 && !wire_len);

    puts("PASS: atomic HS artifact framing, transport failure, kind ordering and hostile SSIDs");
}
