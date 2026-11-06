#include "dare.h"

_SND_tdeSndAsynchTransfer CAsyncTransfer::SynchroTransfer(void)
{
    switch (m_eState) {
    case E_ASYNC_TR_ALLOCATE_IOP_BUFFER:
        AllocateIOPBuffer();
        break;
    case E_ASYNC_TR_START_TRANSFER_EE_IOP:
        StartTransferEE_IOP();
        break;
    case E_ASYNC_TR_TEST_TRANSFER_EE_IOP:
        TestTransferEE_IOP();
        break;
    case E_ASYNC_TR_START_TRANSFER_IOP_SPU:
        StartTransferIOP_SPU();
        break;
    case E_ASYNC_TR_TEST_TRANSFER_IOP_SPU:
        TestTransferIOP_SPU();
        break;
    case E_ASYNC_TR_FREE_IOP_BUFFER:
        FreeIOPBuffer();
        break;
    case E_ASYNC_TRANSFER_ABORTED:
        AsyncTransferAborted();
        return SND_E_ASYNC_TRANSFER_ABORTED;
    case E_ASYNC_TRANSFER_DONE:
        AsyncTransferDone();
        return SND_E_ASYNC_TRANSFER_DONE;
    }

    if (m_eState == E_ASYNC_TRANSFER_ABORTED) {
        AsyncTransferAborted();
        return SND_E_ASYNC_TRANSFER_ABORTED;
    } else if (m_eState == E_ASYNC_TRANSFER_DONE) {
        AsyncTransferDone();
        return SND_E_ASYNC_TRANSFER_DONE;
    }
    return SND_E_ASYNC_TRANSFER_RUNNING;
}
