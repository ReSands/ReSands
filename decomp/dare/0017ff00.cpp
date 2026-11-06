#include "dare.h"

extern CThemeManager* g_pThemeManager;

int SND_fn_rGetPosTheme(int canal) {
    int ret = 0xffff0000;
    if (g_pThemeManager) {
        ret = g_pThemeManager->GetPosTheme(canal);
    }
    return ret;
}
