// Profiler.cpp - sampling profiler (SIGPROF on POSIX, sampler thread on Windows).
//
// POSIX: enables setitimer(ITIMER_PROF, ...) to fire SIGPROF every N us of
// process CPU time.  The signal handler reads `g_profilingInterp->method_`
// and bumps a per-method counter.  At process exit, dumps the top-N
// methods by sample count with their selector names.
//
// Why ITIMER_PROF (not ITIMER_REAL or ITIMER_VIRTUAL): we want CPU time
// (user + sys), not wall time, so we don't sample during sleep/idle.
// Matches our other CPU-time measurement (primitive 247).
//
// Windows: no SIGPROF/setitimer.  A sampler THREAD wakes every interval,
// reads the VM thread's consumed CPU time via GetThreadTimes, and only
// records a sample when CPU time advanced by at least half an interval —
// emulating ITIMER_PROF's don't-sample-while-idle semantics.  The sampled
// read (method_, one aligned 64-bit load) is exactly the same tolerated
// race as the POSIX signal handler: the sample captures either the old or
// the new method, both fine.
//
// PHARO_PROFILE=1 enables.  PHARO_PROFILE_INTERVAL_US=<n> sets the
// sample interval (default 1000us = 1ms).  PHARO_PROFILE_TOP=<n> sets
// the dump size (default 30).
//
// Signal-handler async safety: the handler only reads the interpreter's
// `method_` field (an Oop = uint64_t) and writes into a fixed-size
// open-addressing table.  No memory allocation, no I/O, no locks.
// Brief window of inconsistency if the signal lands mid-write of
// method_ — sample either captures old or new value, both fine.

#include "Profiler.hpp"
#include "Interpreter.hpp"
#include "ObjectMemory.hpp"
#include "DebugSettings.hpp"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#include <thread>
#include <chrono>
#else
#include <sys/time.h>   // setitimer/itimerval — POSIX-only
#endif
#include <unistd.h>
#include <vector>

namespace pharo {

namespace {

constexpr size_t kProfileTableSize = 16384;
constexpr size_t kProbeLimit = 32;

struct ProfileSlot {
    std::atomic<uint64_t> methodBits;
    std::atomic<uint32_t> count;
};

static ProfileSlot g_profileTable[kProfileTableSize];
static std::atomic<uint64_t> g_overflowCount{0};
static std::atomic<uint64_t> g_totalSamples{0};
static std::atomic<uint64_t> g_droppedNoInterp{0};
Interpreter* g_profilingInterp = nullptr;
bool g_profilerActive = false;

// Record one sample of the active method.  Shared by the POSIX SIGPROF
// handler and the Windows sampler thread; async-signal-safe (no
// allocation, no I/O, no locks — fixed-size open-addressing table).
void recordSample() {
    Interpreter* interp = g_profilingInterp;
    if (!interp) { g_droppedNoInterp.fetch_add(1, std::memory_order_relaxed); return; }
    uint64_t mb = interp->activeMethod().rawBits();
    if (mb == 0) { g_droppedNoInterp.fetch_add(1, std::memory_order_relaxed); return; }

    g_totalSamples.fetch_add(1, std::memory_order_relaxed);

    // Open-addressing linear probe.  Use a simple multiplicative hash.
    uint64_t h = (mb * 11400714819323198485ULL) >> 32;
    for (size_t i = 0; i < kProbeLimit; i++) {
        size_t idx = (h + i) & (kProfileTableSize - 1);
        uint64_t existing = g_profileTable[idx].methodBits.load(
            std::memory_order_relaxed);
        if (existing == mb) {
            g_profileTable[idx].count.fetch_add(
                1, std::memory_order_relaxed);
            return;
        }
        if (existing == 0) {
            // Try to claim this slot.  CAS in case another sampler
            // landed at the same time (unlikely but possible on SMP).
            uint64_t expected = 0;
            if (g_profileTable[idx].methodBits.compare_exchange_strong(
                    expected, mb, std::memory_order_relaxed)) {
                g_profileTable[idx].count.store(
                    1, std::memory_order_relaxed);
                return;
            }
            // Lost the race — retry this slot.
            if (g_profileTable[idx].methodBits.load(
                    std::memory_order_relaxed) == mb) {
                g_profileTable[idx].count.fetch_add(
                    1, std::memory_order_relaxed);
                return;
            }
            // Otherwise fall through to next probe slot.
        }
    }
    g_overflowCount.fetch_add(1, std::memory_order_relaxed);
}

#ifndef _WIN32
void sigprofHandler(int /*sig*/) {
    recordSample();
}
#else
// Windows sampler thread state.
std::thread* g_samplerThread = nullptr;
std::atomic<bool> g_samplerStop{false};
HANDLE g_vmThreadHandle = nullptr;

uint64_t vmThreadCpu100ns() {
    FILETIME creation, exit_, kernel, user;
    if (!GetThreadTimes(g_vmThreadHandle, &creation, &exit_, &kernel, &user))
        return 0;
    ULARGE_INTEGER k, u;
    k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;   u.HighPart = user.dwHighDateTime;
    return k.QuadPart + u.QuadPart;  // 100ns units, user+kernel like ITIMER_PROF
}

void samplerLoop(int intervalUs) {
    uint64_t lastCpu = vmThreadCpu100ns();
    const uint64_t minCpuDelta100ns =
        static_cast<uint64_t>(intervalUs) * 10 / 2;  // >= half an interval of CPU
    while (!g_samplerStop.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::microseconds(intervalUs));
        uint64_t cpu = vmThreadCpu100ns();
        if (cpu - lastCpu >= minCpuDelta100ns) {
            recordSample();
        }
        lastCpu = cpu;
    }
}
#endif  // _WIN32

}  // namespace

