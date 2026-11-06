#include "dare.h"

extern _tdstReverbPresetList gs_stReverbPresetList;

_tdstReverbPreset*SND_fn_pstGetReverbPresetFromId(int lReverbPresetId) {
    _tdstReverbPreset*pstReverbPreset = gs_stReverbPresetList.hFirstElementOpt;
    int i = 0;
    while(i < gs_stReverbPresetList.lNumberOfElementsOpt) {
        if(pstReverbPreset->lId == lReverbPresetId) return pstReverbPreset;
        i++;
        pstReverbPreset++;
    }
    return 0;
}
