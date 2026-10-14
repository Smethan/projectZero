/* Compile the real transport/callback code against in-memory radio/RTOS shims. */
#include "../../ESP32C5/main/serial_wardrive.c"
bool uota_busy(void){return false;}
static bool radio_ok=true, prepare_ok=true;
static int stopped_radios, hops, prepare_calls;
static bool inject_live;
bool sw_prepare(void){prepare_calls++;return prepare_ok;}
bool sw_radio_start(void){return radio_ok;}
bool sw_radio_stop(void){stopped_radios++;return true;}
void sw_radio_hop(unsigned i){
    hops++;
    if(inject_live && atomic_load(&collecting)) {
        uint8_t addr[]={1,0,0,0x52,0x1e,0xb4},ad[]={4,9,'a','b','c'};
        sw_ble(addr,0,-62,0,ad,sizeof(ad));inject_live=false;
    }
}
static size_t json_record_length(void) {
    const char *record=strstr(output_capture,"WDG:");
    assert(record);
    record+=4;
    return strcspn(record,"\r\n");
}
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
static void rsn_beacon(const uint8_t akms[][4],unsigned count){
    wifi_promiscuous_pkt_t p={.rx_ctrl={.channel=6,.rssi=-55}};
    p.payload[0]=0x80;p.payload[34]=0x10;
    uint8_t addr[]={0xb4,0x1e,0x52,0,0,1};memcpy(p.payload+10,addr,6);memcpy(p.payload+16,addr,6);
    unsigned pos=36;p.payload[pos++]=0;p.payload[pos++]=3;memcpy(p.payload+pos,"abc",3);pos+=3;
    p.payload[pos++]=48;p.payload[pos++]=(uint8_t)(14+4*count);
    p.payload[pos++]=1;p.payload[pos++]=0; /* RSN version */
    uint8_t group[]={0,0x0f,0xac,4};memcpy(p.payload+pos,group,4);pos+=4;
    p.payload[pos++]=1;p.payload[pos++]=0;memcpy(p.payload+pos,group,4);pos+=4;
    p.payload[pos++]=(uint8_t)count;p.payload[pos++]=0;
    for(unsigned i=0;i<count;i++){memcpy(p.payload+pos,akms[i],4);pos+=4;}
    p.rx_ctrl.sig_len=(uint16_t)(pos+4);wifi_cb(&p,WIFI_PKT_MGMT);
}
int main(void){
    char decoded_job[4097];size_t decoded_len=0;
    assert(decode_job_json("eyJ4IjoyfQ",decoded_job,sizeof(decoded_job),&decoded_len));
    assert(decoded_len==7 && !strcmp(decoded_job,"{\"x\":2}"));
    assert(!decode_job_json("A",decoded_job,sizeof(decoded_job),&decoded_len));
    char too_large_job[5464];memset(too_large_job,'A',5463);too_large_job[5463]=0;
    assert(!decode_job_json(too_large_job,decoded_job,sizeof(decoded_job),&decoded_len));
    output_capture[0]=0;
    assert(device_info(1,NULL)==0);
    assert(strstr(output_capture,"\"kind\":\"device_info\"") &&
           strstr(output_capture,"\"firmware_version\":\"1.8.4\"") &&
           strstr(output_capture,"\"build_id\":\"0123456789abcdef\"") &&
           strstr(output_capture,"\"boot_state\":\"ready\"") &&
           strstr(output_capture,"\"capabilities\":{\"c5_jobs_v1\":true}") &&
           strstr(output_capture,"\"board\":\"xiao\"") &&
           strstr(output_capture,"\"updates\":[\"usb-ota\",\"rom\"]") &&
           strstr(output_capture,"\"operation_limits\"") && json_record_length()<=1000);
    sw_set_boot_state(false,"psram_buffer_failed",false);
    output_capture[0]=0;assert(device_info(1,NULL)==0);
    assert(json_record_length()<=1000 && strstr(output_capture,"\"boot_state\":\"degraded\"") &&
           strstr(output_capture,"\"boot_reason\":\"psram_buffer_failed\"") &&
           strstr(output_capture,"\"build_id\":\"0123456789abcdef\"") &&
           strstr(output_capture,"\"capabilities\":{\"c5_jobs_v1\":false}") &&
           strstr(output_capture,"\"operations\":[]") && strstr(output_capture,"\"channels\":[]") &&
           strstr(output_capture,"\"limits\":{\"targets\":0,\"exclusions\":0,\"batch\":0,\"aps\":0,\"clients\":0,\"probes\":0}"));
    output_capture[0]=0;assert(capabilities(1,NULL)==0);
    assert(strstr(output_capture,"\"c5_jobs_v1\":false") && !strstr(output_capture,"wardrive_serial_v1"));
    sw_set_boot_state(true,"normal",true);
    output_capture[0]=0;assert(device_info(1,NULL)==0);
    test_country=(wifi_country_t){.cc={'U','S',0},.schan=1,.nchan=11};
    test_country_available=true;
    assert(valid_channel(11) && !valid_channel(12) && !valid_channel(14));
    output_capture[0]=0;assert(device_info(1,NULL)==0);
    assert(strstr(output_capture,"\"channels\":[1,2,3,4,5,6,7,8,9,10,11,36") &&
           !strstr(output_capture,"\"channels\":[1,2,3,4,5,6,7,8,9,10,11,12"));
    test_country_available=false;

    /* The typed setter distinguishes policy rejection from driver and
       readback failures, and reports the actual channel only after verify. */
    job_mode=true;job_channel_count=1;job_channels[0]=6;
    test_wifi_mode=WIFI_MODE_APSTA;test_wifi_band=WIFI_BAND_MODE_2G_ONLY;
    sw_channel_result_t channel_result=sw_job_apply_channel_checked(11);
    assert(channel_result.status==SW_CHANNEL_NOT_IN_PLAN &&
           channel_result.requested_channel==11 && channel_result.sdk_error==ESP_ERR_INVALID_ARG);
    test_channel_result=77;channel_result=sw_job_apply_channel_checked(6);
    assert(channel_result.status==SW_CHANNEL_DRIVER_ERROR && channel_result.sdk_error==77 &&
           channel_result.mode==WIFI_MODE_APSTA && channel_result.band==WIFI_BAND_MODE_2G_ONLY);
    test_channel_result=ESP_OK;test_channel_readback_result=78;
    channel_result=sw_job_apply_channel_checked(6);
    assert(channel_result.status==SW_CHANNEL_READBACK_ERROR && channel_result.sdk_error==78);
    test_channel_readback_result=ESP_OK;test_channel_actual=11;test_channel_stale_readback=true;
    channel_result=sw_job_apply_channel_checked(6);
    assert(channel_result.status==SW_CHANNEL_READBACK_ERROR && channel_result.actual_channel!=6);
    test_channel_stale_readback=false;channel_result=sw_job_apply_channel_checked(6);
    assert(channel_result.status==SW_CHANNEL_OK && channel_result.actual_channel==6);
    job_mode=false;job_channel_count=0;test_channel_result=ESP_OK;
    output_capture[0]=0;int job_prepare_count=prepare_calls;
    char *invalid_job[]={"job_start","invalid-token","!"};
    assert(job_start(3,invalid_job)==1 && prepare_calls==job_prepare_count && !sw_active());
    assert(strstr(output_capture,"\"v\":3") && strstr(output_capture,"\"message\":\"invalid_payload\""));
    job_mode=true;memcpy(device_id,"aabbccddeeff",13);memcpy(boot_id,"0123456789abcdef",17);
    snprintf(job_token,sizeof(job_token),"wire-fixture");output_capture[0]=0;
    sw_job_emit_data("wifi","\"batch\":0,\"mac\":\"AA:BB:CC:DD:EE:FF\"");
    assert(strstr(output_capture,"\"v\":3")&&strstr(output_capture,"\"device_id\":\"aabbccddeeff\""));
    assert(strstr(output_capture,"\"boot_id\":\"0123456789abcdef\"")&&strstr(output_capture,"\"kind\":\"wifi\""));
    assert(!strstr(output_capture,"job:data")&&!strstr(output_capture,"data_kind"));
    output_capture[0]=0;sw_job_emit_lifecycle("heartbeat","running","");
    assert(strstr(output_capture,"\"kind\":\"heartbeat\"")&&strstr(output_capture,"\"batch\":0")&&strstr(output_capture,"\"wifi_count\":0")&&strstr(output_capture,"\"ble_count\":0")&&strstr(output_capture,"\"drops\":0")&&strstr(output_capture,"\"losses\":{\"queue\":0,\"age\":0,\"output\":0,\"saturation\":0}"));
    snprintf(active_job_operation,sizeof(active_job_operation),"sniffer");
    sw_job_set_sniffer_stats(UINT32_MAX,UINT32_MAX,UINT32_MAX);
    output_capture[0]=0;sw_job_emit_lifecycle("heartbeat","running","");
    assert(strstr(output_capture,"\"sniffer_packets\":4294967295") &&
           strstr(output_capture,"\"sniffer_aps\":4294967295") &&
           strstr(output_capture,"\"sniffer_drops\":4294967295") &&
           strlen(output_capture)<=1024);
    active_job_operation[0]=0;
    sw_job_set_sniffer_stats(0,0,0);
    output_capture[0]=0;sw_job_emit_legacy_line("HST:{\"kind\":\"scan_done\"}");
    assert(strstr(output_capture,"\"kind\":\"legacy\"")&&strstr(output_capture,"HST:{\\\"kind\\\":\\\"scan_done\\\"}"));
    job_mode=false;

    fresh();atomic_store(&collecting,true);
    beacon(8,45);observation o;assert(xQueueReceive(queue,&o,0));emit(&o);
    assert(strstr(output_capture,"\"kind\":\"wifi\""));assert(strstr(output_capture,"615c") == NULL);
    assert(strstr(output_capture,"612262"));assert(strstr(output_capture,"B4:1E:52:00:00:01"));
    const uint8_t sae[][4]={{0,0x0f,0xac,8}}, psk_sae[][4]={{0,0x0f,0xac,2},{0,0x0f,0xac,8}}, psk[][4]={{0,0x0f,0xac,2}};
    const uint8_t *suite_sets[]={sae[0],psk_sae[0],psk[0]};unsigned suite_counts[]={1,2,1};
    const char *auth_labels[]={"WPA3_PSK","WPA2_WPA3_PSK","WPA2_PSK"};
    for(unsigned i=0;i<3;i++) {
        atomic_store(&active,false);fresh();atomic_store(&collecting,true);output_capture[0]=0;
        rsn_beacon((const uint8_t (*)[4])suite_sets[i],suite_counts[i]);
        assert(xQueueReceive(queue,&o,0));emit(&o);assert(strstr(output_capture,auth_labels[i]));
    }
    atomic_store(&active,false);
    fresh();atomic_store(&collecting,true);
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
    assert(uxQueueMessagesWaiting(queue)==64);assert(atomic_load(&drops)>0 && atomic_load(&loss_queue)>0);
    assert(xQueueReceive(queue,&o,0));test_clock+=2001;unsigned old=atomic_load(&drops),old_age=atomic_load(&loss_age);emit(&o);assert(atomic_load(&drops)==old+1 && atomic_load(&loss_age)==old_age+1);
    transport_limit=0;old=atomic_load(&drops);unsigned old_output=atomic_load(&loss_output);status("stats","");assert(atomic_load(&drops)==old+1 && atomic_load(&loss_output)==old_output+1);
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
    assert(test_wifi_tx_count==0); /* HS Sniff is receive-only, including setup and callback. */
    hs_wifi_cb(&hs,WIFI_PKT_DATA);
    assert(xQueueReceive(hs_pool.ready,&raw,0));
    unsigned stale_age=atomic_load(&loss_age);
    test_clock+=2001;output_capture[0]=0;
    hs_emit(raw);capture_pool_release(&hs_pool,raw);
    assert(atomic_load(&loss_age)==stale_age+1 &&
           !strstr(output_capture,"\"kind\":\"hs_packet\"") &&
           test_wifi_tx_count==0);
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
    assert(strstr(output_capture,"\"hs_capture_exclusions_v1\":true"));
    fresh();assert(!sw_wifi_only_mode());atomic_store(&stopping,true);test_task(NULL);
    /* v2 batches retain the strongest RSSI and latest capture time for an
       identical observation, and preserve the batch id on the wire. */
    char *batch_args[]={"start_wardrive_batch_serial","batch-fixture"};
    output_capture[0]=0;test_clock=1000;
    assert(start(2,batch_args)==0 && batch_mode && batch_capacity>=128);
    observation one={.kind=2,.addr_type=1,.rssi=-70,.event=0,.len=2,.data={1,2}};
    one.addr[0]=0x42;one.at=1000;batch_add(&one);
    one.rssi=-40;one.at=1200;batch_add(&one);
    unsigned occupied=0;batch_slot *saved=NULL;
    for(unsigned i=0;i<batch_capacity;i++)if(batch_slots[i].used){occupied++;saved=&batch_slots[i];}
    assert(occupied==1 && saved->value.rssi==-40 && saved->value.at==1200);
    emit_version(&saved->value,2,7);
    assert(strstr(output_capture,"\"v\":2") && strstr(output_capture,"\"batch\":7"));
    unsigned before_full=atomic_load(&drops);
    for(unsigned i=0;i<batch_capacity+20;i++) {
        observation many={.kind=2,.rssi=-50,.len=2,.data={3,4}};
        many.addr[0]=i&255;many.addr[1]=i>>8;batch_add(&many);
    }
    assert(atomic_load(&drops)>before_full); /* bounded table drops overflow */
    atomic_store(&stopping,true);test_task(NULL);
    assert(!sw_active() && !machine_mode && !heap_live && strstr(output_capture,"\"kind\":\"stopped\""));
    output_capture[0]=0;char *status_args[]={"wardrive_status","batch-fixture"};
    assert(wardrive_status(2,status_args)==0 && strstr(output_capture,"\"state\":\"stopped\""));

    fail_psram=true;output_capture[0]=0;test_clock=1000;
    assert(start(2,batch_args)==0 && batch_capacity==128);
    assert(last_heap_caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    atomic_store(&stopping,true);test_task(NULL);assert(!heap_live);fail_psram=false;

    char *wifi_batch_args[]={"start_wardrive_wifi_batch_serial","wifi-batch"};
    output_capture[0]=0;test_clock=1000;
    assert(start(2,wifi_batch_args)==0 && sw_wifi_only_mode());
    atomic_store(&collecting,true);sw_ble(a,1,-60,0,ad,sizeof(ad));
    assert(atomic_load(&ble_count)==0);
    atomic_store(&stopping,true);test_task(NULL);assert(!heap_live);

    /* A quiet batch still publishes heartbeats and explicit phase boundaries;
       lease expiry cleans up the radio, table and console mode. */
    output_capture[0]=0;test_clock=1000;
    assert(start(2,batch_args)==0);test_task(NULL);
    assert(strstr(output_capture,"\"kind\":\"started\"") && strstr(output_capture,"\"kind\":\"heartbeat\""));
    assert(strstr(output_capture,"\"kind\":\"batch_start\"") && strstr(output_capture,"\"kind\":\"batch_results\""));
    assert(strstr(output_capture,"\"kind\":\"batch_done\"") && strstr(output_capture,"\"state\":\"stopped\""));
    assert(!machine_mode && !heap_live);
    capabilities(0,NULL);assert(strstr(output_capture,"\"wardrive_batch_serial_v2\":true"));
    assert(strstr(output_capture,"\"wardrive_live_oui_v1\":true") && strstr(output_capture,"\"live_oui_max\":8"));

    /* Validate the optional bounded filter before preparing or touching radio state. */
    const char *invalid[]={"live_oui=", "live_oui=B41E5", "live_oui=B41E5Z", "live_oui=B41E52,",
        "live_oui=B41E52,B41e52", "wrong=B41E52", "live_oui=B41E5200",
        "live_oui=000001,000002,000003,000004,000005,000006,000007,000008,000009"};
    int prepares_before=prepare_calls;
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) {
        char *args[]={"start_wardrive_batch_serial","live",(char *)invalid[i]};
        assert(start(3,args)==1 && !sw_active() && !heap_live);
    }
    char *wrong_mode[]={"start_wardrive_serial","live","live_oui=B41E52"};
    assert(start(3,wrong_mode)==1 && prepare_calls==prepares_before);
    char *live_args[]={"start_wardrive_batch_serial","live","live_oui=b41e52,0025DF"};
    test_clock=1000;output_capture[0]=0;assert(start(3,live_args)==0 && live_oui_count==2);
    atomic_store(&collecting,true);
    unsigned allocations=heap_live;
    beacon(8,45);assert(!output_capture[0] && heap_live==allocations); /* callback only queues */
    assert(xQueueReceive(queue,&o,0));batch_observe(&o,1);
    assert(strstr(output_capture,"\"kind\":\"live_oui\"") && !strstr(output_capture,"ssid_hex"));
    assert(!strstr(output_capture,"\"kind\":\"wifi\""));
    assert(live_identity_count==1);
    output_capture[0]=0;batch_observe(&o,1);assert(!output_capture[0] && live_identity_count==1);
    observation candidate={.kind=2,.addr_type=0,.rssi=-62,.at=1000};
    uint8_t axon[]={0,0x25,0xdf,0,0,1};memcpy(candidate.addr,axon,6);
    candidate.addr_type=1;batch_observe(&candidate,1);assert(!output_capture[0]);
    candidate.addr_type=2;batch_observe(&candidate,1);assert(!output_capture[0]);
    candidate.addr_type=0;batch_observe(&candidate,1);assert(strstr(output_capture,"00:25:DF:00:00:01"));
    output_capture[0]=0;
    candidate.kind=1;candidate.addr[0]=2;batch_observe(&candidate,1);assert(!output_capture[0]);
    /* Receiver-only OUI never alerts. A distinct eligible beacon BSSID does. */
    candidate.event=4;memcpy(candidate.rx,axon,6);batch_observe(&candidate,1);assert(!output_capture[0]);
    candidate.event=8;memcpy(candidate.bssid,axon,6);batch_observe(&candidate,1);
    assert(strstr(output_capture,"\"radio\":\"wifi\"") && strstr(output_capture,"00:25:DF:00:00:01"));
    output_capture[0]=0;candidate.kind=2;candidate.event=0;candidate.addr_type=0;memcpy(candidate.addr,axon,6);
    candidate.addr[5]=2;candidate.at=-1001;batch_observe(&candidate,1);assert(!output_capture[0]);
    /* A failed hint is still saved and can be emitted with all evidence later. */
    candidate.at=test_clock;transport_limit=0;old=atomic_load(&drops);
    batch_observe(&candidate,1);assert(atomic_load(&drops)==old+1);
    bool retained=false;
    for(unsigned i=0;i<batch_capacity;i++) if(batch_slots[i].used && observation_equal(&batch_slots[i].value,&candidate)) retained=true;
    assert(retained);transport_limit=1024;emit_version(&candidate,2,1);
    assert(strstr(output_capture,"\"kind\":\"ble\""));
    output_capture[0]=0;
    for(unsigned i=3;i<80;i++) {candidate.addr[5]=i;batch_observe(&candidate,1);}
    assert(live_identity_count==LIVE_OUI_IDENTITIES && heap_live==allocations);
    output_capture[0]=0;candidate.addr[5]=90;batch_observe(&candidate,1);assert(!output_capture[0]);
    atomic_store(&stopping,true);test_task(NULL);
    assert(!heap_live && !live_oui_count && !live_identity_count);
    assert(start(2,batch_args)==0 && !live_oui_count);atomic_store(&stopping,true);test_task(NULL);
    /* End-to-end worker ordering and fake-clock latency: a hint is emitted
       within 10ms while collection continues; complete evidence waits 10s. */
    test_clock=1000;output_capture[0]=0;inject_live=true;
    assert(start(3,live_args)==0);test_task(NULL);
    const char *hint=strstr(output_capture,"\"kind\":\"live_oui\"");
    const char *results=strstr(output_capture,"\"kind\":\"batch_results\"");
    const char *full=strstr(output_capture,"\"kind\":\"ble\"");
    assert(hint && results && full && hint<results && results<full);
    assert(strstr(hint,"\"age_ms\":10") && !heap_live && !machine_mode);
    atomic_store(&seq,UINT32_MAX-1u);atomic_store(&sequence_exhausted,false);atomic_store(&stopping,false);
    assert(next_seq()==UINT32_MAX && atomic_load(&sequence_exhausted) && atomic_load(&stopping));
    assert(next_seq()==UINT32_MAX); /* saturates; never wraps back to sequence zero */
    puts("PASS: WiFi/BLE + passive EAPOL framing, malformed/protected data, queue overflow, age/backpressure, lease, cleanup, v2 batching and mode transitions");
}
