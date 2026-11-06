#include "dare.h"

void CASyncBankLoader::LoadBankAborting(void) {
    if (m_eAbortingState == E_NOT_ABORTING) {
        m_eAbortingState = E_CLOSE_BANK;
    }
    switch (m_eAbortingState) {
        case E_CLOSE_BANK:
            LoadBankAborting_CloseBank();
            break;
        case E_ABORT_LAST_TRANSFER:
            LoadBankAborting_AbortLastTransfer();
            break;
        case E_UNLOAD_EVENTS:
            LoadBankAborting_UnloadEvents();
            break;
        case E_UNLOAD_RESOURCES:
            LoadBankAborting_UnloadResources();
            break;
        case E_FREE_SUB_BLOCKS_DATA:
            LoadBankAborting_FreeSubBlocksData();
            break;
        case E_FREE_MEMORY_AND_RESET_FIELDS:
            LoadBankAborting_FreeMemoryAndResetFields();
            break;
        default:
            m_eLoadingState = (_tdeLoadSoundBankState)0x14;
            m_pBank = 0;
            break;
    }
}
