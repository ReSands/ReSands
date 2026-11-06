#include "dare.h"

int SndManager::LoadBinaryResourceSilence(_tdstBlockResourceDisk* disk, _tdstBlockResourceMem* mem, char*) {
    mem->Id = disk->Id;
    mem->eType = disk->eType;
    mem->rVolume_dB = disk->rVolume_dB;
    *(unsigned int *)&mem->uRes = *(unsigned int *)&disk->uRes;
    return 1;
}
