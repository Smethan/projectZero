/* Exercise the actual Wi-Fi callback with a saturated fixed frame pool. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

typedef int wifi_promiscuous_pkt_type_t;
#define WIFI_PKT_MGMT 0
#define WIFI_PKT_DATA 1
#define pdTRUE 1
#define pdFALSE 0
typedef struct {
    struct { unsigned sig_len; int8_t rssi; uint8_t channel; } rx_ctrl;
    uint8_t payload[2305];
} wifi_promiscuous_pkt_t;
typedef struct { unsigned queued; uint8_t slots[8]; bool fail_send; } queue_t;
typedef queue_t *QueueHandle_t;
static queue_t queue;
static QueueHandle_t sniffer_frame_queue=&queue;
static bool sniffer_active=true,sniffer_scan_phase;
static atomic_uint sniffer_producers,sniffer_drop_count,sniffer_packet_counter;
static unsigned saturations;
static void sw_job_note_saturation(void) { saturations++; }
static int xQueueSend(QueueHandle_t q,const void *item,unsigned delay) {
    (void)delay;
    if(q->fail_send || q->queued>=8)return pdFALSE;
    q->slots[q->queued++]=*(const uint8_t *)item;
    return pdTRUE;
}

/* Extracted production frame layout and callback, with bounded RTOS shims. */
#include "sniffer_frame_type.inc"
static sniffer_frame_t *sniffer_frames;
static atomic_bool sniffer_slot_busy[SNIFFER_FRAME_SLOTS];
#include "sniffer_callback_under_test.inc"

int main(void) {
    static sniffer_frame_t storage[SNIFFER_FRAME_SLOTS];
    sniffer_frames=storage;
    wifi_promiscuous_pkt_t frame={.rx_ctrl={.sig_len=64,.rssi=-57,.channel=6}};
    for(unsigned i=0;i<64;i++)frame.payload[i]=(uint8_t)i;
    for(unsigned i=0;i<SNIFFER_FRAME_SLOTS;i++)
        sniffer_promiscuous_callback(&frame,WIFI_PKT_MGMT);
    assert(queue.queued==SNIFFER_FRAME_SLOTS);
    assert(atomic_load(&sniffer_packet_counter)==SNIFFER_FRAME_SLOTS);
    for(unsigned i=0;i<SNIFFER_FRAME_SLOTS;i++) {
        assert(atomic_load(&sniffer_slot_busy[i]));
        assert(storage[i].len==64 && storage[i].rssi==-57 && storage[i].channel==6);
        assert(!memcmp(storage[i].payload,frame.payload,64));
    }
    sniffer_promiscuous_callback(&frame,WIFI_PKT_DATA);
    assert(atomic_load(&sniffer_drop_count)==1 && saturations==1);
    assert(atomic_load(&sniffer_producers)==0);

    /* Simulate one worker retirement and verify that its slot can be reused. */
    queue.queued=7;
    atomic_store(&sniffer_slot_busy[0],false);
    sniffer_promiscuous_callback(&frame,WIFI_PKT_MGMT);
    assert(queue.queued==8 && queue.slots[7]==0);
    queue.queued=7;queue.fail_send=true;
    atomic_store(&sniffer_slot_busy[1],false);
    sniffer_promiscuous_callback(&frame,WIFI_PKT_MGMT);
    assert(!atomic_load(&sniffer_slot_busy[1]));
    assert(atomic_load(&sniffer_drop_count)==2 && saturations==2);

    frame.rx_ctrl.sig_len=2305;
    sniffer_promiscuous_callback(&frame,WIFI_PKT_MGMT);
    assert(atomic_load(&sniffer_drop_count)==3);
    unsigned packets=atomic_load(&sniffer_packet_counter);
    sniffer_active=false;
    sniffer_promiscuous_callback(&frame,WIFI_PKT_MGMT);
    assert(atomic_load(&sniffer_packet_counter)==packets &&
           atomic_load(&sniffer_producers)==0);
    puts("PASS: managed Sniffer callback uses fixed slots, zero-wait enqueue and bounded overflow");
}
