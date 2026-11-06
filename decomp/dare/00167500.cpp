#include "dare.h"

int Create_OneShotPath(OneShotPath **pPath, _SND_tdstToolSourceFormat *pSourceFormat, const _SND_tdstToolFormat *pFormat, _tdstBlockResourceMem *pRes, _SoundParam *pPar)
{
    switch (pRes->eType) {
    case TYPE_SAMPLE:
        return Create_OneShotSAMPLEPath(pPath, pSourceFormat, pFormat, pRes, pPar);
    case TYPE_MULTITRACK:
        return Create_OneShotMULTITRACKPath(pPath, pSourceFormat, pFormat, pRes, pPar);
    case TYPE_SILENCE:
        return Create_OneShotSILENCEPath(pPath, pSourceFormat, pFormat, pRes, pPar);
    default:
        return -1;
    }
}
