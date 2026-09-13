#pragma once
#include "platform.h"
#define NVS_READWRITE 1
#define NVS_READONLY 0
typedef int nvs_handle_t;
static uint8_t saved_meta[128];
static unsigned saved_size;
static bool nvs_fail;
static int nvs_open(const char *s,int mode,nvs_handle_t *h){*h=1;return 0;}
static int nvs_set_blob(nvs_handle_t h,const char *key,const void *data,unsigned size){if(nvs_fail)return 1;assert(size<=sizeof(saved_meta));memcpy(saved_meta,data,size);saved_size=size;return 0;}
static int nvs_get_blob(nvs_handle_t h,const char *key,void *data,size_t *size){if(!saved_size)return 1;assert(*size>=saved_size);memcpy(data,saved_meta,saved_size);*size=saved_size;return 0;}
static int nvs_erase_key(nvs_handle_t h,const char *key){saved_size=0;return 0;}
static int nvs_commit(nvs_handle_t h){return nvs_fail?1:0;}
static void nvs_close(nvs_handle_t h){}
