#include "dare.h"

extern int gs_bInitFastIOPMemDone;
extern struct { void *ptr; int pad; } gs_stIOPMemBlock[1];
extern int sceSifFreeIopHeap(void *heap);

void SND_fn_vDesInitFastIOPMem(void) {
    if (gs_bInitFastIOPMemDone) {
        for (int i = 0; i < 1; i++) {
            if (gs_stIOPMemBlock[i].ptr) {
                sceSifFreeIopHeap(gs_stIOPMemBlock[i].ptr);
                gs_stIOPMemBlock[i].ptr = 0;
            }
        }
        gs_bInitFastIOPMemDone = 0;
    }
}
