#include "dare.h"
extern "C" char *strcpy(char *, const char *);
extern char g_czSoundSoftDirectory[256];
void SND_fn_vSetSoftDirectory(char *cszDir)
{
    SND_fn_vEnterCriticalSectionThreadSnd_();
    SND_fn_vMouchardThreadsnd(__FILE__, 0x1ee);
    strcpy(g_czSoundSoftDirectory, cszDir);
    SND_fn_vQuitCriticalSectionThreadSnd();
}
