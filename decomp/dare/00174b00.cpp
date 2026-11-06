#include "dare.h"

extern int gs_bInitProjectDone;
extern _SND_tdeDataLoadMode gs_eLoadProjectType;

extern "C" {
void SND_fn_vDisplayError(unsigned char error, char *text);
_SND_tdstBlockEvent* SND_fn_hGetSoundEventHandleFromEditorId_Map(unsigned int ulEdId);
_SND_tdstBlockEvent* SND_fn_hGetSoundEventHandleFromEditorId_Bank(unsigned int ulEdId);

_SND_tdstBlockEvent* SND_fn_hGetSoundEventHandleFromEditorId(unsigned int ulEdId) {
    if (!gs_bInitProjectDone) {
        SND_fn_vDisplayError(0, "ERR_INIT");
        return 0;
    }
    switch (gs_eLoadProjectType) {
        case SND_E_DATALOADMODE_MAP:
            return SND_fn_hGetSoundEventHandleFromEditorId_Map(ulEdId);
        case SND_E_DATALOADMODE_BANK:
            return SND_fn_hGetSoundEventHandleFromEditorId_Bank(ulEdId);
        default:
            SND_fn_vDisplayError(0, "ERR_LOAD");
            return 0;
    }
}
}
