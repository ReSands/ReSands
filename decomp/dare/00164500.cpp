#include "dare.h"

void LoopingPath_ImminentEndOfStreamingCallback(int lParam) {
    LoopingPath* pLoopingPath = (LoopingPath*)lParam;
    if (pLoopingPath && pLoopingPath->m_pOneShotPath && !pLoopingPath->m_bTransition) {
        pLoopingPath->m_pOneShotPath->PreSetResource(&pLoopingPath->m_stLoopSourceFormat, 0);
        pLoopingPath->m_bTransition = 1;
    }
}
