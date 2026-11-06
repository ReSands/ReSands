#include "dare.h"

extern int gs_bStackInitialised;
extern void **g_pStackCursorPos;

void SND_fn_vMarkEndStack(void)
{
    if (gs_bStackInitialised)
    {
        *g_pStackCursorPos = 0;
        g_pStackCursorPos = g_pStackCursorPos + 1;
    }
}
