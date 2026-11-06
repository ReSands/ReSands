#include "dare.h"

extern SndManager* pManager;
extern int SND_fn_lCreateMicroSxdMic();

int SND_fn_lCreateMicroSxd(_MicroParam* pMicroParam) {
    int lRetMic;
    int lRet = -1;

    if (pManager != 0) {
        lRetMic = SND_fn_lCreateMicroSxdMic();
        EnterCriticalSectionTool_();
        MouchardThreadTool(__FILE__, 0x18e);
        lRet = pManager->CreateMicro(pMicroParam, lRetMic);
        QuitCriticalSectionTool();
    }
    
    return lRet;
}
