#pragma once
#include <stdint.h>
#include "esp_ota_ops.h"
typedef struct {char project_name[32];char version[32];uint8_t app_elf_sha256[32];}esp_app_desc_t;
static int esp_ota_get_partition_description(const esp_partition_t *p,esp_app_desc_t *d){strcpy(d->project_name,"projectZerobyLOCOSP");return 0;}
static const esp_app_desc_t test_app_description={
    .project_name="projectZerobyLOCOSP",.version="1.8.5",
    .app_elf_sha256={0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef}
};
static const esp_app_desc_t *esp_app_get_description(void){return &test_app_description;}
static size_t esp_app_get_elf_sha256(char *dst,size_t max_len){
    /* Match the shipping CONFIG_APP_RETRIEVE_LEN_ELF_SHA=9: the formatted
     * helper cannot supply the 16 hex characters required by device_info. */
    static const char configured_prefix[]="012345678";
    if(!dst||max_len<sizeof(configured_prefix))return 0;
    memcpy(dst,configured_prefix,sizeof(configured_prefix));
    return sizeof(configured_prefix);
}
