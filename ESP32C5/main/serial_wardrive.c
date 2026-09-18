/* Host-owned GPS/SD-free coexistence capture. No allocations or I/O in callbacks. */
#include "serial_wardrive.h"
#include "hs_capture.h"
#include "serial_output.h"
#include "usb_ota.h"
#include "capture_pool.h"
#include "capture_memory.h"
#include "esp_heap_caps.h"
#include "linenoise/linenoise.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdatomic.h>
#include "esp_wifi.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/usb_serial_jtag.h"
#include "driver/uart.h"
#include "sdkconfig.h"
#define QLEN 64
#define CACHE 128
static atomic_bool active, stopping, collecting;
static bool passive_hs;
static bool wifi_only;
static bool batch_mode;
bool sw_hs_mode(void) { return passive_hs; }
bool sw_wifi_only_mode(void) { return wifi_only || passive_hs; }
#define HS_MAX_FRAME 2304
typedef struct {
    int64_t at;
    uint16_t len;
    uint8_t channel;
    int8_t rssi;
    uint8_t data[HS_MAX_FRAME];
} hs_observation;
static capture_pool_t hs_pool;
static unsigned packet_id;
static atomic_uint drops, wifi_count, ble_count, producers;
static atomic_llong lease;
static StaticQueue_t queue_storage;
static uint8_t queue_bytes[QLEN * 128];
static QueueHandle_t queue;
static char session[33];
static atomic_uint seq;
static atomic_uint batch_number;
static atomic_int batch_phase; /* 0 idle, 1 collecting, 2 reporting */
typedef struct {
    int64_t at;
    uint8_t kind, addr[6], rx[6], bssid[6], addr_type, event, len, data[62];
    int8_t rssi;
    uint8_t channel, ssid_present, privacy;
} observation;
_Static_assert(sizeof(observation) <= 128, "queue backing size");
static struct { uint32_t hash; int64_t at; } seen[CACHE];
typedef struct { uint32_t hash; bool used; observation value; } batch_slot;
static batch_slot *batch_slots;
static unsigned batch_capacity;
static portMUX_TYPE cache_lock = portMUX_INITIALIZER_UNLOCKED;
bool sw_active(void) { return atomic_load(&active); }
static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static unsigned next_seq(void) { return atomic_fetch_add(&seq,1)+1; }
static void hex(char *out, const uint8_t *in, size_t len) {
    static const char h[]="0123456789abcdef";
    for (size_t i=0;i<len;i++) { out[2*i]=h[in[i]>>4]; out[2*i+1]=h[in[i]&15]; }
    out[2*len]=0;
}
static void mac(char *out,const uint8_t *p) {
    snprintf(out,18,"%02X:%02X:%02X:%02X:%02X:%02X",p[0],p[1],p[2],p[3],p[4],p[5]);
}
/* The console uses this same transport. Lock stdout for an entire bounded frame.
 * A leading newline allows recovery after a partial write or console prompt. */
static void output(const char *body) {
    char line[1024];
    int n=snprintf(line,sizeof(line),"\nWDG:%s\n",body);
    if(n<=0 || n>=sizeof(line) || !serial_output(line,n,100))
        atomic_fetch_add(&drops,1);
}

/* Batch control frames are sparse and define lifecycle state.  Give them a
 * larger deadline and retry the exact same frame/sequence number. */
static bool control_output(const char *body) {
    char line[1024];
    int n=snprintf(line,sizeof(line),"\nWDG:%s\n",body);
    if(n<=0 || n>=sizeof(line)) { atomic_fetch_add(&drops,1); return false; }
    for(unsigned attempt=0;attempt<3;attempt++)
        if(serial_output(line,n,250)) return true;
    atomic_fetch_add(&drops,1);
    return false;
}

