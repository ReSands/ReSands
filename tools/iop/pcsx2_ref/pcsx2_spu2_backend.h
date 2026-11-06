#pragma once
// PCSX2's SPU2 core (pcsx2/SPU2, GPL-3.0+, built by build.py) as a ps2xIOP SPU2 backend: the reference for the
// native mixer (tools/iop/iop_native_check.cpp --sound). The game's real libsd writes SPU2 registers and DMA on
// PCSX2's core, as it would on a PS2. Our code is only the glue that stands in for PCSX2's spu2.cpp (its emulator
// side: clock, DMA, output). One instance at a time (PCSX2's SPU2 state is global).
#include "ps2x/iop/spu2_backend.h"

#include <memory>

namespace pop
{
    std::unique_ptr<ps2x::iop::Spu2Backend> makePcsx2Spu2Backend();
}
