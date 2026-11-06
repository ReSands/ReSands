#include "dare.h"

extern int RPS2::GetResLength(_tduRefRes uRes);

int SndManager::GetLengthSample(_tduRefRes uRes) {
    return RPS2::GetResLength(uRes);
}
