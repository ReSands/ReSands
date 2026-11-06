#include "dare.h"

extern SndManager* pManager;

void SND_fn_vPostDesInitEngineConf(void) {
    EnterCriticalSectionTool_();
    MouchardThreadTool(__FILE__, 0x4e6);
    if (pManager != 0) {
        pManager->PostDesInitEngineConf();
    }
    QuitCriticalSectionTool();
}
