#include "dare.h"

extern _tdstSndToc g_stSndToc[256];
extern void SND_fn_vFreeSnd(void *p);
extern int gs_bInitSndTocSet;
extern int gs_bLoadSndTocSet;

void SND_fn_vDesInitSndToc(void) {
    int i;
    for (i = 0; i < 256; i++) {
        g_stSndToc[i].lSize = 0;
        if (g_stSndToc[i].pstSndTocElt != 0) {
            SND_fn_vFreeSnd(g_stSndToc[i].pstSndTocElt);
            g_stSndToc[i].pstSndTocElt = 0;
        }
    }
    gs_bInitSndTocSet = 0;
    gs_bLoadSndTocSet = 0;
}
