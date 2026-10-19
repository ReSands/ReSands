// Sampling profiler for the recompiled game (debug aid, off by default). A thread suspends every
// other thread of the process about every millisecond, records its call stack, and at the end writes
// the counts per stack and thread. Symbolize with tools/profsym.py.
//
// Environment: POP_PROFILE=<out file>:<start seconds>:<duration seconds>, e.g.
//   POP_PROFILE=prof.txt:20:30   (samples from 20 s to 50 s after start)
// The out file is split at the last two colons, so Windows paths with a drive letter work.
// POP_PROFILE_LAST=<seconds>: rolling mode, sampling all the time and keeping the last <seconds>; every
// kept quick-save (states/play_<date>_<time>.state) also writes the profile of those seconds next to it
// as play_<date>_<time>.prof (pop_profiler::dumpLast, called from keepQuickSaves). play.ps1 turns it on.
//
// Stacks: Windows x64 unwinds with the image's unwind tables (RtlVirtualUnwind); macOS arm64
// follows the frame-pointer chain. Nothing is allocated while a thread is suspended (it may hold
// the heap lock). Other platforms: no-op.
//
// Output (text): "image <load address hex> <path>", (Windows) "module <base> <size> <name>" per loaded
// module, "thread <index> <samples> <name>", then one
// line per distinct stack: "s <thread index> <count> <pc> <caller> ..." (absolute addresses, hex).

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#elif defined(__APPLE__) && defined(__aarch64__)
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <pthread.h>
#endif

namespace
{
    constexpr int kMaxDepth = 24;

    struct Sample
    {
        uint64_t pc[kMaxDepth];
        int depth = 0;
    };

    struct ThreadInfo
    {
        std::string name;
        uint64_t samples = 0;
        std::map<std::vector<uint64_t>, uint64_t> stacks;
    };

#if defined(_WIN32)
    using ThreadKey = DWORD;

    bool sampleThread(ThreadKey tid, Sample &out)
    {
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
        if (!h)
            return false;
        bool ok = false;
        if (SuspendThread(h) != static_cast<DWORD>(-1))
        {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(h, &ctx))
            {
                out.depth = 0;
                for (int d = 0; d < kMaxDepth && ctx.Rip; ++d)
                {
                    out.pc[out.depth++] = ctx.Rip;
                    DWORD64 imageBase = 0;
                    PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
                    if (!fe)
                    {
                        // Leaf function: the return address is on top of the stack.
                        if (!ctx.Rsp)
                            break;
                        ctx.Rip = *reinterpret_cast<DWORD64 *>(ctx.Rsp);
                        ctx.Rsp += 8;
                        continue;
                    }
                    PVOID handlerData = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fe, &ctx, &handlerData, &establisher, nullptr);
                }
                ok = out.depth > 0;
            }
            ResumeThread(h);
        }
        CloseHandle(h);
        return ok;
    }

    std::vector<ThreadKey> listThreads()
    {
        std::vector<ThreadKey> ids;
        const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return ids;
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        for (BOOL more = Thread32First(snap, &te); more; more = Thread32Next(snap, &te))
            if (te.th32OwnerProcessID == pid && te.th32ThreadID != self)
                ids.push_back(te.th32ThreadID);
        CloseHandle(snap);
        return ids;
    }

    std::string threadName(ThreadKey tid)
    {
        std::string name = "tid" + std::to_string(tid);
        using GetDescFn = HRESULT(WINAPI *)(HANDLE, PWSTR *);
        static GetDescFn getDesc = reinterpret_cast<GetDescFn>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
        if (h && getDesc)
        {
            PWSTR desc = nullptr;
            if (SUCCEEDED(getDesc(h, &desc)) && desc)
            {
                char buf[128];
                const int n = WideCharToMultiByte(CP_UTF8, 0, desc, -1, buf, sizeof(buf), nullptr, nullptr);
                if (n > 1)
                    name = std::string(buf) + "/" + std::to_string(tid);
                LocalFree(desc);
            }
        }
        if (h)
            CloseHandle(h);
        return name;
    }

    std::string imageLine()
    {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        char line[MAX_PATH + 32];
        std::snprintf(line, sizeof(line), "image %llx %s", static_cast<unsigned long long>(
                      reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))), path);
        return line;
    }

    // "module <base> <size> <name>" for every loaded module, so samples outside the executable can be
    // told apart (kernel waits in ntdll, the GL driver, the C runtime's memcpy, ...).
    std::string moduleLines()
    {
        std::string out;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
        if (snap == INVALID_HANDLE_VALUE)
            return out;
        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me))
        {
            char name[MAX_PATH] = {};
            WideCharToMultiByte(CP_UTF8, 0, me.szModule, -1, name, sizeof(name), nullptr, nullptr);
            char line[MAX_PATH + 64];
            std::snprintf(line, sizeof(line), "module %llx %lx %s\n",
                          static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(me.modBaseAddr)),
                          static_cast<unsigned long>(me.modBaseSize), name);
            out += line;
        }
        CloseHandle(snap);
        return out;
    }
