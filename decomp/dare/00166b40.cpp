#include "dare.h"

int OneShotMULTITRACKPath::PreSetResource(const _SND_tdstToolSourceFormat *pSourceFormat, _tdstBlockResourceMem *pRes) {
    if (pSourceFormat->bStream == m_stSourceFormat.bStream) {
        return m_pSource->PreSetResource(pSourceFormat);
    }
    return (Create_Source(&m_pNextSource, pSourceFormat, pRes) != 0) ^ 1;
}
