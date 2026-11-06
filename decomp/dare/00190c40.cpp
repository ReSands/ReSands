#include "dare.h"

void CASyncBankUnLoader::Start(unsigned int hBank)
{
    m_bIsIdle = 1;
    m_pBank = 0;
    m_lWaveFormFreed = 0;
    m_lUnLoadedResources = 0;
    m_eState = (_tdeUnLoadSoundBankState)0;
    m_bIsIdle = 0;
    m_pBank = (_tdstBankElement*)hBank;
}
