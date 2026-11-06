#include "dare.h"

extern SndManager* pManager;

void SND_fn_vSetMicroParamSxd(int lMicroId, _MicroParam* pMicroParam) {
    if (pManager) {
        SND_fn_vSetMicroParamSxdMic(lMicroId, pMicroParam);
        EnterCriticalSectionTool_();
        MouchardThreadTool(__FILE__, 0x19b);
        pManager->SetMicroParam(lMicroId, pMicroParam);
        QuitCriticalSectionTool();
    }
}
