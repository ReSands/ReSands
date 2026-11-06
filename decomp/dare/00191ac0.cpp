#include "dare.h"

extern int gs_bInitPS2Reverb;
extern int gs_lSpuBiggestReverb;
extern void SND_fn_vDisplayError(int, const char*);

int SND_fn_lGetSpuBiggestReverb(void) {
    if (gs_bInitPS2Reverb == 0) {
        SND_fn_vDisplayError(8, "Cannot return reverb, no sound project loaded.");
    }
    return gs_lSpuBiggestReverb;
}
