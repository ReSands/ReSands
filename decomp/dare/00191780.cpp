#include "dare.h"

void CAsyncTransfer::AsyncTransferAborted()
{
    if (m_pIOPMemory != 0) {
        SND_fn_vFastFreeIOP((void *)m_pIOPMemory);
        m_pIOPMemory = 0;
    }
}
