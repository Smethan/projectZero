#pragma once
#include "esp_ota_ops.h"
typedef struct {char project_name[32];char version[32];}esp_app_desc_t;
static int esp_ota_get_partition_description(const esp_partition_t *p,esp_app_desc_t *d){strcpy(d->project_name,"projectZerobyLOCOSP");return 0;}
static const esp_app_desc_t test_app_description={.project_name="projectZerobyLOCOSP",.version="1.8.5"};
static const esp_app_desc_t *esp_app_get_description(void){return &test_app_description;}
static size_t esp_app_get_elf_sha256(char *dst,size_t max_len){
    static const char build_id[]="0123456789abcdef";
    if(!dst||max_len<sizeof(build_id)-1)return 0;
    memcpy(dst,build_id,sizeof(build_id));
    return sizeof(build_id)-1;
}
