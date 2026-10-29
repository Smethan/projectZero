/* Host-owned GPS/SD-free coexistence capture. No allocations or I/O in callbacks. */
#include "serial_wardrive.h"
#include "hs_capture.h"
#include "serial_output.h"
#include "usb_ota.h"
#include "capture_pool.h"
#include "capture_memory.h"
#include "esp_heap_caps.h"
#include "linenoise/linenoise.h"
/* Supplied by the pinned-IDF patch in tools/patch_idf_linenoise_machine.cmake.
 * Some incremental SDK include trees expose the stock header during compile. */
extern void linenoiseSetMachineMode(bool enabled);
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdatomic.h>
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "esp_app_desc.h"
#include "cJSON.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/usb_serial_jtag.h"
#include "driver/uart.h"
#include "sdkconfig.h"
#if CONFIG_PROJECTZERO_BOARD_WAVESHARE_ZERO
#define ZERO_STACK_NOINLINE __attribute__((noinline))
#else
#define ZERO_STACK_NOINLINE
#endif
#define QLEN 64
#define CACHE 128
#define LIVE_OUI_MAX 8
#define LIVE_OUI_IDENTITIES 32
#define RID_WIFI_MAX 228
#define RID_WIFI_QUEUE_LEN 4
static uint8_t live_ouis[LIVE_OUI_MAX][3];
static unsigned live_oui_count, live_identity_count;
static struct { uint8_t kind, addr_type, addr[6]; } live_identities[LIVE_OUI_IDENTITIES];
static atomic_bool active, stopping, collecting;
static bool passive_hs;
static bool wifi_only;
static bool batch_mode;
bool sw_hs_mode(void) { return passive_hs; }
#define HS_MAX_FRAME 2304
#define ZERO_JOB_WORKING_SET_BUDGET (48u * 1024u)
#define ZERO_JOB_INTERNAL_RESERVE (32u * 1024u)
static bool job_mode, in_job_dispatch, collect_wifi = true, collect_ble = true;
static atomic_bool external_job_active;
static atomic_bool external_job_ready;
static atomic_bool external_operation_ready;
static atomic_bool external_job_starting;
static atomic_bool external_job_stopping;
static atomic_bool external_stop_completed;
static atomic_bool radio_ready;
static atomic_bool external_stop_claimed;
static TaskHandle_t external_supervisor_task;
static sw_radio_busy_probe_t radio_busy_probe;
static char active_job_operation[24];
static atomic_uint sniffer_packets, sniffer_aps, sniffer_drops;
void sw_job_set_sniffer_stats(uint32_t packets,uint32_t aps,uint32_t drops) {
    atomic_store(&sniffer_packets,packets);
    atomic_store(&sniffer_aps,aps);
    atomic_store(&sniffer_drops,drops);
}
static char device_id[13], boot_id[17], job_token[33];
enum { BOOT_READY, BOOT_INITIALIZING, BOOT_RECOVERY };
enum { REASON_NORMAL, REASON_INITIALIZING, REASON_PSRAM_BUFFER_FAILED,
       REASON_NVS_INIT_FAILED, REASON_PSRAM_INIT_FAILED,
       REASON_PSRAM_UNAVAILABLE, REASON_UNKNOWN };
#define BOOT_STATE_BITS 2u
#define BOOT_OTA_BIT (1u << BOOT_STATE_BITS)
#define BOOT_REASON_SHIFT (BOOT_STATE_BITS + 1u)
static atomic_uint boot_state_snapshot = ATOMIC_VAR_INIT(BOOT_READY | (REASON_NORMAL << BOOT_REASON_SHIFT));
typedef struct {
    unsigned state;
    unsigned reason;
    bool ota_available;
} sw_boot_snapshot_t;
static sw_boot_snapshot_t sw_get_boot_snapshot(void) {
    unsigned packed = atomic_load(&boot_state_snapshot);
    return (sw_boot_snapshot_t){
        .state = packed & ((1u << BOOT_STATE_BITS) - 1u),
        .reason = packed >> BOOT_REASON_SHIFT,
        .ota_available = (packed & BOOT_OTA_BIT) != 0,
    };
}
static const char *sw_boot_reason_name(unsigned reason) {
    switch(reason) {
        case REASON_NORMAL: return "normal";
        case REASON_INITIALIZING: return "initializing";
        case REASON_PSRAM_BUFFER_FAILED: return "psram_buffer_failed";
        case REASON_NVS_INIT_FAILED: return "nvs_init_failed";
        case REASON_PSRAM_INIT_FAILED: return "psram_init_failed";
        case REASON_PSRAM_UNAVAILABLE: return "psram_unavailable";
        default: return "unknown";
    }
}
void sw_set_boot_state(bool ready, const char *reason, bool ota_available) {
    unsigned reason_code = REASON_UNKNOWN;
    if(reason && !strcmp(reason,"normal")) reason_code=REASON_NORMAL;
    else if(reason && !strcmp(reason,"initializing")) reason_code=REASON_INITIALIZING;
    else if(reason && !strcmp(reason,"psram_buffer_failed")) reason_code=REASON_PSRAM_BUFFER_FAILED;
    else if(reason && !strcmp(reason,"nvs_init_failed")) reason_code=REASON_NVS_INIT_FAILED;
    else if(reason && !strcmp(reason,"psram_init_failed")) reason_code=REASON_PSRAM_INIT_FAILED;
    else if(reason && !strcmp(reason,"psram_unavailable")) reason_code=REASON_PSRAM_UNAVAILABLE;
    unsigned state = ready ? BOOT_READY : (reason_code==REASON_INITIALIZING ? BOOT_INITIALIZING : BOOT_RECOVERY);
    unsigned packed = state | (reason_code << BOOT_REASON_SHIFT) | (ota_available ? BOOT_OTA_BIT : 0u);
    atomic_store(&boot_state_snapshot, packed);
}
static uint32_t job_request_hash;
static uint8_t job_channels[40];
static unsigned job_channel_count;
static unsigned job_dwell_ms = 160;
#define JOB_TARGET_MAX 16
#define JOB_EXCLUSION_MAX 32
static uint8_t job_targets[JOB_TARGET_MAX][6],job_exclusions[JOB_EXCLUSION_MAX][6];
static unsigned job_target_count,job_exclusion_count;
static bool job_filter_bssid(const uint8_t *bssid) {
    if(!bssid)return false;
    for(unsigned i=0;i<job_exclusion_count;i++)if(!memcmp(bssid,job_exclusions[i],6))return false;
    if(job_target_count) {
        for(unsigned i=0;i<job_target_count;i++)if(!memcmp(bssid,job_targets[i],6))return true;
        return false;
    }
    return true;
}
static struct { bool used; char token[33]; uint32_t hash; } terminal_jobs[4];
static unsigned terminal_job_next;
static void remember_terminal_job(const char *token,uint32_t hash);
static struct { char operation[24]; sw_job_operation_handler_t handler; sw_job_operation_stop_t stop; } job_handlers[12];
static unsigned job_handler_count;
static void external_job_supervisor(void *arg);
static bool wait_external_supervisor_retirement(unsigned timeout_ms) {
    for(unsigned waited=0;external_supervisor_task && waited<timeout_ms;waited+=20)
        vTaskDelay(pdMS_TO_TICKS(20));
    return external_supervisor_task==NULL;
}
bool sw_wifi_only_mode(void) { return wifi_only || passive_hs || (job_mode && !collect_ble); }
bool sw_ble_only_mode(void) { return job_mode && !collect_wifi && collect_ble; }
static bool valid_channel(unsigned channel);
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
static atomic_uint ble_callbacks;
static atomic_bool ble_accepting;
static atomic_uint loss_queue, loss_age, loss_output, loss_saturation;
static atomic_llong lease;
static StaticQueue_t queue_storage;
static uint8_t queue_bytes[QLEN * 128];
static QueueHandle_t queue;
typedef struct {
    int64_t at;
    uint16_t len;
    uint8_t channel, transport;
    int8_t rssi;
    uint8_t mac[6];
    bool truncated;
    uint8_t data[RID_WIFI_MAX];
} rid_wifi_observation;
static StaticQueue_t rid_wifi_queue_storage;
static uint8_t rid_wifi_queue_bytes[RID_WIFI_QUEUE_LEN * sizeof(rid_wifi_observation)];
static QueueHandle_t rid_wifi_queue;
static rid_wifi_observation rid_wifi_staging;
static rid_wifi_observation rid_wifi_worker_scratch;
static atomic_flag rid_wifi_staging_busy = ATOMIC_FLAG_INIT;
static char session[33];
static atomic_uint seq;
static atomic_bool sequence_exhausted;
static atomic_uint batch_number;
static atomic_int batch_phase; /* 0 idle, 1 collecting, 2 reporting */
static const char *job_current_state(void) {
    if(atomic_load(&external_job_starting))return "starting";
    if(atomic_load(&external_stop_completed))return "stopping";
    if(atomic_load(&external_job_active))return atomic_load(&external_job_stopping)?"stopping":"running";
    if(!atomic_load(&active))return "stopped";
    if(atomic_load(&stopping))return "stopping";
    if(!atomic_load(&radio_ready))return "starting";
    return atomic_load(&batch_phase)==2?"reporting":"running";
}
static void note_loss(atomic_uint *category, unsigned amount) {
    if(!amount)return;
    atomic_fetch_add(category,amount);
    atomic_fetch_add(&drops,amount);
}
static void reset_losses(void) {
    atomic_store(&loss_queue,0); atomic_store(&loss_age,0);
    atomic_store(&loss_output,0); atomic_store(&loss_saturation,0);
}
void sw_job_note_saturation(void) {
    if(job_mode) note_loss(&loss_saturation,1);
}
typedef struct {
    int64_t at;
    uint8_t kind, addr[6], rx[6], bssid[6], addr_type, event, len, data[62];
    int8_t rssi;
    uint8_t channel, ssid_present, privacy, authmode;
} observation;
_Static_assert(sizeof(observation) <= 128, "queue backing size");
static const char *wifi_auth_label(const observation *o);
static struct { uint32_t hash; int64_t at; } seen[CACHE];
typedef struct { uint32_t hash; bool used; observation value; } batch_slot;
static batch_slot *batch_slots;
static unsigned batch_capacity;
static portMUX_TYPE cache_lock = portMUX_INITIALIZER_UNLOCKED;
bool sw_active(void) { return atomic_load(&active) || atomic_load(&external_job_active); }
static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static unsigned next_seq(void) {
    unsigned current=atomic_load(&seq);
    for(;;) {
        if(current>=UINT32_MAX-1u) {
            atomic_store(&seq,UINT32_MAX);
            atomic_store(&sequence_exhausted,true);
            atomic_store(&stopping,true);
            atomic_store(&lease,now_ms()-15000);
            return UINT32_MAX;
        }
        if(atomic_compare_exchange_weak(&seq,&current,current+1u))return current+1u;
    }
}
static void hex(char *out, const uint8_t *in, size_t len) {
    static const char h[]="0123456789abcdef";
    for (size_t i=0;i<len;i++) { out[2*i]=h[in[i]>>4]; out[2*i+1]=h[in[i]&15]; }
    out[2*len]=0;
}
static void mac(char *out,const uint8_t *p) {
    snprintf(out,18,"%02X:%02X:%02X:%02X:%02X:%02X",p[0],p[1],p[2],p[3],p[4],p[5]);
}
static bool serial_record_write(char *line,int n,bool reliable) {
    if(n<=0 || n>=1024) return false;
    if(!reliable) return serial_output(line,n,100);
    for(unsigned attempt=0;attempt<3;attempt++)
        if(serial_output(line,n,250)) return true;
    return false;
}
/* The console uses this same transport. Lock stdout for an entire bounded frame.
 * A leading newline allows recovery after a partial write or console prompt. */
static void output(const char *body) {
    char line[1024];
    int n;
    if(job_mode) {
        const char *version=strstr(body,"\"device_id\"")&&strstr(body,"\"boot_id\"")?NULL:strchr(body,',');
        if(!version && !(strstr(body,"\"device_id\"")&&strstr(body,"\"boot_id\""))) {
            note_loss(&loss_output,1); return;
        }
        if(!version) n=snprintf(line,sizeof(line),"\nWDG:%s\n",body);
        else n=snprintf(line,sizeof(line),"\nWDG:{\"v\":3,\"device_id\":\"%s\",\"boot_id\":\"%s\",%s\n",
                        device_id,boot_id,version+1);
    } else {
        n=snprintf(line,sizeof(line),"\nWDG:%s\n",body);
    }
    if(!serial_record_write(line,n,false))
        note_loss(&loss_output,1);
}

/* Batch control frames are sparse and define lifecycle state.  Give them a
 * larger deadline and retry the exact same frame/sequence number. */
