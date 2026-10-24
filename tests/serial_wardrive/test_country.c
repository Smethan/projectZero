#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
typedef struct { char cc[3]; unsigned schan, nchan, policy; } wifi_country_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define WIFI_COUNTRY_POLICY_MANUAL 1
#define MY_LOG_INFO(...) ((void)0)

static wifi_country_t current;
static int reads, writes, fail_read_at, set_error;
static bool wrong_readback;

static int esp_wifi_get_country(wifi_country_t *country) {
    reads++;
    if (reads == fail_read_at) return -7;
    *country = current;
    return ESP_OK;
}
static int esp_wifi_set_country_code(const char *code, bool ieee80211d_enabled) {
    assert(strcmp(code, "US") == 0 && !ieee80211d_enabled);
    writes++;
    if (set_error) return set_error;
    current = (wifi_country_t){ .cc="US", .schan=1, .nchan=11,
                                .policy=WIFI_COUNTRY_POLICY_MANUAL };
    if (wrong_readback) memcpy(current.cc, "PH", 3);
    return ESP_OK;
}
static const char *esp_err_to_name(int error) { (void)error; return "fixture"; }
#include "country_under_test.inc"

int main(void) {
    current = (wifi_country_t){ .cc="PH", .schan=1, .nchan=14 };
    assert(wifi_apply_us_country() == ESP_OK && writes == 1 && reads == 2);
    assert(strcmp(current.cc, "US") == 0 && current.nchan == 11 &&
           current.policy == WIFI_COUNTRY_POLICY_MANUAL);
    assert(wifi_apply_us_country() == ESP_OK && writes == 1 && reads == 3);

    current = (wifi_country_t){ .cc="PH", .schan=1, .nchan=14 };
    set_error = -6;
    assert(wifi_apply_us_country() == -6 && writes == 2);
    set_error = 0;

    fail_read_at = reads + 2;
    assert(wifi_apply_us_country() == -7 && writes == 3);
    fail_read_at = 0;

    current = (wifi_country_t){ .cc="PH", .schan=1, .nchan=14 };
    wrong_readback = true;
    assert(wifi_apply_us_country() == ESP_FAIL && writes == 4);
    wrong_readback = false;
    assert(wifi_apply_us_country() == ESP_OK && writes == 5);
    puts("PASS: US fixed-country setup, no duplicate write, setter/readback/mismatch errors");
}
