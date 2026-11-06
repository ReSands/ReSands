#include "dare.h"

extern _SND_tdstAllRetSoundObject g_astTypeObjectSoundInfosDefault;

void fn_vUpdateTypeObjectSoundInfos(_SND_tdstAllRetSoundObject *ptrfct, _SND_tdstAllRetSoundObject *new_ptrfct) {
    SND_fn_vMemCopySnd(ptrfct, new_ptrfct, 0x24);
    if (!ptrfct->POS) ptrfct->POS = g_astTypeObjectSoundInfosDefault.POS;
    if (!ptrfct->SPEED) ptrfct->SPEED = g_astTypeObjectSoundInfosDefault.SPEED;
    if (!ptrfct->ExtraCoef) ptrfct->ExtraCoef = g_astTypeObjectSoundInfosDefault.ExtraCoef;
    if (!ptrfct->Switch) ptrfct->Switch = g_astTypeObjectSoundInfosDefault.Switch;
    if (!ptrfct->MultiLayer) ptrfct->MultiLayer = g_astTypeObjectSoundInfosDefault.MultiLayer;
    if (!ptrfct->MicroLink) ptrfct->MicroLink = g_astTypeObjectSoundInfosDefault.MicroLink;
    if (!ptrfct->VectorI) ptrfct->VectorI = g_astTypeObjectSoundInfosDefault.VectorI;
    if (!ptrfct->VectorJ) ptrfct->VectorJ = g_astTypeObjectSoundInfosDefault.VectorJ;
    if (!ptrfct->VectorK) ptrfct->VectorK = g_astTypeObjectSoundInfosDefault.VectorK;
}
