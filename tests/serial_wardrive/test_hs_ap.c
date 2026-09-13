#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HS_MAX_APS 64
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
    bool beacon_captured;
    bool has_existing_file;
    int64_t last_deauth_us;
} hs_ap_target_t;

static hs_ap_target_t aps[HS_MAX_APS];
static hs_ap_target_t *hs_ap_targets = aps;
static int hs_ap_count;
static bool handshake_serial_mode;
static void *hs_exchange_state;
static unsigned name_checks, bssid_checks, ssid_updates;

static bool check_handshake_file_exists(const char *ssid) {
    (void)ssid;
    name_checks++;
    return true;
}
static bool check_handshake_file_exists_by_bssid(const uint8_t *bssid) {
    (void)bssid;
    bssid_checks++;
    return true;
}
static void hsx_set_ap_ssid(void *state, const uint8_t bssid[6],
                            const uint8_t *ssid, size_t len) {
    (void)state; (void)bssid; (void)ssid; (void)len;
    ssid_updates++;
}

#include "hs_ap_under_test.inc"

int main(void) {
    const uint8_t first[6] = {0x02, 1, 2, 3, 4, 5};
    const uint8_t second[6] = {0x02, 1, 2, 3, 4, 6};
    const uint8_t ssid[] = "network";

    handshake_serial_mode = true;
    int serial_ap = hs_add_or_update_ap(first, ssid, sizeof(ssid) - 1,
                                         1, WIFI_AUTH_WPA2_PSK, -40);
    assert(serial_ap == 0 && !hs_ap_targets[0].has_existing_file);
    assert(name_checks == 0 && bssid_checks == 0);

    /* A later update remains independent of old ESP SD contents. */
    assert(hs_add_or_update_ap(first, ssid, sizeof(ssid) - 1,
                               6, WIFI_AUTH_WPA2_PSK, -30) == serial_ap);
    assert(!hs_ap_targets[0].has_existing_file && ssid_updates == 1);

    handshake_serial_mode = false;
    int sd_ap = hs_add_or_update_ap(second, ssid, sizeof(ssid) - 1,
                                    11, WIFI_AUTH_WPA2_PSK, -50);
    assert(sd_ap == 1 && hs_ap_targets[1].has_existing_file);
    assert(name_checks == 1 && bssid_checks == 0); /* short-circuit hit */

    puts("PASS: serial HS capture ignores old ESP SD artifacts; SD mode still skips them");
}
