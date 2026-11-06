#include "dare.h"

int TTransition::SetOutputFormat(const _SND_tdstToolFormat *pFormat) {
    int bRet = CTool::SetOutputFormat(pFormat);
    if (bRet) {
        if (*(const unsigned short*)((const char*)pFormat + 4) == 0x10) {
            this->m_ulSampleToByteFact = *(const unsigned short*)((const char*)pFormat + 6) << 1;
        } else {
            bRet = 0;
        }
    }
    return bRet;
}
