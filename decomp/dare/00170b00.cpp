#include "dare.h"

TVirtualSample::TVirtualSample() {
    m_bImminentEOS = 0;
    m_rPosSec = 0;
    m_lSampleLength = 0;
    m_rSampleLengthSec = 0;
    m_rInternalPitch = 0x10000;
    m_rPitch = 0x10000;
    m_rVolumeLeft_Linear = 0x10000;
    m_rVolumeRight_Linear = 0x10000;
    m_bFading = 0;
    m_rStreamDeviceLatency = 0x8000;
}
