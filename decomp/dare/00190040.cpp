#include "dare.h"

void CASyncBankLoader::TransferBankWaveForms_TestRead(void) {
    if (m_stAsyncRead.bDone == 0) {
        TestRead();
        if (m_eLoadingState != E_LOAD_BANK_ABORTING) {
            if (m_stAsyncRead.bDone != 0) {
                m_stBlockTransfer.ulSynchedSize += m_stBlockTransfer.stDoubleBuffer[m_stBlockTransfer.lReadBufferIdx].ulSize;
                if (m_stBlockTransfer.ulSynchedSize >= m_stBlockTransfer.ulTotalSize) {
                    m_stBlockTransfer.lReadBufferIdx = -2;
                } else {
                    m_stBlockTransfer.lReadBufferIdx = !m_stBlockTransfer.lReadBufferIdx;
                }
                if (m_stBlockTransfer.lTransferBufferIdx == -2) {
                    m_stBlockTransfer.lTransferBufferIdx = 0;
                }
            }
        }
    }
}
