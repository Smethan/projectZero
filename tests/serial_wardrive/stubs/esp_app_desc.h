#pragma once
#include "esp_ota_ops.h"
typedef struct {char project_name[32];}esp_app_desc_t;
static int esp_ota_get_partition_description(const esp_partition_t *p,esp_app_desc_t *d){strcpy(d->project_name,"projectZerobyLOCOSP");return 0;}
