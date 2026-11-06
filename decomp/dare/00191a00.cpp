#include "dare.h"

extern int gs_bInitPS2Reverb;
extern int gs_lSpuBiggestReverb;
extern int fn_lGetEffectRequiredSize(int lMode);

_SND_tdeInit SND_fn_eInitSpuReverb(void) {
    int lEffect;
    int lMaxSize;
    int i;
    _tdstReverbPreset *pstReverbPreset;
    _tdstReverbPresetList *pstReverbPresetList;

    lEffect = 0;
    lMaxSize = 0;
    pstReverbPresetList = SND_fn_pstGetReverbPresetList();
    pstReverbPreset = pstReverbPresetList->hFirstElementOpt;

    for (i = 0; i < pstReverbPresetList->lNumberOfElementsOpt; i++, pstReverbPreset++) {
        int lSize = fn_lGetEffectRequiredSize(pstReverbPreset->lMode);
        if (lMaxSize < lSize) {
            lEffect = pstReverbPreset->lMode;
            lMaxSize = lSize;
        }
    }

    gs_lSpuBiggestReverb = lEffect;
    gs_bInitPS2Reverb = 1;

    return (_SND_tdeInit)1;
}
