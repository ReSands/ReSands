// vu1replay: replays VU1 runs recorded with PS2X_VU1_TRACE (runtime ps2_vu1_core.cpp traceVu1Run) through
// the runtime's own VU1 interpreter and writes each run's XGKICK output: the judge for VU1 program ports
// (tools/vu1port/). Record: 'V1TR', maxCycles, image hash, vsync tick, TraceState (as the runtime build that
// wrote it lays it out; this tool is built x86_64 so that the layout is the PC's), then VU data deltas
// against the previous record ({first qword, count} + data, ended by {0xFFFF, 0}).
//
//   vu1replay <trace> <image dir> <out> [--only <hash>] [--limit N] [--twice]
// <out>: per replayed run 'V1RO', record index, image hash, start pc, packet count, then per packet its
// byte size and bytes; then the VU data after the run (16 KB) and the end state's vi[16] (judges of a port's
// memory side effects). --only: just the runs of one image (hex); --twice: run each twice, compare (the
// interpreter must be deterministic for the judge to mean anything).
//   --coverage: per pc of the replayed runs, how many runs execute it (<out> becomes a text table).
//   --start <pc>: just the runs that start at that pc (e.g. one entry of a multi-entry program).
//   --stage <entry>:<exit>[,<exit>...]: <out> becomes the stage's cases for the port judge (judge.cpp): each
//   time a run reaches the entry pc, the state there (VuState, vu_task.h) and at the next exit pc reached, and
//   the packets kicked in between. Boundaries must be clean (no write pending); unclean ones are counted.
//   --max-cases N (default 4000), --stride N: write every Nth case only.
//   --cutscan <file> (with --hints): no cases; per pc of the replayed runs, what is in flight before it (Pending
//   bits, union over all visits), whether it is ever a delay slot, whether it lies between a MAC / clip flag write
//   and the FM* / FC* reader of those flags, visits; the hints then cover the whole program.
//   tools/vu1port/cutscan.py picks stage borders from both.
//   With --hints <file>: per pc inside the stage, which DIV the visible Q came from, which upper instruction
//   the visible MAC flags came from, and whether a branch saw a vi value from before the last write.
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
// a tool, not runtime code: the stepping loop below needs the interpreter's pipeline state
#define private public
#define protected public
#include "ps2_vu1_core.cpp"
#undef private
#undef protected
#include "vu_gif.h"
#include "vu_task.h"

void PS2Memory::submitGifPacket(GifPathId, const uint8_t *, uint32_t, bool, bool) {}
void GS::processGIFPacket(const uint8_t *, uint32_t) {}

namespace
{
    struct Run
    {
        std::vector<std::vector<uint8_t>> packets;
        std::vector<uint8_t> dataAfter;
        int32_t vi[16];
        bool ended;
    };

    // the interpreter's run loop (runLoopWith without compiled code), calling visit(pc) before each pair
    bool stepRun(VU1Interpreter &vu, uint8_t *code, uint8_t *data, GS &gs, uint32_t maxCycles, const std::function<void(uint32_t)> &visit,
                 const std::function<void(uint32_t)> &after = nullptr)
    {
        vu.m_activeVuData = data;
        vu.m_activeVuDataSize = 0x4000;
        vu.m_activeGs = &gs;
        vu.m_activeMemory = nullptr;
        const unsigned csr = _mm_getcsr();
        _mm_setcsr(csr | 0x6000u);
        const uint64_t budgetEnd = vu.m_cycle + maxCycles;
        bool ended = false;
        while (vu.m_cycle < budgetEnd && !vu.m_stopRequested)
        {
            vu1NormShadow.valid = false;
            vu.commitReadyPipelines();
            if (vu.m_state.pc + 8u > 0x4000u)
                break;
            visit(vu.m_state.pc);
            const VU1InterpreterAccess::Decoded &decoded = vu.getDecodedInstructionPairForPc(code, 0x4000, nullptr, vu.m_state.pc);
            if (decoded.upperUsage.reserved || decoded.lowerUsage.reserved)
                break;
            const uint32_t pc = vu.m_state.pc;
            const VU1InterpreterAccess::StepResult step = VU1InterpreterAccess::stepPair(vu, decoded, data, 0x4000, gs, nullptr, 0x4000, budgetEnd);
            if (after)
                after(pc);
            if (step == VU1InterpreterAccess::StepResult::Budget)
                break;
            if (step == VU1InterpreterAccess::StepResult::Ended)
            {
                ended = true;
                break;
            }
        }
        if (ended)
        {
            vu.flushPipelines();
            vu.m_state.ebit = false;
            vu.m_state.haltAfterDelaySlot = false;
            vu.m_pendingHaltD = false;
            vu.m_pendingHaltT = false;
        }
        _mm_setcsr(csr);
        return ended;
    }

