#include "dare.h"

extern void Destroy_OneShotPath__FPP11OneShotPath(OneShotPath**);
extern void Destroy_TTransition__FPP11TTransition(TTransition**);

void LoopingPath::DestroyPath(void) {
    if (m_pOneShotPath != 0) {
        Destroy_OneShotPath__FPP11OneShotPath(&m_pOneShotPath);
        m_pOneShotPath = 0;
    }
    if (m_pTTransition != 0) {
        Destroy_TTransition__FPP11TTransition(&m_pTTransition);
        m_pTTransition = 0;
    }
}
