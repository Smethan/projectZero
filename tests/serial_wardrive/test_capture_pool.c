#include "capture_pool.h"
int main(void) {
    capture_pool_t p={0};
    assert(!capture_pool_open(&p,0,2304));
    assert(!capture_pool_open(&p,9,2304));
    assert(!capture_pool_open(&p,8,SIZE_MAX));
    for(int failure=0;failure<2;failure++) {
        queue_fail_after=failure;
        assert(!capture_pool_open(&p,8,2304));
        assert(!heap_live && !p.ready && !p.available && !p.frames);
    }
    queue_fail_after=-1;
    assert(capture_pool_open(&p,8,2304));
    assert(!capture_pool_open(&p,8,2304)); /* never leak a live pool */
    for(unsigned i=0;i<8;i++) {
        uint8_t *frame=capture_pool_acquire(&p);assert(frame);
        memset(frame,i,2304);assert(capture_pool_publish(&p,frame));
    }
    assert(!capture_pool_acquire(&p));
    uint8_t *held;assert(xQueueReceive(p.ready,&held,0));
    assert(!capture_pool_acquire(&p)); /* consumer still owns its frame */
    for(unsigned i=0;i<2304;i++) assert(held[i]==0);
    capture_pool_release(&p,held);
    uint8_t *reuse=capture_pool_acquire(&p);assert(reuse==held);
    memset(reuse,99,2304);assert(capture_pool_publish(&p,reuse));
    for(unsigned i=1;i<=8;i++) {
        uint8_t *frame;assert(xQueueReceive(p.ready,&frame,0));
        for(unsigned j=0;j<2304;j++) assert(frame[j]==(i==8?99:i));
        capture_pool_release(&p,frame);
    }
    assert(uxQueueMessagesWaiting(p.available)==8);
    capture_pool_close(&p);capture_pool_close(&p);assert(!heap_live);
    puts("PASS: capture pool allocation failures, bounded ownership, full-size frame integrity and cleanup");
}
