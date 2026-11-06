#include "dare.h"
extern int sceSdRemote(unsigned int, unsigned int, unsigned int, unsigned int);

void fn_vChangeMasterVolume(unsigned int ulVal)
{
    int iCore = 0;
    do {
        sceSdRemote(1, 0x8010, iCore | 0x980, ulVal);
        sceSdRemote(1, 0x8010, iCore | 0xa80, ulVal);
        sceSdRemote(1, 0x8010, iCore | 0x800, -0x31);
        iCore++;
    } while (iCore < 2);
}