#elif defined(__APPLE__) && defined(__aarch64__)
    using ThreadKey = thread_act_t;

    bool sampleThread(ThreadKey th, Sample &out)
    {
        if (thread_suspend(th) != KERN_SUCCESS)
            return false;
        arm_thread_state64_t st{};
        mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
        bool ok = false;
        if (thread_get_state(th, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&st), &count) == KERN_SUCCESS)
        {
            out.depth = 0;
            out.pc[out.depth++] = arm_thread_state64_get_pc(st);
            uint64_t lr = arm_thread_state64_get_lr(st), fp = arm_thread_state64_get_fp(st);
            if (lr && out.depth < kMaxDepth)
                out.pc[out.depth++] = lr; // the leaf may not have pushed a frame yet
            for (int d = 0; d < kMaxDepth && fp && (fp & 0xF) == 0 && out.depth < kMaxDepth; ++d)
            {
                vm_size_t got = 0;
                uint64_t frame[2] = {};
                if (vm_read_overwrite(mach_task_self(), fp, sizeof(frame), reinterpret_cast<vm_address_t>(frame), &got) != KERN_SUCCESS)
                    break;
                if (!frame[1] || frame[0] <= fp)
                    break;
                if (frame[1] != out.pc[out.depth - 1])
                    out.pc[out.depth++] = frame[1];
                fp = frame[0];
            }
            ok = true;
        }
        thread_resume(th);
        return ok;
    }

    std::vector<ThreadKey> listThreads()
    {
        std::vector<ThreadKey> ids;
        thread_act_array_t list = nullptr;
        mach_msg_type_number_t n = 0;
        if (task_threads(mach_task_self(), &list, &n) != KERN_SUCCESS)
            return ids;
        const thread_act_t self = mach_thread_self();
        for (mach_msg_type_number_t i = 0; i < n; ++i)
        {
            if (list[i] != self)
                ids.push_back(list[i]);
            else
                mach_port_deallocate(mach_task_self(), list[i]);
        }
        mach_port_deallocate(mach_task_self(), self);
        vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(list), n * sizeof(thread_act_t));
        return ids;
    }

    std::string threadName(ThreadKey th)
    {
        std::string name = "port" + std::to_string(th);
        if (pthread_t p = pthread_from_mach_thread_np(th))
        {
            char buf[64] = {};
            if (pthread_getname_np(p, buf, sizeof(buf)) == 0 && buf[0])
                name = std::string(buf) + "/" + std::to_string(th);
        }
        return name;
    }

    std::string imageLine()
    {
        char line[1100];
        std::snprintf(line, sizeof(line), "image %llx %s",
                      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(_dyld_get_image_header(0))),
                      _dyld_get_image_name(0));
        return line;
    }
#endif

