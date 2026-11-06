#include "dare.h"

void CASyncBankLoader::TransferBankWaveForms_StartReadAndTransfer(void) {
    if (((m_stBlockTransfer.lReadBufferIdx == -2) ||
         (TransferBankWaveForms_StartRead(), m_eLoadingState != E_LOAD_BANK_ABORTING)) &&
        ((m_stBlockTransfer.lTransferBufferIdx == -2) ||
         (TransferBankWaveForms_StartTransfer(), m_eLoadingState != E_LOAD_BANK_ABORTING))) {
        m_eLoadingState = (_tdeLoadSoundBankState)0xf;
        TransferBankWaveForms_TestReadAndTransfer();
    }
}
