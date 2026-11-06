#include "dare.h"

extern char g_bMicInit;
extern int g_iMicErrorShown;

extern "C" {
void Snd_ParConv_ConvertWithNoMicro(_SoundParam *pPublic3DSoundParam, _PrivateSoundParam *pPrivate3DSoundParam) {
    pPrivate3DSoundParam->rVol_dB = 0;
    pPrivate3DSoundParam->rPitch = 0x10000;

    if (g_bMicInit == 0) {
        g_iMicErrorShown = 0;
        g_bMicInit = 1;
    }

    if (g_iMicErrorShown == 0) {
        g_iMicErrorShown = 1;
        SND_fn_vDisplayError(8, "You should define a microphone to play a 3D Sound");
    }

    switch (pPrivate3DSoundParam->ePositionFormat) {
        case POS_WITH_PAN_AND_SPACE:
            pPrivate3DSoundParam->uPositionFormat.stPosWithPanAndSpace.ucPan = 0x40;
            pPrivate3DSoundParam->uPositionFormat.stPosWithPanAndSpace.ucSpace = 0;
            break;
        case POS_WITH_PAN:
            pPrivate3DSoundParam->uPositionFormat.stPosWithPan.ucPan = 0x40;
            break;
        case POS_WITH_ANGLE:
            pPrivate3DSoundParam->uPositionFormat.stPosWithAngle.lCurrentAngle = 0;
            break;
        default:
            break;
    }
}
}
