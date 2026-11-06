#include "dare.h"

int SetParamAsso(ASSOCIATION_DESCRIPTION_ *_pAsso, _SoundParam *_pstParam) {
    switch (_pAsso->eType) {
        case ASSO_PLAY:
            return SetParamAssoPlay(_pAsso, _pstParam);
        case ASSO_SWITCH:
            return SetParamAssoSwitch(_pAsso, _pstParam);
        case ASSO_STOPNGO:
            return SetParamAssoStopNGo(_pAsso, _pstParam);
        default:
            return 0;
    }
}
