#include "dare.h"
extern int Create_SMemory(CSource **pSource, const _SND_tdstToolSourceFormat *pSourceFormat, _tdstBlockResourceMem *pRes);
extern int Create_SStream(CSource **pSource, const _SND_tdstToolSourceFormat *pSourceFormat, _tdstBlockResourceMem *pRes);
int Create_Source(CSource **pSource, const _SND_tdstToolSourceFormat *pSourceFormat, _tdstBlockResourceMem *pRes)
{
    if (pSourceFormat->bStream)
        return Create_SStream(pSource, pSourceFormat, pRes);
    return Create_SMemory(pSource, pSourceFormat, pRes);
}
