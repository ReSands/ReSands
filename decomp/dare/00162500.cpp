#include "dare.h"

extern int Create_TOggVorbis(TDecoder **pDecoder, const _SND_tdstToolFormat *pOutputFormat, _tdstBlockResourceMem *pRes);
extern int Create_TImaAdpcm(TDecoder **pDecoder, const _SND_tdstToolFormat *pOutputFormat, _tdstBlockResourceMem *pRes);
extern int Create_TPCM(TDecoder **pDecoder, const _SND_tdstToolFormat *pOutputFormat, _tdstBlockResourceMem *pRes);
extern int Create_TAdpcm(TDecoder **pDecoder, const _SND_tdstToolFormat *pOutputFormat, _tdstBlockResourceMem *pRes);
extern void SND_fn_vDisplayError(int, const char *);

int Create_TDecoder(TDecoder **pDecoder, const _SND_tdstToolFormat *pOutputFormat, _SND_tdeTypeZip eZip, _tdstBlockResourceMem *pRes) {
    switch (eZip) {
        case SAMPLE_ADPCM:
            return Create_TAdpcm(pDecoder, pOutputFormat, pRes);
        case SAMPLE_PCM:
            return Create_TPCM(pDecoder, pOutputFormat, pRes);
        case SAMPLE_IMAADPCM:
            return Create_TImaAdpcm(pDecoder, pOutputFormat, pRes);
        case SAMPLE_OGG:
            return Create_TOggVorbis(pDecoder, pOutputFormat, pRes);
        default:
            SND_fn_vDisplayError(0x18, "");
            return -1;
    }
}
