/* The runner extracts the actual command handler from main.c. This harness
 * proves it resolves a fresh scan into an immutable target/channel set and
 * refuses every invalid scope before capture startup, without using RF. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "hs_targets.h"

typedef enum {
    WIFI_AUTH_OPEN = 0,
    WIFI_AUTH_WEP = 1,
    WIFI_AUTH_WPA_PSK = 2,
    WIFI_AUTH_WPA2_PSK = 3,
} wifi_auth_mode_t;

typedef struct {
    uint8_t bssid[6];
    uint8_t primary;
    wifi_auth_mode_t authmode;
} wifi_ap_record_t;

typedef struct {
    char token[33];
    uint16_t count;
    wifi_ap_record_t records[64];
} hs_scan_snapshot_t;

static bool handshake_attack_active;
static void *handshake_attack_task_handle;
static bool g_scan_in_progress;
static bool g_scan_cancel_pending;
static bool sd_card_mounted;
static bool hs_scan_pending;
static bool hs_scan_ready;
static bool g_scan_done;
static char hs_scan_token[33];
static hs_scan_snapshot_t hs_scan_snapshot;
static atomic_bool hs_scan_output_active;
static atomic_bool handshake_cleanup_active;
static int64_t hs_scan_completed_us;
static unsigned g_scan_count;
static wifi_ap_record_t g_scan_results[64];
static int dual_band_channels[64];
static int dual_band_channels_count;
static int64_t now_us;
static int start_calls;
static bool started_serial;
static hs_target_set started_targets;
static int forced_start_result;
static char reported_storage[16];
static char reported_error[32];

static int64_t esp_timer_get_time(void) { return now_us; }
static int hs_scope_error(const char *storage, const char *error) {
    snprintf(reported_storage, sizeof(reported_storage), "%s", storage);
    snprintf(reported_error, sizeof(reported_error), "%s", error);
    return 1;
}
static int start_handshake_scoped(bool serial_storage, const hs_target_set *scope) {
    start_calls++;
    started_serial = serial_storage;
    started_targets = *scope;
    return forced_start_result;
}

#include "handshake_scope_under_test.inc"

static void mac(uint8_t out[6], unsigned last) {
    const uint8_t prefix[5] = {0x02, 0x11, 0x22, 0x33, 0x44};
    memcpy(out, prefix, 5);
    out[5] = (uint8_t)last;
}

static void reset_state(void) {
    handshake_attack_active = false;
    handshake_attack_task_handle = NULL;
    g_scan_in_progress = false;
    g_scan_cancel_pending = false;
    sd_card_mounted = true;
    hs_scan_ready = true;
    hs_scan_pending = false;
    g_scan_done = true;
    snprintf(hs_scan_token, sizeof(hs_scan_token), "mutable-token");
    memset(&hs_scan_snapshot, 0, sizeof(hs_scan_snapshot));
    snprintf(hs_scan_snapshot.token, sizeof(hs_scan_snapshot.token), "scan-token");
    hs_scan_completed_us = 1000000;
    now_us = 1000000;
    g_scan_count = 2;
    memset(g_scan_results, 0, sizeof(g_scan_results));
    hs_scan_snapshot.count = 2;
    mac(hs_scan_snapshot.records[0].bssid, 0x55);
    hs_scan_snapshot.records[0].primary = 1;
    hs_scan_snapshot.records[0].authmode = WIFI_AUTH_WPA2_PSK;
    mac(hs_scan_snapshot.records[1].bssid, 0x66);
    hs_scan_snapshot.records[1].primary = 149;
    hs_scan_snapshot.records[1].authmode = WIFI_AUTH_WPA_PSK;
    atomic_store(&hs_scan_output_active, false);
    atomic_store(&handshake_cleanup_active, false);
    dual_band_channels[0] = 1;
    dual_band_channels[1] = 6;
    dual_band_channels[2] = 11;
    dual_band_channels[3] = 149;
    dual_band_channels_count = 4;
    start_calls = 0;
    forced_start_result = 0;
    memset(&started_targets, 0, sizeof(started_targets));
    reported_storage[0] = reported_error[0] = 0;
}

static int invoke(int argc, const char *const input[]) {
    char *argv[8] = {0};
    for (int i = 0; i < argc; i++) argv[i] = (char *)input[i];
    return cmd_handshake_scope(argc, argv);
}

static void assert_error(const char *expected, int argc, const char *const argv[]) {
    assert(invoke(argc, argv) == 1);
    assert(start_calls == 0);
    assert(!strcmp(reported_error, expected));
}

int main(void) {
    const char *all_serial[] = {"start_handshake_scope", "serial", "all"};
    const char *all_sd[] = {"start_handshake_scope", "sd", "all"};
    const char *selected[] = {"start_handshake_scope", "serial", "scan-token",
                              "02:11:22:33:44:66,02:11:22:33:44:55"};

    reset_state();
    assert(invoke(3, all_serial) == 0 && start_calls == 1 && started_serial);
    assert(started_targets.count == 0);

    reset_state();
    assert(invoke(3, all_sd) == 0 && start_calls == 1 && !started_serial);
    assert(started_targets.count == 0);

    reset_state();
    assert(invoke(4, selected) == 0 && start_calls == 1 && started_serial);
    assert(started_targets.count == 2);
    assert(started_targets.channel[0] == 149 && started_targets.channel[1] == 1);
    assert(!memcmp(started_targets.mac[0], hs_scan_snapshot.records[1].bssid, 6));
    assert(!memcmp(started_targets.mac[1], hs_scan_snapshot.records[0].bssid, 6));

    reset_state();
    const char *selected_sd[] = {"start_handshake_scope", "sd", "scan-token",
                                 "02:11:22:33:44:55"};
    assert(invoke(4, selected_sd) == 0 && start_calls == 1 && !started_serial);
    assert(started_targets.count == 1 && started_targets.channel[0] == 1);

    reset_state();
    const char *bad_storage[] = {"start_handshake_scope", "flash", "all"};
    assert(invoke(3, bad_storage) == 1 && start_calls == 0);

    reset_state();
    const char *all_extra[] = {"start_handshake_scope", "serial", "all", "extra"};
    assert_error("invalid_targets", 4, all_extra);

    reset_state();
    const char *missing_list[] = {"start_handshake_scope", "serial", "scan-token"};
    assert_error("invalid_targets", 3, missing_list);

    reset_state();
    handshake_attack_active = true;
    assert_error("busy", 3, all_serial);
    reset_state();
    handshake_attack_task_handle = (void *)1;
    assert_error("busy", 3, all_serial);
    reset_state();
    g_scan_in_progress = true;
    assert_error("busy", 3, all_serial);
    reset_state();
    g_scan_cancel_pending = true;
    assert_error("busy", 3, all_serial);
    reset_state();
    hs_scan_pending = true;
    assert_error("busy", 3, all_serial);
    reset_state();
    atomic_store(&hs_scan_output_active, true);
    assert_error("busy", 3, all_serial);
    reset_state();
    atomic_store(&handshake_cleanup_active, true);
    assert_error("busy", 3, all_serial);

    reset_state();
    sd_card_mounted = false;
    assert_error("sd_required", 3, all_sd);

    reset_state();
    const char *wrong_token[] = {"start_handshake_scope", "serial", "other-token",
                                 "02:11:22:33:44:55"};
    assert_error("scan_expired", 4, wrong_token);
    reset_state();
    hs_scan_ready = false;
    assert_error("scan_expired", 4, selected);
    reset_state();
    g_scan_done = false;
    assert_error("scan_expired", 4, selected);
    reset_state();
    now_us = hs_scan_completed_us + 300000001LL;
    assert_error("scan_expired", 4, selected);
    reset_state();
    now_us = hs_scan_completed_us + 300000000LL;
    assert(invoke(4, selected) == 0 && start_calls == 1); /* inclusive boundary */

    reset_state();
    const char *malformed[] = {"start_handshake_scope", "serial", "scan-token",
                               "02:11:22:33:44:zz"};
    assert_error("invalid_targets", 4, malformed);
    reset_state();
    const char *duplicate[] = {"start_handshake_scope", "serial", "scan-token",
                               "02:11:22:33:44:55,02:11:22:33:44:55"};
    assert_error("invalid_targets", 4, duplicate);

    reset_state();
    const char *unknown[] = {"start_handshake_scope", "serial", "scan-token",
                             "02:11:22:33:44:77"};
    assert_error("target_unavailable", 4, unknown);
    reset_state();
    const char *mixed_unknown[] = {"start_handshake_scope", "serial", "scan-token",
                                   "02:11:22:33:44:55,02:11:22:33:44:77"};
    assert_error("target_unavailable", 4, mixed_unknown);
    reset_state();
    hs_scan_snapshot.records[0].authmode = WIFI_AUTH_OPEN;
    const char *first[] = {"start_handshake_scope", "serial", "scan-token",
                           "02:11:22:33:44:55"};
    assert_error("target_unavailable", 4, first);
    reset_state();
    hs_scan_snapshot.records[0].authmode = WIFI_AUTH_WEP;
    assert_error("target_unavailable", 4, first);

    reset_state();
    hs_scan_snapshot.records[0].primary = 13;
    assert_error("target_channel", 4, first);

    reset_state();
    forced_start_result = 1;
    assert(invoke(4, selected) == 1 && start_calls == 1);
    assert(!strcmp(reported_storage, "serial"));
    assert(!strcmp(reported_error, "start_failed"));

    puts("PASS: production HS scope command validation, scan resolution and fail-closed startup");
}
