#include "dare.h"

int GetFadeDuration(_SND_tdstBlockEvent *_pstBlockEvent) {
    switch (_pstBlockEvent->eType) {
        case EVT_SON_PLAY:
            return _pstBlockEvent->uParams.stPlay.rFadeDuration;
        case EVT_SON_PITCH:
        case EVT_SON_PAN:
        case EVT_SON_VOLUME:
            return _pstBlockEvent->uParams.stExtraParam.rFadeDuration;
        case EVT_SON_EXTRA:
            return _pstBlockEvent->uParams.stExtraParam.rFadeDuration;
        case EVT_SON_STOP_N_GO:
            return _pstBlockEvent->uParams.stStopNGo.rFadeDuration;
        default:
            return 0;
    }
}
