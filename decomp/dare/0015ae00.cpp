#include "dare.h"

void KillAndDestroyAssoPlay(ASSOCIATION_DESCRIPTION_* _pAsso) {
    if (_pAsso->uParam.stPlayParam.lVoice != -1) {
        SND_fn_vStopSnd(_pAsso->uParam.stPlayParam.lVoice);
        _pAsso->uParam.stPlayParam.lVoice = -1;
    }
    if (_pAsso->uParam.stPlayParam.FadeInParam.lFadeTimer != -2) {
        SND_fn_vDestroyTimer(_pAsso->uParam.stPlayParam.FadeInParam.lFadeTimer);
        _pAsso->uParam.stPlayParam.FadeInParam.lFadeTimer = -2;
    }
    if (_pAsso->uParam.stPlayParam.FadeOutParam.lFadeTimer != -2) {
        SND_fn_vDestroyTimer(_pAsso->uParam.stPlayParam.FadeOutParam.lFadeTimer);
        _pAsso->uParam.stPlayParam.FadeOutParam.lFadeTimer = -2;
    }
    if (_pAsso->PauseResumeFadeParam.lFadeTimer != -2) {
        SND_fn_vDestroyTimer(_pAsso->PauseResumeFadeParam.lFadeTimer);
        _pAsso->PauseResumeFadeParam.lFadeTimer = -2;
        _pAsso->lPauseCounter = 0;
        _pAsso->bResumeAssoWhenNextSynchro = 0;
        _pAsso->PauseResumeFadeParam.eFadeDirection = (_tdeFadeDirection)0;
    }
}
