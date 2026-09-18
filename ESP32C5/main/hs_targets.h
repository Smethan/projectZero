#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define HS_TARGET_LIMIT 16
#define HS_EXCLUSION_LIMIT 32
#define HS_SCOPE_LIMIT HS_EXCLUSION_LIMIT
/* Immutable while the callback is installed. Empty means explicit ALL;
 * exclude reverses membership so listed BSSIDs are never captured/deauthed. */
typedef struct {
    unsigned count;
    bool exclude;
    uint8_t mac[HS_SCOPE_LIMIT][6], channel[HS_SCOPE_LIMIT];
} hs_target_set;
static int hst_hex(char c) {
    if(c>='0'&&c<='9')return c-'0';
    if(c>='a'&&c<='f')return c-'a'+10;
    if(c>='A'&&c<='F')return c-'A'+10;
    return -1;
}
static bool hst_contains(const hs_target_set *set,const uint8_t *mac) {
    if(!set->count)return true;
    bool listed=false;
    for(unsigned i=0;i<set->count;i++)if(!memcmp(set->mac[i],mac,6)){listed=true;break;}
    return set->exclude?!listed:listed;
}
static bool hst_parse_limit(hs_target_set *set,const char *list,unsigned limit) {
    memset(set,0,sizeof(*set));
    if(!list||!*list)return false;
    while(*list) {
        if(set->count==limit||strlen(list)<17)return false;
        uint8_t *mac=set->mac[set->count];
        for(unsigned i=0;i<6;i++) {
            int a=hst_hex(list[i*3]),b=hst_hex(list[i*3+1]);
            if(a<0||b<0||(i<5&&list[i*3+2]!=':'))return false;
            mac[i]=(a<<4)|b;
        }
        static const uint8_t zero[6];
        if((mac[0]&1)||!memcmp(mac,zero,6))return false;
        for(unsigned i=0;i<set->count;i++)if(!memcmp(set->mac[i],mac,6))return false;
        set->count++;list+=17;
        if(!*list)return true;
        if(*list++!=','||!*list)return false;
    }
    return false;
}
static bool hst_parse(hs_target_set *set,const char *list) {
    return hst_parse_limit(set,list,HS_TARGET_LIMIT);
}
static bool hst_parse_exclusions(hs_target_set *set,const char *list) {
    if(!hst_parse_limit(set,list,HS_EXCLUSION_LIMIT))return false;
    set->exclude=true;
    return true;
}
static bool hst_channel(const hs_target_set *set,unsigned channel) {
    if(!set->count||set->exclude)return true;
    for(unsigned i=0;i<set->count;i++)if(set->channel[i]==channel)return true;
    return false;
}
static bool hst_frame_allowed(const hs_target_set *set,const uint8_t *p,size_t n) {
    if(!set->count)return true; /* Preserve existing unscoped behavior. */
    if(n<24||(p[0]&3))return false;
    unsigned type=(p[0]>>2)&3,ds=p[1]&3;
    const uint8_t *bssid;
    if(type==0) {
        bssid=p+16;
        unsigned subtype=p[0]>>4;
        if((subtype==8||subtype==5)&&memcmp(p+10,bssid,6))return false;
    } else if(type==2&&ds!=3) bssid=p+(ds==1?4:ds==2?10:16);
    else return false;
    return hst_contains(set,bssid);
}
