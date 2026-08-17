/*
 * test_relaunch.cpp - Launch, quit and relaunch the VM in one process
 *
 * This is the sequence the app performs when the user quits an image and
 * opens another one: vm_initialize → vm_loadImage → vm_run → vm_stop →
 * vm_destroy, then all of it again. Relaunch has broken several times in
 * ways a single-launch test cannot see, because the failures come from
 * global state that survives vm_destroy — a static flag that was never
 * reset, a plugin thread still holding stale pointers, an event queue that
 * stayed disabled.
 *
 * Each cycle is judged on three things:
 *
 *   1. vm_isRunning() is true after vm_run and still true at the end. The
 *      build-118 bug was gRunning surviving vm_destroy, so the second
 *      vm_run() returned immediately and the VM was simply dead.
 *   2. The interpreter advances bytecodes. Running is not the same as
 *      making progress.
 *   3. Posted input events get consumed. The build-122 bug was a static
 *      flag in stub_SDL_PollEvent that vm_destroy could not reach, so the
 *      relaunched image displayed but silently dropped every event. A
 *      queue that never drains is that failure.
 *
 * On this branch each cycle also builds and tears down a JIT code zone and
 * method map, so the loop doubles as a check that JIT state does not
 * outlive its Interpreter.
 *
 * This harness cannot check rendering: the display form never becomes
 * ready without the app's startup.st, and test_platform sees the same
 * thing on a first launch. Rendering is verified from the app instead.
 *
 * Usage: test_relaunch <image-path> [cycles] [seconds-per-cycle]
 */

#include "PlatformBridge.h"
#include "EventQueue.hpp"
#include <CoreFoundation/CoreFoundation.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

// Bytecode counter published by the interpreter's watchdog (Interpreter.cpp).
extern std::atomic<long long> g_watchdogSteps;

namespace {

// Bytecodes a healthy cycle runs in its first second. Startup alone is
// millions, so this only has to be far enough above zero to tell "running"
// from "stalled".
constexpr long long kMinStepsPerCycle = 100000;

// Virtual-address ceiling for old space. This is NOT a heap size: old space
// starts at oldSpaceInitialSize (128 MB by default) and grows inside this
// reservation, so the number only has to be comfortably above what a boot
// needs. A ceiling low enough to be hit would fail the cycle for reasons
// that have nothing to do with relaunch.
constexpr size_t kMaxHeap = 1024ULL * 1024 * 1024;

struct CycleResult {
    bool started = false;
    bool ranToEnd = false;
    long long steps = 0;
    size_t eventsLeftQueued = 0;
    bool eventsDrained = false;
    bool stoppedCleanly = false;
};

// Pump the main runloop instead of parking main in a sleep.
//
// The image's SDL/AppKit init reaches -[NSMenu insertItemWithTitle:action:
// keyEquivalent:atIndex:] through FFI on the VM thread.  AppKit posts a
// notification from there that ends in -[NSOperation waitUntilFinished]
// against the main thread.  swizzleCatalystAppKit() no-ops that call only
// under TARGET_OS_MACCATALYST, so in a plain macOS CLI harness the real one
// runs -- and if main is asleep the VM thread blocks in __psynch_cvwait
// forever: no bytecodes, no event drain, and interpret() can no longer
// return, which would strand the parked worker and make every cycle after
// the first execute nothing at all.  The app's main thread pumps a runloop,
// so this harness does too.
static void pumpMainRunLoop(double seconds) {
    auto end = std::chrono::steady_clock::now() +
               std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);
    }
}

// Post a few mouse events and give the image a moment to take them.
void probeEventDelivery(CycleResult& result) {
    for (int i = 0; i < 5; ++i) {
        vm_postMouseEvent(1, 100 + i, 100 + i, 0, 0);  // move
    }
    for (int waited = 0; waited < 30; ++waited) {
        if (pharo::gEventQueue.isEmpty()) break;
        pumpMainRunLoop(0.1);
    }
    result.eventsLeftQueued = pharo::gEventQueue.size();
    result.eventsDrained = (result.eventsLeftQueued == 0);
}

CycleResult runOneCycle(const char* imagePath, int seconds) {
    CycleResult result;
    const int width = 1024, height = 768;

    if (!vm_initialize_with_config(kMaxHeap, 0)) {
        std::cerr << "  vm_initialize failed" << std::endl;
        return result;
    }

    vm_setDisplaySize(width, height, 32);

    if (!vm_loadImage(imagePath)) {
        std::cerr << "  vm_loadImage failed" << std::endl;
        vm_destroy();
        return result;
    }

    long long stepsBefore = g_watchdogSteps.load(std::memory_order_relaxed);
    vm_run();
    result.started = vm_isRunning();

    // Let the image boot far enough to be polling for input.
    pumpMainRunLoop(static_cast<double>(seconds));

    probeEventDelivery(result);

    result.ranToEnd = vm_isRunning();
    // The counter is not reset between cycles, so measure the delta. A cycle
    // that never ran leaves it exactly where the previous one stopped.
    result.steps = g_watchdogSteps.load(std::memory_order_relaxed) - stepsBefore;

    vm_stop();
    // The parked worker is only safe to hand another image if interpret()
    // actually returned.  If vm_stop times out with gRunning still set, the
    // worker is stuck inside this image and the next launch is dead -- the
    // exact failure one reused thread introduces and thread-per-launch hides.
    result.stoppedCleanly = !vm_isRunning();
    vm_destroy();
    return result;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: " << argv[0]
                  << " <image-path> [cycles] [seconds-per-cycle]" << std::endl;
        return 1;
    }

    const char* imagePath = argv[1];
    const int cycles = (argc >= 3) ? std::atoi(argv[2]) : 3;
    const int seconds = (argc >= 4) ? std::atoi(argv[3]) : 8;

    std::cout << "Relaunch test: " << cycles << " cycles of " << seconds
              << "s on " << imagePath << std::endl;

    std::vector<CycleResult> results;
    for (int c = 1; c <= cycles; ++c) {
        std::cout << "\n=== Cycle " << c << " of " << cycles << " ===" << std::endl;
        CycleResult r = runOneCycle(imagePath, seconds);
        std::cout << "  started=" << (r.started ? "yes" : "no")
                  << " stillRunning=" << (r.ranToEnd ? "yes" : "no")
                  << " steps=" << r.steps
                  << " eventsQueued=" << r.eventsLeftQueued << std::endl;
        results.push_back(r);
    }

    std::cout << "\n=== Summary ===" << std::endl;
    int failures = 0;
    for (size_t i = 0; i < results.size(); ++i) {
        const CycleResult& r = results[i];
        bool ok = r.started && r.ranToEnd && r.stoppedCleanly &&
                  r.steps >= kMinStepsPerCycle && r.eventsDrained;
        if (!ok) failures++;
        std::cout << "  cycle " << (i + 1) << ": " << (ok ? "PASS" : "FAIL");
        if (!ok) {
            if (!r.started) std::cout << " (never started)";
            else if (!r.ranToEnd) std::cout << " (stopped early)";
            else if (!r.stoppedCleanly) std::cout << " (vm_stop timed out; worker stuck)";
            else if (r.steps < kMinStepsPerCycle) std::cout << " (no bytecode progress)";
            else std::cout << " (input events not consumed)";
        }
        std::cout << std::endl;
    }

    if (failures != 0) {
        std::cout << failures << " of " << results.size() << " cycles failed" << std::endl;
        return 1;
    }
    std::cout << "All " << results.size() << " cycles passed" << std::endl;
    return 0;
}
