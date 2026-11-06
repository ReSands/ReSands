#include "dare.h"
extern CThemeManager *g_pThemeManager;
void SND_fn_vDesInitTheme(void)
{
    delete g_pThemeManager;
    g_pThemeManager = 0;
}
