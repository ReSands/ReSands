#include "dare.h"
int SND_fn_bIsResourceStreamedSnd(_tdstBlockResourceDisk *pstBRDisk)
{
    switch (pstBRDisk->eType)
    {
    case TYPE_SAMPLE:
        return SND_fn_bIsResourceStreamedSxd(pstBRDisk);
    case TYPE_MULTITRACK:
        return SND_fn_bIsResourceStreamedMultiTrack(pstBRDisk);
    default:
        return 0;
    }
}
