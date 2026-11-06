#include "dare.h"

TVolPan::TVolPan(void)
{
    m_pChild = 0;
    m_ulInputSampleToByte = 0;
    m_ulOutputSampleToByte = 0;
    m_rVolumeDynLeft_Linear = 0x10000;
    m_rVolumeDynRight_Linear = 0x10000;
    m_rEffVolLeft_Linear = 0x10000;
    m_rEffVolRight_Linear = 0x10000;
    m_lEffVolLeft = 0x4000;
    m_lEffVolRight = 0x4000;
    m_rParentFadeVol_Linear = 0x10000;
    m_ulNbInputChannels = 1;
}