    // what is still in flight at a stage boundary. vf / vi / ACC writes are harmless (a read of a pending
    // vf register stalls until it lands; the snapshot is the effective state); the others are not: Q / P
    // and the flags are read without stalling, pending stores and transfers change memory later.
    enum Pending : uint32_t
    {
        kVf = 1, kVi = 2, kAcc = 4, kStore = 8, kKick = 16, kQ = 32, kP = 64, kMacFlags = 128, kClipFlags = 256, kStatusFlags = 512, kBranch = 1024
    };
    const char *const kPendingNames[] = {"vf", "vi", "acc", "store", "xgkick", "Q", "P", "MAC", "clip", "status", "branch"};
    // --allow <kinds> adds to these for one program (P9: xgkick, the packet is taken from memory at the XGKICK, so a
    // transfer in flight changes nothing the judge sees; MAC and status, when the hints show no FMAND reading flags
    // from before the stage's entry and no status reader exists)
    // clip flags in flight are landed by snapshot(); the cutter counts the reads that would see them too early
    uint32_t kHarmless = kVf | kVi | kAcc | kClipFlags;

    uint32_t pending(const VU1Interpreter &vu)
    {
        uint32_t p = (vu.m_vfWriteValid ? kVf : 0) | (vu.m_viWriteValid ? kVi : 0) | (vu.m_accWriteValid ? kAcc : 0) |
                     (vu.m_storeValid ? kStore : 0) | (vu.m_xgkick.active ? kKick : 0) | (vu.m_fdiv.valid ? kQ : 0) |
                     (vu.m_efu[0].valid || vu.m_efu[1].valid ? kP : 0) | (vu.m_state.branchPending ? kBranch : 0);
        for (const auto &f : vu.m_flagPipeline)
            if (f.valid)
                p |= (f.writesMac ? kMacFlags : 0) | (f.writesClip ? kClipFlags : 0) | (f.writesStatus ? kStatusFlags : 0);
        return p;
    }
    bool clean(const VU1Interpreter &vu, uint32_t *seen = nullptr)
    {
        const uint32_t p = pending(vu);
        if (seen)
            *seen |= p;
        return (p & ~kHarmless) == 0;
    }

    void snapshot(const VU1Interpreter &vu, const uint8_t *mem, VuState &out)
    {
        const VU1State s = VU1InterpreterAccess::effectiveState(vu);
        std::memcpy(out.vf, s.vf, sizeof out.vf);
        std::memcpy(out.vi, s.vi, sizeof out.vi);
        std::memcpy(out.acc, s.acc, sizeof out.acc);
        // Q as it is once a DIV in flight lands: a port computes a DIV's result at once, and stage borders
        // may fall between a DIV and its result (P6's inner loops end with one; nothing reads Q before it lands)
        out.q = vu.m_fdiv.valid ? vu.m_fdiv.value : s.q;
        out.p = s.p;
        out.i = s.i;
        out.mac = s.mac;
        // clip flags likewise, as they are once the CLIPs in flight land (m_workingClip): a port applies a CLIP at
        // once. Until 25 Oct this took the visible flags, so P6's b_ck_loop / b_ck_next (a CLIP 1-3 pairs before
        // their boundaries in every case) could not pass and b_ck_head passed by clearing the flags it had set
        out.clip = vu.m_workingClip & 0xFFFFFFu;
        out.status = s.status;
        out.top = s.top;
        out.pc = s.pc;
#if VU_TASK_R
        out.r = s.r;
#endif
        std::memcpy(out.mem, mem, sizeof out.mem);
    }

