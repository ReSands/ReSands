#include "dare.h"

void CASyncBankUnLoader::Synchro() {
    if (m_bIsIdle) return;
    switch (m_eState) {
        case 0: TestBankLoaded(); break;
        case 1: UnloadEvents(); break;
        case 2: UnloadResources(); break;
        case 3: FreeSubBlocksData(); break;
        case 4: break;
    }
    if (m_eState == 4) UnLoadBankDone();
}
