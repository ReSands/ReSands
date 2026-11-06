#include "dare.h"
extern SndManager *pManager;
_SND_tdeAudioMode SND_bGetAudioModeSxd(void)
{
    _SND_tdeAudioMode eMode;
    if (pManager) {
        EnterCriticalSectionTool_();
        MouchardThreadTool(__FILE__, 0x68);
        eMode = SndManager::m_tdeAudioModeDARE;
        QuitCriticalSectionTool();
        return eMode;
    }
    return SND_E_STEREO;
}
