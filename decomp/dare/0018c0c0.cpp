#include "dare.h"

extern void *SND_fn_pvMallocSnd(unsigned int size, int);
extern void *SND_fn_pvReAllocSnd(void *p, unsigned int size);

void SndManager::UpdateTrackVolumes(int rNewVolume, int lTrackID, int lMTTChanID) {
    _tdstMTTrackVolume* pstMTTrackVolume = SND_fn_pstGetMTTrackVolume(lMTTChanID);
    if (pstMTTrackVolume != 0 && lTrackID >= 0) {
        int oldCount = pstMTTrackVolume->ulNumberOfTrack;
        if (lTrackID >= oldCount) {
            pstMTTrackVolume->ulNumberOfTrack = lTrackID + 1;
            if (pstMTTrackVolume->prTrackVolumeArray == 0) {
                pstMTTrackVolume->prTrackVolumeArray = (int*)SND_fn_pvMallocSnd(pstMTTrackVolume->ulNumberOfTrack << 2, 0);
            } else {
                pstMTTrackVolume->prTrackVolumeArray = (int*)SND_fn_pvReAllocSnd(pstMTTrackVolume->prTrackVolumeArray, pstMTTrackVolume->ulNumberOfTrack << 2);
            }
            int i = oldCount;
            while (i < pstMTTrackVolume->ulNumberOfTrack - 1) {
                pstMTTrackVolume->prTrackVolumeArray[i] = 0;
                i++;
            }
        }
        pstMTTrackVolume->prTrackVolumeArray[lTrackID] = rNewVolume;
    }
}
