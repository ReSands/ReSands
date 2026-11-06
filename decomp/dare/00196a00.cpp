#include "dare.h"

extern void SND_fn_vDisplayError(int code, const char* msg);

_SND_tdeSndAsynchTransfer SND_fn_eSynchAsyncSubBlockTransferHW(unsigned int hAsyncTransfer) {
    CAsyncTransfer* pAsyncTransfer = (CAsyncTransfer*)hAsyncTransfer;
    _SND_tdeSndAsynchTransfer eRet;

    if (pAsyncTransfer == 0 || !pAsyncTransfer->IsValid()) {
        SND_fn_vDisplayError(8, "Invalid sound transfer handle.");
        return (_SND_tdeSndAsynchTransfer)0;
    }
    eRet = pAsyncTransfer->SynchroTransfer();
    if (eRet == 1 || eRet == 0 || eRet == 3) {
        delete pAsyncTransfer;
    }
    return eRet;
}
