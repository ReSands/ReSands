#include "dare.h"
extern int gs_bStackInitialised;
extern void **g_pStackCursorPos;
extern void **g_pStackHead;
void SND_fn_vResetStack(void)
{
    if (gs_bStackInitialised)
    {
        g_pStackCursorPos = g_pStackHead;
        *g_pStackCursorPos = 0;
    }
}
