#include "dare.h"

extern int gs_bInitFastIOPMemDone;
extern struct { void *ptr; int pad; } gs_stIOPMemBlock[1];
extern int sceSifFreeIopHeap(void *heap);

void SND_fn_vFastFreeIOP(void *pvIOPMem) {
    if (gs_bInitFastIOPMemDone) {
        if (pvIOPMem != 0) {
            for (int i = 0; i < 1; i++) {
                if (gs_stIOPMemBlock[i].ptr == pvIOPMem) {
                    gs_stIOPMemBlock[i].pad = 0;
                    return;
                }
            }
            sceSifFreeIopHeap(pvIOPMem);
        }
    }
}
