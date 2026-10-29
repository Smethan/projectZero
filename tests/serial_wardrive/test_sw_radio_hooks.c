/* Fault-inject the actual production serial-radio hooks without RF hardware. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
typedef int wifi_band_mode_t;
typedef int wifi_mode_t;
typedef int wifi_second_chan_t;
typedef struct { int filter_mask; } wifi_promiscuous_filter_t;
#define ESP_OK 0
#define WIFI_BAND_MODE_AUTO 1
#define WIFI_MODE_NULL 0
#define WIFI_SECOND_CHAN_NONE 0
#define BLE_HS_EALREADY 1
#define pdMS_TO_TICKS(x) (x)
#define TAG "radio-test"
static char last_log[160];
#define MY_LOG_INFO(tag,fmt,...) snprintf(last_log,sizeof(last_log),fmt,##__VA_ARGS__)

static bool wardrive_active,wardrive_promisc_active,bt_scan_active;
static bool handshake_attack_active,antisurv_active,nimble_initialized;
static bool wifi_only,ble_only,ensure_wifi_ok=true;
static bool passive_hs,job_mode,collect_wifi=true,collect_ble=true;
static int fail_get_band,fail_get_filter,fail_get_mode,fail_get_channel;
static int fail_set_mode,fail_restore_mode,fail_set_band,fail_restore_channel;
static bool stale_channel_readback;
static uint8_t test_channel=6;
static unsigned set_channel_calls;
static int nimble_result,scan_result;
static bool scan_active,ble_quiescent=true,nimble_deinit_ok=true,partial_nimble_after_failed_init,scan_cancel_stuck;
static bool ble_accepting;
static int scan_cancel_result;
static unsigned cancel_delay_polls;
static unsigned get_band_calls,get_filter_calls,get_mode_calls,set_mode_calls,set_band_calls;
static unsigned set_filter_calls,set_promisc_off_calls,set_cb_null_calls,bt_stop_calls;
static unsigned nimble_calls,scan_calls,scan_cancel_calls,nimble_deinit_calls,ble_quiescence_calls;
static int last_set_mode,last_set_band,last_filter_mask;
static wifi_promiscuous_filter_t original_filter={.filter_mask=0x5a};
static bool ensure_wifi_mode(void) { return ensure_wifi_ok; }
static bool sw_wifi_only_mode(void) { return wifi_only || passive_hs || (job_mode && !collect_ble); }
static bool sw_ble_only_mode(void) { return job_mode && !collect_wifi && collect_ble; }
static void bt_stop_scan(void) { bt_stop_calls++; }
static bool sw_ble_wait_quiescent(unsigned timeout_ms) {
    (void)timeout_ms;ble_quiescence_calls++;return ble_quiescent;
}
static void sw_ble_set_accepting(bool accepting) { ble_accepting=accepting; }
static int ble_gap_disc_active(void) { return scan_active?1:0; }
static int ble_gap_disc_cancel(void) {
    scan_cancel_calls++;
    if(!scan_cancel_result&&!scan_cancel_stuck&&!cancel_delay_polls)scan_active=false;
    return scan_cancel_result;
}
static void vTaskDelay(unsigned ticks) {
    (void)ticks;
    if(cancel_delay_polls && --cancel_delay_polls==0 && !scan_cancel_stuck)scan_active=false;
}
static bool bt_nimble_deinit(void) {
    nimble_deinit_calls++;
    if(!nimble_deinit_ok)return false;
    nimble_initialized=false;return true;
}
static esp_err_t bt_nimble_init(void) {
    nimble_calls++;
    if(nimble_result==ESP_OK || partial_nimble_after_failed_init)nimble_initialized=true;
    return nimble_result;
}
static int bt_start_scan_coex(void) {
    scan_calls++;
    if(scan_active)return -1;
    if(scan_result==0)scan_active=true;
    return scan_result;
}
static esp_err_t esp_wifi_get_band_mode(wifi_band_mode_t *value) {
    get_band_calls++; if(fail_get_band)return fail_get_band; *value=7; return ESP_OK;
}
static esp_err_t esp_wifi_get_promiscuous_filter(wifi_promiscuous_filter_t *value) {
    get_filter_calls++; if(fail_get_filter)return fail_get_filter; *value=original_filter; return ESP_OK;
}
static esp_err_t esp_wifi_get_mode(wifi_mode_t *value) {
    get_mode_calls++; if(fail_get_mode)return fail_get_mode; *value=4; return ESP_OK;
}
static esp_err_t esp_wifi_get_channel(uint8_t *value,wifi_second_chan_t *second) {
    if(fail_get_channel)return fail_get_channel;
    *value=stale_channel_readback && set_channel_calls ? (uint8_t)(test_channel+1) : test_channel;
    *second=WIFI_SECOND_CHAN_NONE;
    return ESP_OK;
}
static esp_err_t esp_wifi_set_channel(uint8_t value,wifi_second_chan_t second) {
    (void)second;set_channel_calls++;
    if(fail_restore_channel)return fail_restore_channel;
    test_channel=value;return ESP_OK;
}
static esp_err_t esp_wifi_set_mode(wifi_mode_t value) {
    set_mode_calls++; last_set_mode=value;
    return value==WIFI_MODE_NULL ? fail_set_mode : fail_restore_mode;
}
static esp_err_t esp_wifi_set_band_mode(wifi_band_mode_t value) {
    set_band_calls++; last_set_band=value;
    return value==WIFI_BAND_MODE_AUTO ? fail_set_band : ESP_OK;
}
static esp_err_t esp_wifi_set_promiscuous(bool enabled) {
    if(!enabled)set_promisc_off_calls++; return ESP_OK;
}
static esp_err_t esp_wifi_set_promiscuous_rx_cb(void *callback) {
    if(!callback)set_cb_null_calls++; return ESP_OK;
}
static esp_err_t esp_wifi_set_promiscuous_filter(const wifi_promiscuous_filter_t *value) {
    set_filter_calls++; last_filter_mask=value->filter_mask; return ESP_OK;
}
static bool sw_job_owned(void) { return false; }
static void sw_job_emit_lifecycle(const char *kind,const char *state,const char *fields) {
    (void)kind;(void)state;(void)fields;
}
bool sw_radio_stop(void);

#include "sw_radio_start_under_test.inc"
#include "sw_radio_stop_under_test.inc"

static void reset_fixture(void) {
    wardrive_active=wardrive_promisc_active=bt_scan_active=false;
    handshake_attack_active=antisurv_active=nimble_initialized=false;
    wifi_only=ble_only=passive_hs=job_mode=false;collect_wifi=collect_ble=true;ensure_wifi_ok=true;
    fail_get_band=fail_get_filter=fail_get_mode=fail_get_channel=0;
    fail_set_mode=fail_restore_mode=fail_set_band=fail_restore_channel=0;
    stale_channel_readback=false;test_channel=6;set_channel_calls=0;
    nimble_result=scan_result=ESP_OK;
    scan_active=false;scan_cancel_stuck=false;scan_cancel_result=0;cancel_delay_polls=0;
    ble_quiescent=nimble_deinit_ok=true;partial_nimble_after_failed_init=false;ble_accepting=false;
    get_band_calls=get_filter_calls=get_mode_calls=set_mode_calls=set_band_calls=0;
    set_filter_calls=set_promisc_off_calls=set_cb_null_calls=bt_stop_calls=0;
    nimble_calls=scan_calls=0;last_set_mode=last_set_band=last_filter_mask=0;
    scan_cancel_calls=nimble_deinit_calls=ble_quiescence_calls=0;
    last_log[0]=0;sw_saved_radio=false;sw_saved_mode_changed=false;
    sw_channel_restore_pending=false;
    sw_nimble_owned=sw_ble_scan_owned=false;
    sw_ble_cleanup_error_reported=sw_nimble_cleanup_error_reported=false;
}

static void assert_restored(bool restore_mode) {
    assert(set_promisc_off_calls>=1 && set_cb_null_calls>=1);
    assert(set_filter_calls>=1 && last_filter_mask==original_filter.filter_mask);
    assert(set_band_calls>=1 && last_set_band==7);
    if(restore_mode)assert(set_mode_calls>=2 && last_set_mode==4 &&
                           set_channel_calls>=1 && test_channel==6);
    else assert(set_channel_calls==0);
    assert(!sw_saved_radio);
}

int main(void) {
    reset_fixture(); wardrive_active=true;
    assert(!sw_radio_start() && strstr(last_log,"busy-operation gate"));
    assert(!get_band_calls && !sw_saved_radio);

    reset_fixture(); ensure_wifi_ok=false;
    assert(!sw_radio_start() && strstr(last_log,"ensure_wifi_mode"));
    assert(!get_band_calls && !sw_saved_radio);

    reset_fixture(); fail_get_band=21;
    assert(!sw_radio_start() && strstr(last_log,"get_band_mode: 21"));
    assert(!sw_saved_radio && !set_band_calls);
    reset_fixture(); fail_get_filter=22;
    assert(!sw_radio_start() && strstr(last_log,"get_promiscuous_filter: 22"));
    assert(!sw_saved_radio && !set_band_calls);
    reset_fixture(); fail_get_mode=23;
    assert(!sw_radio_start() && strstr(last_log,"get_mode: 23"));
    assert(!sw_saved_radio && !set_band_calls);
    reset_fixture(); fail_get_channel=24;
    assert(!sw_radio_start() && strstr(last_log,"get_channel: 24"));
    assert(!sw_saved_radio && !set_band_calls);

    reset_fixture(); job_mode=true;collect_wifi=true;collect_ble=true;fail_set_mode=31;
    assert(!sw_radio_start() && strstr(last_log,"set_null_mode: 31") && sw_saved_radio);
    assert(!sw_saved_mode_changed);sw_radio_stop(); assert_restored(false);
    assert(set_mode_calls==1);

    reset_fixture(); job_mode=true;collect_wifi=true;collect_ble=true;fail_set_band=32;
    assert(!sw_radio_start() && strstr(last_log,"set_auto_band: 32") && sw_saved_radio);
    assert(sw_saved_mode_changed);
    sw_radio_stop(); assert_restored(true);

    reset_fixture(); nimble_result=41;
    assert(!sw_radio_start() && strstr(last_log,"nimble_init: 41") && sw_saved_radio);
    assert(nimble_calls==1 && !scan_calls);
    assert(sw_saved_mode_changed);assert(sw_radio_stop()); assert(nimble_deinit_calls==1 && !nimble_initialized);assert_restored(true);

    reset_fixture();nimble_result=41;partial_nimble_after_failed_init=true;nimble_deinit_ok=false;
    assert(!sw_radio_start() && sw_saved_radio && nimble_initialized);
    assert(!sw_radio_stop() && sw_saved_radio && sw_nimble_owned);
    nimble_deinit_ok=true;assert(sw_radio_stop() && !sw_saved_radio && !sw_nimble_owned);
    assert(nimble_deinit_calls==2);

    reset_fixture(); job_mode=true;collect_wifi=true;collect_ble=true;scan_result=-7;
    assert(!sw_radio_start() && strstr(last_log,"ble_scan_coex: -7") && sw_saved_radio);
    assert(nimble_calls==1 && scan_calls==1 && nimble_initialized);
    scan_cancel_result=BLE_HS_EALREADY;
    assert(sw_saved_mode_changed);assert(sw_radio_stop());
    assert(scan_cancel_calls==1 && ble_quiescence_calls==1 && nimble_deinit_calls==1 && !nimble_initialized);assert_restored(true);

    reset_fixture(); wifi_only=true; nimble_initialized=true;
    assert(sw_radio_start());
    assert(!nimble_calls && !scan_calls && bt_stop_calls==0);
    test_channel=11; /* Radio hopping changed the channel after the snapshot. */
    assert(sw_radio_stop()); assert(nimble_initialized && !nimble_deinit_calls && !scan_cancel_calls);assert_restored(true);

    reset_fixture(); wifi_only=true;
    assert(sw_radio_start());test_channel=11;fail_restore_channel=88;
    assert(!sw_radio_stop() && sw_saved_radio && sw_channel_restore_pending);
    fail_restore_channel=0;assert(sw_radio_stop());assert_restored(true);

    reset_fixture(); wifi_only=true;
    assert(sw_radio_start());test_channel=11;stale_channel_readback=true;
    assert(!sw_radio_stop() && sw_saved_radio && sw_channel_restore_pending);
    stale_channel_readback=false;assert(sw_radio_stop());assert_restored(true);

    reset_fixture(); ble_only=true;
    assert(sw_radio_start());
    assert(nimble_calls==1 && scan_calls==1 && last_set_mode==WIFI_MODE_NULL && ble_accepting);
    assert(sw_radio_stop()); assert(scan_cancel_calls==1 && nimble_deinit_calls==1 && !nimble_initialized);assert_restored(true);
    assert(!ble_accepting);

    reset_fixture(); job_mode=true;collect_wifi=true;collect_ble=true;nimble_initialized=true;
    assert(sw_radio_start());
    assert(nimble_calls==0 && scan_calls==1 && last_set_mode==WIFI_MODE_NULL && bt_stop_calls==0);
    assert(sw_saved_mode_changed);assert(sw_radio_stop());
    assert(scan_cancel_calls==1 && nimble_deinit_calls==0 && nimble_initialized);assert_restored(true);

    reset_fixture();job_mode=true;collect_wifi=true;collect_ble=true;
    assert(sw_radio_start() && sw_ble_scan_owned && sw_nimble_owned);
    scan_cancel_result=17;
    assert(!sw_radio_stop() && sw_saved_radio && sw_ble_scan_owned && sw_nimble_owned);
    assert(!ble_quiescence_calls && !nimble_deinit_calls);
    scan_cancel_result=0;
    assert(sw_radio_stop() && !sw_saved_radio && !sw_ble_scan_owned && !sw_nimble_owned);
    assert(scan_cancel_calls==2 && ble_quiescence_calls==1 && nimble_deinit_calls==1 && !nimble_initialized);

    reset_fixture();job_mode=true;collect_wifi=true;collect_ble=true;
    assert(sw_radio_start());ble_quiescent=false;
    assert(!sw_radio_stop() && sw_saved_radio && sw_ble_scan_owned && sw_nimble_owned);
    assert(!nimble_deinit_calls);
    ble_quiescent=true;
    assert(sw_radio_stop() && !sw_saved_radio && !sw_ble_scan_owned && !sw_nimble_owned);
    assert(ble_quiescence_calls==2 && nimble_deinit_calls==1);

    reset_fixture();job_mode=true;collect_wifi=true;collect_ble=true;
    assert(sw_radio_start());cancel_delay_polls=2;
    assert(sw_radio_stop() && !scan_active && scan_cancel_calls==1);

    reset_fixture();job_mode=true;collect_wifi=true;collect_ble=true;
    assert(sw_radio_start());scan_cancel_stuck=true;
    unsigned mode_calls_before= set_mode_calls,band_calls_before=set_band_calls;
    assert(!sw_radio_stop() && sw_saved_radio && sw_ble_scan_owned && sw_nimble_owned);
    assert(set_mode_calls==mode_calls_before && set_band_calls==band_calls_before);
    scan_cancel_stuck=false;scan_active=false;
    assert(sw_radio_stop() && !sw_saved_radio);

    reset_fixture();job_mode=true;collect_wifi=true;collect_ble=true;
    assert(sw_radio_start());nimble_deinit_ok=false;
    unsigned mode_before_deinit=set_mode_calls,band_before_deinit=set_band_calls;
    assert(!sw_radio_stop() && sw_saved_radio && !sw_ble_scan_owned && sw_nimble_owned);
    assert(set_mode_calls==mode_before_deinit && set_band_calls==band_before_deinit);
    nimble_deinit_ok=true;
    assert(sw_radio_stop() && !sw_saved_radio && !sw_nimble_owned && !nimble_initialized);
    assert(scan_cancel_calls==1 && nimble_deinit_calls==2);

    reset_fixture();job_mode=true;collect_wifi=true;collect_ble=true;nimble_initialized=true;scan_active=true;
    assert(!sw_radio_start() && scan_active && !sw_ble_scan_owned && !sw_nimble_owned);
    assert(!sw_saved_radio && !set_mode_calls && !set_band_calls);
    assert(sw_radio_stop() && scan_active && !scan_cancel_calls && !nimble_deinit_calls);

    reset_fixture(); job_mode=true;collect_wifi=true;collect_ble=true;
    assert(sw_radio_start());fail_restore_mode=77;assert(!sw_radio_stop());
    assert(sw_saved_radio && sw_saved_mode_changed && strstr(last_log,"restore failed at set_mode"));
    fail_restore_mode=ESP_OK;
    assert(sw_radio_start()); /* retries the pending restore before saving current state */
    assert(sw_saved_radio && sw_saved_mode_changed);
    assert(sw_radio_stop());
    assert(set_promisc_off_calls==3 && set_cb_null_calls==3 && set_filter_calls==3);
    assert(last_filter_mask==original_filter.filter_mask && last_set_band==7 && last_set_mode==4);
    assert(!sw_saved_radio && !sw_saved_mode_changed);
    puts("PASS: extracted radio hooks report each failure stage and restore state across Wi-Fi/BLE paths");
    return 0;
}
