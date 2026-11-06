#include "dare.h"

TPitchShifter::TPitchShifter() : CTool() {
    m_ulPitchFactor = 0x1000;
    m_ulRelativePitch = 0x1000;
    m_ratio = 0x10000;
    m_ulShift = 0;
    m_ulOffset = 0;
    m_PitchChgMode = C_CONSTANT_PITCH;
    m_InterpolMode = C_NOINTERPOLATION;
    m_pChild = 0;
    m_ulinDataSize = 0;
    m_bLastSamplesNoGood = 1;
    m_ulSampleToByteFact = 0;
}
