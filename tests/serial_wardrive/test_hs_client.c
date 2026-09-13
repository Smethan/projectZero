#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HS_MAX_APS 64
#define HS_MAX_CLIENTS 128

typedef struct {
    uint8_t mac[6];
    int hs_ap_index;
    int rssi;
    int64_t last_seen_us;
    int64_t last_deauth_us;
    bool deauthed;
} hs_client_entry_t;

static hs_client_entry_t clients[HS_MAX_CLIENTS];
static hs_client_entry_t *hs_clients = clients;
static int hs_client_count;
static int hs_ap_count;
static int hs_dwell_new_clients;
static int64_t now_us;
static int64_t esp_timer_get_time(void) { return ++now_us; }

#include "hs_client_under_test.inc"

int main(void) {
    const uint8_t sta[6] = {0x02, 1, 2, 3, 4, 5};
    hs_ap_count = 2;
    int first = hs_add_or_update_client(sta, 0, -41);
    int second = hs_add_or_update_client(sta, 1, -52);
    assert(first == 0 && second == 1 && hs_client_count == 2);
    assert(hs_clients[first].hs_ap_index == 0);
    assert(hs_clients[second].hs_ap_index == 1);
    assert(hs_dwell_new_clients == 2);

    int updated = hs_add_or_update_client(sta, 0, -30);
    assert(updated == first && hs_client_count == 2);
    assert(hs_clients[first].rssi == -30);
    assert(hs_clients[second].rssi == -52);
    assert(hs_dwell_new_clients == 2);
    assert(hs_find_client(sta, 0) == first);
    assert(hs_find_client(sta, 1) == second);
    assert(hs_add_or_update_client(sta, -1, 0) == -1);
    assert(hs_add_or_update_client(sta, 2, 0) == -1);

    puts("PASS: identical STA addresses retain independent AP state and cooldown entries");
}
