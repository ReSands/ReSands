#include "dare.h"

extern _SND_tdstBlockEvent* fn_pGetEventPointerFromEdId_Map(unsigned int ulEdId, int a1, int a2);

_SND_tdstBlockEvent* SND_fn_hGetSoundEventHandleFromEditorId_Map(unsigned int ulEdId) {
    _SND_tdstBlockEvent* pstBlockEvent = fn_pGetEventPointerFromEdId_Map(ulEdId, 0, 0);
    if (pstBlockEvent != 0) return pstBlockEvent;
    
    pstBlockEvent = fn_pGetEventPointerFromEdId_Map(ulEdId, 1, 0);
    if (pstBlockEvent != 0) return pstBlockEvent;
    
    if (g_stProjectDesc.bLocalised != 0) {
        pstBlockEvent = fn_pGetEventPointerFromEdId_Map(ulEdId, 0, 1);
        if (pstBlockEvent != 0) return pstBlockEvent;
        
        pstBlockEvent = fn_pGetEventPointerFromEdId_Map(ulEdId, 1, 1);
        if (pstBlockEvent != 0) return pstBlockEvent;
    }
    
    return 0;
}
