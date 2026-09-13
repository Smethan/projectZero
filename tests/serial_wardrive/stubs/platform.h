#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 1
#define ESP_ERROR_CHECK(x) assert((x)==0)
typedef int (*esp_console_cmd_func_t)(int,char**);
typedef struct {const char *command,*help,*hint;esp_console_cmd_func_t func;void *argtable;} esp_console_cmd_t;
static int esp_console_cmd_register(const esp_console_cmd_t *c){return 0;}
#define CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG 1
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
#define portMUX_INITIALIZER_UNLOCKED 0
typedef int portMUX_TYPE;
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
typedef struct {int size,count,head,capacity;bool dynamic;uint8_t *data;} StaticQueue_t;
typedef StaticQueue_t *QueueHandle_t;
static int64_t test_clock;
static int64_t esp_timer_get_time(void){return test_clock*1000;}
static int queue_fail_after=-1;
static QueueHandle_t xQueueCreateStatic(int n,int size,uint8_t *data,StaticQueue_t *q){if(queue_fail_after==0)return NULL;if(queue_fail_after>0)--queue_fail_after;*q=(StaticQueue_t){.size=size,.data=data,.capacity=n};return q;}
static QueueHandle_t xQueueCreate(int n,int size){QueueHandle_t q=malloc(sizeof(*q));xQueueCreateStatic(n,size,calloc(n,size),q);q->dynamic=true;return q;}
static void vQueueDelete(QueueHandle_t q){if(q->dynamic){free(q->data);free(q);}}
static int xQueueSend(QueueHandle_t q,const void *p,int ticks){if(q->count>=q->capacity)return 0;memcpy(q->data+((q->head+q->count)%q->capacity)*q->size,p,q->size);q->count++;return 1;}
static int xQueueReceive(QueueHandle_t q,void *p,int ticks){test_clock+=ticks;if(!q->count)return 0;memcpy(p,q->data+q->head*q->size,q->size);q->head=(q->head+1)%q->capacity;q->count--;return 1;}
static int uxQueueMessagesWaiting(QueueHandle_t q){return q->count;}
static void xQueueReset(QueueHandle_t q){q->head=q->count=0;}
static void (*test_task)(void*);
static int task_ok=1;
static int xTaskCreate(void (*f)(void*),const char *name,int stack,void *arg,int pri,void *handle){test_task=f;return task_ok;}
static void vTaskDelay(int ms){test_clock+=ms;}
static void vTaskDelete(void *task){}
#define WIFI_PKT_MGMT 0
#define WIFI_PKT_DATA 1
#define WIFI_PROMIS_FILTER_MASK_MGMT 1
#define WIFI_PROMIS_FILTER_MASK_DATA 2
typedef int wifi_promiscuous_pkt_type_t;
typedef struct {int filter_mask;} wifi_promiscuous_filter_t;
typedef struct {struct {int sig_len;uint8_t channel;int8_t rssi;uint32_t timestamp;}rx_ctrl;uint8_t payload[512];}wifi_promiscuous_pkt_t;
static int esp_wifi_set_promiscuous_filter(const wifi_promiscuous_filter_t *f){return 0;}
static int esp_wifi_set_promiscuous_rx_cb(void (*f)(void*,wifi_promiscuous_pkt_type_t)){return 0;}
static int esp_wifi_set_promiscuous(bool on){return 0;}
static char output_capture[65536];
static int transport_limit=1024;
static int usb_serial_jtag_write_bytes(const char *buf,int n,int ticks){int count=n>256?0:(n<transport_limit?n:transport_limit);strncat(output_capture,buf,count);return count;}

#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_INTERNAL 2
#define MALLOC_CAP_8BIT 4
static bool fail_psram, fail_internal;
static unsigned heap_live, last_heap_caps;
static void *heap_caps_malloc(size_t size,unsigned caps){
    last_heap_caps=caps;
    if(((caps&MALLOC_CAP_SPIRAM)&&fail_psram)||((caps&MALLOC_CAP_INTERNAL)&&fail_internal))return NULL;
    void *p=malloc(size);if(p)heap_live++;return p;
}
static void heap_caps_free(void *p){if(p){assert(heap_live);heap_live--;free(p);}}
static size_t heap_caps_get_free_size(unsigned caps){return 12345;}
static size_t heap_caps_get_largest_free_block(unsigned caps){return 1234;}
#define ESP_LOGI(tag,fmt,...) ((void)0)
