#include "dare.h"

int TPitchShifter::IsInputFormatValid(const _SND_tdstToolFormat *pFormat) const {
    if (!pFormat) return 0;
    if (pFormat->eFmt != m_stOutputFormat.eFmt) return 0;
    const unsigned short* pU = (const unsigned short*)pFormat;
    const unsigned short* tU = (const unsigned short*)this;
    if (pU[2] != tU[10]) return 0;
    if (pU[3] != tU[11]) return 0;
    return 1;
}
