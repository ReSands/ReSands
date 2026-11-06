#include "dare.h"

extern int gs_bStackInitialised;
extern void **g_pStackCursorPos;
extern void **g_pStackHead;

int SND_fn_iGetStackSize(void)
{
    if (!gs_bStackInitialised)
        return 0;
    return (int)g_pStackCursorPos - (int)g_pStackHead;
}
