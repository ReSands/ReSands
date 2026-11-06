#include "dare.h"

CVirtualPlayer::CVirtualPlayer() {
    m_stVirtualVoiceList.pList.hFirstElementDyn = 0;
    m_stVirtualVoiceList.pList.hLastElementDyn = 0;
    m_stVirtualVoiceList.pList.lNumberOfElementsDyn = 0;
    m_bInitialized = 0;
    m_lInstance = 0;
    m_ulMaxAudibleVoices = 0;
    m_pstVirtFunc = 0;
    m_pHwModuleInfo = 0;
    m_ulNumberOfHwModule = 0;
}
