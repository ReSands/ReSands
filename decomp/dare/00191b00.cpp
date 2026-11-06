#include "dare.h"

extern int gs_bInitPS2Reverb;
extern int gs_lSpuBiggestReverb;
extern char g_62[47];
extern void SND_fn_vDisplayError(int, char*);
extern int fn_lGetEffectRequiredSize(int);

int SND_fn_lGetSpuBiggestReverbSize(void) {
    if (gs_bInitPS2Reverb == 0) {
        SND_fn_vDisplayError(8, g_62);
    }
    return fn_lGetEffectRequiredSize(gs_lSpuBiggestReverb);
}
