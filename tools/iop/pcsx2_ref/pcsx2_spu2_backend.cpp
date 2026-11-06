// Glue between ps2xIOP and PCSX2's SPU2 core; see pcsx2_spu2_backend.h. Compiled with build.py's flags:
//   -include shim/pcsx2_prelude.h -I shim -I game/analysis/pcsx2_spu2/<commit>
#include "pcsx2_spu2_backend.h"

#include "SPU2/defs.h"
#include "SPU2/spu2.h"

#include <deque>
#include <vector>

// What PCSX2's emulator provides to the core (see shim/pcsx2_prelude.h).
Pcsx2PsxRegs psxRegs;
Pcsx2PsxCounter psxCounters[8];
s32 psxNextDeltaCounter = 0;
u32 psxNextStartCounter = 0;
u32 HW_DMA4_MADR, HW_DMA4_TADR, HW_DMA4_BCR, HW_DMA4_CHCR, HW_DMA7_MADR, HW_DMA7_TADR, HW_DMA7_BCR, HW_DMA7_CHCR;
u64 lClocks = 0;
const StereoOut32 StereoOut32::Empty(0, 0);

bool SPU2::IsRunningPSXMode()
{
    return false;
}

u8 *iopPhysMem(u32)
{
    return nullptr; // the core only falls back to it after a PCSX2 state load
}

namespace
{
    constexpr u32 kTick = 768; // IOP cycles per output sample (PCSX2's TickInterval)

    class Pcsx2Spu2Backend;
    Pcsx2Spu2Backend *s_backend = nullptr;

    class Pcsx2Spu2Backend final : public ps2x::iop::Spu2Backend
    {
    public:
        Pcsx2Spu2Backend() { s_backend = this; }
        ~Pcsx2Spu2Backend() override { s_backend = nullptr; }

        void reset() override
        {
            // PCSX2 SPU2::InternalReset (non-PS1 mode) and SPU2::Open's clock.
            spu2Mix = MULTI_ISA_SELECT(spu2Mix);
            ReverbDownsample = MULTI_ISA_SELECT(ReverbDownsample);
            ReverbUpsample = MULTI_ISA_SELECT(ReverbUpsample);
            std::memset(spu2regs, 0, sizeof(spu2regs));
            std::memset(_spu2mem, 0, sizeof(_spu2mem));
            std::memset(_spu2mem + 0x2800, 7, 0x10); // from PCSX2: the BIOS's voice lock and Megaman X7's loop
            std::memset(_spu2mem + 0xe870, 7, 0x10);
            Spdif.Info = 0;
            Cores[0].Init(0);
            Cores[1].Init(1);
            Cycles = 0;
            m_cycle = 0;
            psxRegs.cycle = 0;
            lClocks = 0;
            for (int c = 0; c < 2; ++c)
            {
                m_adma[c].clear();
                m_admaActive[c] = false;
            }
            m_out.clear();
            irqs = 0;
        }

        u16 read16(uint32_t physicalAddress, uint64_t cycle) override
        {
            clock(cycle);
            // PCSX2 SPU2read (the DMA data port case never matches there: it compares a masked address).
            const u32 mem = physicalAddress & 0xFFFFu;
            const u16 value = mem >= 0x800 ? spu2Ru16(mem) : *(regtable[mem >> 1]);
            startQueued();
            return value;
        }

        void write16(uint32_t physicalAddress, uint16_t value, uint64_t cycle) override
        {
            clock(cycle);
            SPU2_FastWrite(physicalAddress, value); // PCSX2 SPU2write
            startQueued();
        }

        bool autoDma(uint32_t core) const override
        {
            return (Cores[core & 1u].AutoDMACtrl & ((core & 1u) + 1u)) != 0u;
        }

        // A plain transfer, all of it now: PCSX2 copies a part per call and the rest as its IOP DMA clock runs, but
        // our runtime times the IOP side itself. Then what PCSX2's IOP DMA does at the end (SPU2interruptDMA4/7).
        void dmaWrite(uint32_t core, const uint8_t *source, size_t bytes, uint64_t cycle) override
        {
            clock(cycle);
            V_Core &c = Cores[core & 1u];
            c.DoDMAwrite(reinterpret_cast<u16 *>(const_cast<uint8_t *>(source)), static_cast<u32>(bytes / 2u));
            while (c.ReadSize != 0u)
                c.FinishDMAwrite();
            dmaEnd(c);
        }

