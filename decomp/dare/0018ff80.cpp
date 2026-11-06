#include "dare.h"

void CASyncBankLoader::TransferBankWaveForms_TestReadAndTransfer(void) {
    if ((m_stBlockTransfer.lReadBufferIdx == -2 ||
         (TransferBankWaveForms_TestRead(), m_eLoadingState != E_LOAD_BANK_ABORTING)) &&
        (m_stBlockTransfer.lTransferBufferIdx == -2 ||
         (TransferBankWaveForms_TestTransfer(), m_eLoadingState != E_LOAD_BANK_ABORTING))) {
        if (m_stAsyncRead.bDone != 0 && m_stBlockTransfer.bDone != 0) {
            if (m_stBlockTransfer.lReadBufferIdx == -2 && m_stBlockTransfer.lTransferBufferIdx == -2) {
                m_eLoadingState = E_TRANSFER_BANK_WAVEFORMS_PREPARE_TRANSFER;
                TransferBankWaveForms_PrepareTransfer();
            } else {
                m_eLoadingState = E_TRANSFER_BANK_WAVEFORMS_START_READ_AND_TRANSFER;
            }
        }
    }
}
