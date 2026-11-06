#include "dare.h"

extern void sceSdRemote(unsigned int, unsigned int, unsigned int, unsigned int);

void fn_vChangeSoundDataInputAreaVolume(unsigned int ulVal) {
    sceSdRemote(1, 0x8010, 0xf80, ulVal);
    sceSdRemote(1, 0x8010, 0x1080, ulVal);
}
