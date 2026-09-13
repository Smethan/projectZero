/* The runner extracts hs_scan_event(), hs_scan_complete() and cmd_hs_scan()
 * directly from main.c. These tests model serial loss and command races without
 * starting Wi-Fi or transmitting any RF frames. */
#include <assert.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define TEST_AP_LIMIT 64
#define OUTPUT_CAPACITY 32768

typedef enum {
    WIFI_AUTH_OPEN = 0,
    WIFI_AUTH_WEP = 1,
    WIFI_AUTH_WPA_PSK = 2,
    WIFI_AUTH_WPA2_PSK = 3,
} wifi_auth_mode_t;

typedef struct {
    uint8_t bssid[6];
    uint8_t ssid[33];
    uint8_t primary;
    int8_t rssi;
    wifi_auth_mode_t authmode;
} wifi_ap_record_t;

typedef struct {
    char token[33];
    uint16_t count;
    wifi_ap_record_t records[TEST_AP_LIMIT];
} hs_scan_snapshot_t;

static uint16_t g_scan_count;
static wifi_ap_record_t g_scan_results[TEST_AP_LIMIT];
static bool hs_scan_pending;
static bool hs_scan_ready;
static char hs_scan_token[33];
static hs_scan_snapshot_t hs_scan_snapshot;
static atomic_bool hs_scan_output_active;
static atomic_bool hs_scan_start_authorized;
static atomic_bool handshake_cleanup_active;
static int64_t hs_scan_completed_us;
static bool g_scan_in_progress;
static bool g_scan_cancel_pending;
static bool g_scan_teardown_in_progress;
static bool operation_stop_requested;
static bool handshake_attack_active;
static void *handshake_attack_task_handle;

static int64_t now_us;
static unsigned serial_calls;
static unsigned serial_fail_call;
static unsigned mutate_token_call;
static int64_t serial_advance_us;
static size_t attempted_bytes;
static size_t output_bytes;
static size_t largest_record;
static char output[OUTPUT_CAPACITY];
static bool done_was_attempted;
static bool done_saw_ready;
static bool done_saw_pending;
static bool done_saw_in_progress;
static bool done_saw_output_owner;
static bool ap_saw_output_owner;
static bool ap_saw_in_progress;
static unsigned scan_command_calls;
static int scan_command_result;

static int64_t esp_timer_get_time(void) { return now_us; }

static bool serial_output(const char *data, unsigned len, unsigned timeout_ms) {
    (void)timeout_ms;
    char record[512];
    assert(len < sizeof(record));
    memcpy(record, data, len);
    record[len] = 0;
    serial_calls++;
    attempted_bytes += len;
    if (len > largest_record) largest_record = len;
    if (strstr(record, "\"kind\":\"scan_done\"")) {
        done_was_attempted = true;
        done_saw_ready = hs_scan_ready;
        done_saw_pending = hs_scan_pending;
        done_saw_in_progress = g_scan_in_progress;
        done_saw_output_owner = atomic_load(&hs_scan_output_active);
    } else if (strstr(record, "\"kind\":\"ap\"")) {
        ap_saw_output_owner = atomic_load(&hs_scan_output_active);
        ap_saw_in_progress = g_scan_in_progress;
    }
    if (mutate_token_call == serial_calls) {
        snprintf(hs_scan_token, sizeof(hs_scan_token), "mutated-token");
    }
    now_us += serial_advance_us;
    if (serial_fail_call == serial_calls) return false;
    assert(output_bytes + len < sizeof(output));
    memcpy(output + output_bytes, data, len);
    output_bytes += len;
    output[output_bytes] = 0;
    return true;
}

static int cmd_scan_networks(int argc, char **argv) {
    (void)argc;
    (void)argv;
    scan_command_calls++;
    return scan_command_result;
}

#include "hs_scan_under_test.inc"

static void fill_ap(unsigned index) {
    wifi_ap_record_t *ap = &g_scan_results[index];
    memset(ap, 0, sizeof(*ap));
    ap->bssid[0] = 0x02;
    ap->bssid[1] = 0x11;
    ap->bssid[2] = 0x22;
    ap->bssid[3] = 0x33;
    ap->bssid[4] = (uint8_t)(index >> 8);
    ap->bssid[5] = (uint8_t)index;
    snprintf((char *)ap->ssid, sizeof(ap->ssid), "network-%02u", index);
    ap->primary = index % 2 ? 149 : 1;
    ap->rssi = (int8_t)(-30 - (int)(index % 60));
    ap->authmode = WIFI_AUTH_WPA2_PSK;
}

