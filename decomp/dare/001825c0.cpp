#include "dare.h"

extern int iSmallAligned[16];

int SND_fn_lGetNbVoiceWishedSxdWave(void)
{
  SND_fn_iIOPCommandRPC(2, 0, 0, &iSmallAligned[0], 16);
  return iSmallAligned[0];
}
