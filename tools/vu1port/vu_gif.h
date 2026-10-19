// The GIF packet an XGKICK sends from VU1 data memory: GIF tags and their data up to the tag with EOP
// (addresses wrap at 16 KB, as the interpreter's transfer does). Shared by vu1replay and the judge.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

inline std::vector<uint8_t> vuGifPacket(const uint8_t *mem, uint32_t qwordAddress)
{
    std::vector<uint8_t> out;
    uint32_t qw = qwordAddress & 0x3FF;
    for (int tags = 0; tags < 1024; ++tags)
    {
        uint64_t lo;
        std::memcpy(&lo, mem + qw * 16, 8);
        const uint64_t nloop = lo & 0x7FFF, eop = (lo >> 15) & 1, flg = (lo >> 58) & 3;
        uint64_t nreg = (lo >> 60) & 0xF;
        if (!nreg)
            nreg = 16;
        const uint64_t qwords = 1 + (flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop);
        for (uint64_t i = 0; i < qwords && out.size() < 0x4000; ++i, qw = (qw + 1) & 0x3FF)
            out.insert(out.end(), mem + qw * 16, mem + qw * 16 + 16);
        if (eop || out.size() >= 0x4000)
            break;
    }
    return out;
}
