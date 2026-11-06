#include "dare.h"

void SND_fn_bLoadResBinaryMultiLayerElement(_tdstMultiLayerElement*pMultiLayerElement, char*pDataBloc) {
    _tdstMultiLayerElement* s2 = pMultiLayerElement;
    char* s1 = pDataBloc;
    s2->hFirstElementOpt = (_tdstEffectGraph*)(s1 + s2->lOffsetEffectList);
    s2->lNumberOfElementsOpt = s2->ulNbEffect;
    _tdstEffectGraph* pEffectGraph = s2->hFirstElementOpt;
    int iPos = 0;
    while (iPos < s2->lNumberOfElementsOpt) {
        SND_fn_bLoadResBinaryEffectGraph(pEffectGraph, s1);
        iPos++;
        pEffectGraph++;
    }
}
