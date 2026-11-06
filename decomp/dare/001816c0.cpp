#include "dare.h"

int SND_fn_bLoadResBinarySwitch(_tdstBlockResourceDisk* _pBRDisk, _tdstBlockResourceMem* _pBRMem, char* pDataBloc) {
    int iPos;
    unsigned int* pSwitchElement;

    _pBRMem->uRes.stSwitch.hFirstElementOpt = (_tdstSwitchElement*)(pDataBloc + _pBRDisk->ulExtraDataOffset);
    _pBRMem->uRes.stSwitch.lNumberOfElementsOpt = _pBRDisk->uRes.stSwitch.ulNbElements;

    pSwitchElement = (unsigned int*)_pBRMem->uRes.stSwitch.hFirstElementOpt;
    for (iPos = 0; iPos < _pBRMem->uRes.stSwitch.lNumberOfElementsOpt; iPos++) {
        pSwitchElement[0] = (unsigned int)SND_fn_pGetBinRes((unsigned long)pSwitchElement[0]);
        pSwitchElement += 2;
    }

    _pBRMem->Id = _pBRDisk->Id;
    _pBRMem->eType = _pBRDisk->eType;
    _pBRMem->rVolume_dB = _pBRDisk->rVolume_dB;
    _pBRMem->uRes.stSwitch.ulNbElements = _pBRDisk->uRes.stSwitch.ulNbElements;
    _pBRMem->uRes.stSwitch.lTypeSwitch = _pBRDisk->uRes.stSwitch.lTypeSwitch;
    _pBRMem->uRes.stSwitch.hDefault = _pBRDisk->uRes.stSwitch.hDefault;

    _pBRMem->uRes.stSwitch.bDynamic = _pBRDisk->uRes.stSwitch.bDynamic;

    return 1;
}
