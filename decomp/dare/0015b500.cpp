#include "dare.h"

void ResumeAsso(ASSOCIATION_DESCRIPTION_* pAsso) {
    if (pAsso->_retail04 != 0) return;
    if (pAsso->lPauseCounter == 0) return;
    
    pAsso->lPauseCounter--;
    if (pAsso->lPauseCounter > 0) return;
    
    pAsso->bResumeAssoWhenNextSynchro = 1;
    if (pAsso->PauseResumeFadeParam.lFadeTimer == -2) {
        // skip
    } else {
        SND_fn_vDestroyTimer(pAsso->PauseResumeFadeParam.lFadeTimer);
        pAsso->PauseResumeFadeParam.lFadeTimer = -2;
        pAsso->bResumeAssoWhenNextSynchro = 0;
    }
    
    pAsso->PauseResumeFadeParam.bWaitingForResource = 0;
    pAsso->PauseResumeFadeParam.eFadeType = (_SND_tdeFadeType)0;
    pAsso->PauseResumeFadeParam.rFadeDuration = 0;
    pAsso->PauseResumeFadeParam.eFadeDirection = (_tdeFadeDirection)0;
    pAsso->PauseResumeFadeParam.rTimeOffset = 0;
}