static void status(const char *kind,const char *extra) {
    char b[512];
    snprintf(b,sizeof(b),"{\"v\":1,\"kind\":\"%s\",\"session\":\"%s\",\"seq\":%u,\"uptime_ms\":%lld,\"wifi_count\":%u,\"ble_count\":%u,\"drops\":%u%s}",kind,session,next_seq(),(long long)now_ms(),atomic_load(&wifi_count),atomic_load(&ble_count),atomic_load(&drops),extra);
    output(b);
}
static void batch_status(const char *kind,const char *state,const char *extra) {
    char b[640];
    unsigned record_seq=next_seq();
    snprintf(b,sizeof(b),"{\"v\":2,\"kind\":\"%s\",\"session\":\"%s\",\"seq\":%u,\"uptime_ms\":%lld,\"state\":\"%s\",\"batch\":%u,\"wifi_count\":%u,\"ble_count\":%u,\"drops\":%u%s}",kind,session,record_seq,(long long)now_ms(),state,atomic_load(&batch_number),atomic_load(&wifi_count),atomic_load(&ble_count),atomic_load(&drops),extra);
    control_output(b);
}
static void offer(observation *o) {
    atomic_fetch_add(&producers,1);
    if(!atomic_load(&collecting)) { atomic_fetch_sub(&producers,1); return; }
    o->at=now_ms();
    /* Hash evidence, including advertisement event type: scan responses bypass
       advertisement throttling. Collisions only reduce throttling, never hide new evidence. */
    uint32_t hash=2166136261u;
    observation key=*o; key.rssi=0;
    const uint8_t *p=(const uint8_t*)&key + sizeof(o->at);
    for(size_t i=sizeof(o->at);i<sizeof(*o);i++) hash=(hash ^ *p++)*16777619u;
    unsigned slot=hash % CACHE;
    portENTER_CRITICAL(&cache_lock);
    bool repeat=seen[slot].hash==hash && o->at-seen[slot].at<1000;
    if(!repeat) { seen[slot].hash=hash; seen[slot].at=o->at; }
    portEXIT_CRITICAL(&cache_lock);
    if(!repeat && xQueueSend(queue,o,0)!=pdTRUE) atomic_fetch_add(&drops,1);
    atomic_fetch_sub(&producers,1);
}
static void wifi_cb(void *buf,wifi_promiscuous_pkt_type_t type) {
    if(type!=WIFI_PKT_MGMT || !atomic_load(&collecting)) return;
    wifi_promiscuous_pkt_t *pkt=buf;
    const uint8_t *p=pkt->payload;
    int len=pkt->rx_ctrl.sig_len-4; /* discard FCS */
    if(len<24 || (p[0]&0x0c)) return;
    int subtype=p[0]>>4;
    if(subtype!=8 && subtype!=4 && subtype!=5) return;
    int pos=subtype==4 ? 24:36;
    if(len<pos) return;
    observation o={.kind=1,.event=subtype,.channel=pkt->rx_ctrl.channel,.rssi=pkt->rx_ctrl.rssi};
    memcpy(o.addr,p+10,6); memcpy(o.rx,p+4,6); memcpy(o.bssid,p+16,6);
    if(subtype!=4) o.privacy=(p[34]&0x10)!=0;
    while(pos+2<=len) {
        unsigned tag=p[pos++], n=p[pos++];
        if(pos+n>len) return;
        if(tag==0 && n<=32) { o.ssid_present=1; o.len=n; memcpy(o.data,p+pos,n); }
        pos+=n;
    }
    atomic_fetch_add(&wifi_count,1); offer(&o);
}
void sw_ble(const uint8_t *addr,uint8_t type,int rssi,uint8_t event,const uint8_t *data,size_t len) {
    if (sw_wifi_only_mode()) return;
    observation o={.kind=2,.addr_type=type,.rssi=rssi,.event=event,.len=len>62?62:len};
    for(int i=0;i<6;i++) o.addr[i]=addr[5-i]; /* NimBLE is little-endian */
    memcpy(o.data,data,o.len); atomic_fetch_add(&ble_count,1); offer(&o);
}
static void emit_version(const observation *o,unsigned version,unsigned batch) {
    int64_t age=now_ms()-o->at;
    if(age>(version==2?20000:2000)) { atomic_fetch_add(&drops,1); return; }
    char a[18],r[18],bssid[18],bssid_json[22],data[125],b[900];
    mac(a,o->addr); mac(r,o->rx); mac(bssid,o->bssid); hex(data,o->data,o->len);
    if(o->event==4) strcpy(bssid_json,"null");
    else snprintf(bssid_json,sizeof(bssid_json),"\"%s\"",bssid);
    if(o->kind==2) {
        if(version==2)
            snprintf(b,sizeof(b),"{\"v\":2,\"kind\":\"ble\",\"session\":\"%s\",\"seq\":%u,\"batch\":%u,\"capture_ms\":%lld,\"age_ms\":%lld,\"mac\":\"%s\",\"addr_type\":%u,\"event\":%u,\"rssi\":%d,\"data_hex\":\"%s\",\"truncated\":false}",session,next_seq(),batch,(long long)o->at,(long long)age,a,o->addr_type,o->event,o->rssi,data);
        else
            snprintf(b,sizeof(b),"{\"v\":1,\"kind\":\"ble\",\"session\":\"%s\",\"seq\":%u,\"capture_ms\":%lld,\"age_ms\":%lld,\"mac\":\"%s\",\"addr_type\":%u,\"event\":%u,\"rssi\":%d,\"data_hex\":\"%s\",\"truncated\":false}",session,next_seq(),(long long)o->at,(long long)age,a,o->addr_type,o->event,o->rssi,data);
        output(b);
    } else {
        if(version==2)
            snprintf(b,sizeof(b),"{\"v\":2,\"kind\":\"wifi_mgmt\",\"session\":\"%s\",\"seq\":%u,\"batch\":%u,\"capture_ms\":%lld,\"age_ms\":%lld,\"mac\":\"%s\",\"receiver\":\"%s\",\"bssid\":%s,\"frame_type\":0,\"subtype\":%u,\"ssid_present\":%s,\"ssid_hex\":\"%s\",\"channel\":%u,\"rssi\":%d}",session,next_seq(),batch,(long long)o->at,(long long)age,a,r,bssid_json,o->event,o->ssid_present?"true":"false",data,o->channel,o->rssi);
        else
            snprintf(b,sizeof(b),"{\"v\":1,\"kind\":\"wifi_mgmt\",\"session\":\"%s\",\"seq\":%u,\"capture_ms\":%lld,\"age_ms\":%lld,\"mac\":\"%s\",\"receiver\":\"%s\",\"bssid\":%s,\"frame_type\":0,\"subtype\":%u,\"ssid_present\":%s,\"ssid_hex\":\"%s\",\"channel\":%u,\"rssi\":%d}",session,next_seq(),(long long)o->at,(long long)age,a,r,bssid_json,o->event,o->ssid_present?"true":"false",data,o->channel,o->rssi);
        output(b);
        if(o->event==8 || o->event==5) {
            if(version==2)
                snprintf(b,sizeof(b),"{\"v\":2,\"kind\":\"wifi\",\"session\":\"%s\",\"seq\":%u,\"batch\":%u,\"capture_ms\":%lld,\"age_ms\":%lld,\"mac\":\"%s\",\"ssid_hex\":\"%s\",\"channel\":%u,\"rssi\":%d,\"auth\":\"%s\"}",session,next_seq(),batch,(long long)o->at,(long long)(now_ms()-o->at),bssid,data,o->channel,o->rssi,o->privacy?"PRIVACY":"OPEN");
            else
                snprintf(b,sizeof(b),"{\"v\":1,\"kind\":\"wifi\",\"session\":\"%s\",\"seq\":%u,\"capture_ms\":%lld,\"age_ms\":%lld,\"mac\":\"%s\",\"ssid_hex\":\"%s\",\"channel\":%u,\"rssi\":%d,\"auth\":\"%s\"}",session,next_seq(),(long long)o->at,(long long)(now_ms()-o->at),bssid,data,o->channel,o->rssi,o->privacy?"PRIVACY":"OPEN");
            output(b);
        }
    }
}
static void emit(const observation *o) { emit_version(o,1,0); }

