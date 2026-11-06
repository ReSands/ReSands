#include "dare.h"

extern int GetRendererLatency__10SndManagerFv();

BranchPath::BranchPath(void) {
    m_pTFader = 0;
    m_pTVolPan = 0;
    m_pTPitch = 0;
    m_pTDecoder = 0;
    m_lFaderChildId = -1;
    m_lPitchChildId = -1;
    m_lVolPanChildId = -1;
    m_lDolbyPL2Angle = 0;
    m_rRendererLatency = GetRendererLatency__10SndManagerFv();
}
