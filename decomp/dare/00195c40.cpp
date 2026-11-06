#include "dare.h"

void CTheme::SetNbPlayedPart(unsigned int ulNbPlayedPart) {
    if (ulNbPlayedPart < m_PartList.m_nCount) {
        m_ulNbPartSent = ulNbPlayedPart;
        return;
    }
    m_ulNbPartSent = 0;
}
