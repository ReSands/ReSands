#include "dare.h"

void CASyncBankLoader::LoadBankDone(void) {
    if (m_stBlockTransfer.stDoubleBuffer[0].pvReadBuffer != 0) {
        SND_fn_vFreeSnd(m_stBlockTransfer.stDoubleBuffer[0].pvReadBuffer);
        m_stBlockTransfer.stDoubleBuffer[0].pvReadBuffer = 0;
    }
    if (m_stBlockTransfer.stDoubleBuffer[1].pvReadBuffer != 0) {
        SND_fn_vFreeSnd(m_stBlockTransfer.stDoubleBuffer[1].pvReadBuffer);
        m_stBlockTransfer.stDoubleBuffer[1].pvReadBuffer = 0;
    }
    if (m_pResDisk != 0) {
        SND_fn_vFreeSnd(m_pResDisk);
        m_pResDisk = 0;
    }
    if (m_stAsyncRead.hBankFile != -1) {
        SND_fn_vDestroyStreamAsyncSnd((void *)m_stAsyncRead.hBankFile);
        m_stAsyncRead.hBankFile = -1;
    }
    m_pBank = 0;
}
