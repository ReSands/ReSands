#include "dare.h"

void ThemePath::GetFinalVolume(int &rEffVolLeft_Linear, int &rEffVolRight_Linear) const {
    if (m_pOneShotPath) {
        m_pOneShotPath->GetFinalVolume(rEffVolLeft_Linear, rEffVolRight_Linear);
    } else {
        rEffVolLeft_Linear = 0;
        rEffVolRight_Linear = 0;
    }
}