    // where the visible Q / MAC flags came from (pc of the producing instruction), per pc visited
    struct Hints
    {
        std::map<uint32_t, std::set<uint32_t>> q, mac, clip;
        std::map<uint32_t, std::set<uint32_t>> viBackup; // branch pc -> vi registers whose old value a branch there would read
        uint32_t qSource = 0xFFFFFFFF, macSource = 0xFFFFFFFF, clipSource = 0xFFFFFFFF;
        bool divPending = false;
        uint32_t divPc = 0;
        std::map<uint64_t, uint32_t> flagIssue, clipIssue; // pending MAC / clip-writing entries: issue cycle -> pc
        void reset()
        {
            qSource = macSource = clipSource = 0xFFFFFFFF;
            divPending = false;
            flagIssue.clear();
            clipIssue.clear();
        }
        // pending entries that left the pipeline since the last pair: the newest is the visible source
        static void retire(const VU1Interpreter &vu, std::map<uint64_t, uint32_t> &issued, uint32_t &source)
        {
            uint64_t newest = 0;
            bool any = false;
            for (auto it = issued.begin(); it != issued.end();)
            {
                bool still = false;
                for (const auto &f : vu.m_flagPipeline)
                    if (f.valid && f.issueCycle == it->first)
                        still = true;
                if (!still)
                {
                    if (!any || it->first >= newest)
                    {
                        newest = it->first;
                        source = it->second;
                        any = true;
                    }
                    it = issued.erase(it);
                }
                else
                    ++it;
            }
        }
        // before the pair at pc (after the pipelines committed)
        void before(const VU1Interpreter &vu, uint32_t pc, bool record)
        {
            if (divPending && !vu.m_fdiv.valid)
            {
                qSource = divPc;
                divPending = false;
            }
            retire(vu, flagIssue, macSource);
            retire(vu, clipIssue, clipSource);
            if (record)
            {
                q[pc].insert(qSource);
                mac[pc].insert(macSource);
                clip[pc].insert(clipSource);
                if (vu.m_viBranchBackupValid)
                    viBackup[pc].insert(vu.m_viBranchBackupReg);
            }
        }
        // after the pair at pc: new pending DIV / flag entries are this pair's
        void after(const VU1Interpreter &vu, uint32_t pc, uint32_t lower)
        {
            if (vu.m_fdiv.valid && !divPending)
            {
                divPending = true;
                divPc = pc;
            }
            else if (vu.m_fdiv.valid && ((lower >> 25) == 0x40) && ((lower & 0x7FF) == 0x3BC || (lower & 0x7FF) == 0x3BD || (lower & 0x7FF) == 0x3BE))
                divPc = pc; // DIV / SQRT / RSQRT replacing a pending one
            for (const auto &f : vu.m_flagPipeline)
            {
                if (f.valid && f.writesMac && !flagIssue.count(f.issueCycle))
                    flagIssue[f.issueCycle] = pc;
                if (f.valid && f.writesClip && !clipIssue.count(f.issueCycle))
                    clipIssue[f.issueCycle] = pc;
            }
        }
    };

