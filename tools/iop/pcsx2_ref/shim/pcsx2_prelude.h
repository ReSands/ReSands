#pragma once
// What PCSX2's SPU2 core (pcsx2/SPU2, GPL-3.0+) expects from the rest of PCSX2, for running it as the reference
// SPU2 behind ps2xIOP (tools/iop/pcsx2_ref). Our own: types, logging, the IOP clock and DMA registers it reads.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;
using uptr = uintptr_t;
using sptr = intptr_t;
using uint = unsigned int;

// As in PCSX2: a hint, not `inline` (spu2sys.cpp's __forceinline functions are called from the other files).
#define __forceinline __attribute__((always_inline))
#define __fi __forceinline
#define pxAssume(x) ((void)0)
#define pxAssert(x) ((void)0)
#define pxAssertMsg(x, msg) ((void)0)
#define pxFail(msg) ((void)0)
#define pxFailRel(msg) ((void)0)
#define jNO_DEFAULT \
    default:      \
        break;
static constexpr bool IsDevBuild = false;

// PCSX2 builds the mixer once per instruction set, each in its own namespace; here there is one.
#define MULTI_ISA_DEF(...) namespace isa_native { __VA_ARGS__ }
#define MULTI_ISA_UNSHARED_START namespace isa_native {
#define MULTI_ISA_UNSHARED_END }
#define MULTI_ISA_COMPILE_ONCE 1
#define MULTI_ISA_SELECT(name) isa_native::name

struct Pcsx2Console
{
    template <typename... A> void WriteLn(A...) const {}
    template <typename... A> void Warning(A...) const {}
    template <typename... A> void Error(A...) const {}
};
inline constexpr Pcsx2Console Console{}, DevCon{};

// The IOP clock the core reads (psxRegs.cycle, IOP cycles at 36.864 MHz) and its event counter hooks.
struct Pcsx2PsxRegs
{
    u32 cycle = 0;
};
extern Pcsx2PsxRegs psxRegs;
struct Pcsx2PsxCounter
{
    u32 startCycle = 0;
    s32 deltaCycles = 0;
};
extern Pcsx2PsxCounter psxCounters[8];
extern s32 psxNextDeltaCounter;
extern u32 psxNextStartCounter;

// IOP DMA channel 4 / 7 registers the core advances; our runtime runs the IOP's DMA itself, so these are scratch.
extern u32 HW_DMA4_MADR, HW_DMA4_TADR, HW_DMA4_BCR, HW_DMA4_CHCR, HW_DMA7_MADR, HW_DMA7_TADR, HW_DMA7_BCR, HW_DMA7_CHCR;
u8 *iopPhysMem(u32 address);
void spu2Irq();
void spu2DMA4Irq();
void spu2DMA7Irq();

// Save states are not used here.
struct DataBlock;
enum class FreezeAction { Load, Save, Size };
struct freezeData { int size; u8 *data; };
