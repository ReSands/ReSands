#include "dare.h"

extern void *SND_fn_pvMallocSnd(unsigned int size, unsigned int flags);
extern SNDLST2_tdstAnchorToptdstVolumeLineDyn g_VolumeLines;
extern int gs_ulNbVolumeLines;

int SND_fn_lAddSoundVolumeLine(void) {
    SND_fn_vEnterCriticalSectionThreadSnd_();
    SND_fn_vMouchardThreadsnd(__FILE__, 0x1966);

    if (gs_ulNbVolumeLines == 0) {
        g_VolumeLines.hFirstElementDyn = 0;
        g_VolumeLines.hLastElementDyn = 0;
        g_VolumeLines.lNumberOfElementsDyn = 0;
    }

    tdstVolumeLine* pLine = (tdstVolumeLine*)SND_fn_pvMallocSnd(0x10, 0);
    SND_fn_vMemsetSnd(pLine, 0, 0x10);

    pLine->hNextBrotherDyn = 0;
    pLine->hPrevBrotherDyn = 0;
    pLine->hFatherDyn = 0;

    if (pLine->hNextBrotherDyn) {
        pLine->hNextBrotherDyn->hPrevBrotherDyn = pLine->hPrevBrotherDyn;
    }
    if (pLine->hPrevBrotherDyn) {
        pLine->hPrevBrotherDyn->hNextBrotherDyn = pLine->hNextBrotherDyn;
    }
    if (pLine->hFatherDyn) {
        pLine->hFatherDyn->lNumberOfElementsDyn--;
        if (pLine->hFatherDyn->hFirstElementDyn == pLine) {
            pLine->hFatherDyn->hFirstElementDyn = pLine->hNextBrotherDyn;
        }
        if (pLine->hFatherDyn->hLastElementDyn == pLine) {
            pLine->hFatherDyn->hLastElementDyn = pLine->hPrevBrotherDyn;
        }
    }

    pLine->hNextBrotherDyn = 0;
    pLine->hPrevBrotherDyn = 0;
    pLine->hFatherDyn = 0;
    pLine->hNextBrotherDyn = 0;
    pLine->hPrevBrotherDyn = g_VolumeLines.hLastElementDyn;
    pLine->hFatherDyn = &g_VolumeLines;

    if (!g_VolumeLines.hFirstElementDyn) {
        g_VolumeLines.hFirstElementDyn = pLine;
    } else {
        g_VolumeLines.hLastElementDyn->hNextBrotherDyn = pLine;
    }
    g_VolumeLines.hLastElementDyn = pLine;
    g_VolumeLines.lNumberOfElementsDyn++;
    pLine->rVol_dB = 0;
    gs_ulNbVolumeLines++;

    SND_fn_vQuitCriticalSectionThreadSnd();
    return (int)pLine;
}
