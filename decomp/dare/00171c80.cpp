#include "dare.h"

extern SndManager* pManager;

int SND_fn_lPlayTransitionExSxd(_tduRefRes uFirstRes, _tduRefRes uNextRes, _SoundParam* pPar, int lPrio, int param_callback) {
    if (pManager != 0) {
        EnterCriticalSectionTool_();
        MouchardThreadTool(__FILE__, 0x21b);
        int lRet = pManager->PlayTransition(uFirstRes, uNextRes, pPar, lPrio, param_callback);
        QuitCriticalSectionTool();
        return lRet;
    }
    return -1;
}
