#include "dare.h"

void OneShotSAMPLEPath::GetFinalVolume(int &rEffVolLeft_Linear, int &rEffVolRight_Linear) const {
    if (m_pBranchPath) {
        m_pBranchPath->GetFinalVolume(rEffVolLeft_Linear, rEffVolRight_Linear);
    } else {
        rEffVolLeft_Linear = 0x10000;
        rEffVolRight_Linear = 0x10000;
    }
}