static bool control_output(const char *body) {
    char line[1024];
    int n;
    if(job_mode) {
        const char *version=strstr(body,"\"device_id\"")&&strstr(body,"\"boot_id\"")?NULL:strchr(body,',');
        if(!version && !(strstr(body,"\"device_id\"")&&strstr(body,"\"boot_id\""))) {
            note_loss(&loss_output,1); return false;
        }
        if(!version) n=snprintf(line,sizeof(line),"\nWDG:%s\n",body);
        else n=snprintf(line,sizeof(line),"\nWDG:{\"v\":3,\"device_id\":\"%s\",\"boot_id\":\"%s\",%s\n",
                        device_id,boot_id,version+1);
    } else {
        n=snprintf(line,sizeof(line),"\nWDG:%s\n",body);
    }
    if(!serial_record_write(line,n,true)) { note_loss(&loss_output,1); return false; }
    return true;
}

static ZERO_STACK_NOINLINE void status(const char *kind,const char *extra) {
    char b[512];
    snprintf(b,sizeof(b),"{\"v\":1,\"kind\":\"%s\",\"session\":\"%s\",\"seq\":%u,\"uptime_ms\":%lld,\"wifi_count\":%u,\"ble_count\":%u,\"drops\":%u%s}",kind,session,next_seq(),(long long)now_ms(),atomic_load(&wifi_count),atomic_load(&ble_count),atomic_load(&drops),extra);
    output(b);
}
static ZERO_STACK_NOINLINE void batch_status(const char *kind,const char *state,const char *extra) {
    if(job_mode) {
        sw_job_emit_lifecycle(kind,state,extra&&*extra==','?extra+1:extra);
        return;
    }
    char b[640];
    unsigned record_seq=next_seq();
    snprintf(b,sizeof(b),"{\"v\":2,\"kind\":\"%s\",\"session\":\"%s\",\"seq\":%u,\"uptime_ms\":%lld,\"state\":\"%s\",\"batch\":%u,\"wifi_count\":%u,\"ble_count\":%u,\"drops\":%u%s}",kind,session,record_seq,(long long)now_ms(),state,atomic_load(&batch_number),atomic_load(&wifi_count),atomic_load(&ble_count),atomic_load(&drops),extra);
    control_output(b);
}
static void offer(observation *o) {
    atomic_fetch_add(&producers,1);
    if(!atomic_load(&collecting)) { atomic_fetch_sub(&producers,1); return; }
    const uint8_t *identity=o->kind==1?o->bssid:o->addr;
    if(o->kind==1&&!job_filter_bssid(identity)) { atomic_fetch_sub(&producers,1);return; }
    if(o->kind==2)for(unsigned i=0;i<job_exclusion_count;i++)if(!memcmp(identity,job_exclusions[i],6)) { atomic_fetch_sub(&producers,1);return; }
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
    if(!repeat && xQueueSend(queue,o,0)!=pdTRUE) note_loss(&loss_queue,1);
    atomic_fetch_sub(&producers,1);
}
static void wifi_cb(void *buf,wifi_promiscuous_pkt_type_t type) {
    if(!collect_wifi || type!=WIFI_PKT_MGMT || !atomic_load(&collecting)) return;
    wifi_promiscuous_pkt_t *pkt=buf;
    const uint8_t *p=pkt->payload;
    int len=pkt->rx_ctrl.sig_len-4; /* discard FCS */
    if(len<24 || (p[0]&0x0c)) return;
    int subtype=p[0]>>4;
    if(subtype==13) {
        /* Public action / NAN service discovery; capture remains passive. */
        if(len>=30 && p[24]==4 && p[25]==9 && p[26]==0x50 && p[27]==0x6f &&
           p[28]==0x9a && p[29]==0x13) {
            int pos=30;
            while(pos+3<=len) {
                unsigned attr=p[pos];
                unsigned attr_len=(unsigned)p[pos+1]|((unsigned)p[pos+2]<<8);
                pos+=3;
                if(attr_len>(unsigned)(len-pos)) return;
                if(attr==3 && attr_len>=10 &&
                   p[pos]==0x88 && p[pos+1]==0x69 && p[pos+2]==0x19 &&
                   p[pos+3]==0x9d && p[pos+4]==0x92 && p[pos+5]==0x09) {
                    /* Descriptor: service id, instance/requestor IDs, control,
                     * service-info length, then counter and OpenDroneID pack. */
                    unsigned service_info_len=p[pos+9];
                    if(attr_len!=10u+service_info_len || service_info_len<5u) return;
                    unsigned pack_len=service_info_len-1u;
                    const uint8_t *pack=p+pos+11u; /* skip service-info counter */
                    unsigned messages=pack_len>=3?pack[2]:0;
                    if(pack_len<3 || pack[1]!=25 || messages<1 || messages>9 ||
                       pack_len!=3u+messages*25u) return;
                    /* Join the batch's in-flight producer barrier.  Recheck
                     * collecting after joining so a late parser cannot put
                     * this frame into the next batch. */
                    atomic_fetch_add(&producers,1);
                    if(atomic_load(&collecting) &&
                       !atomic_flag_test_and_set(&rid_wifi_staging_busy)) {
                        rid_wifi_observation *r=&rid_wifi_staging;
                        memset(r,0,sizeof(*r));
                        r->at=now_ms(); r->channel=pkt->rx_ctrl.channel;
                        r->rssi=pkt->rx_ctrl.rssi; r->transport=1; memcpy(r->mac,p+10,6);
                        r->len=pack_len>RID_WIFI_MAX?RID_WIFI_MAX:(uint16_t)pack_len;
                        r->truncated=pack_len>RID_WIFI_MAX;
                        memcpy(r->data,pack,r->len);
                        if(!rid_wifi_queue || xQueueSend(rid_wifi_queue,r,0)!=pdTRUE)
                            note_loss(&loss_queue,1);
                        atomic_flag_clear(&rid_wifi_staging_busy);
                    }
                    atomic_fetch_sub(&producers,1);
                    return;
                }
                pos+=(int)attr_len;
            }
        }
        return;
    }
    if(subtype!=8 && subtype!=4 && subtype!=5) return;
    int pos=subtype==4 ? 24:36;
    if(len<pos) return;
    observation o={.kind=1,.event=subtype,.channel=pkt->rx_ctrl.channel,.rssi=pkt->rx_ctrl.rssi};
    memcpy(o.addr,p+10,6); memcpy(o.rx,p+4,6); memcpy(o.bssid,p+16,6);
    if(subtype!=4) {
        o.privacy=(p[34]&0x10)!=0;
        o.authmode=o.privacy?255:0;
    }
    while(pos+2<=len) {
        unsigned tag=p[pos++], n=p[pos++];
        if(pos+n>len) return;
        if(tag==0 && n<=32) { o.ssid_present=1; o.len=n; memcpy(o.data,p+pos,n); }
        else if(subtype==8 && tag==221 && n>=8 && p[pos]==0xfa && p[pos+1]==0x0b &&
                p[pos+2]==0xbc && p[pos+3]==0x0d) {
            /* OpenDroneID Beacon IE: OUI/type, counter, then message pack. */
            unsigned pack_len=n-5u;
            const uint8_t *pack=p+pos+5u;
            unsigned messages=pack_len>=3?pack[2]:0;
            if(pack_len>=3 && pack[1]==25 && messages>=1 && messages<=9 &&
               pack_len==3u+messages*25u) {
                atomic_fetch_add(&producers,1);
                if(atomic_load(&collecting) &&
                   !atomic_flag_test_and_set(&rid_wifi_staging_busy)) {
                    rid_wifi_observation *r=&rid_wifi_staging;
                    memset(r,0,sizeof(*r));
                    r->at=now_ms(); r->channel=pkt->rx_ctrl.channel;
                    r->rssi=pkt->rx_ctrl.rssi; r->transport=0; memcpy(r->mac,p+10,6);
                    r->len=pack_len>RID_WIFI_MAX?RID_WIFI_MAX:(uint16_t)pack_len;
                    r->truncated=pack_len>RID_WIFI_MAX;
                    memcpy(r->data,pack,r->len);
                    if(!rid_wifi_queue || xQueueSend(rid_wifi_queue,r,0)!=pdTRUE)
                        note_loss(&loss_queue,1);
                    atomic_flag_clear(&rid_wifi_staging_busy);
                }
                atomic_fetch_sub(&producers,1);
            }
        }
        else if(tag==48 && subtype!=4 && o.privacy) {
            const uint8_t *rsn=p+pos; size_t left=n;
            if(left>=8 && rsn[0]==1 && rsn[1]==0) {
                size_t at=2+4;
                unsigned pair_count=(unsigned)rsn[at]|((unsigned)rsn[at+1]<<8); at+=2;
                if(pair_count<=(left-at)/4) {
                    at+=(size_t)pair_count*4;
                    if(left-at>=2) {
                        unsigned akm_count=(unsigned)rsn[at]|((unsigned)rsn[at+1]<<8); at+=2;
                        if(akm_count<=(left-at)/4) {
                            bool psk=false,enterprise=false,sae=false,owe=false;
                            for(unsigned suite=0;suite<akm_count;suite++,at+=4) {
                                if(rsn[at]!=0x00||rsn[at+1]!=0x0f||rsn[at+2]!=0xac) continue;
                                switch(rsn[at+3]) {
                                    case 1: enterprise=true; break;
                                    case 2: psk=true; break;
                                    case 8: case 9: sae=true; break;
                                    case 18: owe=true; break;
                                    default: break;
                                }
                            }
                            if(sae) o.authmode=psk?7:6;
                            else if(psk) o.authmode=3;
                            else if(enterprise) o.authmode=5;
                            else if(owe) o.authmode=9;
                        }
                    }
                }
            }
        }
        pos+=n;
    }
    atomic_fetch_add(&wifi_count,1); offer(&o);
}
static const char *wifi_auth_label(const observation *o) {
    switch(o->authmode) {
        case 0: return "OPEN";
        case 3: return "WPA2_PSK";
        case 5: return "WPA2_ENTERPRISE";
        case 6: return "WPA3_PSK";
        case 7: return "WPA2_WPA3_PSK";
        case 9: return "OWE";
        default: return o->privacy?"PRIVACY":"OPEN";
    }
}
void sw_ble(const uint8_t *addr,uint8_t type,int rssi,uint8_t event,const uint8_t *data,size_t len) {
    atomic_fetch_add(&ble_callbacks,1);
    if (atomic_load(&ble_accepting) && !sw_wifi_only_mode() && collect_ble) {
        observation o={.kind=2,.addr_type=type,.rssi=rssi,.event=event,.len=len>62?62:len};
        for(int i=0;i<6;i++) o.addr[i]=addr[5-i]; /* NimBLE is little-endian */
        memcpy(o.data,data,o.len); atomic_fetch_add(&ble_count,1); offer(&o);
    }
    atomic_fetch_sub(&ble_callbacks,1);
}
bool sw_ble_wait_quiescent(unsigned timeout_ms) {
    for(unsigned waited=0;;) {
        if(!atomic_load(&ble_callbacks))return true;
        if(waited>=timeout_ms)return false;
        vTaskDelay(pdMS_TO_TICKS(10));waited+=10;
    }
}
void sw_ble_set_accepting(bool accepting) { atomic_store(&ble_accepting,accepting); }
static ZERO_STACK_NOINLINE void emit_version(const observation *o,unsigned version,unsigned batch) {
    int64_t age=now_ms()-o->at;
    if(age>(version==2?20000:2000)) { note_loss(&loss_age,1); return; }
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
                snprintf(b,sizeof(b),"{\"v\":2,\"kind\":\"wifi\",\"session\":\"%s\",\"seq\":%u,\"batch\":%u,\"capture_ms\":%lld,\"age_ms\":%lld,\"mac\":\"%s\",\"ssid_hex\":\"%s\",\"channel\":%u,\"rssi\":%d,\"auth\":\"%s\"}",session,next_seq(),batch,(long long)o->at,(long long)(now_ms()-o->at),bssid,data,o->channel,o->rssi,wifi_auth_label(o));
            else
                snprintf(b,sizeof(b),"{\"v\":1,\"kind\":\"wifi\",\"session\":\"%s\",\"seq\":%u,\"capture_ms\":%lld,\"age_ms\":%lld,\"mac\":\"%s\",\"ssid_hex\":\"%s\",\"channel\":%u,\"rssi\":%d,\"auth\":\"%s\"}",session,next_seq(),(long long)o->at,(long long)(now_ms()-o->at),bssid,data,o->channel,o->rssi,wifi_auth_label(o));
            output(b);
        }
    }
}
static void emit(const observation *o) { emit_version(o,1,0); }

