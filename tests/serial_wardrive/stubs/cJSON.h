#pragma once
#include <stddef.h>
typedef struct cJSON { double valuedouble; char *valuestring; char *string; struct cJSON *child,*next; } cJSON;
static inline cJSON *cJSON_ParseWithLength(const char *value,size_t length) { (void)value; (void)length; return NULL; }
static inline void cJSON_Delete(cJSON *item) { (void)item; }
static inline int cJSON_IsObject(const cJSON *item) { return item!=NULL; }
static inline int cJSON_IsString(const cJSON *item) { return item && item->valuestring; }
static inline int cJSON_IsArray(const cJSON *item) { return item!=NULL; }
static inline int cJSON_IsNumber(const cJSON *item) { return item!=NULL; }
static inline cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *object,const char *string) { (void)object;(void)string;return NULL; }
static inline int cJSON_GetArraySize(const cJSON *array) { (void)array;return 0; }
static inline cJSON *cJSON_GetArrayItem(const cJSON *array,int index) { (void)array;(void)index;return NULL; }
