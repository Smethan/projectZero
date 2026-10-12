/* One-request process harness around the production parser and real cJSON. */
#define main serial_wardrive_jobs_test_main
#include "test_jobs.c"
#undef main

static void emit_captured_records(void) {
    size_t length=strlen(output_capture);
    const char *records=output_capture;
    if(length && records[0]=='\n') { records++; length--; }
    if(length) fwrite(records,1,length,stdout);
    output_capture[0]=0;
    fflush(stdout);
}

static void register_interop_operations(void) {
    const char *ops[]={"handshake","portal","deauth","sae","blackout","evil_twin",
                       "sniffer","bt_track","airtag_scan"};
    for(size_t i=0;i<sizeof(ops)/sizeof(ops[0]);i++)
        if(!sw_register_job_operation_ex(ops[i],custom_start,custom_stop)) abort();
}

int main(void) {
    test_free_heap=1024*1024;
    if(!ensure_identity()) return 10;
    /* Match the firmware hex() emitter; host uppercase encoding is tested separately. */
    snprintf(boot_id,sizeof(boot_id),"a1b2c3d4e5f60789");
    sw_register();
    register_interop_operations();
    sw_register_radio_busy_probe(busy_probe);

    if(device_info(1,NULL)!=0) return 11;
    emit_captured_records();

    char line[6000];
    if(!fgets(line,sizeof(line),stdin)) return 12;
    line[strcspn(line,"\r\n")]=0;
    char *save=NULL;
    char *command=strtok_r(line," \t",&save);
    char *wire_token=strtok_r(NULL," \t",&save);
    char *payload=strtok_r(NULL," \t",&save);
    char *extra=strtok_r(NULL," \t",&save);
    if(!command || strcmp(command,"job_start") || !wire_token || !payload || extra)return 13;

    char *argv[]={"job_start",wire_token,payload};
    int rc=job_start(3,argv);
    emit_captured_records();

    if(rc==0) {
        /* The native task stub records workers instead of running them. Drive
           the real worker synchronously so built-in jobs emit started and
           drain through their normal lease-expiry cleanup without RF. */
        if(!external_job_active && atomic_load(&active) && test_task) test_task(test_task_arg);
        char *stop_argv[]={"job_stop",wire_token};
        if(job_stop(2,stop_argv)!=0) return 14;
        if(external_supervisor_task && test_task) test_task(test_task_arg);
        emit_captured_records();
        if(sw_active() || heap_live) return 15;
        return 0;
    }
    if(sw_active() || heap_live) return 16;
    return 2;
}
