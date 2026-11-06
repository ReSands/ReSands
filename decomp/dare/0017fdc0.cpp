#include "dare.h"

extern CThemeManager* g_pThemeManager;

int SND_fn_bSetParamTheme(int canal, _SoundParam*pPar, _MultiLayerParam*pstMLPar) {
    if (g_pThemeManager) {
        return g_pThemeManager->SetParam(canal, pPar, pstMLPar);
    }
    return 0;
}
