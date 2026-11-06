#include "dare.h"

int OneShotMULTITRACKPath::GetPos(void) {
    BranchPath** pBranchPaths = m_MTTPath.pBranchPath;
    if (pBranchPaths) {
        BranchPath* pBranch = *pBranchPaths;
        if (pBranch) {
            return pBranch->GetPos();
        }
    }
    return 0xFFFF0000;
}
