/* Exercise production encoded job parsing with upstream cJSON, both profiles. */
#include "../../ESP32C5/main/serial_wardrive.c"
static bool update_busy_fixture, legacy_busy_fixture;
bool uota_busy(void) { return update_busy_fixture; }
static bool busy_probe(void) { return legacy_busy_fixture; }
static unsigned prepare_calls, radio_stops, custom_starts, custom_stops;
static bool custom_fail;
static int custom_start_result=SW_JOB_START_READY;
static unsigned custom_stop_failures;
static bool require_start_barrier;
static void *custom_buffer;
static size_t json_record_length(void) {
    const char *record=strstr(output_capture,"WDG:");
    assert(record);
    record+=4;
    return strcspn(record,"\r\n");
}
bool sw_prepare(void) { prepare_calls++; return true; }
bool sw_radio_start(void) { return true; }
bool sw_radio_stop(void) { radio_stops++; return true; }
bool sw_radio_hop(unsigned index) { (void)index; return true; }
static int custom_start(const cJSON *request,const char *token) {
    assert(request && token && sw_job_owner_matches(token));
    custom_starts++;
    custom_buffer=heap_caps_malloc(512,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(!custom_buffer)return 1;
    if(custom_fail || custom_start_result==SW_JOB_START_FAILED) {
        heap_caps_free(custom_buffer);custom_buffer=NULL;return SW_JOB_START_FAILED;
    }
    return custom_start_result;
}
static int custom_stop(const char *token) {
    assert(sw_job_owner_matches(token));custom_stops++;
    assert(atomic_load(&external_stop_claimed));
    if(require_start_barrier)assert(!atomic_load(&external_job_ready));
    if(custom_stop_failures) {custom_stop_failures--;return 1;}
    heap_caps_free(custom_buffer);custom_buffer=NULL;return 0;
}
static void encode(const char *json,char out[5500]) {
    static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    unsigned bits=0, value=0;size_t pos=0;
    for(const unsigned char *p=(const unsigned char *)json;*p;p++) {
        value=(value<<8)|*p;bits+=8;
        while(bits>=6){bits-=6;out[pos++]=alphabet[(value>>bits)&63];}
    }
    if(bits)out[pos++]=alphabet[(value<<(6-bits))&63];out[pos]=0;
}
static int submit_boot(const char *boot,const char *token,const char *operation,const char *band,const char *ble,
                       const char *channels,const char *targets,const char *exclude,const char *options) {
    char json[4097], payload[5500];
    snprintf(json,sizeof(json),"{\"boot_id\":\"%s\",\"op\":\"%s\",\"band\":\"%s\",\"channels\":%s,\"ble\":\"%s\",\"dwell_ms\":160,\"targets\":%s,\"exclude\":%s,\"options\":%s,\"live_ouis\":[]}",
        boot,operation,band,channels,ble,targets,exclude,options);
    encode(json,payload);char *argv[]={"job_start",(char *)token,payload};
    return job_start(3,argv);
}
static int submit(const char *token,const char *operation,const char *band,const char *ble,
                   const char *channels,const char *targets,const char *exclude,const char *options) {
    return submit_boot(boot_id,token,operation,band,ble,channels,targets,exclude,options);
}
static int submit_implicit_channels(const char *token,const char *operation,
                                    const char *band,const char *options) {
    char json[4097], payload[5500];
    snprintf(json,sizeof(json),"{\"boot_id\":\"%s\",\"op\":\"%s\",\"band\":\"%s\",\"ble\":\"off\",\"dwell_ms\":160,\"targets\":[],\"exclude\":[],\"options\":%s,\"live_ouis\":[]}",
        boot_id,operation,band,options);
    encode(json,payload);char *argv[]={"job_start",(char *)token,payload};
    return job_start(3,argv);
}
#define ALL "{\"storage\":\"serial\",\"scope\":\"all\"}"
#define SELECTED "{\"storage\":\"serial\",\"scope\":\"selected\"}"
#define TARGET "[{\"bssid\":\"02:11:22:33:44:55\",\"channel\":6,\"auth\":3,\"ssid_hex\":\"4142\"}]"
int main(void) {
    test_free_heap=1024*1024;fail_psram=true;
    assert(ensure_identity());sw_register();
    snprintf(boot_id,sizeof(boot_id),"a1b2c3d4e5f60789");
    assert(sw_register_job_operation_ex("handshake",custom_start,custom_stop));
    assert(sw_register_job_operation_ex("portal",custom_start,custom_stop));
    const char *extra_ops[]={"deauth","sae","blackout","evil_twin","sniffer","bt_track","airtag_scan"};
    for(unsigned i=0;i<sizeof(extra_ops)/sizeof(extra_ops[0]);i++)
        assert(sw_register_job_operation_ex(extra_ops[i],custom_start,custom_stop));
    sw_set_boot_state(false,"initializing",false);
    output_capture[0]=0;
    assert(device_info(1,NULL)==0 && strstr(output_capture,"\"boot_state\":\"degraded\"") &&
           strstr(output_capture,"\"boot_reason\":\"initializing\"") &&
           strstr(output_capture,"\"operations\":[]") && strstr(output_capture,"\"channels\":[]") &&
           strstr(output_capture,"\"limits\":{\"targets\":0,\"exclusions\":0,\"batch\":0,\"aps\":0,\"clients\":0,\"probes\":0}"));
    output_capture[0]=0;
    assert(capabilities(1,NULL)==0 && strstr(output_capture,"\"c5_jobs_v1\":false") &&
           !strstr(output_capture,"wardrive_serial_v1"));
    unsigned initializing_prepares=prepare_calls;
    char *initializing_job[]={"job_start","boot-gate","invalid"};
    output_capture[0]=0;
    assert(dispatch(3,initializing_job)==1 && prepare_calls==initializing_prepares && !sw_job_owned());
    assert(dispatch(0,NULL)==1); /* argc is checked before argv[0]. */
    sw_set_boot_state(true,"normal",true);
    /* Exercise the production checked channel helper with SDK readback faults.
     * C5 may report an automatic secondary on 5 GHz, while 2.4 GHz must stay
     * HT20 and every managed hop must remain inside its declared plan. */
    const uint8_t radio_plan[]={6,36};
    assert(sw_job_set_radio_plan(radio_plan,sizeof(radio_plan),160));
    job_mode=true;
    test_channel_result=ESP_OK;
    test_channel_readback_result=ESP_OK;
    test_channel_actual=36;
    test_channel_secondary=WIFI_SECOND_CHAN_ABOVE;
    sw_channel_result_t channel_check=sw_job_apply_channel_checked(36);
    assert(channel_check.status==SW_CHANNEL_OK && channel_check.requested_channel==36 &&
           channel_check.actual_channel==36);

    test_channel_stale_readback=true;
    test_channel_actual=40; /* SDK readback reports a different primary. */
    channel_check=sw_job_apply_channel_checked(36);
    assert(channel_check.status==SW_CHANNEL_READBACK_ERROR && channel_check.sdk_error==ESP_FAIL);

    test_channel_stale_readback=false;
    test_channel_actual=36;
    test_channel_readback_result=21; /* SDK could not report the applied channel. */
    channel_check=sw_job_apply_channel_checked(36);
    assert(channel_check.status==SW_CHANNEL_READBACK_ERROR && channel_check.sdk_error==21);

    test_channel_readback_result=ESP_OK;
    test_channel_result=22; /* Channel set itself failed in the SDK. */
    channel_check=sw_job_apply_channel_checked(36);
    assert(channel_check.status==SW_CHANNEL_DRIVER_ERROR && channel_check.sdk_error==22);
    test_channel_result=ESP_OK;
    test_channel_actual=6;
    test_channel_secondary=WIFI_SECOND_CHAN_NONE;
    channel_check=sw_job_apply_channel_checked(6);
    assert(channel_check.status==SW_CHANNEL_OK);
    test_channel_secondary=WIFI_SECOND_CHAN_ABOVE;
    channel_check=sw_job_apply_channel_checked(6);
    assert(channel_check.status==SW_CHANNEL_READBACK_ERROR && channel_check.sdk_error==ESP_FAIL);

    /* Also exercise the shared managed-hop entry point used by job workers. */
    test_channel_actual=36;
    test_channel_secondary=WIFI_SECOND_CHAN_ABOVE;
    sw_channel_result_t hop_check=sw_job_radio_hop_checked(1);
    assert(hop_check.status==SW_CHANNEL_OK && hop_check.requested_channel==36 &&
           hop_check.actual_channel==36);

    test_channel_stale_readback=true;
    test_channel_actual=40;
    hop_check=sw_job_radio_hop_checked(1);
    assert(hop_check.status==SW_CHANNEL_READBACK_ERROR && hop_check.sdk_error==ESP_FAIL);
    test_channel_stale_readback=false;

    test_channel_secondary=WIFI_SECOND_CHAN_ABOVE;
    hop_check=sw_job_radio_hop_checked(0);
    assert(hop_check.status==SW_CHANNEL_READBACK_ERROR && hop_check.sdk_error==ESP_FAIL);

    test_channel_secondary=WIFI_SECOND_CHAN_NONE;
    channel_check=sw_job_apply_channel_checked(40); /* Valid RF channel, absent from this job. */
    assert(channel_check.status==SW_CHANNEL_NOT_IN_PLAN && channel_check.sdk_error==ESP_ERR_INVALID_ARG);
    job_mode=false;
    job_channel_count=0;
    test_channel_actual=0;
    test_channel_secondary=WIFI_SECOND_CHAN_NONE;
    output_capture[0]=0;
    assert(device_info(1,NULL)==0 && strstr(output_capture,"\"boot_state\":\"ready\"") &&
           strstr(output_capture,"\"boot_reason\":\"normal\""));
#if PROJECTZERO_XIAO_US_CH144
    assert(strstr(output_capture,"140,144,149"));
#else
    assert(strstr(output_capture,"140,149") && !strstr(output_capture,"140,144,149"));
#endif
    output_capture[0]=0;
    assert(capabilities(1,NULL)==0 && strstr(output_capture,"\"c5_jobs_v1\":true"));
    sw_register_radio_busy_probe(busy_probe);
    legacy_busy_fixture=true;output_capture[0]=0;
    assert(device_info(1,NULL)==0 && strstr(output_capture,"\"legacy_busy\":true"));
    assert(json_record_length()<=1000);
    assert(submit("legacy-busy","wardrive","wifi24","off","[6]","[]","[]",ALL)==1);
    assert(!prepare_calls && !heap_live && strstr(output_capture,"external_busy"));
    legacy_busy_fixture=false;update_busy_fixture=true;output_capture[0]=0;
    assert(submit("update-busy","wardrive","wifi24","off","[6]","[]","[]",ALL)==1);
    assert(!prepare_calls && !heap_live && strstr(output_capture,"update_busy"));
    update_busy_fixture=false;output_capture[0]=0;
    assert(device_info(1,NULL)==0 && strstr(output_capture,"\"legacy_busy\":false"));
    assert(json_record_length()<=1000);
    const char *info_fixture=getenv("C5_INFO_FIXTURE");
    if(info_fixture) {FILE *f=fopen(info_fixture,"w");assert(f);fputs(output_capture,f);fclose(f);}
    output_capture[0]=0;assert(capabilities(1,NULL)==0);
    assert(strstr(output_capture,"\"c5_jobs_v1\":true"));
    assert(job_boot_id_matches("A1B2C3D4E5F60789"));
    assert(job_boot_id_matches("A1b2C3d4E5f60789"));
    assert(!job_boot_id_matches("a1b2c3d4e5f60788"));
    assert(!job_boot_id_matches("a1b2c3d4e5f6078"));
    assert(!job_boot_id_matches("a1b2c3d4e5f6078g"));
    unsigned starts_before_nonce=custom_starts;
    unsigned prepares_before_nonce=prepare_calls;
    output_capture[0]=0;
    assert(submit_boot("A1B2C3D4E5F60789","nonce-upper","handshake","wifi24","off","[6]",TARGET,"[]",SELECTED)==0);
    assert(custom_starts==starts_before_nonce+1 && external_job_active);
    char *nonce_stop[]={"job_stop","nonce-upper"};
    test_auto_schedule=true;
    assert(job_stop(2,nonce_stop)==0);
    assert(test_task && test_task_arg==NULL && !external_supervisor_task);
    assert(strstr(output_capture,"\"kind\":\"stopped\""));
    /* The next job starts immediately after stop ACK without manually running
       the old supervisor task from the test. */
    assert(submit("nonce-replacement","handshake","wifi24","off","[6]",TARGET,"[]",SELECTED)==0);
    assert(external_job_active && external_supervisor_task);
    char *nonce_replacement_stop[]={"job_stop","nonce-replacement"};
    assert(job_stop(2,nonce_replacement_stop)==0 && !external_supervisor_task);
    test_auto_schedule=false;
    assert(!sw_active() && !heap_live);
    const char *bad_boots[]={"a1b2c3d4e5f60788","a1b2c3d4e5f6078","a1b2c3d4e5f6078g"};
    for(unsigned i=0;i<sizeof(bad_boots)/sizeof(bad_boots[0]);i++) {
        output_capture[0]=0;
        assert(submit_boot(bad_boots[i],"nonce-invalid","handshake","wifi24","off","[6]",TARGET,"[]",SELECTED)==1);
        assert(strstr(output_capture,"invalid_request") && !sw_active() && !heap_live);
    }
    assert(custom_starts==starts_before_nonce+2 && prepare_calls==prepares_before_nonce);
    assert(submit("rx","wardrive","wifi24","off","[6]","[]","[]",ALL)==0);
    assert(sw_active() && sw_wifi_only_mode() && prepare_calls==1);
    char *rx_status[]={"job_status","rx"};output_capture[0]=0;
    assert(job_status(2,rx_status)==0 && strstr(output_capture,"\"state\":\"starting\""));
    assert(strstr(output_capture,"\"losses\":{\"queue\":0,\"age\":0,\"output\":0,\"saturation\":0}"));
    const char *lifecycle_fixture=getenv("C5_LIFECYCLE_FIXTURE");
    if(lifecycle_fixture) {FILE *f=fopen(lifecycle_fixture,"w");assert(f);fputs(output_capture,f);fclose(f);}
    atomic_store(&radio_ready,true);output_capture[0]=0;
    assert(job_status(2,rx_status)==0 && strstr(output_capture,"\"state\":\"running\""));
    atomic_store(&radio_ready,false);
    unsigned old_prepares=prepare_calls;
    assert(submit("rx","wardrive","wifi24","off","[6]","[]","[]",ALL)==0);
    assert(prepare_calls==old_prepares); /* identical retry never starts twice */
    assert(submit("busy","wardrive","wifi5","off","[36]","[]","[]",ALL)==1);
    assert(sw_active() && sw_job_owner_matches("rx"));
    assert(submit("rx","wardrive","wifi24","off","[11]","[]","[]",ALL)==1);
    assert(sw_active() && sw_job_owner_matches("rx"));
    output_capture[0]=0;test_task(NULL); /* lease expiry in production worker */
    assert(!sw_active() && !sw_job_owned() && !heap_live && radio_stops==1);
    assert(strstr(output_capture,"\"kind\":\"started\"") && strstr(output_capture,"\"kind\":\"stopped\""));
    output_capture[0]=0;
    assert(submit("hs","hs_sniff","wifi24","off","[6]","[]","[]",ALL)==0);
    test_task(NULL);assert(!sw_active() && !heap_live);
    assert(submit("ble","ble_scan","none","scan","[]","[]","[]",ALL)==0);
    test_task(NULL);assert(!sw_active() && !heap_live);
    unsigned before_custom=custom_starts;
    assert(submit("selected","handshake","wifi24","off","[6]",TARGET,
        "[\"02:66:77:88:99:AA\"]",SELECTED)==0);
    assert(custom_starts==before_custom+1 && external_job_active);
    output_capture[0]=0;char *status_args[]={"job_status","selected"};assert(job_status(2,status_args)==0);
    assert(strstr(output_capture,"\"session\":\"selected\"") && strstr(output_capture,"\"state\":\"running\""));
    output_capture[0]=0;test_task(test_task_arg);
    assert(!sw_active() && !heap_live && custom_stops==3);
    assert(strstr(output_capture,"\"kind\":\"heartbeat\"") && strstr(output_capture,"lease_expired") && strstr(output_capture,"\"kind\":\"stopped\""));
    assert(submit("protected","handshake","wifi24","off","[6]",TARGET,
        "[\"02:11:22:33:44:55\"]",SELECTED)==1);
    assert(custom_starts==before_custom+1);
    assert(submit("outside","handshake","wifi5","off","[36]",TARGET,"[]",SELECTED)==1);
    custom_start_result=SW_JOB_START_PENDING;
    output_capture[0]=0;
    assert(submit("pending","handshake","wifi24","off","[6]",TARGET,"[]",SELECTED)==0);
    assert(external_job_active && external_supervisor_task && !atomic_load(&external_operation_ready));
    assert(strstr(output_capture,"\"kind\":\"status\"") && strstr(output_capture,"\"state\":\"starting\"") &&
           !strstr(output_capture,"\"kind\":\"started\""));
    char *pending_status[]={"job_status","pending"};output_capture[0]=0;
    assert(job_status(2,pending_status)==0 && strstr(output_capture,"\"state\":\"starting\""));
    assert(sw_job_signal_ready("pending") && !sw_job_signal_ready("wrong-token"));
    test_task(test_task_arg);
    assert(!external_job_active && !heap_live && strstr(output_capture,"\"kind\":\"started\"") &&
           strstr(output_capture,"\"kind\":\"stopped\""));
    custom_start_result=SW_JOB_START_READY;
    assert(submit("portal","portal","wifi24","off","[6]","[]","[]",
        "{\"storage\":\"serial\",\"scope\":\"selected\",\"ssid_hex\":\"4142\"}")==0);
    test_task(test_task_arg);assert(!sw_active()&&!heap_live);
    output_capture[0]=0;assert(job_status(2,status_args)==0);
    assert(strstr(output_capture,"\"session\":\"selected\"") && strstr(output_capture,"\"state\":\"stopped\""));
    custom_fail=true;assert(submit("failed","handshake","wifi24","off","[6]",TARGET,"[]",SELECTED)==1);
    assert(!sw_active()&&!heap_live && external_supervisor_task);custom_fail=false;
    test_auto_schedule=true;
    assert(submit("after-failed-start","handshake","wifi24","off","[6]",TARGET,"[]",SELECTED)==0);
    assert(external_job_active && external_supervisor_task);
    char *after_failed_stop[]={"job_stop","after-failed-start"};
    assert(job_stop(2,after_failed_stop)==0 && !external_supervisor_task);
    test_auto_schedule=false;
    task_ok=0;before_custom=custom_starts;
    assert(submit("no-supervisor","handshake","wifi24","off","[6]",TARGET,"[]",SELECTED)==1);
    assert(custom_starts==before_custom && !external_supervisor_task && !sw_active() && !heap_live);
    task_ok=1;
    assert(submit("stop-retry","handshake","wifi24","off","[6]",TARGET,"[]",SELECTED)==0);
    char *retry_stop[]={"job_stop","stop-retry"},*retry_status[]={"job_status","stop-retry"};
    custom_stop_failures=1;output_capture[0]=0;
    assert(job_stop(2,retry_stop)==1 && sw_active() && custom_buffer);
    assert(job_status(2,retry_status)==0 && strstr(output_capture,"\"state\":\"stopping\""));
    assert(job_keepalive(2,retry_status)==0); /* Lease renewal cannot cancel cleanup. */
    test_task(test_task_arg);assert(!sw_active()&&!heap_live);
    assert(strstr(output_capture,"host_stop") && strstr(output_capture,"\"kind\":\"stopped\""));
    output_capture[0]=0;assert(job_status(2,retry_status)==0);
    assert(strstr(output_capture,"\"v\":3") && strstr(output_capture,"\"device_id\"") && strstr(output_capture,"\"state\":\"stopped\""));
    const char *terminal_fixture=getenv("C5_TERMINAL_FIXTURE");
    if(terminal_fixture) {FILE *f=fopen(terminal_fixture,"w");assert(f);fputs(output_capture,f);fclose(f);}
    assert(submit("retire-timeout","handshake","wifi24","off","[6]",TARGET,"[]",SELECTED)==0);
    char *retire_timeout_stop[]={"job_stop","retire-timeout"};
    output_capture[0]=0;
    assert(job_stop(2,retire_timeout_stop)==1);
    assert(!external_job_active && external_stop_completed && external_supervisor_task && !heap_live);
    assert(strstr(output_capture,"supervisor_retire_pending") && !strstr(output_capture,"\"kind\":\"stopped\""));
    test_task(test_task_arg); /* Delayed supervisor eventually owns terminal completion. */
    assert(!external_supervisor_task && !external_stop_completed);
    assert(strstr(output_capture,"\"kind\":\"stopped\""));
    assert(submit("after-retire-timeout","handshake","wifi24","off","[6]",TARGET,"[]",SELECTED)==0);
    test_auto_schedule=true;
    char *after_timeout_stop[]={"job_stop","after-retire-timeout"};
    assert(job_stop(2,after_timeout_stop)==0 && !external_supervisor_task);
    test_auto_schedule=false;
#if CONFIG_PROJECTZERO_BOARD_WAVESHARE_ZERO
    test_free_heap=1024;require_start_barrier=true;
    assert(submit("custom-low-floor","handshake","wifi24","off","[6]",TARGET,"[]",SELECTED)==1);
    assert(!sw_active()&&!heap_live&&external_supervisor_task);
    test_auto_schedule=true;
    assert(submit("custom-low-floor-replacement","handshake","wifi24","off","[6]",TARGET,"[]",SELECTED)==1);
    assert(!sw_active()&&!heap_live&&external_supervisor_task);
    test_auto_schedule=false;test_task(test_task_arg);
    assert(!external_supervisor_task);require_start_barrier=false;
    test_free_heap=1024;
    assert(submit("low-heap","wardrive","wifi24","off","[6]","[]","[]",ALL)==1);
    assert(!sw_active()&&!heap_live);test_free_heap=1024*1024;
    assert(submit("post-start-low-heap","wardrive","wifi24","off","[6]","[]","[]",ALL)==0);
    test_free_heap=1024;output_capture[0]=0;test_task(NULL);
    assert(!sw_active()&&!heap_live && strstr(output_capture,"memory_floor_failed"));
    assert(!strstr(output_capture,"\"kind\":\"started\""));test_free_heap=1024*1024;
#endif
    test_country_available=true;test_country=(wifi_country_t){.schan=1,.nchan=11};
    assert(submit("country-rejected","wardrive","wifi24","off","[13]","[]","[]",ALL)==1);
    assert(!sw_active()&&!heap_live);
    test_country.policy=WIFI_COUNTRY_POLICY_MANUAL;test_country.wifi_5g_channel_mask=CHANNEL_TO_BIT(36);
    assert(valid_channel(36) && !valid_channel(40));
    assert(submit("mask-rejected","wardrive","wifi5","off","[40]","[]","[]",ALL)==1);
    assert(!sw_active()&&!heap_live);
    test_country.wifi_5g_channel_mask=CHANNEL_TO_BIT(144);
    assert(valid_channel(144)==(bool)PROJECTZERO_XIAO_US_CH144);
    test_country.policy=WIFI_COUNTRY_POLICY_AUTO;assert(valid_channel(40));
    test_country_available=false;
    assert(!valid_channel(12) && !valid_channel(13) && !valid_channel(14));
    assert(valid_channel(144)==(bool)PROJECTZERO_XIAO_US_CH144 &&
           valid_channel(140) && valid_channel(149));
    bool legacy_140=false, legacy_144=false, legacy_149=false;
    for (unsigned hop_index=0;hop_index<64;hop_index++) {
        sw_channel_result_t hop=sw_job_radio_hop_checked(hop_index);
        assert(hop.status==SW_CHANNEL_OK &&
               hop.requested_channel!=12 && hop.requested_channel!=13 &&
               hop.requested_channel!=14);
        legacy_140|=hop.requested_channel==140;
        legacy_144|=hop.requested_channel==144;
        legacy_149|=hop.requested_channel==149;
    }
    assert(legacy_140 && legacy_149 && legacy_144==(bool)PROJECTZERO_XIAO_US_CH144);
    test_channel_result=42;
    assert(sw_job_radio_hop_checked(0).status==SW_CHANNEL_DRIVER_ERROR);
    test_channel_result=ESP_OK;
    output_capture[0]=0;
#if PROJECTZERO_XIAO_US_CH144
    assert(submit("supported-144","wardrive","wifi5","off","[144]","[]","[]",ALL)==0);
    assert(sw_active() && sw_job_owner_matches("supported-144"));
    test_task(NULL);
    assert(!sw_active() && !heap_live && strstr(output_capture,"\"kind\":\"started\""));
    output_capture[0]=0;
    assert(submit_implicit_channels("default-144","handshake","both",ALL)==0);
    bool default_has_144=false;
    for (unsigned i=0;i<job_channel_count;i++) default_has_144|=job_channels[i]==144;
    assert(default_has_144 && sw_active() && sw_job_owner_matches("default-144"));
    test_task(NULL);
    assert(!sw_active() && !heap_live);
    assert(submit_implicit_channels("default-144-5g","handshake","wifi5",ALL)==0);
    default_has_144=false;
    for (unsigned i=0;i<job_channel_count;i++) default_has_144|=job_channels[i]==144;
    assert(default_has_144);
    test_task(NULL);
    assert(!sw_active() && !heap_live);
    assert(submit_implicit_channels("default-24","handshake","wifi24",ALL)==0);
    default_has_144=false;
    for (unsigned i=0;i<job_channel_count;i++) default_has_144|=job_channels[i]==144;
    assert(!default_has_144);
    test_task(NULL);
    assert(!sw_active() && !heap_live);
#else
    unsigned prepares_before_unsupported_channel=prepare_calls;
    assert(submit("unsupported-144","wardrive","wifi5","off","[144]","[]","[]",ALL)==1);
    assert(!sw_active()&&!heap_live&&strstr(output_capture,"invalid_request"));
    assert(prepare_calls==prepares_before_unsupported_channel); /* rejected before radio setup */
#endif
    test_channel_result=1;output_capture[0]=0;
    assert(submit("bad-channel","wardrive","wifi24","off","[6]","[]","[]",ALL)==0);
    test_task(NULL);assert(!sw_active()&&!heap_live);
    assert(strstr(output_capture,"channel_apply_failed") && !strstr(output_capture,"\"kind\":\"started\""));test_channel_result=0;
    atomic_store(&seq,UINT32_MAX-2u);output_capture[0]=0;
    assert(submit("sequence-limit","handshake","wifi24","off","[6]",TARGET,"[]",SELECTED)==0);
    sw_job_emit_lifecycle("heartbeat","running","");
    assert(atomic_load(&sequence_exhausted) && atomic_load(&seq)==UINT32_MAX);
    test_task(test_task_arg);assert(!sw_active()&&!heap_live);
    assert(submit("after-exhaustion","wardrive","wifi24","off","[6]","[]","[]",ALL)==1);
    assert(atomic_load(&seq)==UINT32_MAX && !sw_active() && !heap_live);
    puts("PASS real JSON job starts, idempotence, independent exclusions, lease expiry, status recovery and cleanup");
    return 0;
}