static void reset_state(void) {
    g_scan_count = 2;
    memset(g_scan_results, 0, sizeof(g_scan_results));
    for (unsigned i = 0; i < TEST_AP_LIMIT; i++) fill_ap(i);
    hs_scan_pending = false;
    hs_scan_ready = false;
    snprintf(hs_scan_token, sizeof(hs_scan_token), "active-token");
    memset(&hs_scan_snapshot, 0, sizeof(hs_scan_snapshot));
    hs_scan_completed_us = 0;
    g_scan_in_progress = false;
    g_scan_cancel_pending = false;
    g_scan_teardown_in_progress = false;
    operation_stop_requested = false;
    handshake_attack_active = false;
    handshake_attack_task_handle = NULL;
    atomic_store(&hs_scan_output_active, false);
    atomic_store(&hs_scan_start_authorized, false);
    atomic_store(&handshake_cleanup_active, false);
    now_us = 1000000;
    serial_calls = 0;
    serial_fail_call = 0;
    mutate_token_call = 0;
    serial_advance_us = 0;
    attempted_bytes = output_bytes = largest_record = 0;
    output[0] = 0;
    done_was_attempted = done_saw_ready = false;
    done_saw_pending = done_saw_in_progress = done_saw_output_owner = true;
    ap_saw_output_owner = ap_saw_in_progress = false;
    scan_command_calls = 0;
    scan_command_result = 0;
}

static int invoke_scan(int argc, const char *const input[]) {
    char *argv[4] = {0};
    for (int i = 0; i < argc; i++) argv[i] = (char *)input[i];
    return cmd_hs_scan(argc, argv);
}

static void begin_completion(const char *token) {
    snprintf(hs_scan_token, sizeof(hs_scan_token), "%s", token);
    snprintf(hs_scan_snapshot.token, sizeof(hs_scan_snapshot.token), "%s", token);
    hs_scan_snapshot.count = g_scan_count;
    memcpy(hs_scan_snapshot.records, g_scan_results,
           g_scan_count * sizeof(g_scan_results[0]));
    hs_scan_pending = true;
    hs_scan_ready = false;
    g_scan_in_progress = true;
    g_scan_teardown_in_progress = true;
    atomic_store(&hs_scan_output_active, true);
}

static void test_success_and_publication_order(void) {
    reset_state();
    begin_completion("scan-token");
    hs_scan_complete(true);
    assert(!hs_scan_pending && hs_scan_ready && !g_scan_in_progress);
    assert(hs_scan_completed_us == now_us);
    assert(serial_calls == 3);
    assert(done_was_attempted && done_saw_ready);
    assert(!done_saw_pending && !done_saw_in_progress && !done_saw_output_owner);
    assert(ap_saw_output_owner && ap_saw_in_progress);
    assert(strstr(output, "\"kind\":\"ap\""));
    assert(strstr(output, "\"kind\":\"scan_done\""));

    /* The completion routine snapshots its token. A concurrent command-side
     * mutation cannot relabel later AP rows or the terminal record. */
    reset_state();
    begin_completion("stable-token");
    mutate_token_call = 1;
    hs_scan_complete(true);
    assert(!strcmp(hs_scan_token, "mutated-token"));
    assert(!strstr(output, "mutated-token"));
    const char *cursor = output;
    unsigned stable_uses = 0;
    while ((cursor = strstr(cursor, "stable-token"))) {
        stable_uses++;
        cursor += strlen("stable-token");
    }
    assert(stable_uses == 3);
}

static void test_serial_failure_never_publishes_ready(void) {
    reset_state();
    g_scan_count = 3;
    begin_completion("partial-scan");
    serial_fail_call = 2;
    hs_scan_complete(true);
    assert(!hs_scan_pending && !hs_scan_ready && !g_scan_in_progress);
    assert(!done_was_attempted);
    assert(strstr(output, "\"seq\":1"));
    assert(!strstr(output, "\"seq\":2"));
    assert(strstr(output, "serial_output_failed"));

    /* A failed terminal write briefly observes the pre-publication state, but
     * the snapshot is revoked before hs_scan_complete returns. */
    reset_state();
    g_scan_count = 1;
    begin_completion("lost-done");
    serial_fail_call = 2;
    hs_scan_complete(true);
    assert(done_was_attempted && done_saw_ready);
    assert(!done_saw_pending && !done_saw_in_progress && !done_saw_output_owner);
    assert(!hs_scan_ready);

    /* The three-second deadline also bounds a stalled but nominally successful
     * transport. No terminal record is published after the deadline. */
    reset_state();
    g_scan_count = 3;
    begin_completion("deadline-scan");
    serial_advance_us = 4000000;
    hs_scan_complete(true);
    assert(!hs_scan_ready && !hs_scan_pending && !g_scan_in_progress);
    assert(!done_was_attempted);
    assert(strstr(output, "serial_output_failed"));
}

