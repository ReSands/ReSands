#include "dare.h"

TAdder::TAdder()
{
    m_lNewChildID = 1;
    m_ulOutputShift = 0;
    m_ulSampleToByteFact = 0;
    m_pGetDataSampleProc = &TAdder::GetDataSample;
    m_pChildren.pList.hFirstElementDyn = 0;
    m_pChildren.pList.hLastElementDyn = 0;
    m_pChildren.pList.lNumberOfElementsDyn = 0;
    InitBuffers();
}
