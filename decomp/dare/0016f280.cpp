#include "dare.h"

int TPCM::SetOutputFormat(const _SND_tdstToolFormat *pFormat) {
    int bRet = CTool::SetOutputFormat(pFormat);
    if (bRet) {
        if (*(const unsigned short*)&pFormat->uFormat == 0x10) {
            this->m_ulShift = *(const unsigned short*)((const char*)&pFormat->uFormat + 2);
        } else {
            bRet = 0;
        }
    }
    return bRet;
}
