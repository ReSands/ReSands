#include "dare.h"

void CAsyncTransfer::AllocateIOPBuffer(void)
{
    if (m_bIOPMemory != 0) {
        m_pIOPMemory = m_pDestMemory;
    } else {
        m_pIOPMemory = (int)SND_fn_pvFastMallocIOP(m_ulAllocIOPSize);
        if (m_pIOPMemory == 0) {
            SND_fn_vDisplayErrorEx(10, "Problem while Allocating memory on IOP.", 3);
            m_eState = (_tdeAsyncTransfer)6;
            return;
        }
    }
    m_eState = (_tdeAsyncTransfer)1;
    StartTransferEE_IOP();
}
