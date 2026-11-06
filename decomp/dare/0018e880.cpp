#include "dare.h"

extern int gs_bInitFastIOPMemDone;
extern struct { void *ptr; int pad; } gs_stIOPMemBlock[1];
extern void sceSifInitIopHeap(void);
extern void *sceSifAllocIopHeap(unsigned int size);

void SND_fn_vInitFastIOPMem(void) {
    if (!gs_bInitFastIOPMemDone) {
        sceSifInitIopHeap();
        for (int i = 0; i < 1; i++) {
            void *ptr = sceSifAllocIopHeap(0x8000);
            gs_stIOPMemBlock[i].ptr = ptr;
            gs_stIOPMemBlock[i].pad = 0;
        }
        gs_bInitFastIOPMemDone = 1;
    }
}
