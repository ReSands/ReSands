#include "dare.h"

int BranchPath::RemoveChild(int lChildID) {
    if (this->m_pTDecoder) {
        return this->m_pTDecoder->RemoveChild(lChildID);
    }
    return 0;
}
