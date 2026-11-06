#include "dare.h"

extern _tdstSndToc g_stSndToc[256];
extern int gs_bInitSndTocSet;
extern int gs_bLoadSndTocSet;

int SND_fn_bInitSndToc(void) {
    int i;
    if (!gs_bInitSndTocSet) {
        for (i = 0; i < 256; i++) {
            g_stSndToc[i].lSize = 0;
            g_stSndToc[i].pstSndTocElt = 0;
        }
        gs_bLoadSndTocSet = 0;
        gs_bInitSndTocSet = 1;
    }
    return 1;
}
