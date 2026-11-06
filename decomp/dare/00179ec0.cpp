#include "dare.h"
extern "C" char* strcpy(char*, const char*);

void fn_vResetMapMem(_tdstMapMem *pstMapMem)
{
    pstMapMem->bLoaded = 0;
    strcpy(pstMapMem->czLoadedMapName, "dex");
    pstMapMem->stNLoc.lMapIdLoaded = -2;
    pstMapMem->stLoc.lMapIdLoaded = -2;
    pstMapMem->stNLoc.pstMapHeader = 0;
    pstMapMem->stLoc.pstMapHeader = 0;
    pstMapMem->stLoc.bSubMapFixLoaded = 0;
    pstMapMem->stNLoc.bSubMapFixLoaded = 0;
    pstMapMem->stNLoc.lSubMapIdLoaded = -2;
    pstMapMem->stLoc.lSubMapIdLoaded = -2;
    pstMapMem->stNLoc.stSubBlockFixDataList.hFirstElementOpt = 0;
    pstMapMem->stNLoc.stSubBlockFixDataList.lNumberOfElementsOpt = 0;
    pstMapMem->stLoc.stSubBlockFixDataList.hFirstElementOpt = 0;
    pstMapMem->stLoc.stSubBlockFixDataList.lNumberOfElementsOpt = 0;
    pstMapMem->stNLoc.stCurrentSubBlockDataList.hFirstElementOpt = 0;
    pstMapMem->stNLoc.stCurrentSubBlockDataList.lNumberOfElementsOpt = 0;
    pstMapMem->stLoc.stCurrentSubBlockDataList.hFirstElementOpt = 0;
    pstMapMem->stLoc.stCurrentSubBlockDataList.lNumberOfElementsOpt = 0;
    pstMapMem->stNLoc.stBlockResMemList.hFirstElementOpt = 0;
    pstMapMem->stNLoc.stBlockResMemList.lNumberOfElementsOpt = 0;
    pstMapMem->stLoc.stBlockResMemList.hFirstElementOpt = 0;
    pstMapMem->stLoc.stBlockResMemList.lNumberOfElementsOpt = 0;
}
