#include "dare.h"

extern ASSOCIATION_DESCRIPTION_* gs_AssociationList;
extern void SND_fn_vQuitCriticalSectionThreadSnd(void);

void SND_fn_vKillSoundChannel(int _lChannelId, int _lObjectId, int _lTypeId) {
    SND_fn_vEnterCriticalSectionThreadSnd_();
    SND_fn_vMouchardThreadsnd(__FILE__, 0x176d);

    if (_lChannelId != -1) {
        ASSOCIATION_DESCRIPTION_* pAsso = gs_AssociationList;
        while (pAsso != 0) {
            if (pAsso->lChannelId == _lChannelId &&
                pAsso->lObjectId == _lObjectId &&
                pAsso->lTypeId == _lTypeId) {
                pAsso->bDynamic = 0;
                break;
            }
            pAsso = pAsso->hNextBrotherDyn;
        }
    }

    SND_fn_vQuitCriticalSectionThreadSnd();
}
