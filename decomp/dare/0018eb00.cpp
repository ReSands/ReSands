#include "dare.h"

extern void SND_fn_vFreeSnd(void* pv);

CASyncBankLoader::~CASyncBankLoader() {
    if (m_stBlockTransfer.stDoubleBuffer[0].pvReadBuffer != 0) {
        SND_fn_vFreeSnd(m_stBlockTransfer.stDoubleBuffer[0].pvReadBuffer);
        m_stBlockTransfer.stDoubleBuffer[0].pvReadBuffer = 0;
    }
    if (m_stBlockTransfer.stDoubleBuffer[1].pvReadBuffer != 0) {
        SND_fn_vFreeSnd(m_stBlockTransfer.stDoubleBuffer[1].pvReadBuffer);
        m_stBlockTransfer.stDoubleBuffer[1].pvReadBuffer = 0;
    }
}
