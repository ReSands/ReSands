#include "dare.h"

int OneShotMULTITRACKPath::CreateMultiTrackPath(_tdstBlockResourceMem *pRes) {
    AllocateBuffers();
    if (!CreateSubPath(pRes)) return 0;
    if (!CreateBranchs()) return 0;
    if (!AddFader()) return 0;
    SetMaxRequestedDataSize();
    if (!ConnectAdder()) return 0;
    if (!AddFinalPitch()) return 0;
    if (!AddFinalVolume()) return 0;
    if (!AddFinalFader()) return 0;
    if (!AssociateTracks()) return 0;
    InitMultiTrackParameters();
    return 1;
}
