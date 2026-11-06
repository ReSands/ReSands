#include "dare.h"

extern _SND_tdstAllRetSoundMicro g_st_pfnRetSoundMicros;

void SND_fn_vSetRetSoundMicros(_SND_tdstAllRetSoundMicro* ptrfct) {
    SND_fn_vEnterCriticalSectionThreadSnd_();
    SND_fn_vMouchardThreadsnd(__FILE__, 0x194c);
    SND_fn_vMemCopySnd((void*)&g_st_pfnRetSoundMicros, (const void*)ptrfct, 0x10);
    SND_fn_vQuitCriticalSectionThreadSnd();
}
