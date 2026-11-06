#include "dare.h"

struct tdstMapMem;

extern int gs_bInitProjectDone_Map;
extern int gs_bMapsFileDescLoadedLoc;
extern int gs_bMapsFileDescLoadedNLoc;
extern char gs_stCurrentMap[120];
extern char gs_stMapFix[120];
extern int gs_stMapsFileDescLoc[3];
extern int gs_stMapsFileDescNLoc[3];

extern void fn_vResetMapMem(struct tdstMapMem*);

int SND_fn_bInitDataLoadSnd_Map(void)
{
    fn_vResetMapMem((struct tdstMapMem*)&gs_stMapFix);
    fn_vResetMapMem((struct tdstMapMem*)&gs_stCurrentMap);
    
    gs_bMapsFileDescLoadedNLoc = 0;
    gs_stMapsFileDescNLoc[0] = -2;
    gs_stMapsFileDescLoc[0] = -2;
    gs_stMapsFileDescNLoc[1] = 0;
    gs_stMapsFileDescNLoc[2] = 0;
    gs_bMapsFileDescLoadedLoc = 0;
    gs_stMapsFileDescLoc[1] = 0;
    gs_stMapsFileDescLoc[2] = 0;
    
    return gs_bInitProjectDone_Map = 1;
}
