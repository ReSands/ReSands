#include "dare.h"

void SND_fn_vConvertResDiskToMemTheme(_tdstBlockResourceDisk* pResDisk, _tdstBlockResourceMem* pResMem) {
    pResMem->Id = pResDisk->Id;
    pResMem->eType = pResDisk->eType;
    pResMem->rVolume_dB = pResDisk->rVolume_dB;
    pResMem->bIsLoaded = 1;
    pResMem->uRes.stTheme.ulStartLoop = pResDisk->uRes.stTheme.ulStartLoop;
    pResMem->uRes.stTheme.ulNbParts = pResDisk->uRes.stTheme.ulNbParts;
    pResMem->uRes.stTheme.ulNbLoops = pResDisk->uRes.stTheme.ulNbLoops;
    pResMem->uRes.stTheme.rThemeLength = pResDisk->uRes.stTheme.rThemeLength;
    pResMem->uRes.stTheme.rPosMainReLoop = pResDisk->uRes.stTheme.rPosMainReLoop;

    if (pResDisk->uRes.stTheme.rIndexFadeInDur == -65536) {
        pResMem->uRes.stTheme.rIndexFadeInDur = SND_fn_rGetDefaultIndexedFadeInDur();
        pResMem->uRes.stTheme.eIndexFadeInType = (_SND_tdeFadeType)SND_fn_eGetDefaultIndexedFadeInType();
    } else {
        pResMem->uRes.stTheme.rIndexFadeInDur = pResDisk->uRes.stTheme.rIndexFadeInDur;
        pResMem->uRes.stTheme.eIndexFadeInType = pResDisk->uRes.stTheme.eIndexFadeInType;
    }

    if (pResDisk->uRes.stTheme.rIndexFadeOutDur == -65536) {
        pResMem->uRes.stTheme.rIndexFadeOutDur = SND_fn_rGetDefaultIndexedFadeOutDur();
        pResMem->uRes.stTheme.eIndexFadeOutType = (_SND_tdeFadeType)SND_fn_eGetDefaultIndexedFadeOutType();
    } else {
        pResMem->uRes.stTheme.rIndexFadeOutDur = pResDisk->uRes.stTheme.rIndexFadeOutDur;
        pResMem->uRes.stTheme.eIndexFadeOutType = pResDisk->uRes.stTheme.eIndexFadeOutType;
    }

    pResMem->uRes.stTheme.bStartImmediatly = pResDisk->uRes.stTheme.bStartImmediatly;
    pResMem->uRes.stTheme.eTransition = pResDisk->uRes.stTheme.eTransition;
    pResMem->uRes.stTheme.ulTracksFading = pResDisk->uRes.stTheme.ulTracksFading;
}
