#include "dare.h"

void CASyncBankLoader::Synchro(void) {
    if (m_bIsIdle) {
        return;
    }
    switch (m_eLoadingState) {
        case E_TEST_BANK_LOADED:
            TestBankLoaded();
            goto end_switch;
        case E_OPEN_BANK:
            OpenBank();
            break;
        case E_READ_BANK_HEADER:
            ReadBankHeader();
            break;
        case E_TEST_READ_BANK_HEADER:
            TestReadBankHeader();
            break;
        case E_TEST_BANK_HEADER:
            TestBankHeader();
            break;
        case E_READ_BANK_EVENTS:
            ReadBankEvents();
            break;
        case E_TEST_READ_BANK_EVENTS:
            TestReadBankEvents();
            break;
        case E_READ_BANK_RESOURCES:
            ReadBankResources();
            break;
        case E_TEST_READ_BANK_RESOURCES:
            TestReadBankResources();
            break;
        case E_READ_BANK_EXTRADATA:
            ReadBankExtraData();
            break;
        case E_TEST_READ_BANK_EXTRADATA:
            TestReadBankExtraData();
            break;
        case E_READ_BANK_WAVEFORMS:
            ReadBankWaveForms();
            break;
        case E_TEST_READ_BANK_WAVEFORMS:
            TestReadBankWaveForms();
            break;
        case E_TRANSFER_BANK_WAVEFORMS_PREPARE_TRANSFER:
            TransferBankWaveForms_PrepareTransfer();
            break;
        case E_TRANSFER_BANK_WAVEFORMS_START_READ_AND_TRANSFER:
            TransferBankWaveForms_StartReadAndTransfer();
            break;
        case E_TRANSFER_BANK_WAVEFORMS_TEST_READ_AND_TRANSFER:
            TransferBankWaveForms_TestReadAndTransfer();
            break;
        case E_INIT_BANK_RESOURCES:
            InitBankResources();
            break;
        case E_INIT_BANK_EVENTS:
            InitBankEvents();
            break;
        case E_FREE_BANK_WAVEFORMS:
            FreeBankWaveForms();
            break;
        case E_LOAD_BANK_ABORTING:
            LoadBankAborting();
            break;
        case E_LOAD_BANK_ABORTED:
        case E_LOAD_BANK_DONE:
            break;
    }
    end_switch:
    if (m_eLoadingState == E_LOAD_BANK_DONE) {
        LoadBankDone();
    }
}
