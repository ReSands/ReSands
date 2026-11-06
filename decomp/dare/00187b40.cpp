#include "dare.h"

TPitchShifterGeneric::TPitchShifterGeneric() {
    m_u16LastSamplesLeft[0] = 0;
    m_u16LastSamplesLeft[1] = 0;
    m_u16LastSamplesRight[0] = 0;
    m_u16LastSamplesRight[1] = 0;
    InitBuffers();
}
