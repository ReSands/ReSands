#include "dare.h"

extern void (*pfnAudibleTransitionCallback)(int);

void ThemePath_AudibleTransitionCallback(int lParam) {
    if (lParam && pfnAudibleTransitionCallback) {
        SndManager::AddCallBack(pfnAudibleTransitionCallback, ((ThemePath*)lParam)->m_lTransitionParamCB);
    }
}
