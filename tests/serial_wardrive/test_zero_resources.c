/* Sizes are checked against the actual RISC-V ELF DWARF in the build report.
 * Opaque native types isolate production allocation/cleanup logic from drivers. */
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define OPAQUE(name,bytes) typedef struct { uint8_t value[bytes]; } name
OPAQUE(wifi_ap_record_t,96); OPAQUE(sniffer_ap_t,316); OPAQUE(probe_request_t,48);
OPAQUE(target_bssid_t,48); OPAQUE(selected_station_t,7); OPAQUE(whitelisted_bssid_t,6);
OPAQUE(hs_ap_target_t,112); OPAQUE(hs_client_entry_t,40); OPAQUE(ducb_channel_t,32);
OPAQUE(hs_queued_frame_t,524); OPAQUE(bt_device_info_t,120); OPAQUE(wdp_network_t,72);
#define MAX_SNIFFER_APS 16
#define MAX_PROBE_REQUESTS 64
#define MAX_TARGET_BSSIDS 8
#define MAX_SELECTED_STATIONS 16
#define MAX_WHITELISTED_BSSIDS 150
#define MAX_AP_CNT 8
#define HS_MAX_APS 8
#define HS_MAX_CLIENTS 32
#define HS_FRAME_POOL_COUNT 8
#define BT_INITIAL_CAPACITY 32
#define WDP_INITIAL_CAPACITY 64
#define ZERO_JOB_MEMORY_CAP (48u*1024u)
#define ZERO_JOB_MEMORY_RESERVE (32u*1024u)
#define SNIFFER_CHANNEL_TASK_STACK_SIZE 4096u
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
static unsigned live_allocations, allocation_attempts;
static int fail_at=-1;
static size_t free_heap=1024*1024;
static void *heap_caps_calloc(size_t count,size_t size,unsigned caps) {
    (void)caps;
    if ((int)allocation_attempts++==fail_at)return NULL;
    void *p=calloc(count,size);if(p)live_allocations++;return p;
}
static void heap_caps_free(void *p) {if(p){assert(live_allocations);live_allocations--;free(p);}}
static size_t heap_caps_get_free_size(unsigned caps) {(void)caps;return free_heap;}
static sniffer_ap_t *sniffer_aps;
static probe_request_t *probe_requests;
static target_bssid_t *target_bssids;
static selected_station_t *selected_stations;
static whitelisted_bssid_t *whiteListedBssids;
static uint8_t (*bt_found_devices)[6];
static bt_device_info_t *bt_devices;
static wifi_ap_record_t *wardrive_scan_results,*handshake_targets;
static hs_ap_target_t *hs_ap_targets;
static hs_client_entry_t *hs_clients;
static ducb_channel_t *ducb_channels;
static wdp_network_t *wdp_seen_networks;
static void *hs_exchange_state,*hs_artifact_buffer;
static struct {void *frames;} hs_frame_pool;
static void hs_capture_close(void) {hs_frame_pool.frames=NULL;}
static int dual_band_channels_count=39;
static unsigned bt_found_device_capacity,bt_device_capacity,wdp_seen_capacity;
static unsigned target_bssid_count,selected_stations_count,hs_ap_count,hs_client_count,ducb_channel_count;
static unsigned zero_job_blackout_target_count;
static bool zero_job_blackout_selected;
enum {ZERO_RES_SNIFFER=1u<<0,ZERO_RES_TARGETS=1u<<1,ZERO_RES_BLE=1u<<2,
      ZERO_RES_HANDSHAKE=1u<<3,ZERO_RES_WARDRIVE=1u<<4,ZERO_RES_PORTAL=1u<<5};
#include "zero_resources_under_test.inc"
int main(void) {
    assert(zero_job_resource_estimate(ZERO_RES_SNIFFER) ==
        6144u + SNIFFER_CHANNEL_TASK_STACK_SIZE +
        MAX_SNIFFER_APS*sizeof(sniffer_ap_t) + MAX_PROBE_REQUESTS*sizeof(probe_request_t));
    const unsigned masks[]={ZERO_RES_SNIFFER,ZERO_RES_TARGETS,ZERO_RES_BLE,
        ZERO_RES_HANDSHAKE|ZERO_RES_TARGETS,ZERO_RES_WARDRIVE,ZERO_RES_PORTAL,
        ZERO_RES_SNIFFER|ZERO_RES_TARGETS};
    for(unsigned m=0;m<sizeof(masks)/sizeof(masks[0]);m++) {
        unsigned mask=masks[m];
        assert(zero_job_resource_estimate(mask)<=ZERO_JOB_MEMORY_CAP);
        allocation_attempts=0;fail_at=-1;
        assert(zero_job_resources_open(mask));
        unsigned count=allocation_attempts;
        assert(!zero_job_resources_open(mask)); /* Exclusive resource ownership. */
        zero_job_resources_close();assert(!live_allocations);
        for(unsigned i=0;i<count;i++) {
            allocation_attempts=0;fail_at=(int)i;
            assert(!zero_job_resources_open(mask));
            assert(!live_allocations && !zero_job_allocation_count && !zero_job_resource_mask);
            assert(!sniffer_aps&&!target_bssids&&!bt_devices&&!hs_clients&&!ducb_channels);
            zero_job_resources_close();
        }
        fail_at=-1;free_heap=zero_job_resource_estimate(mask)+ZERO_JOB_MEMORY_RESERVE-1;
        assert(!zero_job_resources_open(mask));assert(!live_allocations);
        free_heap=1024*1024;
        for(unsigned repeat=0;repeat<100;repeat++) {
            assert(zero_job_resources_open(mask));zero_job_resources_close();
            assert(!live_allocations&&!zero_job_allocated_bytes&&!zero_job_resource_mask);
        }
    }
    puts("PASS: Zero production admission, each allocation-stage failure, exclusive ownership and 700 leak-free transitions");
}
