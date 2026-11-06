#include "dare.h"

void KillAndDestroyAsso(ASSOCIATION_DESCRIPTION_ *_pAsso) {
    switch (_pAsso->eType) {
        case ASSO_PLAY:
            KillAndDestroyAssoPlay(_pAsso);
            break;
        case ASSO_SWITCH:
            if (_pAsso->uParam.stSwitchParam.pChildAsso != 0) {
                KillAndDestroyAsso(_pAsso->uParam.stSwitchParam.pChildAsso);
                _pAsso->uParam.stSwitchParam.pChildAsso = 0;
            }
            break;
        case ASSO_STOPNGO:
            KillAndDestroyAssoStopNGo(_pAsso);
            break;
        default:
            break;
    }

    if (_pAsso->hNextBrotherDyn != 0) {
        _pAsso->hNextBrotherDyn->hPrevBrotherDyn = _pAsso->hPrevBrotherDyn;
    }
    if (_pAsso->hPrevBrotherDyn != 0) {
        _pAsso->hPrevBrotherDyn->hNextBrotherDyn = _pAsso->hNextBrotherDyn;
    }

    if (_pAsso->hFatherDyn != 0) {
        _pAsso->hFatherDyn->lNumberOfElementsDyn--;
        if (_pAsso->hFatherDyn->hFirstElementDyn == _pAsso) {
            _pAsso->hFatherDyn->hFirstElementDyn = _pAsso->hNextBrotherDyn;
        }
        if (_pAsso->hFatherDyn->hLastElementDyn == _pAsso) {
            _pAsso->hFatherDyn->hLastElementDyn = _pAsso->hPrevBrotherDyn;
        }
    }

    _pAsso->hNextBrotherDyn = 0;
    _pAsso->hPrevBrotherDyn = 0;
    _pAsso->hFatherDyn = 0;
    SND_fn_vFreeSnd(_pAsso);
}
