#include "dare.h"

int OneShotSAMPLEPath::PreSetResource(const _SND_tdstToolSourceFormat *pSourceFormat, _tdstBlockResourceMem *pRes)
{
    int ret = 0;
    if (m_pSource) {
        if (pSourceFormat->bStream == m_stSourceFormat.bStream)
            return m_pSource->PreSetResource(pSourceFormat);
        ret = (Create_Source(&m_pNextSource, pSourceFormat, pRes) != 0) ^ 1;
    }
    return ret;
}