static uint32_t observation_hash(const observation *o) {
    uint32_t hash=2166136261u;
    const uint8_t *parts[]={&o->kind,o->addr,o->rx,o->bssid,&o->addr_type,
                            &o->event,&o->len,o->data,&o->channel,
                            &o->ssid_present,&o->privacy};
    const size_t sizes[]={1,6,6,6,1,1,1,o->len,1,1,1};
    for(unsigned part=0;part<sizeof(parts)/sizeof(parts[0]);part++)
        for(size_t i=0;i<sizes[part];i++) hash=(hash^parts[part][i])*16777619u;
    return hash ? hash : 1;
}
static bool observation_equal(const observation *a,const observation *b) {
    return a->kind==b->kind && !memcmp(a->addr,b->addr,6) &&
        !memcmp(a->rx,b->rx,6) && !memcmp(a->bssid,b->bssid,6) &&
        a->addr_type==b->addr_type && a->event==b->event && a->len==b->len &&
        !memcmp(a->data,b->data,a->len) && a->channel==b->channel &&
        a->ssid_present==b->ssid_present && a->privacy==b->privacy;
}
static bool batch_open(void) {
    batch_capacity=256;
    batch_slots=heap_caps_malloc(sizeof(*batch_slots)*batch_capacity,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!batch_slots) {
        batch_capacity=128;
        batch_slots=heap_caps_malloc(sizeof(*batch_slots)*batch_capacity,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    }
    if(batch_slots) memset(batch_slots,0,sizeof(*batch_slots)*batch_capacity);
    return batch_slots!=NULL;
}
static void batch_close(void) {
    heap_caps_free(batch_slots); batch_slots=NULL; batch_capacity=0;
}
static void batch_add(const observation *o) {
    uint32_t hash=observation_hash(o);
    for(unsigned probe=0;probe<batch_capacity;probe++) {
        batch_slot *slot=&batch_slots[(hash+probe)%batch_capacity];
        if(!slot->used) { slot->used=true;slot->hash=hash;slot->value=*o;return; }
        if(slot->hash==hash && observation_equal(&slot->value,o)) {
            if(o->rssi>slot->value.rssi) slot->value.rssi=o->rssi;
            slot->value.at=o->at;
            return;
        }
    }
    atomic_fetch_add(&drops,1);
}
static void hs_wifi_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (!buf ||
        (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA)) return;
    atomic_fetch_add(&producers, 1);
    if (!atomic_load(&collecting)) goto done;
    const wifi_promiscuous_pkt_t *p = buf;
    /* sig_len includes the four-byte FCS; PCAP uses plain 802.11 without FCS. */
    unsigned n = p->rx_ctrl.sig_len;
    if (n < 4) goto done;
    n -= 4;
    int kind = hs_capture_kind(p->payload, n);
    if (!kind) goto done;
    if (n > HS_MAX_FRAME) { atomic_fetch_add(&drops, 1); goto done; }
    /* Limit repeated beacons, but never suppress EAPOL or association frames. */
    unsigned subtype = p->payload[0] >> 4;
    if (kind == 1 && (subtype == 8 || subtype == 5)) {
        uint32_t hash = 2166136261u;
        /* Ignore timestamp/sequence changes; include BSSID and tagged fields. */
        for (unsigned i = 10; i < 22; i++) hash = (hash ^ p->payload[i]) * 16777619u;
        for (unsigned i = 36; i < n; i++) hash = (hash ^ p->payload[i]) * 16777619u;
        unsigned slot = hash % CACHE;
        int64_t now = now_ms();
        if (seen[slot].hash == hash && now - seen[slot].at < 10000) goto done;
        seen[slot].hash = hash; seen[slot].at = now;
    }
    hs_observation *o = capture_pool_acquire(&hs_pool);
    if (!o) { atomic_fetch_add(&drops, 1); goto done; }
    o->at=now_ms(); o->len=n; o->channel=p->rx_ctrl.channel; o->rssi=p->rx_ctrl.rssi;
    memcpy(o->data, p->payload, n);
    if (!capture_pool_publish(&hs_pool, o)) atomic_fetch_add(&drops, 1);
    else atomic_fetch_add(&wifi_count, 1);
done:
    atomic_fetch_sub(&producers, 1);
}
static void hs_emit(const hs_observation *o) {
    unsigned id = ++packet_id;
    for (unsigned offset = 0; offset < o->len; offset += 240) {
        int64_t age = now_ms() - o->at;
        if (age > 2000) { atomic_fetch_add(&drops, 1); return; }
        unsigned size = o->len-offset < 240 ? o->len-offset : 240;
        char data[481], body[900];
        hex(data, o->data+offset, size);
        snprintf(body, sizeof(body), "{\"v\":1,\"kind\":\"hs_packet\",\"session\":\"%s\",\"seq\":%u,\"packet\":%u,\"offset\":%u,\"total\":%u,\"capture_ms\":%lld,\"age_ms\":%lld,\"channel\":%u,\"rssi\":%d,\"data_hex\":\"%s\"}",
                 session, next_seq(), id, offset, o->len, (long long)o->at, (long long)age, o->channel, o->rssi, data);
        output(body);
    }
}
static void batch_worker_loop(void) {
    linenoiseSetMachineMode(true);
    atomic_store(&batch_phase,1);
    batch_status("started","running",",\"window_ms\":10000");
    int64_t heartbeat=now_ms(); unsigned hop_index=0; int64_t hop=0;
    while(!atomic_load(&stopping) && now_ms()-atomic_load(&lease)<15000) {
        unsigned current=atomic_fetch_add(&batch_number,1)+1;
        memset(batch_slots,0,sizeof(*batch_slots)*batch_capacity);
        memset(seen,0,sizeof(seen)); xQueueReset(queue);
        atomic_store(&batch_phase,1); atomic_store(&collecting,true);
        batch_status("batch_start","running",",\"window_ms\":10000");
        int64_t deadline=now_ms()+10000;
        while(!atomic_load(&stopping) && now_ms()<deadline && now_ms()-atomic_load(&lease)<15000) {
            if(now_ms()-hop>=160) { sw_radio_hop(hop_index++);hop=now_ms(); }
            observation o;
            if(xQueueReceive(queue,&o,pdMS_TO_TICKS(10))==pdTRUE) batch_add(&o);
            if(now_ms()-heartbeat>=2000) {
                batch_status("heartbeat","running",""); heartbeat=now_ms();
            }
        }
        atomic_store(&collecting,false);
        while(atomic_load(&producers) && !atomic_load(&stopping)) vTaskDelay(1);
        observation pending;
        while(xQueueReceive(queue,&pending,0)==pdTRUE) batch_add(&pending);
        if(atomic_load(&stopping) || now_ms()-atomic_load(&lease)>=15000) break;
        atomic_store(&batch_phase,2);
        unsigned wifi=0,ble=0;
        for(unsigned i=0;i<batch_capacity;i++) if(batch_slots[i].used) {
            if(batch_slots[i].value.kind==2) ble++; else wifi++;
        }
        char extra[128];
        snprintf(extra,sizeof(extra),",\"batch_wifi\":%u,\"batch_ble\":%u,\"capacity\":%u",wifi,ble,batch_capacity);
        batch_status("batch_results","reporting",extra);
        int64_t report_deadline=now_ms()+5000;
        unsigned sent_wifi=0,sent_ble=0;
        for(unsigned i=0;i<batch_capacity && !atomic_load(&stopping);i++) {
            if(!batch_slots[i].used) continue;
            if(now_ms()>=report_deadline) { atomic_fetch_add(&drops,1);continue; }
            emit_version(&batch_slots[i].value,2,current);
            if(batch_slots[i].value.kind==2) sent_ble++; else sent_wifi++;
            if(now_ms()-heartbeat>=2000) {
                batch_status("heartbeat","reporting",""); heartbeat=now_ms();
            }
        }
        if(atomic_load(&stopping)) break;
        snprintf(extra,sizeof(extra),",\"batch_wifi\":%u,\"batch_ble\":%u",sent_wifi,sent_ble);
        batch_status("batch_done","running",extra);
    }
}
static void worker(void *unused) {
    bool ready=sw_radio_start();
    wifi_promiscuous_filter_t filter={.filter_mask=WIFI_PROMIS_FILTER_MASK_MGMT | (passive_hs ? WIFI_PROMIS_FILTER_MASK_DATA : 0)};
    if(ready) ready=esp_wifi_set_promiscuous_filter(&filter)==ESP_OK && esp_wifi_set_promiscuous_rx_cb(passive_hs ? hs_wifi_cb : wifi_cb)==ESP_OK && esp_wifi_set_promiscuous(true)==ESP_OK;
    if(ready && !atomic_load(&stopping)) {
        capture_memory_log("serial_capture_ready");
        if(batch_mode) batch_worker_loop();
        else {
            atomic_store(&collecting,true); status("started","");
            int64_t stats=now_ms(),hop=0; unsigned index=0;
            while(!atomic_load(&stopping) && now_ms()-atomic_load(&lease)<15000) {
                if(now_ms()-hop>=160) { sw_radio_hop(index++); hop=now_ms(); }
                if (passive_hs) {
                    hs_observation *o;
                    if(xQueueReceive(hs_pool.ready,&o,pdMS_TO_TICKS(10))==pdTRUE) {
                        hs_emit(o); capture_pool_release(&hs_pool, o);
                    }
                } else {
                    observation o;
                    if(xQueueReceive(queue,&o,pdMS_TO_TICKS(10))==pdTRUE) emit(&o);
                }
                if(now_ms()-stats>=2000) { status("stats",""); stats=now_ms(); }
            }
        }
    } else if(!ready) {
        if(batch_mode) batch_status("error","error",",\"message\":\"radio_start_failed\"");
        else status("error",",\"message\":\"radio_start_failed\"");
    }
    atomic_store(&collecting,false);
    sw_radio_stop();
    /* Radio callbacks never wait, so outstanding copies finish promptly. */
    while(atomic_load(&producers)) vTaskDelay(1);
    if (passive_hs) {
        int64_t deadline = now_ms() + 1500;
        hs_observation *o;
        while (now_ms() < deadline && xQueueReceive(hs_pool.ready, &o, 0) == pdTRUE) {
            hs_emit(o); capture_pool_release(&hs_pool, o);
        }
        atomic_fetch_add(&drops, uxQueueMessagesWaiting(hs_pool.ready));
        capture_pool_close(&hs_pool);
    } else {
        unsigned discarded=uxQueueMessagesWaiting(queue);
        atomic_fetch_add(&drops,discarded); xQueueReset(queue);
    }
    if(batch_mode) {
        atomic_store(&batch_phase,0);
        batch_status("stopped","stopped","");
        linenoiseSetMachineMode(false);
        batch_close();
    } else status("stopped","");
    atomic_store(&active,false);
    vTaskDelete(NULL);
}
bool sw_stop(void) {
    atomic_store(&stopping,true);
    int64_t deadline=now_ms()+4000;
    while(sw_active() && now_ms()<deadline) vTaskDelay(pdMS_TO_TICKS(10));
    return !sw_active();
}
static int start(int argc,char **argv) {
    if(argc!=2 || !argv[1][0] || strlen(argv[1])>32) return 1;
    for(char *p=argv[1];*p;p++) if(!isalnum((unsigned char)*p) && *p!='-' && *p!='_') return 1;
    if(sw_active()) return 1;
    passive_hs = !strcmp(argv[0], "start_hs_sniff_serial");
    batch_mode = strstr(argv[0],"_batch_serial")!=NULL;
    wifi_only = !strcmp(argv[0], "start_wardrive_wifi_serial") || !strcmp(argv[0], "start_wardrive_wifi_batch_serial");
    if(!queue) queue=xQueueCreateStatic(QLEN,sizeof(observation),queue_bytes,&queue_storage);
    xQueueReset(queue); memset(seen,0,sizeof(seen));
    strcpy(session,argv[1]); atomic_store(&seq,0); atomic_store(&batch_number,0); atomic_store(&batch_phase,0); packet_id=0;
    atomic_store(&drops,0); atomic_store(&wifi_count,0); atomic_store(&ble_count,0);
    if(!sw_prepare()) {
        if(batch_mode) batch_status("error","error",",\"message\":\"radio_prepare_failed\"");
        else status("error",",\"message\":\"radio_prepare_failed\"");
        return 1;
    }
    capture_memory_log("serial_capture_begin");
    if (passive_hs && !capture_pool_open(&hs_pool, 8, sizeof(hs_observation))) {
        capture_memory_log("serial_capture_allocation_failed");
        status("error",",\"message\":\"capture_allocation_failed\""); return 1;
    }
    if(batch_mode && !batch_open()) {
        capture_memory_log("serial_batch_allocation_failed");
        batch_status("error","error",",\"message\":\"batch_allocation_failed\""); return 1;
    }
    atomic_store(&lease,now_ms()); atomic_store(&stopping,false); atomic_store(&active,true);
    if(xTaskCreate(worker,"serial_wardrive",6144,NULL,4,NULL)!=pdPASS) {
        capture_pool_close(&hs_pool);
        batch_close();
        capture_memory_log("serial_task_allocation_failed");
        if(batch_mode) batch_status("error","error",",\"message\":\"task_allocation_failed\"");
        else status("error",",\"message\":\"task_allocation_failed\"");
        atomic_store(&active,false); return 1;
    }
    return 0;
}
static int keepalive(int argc,char **argv) {
    if(argc!=2 || !sw_active() || strcmp(argv[1],session)) return 1;
    atomic_store(&lease,now_ms()); return 0;
}
static int wardrive_status(int argc,char **argv) {
    if(argc!=2 || !session[0] || strcmp(argv[1],session)) return 1;
    const char *state=!batch_mode?"legacy":!sw_active()?"stopped":atomic_load(&batch_phase)==2?"reporting":"running";
    batch_status("status",state,"");
    return 0;
}
static int capabilities(int argc,char **argv) {
    output("{\"v\":1,\"kind\":\"capabilities\",\"wardrive_serial_v1\":true,\"wardrive_wifi_serial_v1\":true,\"wardrive_batch_serial_v2\":true,\"wardrive_wifi_batch_serial_v2\":true,\"batch_window_ms\":10000,\"heartbeat_ms\":2000,\"max_age_ms\":20000,\"hs_sniff_serial_v1\":true,\"hs_capture_targets_v1\":true,\"hs_capture_exclusions_v1\":true,\"bands\":[\"wifi24\",\"wifi5\",\"ble\"],\"wifi_mgmt\":true,\"ble_raw_ad\":true,\"ble_extended\":false,\"max_line\":1024}"); return 0;
}
/* All main console commands share an ownership gate, including attack commands.
 * Registration retains the original handlers and changes no idle behavior. */
