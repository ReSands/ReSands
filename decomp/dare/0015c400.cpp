#include "dare.h"

extern _SND_tdstBlockEvent* SND_fn_hGetSoundEventHandleFromEditorId(unsigned int ulEdId);
extern void SND_fn_vDisplayErrorEx(unsigned int a0, const char* szMsg, unsigned int a2);
extern int sprintf(char *dst, const char *fmt, ...);

_SND_tdstBlockEvent* GetSwitchEventElement(_SND_tdstBlockEvent* _pstBlockEvent, int _hSwitchValue) {
    char szMsg[1024];
    int i;
    int bFound;
    _tdstSwitchEventElement* pstSwitchEventElement;
    _SND_tdstBlockEvent* pstEventToPlay;

    pstSwitchEventElement = _pstBlockEvent->uParams.stSwitch.hFirstElementOpt;
    bFound = 0;
    pstEventToPlay = 0;

    for (i = 0; _pstBlockEvent->uParams.stSwitch.lNumberOfElementsOpt > i; i++, pstSwitchEventElement++) {
        if (pstSwitchEventElement->hSwitchValue == _hSwitchValue) {
            bFound = 1;
            pstEventToPlay = SND_fn_hGetSoundEventHandleFromEditorId(pstSwitchEventElement->ulEventEdId);
            if (0 == pstEventToPlay) {
                sprintf(szMsg, "SwitchEvent 0x%x trying to play an unloaded sound event 0x%x.", _pstBlockEvent->Id, pstSwitchEventElement->ulEventEdId);
                SND_fn_vDisplayErrorEx(8, szMsg, 3);
            }
            goto done;
        }
    }
done:
    if (bFound == 0) {
        pstEventToPlay = _pstBlockEvent->uParams.stSwitch.uDefaultEvent.pstPtr;
    }
    return pstEventToPlay;
}
