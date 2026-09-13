/* App-mode, acknowledged USB OTA. Only the inactive app slot is writable.
 * Checkpoints advance after durable 4 KiB writes; reboot resumes by erasing
 * from that checkpoint, never by trusting a partially written flash sector. */
#include "usb_ota.h"
#include "serial_output.h"
#include "serial_wardrive.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "linenoise/linenoise.h"
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#define SECTOR 4096u
#define CHUNK 256u
#define META_MAGIC 0x554f5431u
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#define BOARD "xiao"
#else
#define BOARD "wroom"
#endif

typedef struct {
    uint32_t magic, address, size, checkpoint;
    char hash[65];
} metadata_t;
static metadata_t meta;
static const esp_partition_t *target;
static esp_ota_handle_t handle;
static uint32_t offset;
static bool active;
static bool (*prepare_cb)(void);
static void (*restart_cb)(void);

bool uota_busy(void) { return active; }
static bool number(const char *s,uint32_t *out) {
    if(!s || !*s) return false;
    for(const char *p=s;*p;p++) if(*p<'0'||*p>'9')return false;
    char *end;unsigned long long n=strtoull(s,&end,10);
    if(*end || n>UINT32_MAX)return false;
    *out=(uint32_t)n;return true;
}
static int nibble(char c) {
    return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;
}
static bool hash_ok(const char *s) {
    if(strlen(s)!=64)return false;
    for(int i=0;i<64;i++)if(nibble(s[i])<0)return false;
    return true;
}
static uint32_t crc32(const uint8_t *p,unsigned n) {
    uint32_t crc=~0u;
    while(n--) { crc^=*p++;for(int i=0;i<8;i++)crc=(crc>>1)^((crc&1)?0xedb88320u:0); }
    return ~crc;
}
static void response(const char *kind,const char *error) {
    char line[400];
    int n=snprintf(line,sizeof(line),"\nUOTA:{\"v\":1,\"kind\":\"%s\",\"board\":\"%s\",\"sha256\":\"%s\",\"size\":%"PRIu32",\"offset\":%"PRIu32",\"active\":%s,\"slot\":\"%s\",\"error\":\"%s\"}\n",
        kind,BOARD,meta.magic==META_MAGIC?meta.hash:"",meta.size,offset,
        active?"true":"false",target?target->label:"",error?error:"");
    if(n>0&&n<sizeof(line))serial_output(line,n,1000);
}
static bool persist(void) {
    nvs_handle_t nvs;
    if(nvs_open("usb_ota",NVS_READWRITE,&nvs)!=ESP_OK)return false;
    esp_err_t err=nvs_set_blob(nvs,"pending",&meta,sizeof(meta));
    if(err==ESP_OK)err=nvs_commit(nvs);
    nvs_close(nvs);return err==ESP_OK;
}
static void clear(void) {
    nvs_handle_t nvs;
    if(nvs_open("usb_ota",NVS_READWRITE,&nvs)==ESP_OK) {
        nvs_erase_key(nvs,"pending");nvs_commit(nvs);nvs_close(nvs);
    }
    memset(&meta,0,sizeof(meta));target=NULL;offset=0;
}
static void load(void) {
    if(active)return;
    memset(&meta,0,sizeof(meta));offset=0;target=NULL;
    nvs_handle_t nvs;size_t size=sizeof(meta);
    if(nvs_open("usb_ota",NVS_READONLY,&nvs)!=ESP_OK)return;
    esp_err_t err=nvs_get_blob(nvs,"pending",&meta,&size);nvs_close(nvs);
    if(err!=ESP_OK||size!=sizeof(meta)||meta.magic!=META_MAGIC||meta.hash[64]!=0||!hash_ok(meta.hash))goto invalid;
    target=esp_ota_get_next_update_partition(NULL);
    if(!target||target==esp_ota_get_running_partition()||target->address!=meta.address||
       meta.size<1024||meta.size>target->size||meta.checkpoint>meta.size||meta.checkpoint%SECTOR)goto invalid;
    offset=meta.checkpoint;return;
invalid:
    memset(&meta,0,sizeof(meta));target=NULL;offset=0;
}
static int status_cmd(int argc,char **argv) {
    (void)argv;if(argc!=1)return 1;
    load();response("status",NULL);return 0;
}
static int fail(const char *reason) { response("error",reason);return 1; }
static int begin_cmd(int argc,char **argv) {
    uint32_t size;
    if(argc!=3||!number(argv[1],&size)||!hash_ok(argv[2]))return fail("arguments");
    load();
    if(active) {
        if(size==meta.size&&!strcmp(argv[2],meta.hash)){response("ready",NULL);return 0;}
        return fail("different_image_abort_first");
    }
    const esp_partition_t *running=esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if(!running||esp_ota_get_boot_partition()!=running||esp_ota_get_state_partition(running,&state)!=ESP_OK||state!=ESP_OTA_IMG_VALID)
        return fail("running_slot_not_valid");
    const esp_partition_t *part=esp_ota_get_next_update_partition(NULL);
    if(!part||part==running||size<1024||size>part->size)return fail("size_or_slot");
    if(meta.magic==META_MAGIC&&(size!=meta.size||strcmp(argv[2],meta.hash)))return fail("different_image_abort_first");
    if(!prepare_cb||!prepare_cb())return fail("radio_or_ota_busy");
    target=part;
    esp_err_t err;
    if(meta.magic==META_MAGIC) {
        unsigned erased=(size+SECTOR-1)&~(SECTOR-1);
        err=erased>meta.checkpoint?esp_partition_erase_range(target,meta.checkpoint,erased-meta.checkpoint):ESP_OK;
        if(err==ESP_OK)err=esp_ota_resume(target,0,meta.checkpoint,&handle);
        offset=meta.checkpoint;
    } else {
        meta=(metadata_t){.magic=META_MAGIC,.address=target->address,.size=size};
        memcpy(meta.hash,argv[2],65);offset=0;
        if(!persist()){clear();return fail("checkpoint");}
        err=esp_ota_begin(target,size,&handle);
    }
    if(err!=ESP_OK)return fail("begin");
    active=true;linenoiseSetMachineMode(true);response("ready",NULL);return 0;
}
static int chunk_cmd(int argc,char **argv) {
    uint32_t at,expected_crc;
    if(argc!=5||!active||strcmp(argv[1],meta.hash)||!number(argv[2],&at)||!number(argv[3],&expected_crc))return fail("chunk_arguments");
    unsigned chars=strlen(argv[4]);
    if(!chars||chars>CHUNK*2||chars%2)return fail("chunk_size");
    unsigned size=chars/2;
    if(at%CHUNK||at>meta.size||size!=((meta.size-at)<CHUNK?meta.size-at:CHUNK))return fail("chunk_bounds");
    uint8_t data[CHUNK],previous[CHUNK];
    for(unsigned i=0;i<size;i++) {
        int a=nibble(argv[4][i*2]),b=nibble(argv[4][i*2+1]);
        if(a<0||b<0)return fail("chunk_hex");
        data[i]=(a<<4)|b;
    }
    if(crc32(data,size)!=expected_crc)return fail("chunk_crc");
    if(at<offset) {
        if(at+size>offset||esp_partition_read(target,at,previous,size)!=ESP_OK||memcmp(data,previous,size))return fail("duplicate_mismatch");
        response("ack",NULL);return 0;
    }
    if(at!=offset)return fail("offset");
    if(esp_ota_write(handle,data,size)!=ESP_OK)return fail("write");
    offset+=size;
    if(offset%SECTOR==0) {
        meta.checkpoint=offset;
        if(!persist()){esp_ota_abort(handle);active=false;linenoiseSetMachineMode(false);return fail("checkpoint");}
    }
    response("ack",NULL);return 0;
}
static int finish_cmd(int argc,char **argv) {
    if(argc!=2||!active||strcmp(argv[1],meta.hash)||offset!=meta.size)return fail("incomplete");
    uint8_t data[1024],digest[32];char hex[65];
    psa_hash_operation_t sha=PSA_HASH_OPERATION_INIT;size_t digest_size=0;
    int err=psa_hash_setup(&sha,PSA_ALG_SHA_256);
    for(unsigned at=0;!err&&at<meta.size;at+=sizeof(data)) {
        unsigned size=meta.size-at<sizeof(data)?meta.size-at:sizeof(data);
        err=esp_partition_read(target,at,data,size);
        if(!err)err=psa_hash_update(&sha,data,size);
        vTaskDelay(1);
    }
    if(!err)err=psa_hash_finish(&sha,digest,sizeof(digest),&digest_size);
    psa_hash_abort(&sha);
    if(err||digest_size!=32)return fail("hash_read");
    for(int i=0;i<32;i++)snprintf(hex+2*i,3,"%02x",digest[i]);
    if(strcmp(hex,meta.hash))return fail("sha256");
    esp_app_desc_t desc;
    if(esp_ota_get_partition_description(target,&desc)!=ESP_OK||strcmp(desc.project_name,"projectZerobyLOCOSP"))return fail("project");
    err=esp_ota_end(handle);active=false;linenoiseSetMachineMode(false);
    if(err!=ESP_OK)return fail("image_validation");
    if(esp_ota_set_boot_partition(target)!=ESP_OK)return fail("boot_selection");
    response("applied",NULL);clear();vTaskDelay(pdMS_TO_TICKS(500));
    if(restart_cb)restart_cb();
    return 0;
}
static int abort_cmd(int argc,char **argv) {
    if(argc!=2)return fail("arguments");
    load();
    if(meta.magic==META_MAGIC&&strcmp(argv[1],meta.hash))return fail("image_mismatch");
    if(active)esp_ota_abort(handle);
    active=false;linenoiseSetMachineMode(false);clear();response("aborted",NULL);return 0;
}
void uota_register(bool (*prepare)(void),void (*restart)(void)) {
    prepare_cb=prepare;restart_cb=restart;
    const esp_console_cmd_t commands[]={
        {.command="uota_status",.help="USB OTA status/resume offset",.func=status_cmd},
        {.command="uota_begin",.help="USB OTA begin/resume: size sha256",.func=begin_cmd},
        {.command="uota_chunk",.help="USB OTA chunk: sha256 offset crc32 hex",.func=chunk_cmd},
        {.command="uota_finish",.help="Verify USB OTA and reboot: sha256",.func=finish_cmd},
        {.command="uota_abort",.help="Discard pending USB OTA: sha256",.func=abort_cmd},
    };
    for(unsigned i=0;i<sizeof(commands)/sizeof(commands[0]);i++)ESP_ERROR_CHECK(sw_register_command(&commands[i]));
}
