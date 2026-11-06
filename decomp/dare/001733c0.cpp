#include "dare.h"

extern int gs_bInitConfSet;
extern int gs_stEngineConf[11];

_SND_tdeInit SND_fn_eInitEngineConf(void) {
  if (gs_bInitConfSet == 0) {
    gs_stEngineConf[2] = 0;
    gs_bInitConfSet = 1;
    gs_stEngineConf[1] = 0;
    gs_stEngineConf[3] = 0;
    gs_stEngineConf[0] = 0;
    gs_stEngineConf[4] = 0;
    gs_stEngineConf[6] = 0;
    gs_stEngineConf[7] = 0;
    gs_stEngineConf[8] = 0;
    gs_stEngineConf[9] = 0;
    gs_stEngineConf[10] = 0;
  }
  return (_SND_tdeInit)1;
}
