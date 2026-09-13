#include "../../ESP32C5/main/hs_monitor.c"
static pcap_frame_observer_t observer;
void pcap_serializer_set_observer(pcap_frame_observer_t cb) { observer=cb; }
int main(void) {
    test_clock=1000;
    assert(hsm_start(true));assert(observer);
    uint8_t frame[336]={0};frame[0]=8;frame[1]=2;
    uint8_t llc[]={0xaa,0xaa,3,0,0,0,0x88,0x8e};memcpy(frame+24,llc,8);
    frame[32]=2;frame[33]=3;frame[34]=1;frame[35]=44;
    observer(frame,20);assert(!uxQueueMessagesWaiting(queue));
    for(int i=0;i<6;i++) observer(frame,sizeof(frame));
    assert(uxQueueMessagesWaiting(queue)==4 && atomic_load(&dropped)==2);
    atomic_store(&stopping,true);test_task(NULL);
    assert(!atomic_load(&running));
    assert(strstr(output_capture,"HSC:") && strstr(output_capture,"\"storage\":\"serial\""));
    assert(strstr(output_capture,"\"offset\":240") && strstr(output_capture,"\"kind\":\"stopped\""));
    hsm_stop();assert(!observer);
    assert(hsm_start(false));output_capture[0]=0;
    atomic_store(&stopping,true);test_task(NULL);
    assert(strstr(output_capture,"\"storage\":\"sd\""));hsm_stop();
    task_ok=0;assert(!hsm_start(false));assert(!observer && !atomic_load(&running));
    puts("PASS: HS progress framing, queue limits, mode identity, cleanup and allocation failure");
}
