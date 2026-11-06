#include "dare.h"

void CAsyncTransfer::FreeIOPBuffer(void)
{
    if (m_pIOPMemory != 0) {
        SND_fn_vFastFreeIOP((void *)m_pIOPMemory);
        m_pIOPMemory = 0;
    }
    m_eState = (_tdeAsyncTransfer)7;
}
