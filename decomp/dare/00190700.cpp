#include "dare.h"

void CASyncBankLoader::LoadBankAborting_CloseBank(void) {
    if (m_stAsyncRead.hBankFile != -1) {
        if (m_stAsyncRead.bDone == 0) {
            SND_fn_vAbortLastStreamAsyncSnd((void *)m_stAsyncRead.hBankFile);
        }
        SND_fn_vDestroyStreamAsyncSnd((void *)m_stAsyncRead.hBankFile);
        m_stAsyncRead.hBankFile = -1;
    }
    m_eAbortingState = (_tdeAbortLoadState)2;
}
