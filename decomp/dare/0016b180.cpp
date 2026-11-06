#include "dare.h"

unsigned short RPS2::GetResNbChannels(_tdstBlockResourceMem *pstBRMem) {
    if (pstBRMem == 0) return 0;
    if (pstBRMem->eType == TYPE_SAMPLE) {
        return pstBRMem->uRes.stSample.uwNbChannels;
    }
    return 0;
}
