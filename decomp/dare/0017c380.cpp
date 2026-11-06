#include "dare.h"

void SND_fn_vUnLoadResBinarySnd(_tdstBlockResourceMem*pstBRMem) {
    if (pstBRMem->eType == TYPE_MULTITRACK) goto call;
    if (pstBRMem->eType == TYPE_SAMPLE) goto call;
    pstBRMem->bIsLoaded = 0; goto end;
call:
    SND_fn_vUnLoadResBinarySxd(pstBRMem);
    pstBRMem->bIsLoaded = 0;
end:
    pstBRMem->eType = TYPE_INVALID;
}
