#include "dare.h"
int SND_fn_rGetLengthSxd(_tduRefRes uResource)
{
    int rLength;
    EnterCriticalSectionTool_();
    MouchardThreadTool(__FILE__, 0x1c5);
    rLength = SndManager::GetLengthSample(uResource);
    QuitCriticalSectionTool();
    return rLength;
}
