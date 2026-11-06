#include "dare.h"

extern "C" void SetVolFadeIn(ASSOCIATION_DESCRIPTION_* pAsso, _SoundParam* pParam) {
    if (pAsso->eType != ASSO_PLAY) return;
    if (pAsso->uParam.stPlayParam.FadeInParam.lFadeTimer == -2) return;

    int rFadeDuration;
    _SND_tdeFadeType eEvtFadeType;
    int rPosition;

    if (pAsso->uParam.stPlayParam.FadeInParam.bWaitingForResource != 0) {
        int pos = SND_fn_rGetPosSnd(pAsso->uParam.stPlayParam.lVoice);
        if (pos == -0x10000 || pos > 0) {
            SND_fn_vResumeTimer(pAsso->uParam.stPlayParam.FadeInParam.lFadeTimer);
            pAsso->uParam.stPlayParam.FadeInParam.bWaitingForResource = 0;
        }
    }

    rPosition = pAsso->uParam.stPlayParam.FadeInParam.rTimeOffset + SND_fn_rGetCurrentTime(pAsso->uParam.stPlayParam.FadeInParam.lFadeTimer);
    rFadeDuration = pAsso->uParam.stPlayParam.FadeInParam.rFadeDuration;

    if (rFadeDuration == -0x10000) {
        rFadeDuration = SND_fn_rGetDefaultEventFadeInDur();
        eEvtFadeType = SND_fn_eGetDefaultEventFadeInType();
    } else {
        eEvtFadeType = pAsso->uParam.stPlayParam.FadeInParam.eFadeType;
    }

    if (rPosition >= rFadeDuration) {
        SND_fn_vDestroyTimer(pAsso->uParam.stPlayParam.FadeInParam.lFadeTimer);
        pAsso->uParam.stPlayParam.FadeInParam.lFadeTimer = -2;
        pAsso->uParam.stPlayParam.FadeInParam.rTimeOffset = 0;
    } else {
        int attenuation = fn_rGetVolFadeAttenuation(SND_fn_rDivRealRealSnd(rPosition, rFadeDuration), eEvtFadeType);
        pParam->rVol_dB += attenuation;
        if (pParam->rVol_dB < (int)0xffa00000) {
            pParam->rVol_dB = (int)0xffa00000;
        }
    }
}
