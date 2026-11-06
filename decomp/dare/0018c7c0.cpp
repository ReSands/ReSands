#include "dare.h"

int CVirtualPlayer::Init(_SND_tdstVirtFunc &stVirtFunc, unsigned int ulMaxAudibleVoices)
{
    _tdstHwModuleInfo stHwModuleInfo;
    stHwModuleInfo.ulMaxAudibleVoices = ulMaxAudibleVoices;
    return Init(stVirtFunc, ulMaxAudibleVoices, &stHwModuleInfo, 1);
}
