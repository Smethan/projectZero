#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>

typedef void *TaskHandle_t;
typedef void *QueueHandle_t;
typedef struct { unsigned filter_mask; } wifi_promiscuous_filter_t;
typedef int err_t;
typedef struct netif netif_t;
typedef err_t (*netif_input_fn)(void *, netif_t *);
typedef err_t (*netif_linkoutput_fn)(netif_t *, void *);
struct netif { netif_input_fn input; netif_linkoutput_fn linkoutput; };
typedef struct { int unused; } esp_netif_t;
enum { ESP_OK=0, ESP_FAIL=-1, ERR_IF=-2 };
enum { PCAP_CAPTURE_IDLE, PCAP_CAPTURE_STARTING, PCAP_CAPTURE_RUNNING,
       PCAP_CAPTURE_STOPPING, PCAP_CAPTURE_COMPLETE, PCAP_CAPTURE_FAILED };

static atomic_bool pcap_capture_active,pcap_writer_stop_requested,pcap_writer_finished;
static atomic_uint pcap_producers;
static atomic_int pcap_capture_state;
static bool pcap_rx_configured,pcap_filter_saved,pcap_net_hooks_installed,pcap_arp_active;
static wifi_promiscuous_filter_t pcap_previous_filter;
static TaskHandle_t pcap_writer_task_handle,pcap_arp_task_handle;
static QueueHandle_t pcap_packet_queue;
static netif_input_fn pcap_original_input;
static netif_linkoutput_fn pcap_original_linkoutput;
static int pcap_capture_mode,pcap_arp_host_count;
#define PCAP_MODE_NONE 0
#define pdMS_TO_TICKS(ms) (ms)

static unsigned delay_calls,queue_deletes,rx_disable_calls,filter_restores;
static unsigned finish_writer_at,finish_arp_at,finish_producer_at;
static bool fail_rx_disable;
static esp_netif_t sta;
static struct netif nif;
static err_t original_input(void *p,netif_t *n) {(void)p;(void)n;return 0;}
static err_t original_output(netif_t *n,void *p) {(void)n;(void)p;return 0;}
static err_t hook_input(void *p,netif_t *n) {(void)p;(void)n;return 0;}
static err_t hook_output(netif_t *n,void *p) {(void)n;(void)p;return 0;}
static int esp_wifi_set_promiscuous(bool enabled) {
    assert(!enabled);rx_disable_calls++;return fail_rx_disable?ESP_FAIL:ESP_OK;
}
static int esp_wifi_set_promiscuous_rx_cb(void *callback) {
    assert(callback==NULL);return ESP_OK;
}
static int esp_wifi_set_promiscuous_filter(const wifi_promiscuous_filter_t *filter) {
    assert(filter==&pcap_previous_filter);filter_restores++;return ESP_OK;
}
static esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) {
    (void)key;return &sta;
}
static void *esp_netif_get_netif_impl(esp_netif_t *netif) {
    assert(netif==&sta);return &nif;
}
static void vTaskDelay(unsigned ticks) {
    assert(ticks==10 || ticks==50);
    delay_calls++;
    if(finish_writer_at && delay_calls>=finish_writer_at) {
        atomic_store(&pcap_writer_finished,true);pcap_writer_task_handle=NULL;
        atomic_store(&pcap_capture_state,PCAP_CAPTURE_COMPLETE);
    }
    if(finish_arp_at && delay_calls>=finish_arp_at)pcap_arp_task_handle=NULL;
    if(finish_producer_at && delay_calls>=finish_producer_at)
        atomic_store(&pcap_producers,0);
}
static void vQueueDelete(QueueHandle_t queue) {
    assert(queue==pcap_packet_queue);queue_deletes++;
}

#include "pcap_stop_under_test.inc"

static void reset_case(void) {
    atomic_store(&pcap_capture_active,true);
    atomic_store(&pcap_capture_state,PCAP_CAPTURE_RUNNING);
    atomic_store(&pcap_writer_stop_requested,false);
    atomic_store(&pcap_writer_finished,false);
    atomic_store(&pcap_producers,0);
    pcap_rx_configured=pcap_filter_saved=true;
    pcap_net_hooks_installed=false;
    pcap_arp_active=false;
    pcap_writer_task_handle=(void *)1;
    pcap_arp_task_handle=NULL;
    pcap_packet_queue=(void *)2;
    pcap_original_input=NULL;pcap_original_linkoutput=NULL;
    pcap_arp_host_count=4;pcap_capture_mode=1;
    delay_calls=queue_deletes=rx_disable_calls=filter_restores=0;
    finish_writer_at=finish_arp_at=finish_producer_at=0;
    fail_rx_disable=false;
}

int main(void) {
    reset_case();
    fail_rx_disable=true;
    assert(!pcap_capture_stop());
    assert(pcap_packet_queue && pcap_writer_task_handle && !queue_deletes);
    assert(!atomic_load(&pcap_writer_stop_requested));
    fail_rx_disable=false;
    finish_writer_at=1;
    assert(pcap_capture_stop());
    assert(queue_deletes==1 && !pcap_packet_queue && !pcap_rx_configured);
    assert(atomic_load(&pcap_capture_state)==PCAP_CAPTURE_COMPLETE);

    reset_case();
    assert(!pcap_capture_stop()); /* writer did not acknowledge within timeout */
    assert(pcap_packet_queue && !queue_deletes);
    atomic_store(&pcap_writer_finished,true);
    pcap_writer_task_handle=NULL;
    atomic_store(&pcap_capture_state,PCAP_CAPTURE_FAILED);
    assert(pcap_capture_stop());
    assert(queue_deletes==1 && atomic_load(&pcap_capture_state)==PCAP_CAPTURE_FAILED);

    reset_case();
    pcap_rx_configured=pcap_filter_saved=false;
    pcap_net_hooks_installed=true;
    pcap_original_input=original_input;
    pcap_original_linkoutput=original_output;
    nif.input=hook_input;nif.linkoutput=hook_output;
    pcap_arp_active=true;
    pcap_arp_task_handle=(void *)3;
    atomic_store(&pcap_producers,1);
    assert(!pcap_capture_stop()); /* ARP worker owns its task and netif */
    assert(nif.input==original_input && nif.linkoutput==original_output);
    assert(pcap_packet_queue && !queue_deletes);
    finish_arp_at=delay_calls+1;
    finish_producer_at=delay_calls+2;
    finish_writer_at=delay_calls+3;
    assert(pcap_capture_stop());
    assert(queue_deletes==1 && !pcap_packet_queue);
    assert(!pcap_original_input && !pcap_original_linkoutput);

    puts("PASS: PCAP stop retains writer, queue and hooks on failure; retry retires cooperatively");
}