void Profiler::enable(Interpreter* interp) {
    if (g_profilerActive) return;
    g_profilingInterp = interp;

    int us = g_debug.profileIntervalUs > 0 ? g_debug.profileIntervalUs : 1000;

#ifdef _WIN32
    // enable() runs on the VM thread — capture a real handle to it for
    // GetThreadTimes (GetCurrentThread() is a pseudo-handle that would
    // resolve to the SAMPLER thread inside samplerLoop).
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                         GetCurrentProcess(), &g_vmThreadHandle,
                         0, FALSE, DUPLICATE_SAME_ACCESS)) {
        fprintf(stderr, "[PROFILE] DuplicateHandle failed — profiler disabled\n");
        return;
    }
    g_samplerStop.store(false, std::memory_order_release);
    g_samplerThread = new std::thread(samplerLoop, us);
    g_profilerActive = true;
    fprintf(stderr, "[PROFILE] enabled (Windows sampler thread), interval=%dus\n", us);
#else
    // Install signal handler.  SA_RESTART = restart interrupted syscalls.
    struct sigaction sa{};
    sa.sa_handler = sigprofHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGPROF, &sa, nullptr);

    // Sample interval (microseconds).  Default 1ms.
    struct itimerval timer{};
    timer.it_interval.tv_sec = us / 1'000'000;
    timer.it_interval.tv_usec = us % 1'000'000;
    timer.it_value = timer.it_interval;
    setitimer(ITIMER_PROF, &timer, nullptr);

    g_profilerActive = true;
    fprintf(stderr, "[PROFILE] enabled, interval=%dus\n", us);
#endif
}

void Profiler::dump() {
    if (!g_profilerActive) return;

    // Disable sampling first.
#ifdef _WIN32
    g_samplerStop.store(true, std::memory_order_release);
    if (g_samplerThread) {
        g_samplerThread->join();
        delete g_samplerThread;
        g_samplerThread = nullptr;
    }
    if (g_vmThreadHandle) {
        CloseHandle(g_vmThreadHandle);
        g_vmThreadHandle = nullptr;
    }
#else
    struct itimerval timer{};
    setitimer(ITIMER_PROF, &timer, nullptr);

    // Reset signal disposition.
    struct sigaction sa{};
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPROF, &sa, nullptr);
#endif
    g_profilerActive = false;

    // Collect non-empty entries.
    struct Entry { uint64_t methodBits; uint32_t count; };
    std::vector<Entry> entries;
    entries.reserve(kProfileTableSize);
    for (size_t i = 0; i < kProfileTableSize; i++) {
        uint64_t mb = g_profileTable[i].methodBits.load(
            std::memory_order_relaxed);
        if (mb == 0) continue;
        uint32_t c = g_profileTable[i].count.load(
            std::memory_order_relaxed);
        if (c == 0) continue;
        entries.push_back({mb, c});
    }

    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) {
                  return a.count > b.count;
              });

    int topN = g_debug.profileTopN > 0 ? g_debug.profileTopN : 30;

    uint64_t total = g_totalSamples.load();
    uint64_t overflow = g_overflowCount.load();
    uint64_t dropped = g_droppedNoInterp.load();
    fprintf(stderr,
        "\n[PROFILE] total=%llu samples, overflow=%llu, dropped=%llu, "
        "unique=%zu, showing top %d:\n",
        (unsigned long long)total,
        (unsigned long long)overflow,
        (unsigned long long)dropped,
        entries.size(), topN);
    fprintf(stderr, "    %%   count  class>>selector\n");
    fprintf(stderr, "  ----  -----  ---------------\n");

    // Try to print class>>selector.  At main()'s end (before
    // destructors) memory_ is still valid.  Guard with isValidPointer
    // against GC-moved oops.
    Interpreter* interp = g_profilingInterp;
    auto& mem = interp->memory();
    int shown = 0;
    for (const Entry& e : entries) {
        if (shown >= topN) break;
        Oop methodOop = Oop::fromRawBits(e.methodBits);
        std::string sel = "?";
        std::string cls = "?";
        if (methodOop.isObject() && methodOop.rawBits() > 0x10000
            && mem.isValidPointer(methodOop)) {
            sel = mem.selectorOf(methodOop);
            // Try to get class via method's last-literal-1 (the
            // associated class) — for AdditionalMethodState methods
            // this lives at slot [numLits-2].
            size_t nLits = mem.numLiteralsOf(methodOop);
            if (nLits >= 2) {
                Oop maybeAssn = mem.fetchPointer(nLits - 2, methodOop);
                // Could be an Association(class -> nil) or just a class.
                if (maybeAssn.isObject()
                    && maybeAssn.rawBits() > 0x10000
                    && mem.isValidPointer(maybeAssn)) {
                    ObjectHeader* h = maybeAssn.asObjectPtr();
                    if (h->slotCount() >= 1) {
                        Oop slot0 = h->slotAt(0);
                        if (slot0.isObject() && slot0.rawBits() > 0x10000
                            && mem.isValidPointer(slot0)) {
                            cls = mem.classNameOf(slot0);
                        }
                    }
                }
            }
        }
        double pct = (double)e.count * 100.0 / (double)total;
        fprintf(stderr, "  %4.1f  %5u  %s>>%s\n",
                pct, e.count, cls.c_str(), sel.c_str());
        shown++;
    }
    fflush(stderr);
}

}  // namespace pharo
