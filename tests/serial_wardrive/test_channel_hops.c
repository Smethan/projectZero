/* Exercise production standalone C5 hop functions with rejected channels. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t;
typedef int wifi_promiscuous_pkt_type_t;
typedef void (*wifi_promiscuous_cb_t)(void *, wifi_promiscuous_pkt_type_t);
typedef struct { int filter_mask; } wifi_promiscuous_filter_t;
typedef enum { SW_CHANNEL_OK, SW_CHANNEL_DRIVER_ERROR } sw_channel_status_t;
typedef struct {
    sw_channel_status_t status;
    uint8_t requested_channel, actual_channel;
    int sdk_error;
} sw_channel_result_t;
typedef enum {
    WDP_TIER_24_PRIMARY, WDP_TIER_24_SECONDARY,
    WDP_TIER_5_NON_DFS, WDP_TIER_5_DFS
} wdp_channel_tier_t;
typedef struct {
    int channel;
    wdp_channel_tier_t tier;
    double discounted_reward, discounted_pulls;
    int total_pulls;
} wdp_ducb_channel_t;

#define ESP_OK 0
#define TAG "channel-hop-test"
static char last_log[192];
#define MY_LOG_INFO(tag, fmt, ...) snprintf(last_log, sizeof(last_log), fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) snprintf(last_log, sizeof(last_log), fmt, ##__VA_ARGS__)

static wdp_ducb_channel_t wdp_ducb_channels[40];
static int wdp_ducb_channel_count;
static bool sniffer_dog_active, deauth_detector_active, deauth_detector_selected_mode;
static int sniffer_dog_current_channel, sniffer_dog_channel_index;
static int deauth_detector_current_channel, deauth_detector_channel_index;
static int deauth_detector_selected_channels[8], deauth_detector_selected_channels_count;
static int64_t sniffer_dog_last_channel_hop, deauth_detector_last_channel_hop;
static int fail_channel = -1, fail_promisc_stage;
static int promisc_off_calls, callback_clear_calls, led_idle_calls;
static uint8_t physical_channel = 6;
static int64_t fake_time_us = 1234000;

static sw_channel_result_t sw_job_apply_channel_checked(uint8_t channel) {
    sw_channel_result_t result = {.requested_channel = channel, .actual_channel = physical_channel};
    if (channel == fail_channel) {
        result.status = SW_CHANNEL_DRIVER_ERROR;
        result.sdk_error = 258;
    } else {
        result.status = SW_CHANNEL_OK;
        physical_channel = result.actual_channel = channel;
    }
    return result;
}
static bool sw_job_owned(void) { return false; }
static void sw_job_emit_channel_failure(sw_channel_result_t result) { (void)result; assert(false); }
static const char *sw_job_token(void) { return ""; }
static bool sw_job_request_cleanup(const char *token, const char *reason) {
    (void)token; (void)reason; assert(false); return false;
}
static esp_err_t esp_wifi_set_promiscuous(bool on) {
    if (!on) promisc_off_calls++;
    return fail_promisc_stage == 3 && on ? 258 : ESP_OK;
}
static esp_err_t esp_wifi_set_promiscuous_rx_cb(wifi_promiscuous_cb_t callback) {
    if (!callback) callback_clear_calls++;
    return fail_promisc_stage == 2 && callback ? 258 : ESP_OK;
}
static esp_err_t esp_wifi_set_promiscuous_filter(const wifi_promiscuous_filter_t *filter) {
    (void)filter; return fail_promisc_stage == 1 ? 258 : ESP_OK;
}
static esp_err_t led_set_idle(void) { led_idle_calls++; return ESP_OK; }
static const char *esp_err_to_name(int error) { (void)error; return "ESP_ERR_INVALID_ARG"; }
static int64_t esp_timer_get_time(void) { return fake_time_us; }
static void dummy_callback(void *packet, wifi_promiscuous_pkt_type_t type) {
    (void)packet; (void)type;
}

#include "channel_hops_under_test.inc"

int main(void) {
    bool saw_140 = false, saw_144 = false, saw_149 = false;
    for (int i = 0; i < dual_band_channels_count; i++) {
        assert(dual_band_channels[i] < 12 || dual_band_channels[i] > 14);
        saw_140 |= dual_band_channels[i] == 140;
        saw_144 |= dual_band_channels[i] == 144;
        saw_149 |= dual_band_channels[i] == 149;
    }
    assert(saw_140 && saw_149 && saw_144 == (bool)PROJECTZERO_XIAO_US_CH144);
    bool wdp_has_144 = false;
    for (unsigned i = 0; i < sizeof(wdp_ch_5_dfs); i++) wdp_has_144 |= wdp_ch_5_dfs[i] == 144;
    assert(wdp_has_144 == (bool)PROJECTZERO_XIAO_US_CH144);

    wdp_ducb_channels[0] = (wdp_ducb_channel_t){.channel=140,.tier=WDP_TIER_5_DFS,.total_pulls=7};
    wdp_ducb_channels[1] = (wdp_ducb_channel_t){.channel=144,.tier=WDP_TIER_5_DFS,.total_pulls=2};
    wdp_ducb_channels[2] = (wdp_ducb_channel_t){.channel=149,.tier=WDP_TIER_5_NON_DFS,.total_pulls=3};
    wdp_ducb_channel_count=3;fail_channel=144;
    assert(!wdp_ducb_apply_channel(1) && wdp_ducb_channel_count==2);
    assert(wdp_ducb_channels[1].channel==149 &&
           wdp_ducb_channels[1].tier==WDP_TIER_5_NON_DFS &&
           wdp_ducb_channels[1].total_pulls==3);
    assert(strstr(last_log,"requested=144") && physical_channel==6);
    fail_channel=-1;
    assert(wdp_ducb_apply_channel(1) && physical_channel==149);
    fail_channel=149;assert(!wdp_ducb_apply_channel(1) && wdp_ducb_channel_count==1);
    fail_channel=140;assert(!wdp_ducb_apply_channel(0) && wdp_ducb_channel_count==0);
    assert(!wdp_ducb_apply_channel(0));

    int idx_149=-1;
    for (int i=0;i<dual_band_channels_count;i++)
        if (dual_band_channels[i]==149) idx_149=i;
    assert(idx_149>=0);
    physical_channel=140;sniffer_dog_current_channel=140;
    sniffer_dog_channel_index=idx_149;sniffer_dog_active=true;fail_channel=149;
    sniffer_dog_channel_hop();
    assert(!sniffer_dog_active && sniffer_dog_current_channel==140 &&
           sniffer_dog_channel_index==idx_149 && physical_channel==140);
    assert(promisc_off_calls==1 && callback_clear_calls==1 && led_idle_calls==1);
    fail_channel=-1;sniffer_dog_active=true;
    sniffer_dog_channel_hop();
    assert(sniffer_dog_active && sniffer_dog_current_channel==149 &&
           sniffer_dog_channel_index==(idx_149+1)%dual_band_channels_count &&
           sniffer_dog_last_channel_hop==fake_time_us/1000);

    deauth_detector_selected_mode=true;deauth_detector_selected_channels_count=1;
    deauth_detector_selected_channels[0]=144;
    deauth_detector_active=true;deauth_detector_current_channel=6;
    deauth_detector_channel_index=0;physical_channel=6;fail_channel=144;
    deauth_detector_channel_hop();
    assert(!deauth_detector_active && deauth_detector_current_channel==6 &&
           deauth_detector_channel_index==0 && physical_channel==6);
    assert(promisc_off_calls==2 && callback_clear_calls==2 && led_idle_calls==2);
    fail_channel=-1;deauth_detector_selected_channels[0]=149;deauth_detector_active=true;
    deauth_detector_channel_hop();
    assert(deauth_detector_active && deauth_detector_current_channel==149 &&
           deauth_detector_last_channel_hop==fake_time_us/1000);

    wifi_promiscuous_filter_t filter={.filter_mask=1};
    for (int stage=1;stage<=3;stage++) {
        fail_promisc_stage=stage;
        assert(!monitor_enable_promiscuous("test",&filter,dummy_callback));
    }
    fail_promisc_stage=0;
    assert(monitor_enable_promiscuous("test",&filter,dummy_callback));
    puts("PASS: standalone C5 channel rejection, WDP prune/exhaustion, selected 144 cleanup, monitor setup");
}
