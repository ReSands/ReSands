#include "dare.h"

int RPS2::IsResLooping(_tdstBlockResourceMem *pstBRMem)
{
    if (pstBRMem == 0) return 0;
    if (pstBRMem->eType == TYPE_SAMPLE) {
        return pstBRMem->uRes.stSample.bLoop;
    }
    return 0;
}
