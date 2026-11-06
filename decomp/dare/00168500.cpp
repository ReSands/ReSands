#include "dare.h"

extern void Destroy_TVirtualSample(TVirtualSample** pSample);

void OneShotSILENCEPath::DestroyPath(void) {
    if (m_pTVirtual) {
        Destroy_TVirtualSample(&m_pTVirtual);
    }
}
