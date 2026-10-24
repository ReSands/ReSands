// pop_state_frame_vu1's VU1 core, plus POP_P9_TRACE=<file>: every sprite-program (P9) run of the frame is
// appended to <file> in the runtime's PS2X_VU1_TRACE format (ps2_vu1_core.cpp traceVu1Run), so vu1replay can cut
// stage cases for the P9 port (tools/vu1port) from saves, without booting the game. The image goes next to the
// file as vu1_<hash>.bin, as the runtime writes it. This file replaces ps2_vu1_core.cpp in that target: it needs
// the interpreter's private state (the same trick as tools/vu1port/replay.cpp).
#define private public
#define protected public
#include "ps2_vu1_core.cpp"
#undef private
#undef protected

namespace pop_native_live
{
    extern void (*g_spriteRunHook)(VU1Interpreter &vu, uint8_t *code, uint8_t *data, GS &gs, int32_t pc, uint32_t tops);
}

namespace
{
    FILE *g_p9File = nullptr;
    std::vector<uint8_t> g_p9Previous;
    uint64_t g_p9Records = 0;

    void p9Run(VU1Interpreter &vu, uint8_t *code, uint8_t *data, GS &gs, int32_t pc, uint32_t tops)
    {
        // the state run() starts from: execute() / resume() up to their run() call
        if (pc >= 0)
        {
            vu.resetScheduler();
            vu.m_state.pc = uint32_t(pc) & vu.microAddressMask();
            vu.m_state.ebit = false;
            vu.m_state.haltAfterDelaySlot = false;
            vu.m_state.branchPending = false;
            vu.m_state.branchTarget = 0;
            vu.m_state.branchDelay = 0;
            vu.m_state.vf[0][0] = vu.m_state.vf[0][1] = vu.m_state.vf[0][2] = 0.0f;
            vu.m_state.vf[0][3] = 1.0f;
        }
        vu.m_state.stoppedByD = false;
        vu.m_state.stoppedByT = false;
        vu.m_state.top = tops;
        vu.m_state.itop = 0;
        const uint32_t maxCycles = 1u << 20;
        VU1InterpreterAccess::TraceState state;
        if (g_p9File && VU1InterpreterAccess::traceCapture(vu, state))
        {
            const uint32_t magic = 0x52543156u;
            const uint64_t hash = fnv1a64(code, 0x4000), tick = g_p9Records;
            std::fwrite(&magic, 4, 1, g_p9File);
            std::fwrite(&maxCycles, 4, 1, g_p9File);
            std::fwrite(&hash, 8, 1, g_p9File);
            std::fwrite(&tick, 8, 1, g_p9File);
            std::fwrite(&state, sizeof state, 1, g_p9File);
            // the first record of a process carries the whole data memory: the replayer's image still holds the
            // previous save's
            const bool all = g_p9Previous.empty();
            if (all)
                g_p9Previous.assign(0x4000, 0);
            for (uint32_t i = 0; i < 1024;)
            {
                if (!all && !std::memcmp(data + i * 16, g_p9Previous.data() + i * 16, 16))
                {
                    ++i;
                    continue;
                }
                uint32_t end = i + 1;
                while (end < 1024 && (all || std::memcmp(data + end * 16, g_p9Previous.data() + end * 16, 16)))
                    ++end;
                const uint16_t h[2] = {uint16_t(i), uint16_t(end - i)};
                std::fwrite(h, 4, 1, g_p9File);
                std::fwrite(data + i * 16, 16, end - i, g_p9File);
                i = end;
            }
            const uint16_t terminator[2] = {0xFFFFu, 0u};
            std::fwrite(terminator, 4, 1, g_p9File);
            std::memcpy(g_p9Previous.data(), data, 0x4000);
            if (!g_p9Records++)
            {
                // the image, named as the runtime names it
                const std::filesystem::path dir = std::filesystem::path(std::getenv("POP_P9_TRACE")).parent_path();
                char name[64];
                std::snprintf(name, sizeof name, "vu1_%016llx.bin", (unsigned long long)hash);
                if (FILE *f = std::fopen((dir / name).string().c_str(), "wb"))
                {
                    std::fwrite(code, 1, 0x4000, f);
                    std::fclose(f);
                }
            }
        }
        vu.run(code, 0x4000, data, 0x4000, gs, nullptr, maxCycles);
    }

    const bool g_p9Installed = [] {
        const char *path = std::getenv("POP_P9_TRACE");
        if (!path || !*path)
            return false;
        g_p9File = std::fopen(path, "ab");
        if (!g_p9File)
            return false;
        std::atexit([] {
            std::fclose(g_p9File);
            std::fprintf(stderr, "[p9trace] %llu runs recorded\n", (unsigned long long)g_p9Records);
        });
        pop_native_live::g_spriteRunHook = p9Run;
        return true;
    }();
}
