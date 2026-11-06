#include "dare.h"
extern SndManager *pManager;
void SND_fn_vSynchroSxd(void)
{
    EnterCriticalSectionTool_();
    MouchardThreadTool(__FILE__, 0x1ee);
    if (pManager)
        pManager->Synchro();
    QuitCriticalSectionTool();
    SND_fn_vSynchroTheme();
}
