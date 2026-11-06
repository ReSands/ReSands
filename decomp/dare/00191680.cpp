#include "dare.h"

extern unsigned int _6a4fc0;
extern int _6a4f80;

void CAsyncTransfer::TestTransferIOP_SPU(void)
{
    _6a4fc0 = m_hSPUTransferHandle;
    SND_fn_iIOPCommandRPC(9, &_6a4fc0, 16, &_6a4f80, 64);
    if (0 != _6a4f80) {
        m_eState = (_tdeAsyncTransfer)5;
        FreeIOPBuffer();
    }
}
