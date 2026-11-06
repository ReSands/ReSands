#include "dare.h"

void CASyncBankLoader::Start(unsigned int hBank) {
    InitFields(0);
    m_bIsIdle = 0;
    m_pBank = (_tdstBankElement*)hBank;
}
