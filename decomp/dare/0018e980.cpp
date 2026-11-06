#include "dare.h"

extern int gs_bInitFastIOPMemDone;
extern struct { void *ptr; int pad; } gs_stIOPMemBlock[1];
extern void *sceSifAllocIopHeap(unsigned int size);

void *SND_fn_pvFastMallocIOP(unsigned int ulSize) {
    if (!gs_bInitFastIOPMemDone) {
        return 0;
    }
    if (ulSize < 0x8001) {
        for (int i = 0; i < 1; i++) {
            if (gs_stIOPMemBlock[i].pad == 0) {
                gs_stIOPMemBlock[i].pad = 1;
                return gs_stIOPMemBlock[i].ptr;
            }
        }
    }
    SND_fn_vDisplayErrorEx(8, "Allocating dynamically on IOP, this should not happen often.", 3);
    return sceSifAllocIopHeap(ulSize);
}
