#include "dare.h"
extern int gs_bStackInitialised;
extern void *g_pStackHead;
void *SND_fn_pvGetStackHead(void)
{
    if (!gs_bStackInitialised)
        return 0;
    return g_pStackHead;
}
