#include "dare.h"

int SND_fn_bLoadResBinaryMultiLayer(_tdstBlockResourceDisk* _pBRDisk, _tdstBlockResourceMem* _pBRMem, char* pDataBloc) {
    _pBRMem->uRes.stMultiLayer.hFirstElementOpt = (_tdstMultiLayerElement*)(pDataBloc + _pBRDisk->ulExtraDataOffset);
    _pBRMem->uRes.stMultiLayer.lNumberOfElementsOpt = _pBRDisk->uRes.stMultiLayer.ulNbElements;

    int iPos;
    _tdstMultiLayerElement* pMultiLayerElement;

    pMultiLayerElement = _pBRMem->uRes.stMultiLayer.hFirstElementOpt;
    iPos = 0;
    while (iPos < _pBRMem->uRes.stMultiLayer.lNumberOfElementsOpt) {
        pMultiLayerElement->uRes.Id = (unsigned int)SND_fn_pGetBinRes(pMultiLayerElement->uRes.Id);
        iPos++;
        pMultiLayerElement++;
    }

    pMultiLayerElement = _pBRMem->uRes.stMultiLayer.hFirstElementOpt;
    iPos = 0;
    while (iPos < _pBRMem->uRes.stMultiLayer.lNumberOfElementsOpt) {
        SND_fn_bLoadResBinaryMultiLayerElement(pMultiLayerElement, pDataBloc);
        iPos++;
        pMultiLayerElement++;
    }

    _pBRMem->Id = _pBRDisk->Id;
    _pBRMem->eType = _pBRDisk->eType;
    _pBRMem->rVolume_dB = _pBRDisk->rVolume_dB;
    _pBRMem->uRes.stMultiLayer.ulNbElements = _pBRDisk->uRes.stMultiLayer.ulNbElements;
    return 1;
}
