/* The runner extracts the actual main.c function into this include. Exercise
 * every fallible startup operation, cleanup and retry without a real radio. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
typedef struct { int dummy; } esp_netif_config_t, wifi_init_config_t, wifi_config_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 257
#define ESP_NETIF_DEFAULT_WIFI_STA() {0}
#define WIFI_INIT_CONFIG_DEFAULT() {0}
#define WIFI_EVENT 1
#define IP_EVENT 2
#define ESP_EVENT_ANY_ID -1
#define IP_EVENT_STA_GOT_IP 3
#define WIFI_MODE_STA 1
#define WIFI_IF_STA 0
#define ESP_LOGE(...) ((void)0)
#define MY_LOG_INFO(...) ((void)0)
static bool netif_initialized, event_loop_initialized;
static bool wifi_event_handler_registered, ip_event_handler_registered;
static void *sta_netif_handle;
static int fail_at, calls, driver, netifs, deinit_count, wifi_reg, ip_reg;
static int step(void) { return ++calls==fail_at ? ESP_ERR_NO_MEM : ESP_OK; }
static void capture_memory_log(const char *s) {}
static void wifi_event_handler(void) {}
static void ip_event_handler(void) {}
static int esp_netif_init(void) { return step(); }
static int esp_event_loop_create_default(void) { return step(); }
static void *esp_netif_new(void *cfg) { if(step()) return NULL; netifs++; return &netifs; }
static int esp_netif_attach_wifi_station(void *n) { return step(); }
static int esp_wifi_set_default_wifi_sta_handlers(void) { return step(); }
static void esp_netif_destroy_default_wifi(void *n) { assert(netifs==1);netifs--; }
static int esp_wifi_init(void *cfg) { int r=step();if(!r)driver=1;return r; }
static int esp_event_handler_instance_register(int base,int id,void (*handler)(void),void *arg,void *instance) {
    int r=step();if(!r) { if(base==WIFI_EVENT)wifi_reg++;else ip_reg++; } return r;
}
static int esp_wifi_set_mode(int mode) { return step(); }
static int esp_wifi_set_config(int iface,void *cfg) { return step(); }
static int esp_wifi_start(void) { return step(); }
static int esp_wifi_stop(void) { return 0; }
static int esp_wifi_deinit(void) { driver=0;deinit_count++;return 0; }
static int esp_wifi_get_mac(int iface,uint8_t *mac) { memset(mac,0,6);return 0; }
#include "wifi_init_under_test.inc"
int main(void) {
    for(int failure=1;failure<=11;failure++) {
        netif_initialized=event_loop_initialized=false;
        wifi_event_handler_registered=ip_event_handler_registered=false;
        sta_netif_handle=NULL;calls=driver=netifs=deinit_count=wifi_reg=ip_reg=0;
        fail_at=failure;
        assert(wifi_init_ap_sta()==ESP_ERR_NO_MEM);
        assert(!driver && !netifs && !sta_netif_handle);
        assert(deinit_count==(failure>6));
        fail_at=0;calls=0;
        assert(wifi_init_ap_sta()==ESP_OK && driver && netifs==1);
        assert(wifi_reg==1 && ip_reg==1); /* successful handlers survive retry */
        esp_wifi_deinit();
        assert(wifi_init_ap_sta()==ESP_OK && driver && netifs==1);
        assert(wifi_reg==1 && ip_reg==1); /* reuse existing netif/handlers */
    }
    puts("PASS: Wi-Fi startup failure at all 11 stages, cleanup and retry without duplicate handlers");
}
