#include "dare.h"

_SND_tdeASyncBankLoaderState CASyncBankLoader::GetState(void) {
    if (m_bIsIdle) {
        return SND_E_ASYNC_BANK_LOADER_IDLE;
    }
    switch (m_eLoadingState) {
        case E_LOAD_BANK_ABORTED:
            m_bIsIdle = 1;
            return SND_E_ASYNC_BANK_LOADER_ABORTED;
        case E_LOAD_BANK_DONE:
            m_bIsIdle = 1;
            return SND_E_ASYNC_BANK_LOADER_DONE;
        default:
            return SND_E_ASYNC_BANK_LOADER_RUNNING;
    }
}