    Run replay(const VU1InterpreterAccess::TraceState &state, const std::vector<uint8_t> &code, const std::vector<uint8_t> &data,
               uint32_t maxCycles)
    {
        static VU1Interpreter vu(VU1Interpreter::Unit::VU1);
        alignas(64) static unsigned char gsStorage[1 << 16]; // never touched: the sink does not forward
        GS &gs = *reinterpret_cast<GS *>(gsStorage);
        VU1InterpreterAccess::traceRestore(vu, state);
        Run run;
        run.dataAfter = data;
        std::vector<uint8_t> codeCopy = code;
        Vu1PacketSink sink;
        sink.forward = false;
        packetSink = &sink;
        run.ended = VU1InterpreterAccess::runLoopWith(vu, codeCopy.data(), uint32_t(codeCopy.size()), run.dataAfter.data(),
                                                      uint32_t(run.dataAfter.size()), gs, nullptr, maxCycles, nullptr);
        packetSink = nullptr;
        run.packets = std::move(sink.packets);
        const VU1State end = VU1InterpreterAccess::effectiveState(vu);
        for (int i = 0; i < 16; ++i)
            run.vi[i] = int32_t(end.vi[i]);
        return run;
    }
}

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: vu1replay <trace> <image dir> <out> [--only <hash>] [--limit N] [--twice]\n");
        return 2;
    }
    uint64_t only = 0;
    uint32_t startPc = 0xFFFFFFFF;
    long limit = -1;
    bool twice = false, coverage = false;
    uint32_t stageEntry = 0xFFFFFFFF;
    std::set<uint32_t> stageExits;
    std::string hintsPath, cutscanPath;
    std::map<uint32_t, uint32_t> scanPending; // --cutscan: pc -> Pending bits seen before it
    std::map<uint32_t, uint64_t> scanVisits;
    std::set<uint32_t> scanDelay;            // pcs reached as a delay slot
    // pcs between a MAC / clip flag write and the FM* / FC* reader that reads it: a stage starting there would
    // read flags from before its entry
    std::set<uint32_t> scanSplitsFlags;
    std::vector<uint32_t> scanPath;          // this run's pcs in order
    std::map<uint32_t, size_t> scanLast;     // pc -> its last index in scanPath
    std::vector<size_t> scanClipWrites;      // scanPath indices of CLIP / FCSET
    long maxCases = 4000;
    long stride = 1; // --stride N: write every Nth case (spread over the whole trace instead of its start)
    uint64_t cases = 0, uncleanEntry = 0, uncleanExit = 0, unfinished = 0;
    uint32_t pendingEntry = 0, pendingExit = 0; // kinds seen in flight (Pending bits)
    std::set<uint64_t> entryClips;               // CLIPs in flight at the current case's entry (issue cycles)
    uint64_t earlyClipReads = 0;
    Hints hints;
    std::map<uint32_t, uint64_t> covered; // pc -> runs that execute it
    for (int i = 4; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--only" && i + 1 < argc)
            only = std::stoull(argv[++i], nullptr, 16);
        else if (a == "--start" && i + 1 < argc)
            startPc = uint32_t(std::stoul(argv[++i], nullptr, 16));
        else if (a == "--limit" && i + 1 < argc)
            limit = std::stol(argv[++i]);
        else if (a == "--twice")
            twice = true;
        else if (a == "--coverage")
            coverage = true;
        else if (a == "--stage" && i + 1 < argc)
        {
            const std::string spec = argv[++i];
            const size_t colon = spec.find(':');
            stageEntry = uint32_t(std::stoul(spec.substr(0, colon), nullptr, 16));
            std::stringstream exits(spec.substr(colon + 1));
            for (std::string e; std::getline(exits, e, ',');)
                stageExits.insert(uint32_t(std::stoul(e, nullptr, 16)));
        }
        else if (a == "--hints" && i + 1 < argc)
            hintsPath = argv[++i];
        else if (a == "--cutscan" && i + 1 < argc)
        {
            cutscanPath = argv[++i];
            if (stageEntry == 0xFFFFFFFF)
                stageEntry = 0xFFFFFFFE; // the stepping loop, no stage
        }
        else if (a == "--max-cases" && i + 1 < argc)
            maxCases = std::stol(argv[++i]);
        else if (a == "--allow" && i + 1 < argc)
        {
            std::stringstream kinds(argv[++i]);
            for (std::string k; std::getline(kinds, k, ',');)
                for (uint32_t b = 0; b < 11; ++b)
                    if (k == kPendingNames[b])
                        kHarmless |= 1u << b;
        }
        else if (a == "--stride" && i + 1 < argc)
            stride = std::max(1L, std::stol(argv[++i]));
    }
    std::ifstream in(argv[1], std::ios::binary);
    std::ofstream out(argv[3], std::ios::binary);
    std::map<uint64_t, std::vector<uint8_t>> images;
    std::vector<uint8_t> data(0x4000, 0);
    std::map<uint64_t, uint64_t> perImage;
    uint64_t records = 0, replayed = 0, packets = 0, unended = 0, nondeterministic = 0;
    std::fprintf(stderr, "TraceState %zu bytes (VU1State %zu)\n", sizeof(VU1InterpreterAccess::TraceState), sizeof(VU1State));
    for (;;)
    {
        uint32_t magic = 0, maxCycles = 0;
        uint64_t hash = 0, tick = 0;
        if (!in.read(reinterpret_cast<char *>(&magic), 4))
            break;
        if (magic != 0x52543156u)
        {
            std::fprintf(stderr, "record %" PRIu64 ": bad magic %08x at %lld (TraceState layout differs from the recorder's?)\n", records, magic,
                         (long long)in.tellg() - 4);
            return 1;
        }
        VU1InterpreterAccess::TraceState state;
        in.read(reinterpret_cast<char *>(&maxCycles), 4);
        in.read(reinterpret_cast<char *>(&hash), 8);
        in.read(reinterpret_cast<char *>(&tick), 8);
        in.read(reinterpret_cast<char *>(&state), sizeof state);
        for (;;)
        {
            uint16_t h[2];
            in.read(reinterpret_cast<char *>(h), 4);
            if (!in || h[0] == 0xFFFFu)
                break;
            if (size_t(h[0] + h[1]) * 16 > data.size())
            {
                std::fprintf(stderr, "record %" PRIu64 ": delta out of range\n", records);
                return 1;
            }
            in.read(reinterpret_cast<char *>(data.data() + size_t(h[0]) * 16), std::streamsize(h[1]) * 16);
        }
        const uint64_t index = records++;
        ++perImage[hash];
        if ((only && hash != only) || (startPc != 0xFFFFFFFF && state.state.pc != startPc) || (limit >= 0 && long(replayed) >= limit))
            continue;
        auto img = images.find(hash);
        if (img == images.end())
        {
            char name[64];
            std::snprintf(name, sizeof name, "/vu1_%016llx.bin", (unsigned long long)hash);
            std::ifstream f(std::string(argv[2]) + name, std::ios::binary);
            std::vector<uint8_t> code((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (code.size() != 0x4000)
            {
                std::fprintf(stderr, "image %016llx missing\n", (unsigned long long)hash);
                return 1;
            }
            img = images.emplace(hash, std::move(code)).first;
        }
        if (stageEntry != 0xFFFFFFFF)
        {
            static VU1Interpreter vu(VU1Interpreter::Unit::VU1);
            alignas(64) static unsigned char gsStorage[1 << 16];
            VU1InterpreterAccess::traceRestore(vu, state);
            std::vector<uint8_t> code = img->second, mem = data;
            static VuState in, outState; // 16 KB each
            bool inside = false;
            uint32_t flags = 0;
            std::vector<std::vector<uint8_t>> kicks;
            Vu1PacketSink sink;
            sink.forward = false;
            packetSink = &sink;
            hints.reset();
            scanPath.clear();
            scanLast.clear();
            scanClipWrites.clear();
            const auto finish = [&](uint32_t exitFlags) {
                if (long(cases) % stride == 0 && long(cases) / stride < maxCases)
                {
                    const uint32_t head[3] = {0x45534143u /* CASE */, uint32_t(index), flags | exitFlags};
                    out.write(reinterpret_cast<const char *>(head), sizeof head);
                    out.write(reinterpret_cast<const char *>(&in), sizeof in);
                    out.write(reinterpret_cast<const char *>(&outState), sizeof outState);
                    const uint32_t n = uint32_t(kicks.size());
                    out.write(reinterpret_cast<const char *>(&n), 4);
                    for (const auto &k : kicks)
                    {
                        const uint32_t size = uint32_t(k.size());
                        out.write(reinterpret_cast<const char *>(&size), 4);
                        out.write(reinterpret_cast<const char *>(k.data()), size);
                    }
                }
                ++cases;
                inside = false;
                kicks.clear();
            };
            const auto visit = [&](uint32_t pc) {
                hints.before(vu, pc, (inside || !cutscanPath.empty()) && !hintsPath.empty());
                if (!cutscanPath.empty())
                {
                    uint32_t lower;
                    std::memcpy(&lower, code.data() + pc, 4);
                    const uint32_t op = lower >> 25;
                    const bool macReader = op == 0x18 || op == 0x1A || op == 0x1B;
                    const bool clipReader = op == 0x10 || op == 0x12 || op == 0x13 || op == 0x1C;
                    const uint32_t src = macReader ? hints.macSource : clipReader ? hints.clipSource : 0xFFFFFFFF;
                    scanPath.push_back(pc);
                    if (src != 0xFFFFFFFF && scanLast.count(src))
                    {
                        // MAC flags: the snapshot holds the visible flags, so no stage may start between the write and
                        // its reader. Clip flags: the snapshot holds them as landed, which is what the reader sees until
                        // the next clip write (CLIP, FCSET) after the source; only a start after that one is wrong
                        size_t from = scanLast[src] + 1;
                        if (clipReader)
                        {
                            auto w = std::upper_bound(scanClipWrites.begin(), scanClipWrites.end(), scanLast[src]);
                            from = w == scanClipWrites.end() ? scanPath.size() : *w + 1;
                        }
                        for (size_t k = from; k < scanPath.size(); ++k)
                            scanSplitsFlags.insert(scanPath[k]);
                    }
                    const auto &dec = vu.getDecodedInstructionPairForPc(code.data(), 0x4000, nullptr, pc);
                    if (dec.upperUsage.writesClip || dec.lowerUsage.writesClip)
                        scanClipWrites.push_back(scanPath.size() - 1);
                    scanLast[pc] = scanPath.size() - 1;
                    scanPending[pc] |= pending(vu);
                    ++scanVisits[pc];
                    if (vu.m_state.branchPending)
                        scanDelay.insert(pc);
                }
                if (inside && stageExits.count(pc) && !vu.m_state.branchPending)
                {
                    const bool ok = clean(vu, &pendingExit);
                    uncleanExit += !ok;
                    snapshot(vu, mem.data(), outState);
                    finish(ok ? 0u : 2u);
                }
                if (!inside && pc == stageEntry && !vu.m_state.branchPending)
                {
                    entryClips.clear();
                    for (const auto &f : vu.m_flagPipeline)
                        if (f.valid && f.writesClip)
                            entryClips.insert(f.issueCycle);
                    const bool ok = clean(vu, &pendingEntry);
                    uncleanEntry += !ok;
                    flags = ok ? 0u : 1u;
                    snapshot(vu, mem.data(), in);
                    inside = true;
                    if (!hintsPath.empty())
                        hints.before(vu, pc, true);
                }
                if (inside)
                {
                    if (!entryClips.empty())
                    {
                        // a clip-flag reader while a CLIP from before the entry is still in flight reads older
                        // flags than the snapshot's (landed) ones: such a boundary cannot be judged
                        bool still = false;
                        for (const auto &f : vu.m_flagPipeline)
                            still |= f.valid && f.writesClip && entryClips.count(f.issueCycle);
                        if (!still)
                            entryClips.clear();
                        else if (vu.getDecodedInstructionPairForPc(code.data(), 0x4000, nullptr, pc).lowerUsage.readsClip)
                            ++earlyClipReads;
                    }
                    uint32_t lower;
                    std::memcpy(&lower, code.data() + pc, 4);
                    if ((lower & 0xFE0007FFu) == 0x800006FCu) // XGKICK is
                    {
                        const VU1State s = VU1InterpreterAccess::effectiveState(vu);
                        kicks.push_back(vuGifPacket(mem.data(), uint32_t(s.vi[(lower >> 11) & 0xF]) & 0x3FF));
                    }
                }
            };
            const bool ended = stepRun(vu, code.data(), mem.data(), *reinterpret_cast<GS *>(gsStorage), maxCycles, visit, [&](uint32_t pc) {
                uint32_t lower;
                std::memcpy(&lower, code.data() + pc, 4);
                hints.after(vu, pc, lower);
            });
            packetSink = nullptr;
            if (inside)
            {
                // the program ended inside the stage: its end is an exit too ([E] = exit pc 0xFFFFFFFF)
                snapshot(vu, mem.data(), outState);
                outState.pc = 0xFFFFFFFFu;
                ++unfinished;
                finish(ended ? 0u : 4u);
            }
            ++replayed;
            continue;
        }
        if (coverage)
        {
            static VU1Interpreter vu(VU1Interpreter::Unit::VU1);
            alignas(64) static unsigned char gsStorage[1 << 16];
            VU1InterpreterAccess::traceRestore(vu, state);
            std::vector<uint8_t> code = img->second, mem = data;
            std::set<uint32_t> seen;
            Vu1PacketSink sink;
            sink.forward = false;
            packetSink = &sink;
            stepRun(vu, code.data(), mem.data(), *reinterpret_cast<GS *>(gsStorage), maxCycles, [&](uint32_t pc) { seen.insert(pc); });
            packetSink = nullptr;
            for (uint32_t pc : seen)
                ++covered[pc];
            ++replayed;
            continue;
        }
        const Run run = replay(state, img->second, data, maxCycles);
        if (twice)
        {
            const Run again = replay(state, img->second, data, maxCycles);
            if (again.packets != run.packets || again.dataAfter != run.dataAfter)
                ++nondeterministic;
        }
        ++replayed;
        unended += !run.ended;
        packets += run.packets.size();
        const uint32_t head[2] = {0x4F523156u, uint32_t(index)};
        out.write(reinterpret_cast<const char *>(head), 8);
        out.write(reinterpret_cast<const char *>(&hash), 8);
        const uint32_t pcCount[2] = {state.state.pc, uint32_t(run.packets.size())};
        out.write(reinterpret_cast<const char *>(pcCount), 8);
        for (const auto &p : run.packets)
        {
            const uint32_t n = uint32_t(p.size());
            out.write(reinterpret_cast<const char *>(&n), 4);
            out.write(reinterpret_cast<const char *>(p.data()), n);
        }
        out.write(reinterpret_cast<const char *>(run.dataAfter.data()), 0x4000);
        out.write(reinterpret_cast<const char *>(run.vi), sizeof run.vi);
    }
    if (stageEntry != 0xFFFFFFFF)
        std::fprintf(stderr, "stage %x: %" PRIu64 " cases (%" PRIu64 " written), unclean entries %" PRIu64 ", unclean exits %" PRIu64
                             ", ended inside %" PRIu64 "\n",
                     stageEntry, cases, std::min<uint64_t>((cases + stride - 1) / stride, uint64_t(maxCases)), uncleanEntry, uncleanExit, unfinished);
    if (!cutscanPath.empty())
    {
        std::ofstream c(cutscanPath);
        for (const auto &[pc, bits] : scanPending)
        {
            c << std::hex << pc << ' ' << bits << ' ' << (scanDelay.count(pc) ? 1 : 0) << ' ' << (scanSplitsFlags.count(pc) ? 1 : 0)
              << std::dec << ' ' << scanVisits[pc] << '\n';
        }
    }
    if (stageEntry < 0xFFFFFFFE && earlyClipReads)
        std::fprintf(stderr, "  clip flags read before a CLIP in flight at the entry landed: %" PRIu64 " times (move the entry)\n",
                     earlyClipReads);
    for (int side = 0; side < 2 && stageEntry != 0xFFFFFFFF; ++side)
    {
        std::fprintf(stderr, "  in flight at the %s:", side ? "exits" : "entry");
        for (int b = 0; b < 11; ++b)
            if ((side ? pendingExit : pendingEntry) >> b & 1)
                std::fprintf(stderr, " %s%s", kPendingNames[b], (kHarmless >> b & 1) ? "" : "(!)");
        std::fprintf(stderr, "\n");
    }
    if (!hintsPath.empty())
    {
        std::ofstream h(hintsPath);
        for (const auto &[pc, srcs] : hints.q)
        {
            h << std::hex << pc << " q";
            for (uint32_t x : srcs)
                h << ' ' << (x == 0xFFFFFFFF ? std::string("entry") : (std::stringstream() << std::hex << x).str());
            h << " mac";
            for (uint32_t x : hints.mac[pc])
                h << ' ' << (x == 0xFFFFFFFF ? std::string("entry") : (std::stringstream() << std::hex << x).str());
            h << " clip";
            for (uint32_t x : hints.clip[pc])
                h << ' ' << (x == 0xFFFFFFFF ? std::string("entry") : (std::stringstream() << std::hex << x).str());
            if (hints.viBackup.count(pc))
            {
                h << " vibackup";
                for (uint32_t r : hints.viBackup[pc])
                    h << " vi" << std::dec << r;
            }
            h << '\n';
        }
    }
    for (const auto &[pc, n] : covered)
        out << std::hex << pc << ' ' << std::dec << n << '\n';
    std::fprintf(stderr, "%" PRIu64 " records, %" PRIu64 " replayed, %" PRIu64 " packets, %" PRIu64 " not ended within maxCycles, %" PRIu64
                         " nondeterministic\n",
                 records, replayed, packets, unended, nondeterministic);
    for (const auto &[h, n] : perImage)
        std::fprintf(stderr, "  image %016llx: %" PRIu64 " runs\n", (unsigned long long)h, n);
    return 0;
}
