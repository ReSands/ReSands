#include "dare.h"
extern void SND_fn_vFreeSnd(void *);
void SndManager::DesInitMTTrackVolumeArray(_tdstMTTrackVolume *pstMTTrackVolume)
{
    if (pstMTTrackVolume) {
        if (pstMTTrackVolume->prTrackVolumeArray) {
            SND_fn_vFreeSnd(pstMTTrackVolume->prTrackVolumeArray);
            pstMTTrackVolume->prTrackVolumeArray = 0;
        }
        pstMTTrackVolume->ulNumberOfTrack = 0;
    }
}
