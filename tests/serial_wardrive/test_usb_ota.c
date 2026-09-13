#include "../../ESP32C5/main/usb_ota.c"
esp_err_t sw_register_command(const esp_console_cmd_t *c){return 0;}
static bool prepared(void){return true;}
static bool rebooted;
static void restarted(void){rebooted=true;}
static uint8_t image[8192];static char hash[65];
static void fresh(void) {
    active=false;running_index=boot_index=0;running_state=2;clear();output_capture[0]=0;
    flash_fail=validate_fail=nvs_fail=rebooted=false;write_calls=0;
    for(unsigned i=0;i<sizeof(image);i++)image[i]=(i*7)&255;
    image[0]=0xe9;uint8_t digest[32];SHA256(image,sizeof(image),digest);
    for(int i=0;i<32;i++)snprintf(hash+2*i,3,"%02x",digest[i]);
    uota_register(prepared,restarted);
}
static int begin(void){char *args[]={"uota_begin","8192",hash};return begin_cmd(3,args);}
static int chunk(unsigned at) {
    char position[20],crc[20],hex[513];snprintf(position,sizeof(position),"%u",at);
    snprintf(crc,sizeof(crc),"%u",crc32(image+at,256));
    for(int i=0;i<256;i++)snprintf(hex+i*2,3,"%02x",image[at+i]);
    char *args[]={"uota_chunk",hash,position,crc,hex};return chunk_cmd(5,args);
}
static int finish(void){char *args[]={"uota_finish",hash};return finish_cmd(2,args);}
int main(void) {
    fresh();assert(crc32((uint8_t*)"123456789",9)==0xcbf43926);
    assert(!begin()&&active);assert(!chunk(0));unsigned writes=write_calls;
    assert(!chunk(0)&&write_calls==writes&&offset==256); /* lost ACK retry */
    assert(chunk(512)&&offset==256);assert(finish()&&!rebooted);
    for(unsigned at=256;at<4608;at+=256)assert(!chunk(at));
    assert(meta.checkpoint==4096&&offset==4608);
    active=false;memset(&meta,0,sizeof(meta));offset=0; /* power loss */
    load();assert(offset==4096&&!active);assert(!begin());
    assert(erase_at==4096&&erase_size==4096&&!memcmp(flash_bytes,image,4096));
    for(unsigned at=4096;at<sizeof(image);at+=256)assert(!chunk(at));
    assert(!finish()&&rebooted&&boot_index==1&&!saved_size);
    fresh();assert(!begin());for(unsigned at=0;at<sizeof(image);at+=256)assert(!chunk(at));
    flash_bytes[123]^=1;assert(finish()&&!rebooted&&boot_index==0); /* whole-image SHA */
    fresh();assert(!begin());flash_fail=true;assert(chunk(0)&&offset==0);
    fresh();assert(!begin());for(unsigned at=0;at<3840;at+=256)assert(!chunk(at));
    nvs_fail=true;assert(chunk(3840)&&!active);nvs_fail=false;load();assert(offset==0);
    fresh();running_state=1;assert(begin()&&!active&&!saved_size);
    fresh();assert(!begin());char other[65];memset(other,'a',64);other[64]=0;
    char *wrong[]={"uota_begin","8192",other};assert(begin_cmd(3,wrong)&&active);
    char *abort[]={"uota_abort",hash};assert(!abort_cmd(2,abort)&&!active&&!saved_size);
    fresh();assert(!begin());for(unsigned at=0;at<sizeof(image);at+=256)assert(!chunk(at));
    validate_fail=true;assert(finish()&&!rebooted&&boot_index==0);
    puts("PASS: USB OTA CRC, duplicate ACK recovery, offsets, reboot checkpoint/erase, SHA, flash/NVS failures and boot validation");
}
