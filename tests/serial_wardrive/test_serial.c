/* Compile the real transport/callback code against in-memory radio/RTOS shims. */
#include "../../ESP32C5/main/serial_wardrive.c"
static bool radio_ok=true;
static int stopped_radios, hops;
bool sw_prepare(void){return true;}
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
    puts("PASS: WiFi/BLE framing, management roles, malformed frames, dedup updates, queue overflow, age drops, backpressure, lease, partial startup, stop during start");
}
