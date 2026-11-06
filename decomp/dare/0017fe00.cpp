#include "dare.h"

extern CThemeManager* g_pThemeManager;

int SND_fn_bTestIsPlayingTheme(int canal) {
    if (g_pThemeManager) {
        return g_pThemeManager->IsThemePlaying(canal);
    }
    return 0;
}
