// Profiler.cpp - SIGPROF-based sampling profiler.
//
// Enables setitimer(ITIMER_PROF, ...) to fire SIGPROF every N us of
// process CPU time.  The signal handler reads `g_profilingInterp->method_`
// and bumps a per-method counter.  At process exit, dumps the top-N
// methods by sample count with their selector names.
//
// Why ITIMER_PROF (not ITIMER_REAL or ITIMER_VIRTUAL): we want CPU time
// (user + sys), not wall time, so we don't sample during sleep/idle.
// Matches our other CPU-time measurement (primitive 247).
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
#include <sys/time.h>
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

void sigprofHandler(int /*sig*/) {
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
            // Try to claim this slot.  CAS in case another signal
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

}  // namespace

void Profiler::enable(Interpreter* interp) {
    if (g_profilerActive) return;
    g_profilingInterp = interp;

    // Install signal handler.  SA_RESTART = restart interrupted syscalls.
    struct sigaction sa{};
    sa.sa_handler = sigprofHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGPROF, &sa, nullptr);

    // Sample interval (microseconds).  Default 1ms.
    int us = g_debug.profileIntervalUs > 0 ? g_debug.profileIntervalUs : 1000;
    struct itimerval timer{};
    timer.it_interval.tv_sec = us / 1'000'000;
    timer.it_interval.tv_usec = us % 1'000'000;
    timer.it_value = timer.it_interval;
    setitimer(ITIMER_PROF, &timer, nullptr);

    g_profilerActive = true;
    fprintf(stderr, "[PROFILE] enabled, interval=%dus\n", us);
}

void Profiler::dump() {
    if (!g_profilerActive) return;

    // Disable timer first.
    struct itimerval timer{};
    setitimer(ITIMER_PROF, &timer, nullptr);

    // Reset signal disposition.
    struct sigaction sa{};
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPROF, &sa, nullptr);
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

    // Try to print selectors.  At main()'s end (before destructors)
    // memory_ is still valid.  Guard against GC-moved oops by
    // checking the oop is in the heap range and the header looks
    // like a CompiledMethod.
    Interpreter* interp = g_profilingInterp;
    auto& mem = interp->memory();
    int shown = 0;
    for (const Entry& e : entries) {
        if (shown >= topN) break;
        Oop methodOop = Oop::fromRawBits(e.methodBits);
        std::string sel = "?";
        if (methodOop.isObject() && methodOop.rawBits() > 0x10000) {
            if (mem.isValidPointer(methodOop)) {
                sel = mem.selectorOf(methodOop);
            }
        }
        double pct = (double)e.count * 100.0 / (double)total;
        fprintf(stderr, "  %4.1f  %5u  0x%llx  %s\n",
                pct, e.count,
                (unsigned long long)e.methodBits, sel.c_str());
        shown++;
    }
    fflush(stderr);
}

}  // namespace pharo
