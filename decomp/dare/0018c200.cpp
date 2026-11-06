#include "dare.h"

_SND_tdeInit SndManager::PostInitEngineConf(void) {
    unsigned int ulNbHwModuleInfo;
    _tdstHwModuleInfo* pstHwModuleInfo = 0;

    m_pRenderer->GetHwModuleInfo(&pstHwModuleInfo, &ulNbHwModuleInfo);

    unsigned int ulMaxAudibleVoices = 0;
    if (m_pRenderer) {
        ulMaxAudibleVoices = m_pRenderer->GetMaxAudibleVoices();
        if (ulMaxAudibleVoices == 0) {
            SND_fn_vDisplayErrorEx(8, "Number of voices is set to \"0\" in your .ini file!", 3);
        }
    }

    m_HWVirtModule.SetHwModuleInfo(ulMaxAudibleVoices, pstHwModuleInfo, ulNbHwModuleInfo);

    if (SND_fn_bInitSourceModule()) {
        if (m_pRenderer) {
            if (m_pRenderer->PostInitEngineConf() == SND_E_INIT_OK) {
                return SND_E_INIT_OK;
            }
        }
    }
    return SND_E_INIT_FAILED;
}
