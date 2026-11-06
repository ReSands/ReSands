#include "dare.h"

void CASyncBankLoader::LoadBankAborting_AbortLastTransfer(void) {
    if (m_stBlockTransfer.bDone == 0) {
        _SND_tdeSndAsynchTransfer eTransferState = SND_fn_eSynchAsyncSubBlockTransfer(m_stBlockTransfer.hAsyncTransfer);
        if (eTransferState != SND_E_ASYNC_TRANSFER_RUNNING) {
            m_stBlockTransfer.bDone = 1;
        }
    }
    if (m_stBlockTransfer.bDone != 0) {
        if (m_pBank != 0) {
            m_eAbortingState = (_tdeAbortLoadState)3;
        } else {
            m_eAbortingState = (_tdeAbortLoadState)6;
        }
    }
}
