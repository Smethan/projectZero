#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
typedef struct {char cc[3];unsigned schan,nchan,policy;} wifi_country_t;
#define ESP_OK 0
#define WIFI_COUNTRY_POLICY_AUTO 0
#define MY_LOG_INFO(...) ((void)0)
static bool managed;
static unsigned writes;
static int result;
static bool sw_job_owned(void) {return managed;}
static int esp_wifi_set_country(const wifi_country_t *country) {
    writes++;assert(!strcmp(country->cc,"PH")&&country->schan==1&&country->nchan==14);return result;
}
static const char *esp_err_to_name(int error) {(void)error;return "fixture";}
#include "country_under_test.inc"
int main(void) {
    managed=true;assert(wifi_apply_extended_country()==0&&!writes);
    managed=false;assert(wifi_apply_extended_country()==0&&writes==1);
    result=-1;assert(wifi_apply_extended_country()==-1&&writes==2);
    managed=true;assert(wifi_apply_extended_country()==0&&writes==2);
    puts("PASS: managed WiFi preserves configured country; legacy setter and error propagation retained");
}