static ZERO_STACK_NOINLINE void emit_remote_id_wifi(const rid_wifi_observation *o,unsigned batch) {
    int64_t age=now_ms()-o->at;
    if(age<0 || age>20000) { note_loss(&loss_age,1); return; }
    char address[18],data[RID_WIFI_MAX*2+1],fields[720],body[900];
    mac(address,o->mac); hex(data,o->data,o->len);
    if(job_mode) {
        int n=snprintf(fields,sizeof(fields),"\"batch\":%u,\"capture_ms\":%lld,\"age_ms\":%lld,\"mac\":\"%s\",\"channel\":%u,\"rssi\":%d,\"transport\":\"%s\",\"data_hex\":\"%s\",\"truncated\":%s",
                       batch,(long long)o->at,(long long)age,address,o->channel,o->rssi,
                       o->transport?"nan":"beacon",data,o->truncated?"true":"false");
        if(n>0 && n<(int)sizeof(fields)) sw_job_emit_data("remote_id_wifi",fields);
    } else {
        int n=snprintf(body,sizeof(body),"{\"v\":2,\"kind\":\"remote_id_wifi\",\"session\":\"%s\",\"seq\":%u,\"batch\":%u,\"capture_ms\":%lld,\"age_ms\":%lld,\"mac\":\"%s\",\"channel\":%u,\"rssi\":%d,\"transport\":\"%s\",\"data_hex\":\"%s\",\"truncated\":%s}",
                       session,next_seq(),batch,(long long)o->at,(long long)age,address,o->channel,o->rssi,
                       o->transport?"nan":"beacon",data,o->truncated?"true":"false");
        if(n>0 && n<(int)sizeof(body)) output(body);
    }
}