#if defined(_WIN32) || (defined(__APPLE__) && defined(__aarch64__))
    void writeProfile(const std::string &path, const std::map<ThreadKey, ThreadInfo> &threads, uint64_t rounds,
                      double seconds)
    {
        FILE *f = std::fopen(path.c_str(), "w");
        if (!f)
        {
            std::fprintf(stderr, "[pop:profile] cannot write %s\n", path.c_str());
            return;
        }
        std::fprintf(f, "# pop profile v1, %llu rounds over %.1f s\n%s\n", static_cast<unsigned long long>(rounds),
                     seconds, imageLine().c_str());
#if defined(_WIN32)
        std::fputs(moduleLines().c_str(), f);
#endif
        int index = 0;
        for (auto &[id, info] : threads)
        {
            std::fprintf(f, "thread %d %llu %s\n", index, static_cast<unsigned long long>(info.samples), info.name.c_str());
            for (auto &[stack, count] : info.stacks)
            {
                std::fprintf(f, "s %d %llu", index, static_cast<unsigned long long>(count));
                for (uint64_t pc : stack)
                    std::fprintf(f, " %llx", static_cast<unsigned long long>(pc));
                std::fputc('\n', f);
            }
            ++index;
        }
        std::fclose(f);
        std::fprintf(stderr, "[pop:profile] wrote %s (%llu rounds, %zu threads)\n", path.c_str(),
                     static_cast<unsigned long long>(rounds), threads.size());
    }

    void run(std::string path, double startSeconds, double seconds)
    {
        std::this_thread::sleep_for(std::chrono::duration<double>(startSeconds));
        std::fprintf(stderr, "[pop:profile] sampling for %.0f s\n", seconds);
        std::map<ThreadKey, ThreadInfo> threads;
        std::vector<ThreadKey> ids;
        Sample sample;
        const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
        auto refresh = std::chrono::steady_clock::time_point{};
        uint64_t rounds = 0;
        while (std::chrono::steady_clock::now() < end)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now >= refresh)
            {
#if defined(__APPLE__)
                for (ThreadKey id : ids)
                    mach_port_deallocate(mach_task_self(), id);
#endif
                ids = listThreads();
                for (ThreadKey id : ids)
                    if (!threads.count(id))
                        threads[id].name = threadName(id);
                refresh = now + std::chrono::seconds(1);
            }
            for (ThreadKey id : ids)
            {
                if (!sampleThread(id, sample))
                    continue;
                ThreadInfo &info = threads[id];
                ++info.samples;
                ++info.stacks[std::vector<uint64_t>(sample.pc, sample.pc + sample.depth)];
            }
            ++rounds;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        writeProfile(path, threads, rounds, seconds);
    }

    // Rolling mode (POP_PROFILE_LAST=<seconds>): samples all the time into one-second slices and keeps the
    // last <seconds> of them; dumpLast() (called for every kept quick-save) writes those slices as one
    // profile, so a dip noticed just after it happened can be saved with one button press.
    struct Slice
    {
        uint64_t rounds = 0;
        std::map<ThreadKey, ThreadInfo> threads; // names are added when a dump merges the slices
    };
    std::mutex g_rollMutex;
    std::vector<std::string> g_dumpRequests;

    void rolling(int keepSeconds)
    {
        std::fprintf(stderr, "[pop:profile] rolling: keeping the last %d s for every kept quick-save\n", keepSeconds);
        std::deque<Slice> slices(1);
        std::map<ThreadKey, std::string> names;
        std::vector<ThreadKey> ids;
        Sample sample;
        auto sliceEnd = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        for (;;)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now >= sliceEnd)
            {
#if defined(__APPLE__)
                for (ThreadKey id : ids)
                    mach_port_deallocate(mach_task_self(), id);
#endif
                ids = listThreads();
                for (ThreadKey id : ids)
                    if (!names.count(id))
                        names[id] = threadName(id);
                slices.emplace_back();
                while (static_cast<int>(slices.size()) > keepSeconds)
                    slices.pop_front();
                sliceEnd = now + std::chrono::seconds(1);

                std::vector<std::string> requests;
                {
                    std::lock_guard<std::mutex> lock(g_rollMutex);
                    requests.swap(g_dumpRequests);
                }
                for (const std::string &path : requests)
                {
                    std::map<ThreadKey, ThreadInfo> merged;
                    uint64_t rounds = 0;
                    for (const Slice &sl : slices)
                    {
                        rounds += sl.rounds;
                        for (auto &[id, info] : sl.threads)
                        {
                            ThreadInfo &m = merged[id];
                            m.name = names[id];
                            m.samples += info.samples;
                            for (auto &[stack, count] : info.stacks)
                                m.stacks[stack] += count;
                        }
                    }
                    writeProfile(path, merged, rounds, static_cast<double>(slices.size()));
                }
            }
            Slice &cur = slices.back();
            for (ThreadKey id : ids)
            {
                if (!sampleThread(id, sample))
                    continue;
                ThreadInfo &info = cur.threads[id];
                ++info.samples;
                ++info.stacks[std::vector<uint64_t>(sample.pc, sample.pc + sample.depth)];
            }
            ++cur.rounds;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    struct Starter
    {
        Starter()
        {
            if (const char *last = std::getenv("POP_PROFILE_LAST"); last && std::atoi(last) > 0)
                std::thread(rolling, std::atoi(last)).detach();
            const char *spec = std::getenv("POP_PROFILE");
            if (!spec || !*spec)
                return;
            const std::string s(spec);
            const size_t c2 = s.rfind(':'), c1 = c2 == std::string::npos ? c2 : s.rfind(':', c2 - 1);
            if (c1 == std::string::npos || c1 == 0)
            {
                std::fprintf(stderr, "[pop:profile] POP_PROFILE wants <file>:<start s>:<seconds>\n");
                return;
            }
            std::thread(run, s.substr(0, c1), std::atof(s.c_str() + c1 + 1), std::atof(s.c_str() + c2 + 1)).detach();
        }
    } g_starter;
#endif
}

namespace pop_profiler
{
    // Writes the rolling profile (POP_PROFILE_LAST) to <path> at the next one-second slice boundary; a
    // no-op when rolling mode is off or the platform has no sampler.
    void dumpLast(const std::string &path)
    {
#if defined(_WIN32) || (defined(__APPLE__) && defined(__aarch64__))
        static const bool on = [] { const char *v = std::getenv("POP_PROFILE_LAST"); return v && std::atoi(v) > 0; }();
        if (!on)
            return;
        std::lock_guard<std::mutex> lock(g_rollMutex);
        g_dumpRequests.push_back(path);
#else
        (void)path;
#endif
    }
}
