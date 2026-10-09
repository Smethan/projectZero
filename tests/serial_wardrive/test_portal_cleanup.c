/* Exercise production failed-start recovery before and after AP netif creation. */
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef struct { int unused; } esp_netif_t;
enum { RADIO_MODE_NONE, RADIO_MODE_WIFI, RADIO_MODE_BLE, RADIO_MODE_IEEE802154 };
static int current_radio_mode;
static bool wifi_initialized,portal_active,darksword_active,karma_mode_active,rogueap_mode_active;
static void *portal_server;
static char *portalSSID;
static esp_netif_t ap_fixture,*ap_netif_handle,*lookup_ap;
static unsigned wifi_stops,wifi_deinits,server_stops,dhcp_stops,destroys,reinitializations;
static int httpd_stop(void *server) { assert(server);server_stops++;return 0; }
static esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) {
    assert(!strcmp(key,"WIFI_AP_DEF"));return lookup_ap;
}
static int esp_netif_dhcps_stop(esp_netif_t *ap) { assert(ap);dhcp_stops++;return 0; }
static int esp_wifi_stop(void) { wifi_stops++;return 0; }
static int esp_wifi_deinit(void) { wifi_deinits++;return 0; }
static void esp_netif_destroy(esp_netif_t *ap) { assert(ap==&ap_fixture);destroys++;lookup_ap=NULL; }
static bool ensure_wifi_mode(void) {
    assert(!wifi_initialized && current_radio_mode==RADIO_MODE_NONE);
    reinitializations++;wifi_initialized=true;current_radio_mode=RADIO_MODE_WIFI;return true;
}
#include "portal_cleanup_under_test.inc"
int main(void) {
    for(unsigned created=0;created<2;created++) {
        wifi_initialized=true;current_radio_mode=RADIO_MODE_WIFI;
        portal_active=darksword_active=karma_mode_active=rogueap_mode_active=true;
        portal_server=(void *)1;portalSSID=malloc(5);assert(portalSSID);
        lookup_ap=created?&ap_fixture:NULL;ap_netif_handle=lookup_ap;
        zero_job_restore_sta_after_partial_portal();
        assert(!portalSSID&&!portal_server&&!portal_active&&!darksword_active&&!karma_mode_active&&!rogueap_mode_active);
        assert(!ap_netif_handle && !lookup_ap && wifi_initialized && current_radio_mode==RADIO_MODE_WIFI);
        assert(wifi_stops==created+1 && wifi_deinits==created+1 && reinitializations==created+1);
    }
    assert(server_stops==2&&dhcp_stops==1&&destroys==1);
    puts("PASS: production portal failed-start cleanup with and without AP netif ownership");
}
