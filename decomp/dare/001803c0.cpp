#include "dare.h"

extern int g_iNbMicrosSxd;

void SND_fn_vSndParToPrivSndPar(_SoundParam *pPublic3DSoundParam, _PrivateSoundParam *pPrivate3DSoundParam, _tdePositionFormat ePositionFormat) {
    switch (ePositionFormat) {
        case POS_WITH_PAN:
            pPrivate3DSoundParam->ePositionFormat = (_tdePositionFormat)0;
            break;
        case POS_WITH_PAN_AND_SPACE:
            pPrivate3DSoundParam->ePositionFormat = (_tdePositionFormat)1;
            break;
        case POS_WITH_ANGLE:
            pPrivate3DSoundParam->ePositionFormat = (_tdePositionFormat)2;
            break;
    }

    if (pPublic3DSoundParam->iFlags & 1) {
        Snd_ParConv_ConvertWithNoPosition(pPublic3DSoundParam, pPrivate3DSoundParam);
    } else {
        if (g_iNbMicrosSxd == 0) {
            Snd_ParConv_ConvertWithNoMicro(pPublic3DSoundParam, pPrivate3DSoundParam);
        } else {
            Snd_ParConv_ConvertWithOneMicro(pPublic3DSoundParam, pPrivate3DSoundParam);
        }
    }
}
