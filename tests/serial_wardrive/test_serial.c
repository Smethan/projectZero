/* Compile the real transport/callback code against in-memory radio/RTOS shims. */
#include "../../ESP32C5/main/serial_wardrive.c"
static bool radio_ok=true, prepare_ok=true;
static int stopped_radios, hops;
bool sw_prepare(void){return prepare_ok;}
bool sw_radio_start(void){return radio_ok;}
void sw_radio_stop(void){stopped_radios++;}
void sw_radio_hop(unsigned i){hops++;}
static void fresh(void){
    atomic_store(&active,false);atomic_store(&collecting,false);
    test_clock=1000;output_capture[0]=0;transport_limit=1024;radio_ok=true;
    char *args[]={"start_wardrive_serial","fixture"};assert(start(2,args)==0);
}
static void beacon(unsigned subtype,unsigned length){
    wifi_promiscuous_pkt_t p={.rx_ctrl={.sig_len=length,.channel=6,.rssi=-55}};
    p.payload[0]=subtype<<4;memset(p.payload+4,0xff,6);
    uint8_t addr[]={0xb4,0x1e,0x52,0,0,1};memcpy(p.payload+10,addr,6);memcpy(p.payload+16,addr,6);
    unsigned pos=subtype==4?24:36;
    p.payload[pos]=0;p.payload[pos+1]=3;memcpy(p.payload+pos+2,"a\"b",3);
    wifi_cb(&p,WIFI_PKT_MGMT);
}
int main(void){
    fresh();atomic_store(&collecting,true);
    beacon(8,45);observation o;assert(xQueueReceive(queue,&o,0));emit(&o);
    assert(strstr(output_capture,"\"kind\":\"wifi\""));assert(strstr(output_capture,"615c") == NULL);
    assert(strstr(output_capture,"612262"));assert(strstr(output_capture,"B4:1E:52:00:00:01"));
    xQueueReset(queue);output_capture[0]=0;beacon(4,33);assert(xQueueReceive(queue,&o,0));emit(&o);
    assert(strstr(output_capture,"wifi_mgmt"));assert(!strstr(output_capture,"\"kind\":\"wifi\""));
    xQueueReset(queue);beacon(8,20);assert(!uxQueueMessagesWaiting(queue));
    uint8_t a[]={1,0,0,0,0,0xc2},ad[]={4,9,'a','b','c'};
    sw_ble(a,1,-60,0,ad,sizeof(ad));assert(xQueueReceive(queue,&o,0));emit(&o);
    assert(strstr(output_capture,"C2:00:00:00:00:01"));
    sw_ble(a,1,-61,0,ad,sizeof(ad));assert(!uxQueueMessagesWaiting(queue)); /* RSSI-only repeat */
    sw_ble(a,1,-61,4,ad,sizeof(ad));assert(uxQueueMessagesWaiting(queue)==1); /* scan response */
    xQueueReset(queue);
    for(int i=0;i<100;i++){a[0]=i;sw_ble(a,1,-60,0,ad,sizeof(ad));}
    assert(uxQueueMessagesWaiting(queue)==64);assert(atomic_load(&drops)>0);
    assert(xQueueReceive(queue,&o,0));test_clock+=2001;unsigned old=atomic_load(&drops);emit(&o);assert(atomic_load(&drops)==old+1);
    transport_limit=1;old=atomic_load(&drops);status("stats","");assert(atomic_load(&drops)==old+1);
    fresh();test_task(NULL);assert(!sw_active());assert(hops>0 && stopped_radios>0);
    assert(strstr(output_capture,"started") && strstr(output_capture,"stats") && strstr(output_capture,"stopped"));
    fresh();radio_ok=false;test_task(NULL);assert(!strstr(output_capture,"started"));assert(strstr(output_capture,"error") && strstr(output_capture,"stopped"));
    fresh();char *bad[]={"wardrive_keepalive","other"};assert(keepalive(2,bad)==1);
    test_clock+=500;char *good[]={"wardrive_keepalive","fixture"};assert(keepalive(2,good)==0);assert(atomic_load(&lease)==test_clock);
    atomic_store(&stopping,true);test_task(NULL);assert(!strstr(output_capture,"started"));assert(strstr(output_capture,"stopped"));
    char *hs_args[]={"start_hs_sniff_serial","passive"};
    assert(start(2,hs_args)==0 && sw_hs_mode());
    atomic_store(&collecting,true);output_capture[0]=0;
    wifi_promiscuous_pkt_t hs={.rx_ctrl={.sig_len=340,.channel=6,.rssi=-50}};
    hs.payload[0]=0x08;hs.payload[1]=2;
    uint8_t llc[]={0xaa,0xaa,3,0,0,0,0x88,0x8e};memcpy(hs.payload+24,llc,8);
    hs.payload[32]=2;hs.payload[33]=3;hs.payload[34]=1;hs.payload[35]=44;
    assert(hs_capture_kind(hs.payload,336)==2);
    assert(hs_capture_kind(hs.payload,335)==0); /* truncated declared EAPOL */
    hs.payload[1]|=0x40;assert(!hs_capture_kind(hs.payload,336));hs.payload[1]=2;
    hs_wifi_cb(&hs,WIFI_PKT_DATA);hs_observation *raw;assert(xQueueReceive(hs_pool.ready,&raw,0));
    assert(raw->len==336);hs_emit(raw);capture_pool_release(&hs_pool,raw);
    assert(strstr(output_capture,"\"kind\":\"hs_packet\"") && strstr(output_capture,"\"offset\":240"));
    assert(!strstr(output_capture,"\"kind\":\"ble\""));
    hs.payload[30]=0x08;assert(!hs_capture_kind(hs.payload,336));hs.payload[30]=0x88;
    for(int i=0;i<10;i++) hs_wifi_cb(&hs,WIFI_PKT_DATA);
    assert(uxQueueMessagesWaiting(hs_pool.ready)==8 && atomic_load(&drops)>=2);
    atomic_store(&stopping,true);test_task(NULL);assert(!sw_active() && hs_pool.ready==NULL);
    assert(strstr(output_capture,"stopped"));
    task_ok=0;assert(start(2,hs_args)==1 && hs_pool.ready==NULL && !sw_active());task_ok=1;
    assert(!heap_live);
    prepare_ok=false;output_capture[0]=0;
    assert(start(2,hs_args)==1 && !sw_active() && !heap_live);
    assert(strstr(output_capture,"radio_prepare_failed"));prepare_ok=true;
    output_capture[0]=0;fail_psram=fail_internal=true;
    assert(start(2,hs_args)==1 && !sw_active() && !heap_live);
    assert(strstr(output_capture,"capture_allocation_failed"));
    fail_internal=false;assert(start(2,hs_args)==0); /* no PSRAM board */
    assert(last_heap_caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    atomic_store(&stopping,true);test_task(NULL);assert(!heap_live);
    hs_wifi_cb(&hs,WIFI_PKT_DATA); /* callback arriving after cleanup */
    fail_psram=false;
    assert(start(2,hs_args)==0);radio_ok=false;output_capture[0]=0;
    test_task(NULL);assert(!sw_active() && !heap_live && !hs_pool.ready);
    assert(strstr(output_capture,"radio_start_failed") && strstr(output_capture,"stopped"));
    fresh();assert(!sw_hs_mode());atomic_store(&stopping,true);test_task(NULL);
    char *wifi_args[]={"start_wardrive_wifi_serial","host-ble"};
    assert(start(2,wifi_args)==0 && sw_wifi_only_mode() && !sw_hs_mode());
    atomic_store(&collecting,true);output_capture[0]=0;
    sw_ble(a,1,-60,0,ad,sizeof(ad));assert(!uxQueueMessagesWaiting(queue));
    assert(atomic_load(&ble_count)==0);
    beacon(8,45);assert(xQueueReceive(queue,&o,0));emit(&o);
    assert(strstr(output_capture,"\"kind\":\"wifi\""));
    test_task(NULL);assert(!sw_active() && strstr(output_capture,"stats"));
    capabilities(0,NULL);assert(strstr(output_capture,"\"wardrive_wifi_serial_v1\":true"));
    fresh();assert(!sw_wifi_only_mode());atomic_store(&stopping,true);test_task(NULL);
    puts("PASS: WiFi/BLE + passive EAPOL framing, malformed/protected data, queue overflow, age/backpressure, lease, cleanup, mode transitions");
}
