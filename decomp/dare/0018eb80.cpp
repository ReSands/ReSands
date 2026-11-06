#include "dare.h"

extern void* SND_fn_pvMallocSnd(unsigned int, unsigned int);

void CASyncBankLoader::InitFields(int bConstructor) {
    m_bIsIdle = 1;
    m_eLoadingState = (_tdeLoadSoundBankState)0;
    m_eAbortingState = (_tdeAbortLoadState)0;
    m_pBank = 0;
    m_pResDisk = 0;
    m_pstBankHeader = 0;
    m_lWaveFormAllocated = 0;
    m_lInitializedResources = 0;
    m_lInitializedEvents = 0;
    m_lUnloadedResources = 0;
    m_lFreedSubBlocksData = 0;
    m_hASyncLoadRes = -1;
    m_stAsyncRead.hBankFile = -1;
    m_stAsyncRead.bDone = 1;
    m_stAsyncRead.ulOffset = 0;
    m_stAsyncRead.ulSize = 0;
    m_stAsyncRead.ulSynched = 0;
    m_stAsyncRead.pvSync = 0;
    m_stAsyncRead.pvDest = 0;
    m_stBlockTransfer.hAsyncTransfer = 0;
    m_stBlockTransfer.ulTotalSize = 0;
    m_stBlockTransfer.lReadBufferIdx = -2;
    m_stBlockTransfer.lTransferBufferIdx = -2;
    m_stBlockTransfer.ulSynchedSize = 0;
    m_stBlockTransfer.ulTransferedSize = 0;
    m_stBlockTransfer.bDone = 1;
    m_stBlockTransfer.stDoubleBuffer[0].ulSize = 0;
    m_stBlockTransfer.stDoubleBuffer[1].ulSize = 0;
    if (bConstructor != 0) {
        m_stBlockTransfer.stDoubleBuffer[0].pvReadBuffer = 0;
        m_stBlockTransfer.stDoubleBuffer[1].pvReadBuffer = 0;
    } else {
        if (m_stBlockTransfer.stDoubleBuffer[0].pvReadBuffer == 0)
            m_stBlockTransfer.stDoubleBuffer[0].pvReadBuffer = SND_fn_pvMallocSnd(0x8000, 0);
        if (m_stBlockTransfer.stDoubleBuffer[1].pvReadBuffer == 0)
            m_stBlockTransfer.stDoubleBuffer[1].pvReadBuffer = SND_fn_pvMallocSnd(0x8000, 0);
        if (m_stBlockTransfer.stDoubleBuffer[0].pvReadBuffer == 0 || m_stBlockTransfer.stDoubleBuffer[1].pvReadBuffer == 0) {
            SND_fn_vDisplayErrorEx(10, (char *)"Problem while Allocating memory.", 3);
            m_eLoadingState = (_tdeLoadSoundBankState)0x13;
        }
    }
}
