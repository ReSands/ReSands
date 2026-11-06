#include "dare.h"
extern SndManager *pManager;
extern void Destroy_SndManager(SndManager **ppManager);
void SND_fn_vDesInitSxd(void)
{
    EnterCriticalSectionTool_();
    MouchardThreadTool(__FILE__, 0x4e);
    Destroy_SndManager(&pManager);
    QuitCriticalSectionTool();
}
