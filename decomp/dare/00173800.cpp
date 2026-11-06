#include "dare.h"

extern _tdstMTTChannelIDList* gs_pstMTTChannelArray;
extern int gs_lNbMTTChannel;

_tdstMTTrackVolume* SND_fn_pstGetMTTrackVolume(int lMTTChannelID) {
    if (gs_pstMTTChannelArray == 0) {
        return 0;
    }
    for (int i = 0; i < gs_lNbMTTChannel; i++) {
        if (gs_pstMTTChannelArray[i].lId == lMTTChannelID) {
            return &gs_pstMTTChannelArray[i].stMTTrackVolume;
        }
    }
    return 0;
}
