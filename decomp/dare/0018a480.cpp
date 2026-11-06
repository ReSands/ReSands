#include "dare.h"

extern int IsResourceLooping(_tduRefRes uRes);

int SndManager::PlayToolVoice(_VOICE *pVoice, _tduRefRes uRes, _SoundParam *pstPar, int lPrio) {
    if (IsResourceLooping(uRes)) {
        return PlayLoopingVoice(pVoice, uRes, pstPar, lPrio);
    } else {
        return PlayOneShotVoice(pVoice, uRes, pstPar, lPrio);
    }
}
