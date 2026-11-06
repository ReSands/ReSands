#include "dare.h"

unsigned int RPS2::GetHwModuleId(_tduRefRes uRes)
{
    return uRes.pstPtr->uRes.stSample.bIOPStream != 0;
}
