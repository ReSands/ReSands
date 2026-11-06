#include "dare.h"

void Snd_ParConv_ConvertWithNoPosition(_SoundParam *pPublic3DSoundParam, _PrivateSoundParam *pPrivate3DSoundParam) {
    pPrivate3DSoundParam->rVol_dB = pPublic3DSoundParam->rVol_dB;
    pPrivate3DSoundParam->rPitch = pPublic3DSoundParam->Freq;
    
    switch (pPrivate3DSoundParam->ePositionFormat) {
        case POS_WITH_PAN_AND_SPACE:
            pPrivate3DSoundParam->uPositionFormat.stPosWithPanAndSpace.ucPan = pPublic3DSoundParam->ucPan;
            pPrivate3DSoundParam->uPositionFormat.stPosWithPanAndSpace.ucSpace = 0;
            break;
        case POS_WITH_PAN:
            pPrivate3DSoundParam->uPositionFormat.stPosWithPan.ucPan = pPublic3DSoundParam->ucPan;
            break;
        case POS_WITH_ANGLE:
            pPrivate3DSoundParam->uPositionFormat.stPosWithAngle.lCurrentAngle = 0;
            break;
    }
}
