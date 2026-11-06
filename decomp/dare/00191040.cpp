#include "dare.h"

void CASyncBankUnLoader::UnLoadBankDone(void)
{
    if (m_pBank->pBankHeader != 0) {
        SND_fn_vFreeSnd(m_pBank->pBankHeader);
        m_pBank->pBankHeader = 0;
    }
    m_pBank = 0;
}
