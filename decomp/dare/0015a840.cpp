#include "dare.h"

int IsAssoStopable(ASSOCIATION_DESCRIPTION_ *_pAsso) {
    switch (_pAsso->eType) {
        case ASSO_PLAY:
            return _pAsso->pstBlockEvent->bStopable;
        case ASSO_SWITCH:
            return _pAsso->uParam.stSwitchParam.pChildAsso != 0 ? IsAssoStopable(_pAsso->uParam.stSwitchParam.pChildAsso) : 1;
        case ASSO_STOPNGO:
            return _pAsso->uParam.stStopNGoParam.pGoAsso != 0 ? IsAssoStopable(_pAsso->uParam.stStopNGoParam.pGoAsso) : 1;
        default:
            return 0;
    }
}