static void test_failure_and_bounds(void) {
    reset_state();
    begin_completion("failed-scan");
    hs_scan_complete(false);
    assert(!hs_scan_ready && !hs_scan_pending && !g_scan_in_progress);
    assert(serial_calls == 1 && strstr(output, "scan_failed_or_cancelled"));

    reset_state();
    begin_completion("cancelled-scan");
    operation_stop_requested = true;
    hs_scan_complete(true);
    assert(!hs_scan_ready && !hs_scan_pending && !g_scan_in_progress);
    assert(serial_calls == 1 && strstr(output, "scan_failed_or_cancelled"));

    reset_state();
    hs_scan_ready = true;
    hs_scan_complete(true); /* a stale SDK event has no HST owner */
    assert(serial_calls == 0 && hs_scan_ready);

    reset_state();
    g_scan_count = TEST_AP_LIMIT;
    for (unsigned i = 0; i < TEST_AP_LIMIT; i++) {
        memset(g_scan_results[i].ssid, 0xff, 32);
        g_scan_results[i].ssid[32] = 0;
    }
    begin_completion("max-snapshot-token-32-characters");
    hs_scan_complete(true);
    assert(hs_scan_ready && serial_calls == TEST_AP_LIMIT + 1);
    assert(largest_record < 384);
    assert(attempted_bytes <= TEST_AP_LIMIT * 383U + 255U);
    assert(output_bytes == attempted_bytes && output_bytes < OUTPUT_CAPACITY);
}

static void assert_busy_preserves_token(void) {
    const char *command[] = {"hs_scan", "new-token"};
    char before[sizeof(hs_scan_token)];
    snprintf(before, sizeof(before), "%s", hs_scan_token);
    bool ready_before = hs_scan_ready;
    bool pending_before = hs_scan_pending;
    assert(invoke_scan(2, command) == 1);
    assert(!strcmp(hs_scan_token, before));
    assert(hs_scan_ready == ready_before && hs_scan_pending == pending_before);
    assert(scan_command_calls == 0);
    assert(strstr(output, "\"scan\":\"new-token\""));
    assert(strstr(output, "\"error\":\"busy\""));
}

static void test_command_lifecycle(void) {
    const char *valid[] = {"hs_scan", "new_token-1"};
    const char *bad_char[] = {"hs_scan", "bad token"};
    const char *too_long[] = {"hs_scan", "123456789012345678901234567890123"};

    reset_state();
    hs_scan_ready = true;
    assert(invoke_scan(1, valid) == 1);
    assert(!strcmp(hs_scan_token, "active-token") && serial_calls == 0);
    assert(invoke_scan(2, bad_char) == 1);
    assert(!strcmp(hs_scan_token, "active-token") && serial_calls == 0);
    assert(invoke_scan(2, too_long) == 1);
    assert(!strcmp(hs_scan_token, "active-token") && serial_calls == 0);

    reset_state();
    hs_scan_pending = true;
    hs_scan_ready = true;
    assert_busy_preserves_token();
    reset_state();
    g_scan_in_progress = true;
    assert_busy_preserves_token();
    reset_state();
    g_scan_cancel_pending = true;
    assert_busy_preserves_token();
    reset_state();
    atomic_store(&hs_scan_output_active, true);
    assert_busy_preserves_token();
    reset_state();
    atomic_store(&handshake_cleanup_active, true);
    assert_busy_preserves_token();
    reset_state();
    handshake_attack_active = true;
    assert_busy_preserves_token();
    reset_state();
    handshake_attack_task_handle = (void *)1;
    assert_busy_preserves_token();

    reset_state();
    hs_scan_ready = true;
    assert(invoke_scan(2, valid) == 0);
    assert(!strcmp(hs_scan_token, "new_token-1"));
    assert(hs_scan_pending && !hs_scan_ready);
    assert(scan_command_calls == 1 && serial_calls == 1);
    assert(strstr(output, "\"kind\":\"scan_started\""));

    reset_state();
    hs_scan_ready = true;
    serial_fail_call = 1;
    assert(invoke_scan(2, valid) == 1);
    assert(!hs_scan_pending && !hs_scan_ready && scan_command_calls == 0);

    reset_state();
    hs_scan_ready = true;
    scan_command_result = 1;
    assert(invoke_scan(2, valid) == 1);
    assert(!hs_scan_pending && !hs_scan_ready && scan_command_calls == 1);
    assert(strstr(output, "\"kind\":\"scan_started\""));
    assert(strstr(output, "scan_start_failed"));
}

int main(void) {
    test_success_and_publication_order();
    test_serial_failure_never_publishes_ready();
    test_failure_and_bounds();
    test_command_lifecycle();
    puts("PASS: HST scan publication, serial failure, token races and bounded output");
}
