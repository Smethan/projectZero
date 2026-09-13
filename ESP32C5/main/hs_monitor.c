/* Bounded, nonblocking observer of existing HS Capture PCAP appends. */
#include "hs_monitor.h"
#include "hs_capture.h"
#include "serial_output.h"
#include "capture_pool.h"
#include "capture_memory.h"
#include "pcap_serializer.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/usb_serial_jtag.h"
#include "driver/uart.h"
#include "sdkconfig.h"

typedef struct { int64_t at; unsigned size; uint8_t data[2304]; } frame_t;
static capture_pool_t pool;
static atomic_bool enabled, stopping, running;
static atomic_uint producers, dropped, frames;
static unsigned seq, packet;
static char session[24];
static bool serial_mode;
static int64_t now_ms(void) { return esp_timer_get_time()/1000; }

static void output(const char *body) {
    char line[1024];
    int n=snprintf(line,sizeof(line),"\nHSC:%s\n",body);
    if(n<=0 || n>=sizeof(line) || !serial_output(line,n,100))
        atomic_fetch_add(&dropped,1);
}

static void status(const char *kind) {
    char body[400];
    snprintf(body,sizeof(body),"{\"v\":1,\"kind\":\"%s\",\"session\":\"%s\",\"seq\":%u,\"storage\":\"%s\",\"wifi_count\":%u,\"ble_count\":0,\"drops\":%u}",
        kind,session,++seq,serial_mode?"serial":"sd",atomic_load(&frames),atomic_load(&dropped));
    output(body);
}
static void observe(const uint8_t *data,unsigned size) {
    atomic_fetch_add(&producers,1);
    if(!atomic_load(&enabled)) goto done;
    if(!hs_capture_kind(data,size)) goto done;
    if(size>2304) { atomic_fetch_add(&dropped,1); goto done; }
    frame_t *frame=capture_pool_acquire(&pool);
    if(!frame) { atomic_fetch_add(&dropped,1); goto done; }
    frame->at=now_ms(); frame->size=size;
    memcpy(frame->data,data,size);
    if(!capture_pool_publish(&pool,frame)) atomic_fetch_add(&dropped,1);
    else atomic_fetch_add(&frames,1);
done:
    atomic_fetch_sub(&producers,1);
}
static void emit(const frame_t *frame) {
    unsigned id=++packet;
    static const char hex[]="0123456789abcdef";
    for(unsigned offset=0;offset<frame->size;offset+=240) {
        int64_t age=now_ms()-frame->at;
        if(age>2000) { atomic_fetch_add(&dropped,1); return; }
        unsigned n=frame->size-offset<240?frame->size-offset:240;
        char data[481],body[900];
        for(unsigned i=0;i<n;i++) { unsigned b=frame->data[offset+i]; data[2*i]=hex[b>>4];data[2*i+1]=hex[b&15]; }
        data[2*n]=0;
        snprintf(body,sizeof(body),"{\"v\":1,\"kind\":\"hs_packet\",\"session\":\"%s\",\"seq\":%u,\"packet\":%u,\"offset\":%u,\"total\":%u,\"capture_ms\":%lld,\"age_ms\":%lld,\"data_hex\":\"%s\"}",
            session,++seq,id,offset,frame->size,(long long)frame->at,(long long)age,data);
        output(body);
    }
}
static void worker(void *unused) {
    status("started");
    int64_t stats=now_ms(),end=0;
    while(true) {
        if(atomic_load(&stopping) && !end) end=now_ms()+200;
        if(end && (now_ms()>=end || !uxQueueMessagesWaiting(pool.ready))) break;
        frame_t *frame;
        if(xQueueReceive(pool.ready,&frame,pdMS_TO_TICKS(10))==pdTRUE) {
            emit(frame); capture_pool_release(&pool,frame);
        }
        if(now_ms()-stats>=2000) { status("stats");stats=now_ms(); }
    }
    pcap_serializer_set_observer(NULL);
    atomic_store(&enabled,false);
    while(atomic_load(&producers)) vTaskDelay(1);
    atomic_fetch_add(&dropped,uxQueueMessagesWaiting(pool.ready));
    capture_pool_close(&pool);
    status("stopped");
    atomic_store(&running,false);
    vTaskDelete(NULL);
}
bool hsm_start(bool serial_storage) {
    if(atomic_load(&running) || atomic_load(&producers)) return false;
    capture_memory_log("hs_progress_begin");
    if(!capture_pool_open(&pool,4,sizeof(frame_t))) {
        capture_memory_log("hs_progress_allocation_failed"); return false;
    }
    serial_mode=serial_storage;seq=packet=0;
    snprintf(session,sizeof(session),"%llx",(unsigned long long)esp_timer_get_time());
    atomic_store(&dropped,0);atomic_store(&frames,0);
    atomic_store(&stopping,false);atomic_store(&running,true);
    atomic_store(&enabled,true);
    pcap_serializer_set_observer(observe);
    if(xTaskCreate(worker,"hs_progress",6144,NULL,3,NULL)!=pdPASS) {
        pcap_serializer_set_observer(NULL);
        atomic_store(&enabled,false);
        while(atomic_load(&producers)) vTaskDelay(1);
        capture_pool_close(&pool);
        atomic_store(&running,false);return false;
    }
    return true;
}
void hsm_stop(void) {
    pcap_serializer_set_observer(NULL);
    atomic_store(&enabled,false);atomic_store(&stopping,true);
    int64_t end=now_ms()+500;
    while(atomic_load(&running) && now_ms()<end) vTaskDelay(pdMS_TO_TICKS(10));
}