static struct { const char *name; esp_console_cmd_func_t func; } commands[256];
static unsigned command_count;
static int dispatch(int argc,char **argv) {
    if(uota_busy() && strncmp(argv[0],"uota_",5) && strcmp(argv[0],"version") && strcmp(argv[0],"ota_info") && strcmp(argv[0],"get_capabilities")) {
        printf("USB OTA is active; finish or abort it first.\n");return 1;
    }
    if(argc<1) return 1;
    if(sw_active() && strcmp(argv[0],"stop") && strcmp(argv[0],"get_capabilities") && strcmp(argv[0],"wardrive_keepalive") && strcmp(argv[0],"wardrive_status")) {
        printf("Serial wardrive busy; stop first.\n"); return 1;
    }
    for(unsigned i=0;i<command_count;i++) if(!strcmp(argv[0],commands[i].name)) return commands[i].func(argc,argv);
    return 1;
}
esp_err_t sw_register_command(const esp_console_cmd_t *cmd) {
    if(command_count>=256 || !cmd->func) return ESP_ERR_NO_MEM;
    esp_console_cmd_t copy=*cmd; copy.func=dispatch;
    esp_err_t rc=esp_console_cmd_register(&copy);
    if(rc==ESP_OK) { commands[command_count].name=cmd->command; commands[command_count++].func=cmd->func; }
    return rc;
}
void sw_register(void) {
    const esp_console_cmd_t cmds[]={
        {.command="start_wardrive_serial",.help="WiFi + BLE over serial, no GPS/SD: <session>",.func=start},
        {.command="start_wardrive_wifi_serial",.help="WiFi only over serial for host BLE, no GPS/SD: <session>",.func=start},
        {.command="start_wardrive_batch_serial",.help="10s batched WiFi + BLE over serial: <session>",.func=start},
        {.command="start_wardrive_wifi_batch_serial",.help="10s batched WiFi only for host BLE: <session>",.func=start},
        {.command="start_hs_sniff_serial",.help="Passive EAPOL/PMKID and management PCAP over serial, no SD: <session>",.func=start},
        {.command="wardrive_keepalive",.help="Renew serial session lease: <session>",.func=keepalive},
        {.command="wardrive_status",.help="Repeat current batched wardrive state: <session>",.func=wardrive_status},
        {.command="get_capabilities",.help="Machine-readable serial capabilities",.func=capabilities}};
    for(unsigned i=0;i<sizeof(cmds)/sizeof(cmds[0]);i++) ESP_ERROR_CHECK(sw_register_command(&cmds[i]));
}
