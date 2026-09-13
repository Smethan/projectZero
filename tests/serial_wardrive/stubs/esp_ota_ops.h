#pragma once
#include "platform.h"
typedef struct {uint32_t address,size;char label[8];}esp_partition_t;
typedef unsigned esp_ota_handle_t;
typedef int esp_ota_img_states_t;
#define ESP_OTA_IMG_VALID 2
static esp_partition_t parts[2]={{0x20000,16384,"ota_0"},{0x420000,16384,"ota_1"}};
static unsigned running_index,boot_index,written;
static uint8_t flash_bytes[16384];
static bool flash_fail,validate_fail;
static int running_state=2;
static unsigned erase_at,erase_size,write_calls;
static const esp_partition_t *esp_ota_get_running_partition(void){return &parts[running_index];}
static const esp_partition_t *esp_ota_get_boot_partition(void){return &parts[boot_index];}
static const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *unused){return &parts[1-running_index];}
static int esp_ota_get_state_partition(const esp_partition_t *p,int *state){*state=running_state;return 0;}
static int esp_partition_erase_range(const esp_partition_t *p,unsigned at,unsigned size){assert(p!=esp_ota_get_running_partition());assert(!(at%4096)&&!(size%4096));assert(at+size<=sizeof(flash_bytes));erase_at=at;erase_size=size;memset(flash_bytes+at,255,size);return 0;}
static int esp_ota_begin(const esp_partition_t *p,unsigned size,esp_ota_handle_t *h){written=0;*h=1;return esp_partition_erase_range(p,0,(size+4095)&~4095u);}
static int esp_ota_resume(const esp_partition_t *p,unsigned erase,unsigned at,esp_ota_handle_t *h){assert(!erase);written=at;*h=1;return 0;}
static int esp_ota_write(esp_ota_handle_t h,const void *data,unsigned size){if(flash_fail)return 1;assert(written+size<=sizeof(flash_bytes));memcpy(flash_bytes+written,data,size);written+=size;write_calls++;return 0;}
static int esp_partition_read(const esp_partition_t *p,unsigned at,void *data,unsigned size){assert(at+size<=sizeof(flash_bytes));memcpy(data,flash_bytes+at,size);return 0;}
static int esp_ota_abort(esp_ota_handle_t h){return 0;}
static int esp_ota_end(esp_ota_handle_t h){return validate_fail?1:0;}
static int esp_ota_set_boot_partition(const esp_partition_t *p){boot_index=p==&parts[0]?0:1;return 0;}
