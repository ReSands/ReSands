#include "dare.h"

extern int gs_bInitConfSet;
extern _tdstMTTChannelElement *gs_pstMTTChannelArray;
extern _tdstReverbPresetList gs_stReverbPresetList;
extern struct { void *hFirstElementOpt; int lNumberOfElementsOpt; } gs_stClusterTypeList;  // type not in POP1's DWARF
extern void SND_fn_vFreeSnd(void *p);

void SND_fn_vDesInitEngineConf(void) {
    if (gs_pstMTTChannelArray) {
        SND_fn_vFreeSnd(gs_pstMTTChannelArray);
    }
    if (gs_stReverbPresetList.hFirstElementOpt) {
        SND_fn_vFreeSnd(gs_stReverbPresetList.hFirstElementOpt);
    }
    if (gs_stClusterTypeList.hFirstElementOpt) {
        SND_fn_vFreeSnd(gs_stClusterTypeList.hFirstElementOpt);
    }
    gs_bInitConfSet = 0;
}
