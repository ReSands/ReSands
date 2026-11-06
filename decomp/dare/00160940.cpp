#include "dare.h"
extern "C" {
void sceSifInitRpc(unsigned int);
int GetThreadId(void);
void sceSifSetRpcQueue(_sif_queue_data *, int);
void sceSifRegisterRpc(_sif_serve_data *, unsigned int, void *(*)(unsigned int, void *, int), void *,
                       void *(*)(unsigned int, void *, int), void *, _sif_queue_data *);
void sceSifRpcLoop(_sif_queue_data *);
}
extern unsigned char gs_cRPCArgBuf[36864];
void fn_vUbiEELoop(void *)
{
    _sif_queue_data queueData;
    _sif_serve_data serverData;
    sceSifInitRpc(0);
    sceSifSetRpcQueue(&queueData, GetThreadId());
    sceSifRegisterRpc(&serverData, 0x87654321, SND_fn_pvRPC_EEServerFunc, gs_cRPCArgBuf, 0, 0, &queueData);
    sceSifRpcLoop(&queueData);
}