static uint32_t observation_hash(const observation *o) {
    uint32_t hash=2166136261u;
    const uint8_t *parts[]={&o->kind,o->addr,o->rx,o->bssid,&o->addr_type,
                            &o->event,&o->len,o->data,&o->channel,
                            &o->ssid_present,&o->privacy,&o->authmode};
    const size_t sizes[]={1,6,6,6,1,1,1,o->len,1,1,1,1};
    for(unsigned part=0;part<sizeof(parts)/sizeof(parts[0]);part++)
        for(size_t i=0;i<sizes[part];i++) hash=(hash^parts[part][i])*16777619u;
    return hash ? hash : 1;
}
static bool observation_equal(const observation *a,const observation *b) {
    return a->kind==b->kind && !memcmp(a->addr,b->addr,6) &&
        !memcmp(a->rx,b->rx,6) && !memcmp(a->bssid,b->bssid,6) &&
        a->addr_type==b->addr_type && a->event==b->event && a->len==b->len &&
        !memcmp(a->data,b->data,a->len) && a->channel==b->channel &&
        a->ssid_present==b->ssid_present && a->privacy==b->privacy && a->authmode==b->authmode;
}
static bool batch_open(void) {
#if CONFIG_PROJECTZERO_BOARD_WAVESHARE_ZERO
    batch_capacity=64;
#else
    batch_capacity=256;
#endif
    batch_slots=heap_caps_malloc(sizeof(*batch_slots)*batch_capacity,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#if CONFIG_PROJECTZERO_BOARD_WAVESHARE_ZERO
    if(!batch_slots) batch_slots=heap_caps_malloc(sizeof(*batch_slots)*batch_capacity,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
#else
    if(!batch_slots) {
        batch_capacity=128;
        batch_slots=heap_caps_malloc(sizeof(*batch_slots)*batch_capacity,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    }
#endif
    if(batch_slots) memset(batch_slots,0,sizeof(*batch_slots)*batch_capacity);
    return batch_slots!=NULL;
}
static void batch_close(void) {
    heap_caps_free(batch_slots); batch_slots=NULL; batch_capacity=0;
}
/* The Zero has no PSRAM. Admit bounded serial work only if the known capture
 * buffers and worker stack fit while preserving RAM for the radio stacks. */
static bool job_memory_admit(bool hs, bool batch) {
#if CONFIG_PROJECTZERO_BOARD_WAVESHARE_ZERO
    size_t requested = 6144; /* serial worker task stack */
    if (hs) requested += 8u * sizeof(hs_observation);
    if (batch) requested += 64u * sizeof(batch_slot);
    if (requested > ZERO_JOB_WORKING_SET_BUDGET) return false;
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >=
           requested + ZERO_JOB_INTERNAL_RESERVE;
#else
    (void)hs;
    (void)batch;
    return true;
#endif
}
static bool batch_add(const observation *o) {
    uint32_t hash=observation_hash(o);
    for(unsigned probe=0;probe<batch_capacity;probe++) {
        batch_slot *slot=&batch_slots[(hash+probe)%batch_capacity];
        if(!slot->used) { slot->used=true;slot->hash=hash;slot->value=*o;return true; }
        if(slot->hash==hash && observation_equal(&slot->value,o)) {
            if(o->rssi>slot->value.rssi) slot->value.rssi=o->rssi;
            slot->value.at=o->at;
            return true;
        }
    }
    note_loss(&loss_saturation,1);
    return false;
}
/* Notification hints only. Full evidence stays in the batch. All filtering and
 * serial I/O run in the worker, never the WiFi/NimBLE callbacks. */
static void live_oui_emit(const observation *o,const uint8_t *addr,unsigned batch) {
    int64_t age=now_ms()-o->at;
    if(!live_oui_count || age<0 || age>2000 || (addr[0]&3) ||
       (o->kind==2 && o->addr_type!=0)) return;
    bool match=false;
    for(unsigned i=0;i<live_oui_count;i++)
        if(!memcmp(addr,live_ouis[i],3)) { match=true;break; }
    if(!match) return;
    for(unsigned i=0;i<live_identity_count;i++)
        if(live_identities[i].kind==o->kind && live_identities[i].addr_type==o->addr_type &&
           !memcmp(addr,live_identities[i].addr,6)) return;
    if(live_identity_count>=LIVE_OUI_IDENTITIES) return;
    unsigned slot=live_identity_count++;
    live_identities[slot].kind=o->kind; live_identities[slot].addr_type=o->addr_type;
    memcpy(live_identities[slot].addr,addr,6);
    char a[18],b[384],type[32]="";
    mac(a,addr);
    if(o->kind==2) snprintf(type,sizeof(type),",\"addr_type\":%u",o->addr_type);
    snprintf(b,sizeof(b),"{\"v\":2,\"kind\":\"live_oui\",\"session\":\"%s\",\"seq\":%u,\"batch\":%u,\"capture_ms\":%lld,\"age_ms\":%lld,\"radio\":\"%s\",\"mac\":\"%s\",\"rssi\":%d%s}",
             session,next_seq(),batch,(long long)o->at,(long long)age,o->kind==2?"ble":"wifi",a,o->rssi,type);
    /* One best-effort attempt per identity/batch. A failed hint cannot discard
     * or mutate its saved batch record, and normal host detection is fallback. */
    output(b);
}
static void batch_observe(const observation *o,unsigned batch) {
    if(!batch_add(o)) return;
    live_oui_emit(o,o->addr,batch);
    if(o->kind==1 && (o->event==8 || o->event==5) && memcmp(o->addr,o->bssid,6))
        live_oui_emit(o,o->bssid,batch);
}
static int hex_digit(char c) {
    if(c>='0' && c<='9') return c-'0';
    if(c>='a' && c<='f') return c-'a'+10;
    if(c>='A' && c<='F') return c-'A'+10;
    return -1;
}
static bool parse_live_ouis(const char *arg,uint8_t values[LIVE_OUI_MAX][3],unsigned *count) {
    if(strncmp(arg,"live_oui=",9)) return false;
    const char *p=arg+9; *count=0;
    do {
        if(*count>=LIVE_OUI_MAX || strlen(p)<6) return false;
        for(unsigned i=0;i<3;i++) {
            int hi=hex_digit(p[2*i]),lo=hex_digit(p[2*i+1]);
            if(hi<0 || lo<0) return false;
            values[*count][i]=(hi<<4)|lo;
        }
        for(unsigned i=0;i<*count;i++) if(!memcmp(values[i],values[*count],3)) return false;
        (*count)++; p+=6;
        if(!*p) return true;
        if(*p++!=',' || !*p) return false;
    } while(true);
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
    if(job_mode) {
        unsigned type=(p->payload[0]>>2)&3,ds=p->payload[1]&3;
        const uint8_t *bssid=NULL;
        if(type==0)bssid=p->payload+16;
        else if(type==2&&ds!=3)bssid=p->payload+(ds==1?4:ds==2?10:16);
        if(bssid&&!job_filter_bssid(bssid))goto done;
    }
    if (n > HS_MAX_FRAME) { note_loss(&loss_saturation, 1); goto done; }
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
    if (!o) { note_loss(&loss_saturation, 1); goto done; }
    o->at=now_ms(); o->len=n; o->channel=p->rx_ctrl.channel; o->rssi=p->rx_ctrl.rssi;
    memcpy(o->data, p->payload, n);
    if (!capture_pool_publish(&hs_pool, o)) note_loss(&loss_saturation, 1);
    else atomic_fetch_add(&wifi_count, 1);
done:
    atomic_fetch_sub(&producers, 1);
}
static ZERO_STACK_NOINLINE void hs_emit(const hs_observation *o) {
    /* Decide queue age once, before emitting any chunks. Abandoning a frame
     * midway through a slow USB write leaves the host with an unusable record. */
    int64_t age = now_ms() - o->at;
    if (age > 2000) { note_loss(&loss_age, 1); return; }
    unsigned id = ++packet_id;
    for (unsigned offset = 0; offset < o->len; offset += 240) {
        unsigned size = o->len-offset < 240 ? o->len-offset : 240;
        char data[481], body[900];
        hex(data, o->data+offset, size);
        snprintf(body, sizeof(body), "{\"v\":1,\"kind\":\"hs_packet\",\"session\":\"%s\",\"seq\":%u,\"packet\":%u,\"offset\":%u,\"total\":%u,\"capture_ms\":%lld,\"age_ms\":%lld,\"channel\":%u,\"rssi\":%d,\"data_hex\":\"%s\"}",
                 session, next_seq(), id, offset, o->len, (long long)o->at, (long long)age, o->channel, o->rssi, data);
        output(body);
    }
}
static ZERO_STACK_NOINLINE void batch_worker_loop(void) {
    linenoiseSetMachineMode(true);
    atomic_store(&batch_phase,1);
    batch_status("started","running",",\"window_ms\":10000");
    int64_t heartbeat=now_ms(); unsigned hop_index=0; int64_t hop=0;
    while(!atomic_load(&stopping) && now_ms()-atomic_load(&lease)<15000) {
        unsigned current=atomic_fetch_add(&batch_number,1)+1;
        memset(batch_slots,0,sizeof(*batch_slots)*batch_capacity);
        live_identity_count=0;
        memset(seen,0,sizeof(seen)); xQueueReset(queue);
        atomic_store(&batch_phase,1); atomic_store(&collecting,true);
        batch_status("batch_start","running",",\"window_ms\":10000");
        int64_t deadline=now_ms()+10000;
        while(!atomic_load(&stopping) && now_ms()<deadline && now_ms()-atomic_load(&lease)<15000) {
            if(now_ms()-hop>=(job_mode?job_dwell_ms:160)) {
                if(job_mode) {
                    if(job_channel_count) { uint8_t applied=0;if(!sw_job_radio_hop(hop_index++,&applied)){sw_job_emit_lifecycle("error","running","\"message\":\"channel_apply_failed\"");atomic_store(&stopping,true);break;} }
                } else if(!sw_radio_hop(hop_index++)) {
                    batch_status("error","error",",\"message\":\"channel_apply_failed\"");
                    atomic_store(&stopping,true);
                    break;
                }
                hop=now_ms();
            }
            observation o;
            if(xQueueReceive(queue,&o,pdMS_TO_TICKS(10))==pdTRUE) batch_observe(&o,current);
            while(xQueueReceive(rid_wifi_queue,&rid_wifi_worker_scratch,0)==pdTRUE)
                emit_remote_id_wifi(&rid_wifi_worker_scratch,current);
            if(now_ms()-heartbeat>=2000) {
                batch_status("heartbeat","running",""); heartbeat=now_ms();
            }
        }
        atomic_store(&collecting,false);
        while(atomic_load(&producers) && !atomic_load(&stopping)) vTaskDelay(1);
        observation pending;
        while(xQueueReceive(queue,&pending,0)==pdTRUE) batch_observe(&pending,current);
        while(xQueueReceive(rid_wifi_queue,&rid_wifi_worker_scratch,0)==pdTRUE)
            emit_remote_id_wifi(&rid_wifi_worker_scratch,current);
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
            if(now_ms()>=report_deadline) { note_loss(&loss_age,1);continue; }
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
static ZERO_STACK_NOINLINE void worker(void *unused) {
    bool ready=sw_radio_start();
    bool memory_floor_failed=false;
    bool channel_start_failed=false;
    bool startup_failed=false;
#if CONFIG_PROJECTZERO_BOARD_WAVESHARE_ZERO
    if(ready && job_mode && heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<ZERO_JOB_INTERNAL_RESERVE) {
        ready=false;
        memory_floor_failed=true;
    }
#endif
    if(ready && job_mode && job_channel_count) {
        uint8_t applied=0;
        if(!sw_job_radio_hop(0,&applied)) {ready=false;channel_start_failed=true;}
    }
    wifi_promiscuous_filter_t filter={.filter_mask=WIFI_PROMIS_FILTER_MASK_MGMT | (passive_hs ? WIFI_PROMIS_FILTER_MASK_DATA : 0)};
    if(ready) ready=esp_wifi_set_promiscuous_filter(&filter)==ESP_OK && esp_wifi_set_promiscuous_rx_cb(passive_hs ? hs_wifi_cb : wifi_cb)==ESP_OK && esp_wifi_set_promiscuous(true)==ESP_OK;
    if(ready && !atomic_load(&stopping)) {
        atomic_store(&radio_ready,true);
        capture_memory_log("serial_capture_ready");
        if(batch_mode) batch_worker_loop();
        else {
            atomic_store(&collecting,true);
            if(job_mode) sw_job_emit_lifecycle("started","running","");
            else status("started","");
            int64_t stats=now_ms(),hop=0; unsigned index=0;
            while(!atomic_load(&stopping) && now_ms()-atomic_load(&lease)<15000) {
                if(now_ms()-hop>=(job_mode?job_dwell_ms:160)) {
                    if(job_mode) {
                        if(job_channel_count) { uint8_t applied=0;if(!sw_job_radio_hop(index++,&applied)){sw_job_emit_lifecycle("error","running","\"message\":\"channel_apply_failed\"");atomic_store(&stopping,true);break;} }
                    } else if(!sw_radio_hop(index++)) {
                        status("error",",\"message\":\"channel_apply_failed\"");
                        atomic_store(&stopping,true);
                        break;
                    }
                    hop=now_ms();
                }
                if (passive_hs) {
                    hs_observation *o;
                    if(xQueueReceive(hs_pool.ready,&o,pdMS_TO_TICKS(10))==pdTRUE) {
                        hs_emit(o); capture_pool_release(&hs_pool, o);
                    }
                } else {
                    observation o;
                    if(xQueueReceive(queue,&o,pdMS_TO_TICKS(10))==pdTRUE) emit(&o);
                }
                if(xQueueReceive(rid_wifi_queue,&rid_wifi_worker_scratch,0)==pdTRUE)
                    emit_remote_id_wifi(&rid_wifi_worker_scratch,atomic_load(&batch_number));
                if(now_ms()-stats>=2000) {
                    if(job_mode) sw_job_emit_lifecycle("heartbeat","running","");
                    else status("stats","");
                    stats=now_ms();
                }
            }
        }
    } else if(!ready) {
        startup_failed=true;
        if(job_mode) sw_job_emit_lifecycle("error","stopping",memory_floor_failed?"\"message\":\"memory_floor_failed\"":channel_start_failed?"\"message\":\"channel_apply_failed\"":"\"message\":\"radio_start_failed\"");
        else if(batch_mode) batch_status("error","error",",\"message\":\"radio_start_failed\"");
        else status("error",",\"message\":\"radio_start_failed\"");
    }
    atomic_store(&collecting,false);
    while(!sw_radio_stop()) {
        /* Keep the token and job mode until the radio snapshot is restored.
         * A replacement job cannot inherit partially restored Wi-Fi state. */
        if(job_mode)sw_job_emit_lifecycle("status","stopping","\"message\":\"radio_restore_retry\"");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    /* Radio callbacks never wait, so outstanding copies finish promptly. */
    while(atomic_load(&producers)) vTaskDelay(1);
    while(xQueueReceive(rid_wifi_queue,&rid_wifi_worker_scratch,0)==pdTRUE)
        emit_remote_id_wifi(&rid_wifi_worker_scratch,atomic_load(&batch_number));
    if (passive_hs) {
        int64_t deadline = now_ms() + 1500;
        hs_observation *o;
        while (now_ms() < deadline && xQueueReceive(hs_pool.ready, &o, 0) == pdTRUE) {
            hs_emit(o); capture_pool_release(&hs_pool, o);
        }
        note_loss(&loss_queue, uxQueueMessagesWaiting(hs_pool.ready));
        capture_pool_close(&hs_pool);
    } else {
        unsigned discarded=uxQueueMessagesWaiting(queue);
        note_loss(&loss_queue,discarded); xQueueReset(queue);
    }
    if(job_mode) sw_job_emit_memory_snapshot("wardrive_stop");
    if(batch_mode) {
        atomic_store(&batch_phase,0);
        batch_close();
        live_oui_count=live_identity_count=0;
        batch_status("stopped","stopped","");
        linenoiseSetMachineMode(false);
    } else if(job_mode) sw_job_emit_lifecycle("stopped","stopped",startup_failed?"\"message\":\"startup_cleanup_complete\"":"");
    else status("stopped","");
    if(job_mode) {
        remember_terminal_job(job_token,job_request_hash);
        job_mode=false;
    }
    atomic_store(&active,false);
    vTaskDelete(NULL);
}
bool sw_stop(void) {
    atomic_store(&stopping,true);
    int64_t deadline=now_ms()+4000;
    while(atomic_load(&active) && now_ms()<deadline) vTaskDelay(pdMS_TO_TICKS(10));
    return !atomic_load(&active);
}
static int start(int argc,char **argv) {
    if((argc!=2 && argc!=3) || !argv[1][0] || strlen(argv[1])>32) return 1;
    for(char *p=argv[1];*p;p++) if(!isalnum((unsigned char)*p) && *p!='-' && *p!='_') return 1;
    if(sw_active()) return 1;
    if(!in_job_dispatch) { job_mode=false; job_request_hash=0; collect_wifi=true; collect_ble=true; job_channel_count=0; job_dwell_ms=160; job_target_count=job_exclusion_count=0; }
    bool requested_batch=strstr(argv[0],"_batch_serial")!=NULL;
    uint8_t requested_ouis[LIVE_OUI_MAX][3]; unsigned requested_count=0;
    if(argc==3 && (!requested_batch || !parse_live_ouis(argv[2],requested_ouis,&requested_count))) return 1;
    live_oui_count=requested_count; live_identity_count=0;
    memcpy(live_ouis,requested_ouis,requested_count*3);
    passive_hs = !strcmp(argv[0], "start_hs_sniff_serial");
    batch_mode = requested_batch;
    wifi_only = !strcmp(argv[0], "start_wardrive_wifi_serial") || !strcmp(argv[0], "start_wardrive_wifi_batch_serial");
    if(!queue) queue=xQueueCreateStatic(QLEN,sizeof(observation),queue_bytes,&queue_storage);
    if(!rid_wifi_queue) rid_wifi_queue=xQueueCreateStatic(RID_WIFI_QUEUE_LEN,sizeof(rid_wifi_observation),rid_wifi_queue_bytes,&rid_wifi_queue_storage);
    if(!queue || !rid_wifi_queue) return 1;
    xQueueReset(queue); xQueueReset(rid_wifi_queue); memset(seen,0,sizeof(seen));
    /* seq is boot-wide. Keeping it monotonic lets a cached terminal token
       receive a fresh status record even after later jobs have run. */
    strcpy(session,argv[1]); atomic_store(&batch_number,0); atomic_store(&batch_phase,0); packet_id=0;
    atomic_store(&drops,0); reset_losses(); atomic_store(&wifi_count,0); atomic_store(&ble_count,0);
    if(!job_memory_admit(passive_hs,batch_mode)) {
        if(batch_mode) batch_status("error","error",",\"message\":\"memory_admission_failed\"");
        else status("error",",\"message\":\"memory_admission_failed\"");
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
    if(!sw_prepare()) {
        capture_pool_close(&hs_pool);
        batch_close();
        if(batch_mode) batch_status("error","error",",\"message\":\"radio_prepare_failed\"");
        else status("error",",\"message\":\"radio_prepare_failed\"");
        return 1;
    }
    atomic_store(&lease,now_ms()); atomic_store(&stopping,false); atomic_store(&radio_ready,false); atomic_store(&active,true);
    if(job_mode) sw_job_emit_memory_snapshot("wardrive_start");
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
    (void)argc; (void)argv;
    sw_boot_snapshot_t boot = sw_get_boot_snapshot();
    if(boot.state != BOOT_READY) {
        output("{\"v\":1,\"kind\":\"capabilities\",\"c5_jobs_v1\":false,\"max_line\":1024}");
        return 0;
    }
    output("{\"v\":1,\"kind\":\"capabilities\",\"c5_jobs_v1\":true,\"wardrive_serial_v1\":true,\"wardrive_wifi_serial_v1\":true,\"wardrive_batch_serial_v2\":true,\"wardrive_wifi_batch_serial_v2\":true,\"wardrive_live_oui_v1\":true,\"remote_id_wifi_v1\":true,\"remote_id_wifi_max_bytes\":228,\"live_oui_max\":8,\"batch_window_ms\":10000,\"heartbeat_ms\":2000,\"max_age_ms\":20000,\"hs_sniff_serial_v1\":true,\"hs_capture_targets_v1\":true,\"hs_capture_exclusions_v1\":true,\"hs_capture_pcapng_v1\":true,\"sniffer_intel_v1\":true,\"sniffer_scope_v1\":true,\"target_intel_v1\":true,\"bands\":[\"wifi24\",\"wifi5\",\"ble\"],\"wifi_mgmt\":true,\"ble_raw_ad\":true,\"ble_extended\":false,\"max_line\":1024}"); return 0;
}
static bool identity_ready;
static bool ensure_identity(void) {
    if(identity_ready) return true;
    uint8_t mac_bytes[6]; uint32_t nonce[2]; char tmp[16];
    if(esp_read_mac(mac_bytes,ESP_MAC_EFUSE_FACTORY)!=ESP_OK) return false;
    hex(tmp,mac_bytes,sizeof(mac_bytes)); memcpy(device_id,tmp,13);
    nonce[0]=esp_random(); nonce[1]=esp_random(); hex(boot_id,(const uint8_t *)nonce,sizeof(nonce));
    identity_ready=true; return true;
}
static void job_event_for(const char *token,const char *kind,const char *state,const char *message) {
    if(!ensure_identity()) return;
    char body[512],line[560]; unsigned seqno=next_seq();
    int n=snprintf(body,sizeof(body),"{\"v\":3,\"kind\":\"%s\",\"device_id\":\"%s\",\"boot_id\":\"%s\",\"session\":\"%s\",\"seq\":%u,\"batch\":%u,\"state\":\"%s\",\"wifi_count\":%u,\"ble_count\":%u,\"drops\":%u%s%s%s}",
        kind,device_id,boot_id,token?token:"",seqno,atomic_load(&batch_number),state?state:"error",atomic_load(&wifi_count),atomic_load(&ble_count),atomic_load(&drops),
        message?",\"message\":\"":"",message?message:"",message?"\"":"");
    if(n<=0 || n>=(int)sizeof(body)) return;
    n=snprintf(line,sizeof(line),"\nWDG:%s\n",body);
    if(n>0 && n<(int)sizeof(line)) serial_output(line,n,250);
}
static void job_event(const char *kind,const char *state,const char *message) {
    job_event_for(job_token,kind,state,message);
}
static void job_request_error_for(const char *token,const char *request_error) {
    if(!ensure_identity() || !token || !request_error) return;
    const char *state=job_current_state();
    char body[512],line[560];
    int n=snprintf(body,sizeof(body),"{\"v\":3,\"kind\":\"status\",\"device_id\":\"%s\",\"boot_id\":\"%s\",\"session\":\"%s\",\"seq\":%u,\"batch\":%u,\"state\":\"%s\",\"wifi_count\":%u,\"ble_count\":%u,\"drops\":%u,\"request_error\":\"%s\"}",
        device_id,boot_id,token,next_seq(),atomic_load(&batch_number),state,
        atomic_load(&wifi_count),atomic_load(&ble_count),atomic_load(&drops),request_error);
    if(n<=0 || n>=(int)sizeof(body)) return;
    n=snprintf(line,sizeof(line),"\nWDG:%s\n",body);
    if(n>0 && n<(int)sizeof(line)) (void)serial_output(line,n,250);
}
static bool json_string(cJSON *obj,const char *name,const char **value) {
    cJSON *item=cJSON_GetObjectItemCaseSensitive(obj,name);
    if(!cJSON_IsString(item) || !item->valuestring) return false;
    *value=item->valuestring; return true;
}
static int b64url_value(char c) {
    if(c>='A'&&c<='Z')return c-'A';
    if(c>='a'&&c<='z')return c-'a'+26;
    if(c>='0'&&c<='9')return c-'0'+52;
    if(c=='-')return 62;
    if(c=='_')return 63;
    return -1;
}
static bool decode_job_json(const char *encoded,char *decoded,size_t cap,size_t *out_len) {
    size_t n=strlen(encoded), used=0; unsigned accumulator=0,bits=0;
    if(!n || n>5462 || n%4==1) return false;
    for(size_t i=0;i<n;i++) {
        int digit=b64url_value(encoded[i]); if(digit<0) return false;
        accumulator=(accumulator<<6)|(unsigned)digit; bits+=6;
        if(bits>=8) {
            bits-=8; if(used+1>=cap) return false;
            decoded[used++]=(char)((accumulator>>bits)&0xffu);
        }
    }
    if(bits && (accumulator & ((1u<<bits)-1u))) return false;
    if(used>4096) return false;
    decoded[used]=0; *out_len=used; return true;
}
static uint32_t request_hash(const char *data,size_t len) {
    uint32_t h=2166136261u; for(size_t i=0;i<len;i++) h=(h^(uint8_t)data[i])*16777619u;
    return h?h:1;
}
static bool valid_channel(unsigned channel) {
    /* US-configured XIAO 1.8.7 applied/read back channel 144 on hardware.
     * Other board profiles stay conservative until tested separately. */
    static const uint8_t allowed[]={1,2,3,4,5,6,7,8,9,10,11,36,40,44,48,52,56,60,64,100,104,108,112,116,120,124,128,132,136,140,
#if PROJECTZERO_XIAO_US_CH144
        144,
#endif
        149,153,157,161,165};
    bool listed=false;
    for(unsigned i=0;i<sizeof(allowed);i++) if(allowed[i]==channel) { listed=true;break; }
    if(!listed)return false;
    wifi_country_t country={0};
    bool has_country=esp_wifi_get_country(&country)==ESP_OK;
    if(channel<=14) {
        if(has_country && country.nchan) {
            unsigned first=country.schan,last=first+country.nchan-1;
            if(channel<first || channel>last)return false;
        } else if(channel>11)return false; /* SDK world-safe range before init. */
    } else if(has_country && country.policy==WIFI_COUNTRY_POLICY_MANUAL && country.wifi_5g_channel_mask) {
        if(!(country.wifi_5g_channel_mask & CHANNEL_TO_BIT(channel)))return false;
    }
    return true;
}
static bool parse_mac_string(const char *text,uint8_t mac_out[6]) {
    if(!text||strlen(text)!=17)return false;
    for(unsigned i=0;i<6;i++) {
        int hi=hex_digit(text[3*i]),lo=hex_digit(text[3*i+1]);
        if(hi<0||lo<0||(i<5&&text[3*i+2]!=':'))return false;
        mac_out[i]=(uint8_t)((hi<<4)|lo);
    }
    static const uint8_t zero[6];
    return !(mac_out[0]&1)&&memcmp(mac_out,zero,6)!=0;
}
bool sw_register_job_operation_ex(const char *operation,sw_job_operation_handler_t handler,sw_job_operation_stop_t stop) {
    if(!operation || !*operation || strlen(operation)>=sizeof(job_handlers[0].operation) || !handler || job_handler_count>=12) return false;
    for(unsigned i=0;i<job_handler_count;i++) if(!strcmp(operation,job_handlers[i].operation)) return false;
    snprintf(job_handlers[job_handler_count].operation,sizeof(job_handlers[0].operation),"%s",operation);
    job_handlers[job_handler_count].handler=handler; job_handlers[job_handler_count].stop=stop; job_handler_count++; return true;
}
bool sw_register_job_operation(const char *operation,sw_job_operation_handler_t handler) { return sw_register_job_operation_ex(operation,handler,NULL); }
void sw_register_radio_busy_probe(sw_radio_busy_probe_t probe) { radio_busy_probe=probe; }
const char *sw_job_token(void) { return job_token; }
bool sw_job_owned(void) { return job_mode; }
bool sw_job_owner_matches(const char *token) { return token && job_token[0] && !strcmp(token,job_token); }
bool sw_job_signal_ready(const char *token) {
    if(!sw_job_owner_matches(token) || !atomic_load(&external_job_starting) ||
       atomic_load(&external_job_stopping))return false;
    atomic_store(&external_operation_ready,true);
    return true;
}
bool sw_job_fail_start(const char *token,const char *reason) {
    if(!sw_job_owner_matches(token) || !atomic_load(&external_job_starting))return false;
    char code[41];size_t used=0;
    if(reason)for(;reason[used] && used<sizeof(code)-1;used++) {
        unsigned char ch=(unsigned char)reason[used];
        if(!islower(ch) && !isdigit(ch) && ch!='_')break;
        code[used]=(char)ch;
    }
    code[used]=0;
    if(!used || (reason && reason[used]))snprintf(code,sizeof(code),"unknown");
    char fields[128];
    snprintf(fields,sizeof(fields),"\"message\":\"operation_start_failed\",\"start_reason\":\"%s\"",code);
    atomic_store(&external_job_stopping,true);
    sw_job_emit_lifecycle("error","stopping",fields);
    return true;
}
bool sw_job_request_cleanup(const char *token,const char *reason) {
    if(!sw_job_owner_matches(token) || !atomic_load(&external_job_active))return false;
    char code[41];size_t used=0;
    if(reason)for(;reason[used] && used<sizeof(code)-1;used++) {
        unsigned char ch=(unsigned char)reason[used];
        if(!islower(ch) && !isdigit(ch) && ch!='_')break;
        code[used]=(char)ch;
    }
    code[used]=0;
    if(!used || (reason && reason[used]))snprintf(code,sizeof(code),"unknown");
    char fields[128];
    snprintf(fields,sizeof(fields),"\"message\":\"operation_failed\",\"failure_reason\":\"%s\"",code);
    atomic_store(&external_job_stopping,true);
    sw_job_emit_lifecycle("error","stopping",fields);
    return true;
}
bool sw_job_lease_valid(void) { return job_mode && now_ms()-atomic_load(&lease)<15000; }
bool sw_job_set_radio_plan(const uint8_t *channels,size_t count,unsigned dwell_ms) {
    if(count>40 || (count && !channels) || dwell_ms<40 || dwell_ms>2000) return false;
    for(size_t i=0;i<count;i++) {
        if(!valid_channel(channels[i])) return false;
        for(size_t j=0;j<i;j++) if(channels[j]==channels[i]) return false;
    }
    if(count) memcpy(job_channels,channels,count);
    job_channel_count=(unsigned)count; job_dwell_ms=dwell_ms; return true;
}
bool sw_job_set_filters(const uint8_t targets[][6],size_t target_count,const uint8_t exclusions[][6],size_t exclusion_count) {
    if(target_count>JOB_TARGET_MAX||exclusion_count>JOB_EXCLUSION_MAX||
       (target_count&&!targets)||(exclusion_count&&!exclusions))return false;
#if CONFIG_PROJECTZERO_BOARD_WAVESHARE_ZERO
    if(target_count>8)return false;
#endif
    for(size_t i=0;i<target_count;i++)for(size_t j=0;j<i;j++)if(!memcmp(targets[i],targets[j],6))return false;
    for(size_t i=0;i<exclusion_count;i++)for(size_t j=0;j<i;j++)if(!memcmp(exclusions[i],exclusions[j],6))return false;
    if(target_count)memcpy(job_targets,targets,target_count*6);
    if(exclusion_count)memcpy(job_exclusions,exclusions,exclusion_count*6);
    job_target_count=(unsigned)target_count;job_exclusion_count=(unsigned)exclusion_count;return true;
}
sw_channel_result_t sw_job_apply_channel_checked(uint8_t channel) {
    sw_channel_result_t result={.status=SW_CHANNEL_DRIVER_ERROR,.requested_channel=channel,
                                .mode=WIFI_MODE_NULL,.band=WIFI_BAND_MODE_AUTO,.sdk_error=ESP_OK};
    (void)esp_wifi_get_mode(&result.mode);
    (void)esp_wifi_get_band_mode(&result.band);
    if(job_mode && !sw_job_channel_allowed(channel)) {
        result.status=SW_CHANNEL_NOT_IN_PLAN;
        result.sdk_error=ESP_ERR_INVALID_ARG;
        return result;
    }
    result.sdk_error=esp_wifi_set_channel(channel,WIFI_SECOND_CHAN_NONE);
    if(result.sdk_error!=ESP_OK) return result;
    wifi_second_chan_t secondary=WIFI_SECOND_CHAN_NONE;
    result.sdk_error=esp_wifi_get_channel(&result.actual_channel,&secondary);
    /* ESP-IDF chooses the secondary channel automatically on 5 GHz even when
     * the caller passes NONE. The primary is the requested RF channel; only
     * require NONE on 2.4 GHz, where the caller's HT20 request is meaningful. */
    if(result.sdk_error!=ESP_OK || result.actual_channel!=channel ||
       (channel<=14 && secondary!=WIFI_SECOND_CHAN_NONE)) {
        result.status=SW_CHANNEL_READBACK_ERROR;
        if(result.sdk_error==ESP_OK)result.sdk_error=ESP_FAIL;
        return result;
    }
    result.status=SW_CHANNEL_OK;
    if(job_mode) {
        char fields[32];snprintf(fields,sizeof(fields),"\"channel\":%u",result.actual_channel);
        sw_job_emit_lifecycle("status",job_current_state(),fields);
    }
    return result;
}
sw_channel_result_t sw_job_radio_hop_checked(unsigned index) {
    if(job_mode && !job_channel_count) {
        sw_channel_result_t result={.status=SW_CHANNEL_NOT_IN_PLAN,.sdk_error=ESP_ERR_INVALID_ARG};
        return result;
    }
    static const uint8_t legacy_channels[]={1,6,11,2,3,4,5,7,8,9,10,
        36,40,44,48,52,56,60,64,100,104,108,112,116,120,124,128,132,136,140,
#if PROJECTZERO_XIAO_US_CH144
        144,
#endif
        149,153,157,161,165};
    uint8_t channel=job_mode?job_channels[index%job_channel_count]:legacy_channels[index%sizeof(legacy_channels)];
    return sw_job_apply_channel_checked(channel);
}
bool sw_job_radio_hop(unsigned index,uint8_t *applied_channel) {
    sw_channel_result_t result=sw_job_radio_hop_checked(index);
    if(result.status!=SW_CHANNEL_OK)return false;
    if(applied_channel)*applied_channel=result.actual_channel;
    return true;
}
bool sw_job_apply_channel(uint8_t channel) {
    return sw_job_apply_channel_checked(channel).status==SW_CHANNEL_OK;
}
void sw_job_emit_channel_failure(sw_channel_result_t result) {
    const char *mode=result.mode==WIFI_MODE_STA?"sta":result.mode==WIFI_MODE_AP?"ap":
        result.mode==WIFI_MODE_APSTA?"sta_ap":"null";
    const char *band=result.band==WIFI_BAND_MODE_2G_ONLY?"2g":
        result.band==WIFI_BAND_MODE_5G_ONLY?"5g":result.band==WIFI_BAND_MODE_AUTO?"auto":"unknown";
    const char *failure=result.status==SW_CHANNEL_NOT_IN_PLAN?"not_in_plan":
        result.status==SW_CHANNEL_READBACK_ERROR?"readback":"driver";
    char fields[240];
    snprintf(fields,sizeof(fields),"\"message\":\"channel_apply_failed\",\"channel_failure\":\"%s\",\"requested_channel\":%u,\"actual_channel\":%u,\"radio_mode\":\"%s\",\"band\":\"%s\",\"sdk_error\":%d",
             failure,result.requested_channel,result.actual_channel,mode,band,result.sdk_error);
    sw_job_emit_lifecycle("error","running",fields);
}
bool sw_job_channel_allowed(uint8_t channel) {
    if(!job_mode)return true;
    for(unsigned i=0;i<job_channel_count;i++)if(job_channels[i]==channel)return true;
    return false;
}
size_t sw_job_copy_channels(uint8_t *channels,size_t capacity) {
    if(!job_mode || !channels || capacity<job_channel_count)return 0;
    memcpy(channels,job_channels,job_channel_count);
    return job_channel_count;
}
unsigned sw_job_dwell_ms(void) { return job_mode?job_dwell_ms:160; }
void sw_job_emit_data(const char *data_kind,const char *json_fields) {
    if(!job_mode || !data_kind || !json_fields) return;
    unsigned seqno=next_seq();
    if(atomic_load(&sequence_exhausted)) {
        (void)seqno;
        sw_job_emit_lifecycle("error","stopping","\"message\":\"sequence_exhausted\"");
        return;
    }
    char line[1024];
    int n=snprintf(line,sizeof(line),"\nWDG:{\"v\":3,\"device_id\":\"%s\",\"boot_id\":\"%s\",\"kind\":\"%s\",\"session\":\"%s\",\"seq\":%u,%s}\n",
        device_id,boot_id,data_kind,job_token,seqno,json_fields);
    if(!serial_record_write(line,n,false))note_loss(&loss_output,1);
}
void sw_job_emit_legacy_line(const char *line) {
    if(!job_mode || !line) return;
    char frame[1024];
    int n=snprintf(frame,sizeof(frame),"\nWDG:{\"v\":3,\"device_id\":\"%s\",\"boot_id\":\"%s\",\"kind\":\"legacy\",\"session\":\"%s\",\"seq\":%u,\"line\":\"",
        device_id,boot_id,job_token,next_seq());
    if(atomic_load(&sequence_exhausted)) {
        sw_job_emit_lifecycle("error","stopping","\"message\":\"sequence_exhausted\"");
        return;
    }
    if(n<=0 || n>=(int)sizeof(frame)) { note_loss(&loss_output,1);return; }
    size_t used=(size_t)n;
    for(const unsigned char *p=(const unsigned char *)line;*p;p++) {
        char pair[2];size_t bytes=1;
        if(*p=='"'||*p=='\\'){pair[0]='\\';pair[1]=(char)*p;bytes=2;}
        else if(*p=='\n'){pair[0]='\\';pair[1]='n';bytes=2;}
        else if(*p=='\r'){pair[0]='\\';pair[1]='r';bytes=2;}
        else if(*p=='\t'){pair[0]='\\';pair[1]='t';bytes=2;}
        else if(*p<0x20)pair[0]='?';
        else pair[0]=(char)*p;
        if(used+bytes+3>=sizeof(frame))break;
        memcpy(frame+used,pair,bytes);used+=bytes;
    }
    frame[used++]='"';frame[used++]='}';frame[used++]='\n';
    if(!serial_record_write(frame,(int)used,true))note_loss(&loss_output,1);
}
ZERO_STACK_NOINLINE void sw_job_emit_lifecycle(const char *kind,const char *state,const char *json_fields) {
    if(!job_mode || !kind || !state) return;
    unsigned seqno=next_seq();
    if(atomic_load(&sequence_exhausted)) {
        kind="error";state="stopping";json_fields="\"message\":\"sequence_exhausted\"";
    }
    char line[1024];
    /* Three uint32 values need at most 81 bytes of JSON plus NUL.  Keep
     * this bounded so the capture task's measured local stack stays within
     * the release gate. */
    char sniffer_fields[88]="";
    if(!strcmp(active_job_operation,"sniffer"))
        snprintf(sniffer_fields,sizeof(sniffer_fields),",\"sniffer_packets\":%u,\"sniffer_aps\":%u,\"sniffer_drops\":%u",
                 atomic_load(&sniffer_packets),atomic_load(&sniffer_aps),atomic_load(&sniffer_drops));
    int n=snprintf(line,sizeof(line),"\nWDG:{\"v\":3,\"device_id\":\"%s\",\"boot_id\":\"%s\",\"kind\":\"%s\",\"session\":\"%s\",\"seq\":%u,\"batch\":%u,\"wifi_count\":%u,\"ble_count\":%u,\"drops\":%u,\"losses\":{\"queue\":%u,\"age\":%u,\"output\":%u,\"saturation\":%u},\"state\":\"%s\"%s%s%s}\n",
        device_id,boot_id,kind,job_token,seqno,atomic_load(&batch_number),atomic_load(&wifi_count),atomic_load(&ble_count),atomic_load(&drops),
        atomic_load(&loss_queue),atomic_load(&loss_age),atomic_load(&loss_output),atomic_load(&loss_saturation),state,sniffer_fields,json_fields&&*json_fields?",":"",json_fields?json_fields:"");
    if(n<=0 || n>=(int)sizeof(line) || !serial_record_write(line,n,true))note_loss(&loss_output,1);
}
void sw_job_emit_memory_snapshot(const char *stage) {
    if(!job_mode || !stage) return;
    char fields[192];
    snprintf(fields,sizeof(fields),"\"message\":\"memory_snapshot\",\"stage\":\"%s\",\"internal_free\":%u,\"internal_largest\":%u,\"psram_free\":%u",
             stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    sw_job_emit_lifecycle("status",job_current_state(),fields);
}
void sw_job_remember_terminal(void) { if(job_mode) remember_terminal_job(job_token,job_request_hash); }
static void finalize_external_host_stop(void) {
    bool expected=true;
    if(!atomic_compare_exchange_strong(&external_stop_completed,&expected,false)) return;
    sw_job_emit_lifecycle("stopped","stopped","");
    sw_job_remember_terminal();
    active_job_operation[0]=0;
    job_mode=false;
}
static void remember_terminal_job(const char *token,uint32_t hash) {
    unsigned i=terminal_job_next++%4; terminal_jobs[i].used=true;
    snprintf(terminal_jobs[i].token,sizeof(terminal_jobs[i].token),"%s",token); terminal_jobs[i].hash=hash;
}
static int device_info(int argc,char **argv) {
    (void)argv;
    if(argc!=1) return 1;
    if(!ensure_identity()) {
        output("{\"v\":3,\"kind\":\"error\",\"error\":\"device_info_identity_unavailable\"}");
        return 1;
    }
    sw_boot_snapshot_t boot = sw_get_boot_snapshot();
    bool firmware_ready = boot.state == BOOT_READY;
    bool legacy_busy=!sw_active() && radio_busy_probe && radio_busy_probe();
    uint32_t flash=0; esp_flash_get_size(NULL,&flash);
#if CONFIG_PROJECTZERO_BOARD_WAVESHARE_ZERO
    const char *board="waveshare-zero"; const char *updates="[\"rom\"]";
    size_t psram=0;
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    const char *board="xiao"; const char *updates="[\"usb-ota\",\"rom\"]";
#if CONFIG_SPIRAM
    size_t psram=esp_psram_get_size();
#else
    size_t psram=0;
#endif
#else
    const char *board="wroom"; const char *updates="[\"usb-ota\",\"rom\"]";
#if CONFIG_SPIRAM
    size_t psram=esp_psram_get_size();
#else
    size_t psram=0;
#endif
#endif
    char operations[512]="\"wardrive\",\"wifi_scan\",\"ble_scan\",\"hs_sniff\"";
    size_t operations_used=strlen(operations);
    for(unsigned i=0;i<job_handler_count && operations_used+32<sizeof(operations);i++) {
        bool duplicate=false;
        if(!strcmp(job_handlers[i].operation,"wardrive")||!strcmp(job_handlers[i].operation,"wifi_scan")||
           !strcmp(job_handlers[i].operation,"ble_scan")||!strcmp(job_handlers[i].operation,"hs_sniff"))duplicate=true;
        if(!duplicate) operations_used+=(size_t)snprintf(operations+operations_used,sizeof(operations)-operations_used,",\"%s\"",job_handlers[i].operation);
    }
#if CONFIG_PROJECTZERO_BOARD_WAVESHARE_ZERO
    const char *limit_json="\"targets\":8,\"exclusions\":32,\"batch\":64,\"aps\":8,\"clients\":32,\"probes\":64";
    const char *operation_limits="\"sae\":{\"targets\":1},\"sniffer\":{\"aps\":16,\"clients\":16,\"probes\":64},\"handshake\":{\"aps\":8,\"clients\":32,\"exchanges\":4}";
    const char *memory_limits=",\"memory_limits\":{\"observation_queue\":64,\"hs_frames\":8,\"max_frame_bytes\":2304,\"job_internal_bytes\":49152,\"min_internal_heap\":32768}";
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#if CONFIG_SPIRAM
    const char *limit_json="\"targets\":16,\"exclusions\":32,\"batch\":256,\"aps\":64,\"clients\":128,\"probes\":200";
#else
    const char *limit_json="\"targets\":16,\"exclusions\":32,\"batch\":128,\"aps\":64,\"clients\":128,\"probes\":200";
#endif
    const char *operation_limits="\"sae\":{\"targets\":1},\"sniffer\":{\"aps\":100,\"clients\":50,\"probes\":200},\"handshake\":{\"aps\":64,\"clients\":128,\"exchanges\":32}";
    const char *memory_limits="";
#else
#if CONFIG_SPIRAM
    const char *limit_json="\"targets\":16,\"exclusions\":32,\"batch\":256,\"aps\":64,\"clients\":128,\"probes\":200";
#else
    const char *limit_json="\"targets\":16,\"exclusions\":32,\"batch\":128,\"aps\":64,\"clients\":128,\"probes\":200";
#endif
    const char *operation_limits="\"sae\":{\"targets\":1},\"sniffer\":{\"aps\":100,\"clients\":50,\"probes\":200},\"handshake\":{\"aps\":64,\"clients\":128,\"exchanges\":32}";
    const char *memory_limits="";
#endif
    static const uint8_t all_channels[]={1,2,3,4,5,6,7,8,9,10,11,36,40,44,48,52,56,60,64,100,104,108,112,116,120,124,128,132,136,140,
#if PROJECTZERO_XIAO_US_CH144
        144,
#endif
        149,153,157,161,165};
    char channels[192];size_t channel_used=0;
    for(unsigned i=0;i<sizeof(all_channels);i++) {
        if(!valid_channel(all_channels[i]))continue;
        int wrote=snprintf(channels+channel_used,sizeof(channels)-channel_used,"%s%u",channel_used?",":"",all_channels[i]);
        if(wrote<=0||(size_t)wrote>=sizeof(channels)-channel_used) {
            output("{\"v\":3,\"kind\":\"error\",\"error\":\"device_info_channels_failed\"}");
            return 1;
        }
        channel_used+=(size_t)wrote;
    }
    const esp_app_desc_t *app_desc=esp_app_get_description();
    if(!app_desc) {
        output("{\"v\":3,\"kind\":\"error\",\"error\":\"device_info_app_description_unavailable\"}");
        return 1;
    }
    char build_id[17] = {0};
    /* CONFIG_APP_RETRIEVE_LEN_ELF_SHA may format only nine hex digits. The
     * app descriptor always retains the complete raw digest, so take its
     * first eight bytes for the protocol's stable 16-character build ID. */
    hex(build_id,app_desc->app_elf_sha256,8);
    if(!firmware_ready) {
        operations[0]=0; channels[0]=0; updates=boot.ota_available?"[\"usb-ota\",\"rom\"]":"[\"rom\"]";
        /* Keep the v3 limit shape for hosts through WDG 0.10.3 while making
         * every scan capacity zero in recovery mode. */
        operation_limits="";
        limit_json="\"targets\":0,\"exclusions\":0,\"batch\":0,\"aps\":0,\"clients\":0,\"probes\":0";
        memory_limits="";
    }
    char b[1100]; int n=snprintf(b,sizeof(b),"{\"v\":3,\"kind\":\"device_info\",\"device_id\":\"%s\",\"boot_id\":\"%s\",\"board\":\"%s\",\"flash_bytes\":%u,\"psram_bytes\":%u,\"legacy_busy\":%s,\"capabilities\":{\"c5_jobs_v1\":%s},\"operations\":[%s],\"channels\":[%s],\"limits\":{%s},\"operation_limits\":{%s},\"updates\":%s%s,\"firmware_version\":\"%s\",\"build_id\":\"%s\",\"boot_state\":\"%s\",\"boot_reason\":\"%s\"}",device_id,boot_id,board,(unsigned)flash,(unsigned)psram,legacy_busy?"true":"false",firmware_ready?"true":"false",operations,channels,limit_json,operation_limits,updates,memory_limits,app_desc->version,build_id,firmware_ready?"ready":"degraded",sw_boot_reason_name(boot.reason));
    if(n<=0 || n>1000) {
        output(n>1000 ? "{\"v\":3,\"kind\":\"error\",\"error\":\"device_info_too_large\",\"max_line\":1000}"
                      : "{\"v\":3,\"kind\":\"error\",\"error\":\"device_info_serialization_failed\",\"max_line\":1000}");
        return 1;
    }
    output(b); return 0;
}
static bool job_boot_id_matches(const char *request_boot) {
    /* Hex nonce casing is presentation only; length and every digit remain exact. */
    const size_t nonce_len=sizeof(boot_id)-1;
    if(!request_boot || strlen(request_boot)!=nonce_len || strlen(boot_id)!=nonce_len) return false;
    for(size_t i=0;i<nonce_len;i++) {
        unsigned char request=(unsigned char)request_boot[i];
        unsigned char current=(unsigned char)boot_id[i];
        if(!isxdigit(request) || !isxdigit(current) ||
           tolower(request)!=tolower(current)) return false;
    }
    return true;
}
static int job_start(int argc,char **argv) {
    if(argc!=3 || !argv[1][0] || strlen(argv[1])>32) return 1;
    for(const char *p=argv[1];*p;p++) if(!isalnum((unsigned char)*p) && *p!='-' && *p!='_') return 1;
    if(!ensure_identity()) return 1;
    if(atomic_load(&sequence_exhausted)) { job_request_error_for(argv[1],"sequence_exhausted");return 1; }
    char *json=heap_caps_malloc(4097,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT); size_t json_len=0;
    if(!json) { job_event_for(argv[1],"error","error","memory_admission_failed"); return 1; }
    if(!decode_job_json(argv[2],json,4097,&json_len)) { heap_caps_free(json); job_event_for(argv[1],"error","error","invalid_payload"); return 1; }
    uint32_t hash=request_hash(json,json_len);
    if(sw_active()) {
        if(!strcmp(argv[1],job_token)) {
            heap_caps_free(json);
            if(hash==job_request_hash) return 0;
            job_request_error_for(argv[1],"token_conflict"); return 1;
        }
        heap_caps_free(json); job_request_error_for(argv[1],"busy"); return 1;
    }
    /* Never let sw_prepare/operation handlers preempt a legacy Wi-Fi or BLE
       operation. The probe is supplied by main.c and is read-only. */
    if(uota_busy()) {
        heap_caps_free(json); job_request_error_for(argv[1],"update_busy"); return 1;
    }
    if(radio_busy_probe && radio_busy_probe()) {
        heap_caps_free(json); job_request_error_for(argv[1],"external_busy"); return 1;
    }
    if(external_supervisor_task) {
        /* A completed or failed external job may have emitted its terminal
         * record just before the supervisor drops its last shared-state use.
         * Wait for that retirement before admitting a replacement. */
        if(atomic_load(&external_job_active) || atomic_load(&external_job_starting) ||
           !atomic_load(&external_job_ready) || !wait_external_supervisor_retirement(3000)) {
            heap_caps_free(json); job_request_error_for(argv[1],"busy_cleanup"); return 1;
        }
    }
    for(unsigned i=0;i<4;i++) if(terminal_jobs[i].used && !strcmp(terminal_jobs[i].token,argv[1])) {
        heap_caps_free(json);
        if(terminal_jobs[i].hash==hash) return 0;
        job_request_error_for(argv[1],"token_conflict"); return 1;
    }
    const char *request_boot=NULL,*op=NULL,*band=NULL,*ble=NULL,*scope=NULL;
#if CONFIG_PROJECTZERO_BOARD_WAVESHARE_ZERO
    const char *storage=NULL;
#endif
    cJSON *root=cJSON_ParseWithLength(json,json_len);
    if(!cJSON_IsObject(root) || !json_string(root,"boot_id",&request_boot) || !job_boot_id_matches(request_boot) ||
       !json_string(root,"op",&op) || !json_string(root,"band",&band) || !json_string(root,"ble",&ble)) goto invalid;
    bool wardrive=!strcmp(op,"wardrive"), wifi=!strcmp(op,"wifi_scan"), ble_only=!strcmp(op,"ble_scan"), hs=!strcmp(op,"hs_sniff");
    bool built_in=wardrive||wifi||ble_only||hs;
    bool band24=!strcmp(band,"wifi24"), band5=!strcmp(band,"wifi5"), both=!strcmp(band,"both"), none=!strcmp(band,"none");
    if(!band24&&!band5&&!both&&!none) goto invalid;
    if((ble_only && strcmp(ble,"scan")) || ((wifi||hs) && strcmp(ble,"off")) ||
       ((wardrive) && strcmp(ble,"scan") && strcmp(ble,"off")) || (built_in&&none&&!ble_only)) goto invalid;
    job_channel_count=0;
    cJSON *channels=cJSON_GetObjectItemCaseSensitive(root,"channels");
    if(channels && !cJSON_IsArray(channels)) goto invalid;
    if(channels) {
        int count=cJSON_GetArraySize(channels); if(count>40) goto invalid;
        for(int i=0;i<count;i++) {
            cJSON *ch=cJSON_GetArrayItem(channels,i); if(!cJSON_IsNumber(ch) || ch->valuedouble<1 || ch->valuedouble>177 || ch->valuedouble!=(double)(unsigned)ch->valuedouble) goto invalid;
            unsigned value=(unsigned)ch->valuedouble;
            if(!valid_channel(value) || (value<=14&&!band24&&!both) || (value>14&&!band5&&!both)) goto invalid;
            for(unsigned j=0;j<job_channel_count;j++) if(job_channels[j]==value) goto invalid;
            job_channels[job_channel_count++]=value;
        }
    } else if(!none) {
        static const uint8_t defaults[]={1,6,11,2,3,4,5,7,8,9,10,36,40,44,48,52,56,60,64,100,104,108,112,116,120,124,128,132,136,140,
#if PROJECTZERO_XIAO_US_CH144
            144,
#endif
            149,153,157,161,165};
        for(unsigned i=0;i<sizeof(defaults);i++) if(valid_channel(defaults[i]) &&
            ((defaults[i]<=14&&(band24||both)) || (defaults[i]>14&&(band5||both)))) job_channels[job_channel_count++]=defaults[i];
    }
    if((none&&job_channel_count) || (!none&&!job_channel_count)) goto invalid;
    job_dwell_ms=160;
    cJSON *dwell=cJSON_GetObjectItemCaseSensitive(root,"dwell_ms");
    if(dwell) { if(!cJSON_IsNumber(dwell) || dwell->valuedouble<40 || dwell->valuedouble>2000 || dwell->valuedouble!=(double)(unsigned)dwell->valuedouble) goto invalid; job_dwell_ms=(unsigned)dwell->valuedouble; }
    cJSON *options=cJSON_GetObjectItemCaseSensitive(root,"options");
    if(options) {
        if(!cJSON_IsObject(options)) goto invalid;
        for(cJSON *item=options->child;item;item=item->next) {
            if(!item->string) goto invalid;
            if(!strcmp(item->string,"storage")) {
                if(!cJSON_IsString(item))goto invalid;
                if(strcmp(item->valuestring,"serial") && strcmp(item->valuestring,"sd"))goto invalid;
#if CONFIG_PROJECTZERO_BOARD_WAVESHARE_ZERO
                if(strcmp(item->valuestring,"serial"))goto unsupported;
#endif
            } else if(!strcmp(item->string,"scope")) {
                if(!cJSON_IsString(item))goto invalid;
                if(strcmp(item->valuestring,"all")&&strcmp(item->valuestring,"selected"))goto invalid;
            } else if(!strcmp(item->string,"ssid_hex")) {
                if(!cJSON_IsString(item)||strlen(item->valuestring)>64||(strlen(item->valuestring)&1))goto invalid;
                for(const char *p=item->valuestring;*p;p++)if(!isxdigit((unsigned char)*p))goto invalid;
            } else if(!strcmp(item->string,"mac")) {
                if(!cJSON_IsString(item)||strlen(item->valuestring)!=17)goto invalid;
            } else goto invalid;
        }
#if CONFIG_PROJECTZERO_BOARD_WAVESHARE_ZERO
        if(json_string(options,"storage",&storage) && !strcmp(storage,"sd")) goto unsupported;
#endif
    }
    cJSON *targets=cJSON_GetObjectItemCaseSensitive(root,"targets"),*exclude=cJSON_GetObjectItemCaseSensitive(root,"exclude");
    if((targets&&!cJSON_IsArray(targets)) || (exclude&&!cJSON_IsArray(exclude))) goto invalid;
    unsigned max_targets=16;
#if CONFIG_PROJECTZERO_BOARD_WAVESHARE_ZERO
    max_targets=8;
#endif
    if((targets&&cJSON_GetArraySize(targets)>(int)max_targets) || (exclude&&cJSON_GetArraySize(exclude)>32)) goto invalid;
    uint8_t parsed_targets[JOB_TARGET_MAX][6],parsed_exclusions[JOB_EXCLUSION_MAX][6];
    size_t parsed_target_count=targets?(size_t)cJSON_GetArraySize(targets):0;
    size_t parsed_exclusion_count=exclude?(size_t)cJSON_GetArraySize(exclude):0;
    for(size_t i=0;i<parsed_target_count;i++) {
        cJSON *target=cJSON_GetArrayItem(targets,(int)i);
        cJSON *bssid=cJSON_GetObjectItemCaseSensitive(target,"bssid"),*channel=cJSON_GetObjectItemCaseSensitive(target,"channel");
        cJSON *auth=cJSON_GetObjectItemCaseSensitive(target,"auth");
        if(!cJSON_IsObject(target)||!cJSON_IsNumber(auth)||auth->valuedouble<0||auth->valuedouble>255||
           auth->valuedouble!=(double)(unsigned)auth->valuedouble||!json_string(target,"bssid",&scope)||
           !parse_mac_string(scope,parsed_targets[i])||!cJSON_IsNumber(channel)||channel->valuedouble<1||channel->valuedouble>177||
           channel->valuedouble!=(double)(unsigned)channel->valuedouble||!valid_channel((unsigned)channel->valuedouble))goto invalid;
        if((channel->valuedouble<=14&&!band24&&!both)||(channel->valuedouble>14&&!band5&&!both))goto invalid;
        if(!none) {
            bool in_plan=false;
            for(unsigned ch=0;ch<job_channel_count;ch++)if(job_channels[ch]==(uint8_t)channel->valuedouble)in_plan=true;
            if(!in_plan)goto invalid;
        }
        for(size_t j=0;j<i;j++)if(!memcmp(parsed_targets[i],parsed_targets[j],6))goto invalid;
        cJSON *ssid=cJSON_GetObjectItemCaseSensitive(target,"ssid_hex");
        if(ssid) {
            if(!cJSON_IsString(ssid)||strlen(ssid->valuestring)>64||(strlen(ssid->valuestring)&1))goto invalid;
            for(const char *p=ssid->valuestring;*p;p++)if(!isxdigit((unsigned char)*p))goto invalid;
        }
        (void)bssid;
    }
    for(size_t i=0;i<parsed_exclusion_count;i++) {
        cJSON *entry=cJSON_GetArrayItem(exclude,(int)i);
        if(!cJSON_IsString(entry)||!parse_mac_string(entry->valuestring,parsed_exclusions[i]))goto invalid;
        for(size_t j=0;j<i;j++)if(!memcmp(parsed_exclusions[i],parsed_exclusions[j],6))goto invalid;
        for(size_t j=0;j<parsed_target_count;j++)if(!memcmp(parsed_exclusions[i],parsed_targets[j],6))goto invalid;
    }
    if(!sw_job_set_filters((const uint8_t (*)[6])parsed_targets,parsed_target_count,
                           (const uint8_t (*)[6])parsed_exclusions,parsed_exclusion_count))goto invalid;
    if(!built_in) {
        char operation[24];
        snprintf(operation,sizeof(operation),"%s",op);
        sw_job_operation_handler_t handler=NULL;
        for(unsigned i=0;i<job_handler_count;i++) if(!strcmp(operation,job_handlers[i].operation)) { handler=job_handlers[i].handler;break; }
        if(!handler) goto unsupported;
        snprintf(job_token,sizeof(job_token),"%s",argv[1]); job_request_hash=hash; job_mode=true;
        atomic_store(&drops,0); reset_losses(); atomic_store(&wifi_count,0); atomic_store(&ble_count,0);
        sw_job_set_sniffer_stats(0,0,0);
        atomic_store(&lease,now_ms());
        atomic_store(&external_job_active,false);
        atomic_store(&external_stop_claimed,false);
        atomic_store(&external_job_ready,false);
        atomic_store(&external_operation_ready,false);
        atomic_store(&external_job_starting,true);
        atomic_store(&external_job_stopping,false);
        atomic_store(&external_stop_completed,false);
        snprintf(active_job_operation,sizeof(active_job_operation),"%s",operation);
        /* Allocate the lease/cleanup supervisor before any operation can touch
           the radio. It waits behind external_job_ready until start succeeds. */
        sw_job_emit_memory_snapshot("supervisor_create");
        if(xTaskCreate(external_job_supervisor,"job_supervisor",6144,NULL,3,&external_supervisor_task)!=pdPASS) {
            external_supervisor_task=NULL; job_mode=false;
            atomic_store(&external_job_starting,false);
            job_event_for(argv[1],"error","error","supervisor_start_failed");
            remember_terminal_job(argv[1],hash);
            cJSON_Delete(root);heap_caps_free(json);
            return 1;
        }
        int rc=handler(root,argv[1]);
        if(rc==SW_JOB_START_READY || rc==SW_JOB_START_PENDING) {
            atomic_store(&external_job_active,true);
            atomic_store(&lease,now_ms());
            if(rc==SW_JOB_START_READY)atomic_store(&external_job_starting,false);
#if CONFIG_PROJECTZERO_BOARD_WAVESHARE_ZERO
            if(heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<ZERO_JOB_INTERNAL_RESERVE) {
                /* Keep the supervisor behind its start barrier while this
                   thread owns low-memory cleanup. */
                atomic_store(&external_job_stopping,true);
                atomic_store(&external_stop_claimed,true);
                sw_job_operation_stop_t stop=NULL;
                for(unsigned i=0;i<job_handler_count;i++) if(!strcmp(operation,job_handlers[i].operation)) { stop=job_handlers[i].stop;break; }
                if(stop && stop(argv[1])==0) {
                    atomic_store(&external_job_active,false);active_job_operation[0]=0;job_mode=false;
                    atomic_store(&external_job_ready,true);
                    job_event_for(argv[1],"error","error","memory_floor_failed");
                    remember_terminal_job(argv[1],hash);
                } else {
                    /* Keep token ownership and the supervisor alive until the
                       operation's cleanup callback succeeds. */
                    atomic_store(&lease,now_ms()-15000);
                    atomic_store(&external_stop_claimed,false);
                    sw_job_emit_lifecycle("status","stopping","\"message\":\"memory_floor_cleanup_pending\"");
                    atomic_store(&external_job_ready,true);
                }
                cJSON_Delete(root);heap_caps_free(json);
                return 1;
            }
#endif
            atomic_store(&external_job_ready,true);
            if(rc==SW_JOB_START_READY)sw_job_emit_lifecycle("started","running","");
            else sw_job_emit_lifecycle("status","starting","");
        }
        else {
            atomic_store(&external_job_active,false); atomic_store(&external_job_ready,true);
            atomic_store(&external_job_starting,false);
            active_job_operation[0]=0; job_mode=false;
            job_event_for(argv[1],"error","error","operation_start_failed"); remember_terminal_job(argv[1],hash);
        }
        cJSON_Delete(root); heap_caps_free(json);
        return rc==SW_JOB_START_PENDING?0:rc;
    }
    char oui_arg[65]={0}; cJSON *ouis=cJSON_GetObjectItemCaseSensitive(root,"live_ouis");
    if(ouis) {
        if(!cJSON_IsArray(ouis)||cJSON_GetArraySize(ouis)>LIVE_OUI_MAX) goto invalid;
        if((hs||ble_only)&&cJSON_GetArraySize(ouis)>0)goto invalid;
        size_t used=(size_t)snprintf(oui_arg,sizeof(oui_arg),"live_oui=");
        for(int i=0;i<cJSON_GetArraySize(ouis);i++) {
            cJSON *entry=cJSON_GetArrayItem(ouis,i); if(!cJSON_IsString(entry)||strlen(entry->valuestring)!=6) goto invalid;
            if(i) oui_arg[used++]=',';
            for(unsigned j=0;j<6;j++) if(!isxdigit((unsigned char)entry->valuestring[j])) goto invalid;
            memcpy(oui_arg+used,entry->valuestring,6); used+=6; oui_arg[used]=0;
        }
        if(used==9) oui_arg[0]=0;
        if(hs||ble_only)oui_arg[0]=0;
    }
    snprintf(job_token,sizeof(job_token),"%s",argv[1]); job_request_hash=hash; job_mode=true;
    collect_wifi=!ble_only; collect_ble=!wifi&&!hs && strcmp(ble,"off");
    const char *command=hs?"start_hs_sniff_serial":ble_only?"start_wardrive_batch_serial":wifi?"start_wardrive_wifi_batch_serial":"start_wardrive_batch_serial";
    char *start_argv[3]={(char *)command,argv[1],oui_arg};
    in_job_dispatch=true;
    int rc=start(oui_arg[0]?3:2,start_argv);
    in_job_dispatch=false;
    cJSON_Delete(root);
    heap_caps_free(json);
    if(rc!=0) { remember_terminal_job(argv[1],hash);job_mode=false;return rc; }
    return 0;
invalid:
    cJSON_Delete(root); heap_caps_free(json); job_event_for(argv[1],"error","error","invalid_request"); return 1;
unsupported:
    cJSON_Delete(root); heap_caps_free(json); job_event_for(argv[1],"error","error","unsupported_operation_or_scope"); return 1;
}
static void external_job_supervisor(void *arg) {
    (void)arg;
    while(!atomic_load(&external_job_ready)) vTaskDelay(pdMS_TO_TICKS(20));
    while(atomic_load(&external_job_active)) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if(!atomic_load(&external_job_active)) break;
        if(atomic_load(&external_job_starting) && atomic_load(&external_operation_ready) &&
           !atomic_load(&external_job_stopping)) {
            atomic_store(&external_job_starting,false);
            sw_job_emit_lifecycle("started","running","");
        }
        if(atomic_load(&external_job_stopping) || now_ms()-atomic_load(&lease)>=15000) {
            const char *stop_fields=now_ms()-atomic_load(&lease)>=15000?
                "\"message\":\"lease_expired\"":"\"message\":\"host_stop\"";
            atomic_store(&external_job_stopping,true);
            bool expected=false;
            if(atomic_compare_exchange_strong(&external_stop_claimed,&expected,true) && atomic_load(&external_job_active)) {
                sw_job_emit_lifecycle("status","stopping",stop_fields);
                sw_job_operation_stop_t stop=NULL;
                for(unsigned i=0;i<job_handler_count;i++) if(!strcmp(active_job_operation,job_handlers[i].operation)) { stop=job_handlers[i].stop;break; }
                if(stop && stop(job_token)==0) {
                    atomic_store(&external_job_active,false);
                    sw_job_emit_lifecycle("stopped","stopped",stop_fields);
                    sw_job_remember_terminal();
                    active_job_operation[0]=0;
                    job_mode=false;
                } else {
                    /* Keep the lease owner live until radio cleanup succeeds.
                       The next two-second supervisor pass retries safely. */
                    sw_job_emit_lifecycle("status","stopping", "\"message\":\"cleanup_retry\"");
                    atomic_store(&external_stop_claimed,false);
                    continue;
                }
            }
            if(!atomic_load(&external_job_active))break;
            continue;
        }
        sw_job_emit_lifecycle("heartbeat",job_current_state(),"");
    }
    /* The console stop path already performed radio cleanup. The supervisor
       owns the terminal record so it is guaranteed even if the waiter times
       out; new starts remain gated on this handle until all shared-state use
       is complete. */
    if(atomic_load(&external_stop_completed))
        finalize_external_host_stop();
    external_supervisor_task=NULL;
    vTaskDelete(NULL);
}
static int job_keepalive(int argc,char **argv) {
    if(argc!=2 || strcmp(argv[1],job_token) || !sw_active()) return 1;
    atomic_store(&lease,now_ms()); return 0;
}
static int job_status(int argc,char **argv) {
    if(argc!=2) return 1;
    if(strcmp(argv[1],job_token)) {
        for(unsigned i=0;i<4;i++) if(terminal_jobs[i].used && !strcmp(argv[1],terminal_jobs[i].token)) {
            job_event_for(argv[1],"status","stopped","");return 0;
        }
        return 1;
    }
    if(!job_token[0])return 1;
    if(!job_mode && !sw_active()) {
        job_event_for(argv[1],"status","stopped","");
        return 0;
    }
    const char *state=job_current_state();
    if(job_mode) sw_job_emit_lifecycle("status",state,"");
    else batch_status("status",state,"");
    return 0;
}
static int job_stop(int argc,char **argv) {
    if(argc!=2 || strcmp(argv[1],job_token)) return 1;
    if(atomic_load(&external_job_active) || atomic_load(&external_stop_completed)) {
        if(atomic_load(&external_job_active)) {
            atomic_store(&external_job_stopping,true);
            bool expected=false;
            if(!atomic_compare_exchange_strong(&external_stop_claimed,&expected,true)) return 1;
            sw_job_operation_stop_t stop=NULL;
            for(unsigned i=0;i<job_handler_count;i++) if(!strcmp(active_job_operation,job_handlers[i].operation)) { stop=job_handlers[i].stop;break; }
            if(!stop || stop(argv[1])!=0) { atomic_store(&external_stop_claimed,false); sw_job_emit_lifecycle("status","stopping", "\"message\":\"stop_retry\""); return 1; }
            atomic_store(&external_stop_completed,true);
            atomic_store(&external_job_active,false);
        }
        /* The supervisor may already have finalized the stop after the
           inactive store above. Waiting for retirement remains valid. */
        if(!wait_external_supervisor_retirement(3000)) {
            sw_job_emit_lifecycle("status","stopping", "\"message\":\"supervisor_retire_pending\"");
            return 1;
        }
        /* The retiring supervisor emitted the terminal record before clearing
           its handle, so it cannot race a replacement job's shared globals. */
        return 0;
    }
    atomic_store(&stopping,true);
    if(!sw_stop()) { job_event("error","stopping","stop_timeout"); return 1; }
    return 0;
}
static int job_device_info(int argc,char **argv) { return device_info(argc,argv); }
/* All main console commands share an ownership gate, including attack commands.
 * Registration retains the original handlers and changes no idle behavior. */
static struct { const char *name; esp_console_cmd_func_t func; } commands[256];
static unsigned command_count;
static int dispatch(int argc,char **argv) {
    if(argc<1 || !argv || !argv[0]) return 1;
    sw_boot_snapshot_t boot = sw_get_boot_snapshot();
    if(boot.state==BOOT_INITIALIZING && strcmp(argv[0],"version") &&
       strcmp(argv[0],"get_capabilities") && strcmp(argv[0],"get_device_info")) {
        printf("Boot initialization is in progress; command rejected.\n");
        return 1;
    }
    if(uota_busy() && strncmp(argv[0],"uota_",5) && strcmp(argv[0],"version") && strcmp(argv[0],"ota_info") && strcmp(argv[0],"get_capabilities")) {
        printf("USB OTA is active; finish or abort it first.\n");return 1;
    }
    if(sw_active() && strcmp(argv[0],"stop") && strcmp(argv[0],"get_capabilities") && strcmp(argv[0],"get_device_info") && strcmp(argv[0],"wardrive_keepalive") && strcmp(argv[0],"wardrive_status") &&
       strcmp(argv[0],"job_start") && strcmp(argv[0],"job_keepalive") && strcmp(argv[0],"job_status") && strcmp(argv[0],"job_stop")) {
        printf("Serial wardrive busy; stop first.\n"); return 1;
    }
#if CONFIG_PROJECTZERO_BOARD_WAVESHARE_ZERO
    static const char *const zero_commands[]={"get_device_info","get_capabilities","start_wardrive_serial","start_wardrive_wifi_serial",
        "start_wardrive_batch_serial","start_wardrive_wifi_batch_serial","start_hs_sniff_serial","wardrive_keepalive",
        "wardrive_status","stop","job_start","job_keepalive","job_status","job_stop","uota_status",
        "uota_begin","uota_chunk","uota_block","uota_finish","uota_abort"};
    bool supported=false;
    for(unsigned i=0;i<sizeof(zero_commands)/sizeof(zero_commands[0]);i++) if(!strcmp(argv[0],zero_commands[i])) { supported=true;break; }
    if(!supported) { printf("Operation unsupported on waveshare-zero profile.\n"); return 1; }
#endif
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
        {.command="get_device_info",.help="Machine-readable device and job capabilities",.func=job_device_info},
        {.command="job_start",.help="Start a token-owned serial job: <token> <base64url-json>",.func=job_start},
        {.command="job_keepalive",.help="Renew a token-owned job lease: <token>",.func=job_keepalive},
        {.command="job_status",.help="Read token-owned job status: <token>",.func=job_status},
        {.command="job_stop",.help="Stop token-owned job: <token>",.func=job_stop},
        {.command="start_wardrive_serial",.help="WiFi + BLE over serial, no GPS/SD: <session>",.func=start},
        {.command="start_wardrive_wifi_serial",.help="WiFi only over serial for host BLE, no GPS/SD: <session>",.func=start},
        {.command="start_wardrive_batch_serial",.help="10s batched WiFi + BLE: <session> [live_oui=HHHHHH,...]",.func=start},
        {.command="start_wardrive_wifi_batch_serial",.help="10s batched WiFi only: <session> [live_oui=HHHHHH,...]",.func=start},
        {.command="start_hs_sniff_serial",.help="Passive EAPOL/PMKID and management PCAP over serial, no SD: <session>",.func=start},
        {.command="wardrive_keepalive",.help="Renew serial session lease: <session>",.func=keepalive},
        {.command="wardrive_status",.help="Repeat current batched wardrive state: <session>",.func=wardrive_status},
        {.command="get_capabilities",.help="Machine-readable serial capabilities",.func=capabilities}};
    for(unsigned i=0;i<sizeof(cmds)/sizeof(cmds[0]);i++) ESP_ERROR_CHECK(sw_register_command(&cmds[i]));
}
void sw_register_recovery(void) {
    const esp_console_cmd_t info={.command="get_device_info",.help="Read firmware recovery identity",.func=job_device_info};
    const esp_console_cmd_t caps={.command="get_capabilities",.help="Read recovery capabilities",.func=capabilities};
    ESP_ERROR_CHECK(sw_register_command(&info));
    ESP_ERROR_CHECK(sw_register_command(&caps));
}
