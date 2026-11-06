#include "dare.h"

int BranchPath::CreateBranch_Failure(void) {
    if (m_pTDecoder) {
        Destroy_TDecoder(&m_pTDecoder);
    }
    if (m_pTPitch) {
        Destroy_TPitchShifter(&m_pTPitch);
    }
    if (m_pTVolPan) {
        Destroy_TVolPan(&m_pTVolPan);
    }
    if (m_pTFader) {
        Destroy_TFader(&m_pTFader);
    }
    return 0;
}