        void dmaRead(uint32_t core, uint8_t *destination, size_t bytes, uint64_t cycle) override
        {
            clock(cycle);
            V_Core &c = Cores[core & 1u];
            c.DoDMAread(reinterpret_cast<u16 *>(destination), static_cast<u32>(bytes / 2u));
            while (c.ReadSize != 0u)
                c.FinishDMAread();
            dmaEnd(c);
        }

        // The runtime hands over each AutoDMA block when its IOP DMA starts; PCSX2's core takes one at a time and
        // says when it has read one (spu2DMA4Irq / 7), so the next waits here.
        void admaWrite(uint32_t core, const uint8_t *source, size_t bytes, uint64_t cycle) override
        {
            clock(cycle);
            m_adma[core & 1u].push_back({reinterpret_cast<u16 *>(const_cast<uint8_t *>(source)), static_cast<u32>(bytes / 2u)});
            startQueued();
        }

        // A sample at a time, so a queued AutoDMA block starts the sample its predecessor ends.
        void advanceTo(uint64_t cycle, const AudioSink &sink) override
        {
            while (m_cycle + kTick <= cycle)
            {
                clock(m_cycle + kTick);
                startQueued();
            }
            clock(cycle);
            if (!m_out.empty())
            {
                sink(m_out);
                m_out.clear();
            }
        }

        void output(StereoOut32 out)
        {
            m_out.push_back(static_cast<int16_t>(std::clamp(out.Left, -0x8000, 0x7FFF)));
            m_out.push_back(static_cast<int16_t>(std::clamp(out.Right, -0x8000, 0x7FFF)));
        }
        void admaDone(int core) { m_admaActive[core] = false; }
        uint64_t irqs = 0;

    private:
        void clock(uint64_t cycle)
        {
            if (cycle > m_cycle)
                m_cycle = cycle;
            psxRegs.cycle = static_cast<u32>(m_cycle); // TimeUpdate works in u32 differences, so the wrap is fine
            TimeUpdate(psxRegs.cycle);
        }

        static void dmaEnd(V_Core &c)
        {
            if (c.DmaMode)
                c.Regs.STATX |= 0x80;
            c.Regs.STATX &= ~0x400;
            c.TSA = c.ActiveTSA;
        }

        void startQueued()
        {
            for (int core = 0; core < 2; ++core)
            {
                if (!autoDma(static_cast<uint32_t>(core)))
                {
                    m_adma[core].clear(); // the driver stopped the input
                    m_admaActive[core] = false;
                    continue;
                }
                if (m_admaActive[core] || m_adma[core].empty())
                    continue;
                const Block block = m_adma[core].front();
                m_adma[core].pop_front();
                m_admaActive[core] = true;
                Cores[core].DoDMAwrite(block.data, block.words);
            }
        }

        struct Block
        {
            u16 *data;
            u32 words;
        };
        std::deque<Block> m_adma[2];
        bool m_admaActive[2] = {};
        uint64_t m_cycle = 0;
        std::vector<int16_t> m_out;
    };
}

// The core's calls out (PCSX2: spu2.cpp's output, the IOP's interrupt controller and DMA).
void spu2Output(StereoOut32 out)
{
    if (s_backend)
        s_backend->output(out); // PCSX2's DC filter is its host side; the console's output is taken as mixed
}

void spu2Irq()
{
    if (s_backend)
        ++s_backend->irqs; // shortcut: counted, not raised on the IOP (nor by the emulated SPU2); sdrdrv can set a handler (libsd 27)
}

void spu2DMA4Irq()
{
    if (s_backend)
        s_backend->admaDone(0);
}

void spu2DMA7Irq()
{
    if (s_backend)
        s_backend->admaDone(1);
}

namespace pop
{
    std::unique_ptr<ps2x::iop::Spu2Backend> makePcsx2Spu2Backend()
    {
        return std::make_unique<Pcsx2Spu2Backend>();
    }
}
