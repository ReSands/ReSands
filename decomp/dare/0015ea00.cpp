#include "dare.h"

extern void* g_d_pstTypeObjectSoundInfos;

void SND_fn_vSetRetSoundChannelType(int _lTypeId, _SND_tdstAllRetSoundChannel* _ptrFct) {
    SND_fn_vEnterCriticalSectionThreadSnd_();
    SND_fn_vMouchardThreadsnd(__FILE__, 0x1793);
    SND_fn_vMemCopySnd((void*)((char*)g_d_pstTypeObjectSoundInfos + _lTypeId * 0x70 + 0x24), _ptrFct, 0x10);
    SND_fn_vQuitCriticalSectionThreadSnd();
}
