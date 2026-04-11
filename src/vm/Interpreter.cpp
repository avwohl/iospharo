/*
 * Interpreter.cpp - Bytecode Interpreter Implementation
 *
 * This implements the Sista V1 bytecode interpreter.
 *
 * Copyright (c) 2025-2026 Aaron Wohl. Licensed under the MIT License.
 *
 * This is a clean C++ reimplementation based on the architecture, bytecode
 * specification, and algorithms defined by the Pharo project (https://pharo.org)
 * and its predecessors (Squeak, OpenSmalltalk-VM). The Pharo VM source
 * (VMMaker/CoInterpreter) served as the authoritative reference.
 * See THIRD_PARTY_LICENSES for upstream license details.
 */

#include "Interpreter.hpp"
#include "InterpreterProxy.h"
#include "FFI.hpp"
#include "jit/TrampolineAsm.hpp"
#include "plugins/sqMemoryAccess.h"
#include "../include/vmCallback.h"
#include "../platform/DisplaySurface.hpp"
#include "../platform/EventQueue.hpp"
#include <cstring>
#include <cmath>
#include <csetjmp>
#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <set>
#include <vector>
#include <algorithm>
#include <atomic>
#include <dlfcn.h>
#include <pthread.h>

// Flag set by FFI.cpp when Emergency Debugger window is created
extern std::atomic<bool> g_emergencyDebuggerTriggered;

#if __APPLE__
#include <CoreFoundation/CFRunLoop.h>
// Undefine Objective-C's nil macro to avoid conflict with Oop::nil()
#undef nil
#endif

// SIGSEGV recovery support - used by executeFromContext, handler in test_load_image.cpp
sigjmp_buf g_sigsegvRecovery;

// Watchdog step counter — written by interpret(), read by heartbeat thread
std::atomic<long long> g_watchdogSteps{0};
// Watchdog phase tracker: 0=idle, 1=in step(), 2=in GC, 3=in processInputEvents, 4=in syncDisplay
std::atomic<int> g_watchdogPhase{0};
// Sub-phase inside step(): 10=GC, 11=timer, 12=finalization, 13=displaySync, 14=forceYield, 15=dispatch, 16=preempt
volatile int g_watchdogSubphase = 0;
volatile uint8_t g_watchdogLastBytecode = 0;

// Forward declaration for SDL rendering active check (defined in FFI.cpp)
extern "C" bool ffi_isSDLRenderingActive();

// Display Form readiness flag — exposed to Swift via vm_isDisplayFormReady().
// Set true when the image calls primitiveBeDisplay (prim 102) or
// primitiveForceDisplayUpdate (prim 127), indicating the Display Form has
// valid pixel content for the non-SDL rendering path.
std::atomic<bool> g_displayFormReady{false};

extern "C" bool vm_isDisplayFormReady() {
    return g_displayFormReady.load(std::memory_order_relaxed);
}
// Watchdog send diagnostic: selector and receiver class name for last send
char g_watchdogSelector[64] = {0};
char g_watchdogReceiverClass[64] = {0};
volatile int g_watchdogPrimIndex = 0;
volatile int g_watchdogProcessPriority = 0;  // Current process priority (updated in step loop)

volatile sig_atomic_t g_sigsegvRecoveryEnabled = 0;

// JIT code zone pointer for crash diagnostics — set by Interpreter when JIT initializes
#if PHARO_JIT_ENABLED
pharo::jit::CodeZone* g_jitCodeZone = nullptr;
#else
void* g_jitCodeZone = nullptr;
#endif

namespace pharo {

// Set to false to disable all debug file logging for performance
constexpr bool ENABLE_DEBUG_LOGGING = false;

uint64_t g_stepNum = 0;  // Global step counter for hang debugging (non-static for use in Primitives.cpp)


// ===== CONSTRUCTION =====

Interpreter::Interpreter(ObjectMemory& memory)
    : memory_(memory)
    , frameDepth_(0)
    , stackPointer_(stack_.data())
    , stackBase_(stack_.data())
    , framePointer_(nullptr)
    , instructionPointer_(nullptr)
    , bytecodeEnd_(nullptr)
    , activeContext_(Oop::nil())
    , currentFrameMaterializedCtx_(Oop::nil())
    , closure_(Oop::nil())
    , nlrTargetCtx_(Oop::nil())
    , nlrEnsureCtx_(Oop::nil())
    , nlrHomeMethod_(Oop::nil())
    , nlrValue_(Oop::nil())
    , lastCannotReturnCtx_(Oop::nil())
    , lastCannotReturnProcess_(Oop::nil())
    , cannotReturnCount_(0)
    , cannotReturnDeadline_(0)
    , argCount_(0)
    , extA_(0)
    , extB_(0)
    , usesSistaV1_(true)  // Default to SistaV1, will be set per method
    , running_(false)
    , primitiveFailed_(false)
    , worldRenderer_(memory)
{
    // Clear method cache
    for (auto& entry : methodCache_) {
        entry.selector = Oop::nil();
        entry.classOop = Oop::nil();
        entry.method = Oop::nil();
        entry.primitive = nullptr;
        entry.primitiveIndex = 0;
    }

    initializePrimitives();
}

bool Interpreter::initialize() {
    // Set up initial execution context
    // Find the startup process from special objects

    // Invalidate all ExternalAddress objects from the previous VM session.
    // The image was saved by a different VM process whose ffi_type*, dlsym,
    // and other C pointers are at different addresses. ExternalAddress objects
    // store raw C pointers as bytes; all of them are stale after image load.
    // Without this, TFBasicType>>validate sees non-null handles and skips
    // primFillType, causing FFI to use garbage pointers.
    {
        Oop extAddrClass = memory_.specialObject(SpecialObjectIndex::ClassExternalAddress);
        uint32_t extAddrClassIndex = 0;
        if (!extAddrClass.isNil() && extAddrClass.isObject()) {
            extAddrClassIndex = memory_.indexOfClass(extAddrClass);
        }
        if (extAddrClassIndex != 0) {
            size_t invalidated = 0;
            memory_.forEachObjectInOldSpace([&](ObjectHeader* obj) {
                if (obj->classIndex() == extAddrClassIndex &&
                    obj->isBytesObject() && obj->byteSize() >= sizeof(void*)) {
                    // Check if non-null before zeroing (avoid touching already-null ones)
                    void* ptr = nullptr;
                    memcpy(&ptr, obj->bytes(), sizeof(void*));
                    if (ptr != nullptr) {
                        memset(obj->bytes(), 0, obj->byteSize());
                        invalidated++;
                    }
                }
            });
            (void)invalidated;
        }
    }

    // Look up class indices dynamically (must happen after image load, before execution)
    initializeClassIndexCache();

    Oop scheduler = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);
    if (scheduler.isNil()) {
        return false;
    }

    // The scheduler association value is the ProcessScheduler
    Oop processScheduler = memory_.fetchPointer(1, scheduler);  // value
    if (processScheduler.isNil()) {
        return false;
    }

    // Get the active process
    // ProcessScheduler layout: quiescentProcessLists (slot 0), activeProcess (slot 1)
    Oop activeProcess = memory_.fetchPointer(1, processScheduler);  // activeProcess is slot 1!
    if (activeProcess.isNil()) {
        return false;
    }

    // Initialize ProcessSignalingLowSpace to active process before execution.
    // This ensures the lowSpaceWatcher has a valid context if it wakes up early.
    // Pharo's lowSpaceWatcher reads ProcessSignalingLowSpace and expects a valid
    // process/context there. Without this, it gets nil and errors.
    Oop currentPSLS = memory_.specialObject(SpecialObjectIndex::ProcessSignalingLowSpace);
    if (currentPSLS.isNil() || currentPSLS.rawBits() == memory_.nil().rawBits()) {
        memory_.setSpecialObject(SpecialObjectIndex::ProcessSignalingLowSpace, activeProcess);
    }

    // Note: ExternalObjectsArray is managed by Pharo's VirtualMachine class.
    // Pharo calls clearExternalObjects during startup which creates a fresh array.
    // The VM should NOT pre-initialize this array as Pharo will replace it.
    // Instead, the VM's parameter 49 set operation handles resizing when needed.

    // Get the suspended context
    // Modern Pharo Process layout:
    //   slot 0 = nextLink (for LinkedList)
    //   slot 1 = suspendedContext
    //   slot 2 = priority
    Oop context = memory_.fetchPointer(1, activeProcess);  // suspendedContext is slot 1 in modern Pharo

    // If context pointer is still at old image base, ImageLoader failed to relocate
    {
        uint64_t contextAddr = context.rawBits() & ~7ULL;
        if (contextAddr >= 0x10000000000ULL && contextAddr < 0x20000000000ULL) {
            stopVM("Unrelocated pointer in active process suspendedContext — ImageLoader bug");
            return false;
        }
    }

    // Check if context is nil (fresh image startup)
    if (context.isNil() || (context.isObject() && context.asObjectPtr()->slotCount() == 0)) {
        return bootstrapStartup();
    }

    // We have a valid context - but first analyze the sender chain
    // to understand what code we're resuming in

    Oop nilObj = memory_.specialObject(SpecialObjectIndex::NilObject);
    Oop currentCtx = context;
    Oop resumeContext = context;  // Default: resume from original context
    int depth = 0;
    bool inSnapshotCode = false;
    int snapshotEndDepth = -1;

    while (currentCtx.isObject() && currentCtx.rawBits() != nilObj.rawBits() && depth < 20) {
        ObjectHeader* ctxHdr = currentCtx.asObjectPtr();

        // Get receiver and method from context
        Oop receiver = memory_.fetchPointer(5, currentCtx);
        Oop method = memory_.fetchPointer(3, currentCtx);

        std::string rcvrClassName = memory_.classNameOf(receiver);
        std::string methodSelector = memory_.selectorOf(method);

        fprintf(stderr, "[RESUME] ctx[%d]: %s>>%s\n", depth,
                rcvrClassName.c_str(), methodSelector.c_str());

        // Check if we're in snapshot-related code
        if (rcvrClassName == "SnapshotOperation" || rcvrClassName == "SessionManager" ||
            rcvrClassName == "SmalltalkImage" || methodSelector == "snapshot:andQuit:" ||
            methodSelector == "snapshotPrimitive" || methodSelector == "primSnapshot" ||
            methodSelector == "primSnapshot:") {
            inSnapshotCode = true;
        } else if (inSnapshotCode && snapshotEndDepth < 0) {
            snapshotEndDepth = depth;
            resumeContext = currentCtx;
        }

        // Move to sender
        Oop sender = memory_.fetchPointer(0, currentCtx);
        currentCtx = sender;
        depth++;
    }
#ifdef DEBUG
    fprintf(stderr, "[RESUME] inSnapshotCode=%d chain depth=%d\n", inSnapshotCode, depth);
#endif

    // If we detected snapshot code, we're resuming from a saved image.
    // The Pharo snapshot code checks the return value:
    //   nil = save succeeded -> quit
    //   non-nil = resuming from save -> run startup handlers
    // We need to modify the context to indicate "resuming" by ensuring the
    // snapshot primitive returns a non-nil value (true).
    fprintf(stderr, "[RESUME] inSnapshotCode=%d chainDepth=%d\n", inSnapshotCode, depth);
    // Log the PC and first bytecodes of the resume context
    {
        Oop pcOop = memory_.fetchPointer(1, context);
        Oop methodOop = memory_.fetchPointer(3, context);
        std::string sel = methodOop.isObject() && !methodOop.isNil() ? memory_.selectorOf(methodOop) : "?";
        fprintf(stderr, "[RESUME] Initial context: method=#%s pc=%s\n",
                sel.c_str(),
                pcOop.isSmallInteger() ? std::to_string(pcOop.asSmallInteger()).c_str() : "nil");
    }
    if (inSnapshotCode) {
        // Patch the snapshot context's stack top to true so Smalltalk
        // interprets this as "resuming from saved image" instead of
        // "save succeeded, now quit".
        ObjectHeader* ctxHdr = context.asObjectPtr();
        Oop stackpOop = memory_.fetchPointer(2, context);
        if (stackpOop.isSmallInteger()) {
            int64_t stackp = stackpOop.asSmallInteger();
            if (stackp > 0) {
                size_t stackTopSlot = 6 + static_cast<size_t>(stackp) - 1;
                if (stackTopSlot < ctxHdr->slotCount()) {
                    Oop trueObj = memory_.specialObject(SpecialObjectIndex::TrueObject);
                    Oop falseObj = memory_.specialObject(SpecialObjectIndex::FalseObject);
                    Oop oldVal = memory_.fetchPointer(stackTopSlot, context);
                    const char* oldDesc = "unknown";
                    if (oldVal.rawBits() == trueObj.rawBits()) oldDesc = "true";
                    else if (oldVal.rawBits() == falseObj.rawBits()) oldDesc = "false";
                    else if (oldVal.isNil()) oldDesc = "nil";
                    else if (oldVal.isSmallInteger()) oldDesc = "SmallInteger";
                    fprintf(stderr, "[RESUME] Patching: stackp=%lld stackTopSlot=%zu oldVal=%s(0x%llx) -> true\n",
                            (long long)stackp, stackTopSlot, oldDesc, (unsigned long long)oldVal.rawBits());
                    memory_.storePointer(stackTopSlot, context, trueObj);
                }
            }
        }
        // Patch isImageStarting=true on ALL SnapshotOperation receivers in
        // the context chain. SnapshotOperation>>doSnapshot stores
        //   isImageStarting := snapshotPrimitive
        // but its receiver may differ from performSnapshot's receiver (the
        // inner call creates a separate context with a cloned or block-local
        // receiver). performSnapshot reads isImageStarting (slot 0) to
        // decide whether to call quitPrimitive. If slot 0 is false, it quits
        // before running session startup handlers — breaking FibRunner, SUnit
        // runner, and all other session handlers.
        {
            Oop trueObj = memory_.specialObject(SpecialObjectIndex::TrueObject);
            Oop walkCtx = context;
            int patchCount = 0;
            for (int d = 0; d < 20 && walkCtx.isObject() && !walkCtx.isNil(); d++) {
                Oop rcv = memory_.fetchPointer(5, walkCtx);
                if (rcv.isObject() && !rcv.isNil()) {
                    std::string cls = memory_.classNameOf(rcv);
                    if (cls == "SnapshotOperation") {
                        Oop slot0 = memory_.fetchPointer(0, rcv);
                        bool isTrue = slot0.rawBits() == trueObj.rawBits();
                        fprintf(stderr, "[RESUME] Found SnapshotOperation 0x%llx at ctx depth %d, isImageStarting=%s\n",
                                (unsigned long long)rcv.rawBits(), d, isTrue ? "true" : "false");
                        if (!isTrue) {
                            memory_.storePointer(0, rcv, trueObj);
                            patchCount++;
                        }
                    }
                }
                walkCtx = memory_.fetchPointer(0, walkCtx);  // sender
            }
            if (patchCount > 0) {
                fprintf(stderr, "[RESUME] Patched isImageStarting on %d SnapshotOperation(s)\n", patchCount);
            }
        }
    } else {
        fprintf(stderr, "[RESUME] Not in snapshot code — resuming as-is\n");
    }

    // Note: Display initialization is deferred to primitiveForceDisplayUpdate
    // to avoid crashes during early VM setup

    // Nil out the active process's suspendedContext now that we've loaded it.
    // This prevents GC from tracing stale context chains that keep objects alive.
    if (activeProcess.isObject() && !activeProcess.isNil()) {
        memory_.storePointer(1, activeProcess, memory_.nil());  // slot 1 = suspendedContext
    }

    // In headless mode, boost the startup process to timingPriority (80).
    // Session handlers may fork processes at timingPriority (80) — e.g.,
    // DelaySemaphoreScheduler. If the startup process runs at its default
    // priority (79), the forked P80 process preempts it and the remaining
    // session handlers never execute. At P80, semaphore signals to P80
    // waiters use putToSleep (same priority = no preemption), so the startup
    // process completes ALL handlers before yielding.
    // NOTE: Cannot use >80 — the scheduler list array has exactly 80 entries.
    if (isHeadless() && activeProcess.isObject() && !activeProcess.isNil()) {
        Oop currentPri = memory_.fetchPointer(ProcessPriorityIndex, activeProcess);
        if (currentPri.isSmallInteger()) {
            int pri = static_cast<int>(currentPri.asSmallInteger());
            fprintf(stderr, "[STARTUP] Boosting startup process 0x%llx from P%d to P80\n",
                    (unsigned long long)activeProcess.rawBits(), pri);
            memory_.storePointer(ProcessPriorityIndex, activeProcess, Oop::fromSmallInteger(80));
        }
    }
    //
    // Demote existing P40 processes to P10 to prevent the saved Morphic loop from
    // competing with newly created session processes.
    if (isHeadless() && activeProcess.isObject() && !activeProcess.isNil()) {
        Oop schedLists = memory_.fetchPointer(SchedulerProcessListsIndex, scheduler);
        if (schedLists.isObject() && !schedLists.isNil()) {
            // P40 is at index 39 (0-based)
            Oop p40List = memory_.fetchPointer(39, schedLists);
            if (p40List.isObject() && !p40List.isNil()) {
                Oop first = memory_.fetchPointer(LinkedListFirstLinkIndex, p40List);
                if (first.isObject() && !first.isNil() && first.rawBits() != memory_.nil().rawBits()) {
                    // There are processes in the P40 queue. Move them all to P10.
                    int demoted = 0;
                    Oop proc = first;
                    while (proc.isObject() && !proc.isNil() && proc.rawBits() != memory_.nil().rawBits()) {
                        Oop next = memory_.fetchPointer(ProcessNextLinkIndex, proc);
                        // Demote: change priority to 10
                        memory_.storePointer(ProcessPriorityIndex, proc, Oop::fromSmallInteger(10));
                        demoted++;
                        proc = next;
                    }
                    // Move the whole linked list from P40 queue to P10 queue
                    Oop p10List = memory_.fetchPointer(9, schedLists);  // P10 at index 9
                    if (p10List.isObject() && !p10List.isNil()) {
                        // Append P40 list to P10 list
                        Oop p10Last = memory_.fetchPointer(LinkedListLastLinkIndex, p10List);
                        Oop p10First = memory_.fetchPointer(LinkedListFirstLinkIndex, p10List);
                        if (p10Last.isObject() && !p10Last.isNil() && p10Last.rawBits() != memory_.nil().rawBits()) {
                            // P10 list has existing entries; append
                            memory_.storePointer(ProcessNextLinkIndex, p10Last, first);
                            Oop p40Last = memory_.fetchPointer(LinkedListLastLinkIndex, p40List);
                            memory_.storePointer(LinkedListLastLinkIndex, p10List, p40Last);
                        } else {
                            // P10 list is empty; copy P40 list
                            Oop p40Last = memory_.fetchPointer(LinkedListLastLinkIndex, p40List);
                            memory_.storePointer(LinkedListFirstLinkIndex, p10List, first);
                            memory_.storePointer(LinkedListLastLinkIndex, p10List, p40Last);
                        }
                        // Update myList pointers for each moved process
                        proc = first;
                        while (proc.isObject() && !proc.isNil() && proc.rawBits() != memory_.nil().rawBits()) {
                            Oop next = memory_.fetchPointer(ProcessNextLinkIndex, proc);
                            memory_.storePointer(ProcessMyListIndex, proc, p10List);
                            proc = next;
                        }
                        // Clear P40 list
                        memory_.storePointer(LinkedListFirstLinkIndex, p40List, memory_.nil());
                        memory_.storePointer(LinkedListLastLinkIndex, p40List, memory_.nil());
                        fprintf(stderr, "[STARTUP] Headless mode: demoted %d P40 processes to P10\n", demoted);
                    }
                }
            }
        }
    }

    // Initialize JIT before execution starts so noteMethodEntry works from first send
#if PHARO_JIT_ENABLED
    initializeJIT();
#endif

    // Bootstrap the Delay scheduler: signal TheTimerSemaphore.
    // When resuming from a snapshot, the Delay scheduler is blocked on the
    // timer semaphore (set in the previous session). Our VM starts with
    // nextWakeupTime_=0 and timerSemaphore_=nil, so checkTimerSemaphore()
    // will never fire. Signal the semaphore once to wake the scheduler,
    // which will then re-arm the timer and maintain itself.
    //
    // In headless mode, defer this signal until startup handlers have had a
    // chance to run. The saved MorphicRenderLoop (pri-80) has a short Delay
    // that could preempt CommandLineHandler (pri-40) during startup. We defer
    // for ~5M bytecodes (~2-3 seconds of startup) then signal, giving the
    // startup handlers time to install CommandLineUIManager and disable
    // MorphicRenderLoop before the timer wakes it.
    {
        Oop timerSema = memory_.specialObject(SpecialObjectIndex::TheTimerSemaphore);
        if (timerSema.isObject() && !timerSema.isNil() && timerSema.rawBits() > 0x10000) {
            lastKnownTimerSemaphore_ = timerSema;
            if (!isHeadless()) {
                synchronousSignal(timerSema);
            } else {
                timerSignalDeferred_ = true;
                fprintf(stderr, "[STARTUP] Headless mode: deferring timer semaphore signal\n");
            }
        }
    }

    // Image is booted (valid snapshot context being resumed)
    imageBooted_ = true;

    // PHARO_BENCH: replace resume context with benchmark(s) to bypass session handlers.
    // PHARO_BENCH=1 or PHARO_BENCH=fib: run fibonacci only
    // PHARO_BENCH=sieve: run sieve only
    // PHARO_BENCH=all: run fib + sieve
    // PHARO_FIB_N: override fibonacci argument (default 28)
    if (getenv("PHARO_BENCH")) {
        const char* benchType = getenv("PHARO_BENCH");
        fprintf(stderr, "[BENCH] PHARO_BENCH=%s detected\n", benchType);
        int fibN = 28;
        const char* fibEnv = getenv("PHARO_FIB_N");
        if (fibEnv) fibN = atoi(fibEnv);

        bool wantFib = (strcmp(benchType, "1") == 0 || strcmp(benchType, "fib") == 0 || strcmp(benchType, "all") == 0);
        bool wantSieve = (strcmp(benchType, "sieve") == 0 || strcmp(benchType, "all") == 0);
        bool wantEven = (strcmp(benchType, "even") == 0);
        bool wantTiny = (strcmp(benchType, "tiny") == 0 || strcmp(benchType, "tinyBenchmarks") == 0);

        if (wantFib)
            benchSpecs_.push_back({"fib(" + std::to_string(fibN) + ")", "Integer", "benchFib", fibN, 5});
        if (wantSieve)
            benchSpecs_.push_back({"sieve x3", "Integer", "benchmark", 3, 5});
        if (wantEven)
            benchSpecs_.push_back({"even", "Integer", "even", 42, 3});
        if (strcmp(benchType, "factorial") == 0)
            benchSpecs_.push_back({"factorial", "Integer", "factorial", 100, 5});
        if (wantTiny)
            benchSpecs_.push_back({"tinyBenchmarks", "Integer", "tinyBenchmarks", 0, 1});

        bool wantAWFY = (strcmp(benchType, "awfy") == 0);
        if (wantAWFY) {
            // Are We Fast Yet benchmarks — each is a class with innerBenchmarkLoop:
            benchSpecs_.push_back({"Richards",    "Richards",    "innerBenchmarkLoop:", 100,    5, true});
            benchSpecs_.push_back({"DeltaBlue",   "DeltaBlue",   "innerBenchmarkLoop:", 12000,  5, true});
            benchSpecs_.push_back({"Mandelbrot",  "Mandelbrot",  "innerBenchmarkLoop:", 500,    5, true});
            benchSpecs_.push_back({"NBody",       "NBody",       "innerBenchmarkLoop:", 250000, 5, true});
            benchSpecs_.push_back({"Bounce",      "Bounce",      "innerBenchmarkLoop:", 1500,   5, true});
            benchSpecs_.push_back({"Permute",     "Permute",     "innerBenchmarkLoop:", 1000,   5, true});
            benchSpecs_.push_back({"Queens",      "Queens",      "innerBenchmarkLoop:", 1000,   5, true});
            benchSpecs_.push_back({"Sieve",       "Sieve",       "innerBenchmarkLoop:", 3000,   5, true});
            benchSpecs_.push_back({"Storage",     "Storage",     "innerBenchmarkLoop:", 1000,   5, true});
            benchSpecs_.push_back({"Towers",      "Towers",      "innerBenchmarkLoop:", 600,    5, true});
            benchSpecs_.push_back({"List",        "List",        "innerBenchmarkLoop:", 1500,   5, true});
        }

        // Default to fib if unrecognized
        if (benchSpecs_.empty())
            benchSpecs_.push_back({"fib(" + std::to_string(fibN) + ")", "Integer", "benchFib", fibN, 5});

        benchMode_ = true;
        benchSpecIdx_ = 0;
        fprintf(stderr, "[BENCH] Starting %zu benchmark(s)\n", benchSpecs_.size());
        startBench(benchSpecs_[0]);
        return true;
    }

    // Now execute from the original context
    return executeFromContext(context);
}

// ===== DISPLAY INITIALIZATION =====

void Interpreter::initializeDisplayForm() {
    // Display surface initialization.
    // Once OSSDL2Driver starts, SDL_RenderPresent updates gDisplaySurface.
    // Until then, fill with black.
    if (pharo::gDisplaySurface && displayForm_.isNil()) {
        uint32_t* pixels = pharo::gDisplaySurface->pixels();
        int width = pharo::gDisplaySurface->width();
        int height = pharo::gDisplaySurface->height();
        memset(pixels, 0, width * height * 4);
        pharo::gDisplaySurface->update();
    }
}

Oop Interpreter::findMethod(const char* className, const char* selector) {
    Oop cls = memory_.findGlobal(className);
    if (!cls.isObject() || cls.isNil()) return Oop::nil();
    Oop methodDict = memory_.fetchPointer(1, cls);
    if (!methodDict.isObject()) return Oop::nil();
    ObjectHeader* mdHdr = methodDict.asObjectPtr();
    size_t mdSlots = mdHdr->slotCount();
    size_t selLen = strlen(selector);
    for (size_t i = 2; i < mdSlots; i++) {
        Oop key = mdHdr->slotAt(i);
        if (!key.isObject() || key.isNil()) continue;
        ObjectHeader* kHdr = key.asObjectPtr();
        if (kHdr->isBytesObject() && kHdr->byteSize() == selLen &&
            memcmp(kHdr->bytes(), selector, selLen) == 0) {
            Oop values = memory_.fetchPointer(1, methodDict);
            if (values.isObject()) {
                ObjectHeader* vHdr = values.asObjectPtr();
                if (i - 2 < vHdr->slotCount()) {
                    return vHdr->slotAt(i - 2);
                }
            }
            break;
        }
    }
    return Oop::nil();
}

Oop Interpreter::findMethodInHierarchy(Oop cls, const char* selector) {
    size_t selLen = strlen(selector);
    Oop current = cls;
    for (int depth = 0; depth < 20 && current.isObject() && !current.isNil(); depth++) {
        Oop methodDict = memory_.fetchPointer(1, current);
        if (methodDict.isObject() && !methodDict.isNil()) {
            ObjectHeader* mdHdr = methodDict.asObjectPtr();
            size_t mdSlots = mdHdr->slotCount();
            for (size_t i = 2; i < mdSlots; i++) {
                Oop key = mdHdr->slotAt(i);
                if (!key.isObject() || key.isNil()) continue;
                ObjectHeader* kHdr = key.asObjectPtr();
                if (kHdr->isBytesObject() && kHdr->byteSize() == selLen &&
                    memcmp(kHdr->bytes(), selector, selLen) == 0) {
                    Oop values = memory_.fetchPointer(1, methodDict);
                    if (values.isObject()) {
                        ObjectHeader* vHdr = values.asObjectPtr();
                        if (i - 2 < vHdr->slotCount()) {
                            return vHdr->slotAt(i - 2);
                        }
                    }
                    return Oop::nil();
                }
            }
        }
        // Walk to superclass (slot 0)
        current = memory_.fetchPointer(0, current);
    }
    return Oop::nil();
}

Oop Interpreter::allocateInstance(const char* className) {
    Oop cls = memory_.findGlobal(className);
    if (!cls.isObject() || cls.isNil()) return Oop::nil();
    uint32_t classIdx = memory_.indexOfClass(cls);
    if (classIdx == 0) classIdx = memory_.registerClass(cls);
    if (classIdx == 0) return Oop::nil();
    // Read instance spec to get fixed field count
    ObjectHeader* classHdr = cls.asObjectPtr();
    size_t numSlots = 0;
    if (classHdr->slotCount() >= 3) {
        Oop instSpec = classHdr->slotAt(2);
        if (instSpec.isSmallInteger())
            numSlots = static_cast<size_t>(instSpec.asSmallInteger() & 0xFFFF);
    }
    Oop instance = memory_.allocateSlots(classIdx, numSlots, ObjectFormat::FixedSize);
    if (!instance.isNil()) {
        // Initialize all slots to nil
        ObjectHeader* hdr = instance.asObjectPtr();
        for (size_t i = 0; i < numSlots; i++)
            hdr->slotAtPut(i, Oop::nil());
    }
    return instance;
}

Oop Interpreter::findBenchFibMethod() {
    return findMethod("Integer", "benchFib");
}

void Interpreter::startBench(const BenchSpec& spec) {
    fprintf(stderr, "[BENCH] === %s ===\n", spec.name.c_str());
    // GC between benchmarks to avoid heap exhaustion (especially after Storage)
    memory_.fullGC();
    benchRunCount_ = -1;  // warmup
    setupBenchContext();
}

void Interpreter::setupBenchContext() {
    const BenchSpec& spec = benchSpecs_[benchSpecIdx_];
    checkCountdown_ = INT32_MAX;
    benchStartTime_ = std::chrono::high_resolution_clock::now();

    Oop method;
    if (spec.instanceReceiver) {
        // Walk superclass chain to find inherited methods (e.g., innerBenchmarkLoop: on Benchmark)
        Oop cls = memory_.findGlobal(spec.className);
        method = (cls.isObject() && !cls.isNil()) ? findMethodInHierarchy(cls, spec.selector) : Oop::nil();
    } else {
        method = findMethod(spec.className, spec.selector);
    }
    fprintf(stderr, "[BENCH] setupBenchContext: %s>>%s method=0x%llx%s\n",
            spec.className, spec.selector, (unsigned long long)method.rawBits(),
            spec.instanceReceiver ? " (instance)" : "");

    if (!method.isNil()) {
        stackPointer_ = stackBase_;
        frameDepth_ = 0;
        Oop ctx;
        if (spec.instanceReceiver) {
            Oop receiver = allocateInstance(spec.className);
            if (receiver.isNil()) {
                fprintf(stderr, "[BENCH] Failed to allocate instance of %s\n", spec.className);
                goto skip;
            }
            ctx = memory_.createStartupContextWithArg(method, receiver, Oop::fromSmallInteger(spec.arg));
        } else {
            ctx = memory_.createStartupContext(method, Oop::fromSmallInteger(spec.arg));
        }
        if (!ctx.isNil()) {
            executeFromContext(ctx);
            return;
        }
    }
skip:
    fprintf(stderr, "[BENCH] Failed to find %s>>%s — skipping\n", spec.className, spec.selector);
    // Skip to next benchmark
    benchSpecIdx_++;
    if (benchSpecIdx_ < (int)benchSpecs_.size()) {
        startBench(benchSpecs_[benchSpecIdx_]);
    } else {
        stop();
    }
}

void Interpreter::handleBenchComplete() {
    fprintf(stderr, "[BENCH] handleBenchComplete called (specIdx=%d runCount=%d)\n", benchSpecIdx_, benchRunCount_);
    const BenchSpec& spec = benchSpecs_[benchSpecIdx_];
    if (benchRunCount_ == -1) {
        fprintf(stderr, "[BENCH] %s warmup done\n", spec.name.c_str());
        benchRunCount_ = 0;
    } else if (benchRunCount_ >= 0 && benchRunCount_ < spec.runs) {
        auto now = std::chrono::high_resolution_clock::now();
        long us = std::chrono::duration_cast<std::chrono::microseconds>(now - benchStartTime_).count();
        fprintf(stderr, "[BENCH] %s run %d: %ld us (%ld ms)\n", spec.name.c_str(), benchRunCount_, us, us / 1000);
        benchRunCount_++;
    }
    if (benchRunCount_ >= spec.runs) {
        // Move to next benchmark
        benchSpecIdx_++;
        if (benchSpecIdx_ < (int)benchSpecs_.size()) {
            startBench(benchSpecs_[benchSpecIdx_]);
            return;
        }
        // All benchmarks done
#if PHARO_JIT_ENABLED
        fprintf(stderr, "[BENCH] JIT stats: IC hits=%zu misses=%zu stale=%zu | J2J patches=%zu stencilCalls=%zu/%zu\n",
            jitICHits_, jitICMisses_, jitICStale_,
            jitJ2JDirectPatches_, jitJ2JStencilCalls_, jitJ2JStencilReturns_);
#endif
        fprintf(stderr, "[BENCH] All benchmarks complete.\n");
        stop();
        return;
    }
    // GC between runs to avoid heap exhaustion from allocation-heavy benchmarks
    memory_.fullGC();
    // Set up next run of same benchmark
    setupBenchContext();
}

void Interpreter::ensureDisplayForm(int width, int height, int depth) {
    // Check if Display already exists AND has valid content
    Oop existingDisplay = memory_.findGlobal("Display");
    if (!existingDisplay.isNil() && existingDisplay.isObject()) {
        // Check if the existing Display has valid width/height (slot 1 and 2)
        Oop existingWidth = memory_.fetchPointer(1, existingDisplay);
        Oop existingHeight = memory_.fetchPointer(2, existingDisplay);
        if (existingWidth.isSmallInteger() && existingHeight.isSmallInteger() &&
            existingWidth.asSmallInteger() > 0 && existingHeight.asSmallInteger() > 0) {
            // Valid existing display - just use it
            displayForm_ = existingDisplay;
            return;
        }
    }

    // Find Form and Bitmap classes
    Oop formClass = memory_.findGlobal("Form");
    Oop bitmapClass = memory_.findGlobal("Bitmap");

    if (formClass.isNil() || !formClass.isObject()) return;
    if (bitmapClass.isNil() || !bitmapClass.isObject()) return;

    uint32_t formClassIdx = memory_.indexOfClass(formClass);
    uint32_t bitmapClassIdx = memory_.indexOfClass(bitmapClass);

    // If class not in table, register it
    if (formClassIdx == 0) formClassIdx = memory_.registerClass(formClass);
    if (bitmapClassIdx == 0) bitmapClassIdx = memory_.registerClass(bitmapClass);
    if (formClassIdx == 0 || bitmapClassIdx == 0) return;

    // Allocate bitmap for pixels (32-bit pixels = 1 word each for 32-bit depth)
    size_t pixelCount = static_cast<size_t>(width) * height;
    Oop bitmapObj = memory_.allocateWords(bitmapClassIdx, pixelCount);

    if (bitmapObj.isNil()) return;

    // Fill bitmap with a distinctive color to show it's our bitmap
    {
        ObjectHeader* bitmapHdr = bitmapObj.asObjectPtr();
        uint32_t* pixels = reinterpret_cast<uint32_t*>(bitmapHdr->bytes());
        for (size_t i = 0; i < pixelCount; i++) {
            pixels[i] = 0xFF4488CC;  // Distinctive blue-gray so we know it's ours
        }
    }

    // GC SAFETY: push bitmapObj onto operand stack before second allocation,
    // since allocateSlots may trigger GC which would invalidate bitmapObj.
    push(bitmapObj);

    // Allocate Form with 5 slots: bits, width, height, depth, offset
    Oop formObj = memory_.allocateSlots(formClassIdx, 5);

    // Pop GC-safe bitmapObj
    bitmapObj = pop();

    if (formObj.isNil()) return;

    // Set Form slots
    memory_.storePointer(0, formObj, bitmapObj);                    // bits
    memory_.storePointer(1, formObj, Oop::fromSmallInteger(width)); // width
    memory_.storePointer(2, formObj, Oop::fromSmallInteger(height)); // height
    memory_.storePointer(3, formObj, Oop::fromSmallInteger(depth));  // depth
    memory_.storePointer(4, formObj, Oop::fromSmallInteger(0));      // offset (0@0)

    // Store locally
    displayForm_ = formObj;
    setScreenSize(width, height);
    setScreenDepth(depth);

    // Bind to 'Display' global so Morphic can find it
    bool bound = memory_.setGlobal("Display", formObj);
    fprintf(stderr, "[ensureDisplayForm] Created %dx%dx%d Form, setGlobal=%s\n",
            width, height, depth, bound ? "true" : "false");

    // Verify the binding worked
    Oop verifyDisplay = memory_.findGlobal("Display");
    fprintf(stderr, "[ensureDisplayForm] Verify: findGlobal(Display)=%s raw=0x%llx formObj=0x%llx\n",
            verifyDisplay.isNil() ? "nil" : "found",
            (unsigned long long)verifyDisplay.rawBits(),
            (unsigned long long)formObj.rawBits());
}

// ===== INPUT EVENT PROCESSING =====

void Interpreter::processInputEvents() {
    // Two event paths exist:
    // 1. InputEventSensor path: gEventQueue -> passThroughEvents_ -> primitive 264
    // 2. OSWindow/SDL2 path: gEventQueue -> SDL_PollEvent (via FFI) -> OSWindow
    //
    // When SDL2 event polling is active (SDL_PollEvent is being called),
    // we let it handle events directly from gEventQueue.
    // Otherwise, we drain gEventQueue into passThroughEvents_ for primitive 264.

    // When SDL event polling is active, let OSSDL2Driver handle events via SDL_PollEvent
    if (pharo::gEventQueue.isSDL2EventPollingActive()) return;
    pharo::Event event;
    while (pharo::gEventQueue.pop(event)) {
        // Skip WindowMetrics events - internal to C++ rendering
        if (event.type == static_cast<int>(pharo::EventType::WindowMetrics)) {
            continue;
        }

        // All events go to Pharo via passThroughEvents_ (consumed by primitive 264)
        passThroughEvents_.push_back(event);

        // Signal the input semaphore to wake up Smalltalk's event loop
        int inputSemaIdx = pharo::gEventQueue.getInputSemaphoreIndex();
        if (inputSemaIdx > 0) {
            signalExternalSemaphore(inputSemaIdx);
        }

    }
}

// ===== DISPLAY SYNCHRONIZATION =====

void Interpreter::syncDisplayToSurface() {
    if (!pharo::gDisplaySurface) return;

    // Process input events - queued for Smalltalk via primitive 264
    processInputEvents();

    // When SDL2 rendering is active, skip the Display Form copy.
    // SDL_RenderPresent copies SDL2 content to gDisplaySurface;
    // don't overwrite it with stale Display Form data.
    if (ffi_isSDLRenderingActive()) {
        static int sdlLogCount = 0;
        if (sdlLogCount < 3) {
            fprintf(stderr, "[syncDisplay] SDL rendering active — skipping Display Form copy\n");
            sdlLogCount++;
        }
        return;
    }

    // Don't copy until the image has drawn at least once (primitiveForceDisplayUpdate).
    // Before that, the Display Form bits are uninitialized heap memory (garbage pixels).
    if (!displayFormReady_) {
        static int logCount = 0;
        if (logCount < 5 || (logCount % 1000 == 0 && logCount < 10000)) {
            fprintf(stderr, "[syncDisplay] displayFormReady_=false (call %d), displayForm_ isNil=%d\n",
                    logCount, displayForm_.isNil() ? 1 : 0);
        }
        logCount++;
        return;
    }

    // displayForm_ is set during startup or by primitiveBeDisplay (prim 102).
    if (displayForm_.isNil()) {
        worldRenderer_.render();
        return;
    }

    // Get the Form's bits (slot 0) — re-fetch every time in case GC moved it
    Oop bits = memory_.fetchPointer(0, displayForm_);
    if (bits.isNil() || !bits.isObject()) {
        return;
    }

    ObjectHeader* bitsHdr = bits.asObjectPtr();
    uint32_t* srcPixels = reinterpret_cast<uint32_t*>(bitsHdr->bytes());

    // Get Form dimensions
    Oop widthOop = memory_.fetchPointer(1, displayForm_);
    Oop heightOop = memory_.fetchPointer(2, displayForm_);
    Oop depthOop = memory_.fetchPointer(3, displayForm_);

    int srcWidth = widthOop.isSmallInteger() ? widthOop.asSmallInteger() : screenWidth_;
    int srcHeight = heightOop.isSmallInteger() ? heightOop.asSmallInteger() : screenHeight_;
    int srcDepth = depthOop.isSmallInteger() ? depthOop.asSmallInteger() : 32;

    uint32_t* dstPixels = pharo::gDisplaySurface->pixels();
    int dstWidth = pharo::gDisplaySurface->width();
    int dstHeight = pharo::gDisplaySurface->height();

    int copyWidth = std::min(srcWidth, dstWidth);
    int copyHeight = std::min(srcHeight, dstHeight);

    if (copyWidth <= 0 || copyHeight <= 0) return;

    // Copy pixels (32-bit assumed for now)
    if (srcDepth == 32) {
        if (srcWidth == dstWidth) {
            // Widths match — single memcpy for entire buffer
            std::memcpy(dstPixels, srcPixels, copyWidth * copyHeight * sizeof(uint32_t));
        } else {
            // Widths differ — memcpy per row
            for (int y = 0; y < copyHeight; y++) {
                std::memcpy(dstPixels + y * dstWidth, srcPixels + y * srcWidth,
                            copyWidth * sizeof(uint32_t));
            }
        }

        // Debug: log pixel sample
        static int copyLogCount = 0;
        if (copyLogCount < 5 || (copyLogCount == 100)) {
            uint32_t p0 = srcPixels[0];
            uint32_t pMid = srcPixels[copyWidth * copyHeight / 2];
            fprintf(stderr, "[syncDisplay] Copied %dx%d pixels. p[0]=0x%08x p[mid]=0x%08x\n",
                    copyWidth, copyHeight, p0, pMid);
        }
        copyLogCount++;
    }

    pharo::gDisplaySurface->update();
}

// ===== MAIN LOOP =====

void Interpreter::stopVM(const char* reason) {
    fprintf(stderr, "[VM] stopVM: %s\n", reason ? reason : "(no reason)");
    running_ = false;
}

void Interpreter::dumpCurrentMethod() {
    fprintf(stderr, "\n=== CURRENT METHOD (frameDepth=%zu) ===\n", frameDepth_);
    fprintf(stderr, "  [current] #%s\n", memory_.selectorOf(method_).c_str());
    int count = 0;
    for (int f = static_cast<int>(frameDepth_); f >= 0 && count < 10; f--, count++) {
        fprintf(stderr, "  [%d] #%s\n", f, memory_.selectorOf(savedFrames_[f].savedMethod).c_str());
    }
    fprintf(stderr, "=== END ===\n\n");
}

void Interpreter::dumpProcessQueues() {
    fprintf(stderr, "\n=== Process Scheduler Dump ===\n");
    Oop nilObj = memory_.specialObject(SpecialObjectIndex::NilObject);
    Oop schedulerAssoc = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);
    if (!schedulerAssoc.isObject()) { fprintf(stderr, "No scheduler\n"); return; }
    Oop scheduler = memory_.fetchPointer(1, schedulerAssoc);
    if (!scheduler.isObject()) { fprintf(stderr, "No scheduler value\n"); return; }
    Oop activeProc = memory_.fetchPointer(1, scheduler);
    fprintf(stderr, "Active process: 0x%llx\n", (unsigned long long)activeProc.rawBits());
    if (activeProc.isObject() && activeProc.rawBits() > 0x10000) {
        Oop prio = memory_.fetchPointer(2, activeProc);
        Oop ctx = memory_.fetchPointer(1, activeProc);
        fprintf(stderr, "  priority=%lld suspCtx=0x%llx\n",
                prio.isSmallInteger() ? prio.asSmallInteger() : -1,
                (unsigned long long)ctx.rawBits());
    }
    Oop queues = memory_.fetchPointer(0, scheduler);
    if (!queues.isObject()) return;
    ObjectHeader* qH = queues.asObjectPtr();
    size_t numQ = qH->slotCount();
    fprintf(stderr, "Priority queues: %zu\n", numQ);
    for (size_t i = 0; i < numQ; i++) {
        Oop queue = qH->slotAt(i);
        if (queue.rawBits() == nilObj.rawBits() || !queue.isObject()) continue;
        Oop first = memory_.fetchPointer(0, queue);
        if (first.rawBits() == nilObj.rawBits() || !first.isObject()) continue;
        fprintf(stderr, "Queue at priority %zu:\n", i + 1);
        Oop proc = first;
        for (int j = 0; j < 10 && proc.isObject() && proc.rawBits() != nilObj.rawBits(); j++) {
            Oop prio = memory_.fetchPointer(2, proc);
            Oop ctx = memory_.fetchPointer(1, proc);
            // Get method name from context
            std::string mname = "?";
            if (ctx.isObject() && ctx.rawBits() > 0x10000) {
                ObjectHeader* ch = ctx.asObjectPtr();
                if (ch->slotCount() >= 6) {
                    Oop meth = memory_.fetchPointer(3, ctx);
                    mname = memory_.selectorOf(meth);
                }
            }
            fprintf(stderr, "  proc=0x%llx pri=%lld ctx=#%s\n",
                    (unsigned long long)proc.rawBits(),
                    prio.isSmallInteger() ? prio.asSmallInteger() : -1,
                    mname.c_str());
            // Follow nextLink (slot 0)
            Oop next = memory_.fetchPointer(0, proc);
            if (next.rawBits() == proc.rawBits()) break;
            proc = next;
        }
    }
    fprintf(stderr, "=== End Process Dump ===\n\n");
}

void Interpreter::interpret() {
#if __APPLE__
    volatile int64_t lastRunLoopPumpMs = 0;  // volatile for longjmp safety
    auto runLoopBase = std::chrono::steady_clock::now();
#endif

    // Entry point for callback re-entry via siglongjmp(reenterInterpreter_, 1)
    if (sigsetjmp(reenterInterpreter_, 0) != 0) {
        // Re-entered from enterInterpreterFromCallback().
        // Active process has been switched; just fall into the loop.
#if __APPLE__
        runLoopBase = std::chrono::steady_clock::now();
        lastRunLoopPumpMs = 0;
#endif
    }

    // ====================================================================
    // COMPUTED GOTO DISPATCH
    //
    // Uses GCC/Clang computed goto (&&label) for direct-threaded dispatch.
    // Each handler jumps directly to the next bytecode's handler, eliminating:
    //   - running_ check between bytecodes (only after sends/returns)
    //   - bytecodeEnd_ check between bytecodes (only in periodic checks)
    //   - Function call overhead for dispatchBytecode()
    //   - Each handler gets its own branch predictor entry
    //
    // Simple handlers (push, pop, store, jump, SmallInt arithmetic) are
    // fully inlined. Complex handlers delegate to existing member functions.
    // ====================================================================

    // --- Dispatch table (one-time init) ---
    static void* dispatchTable[256];
    static bool tableInit = false;
    if (!tableInit) {
        for (int i = 0; i < 256; i++)
            dispatchTable[i] = &&op_slow;
        for (int i = 0x00; i <= 0x0F; i++) dispatchTable[i] = &&op_pushRecvVar;
        for (int i = 0x10; i <= 0x1F; i++) dispatchTable[i] = &&op_pushLitVar;
        for (int i = 0x20; i <= 0x3F; i++) dispatchTable[i] = &&op_pushLitConst;
        for (int i = 0x40; i <= 0x4B; i++) dispatchTable[i] = &&op_pushTemp;
        dispatchTable[0x4C] = &&op_pushSelf;
        dispatchTable[0x4D] = &&op_pushTrue;
        dispatchTable[0x4E] = &&op_pushFalse;
        dispatchTable[0x4F] = &&op_pushNil;
        dispatchTable[0x50] = &&op_push0;
        dispatchTable[0x51] = &&op_push1;
        dispatchTable[0x53] = &&op_dup;
        for (int i = 0x60; i <= 0x6F; i++) dispatchTable[i] = &&op_arith;
        dispatchTable[0x79] = &&op_value;
        dispatchTable[0x7A] = &&op_value1;
        for (int i = 0x80; i <= 0x8F; i++) dispatchTable[i] = &&op_send0;
        for (int i = 0x90; i <= 0x9F; i++) dispatchTable[i] = &&op_send1;
        for (int i = 0xA0; i <= 0xAF; i++) dispatchTable[i] = &&op_send2;
        for (int i = 0xB0; i <= 0xB7; i++) dispatchTable[i] = &&op_jump;
        for (int i = 0xB8; i <= 0xBF; i++) dispatchTable[i] = &&op_jumpTrue;
        for (int i = 0xC0; i <= 0xC7; i++) dispatchTable[i] = &&op_jumpFalse;
        for (int i = 0xC8; i <= 0xCF; i++) dispatchTable[i] = &&op_popStoreRecv;
        for (int i = 0xD0; i <= 0xD7; i++) dispatchTable[i] = &&op_popStoreTemp;
        dispatchTable[0xD8] = &&op_pop;
        dispatchTable[0x76] = &&op_identityEq;   // spec== (2.4M total)
        tableInit = true;
    }

    checkCountdown_ = benchMode_ ? INT32_MAX : 1024;
    uint64_t totalSteps = 0;
    uint8_t bytecode;
    // --- Bytecode pair profiling (compile-time flag) ---
#ifndef PROFILE_BYTECODE_PAIRS
#define PROFILE_BYTECODE_PAIRS 0
#endif
#if PROFILE_BYTECODE_PAIRS
    static uint64_t pairCounts[256 * 256] = {};
    uint8_t prevBytecode = 0;
#endif

    // --- Dispatch macro ---
    // Countdown check is BEFORE inExtension_ reset so periodic_checks can
    // see extension state from the previous handler.
#if PROFILE_BYTECODE_PAIRS
    #define DISPATCH_NEXT() do { \
        if (__builtin_expect(--checkCountdown_ <= 0, 0)) goto periodic_checks; \
        prevBytecode = bytecode; \
        bytecode = *instructionPointer_++; \
        pairCounts[prevBytecode * 256 + bytecode]++; \
        if constexpr (ENABLE_DEBUG_LOGGING) { \
            recentBytecodes_[recentBytecodeIdx_ % 256] = bytecode; \
            recentBytecodeIdx_++; \
            lastBytecode_ = bytecode; \
        } \
        inExtension_ = false; \
        goto *dispatchTable[bytecode]; \
    } while(0)
#else
    #define DISPATCH_NEXT() do { \
        if (__builtin_expect(--checkCountdown_ <= 0, 0)) goto periodic_checks; \
        bytecode = *instructionPointer_++; \
        if constexpr (ENABLE_DEBUG_LOGGING) { \
            recentBytecodes_[recentBytecodeIdx_ % 256] = bytecode; \
            recentBytecodeIdx_++; \
            lastBytecode_ = bytecode; \
        } \
        inExtension_ = false; \
        goto *dispatchTable[bytecode]; \
    } while(0)
#endif

    // --- Entry point ---
    if (__builtin_expect(instructionPointer_ >= bytecodeEnd_, 0)) {
        returnValue(receiver_);
        if (!running_) { goto cg_exit; }
    }
    bytecode = *instructionPointer_++;
    // Debug: trace bytecodes when inside atAllPut: (2nd+ call)
    if (__builtin_expect(traceAtAllPut_ >= 2 && traceAtAllPut_ <= 3 && frameDepth_ >= 1, 0)) {
        static int aapBCCount = 0;
        if (aapBCCount < 40) {
            aapBCCount++;
            ptrdiff_t off = (instructionPointer_ - 1) - method_.asObjectPtr()->bytes();
            fprintf(stderr, "[BC #%d] fd=%zu 0x%02X @off=%td SP=%ld top=0x%llx method=#%s\n",
                    aapBCCount, frameDepth_, bytecode, off,
                    (long)(stackPointer_ - stackBase_),
                    (stackPointer_ > stackBase_) ? (unsigned long long)(stackPointer_[-1].rawBits()) : 0ULL,
                    memory_.selectorOf(method_).c_str());
        }
    }
    if constexpr (ENABLE_DEBUG_LOGGING) {
        recentBytecodes_[recentBytecodeIdx_ % 256] = bytecode;
        recentBytecodeIdx_++;
        lastBytecode_ = bytecode;
    }
    inExtension_ = false;
    goto *dispatchTable[bytecode];

    // ====== FAST INLINE HANDLERS ======

    op_pushRecvVar:
        push(memory_.fetchPointerUnchecked(bytecode & 0x0F, receiver_));
        DISPATCH_NEXT();

    op_pushLitVar:
        pushLiteralVariable(bytecode - 0x10);
        DISPATCH_NEXT();

    op_pushLitConst:
        pushLiteralConstant(bytecode - 0x20);
        DISPATCH_NEXT();

    op_pushTemp: {
        int idx = (bytecode < 0x48) ? (bytecode - 0x40) : (bytecode - 0x48 + 8);
        push(*(framePointer_ + 1 + idx));
        DISPATCH_NEXT();
    }

    op_pushSelf:  push(receiver_);              DISPATCH_NEXT();
    op_pushTrue:  push(memory_.trueObject());   DISPATCH_NEXT();
    op_pushFalse: push(memory_.falseObject());  DISPATCH_NEXT();
    op_pushNil: {
        // Speculative: pushNil + spec== (1.73M, 45.9% — "x == nil")
        // Identity comparison with nil doesn't need method lookup.
        // Normal: pushNil adds nil; == pops rcvr+nil, pushes bool.
        // Net: replaces TOS (rcvr → bool). With branch: rcvr consumed.
        if (*instructionPointer_ == 0x76) { // spec ==
            instructionPointer_++;
            Oop rcvr = stackTop();
            bool isNil = rcvr.isNil();
            // Fuse with following branch too (1.5M spec== + jump pairs)
            uint8_t nextBC = *instructionPointer_;
            if (nextBC >= 0xC0 && nextBC <= 0xC7) { // jumpFalse
                instructionPointer_++;
                pop(); // consume rcvr (== pops rcvr+nil, jump pops bool)
                if (!isNil) instructionPointer_ += (nextBC & 0x07) + 1;
                DISPATCH_NEXT();
            }
            if (nextBC >= 0xB8 && nextBC <= 0xBF) { // jumpTrue
                instructionPointer_++;
                pop(); // consume rcvr
                if (isNil) instructionPointer_ += (nextBC & 0x07) + 1;
                DISPATCH_NEXT();
            }
            // No branch fusion — replace TOS with boolean
            *(stackPointer_ - 1) = isNil ? memory_.trueObject() : memory_.falseObject();
            DISPATCH_NEXT();
        }
        push(memory_.nil());
        DISPATCH_NEXT();
    }
    op_push0: {
        // Speculative: push0 + arith= (356K, "x = 0" pattern)
        if (*instructionPointer_ == 0x66) { // arith =
            Oop rcvr = stackTop();
            if (rcvr.isSmallInteger()) {
                instructionPointer_++;
                *(stackPointer_ - 1) = rcvr.asSmallInteger() == 0
                    ? memory_.trueObject() : memory_.falseObject();
                DISPATCH_NEXT();
            }
        }
        push(Oop::fromSmallInteger(0));
        DISPATCH_NEXT();
    }

    op_push1: {
        // Speculative: push1 + arith+ (2.19M, 64.9% hit rate — "x + 1")
        if (*instructionPointer_ == 0x60) { // arith +
            Oop rcvr = stackTop();
            if (rcvr.isSmallInteger()) {
                int64_t r = rcvr.asSmallInteger() + 1;
                if (r <= Oop::smallIntegerMax()) {
                    instructionPointer_++;
                    *(stackPointer_ - 1) = Oop::fromSmallInteger(r);
                    DISPATCH_NEXT();
                }
            }
        }
        // Speculative: push1 + arith- (183K — "x - 1")
        if (*instructionPointer_ == 0x61) { // arith -
            Oop rcvr = stackTop();
            if (rcvr.isSmallInteger()) {
                int64_t r = rcvr.asSmallInteger() - 1;
                if (r >= Oop::smallIntegerMin()) {
                    instructionPointer_++;
                    *(stackPointer_ - 1) = Oop::fromSmallInteger(r);
                    DISPATCH_NEXT();
                }
            }
        }
        push(Oop::fromSmallInteger(1));
        DISPATCH_NEXT();
    }
    op_dup: {
        // Speculative: dup + pushNil + spec== + jumpFalse (1.45M, 95.7%)
        // Full nil-check idiom: "x ifNotNil:" compiles to dup; pushNil; ==; jmpF
        if (instructionPointer_[0] == 0x4F && instructionPointer_[1] == 0x76) {
            // dup; pushNil; ==
            Oop val = stackTop();
            bool isNil = val.isNil();
            instructionPointer_ += 2; // skip pushNil + spec==
            // Try to fuse with branch
            uint8_t nextBC = *instructionPointer_;
            if (nextBC >= 0xC0 && nextBC <= 0xC7) { // jumpFalse
                instructionPointer_++;
                if (!isNil) {
                    // Not nil: don't jump (ifNotNil path), keep dup'd value
                    instructionPointer_ += (nextBC & 0x07) + 1;
                }
                // Nil: jump (skip ifNotNil body), keep dup'd value
                DISPATCH_NEXT();
            }
            if (nextBC >= 0xB8 && nextBC <= 0xBF) { // jumpTrue
                instructionPointer_++;
                if (isNil) {
                    instructionPointer_ += (nextBC & 0x07) + 1;
                }
                DISPATCH_NEXT();
            }
            // No branch — push boolean only (dup'd val consumed by ==)
            // Before: [..., val]. After dup+pushNil+==: [..., val, bool]
            push(isNil ? memory_.trueObject() : memory_.falseObject());
            DISPATCH_NEXT();
        }
        push(stackTop());
        DISPATCH_NEXT();
    }

    op_jump:
        instructionPointer_ += (bytecode & 0x07) + 1;
        DISPATCH_NEXT();

    op_jumpTrue: {
        Oop val = pop();
        if (__builtin_expect(val.rawBits() == memory_.trueObject().rawBits(), 1)) {
            instructionPointer_ += (bytecode & 0x07) + 1;
        } else if (__builtin_expect(val.rawBits() != memory_.falseObject().rawBits(), 0)) {
            push(val);
            sendMustBeBoolean(val);
            if (__builtin_expect(!running_, 0)) goto cg_exit;
        }
        DISPATCH_NEXT();
    }

    op_jumpFalse: {
        Oop val = pop();
        if (__builtin_expect(val.rawBits() == memory_.falseObject().rawBits(), 1)) {
            instructionPointer_ += (bytecode & 0x07) + 1;
        } else if (__builtin_expect(val.rawBits() != memory_.trueObject().rawBits(), 0)) {
            push(val);
            sendMustBeBoolean(val);
            if (__builtin_expect(!running_, 0)) goto cg_exit;
        }
        DISPATCH_NEXT();
    }

    op_popStoreRecv: {
        Oop value = pop();
        setReceiverInstVar(bytecode & 0x07, value);
        DISPATCH_NEXT();
    }

    op_popStoreTemp: {
        Oop value = pop();
        setTemporary(bytecode & 0x07, value);
        DISPATCH_NEXT();
    }

    op_pop:
        pop();
        DISPATCH_NEXT();

    // --- Arithmetic sends: inline SmallInteger fast paths ---
    op_arith: {
        int which = bytecode & 0x0F;
        Oop rcvr = stackValue(1);
        Oop arg = stackValue(0);

        if (rcvr.isSmallInteger() && arg.isSmallInteger()) {
            int64_t a = rcvr.asSmallInteger();
            int64_t b = arg.asSmallInteger();
            bool cmp = false;  // used by comparison+branch fusion

            switch (which) {
            case 0: { // +
                int64_t r = a + b;
                if (r >= Oop::smallIntegerMin() && r <= Oop::smallIntegerMax()) {
                    popN(2); push(Oop::fromSmallInteger(r)); DISPATCH_NEXT();
                }
                break;
            }
            case 1: { // -
                int64_t r = a - b;
                if (r >= Oop::smallIntegerMin() && r <= Oop::smallIntegerMax()) {
                    popN(2); push(Oop::fromSmallInteger(r)); DISPATCH_NEXT();
                }
                break;
            }
            // --- Comparison cases with branch fusion ---
            // If the next bytecode is a conditional jump, branch directly
            // without creating/pushing a boolean object. This fuses two
            // dispatches into one and eliminates boolean allocation.
            // Profile data: 5.3M comparison+branch pairs per 100M bytecodes.
            case 2: cmp = a < b;  goto arith_cmp_fuse;
            case 3: cmp = a > b;  goto arith_cmp_fuse;
            case 4: cmp = a <= b; goto arith_cmp_fuse;
            case 5: cmp = a >= b; goto arith_cmp_fuse;
            case 6: cmp = a == b; goto arith_cmp_fuse;
            case 7: cmp = a != b; goto arith_cmp_fuse;

            arith_cmp_fuse: {
                uint8_t nextBC = *instructionPointer_;
                if (nextBC >= 0xC0 && nextBC <= 0xC7) {
                    // Short jumpFalse: jump if comparison is false
                    instructionPointer_++;
                    popN(2);
                    if (!cmp) instructionPointer_ += (nextBC & 0x07) + 1;
                    DISPATCH_NEXT();
                }
                if (nextBC >= 0xB8 && nextBC <= 0xBF) {
                    // Short jumpTrue: jump if comparison is true
                    instructionPointer_++;
                    popN(2);
                    if (cmp) instructionPointer_ += (nextBC & 0x07) + 1;
                    DISPATCH_NEXT();
                }
                if (nextBC == 0xEF) {
                    // Extended jumpFalse (2 bytes, no extB in this path)
                    instructionPointer_++;
                    uint8_t offset = *instructionPointer_++;
                    popN(2);
                    if (!cmp) instructionPointer_ += offset;
                    DISPATCH_NEXT();
                }
                if (nextBC == 0xEE) {
                    // Extended jumpTrue (2 bytes, no extB in this path)
                    instructionPointer_++;
                    uint8_t offset = *instructionPointer_++;
                    popN(2);
                    if (cmp) instructionPointer_ += offset;
                    DISPATCH_NEXT();
                }
                // No fusible branch — push boolean normally
                popN(2);
                push(cmp ? memory_.trueObject() : memory_.falseObject());
                DISPATCH_NEXT();
            }
            case 8: { // *
                __int128 r128 = (__int128)a * (__int128)b;
                int64_t r = (int64_t)r128;
                if (r128 == (__int128)r && r >= Oop::smallIntegerMin() && r <= Oop::smallIntegerMax()) {
                    popN(2); push(Oop::fromSmallInteger(r)); DISPATCH_NEXT();
                }
                break;
            }
            case 14: // bitAnd:
                popN(2); push(Oop::fromSmallInteger(a & b)); DISPATCH_NEXT();
            case 15: // bitOr:
                popN(2); push(Oop::fromSmallInteger(a | b)); DISPATCH_NEXT();
            default: break;
            }
        }
        // Slow path: full method lookup
        arithmeticSend(which);
        if (__builtin_expect(!running_, 0)) goto cg_exit;
        DISPATCH_NEXT();
    }

    // --- FullBlockClosure >> value fast path ---
    op_value: {
        Oop rcvr = stackValue(0);
        if (rcvr.isObject() && rcvr.rawBits() > 0x10000 &&
            rcvr.asObjectPtr()->classIndex() == fullBlockClosureClassIndex_) {
            argCount_ = 0;
            primitiveFailed_ = false;
            primFailCode_ = 0;
            if (primitiveFullClosureValue(0) == PrimitiveResult::Success) {
                if (__builtin_expect(!running_, 0)) goto cg_exit;
                DISPATCH_NEXT();
            }
        }
        commonSend(9);
        if (__builtin_expect(!running_, 0)) goto cg_exit;
        DISPATCH_NEXT();
    }

    // --- FullBlockClosure >> value: fast path ---
    op_value1: {
        Oop rcvr = stackValue(1);
        if (rcvr.isObject() && rcvr.rawBits() > 0x10000 &&
            rcvr.asObjectPtr()->classIndex() == fullBlockClosureClassIndex_) {
            argCount_ = 1;
            primitiveFailed_ = false;
            primFailCode_ = 0;
            if (primitiveFullClosureValue(1) == PrimitiveResult::Success) {
                if (__builtin_expect(!running_, 0)) goto cg_exit;
                DISPATCH_NEXT();
            }
        }
        commonSend(10);
        if (__builtin_expect(!running_, 0)) goto cg_exit;
        DISPATCH_NEXT();
    }

    // --- Literal sends: bypass dispatchBytecode overhead ---
    op_send0:
        sendSelector(literal(bytecode & 0x0F), 0);
        if (__builtin_expect(!running_, 0)) goto cg_exit;
        DISPATCH_NEXT();

    op_send1:
        sendSelector(literal(bytecode & 0x0F), 1);
        if (__builtin_expect(!running_, 0)) goto cg_exit;
        DISPATCH_NEXT();

    op_send2:
        sendSelector(literal(bytecode & 0x0F), 2);
        if (__builtin_expect(!running_, 0)) goto cg_exit;
        DISPATCH_NEXT();

    // --- spec== (identity comparison) with branch fusion ---
    // Profile: 2.4M total, 30% followed by conditional jump
    op_identityEq: {
        Oop rcvr = stackValue(1);
        Oop arg = stackValue(0);
        bool eq = rcvr.rawBits() == arg.rawBits();
        // Fuse with following conditional jump
        uint8_t nextBC = *instructionPointer_;
        if (nextBC >= 0xC0 && nextBC <= 0xC7) {
            instructionPointer_++;
            popN(2);
            if (!eq) instructionPointer_ += (nextBC & 0x07) + 1;
            DISPATCH_NEXT();
        }
        if (nextBC >= 0xB8 && nextBC <= 0xBF) {
            instructionPointer_++;
            popN(2);
            if (eq) instructionPointer_ += (nextBC & 0x07) + 1;
            DISPATCH_NEXT();
        }
        if (nextBC == 0xEF) {
            instructionPointer_++;
            uint8_t offset = *instructionPointer_++;
            popN(2);
            if (!eq) instructionPointer_ += offset;
            DISPATCH_NEXT();
        }
        if (nextBC == 0xEE) {
            instructionPointer_++;
            uint8_t offset = *instructionPointer_++;
            popN(2);
            if (eq) instructionPointer_ += offset;
            DISPATCH_NEXT();
        }
        popN(2);
        push(eq ? memory_.trueObject() : memory_.falseObject());
        DISPATCH_NEXT();
    }

    // ====== SLOW PATH (extensions, returns, closures, etc.) ======
    op_slow:
        inExtension_ = false;
        dispatchBytecode(bytecode);
        if (__builtin_expect(!running_, 0)) goto cg_exit;
        DISPATCH_NEXT();

    // ====== PERIODIC CHECKS (every 1024 bytecodes) ======
    periodic_checks: {
        checkCountdown_ = benchMode_ ? INT32_MAX : 1024;
        totalSteps += 1024;
        g_stepNum += 1024;
        g_watchdogSteps.store(g_stepNum, std::memory_order_relaxed);

#if PROFILE_BYTECODE_PAIRS
        // Dump pair counts to file after 100M bytecodes (one-shot)
        if (__builtin_expect(totalSteps == 100 * 1024 * 1024, 0)) {
            FILE* pf = fopen("/tmp/bytecode_pair_counts.tsv", "w");
            if (pf) {
                fprintf(pf, "bc1\tbc2\tcount\n");
                for (int i = 0; i < 256; i++)
                    for (int j = 0; j < 256; j++)
                        if (pairCounts[i * 256 + j] > 0)
                            fprintf(pf, "%d\t%d\t%llu\n", i, j,
                                    (unsigned long long)pairCounts[i * 256 + j]);
                fclose(pf);
                fprintf(stderr, "[PROFILE] Bytecode pair counts written to /tmp/bytecode_pair_counts.tsv at %llu steps\n",
                        (unsigned long long)totalSteps);
            }
        }
#endif

        if (__builtin_expect(!running_, 0)) goto cg_exit;

        // Safety: check IP bounds (was per-bytecode, now periodic)
        if (__builtin_expect(instructionPointer_ >= bytecodeEnd_, 0)) {
            returnValue(receiver_);
            if (!running_) goto cg_exit;
        }

        // CRITICAL: If we just dispatched an extension byte (0xE0/0xE1),
        // skip all process-switching checks. Extension bytes set extA_/extB_
        // which the NEXT bytecode needs. A process switch calls
        // executeFromContext() which resets extA_/extB_ to 0, corrupting
        // the next bytecode's arguments.
        if (inExtension_) {
            if (__builtin_expect(memory_.needsCompactGC(), 0)) {
                memory_.clearCompactGCFlag();
                memory_.fullGC(/* skipEphemerons */ true);
                flushMethodCache();
            }
            // Resume without process switching — don't clear inExtension_
            // (the consumer's DISPATCH_NEXT will clear it)
            bytecode = *instructionPointer_++;
            if constexpr (ENABLE_DEBUG_LOGGING) {
                recentBytecodes_[recentBytecodeIdx_ % 256] = bytecode;
                recentBytecodeIdx_++;
                lastBytecode_ = bytecode;
            }
            goto *dispatchTable[bytecode];
        }

        // -- GC safe point --
        if (__builtin_expect(memory_.needsCompactGC(), 0)) {
            memory_.clearCompactGCFlag();
            memory_.fullGC(/* skipEphemerons */ true);
            flushMethodCache();
        }

        // -- Finalization one-shot after GC --
        if (__builtin_expect(finalizationCheckAfterGC_, 0)) {
            finalizationCheckAfterGC_ = false;
            signalFinalizationIfNeeded();
        }

        // -- Timer semaphore (Delay scheduler) --
        checkTimerSemaphore();

        // -- Deferred timer signal (headless startup) --
        // After ~5M bytecodes, signal the timer semaphore that was deferred
        // during headless startup. By now CommandLineUIManager should be
        // installed and MorphicRenderLoop disabled.
        if (__builtin_expect(timerSignalDeferred_ && g_stepNum > 5000000, 0)) {
            timerSignalDeferred_ = false;
            if (!lastKnownTimerSemaphore_.isNil()) {
                fprintf(stderr, "[STARTUP] Firing deferred timer semaphore signal (step %llu)\n", g_stepNum);
                synchronousSignal(lastKnownTimerSemaphore_);
                lastTimerSignalTime_ = std::chrono::steady_clock::now();
            }
        }

        // -- External semaphore signals (from heartbeat/events) --
        if (hasPendingSignals()) {
            processPendingSignals();
        }

        // -- Force yield (set by heartbeat every ~2ms) --
        if (forceYield_.load(std::memory_order_acquire)) {
            if (suppressContextSwitch_) {
                suppressContextSwitch_ = false;
            } else {
                forceYield_.store(false, std::memory_order_release);
                handleForceYield();
            }
        }

        // -- Terminate stuck process (set by watchdog, rare) --
        if (__builtin_expect(terminateStuck_.load(std::memory_order_acquire), 0)) {
            terminateStuck_.store(false, std::memory_order_relaxed);
            terminateAndSwitchProcess();
        }

        // -- cannotReturn: deadline --
        if (__builtin_expect(cannotReturnDeadline_ > 0 && g_stepNum >= cannotReturnDeadline_, 0)) {
            Oop currentProcess = getActiveProcess();
            if (currentProcess.rawBits() == lastCannotReturnProcess_.rawBits()) {
                cannotReturnCount_ = 0;
                cannotReturnDeadline_ = 0;
                lastCannotReturnProcess_ = Oop::nil();
                lastCannotReturnCtx_ = Oop::nil();
                terminateCurrentProcess();
                if (!tryReschedule() && !bootstrapStartup()) {
                    stopVM("No runnable processes after cannotReturn: deadline termination");
                }
            } else {
                cannotReturnDeadline_ = 0;
            }
        }

        // -- Display sync requested by heartbeat thread --
        if (pendingDisplaySync_.load(std::memory_order_acquire)) {
            pendingDisplaySync_.store(false, std::memory_order_release);
            syncDisplayToSurface();
        }

        // -- Test runner trigger (from monitor thread) --
        // Note: pendingTestRun_ flag is set by monitor thread but test execution
        // happens via Smalltalk startup.st script loaded by StartupPreferencesLoader.
        // This flag is only used by the monitor thread to detect when to check results.
        if (__builtin_expect(pendingTestRun_.load(std::memory_order_acquire), 0)) {
            pendingTestRun_.store(false, std::memory_order_release);
        }

        // -- Finalization (periodic, for auto-GC mourners) --
        signalFinalizationIfNeeded();

        // === LESS FREQUENT CHECKS (every ~64K bytecodes) ===
        if ((totalSteps & 0xFFFF) == 0) {
            checkForPreemption();

            // Stuck process termination (wall-clock based)
            {
                Oop currentActive = getActiveProcess();
                Oop prioOop = memory_.fetchPointer(ProcessPriorityIndex, currentActive);
                int prio = prioOop.isSmallInteger() ? (int)prioOop.asSmallInteger() : 0;

                if (prio >= 80) {
                    startupGracePeriod_ = false;
                    if (trackedProcess_.rawBits() == currentActive.rawBits())
                        trackedProcess_ = Oop::nil();
                } else if (!startupGracePeriod_ && prio < 79) {
                    if (currentActive.rawBits() != trackedProcess_.rawBits()) {
                        trackedProcess_ = currentActive;
                        cumulativeMs_ = 0;
                        lastResumeTime_ = std::chrono::steady_clock::now();
                        trackStartTime_ = lastResumeTime_;
                    } else {
                        auto now = std::chrono::steady_clock::now();
                        auto wallMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - trackStartTime_).count();
                        if (wallMs >= 600000) {
                            fprintf(stderr, "[VM-TIMEOUT] Process 0x%llx at P%d stuck for %lldms — terminating\n",
                                    (unsigned long long)currentActive.rawBits(), prio, (long long)wallMs);
                            trackedProcess_ = Oop::nil();
                            memory_.storePointer(ProcessSuspendedContextIndex, currentActive, Oop::nil());
                            Oop nextProc = wakeHighestPriority();
                            if (!nextProc.isNil() && nextProc.isObject())
                                transferTo(nextProc);
                        }
                    }
                }
            }

            // Watchdog process priority update
            {
                Oop proc = getActiveProcess();
                if (proc.isObject() && proc.rawBits() > 0x10000) {
                    Oop priOop = memory_.fetchPointer(ProcessPriorityIndex, proc);
                    g_watchdogProcessPriority = priOop.isSmallInteger() ? priOop.asSmallInteger() : -1;
                }
            }
        }

        // === INFREQUENT CHECKS (every ~100K bytecodes) ===
        if ((totalSteps % 102400) == 0) {
            processInputEvents();

#if __APPLE__
            if (relinquishCallback_) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - runLoopBase).count();
                if (elapsed - lastRunLoopPumpMs >= 50) {
                    lastRunLoopPumpMs = elapsed;
                    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true);
                }
            }
#endif

            if (displayForm_.isNil()) {
                Oop display = memory_.findGlobal("Display");
                if (!display.isNil() && display.isObject()) {
                    ObjectHeader* hdr = display.asObjectPtr();
                    if (hdr->slotCount() >= 4) {
                        Oop w = memory_.fetchPointer(1, display);
                        Oop h = memory_.fetchPointer(2, display);
                        if (w.isSmallInteger() && h.isSmallInteger() &&
                            w.asSmallInteger() > 0 && h.asSmallInteger() > 0) {
                            setDisplayForm(display);
                            setScreenSize(w.asSmallInteger(), h.asSmallInteger());
                            Oop d = memory_.fetchPointer(3, display);
                            if (d.isSmallInteger()) setScreenDepth(d.asSmallInteger());
                        }
                    }
                }
            }
        }

        // Resume dispatch after checks
        if (__builtin_expect(!running_, 0)) goto cg_exit;
        bytecode = *instructionPointer_++;
        if constexpr (ENABLE_DEBUG_LOGGING) {
            recentBytecodes_[recentBytecodeIdx_ % 256] = bytecode;
            recentBytecodeIdx_++;
            lastBytecode_ = bytecode;
        }
        inExtension_ = false;
        goto *dispatchTable[bytecode];
    } // periodic_checks

    cg_exit:
#if PROFILE_BYTECODE_PAIRS
    {
        // Dump top 50 bytecode pairs by frequency
        struct PairEntry { uint8_t a, b; uint64_t count; };
        std::vector<PairEntry> pairs;
        pairs.reserve(1000);
        for (int i = 0; i < 256; i++)
            for (int j = 0; j < 256; j++)
                if (pairCounts[i * 256 + j] > 0)
                    pairs.push_back({(uint8_t)i, (uint8_t)j, pairCounts[i * 256 + j]});
        std::sort(pairs.begin(), pairs.end(), [](const PairEntry& a, const PairEntry& b) {
            return a.count > b.count;
        });

        // Bytecode name helper
        auto bcName = [](uint8_t bc) -> std::string {
            if (bc <= 0x0F) return "pushRecvVar" + std::to_string(bc);
            if (bc <= 0x1F) return "pushLitVar" + std::to_string(bc - 0x10);
            if (bc <= 0x3F) return "pushLitConst" + std::to_string(bc - 0x20);
            if (bc <= 0x4B) return "pushTemp" + std::to_string(bc < 0x48 ? bc - 0x40 : bc - 0x48 + 8);
            if (bc == 0x4C) return "pushSelf";
            if (bc == 0x4D) return "pushTrue";
            if (bc == 0x4E) return "pushFalse";
            if (bc == 0x4F) return "pushNil";
            if (bc == 0x50) return "push0";
            if (bc == 0x51) return "push1";
            if (bc == 0x52) return "pushThisCtx";
            if (bc == 0x53) return "dup";
            if (bc <= 0x57) return "unused" + std::to_string(bc);
            if (bc == 0x58) return "retRecv";
            if (bc == 0x59) return "retTrue";
            if (bc == 0x5A) return "retFalse";
            if (bc == 0x5B) return "retNil";
            if (bc == 0x5C) return "retTop";
            if (bc == 0x5D) return "blockRetNil";
            if (bc == 0x5E) return "blockRetTop";
            if (bc == 0x5F) return "nop";
            if (bc <= 0x6F) {
                const char* ops[] = {"+","-","<",">","<=",">=","=","~=","*","/","\\\\","@","<<","//","&","|"};
                return std::string("arith") + ops[bc - 0x60];
            }
            if (bc <= 0x7F) {
                const char* ops[] = {"at:","at:put:","size","next","nextPut:","atEnd","==","class","~~","value","value:","do:","new","new:","x","y"};
                return std::string("spec") + ops[bc - 0x70];
            }
            if (bc <= 0x8F) return "send0_" + std::to_string(bc & 0x0F);
            if (bc <= 0x9F) return "send1_" + std::to_string(bc & 0x0F);
            if (bc <= 0xAF) return "send2_" + std::to_string(bc & 0x0F);
            if (bc <= 0xB7) return "jump+" + std::to_string((bc & 7) + 1);
            if (bc <= 0xBF) return "jmpT+" + std::to_string((bc & 7) + 1);
            if (bc <= 0xC7) return "jmpF+" + std::to_string((bc & 7) + 1);
            if (bc <= 0xCF) return "popStRecv" + std::to_string(bc & 7);
            if (bc <= 0xD7) return "popStTemp" + std::to_string(bc & 7);
            if (bc == 0xD8) return "pop";
            if (bc == 0xD9) return "trap";
            if (bc <= 0xDF) return "unused" + std::to_string(bc);
            if (bc == 0xE0) return "extA";
            if (bc == 0xE1) return "extB";
            if (bc == 0xE2) return "xPushRecvV";
            if (bc == 0xE3) return "xPushLitV";
            if (bc == 0xE4) return "xPushLitC";
            if (bc == 0xE5) return "xPushTemp";
            if (bc == 0xE7) return "pushArray";
            if (bc == 0xE8) return "pushInt";
            if (bc == 0xE9) return "pushChar";
            if (bc == 0xEA) return "xSend";
            if (bc == 0xEB) return "xSendSup";
            if (bc == 0xEC) return "callMap";
            if (bc == 0xED) return "xJump";
            if (bc == 0xEE) return "xJmpTrue";
            if (bc == 0xEF) return "xJmpFalse";
            if (bc == 0xF0) return "xPopStRecv";
            if (bc == 0xF1) return "xPopStLitV";
            if (bc == 0xF2) return "xPopStTemp";
            if (bc == 0xF3) return "xStRecv";
            if (bc == 0xF4) return "xStLitVar";
            if (bc == 0xF5) return "xStTemp";
            if (bc == 0xF8) return "callPrim";
            if (bc == 0xF9) return "fullBlock";
            if (bc == 0xFA) return "closure";
            if (bc == 0xFB) return "pushTempVec";
            return "0x" + std::to_string(bc);
        };

        uint64_t total = 0;
        for (auto& p : pairs) total += p.count;

        fprintf(stderr, "\n=== BYTECODE PAIR PROFILE (top 50) ===\n");
        fprintf(stderr, "Total pairs: %llu\n\n", (unsigned long long)total);
        fprintf(stderr, "  %-20s %-20s %12s %7s %7s\n", "First", "Second", "Count", "Pct", "Cum");
        double cumPct = 0;
        int shown = std::min((int)pairs.size(), 50);
        for (int i = 0; i < shown; i++) {
            double pct = 100.0 * pairs[i].count / total;
            cumPct += pct;
            fprintf(stderr, "  %-20s %-20s %12llu %6.2f%% %6.2f%%\n",
                    bcName(pairs[i].a).c_str(), bcName(pairs[i].b).c_str(),
                    (unsigned long long)pairs[i].count, pct, cumPct);
        }
        fprintf(stderr, "=== END BYTECODE PAIR PROFILE ===\n\n");
    }
#endif
    #undef DISPATCH_NEXT
    return;
}

void Interpreter::handleForceYield() {
    // Periodic diagnostic: log active method every 10 seconds
    {
        static auto lastDiagTime = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastDiagTime).count();
        if (elapsed >= 10) {
            lastDiagTime = now;
            Oop proc = getActiveProcess();
            Oop prioOop = memory_.fetchPointer(ProcessPriorityIndex, proc);
            int prio = prioOop.isSmallInteger() ? static_cast<int>(prioOop.asSmallInteger()) : -1;
            std::string rcvrClass = "(unknown)";
            if (receiver_.isObject() && !receiver_.isNil()) {
                rcvrClass = memory_.classNameOf(receiver_);
            } else if (receiver_.isSmallInteger()) {
                rcvrClass = "SmallInteger";
            } else if (receiver_.isNil()) {
                rcvrClass = "nil";
            }
            // Try to get method selector from penultimate literal
            std::string selector = "?";
            if (method_.isObject() && !method_.isNil()) {
                int numLits = (int)memory_.numLiteralsOf(method_);
                if (numLits >= 2) {
                    Oop penLit = memory_.fetchPointer(numLits - 2, method_);
                    if (penLit.isObject() && !penLit.isNil()) {
                        int fmt = (int)penLit.asObjectPtr()->format();
                        if (fmt >= 16) {
                            // It's a byte object (likely a Symbol)
                            size_t sz = memory_.byteSizeOf(penLit);
                            if (sz < 200) {
                                selector.clear();
                                for (size_t i = 0; i < sz && i < 60; i++)
                                    selector += (char)memory_.fetchByte(i, penLit);
                            }
                        } else {
                            // Might be AdditionalMethodState - get selector from slot 1
                            Oop sel = memory_.fetchPointer(1, penLit);
                            if (sel.isObject() && !sel.isNil()) {
                                int sfmt = (int)sel.asObjectPtr()->format();
                                if (sfmt >= 16) {
                                    size_t ssz = memory_.byteSizeOf(sel);
                                    if (ssz < 200) {
                                        selector.clear();
                                        for (size_t j = 0; j < ssz && j < 60; j++)
                                            selector += (char)memory_.fetchByte(j, sel);
                                    }
                                }
                            }
                        }
                    }
                }
            }
            fprintf(stderr, "[DIAG] P%d %s>>%s ip=%lld fd=%d\n",
                    prio, rcvrClass.c_str(), selector.c_str(),
                    (long long)ipOffset_, frameDepth_);
            // Show call stack (up to 20 callers)
            try {
                for (size_t fi = 1; fi <= 20 && fi <= frameDepth_; fi++) {
                    SavedFrame& sf = savedFrames_[frameDepth_ - fi];
                    std::string cname = "(?)";
                    if (sf.savedReceiver.isObject() && !sf.savedReceiver.isNil()
                        && memory_.isValidPointer(sf.savedReceiver))
                        cname = memory_.classNameOf(sf.savedReceiver);
                    else if (sf.savedReceiver.isSmallInteger()) cname = "SmallInteger";
                    else if (sf.savedReceiver.isNil()) cname = "nil";
                    std::string ssel = "?";
                    if (sf.savedMethod.isObject() && memory_.isValidPointer(sf.savedMethod))
                        ssel = memory_.selectorOf(sf.savedMethod);
                    fprintf(stderr, "[DIAG]   [-%zu] %s>>%s\n", fi, cname.c_str(), ssel.c_str());
                }
            } catch (...) {
                fprintf(stderr, "[DIAG]   (stack trace failed)\n");
            }
            // Timer state diagnostic
            {
                bool usecArmed = (nextWakeupUsec_ != INT64_MAX && !timerSemaphore_.isNil());
                bool msArmed = (nextWakeupTime_ != 0 && !timerSemaphore_.isNil());
                auto sinceSignal = std::chrono::steady_clock::now() - lastTimerSignalTime_;
                auto sigSecs = std::chrono::duration_cast<std::chrono::seconds>(sinceSignal).count();
                fprintf(stderr, "[DIAG-TIMER] usecArmed=%d msArmed=%d timerWasArmed=%d timerSem=%s "
                        "lastSignal=%llds ago nextUsec=0x%llx nextMs=%lld\n",
                        usecArmed, msArmed, (int)timerWasArmed_,
                        timerSemaphore_.isNil() ? "nil" : "set",
                        (long long)sigSecs,
                        (unsigned long long)nextWakeupUsec_,
                        (long long)nextWakeupTime_);
                if (usecArmed) {
                    // Show how far in the future the wakeup is
                    static constexpr int64_t kSmalltalkEpochOffset = 2177452800LL * 1000000LL;
                    auto nowClock = std::chrono::system_clock::now();
                    int64_t unixUsec = std::chrono::duration_cast<std::chrono::microseconds>(
                        nowClock.time_since_epoch()).count();
                    int64_t currentUsec = unixUsec + kSmalltalkEpochOffset;
                    int64_t deltaUsec = nextWakeupUsec_ - currentUsec;
                    fprintf(stderr, "[DIAG-TIMER] wakeup delta=%lld usec (%.3f sec)\n",
                            (long long)deltaUsec, deltaUsec / 1000000.0);
                }
            }
        }
    }

    // Process forced yield from heartbeat thread.
    // Check scheduler queues for higher-priority or same-priority processes.
    Oop activeProcess = getActiveProcess();
    Oop activePriorityOop = memory_.fetchPointer(ProcessPriorityIndex, activeProcess);
    int activePriority = activePriorityOop.isSmallInteger() ?
                        static_cast<int>(activePriorityOop.asSmallInteger()) : 0;

    Oop nilObj = memory_.nil();
    Oop nextProcess = nilObj;

    {
        Oop schedulerAssoc = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);
        Oop scheduler = memory_.fetchPointer(1, schedulerAssoc);
        Oop schedLists = memory_.fetchPointer(SchedulerProcessListsIndex, scheduler);
        int maxPri = static_cast<int>(schedLists.asObjectPtr()->slotCount());

        // Check for higher priority processes (preemption)
        for (int pri = maxPri; pri > activePriority; pri--) {
            Oop processList = memory_.fetchPointer(pri - 1, schedLists);
            Oop first = memory_.fetchPointer(LinkedListFirstLinkIndex, processList);
            if (first.isObject() && first.rawBits() != nilObj.rawBits()) {
                nextProcess = removeFirstLinkOfList(processList);
                break;
            }
        }

        // Round-robin at same priority level
        if (nextProcess.rawBits() == nilObj.rawBits() &&
            activePriority > 0 && activePriority <= maxPri) {
            Oop processList = memory_.fetchPointer(activePriority - 1, schedLists);
            Oop first = memory_.fetchPointer(LinkedListFirstLinkIndex, processList);
            if (first.isObject() && first.rawBits() != nilObj.rawBits() &&
                first.rawBits() != activeProcess.rawBits()) {
                nextProcess = removeFirstLinkOfList(processList);
            }
        }

        // Aging-based preemption: our VM is much slower than standard Pharo,
        // so CPU-intensive processes can starve lower-priority ones for minutes.
        // MorphicRenderLoop (pri-80) never yields via Delay when cycles > 16ms.
        // FFI struct compilation (pri-79) runs for 5+ minutes.
        //
        // In headless mode: age pri-41+ after 50ms, grace 200ms (75% to lower pri).
        // In GUI mode: age pri-60-79 after 500ms, grace 100ms (~17% to lower pri).
        {
            static uint64_t agingProcBits = 0;
            static int agingProcPri = 0;
            static auto agingStartTime = std::chrono::steady_clock::now();
            static auto agingGraceUntil = std::chrono::steady_clock::now();
            static bool agingInGrace = false;

            auto now = std::chrono::steady_clock::now();

            // During grace period, undo any higher-priority preemption
            if (agingInGrace && nextProcess.isObject() && nextProcess.rawBits() != nilObj.rawBits()) {
                int nextPri = safeProcessPriority(nextProcess);
                if (nextPri > activePriority && now < agingGraceUntil) {
                    // Put the higher-priority process back and keep running
                    addLastLinkToList(nextProcess, memory_.fetchPointer(
                        nextPri - 1, schedLists));
                    nextProcess = nilObj;
                }
            }
            if (agingInGrace && now >= agingGraceUntil) {
                agingInGrace = false;
            }

            // Determine aging thresholds based on mode.
            // In headless mode, don't age the startup process (P79). Session
            // handlers must complete before lower-priority processes run.
            // The old 5ms threshold aged the startup process mid-handler-iteration,
            // causing session startup to never complete.
            bool headless = isHeadless();
            int agingMinPri = headless ? 41 : 60;
            int agingMaxPri = headless ? 78 : 79;  // Exclude P79 startup process
            int agingThresholdMs = headless ? 500 : 500;
            int agingGraceMs = headless ? 500 : 100;

            if (nextProcess.rawBits() == nilObj.rawBits() &&
                activePriority >= agingMinPri && activePriority <= agingMaxPri) {
                if (activeProcess.rawBits() != agingProcBits) {
                    if (activePriority <= agingProcPri || agingProcBits == 0) {
                        agingProcBits = activeProcess.rawBits();
                        agingProcPri = activePriority;
                        agingStartTime = now;
                    }
                } else {
                    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - agingStartTime).count();
                    if (elapsedMs >= agingThresholdMs) {
                        agingStartTime = now;
                        for (int pri = activePriority - 1; pri >= 1; pri--) {
                            Oop processList = memory_.fetchPointer(pri - 1, schedLists);
                            Oop first = memory_.fetchPointer(LinkedListFirstLinkIndex, processList);
                            if (first.isObject() && first.rawBits() != nilObj.rawBits()) {
                                nextProcess = removeFirstLinkOfList(processList);
                                agingGraceUntil = now + std::chrono::milliseconds(agingGraceMs);
                                agingInGrace = true;
                                static int agingLog = 0;
                                if (agingLog++ < 20) {
                                    fprintf(stderr, "[AGING] P%d→P%d (step %llu)\n",
                                            activePriority, pri, (unsigned long long)g_stepNum);
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    if (nextProcess.isObject() && nextProcess.rawBits() != nilObj.rawBits() &&
        nextProcess.rawBits() != activeProcess.rawBits()) {
        putToSleep(activeProcess);
        transferTo(nextProcess);
    }

    if (hasPendingDriverInstall_) {
        executePendingDriverInstall();
        return;
    }

    // Convert pending driver setup to install if ready
    if (hasPendingDriverSetup_ && pendingDriverSetupMethod_.isObject()) {
        Oop nilObj2 = memory_.nil();
        Oop osWindowDriverClass = memory_.findGlobal("OSWindowDriver");
        if (osWindowDriverClass.isObject() && osWindowDriverClass.rawBits() != nilObj2.rawBits()) {
            Oop classPool = memory_.fetchPointer(7, osWindowDriverClass);
            if (classPool.isObject() && classPool.rawBits() != nilObj2.rawBits()) {
                ObjectHeader* poolHdr = classPool.asObjectPtr();
                if (poolHdr->slotCount() >= 2) {
                    Oop assocArray = memory_.fetchPointer(1, classPool);
                    if (assocArray.isObject()) {
                        ObjectHeader* arrayHdr = assocArray.asObjectPtr();
                        for (size_t i = 0; i < arrayHdr->slotCount(); i++) {
                            Oop assoc = memory_.fetchPointer(i, assocArray);
                            if (assoc.isObject() && assoc.rawBits() != nilObj2.rawBits()) {
                                ObjectHeader* assocHdr = assoc.asObjectPtr();
                                if (assocHdr->slotCount() >= 2) {
                                    Oop key = memory_.fetchPointer(0, assoc);
                                    if (key.isObject()) {
                                        ObjectHeader* keyHdr = key.asObjectPtr();
                                        if (keyHdr->isBytesObject() && keyHdr->byteSize() == 7) {
                                            std::string keyName((char*)keyHdr->bytes(), keyHdr->byteSize());
                                            if (keyName == "Current") {
                                                Oop driverInstance = memory_.fetchPointer(1, assoc);
                                                if (driverInstance.isObject() && driverInstance.rawBits() != nilObj2.rawBits()) {
                                                    pendingDriverInstallMethod_ = pendingDriverSetupMethod_;
                                                    pendingDriverInstallReceiver_ = driverInstance;
                                                    hasPendingDriverInstall_ = true;
                                                    pendingDriverMethodNeedsArg_ = false;
                                                }
                                                break;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        hasPendingDriverSetup_ = false;
        pendingDriverSetupMethod_ = Oop::nil();
        if (hasPendingDriverInstall_) {
            executePendingDriverInstall();
        }
    }
}

void Interpreter::checkTimerSemaphore() {
    // Check microsecond timer (primitive 242 - used by DelaySemaphoreScheduler)
    if (nextWakeupUsec_ != INT64_MAX && !timerSemaphore_.isNil()) {
        static constexpr int64_t kSmalltalkEpochOffset = 2177452800LL * 1000000LL;
        auto now = std::chrono::system_clock::now();
        auto epoch = now.time_since_epoch();
        int64_t unixUsec = std::chrono::duration_cast<std::chrono::microseconds>(epoch).count();
        int64_t currentUsec = unixUsec + kSmalltalkEpochOffset;

        if (currentUsec >= nextWakeupUsec_) {
            Oop semaphore = timerSemaphore_;
            lastKnownTimerSemaphore_ = semaphore;  // save for recovery
            timerSemaphore_ = Oop::nil();
            nextWakeupUsec_ = INT64_MAX;
            lastTimerSignalTime_ = std::chrono::steady_clock::now();
            timerWasArmed_ = false;
            schedulerDeathLogged_ = false;
            synchronousSignal(semaphore);
            return;
        }
    }

    // Check millisecond timer (primitive 136 - legacy)
    if (nextWakeupTime_ == 0 || timerSemaphore_.isNil()) {
        // Delay scheduler death detection and recovery
        if (lastTimerSignalTime_.time_since_epoch().count() > 0 && !timerWasArmed_) {
            auto elapsed = std::chrono::steady_clock::now() - lastTimerSignalTime_;
            auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();

            if (secs >= 5 && !schedulerDeathLogged_) {
                schedulerDeathLogged_ = true;
                std::cout << "[DELAY-DEATH] Timer semaphore signaled " << secs
                          << "s ago but scheduler never re-armed."
                          << " Recovery attempt #" << (schedulerRecoveryAttempts_ + 1)
                          << std::endl;

                // Recovery: re-signal the last known timer semaphore.
                // If the scheduler process is still alive but stuck waiting,
                // this will wake it up so it can re-arm the timer.
                // No attempt limit — keep trying indefinitely. A dead Delay
                // scheduler deadlocks the entire system (watchdogs use Delay).
                if (!lastKnownTimerSemaphore_.isNil()) {
                    schedulerRecoveryAttempts_++;
                    if (schedulerRecoveryAttempts_ <= 3) {
                        std::cout << "[DELAY-RECOVERY] Re-signaling timer semaphore 0x"
                                  << std::hex << lastKnownTimerSemaphore_.rawBits()
                                  << std::dec
                                  << " (attempt " << schedulerRecoveryAttempts_ << ")"
                                  << std::endl;
                    }
                    synchronousSignal(lastKnownTimerSemaphore_);
                    // Give it 5 more seconds to re-arm
                    lastTimerSignalTime_ = std::chrono::steady_clock::now();
                    schedulerDeathLogged_ = false;
                }
            }
        }
        return;
    }

    int64_t currentMs = ioMSecs();
    int64_t targetMs = nextWakeupTime_;
    int64_t diff = (currentMs - targetMs) & 0x3FFFFFFF;
    bool timerElapsed = (diff > 0) && (diff < 0x20000000);

    if (timerElapsed) {
        Oop semaphore = timerSemaphore_;
        lastKnownTimerSemaphore_ = semaphore;  // save for recovery
        timerSemaphore_ = Oop::nil();
        nextWakeupTime_ = 0;
        lastTimerSignalTime_ = std::chrono::steady_clock::now();
        timerWasArmed_ = false;
        schedulerDeathLogged_ = false;
        synchronousSignal(semaphore);
    }
}

void Interpreter::synchronousSignal(Oop semaphore) {
    Oop firstLink = memory_.fetchPointer(LinkedListFirstLinkIndex, semaphore);

    static int signalLog = 0;
    if (signalLog < 50) {
        signalLog++;
        Oop activeProcess = getActiveProcess();
        int activePri = safeProcessPriority(activeProcess);
        bool hasWaiter = !(firstLink.isNil() || firstLink.rawBits() == memory_.nil().rawBits());
        if (hasWaiter) {
            int waiterPri = safeProcessPriority(firstLink);
            fprintf(stderr, "[SEM-SIGNAL-%d] sem=0x%llx active-pri=%d waiter=0x%llx waiter-pri=%d\n",
                    signalLog, (unsigned long long)semaphore.rawBits(), activePri,
                    (unsigned long long)firstLink.rawBits(), waiterPri);
        }
    }

    if (firstLink.isNil() || firstLink.rawBits() == memory_.nil().rawBits()) {
        // No processes waiting - increment excessSignals
        Oop excessOop = memory_.fetchPointer(SemaphoreExcessSignalsIndex, semaphore);
        int64_t excess = excessOop.isSmallInteger() ? excessOop.asSmallInteger() : 0;
        memory_.storePointer(SemaphoreExcessSignalsIndex, semaphore,
                            Oop::fromSmallInteger(excess + 1));
    } else {
        // Validate priority BEFORE removing from semaphore to avoid losing processes
        Oop firstProcess = firstLink;
        int processPriority = safeProcessPriority(firstProcess);
        if (processPriority < 0) {
            // Process priority is corrupted — increment excessSignals instead of
            // removing and losing the process. This preserves the semaphore's wait
            // list so the process isn't orphaned (which kills the Delay scheduler).
            fprintf(stderr, "[SIGNAL-SKIP] Skipping corrupted process 0x%llx on semaphore 0x%llx\n",
                    (unsigned long long)firstProcess.rawBits(),
                    (unsigned long long)semaphore.rawBits());
            Oop excessOop = memory_.fetchPointer(SemaphoreExcessSignalsIndex, semaphore);
            int64_t excess = excessOop.isSmallInteger() ? excessOop.asSmallInteger() : 0;
            memory_.storePointer(SemaphoreExcessSignalsIndex, semaphore,
                                Oop::fromSmallInteger(excess + 1));
            return;
        }

        // Safe to remove — priority is valid
        Oop process = removeFirstLinkOfList(semaphore);

        Oop activeProcess = getActiveProcess();
        int activePriority = safeProcessPriority(activeProcess);
        if (activePriority < 0) {
            // Active process corrupted - just transfer to woken process
            transferTo(process);
            return;
        }

        if (processPriority > activePriority) {
            putToSleep(activeProcess);
            transferTo(process);
        } else {
            // Same or lower priority: just add woken process to its priority queue.
            // Per the reference VM's resume: method, synchronousSignal does NOT
            // yield for same-priority processes. Round-robin scheduling is handled
            // by the heartbeat thread's forceYield (every 2ms), which triggers
            // checkForPreemption with same-priority round-robin.
            putToSleep(process);
        }
    }
}

void Interpreter::signalFinalizationIfNeeded() {
    // Signal the finalization semaphore when GC has queued mourners (dead weak
    // objects / fired ephemerons). The finalization process wakes up and reads
    // mourners via primitive 172 (primitiveFetchNextMourner).
    //
    // This is called from step() after GC primitives and periodically.
    // No priority guard — the Cog VM signals regardless of caller priority.
    // The woken finalization process (P50) will preempt only if the active
    // process has lower priority; otherwise it waits in the ready queue.
    if (memory_.pendingFinalizationSignals() <= 0) return;

    memory_.clearPendingFinalizationSignals();

    Oop sema = memory_.specialObject(SpecialObjectIndex::TheFinalizationSemaphore);
    if (!sema.isObject() || sema == memory_.nil()) return;

    synchronousSignal(sema);
}


// ===== HEARTBEAT THREAD =====

void Interpreter::startHeartbeat() {
    if (heartbeatRunning_) return;

    heartbeatRunning_ = true;
    heartbeatThread_ = std::thread([this]() {
      try {
        int tickCount = 0;

        while (heartbeatRunning_) {
            // Sleep for ~1ms between ticks (like official VM heartbeat)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

            tickCount++;

            // Timer semaphore signaling is handled by the main thread in
            // checkTimerSemaphore(). DO NOT manipulate Smalltalk heap objects
            // from this thread — memory_.fetchPointer/storePointer are not
            // thread-safe and cause data races that corrupt process state.

            // Every ~33ms (30fps), request display sync from main thread AND push a timer event
            if (tickCount % 33 == 0) {
                // Do NOT call syncDisplayToSurface() here — it accesses the
                // Smalltalk heap (memory_.fetchPointer) which is not thread-safe.
                // Set flag for the main interpreter loop to handle it.
                pendingDisplaySync_.store(true, std::memory_order_release);

                // Signal the input semaphore to wake up the UI process
                // (Don't push WindowMetrics events — they cause constant resize
                // processing in OSSDL2Driver. Display sync is handled by
                // pendingDisplaySync_ flag in the main interpreter loop.)
                int inputSemaIdx = pharo::gEventQueue.getInputSemaphoreIndex();
                if (inputSemaIdx > 0) {
                    signalExternalSemaphore(inputSemaIdx);
                }
            }

            // Every ~2ms, set force yield flag for round-robin scheduling and preemption.
            // Reference VM heartbeat fires every ~2ms (DEFAULT_BEAT_MS = 2 in heartbeat.c)
            // and forces interrupt checks via stackLimit manipulation.
            // Critical for queue contention tests with many same-priority processes.
            if (tickCount % 2 == 0) {
                forceYield_.store(true, std::memory_order_release);
            }

            // Watchdog: check for stuck VM every 5 seconds
            if (tickCount % 5000 == 0) {
                long long steps = g_watchdogSteps.load(std::memory_order_relaxed);
                bool stuck = (steps == lastHeartbeatSteps_);
                if (stuck) {
                    int ticks = stuckTicks_.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (ticks >= 3) {
                        terminateStuck_.store(true, std::memory_order_release);
                        stuckTicks_.store(0, std::memory_order_relaxed);
                    }
                } else {
                    stuckTicks_.store(0, std::memory_order_relaxed);
                }
                lastHeartbeatSteps_ = steps;
            }
        }

      } catch (const std::exception& e) {
        (void)e;  // Heartbeat thread exception
      } catch (...) {
      }
    });
}

void Interpreter::stopHeartbeat() {
    if (!heartbeatRunning_) return;

    heartbeatRunning_ = false;
    if (heartbeatThread_.joinable()) {
        heartbeatThread_.join();
    }
}

// ===== EXTERNAL SEMAPHORE SIGNALING =====

void Interpreter::signalExternalSemaphore(int index) {
    // Lock-free ring buffer: producer appends index, consumer drains in processPendingSignals.
    // If the buffer is full, the signal is dropped (better than blocking the signaling thread).
    int head = pendingSignalHead_.load(std::memory_order_relaxed);
    int next = (head + 1) % kPendingSignalCapacity;
    if (next == pendingSignalTail_.load(std::memory_order_acquire)) {
        return;  // buffer full — drop signal
    }
    pendingSignals_[head].store(index, std::memory_order_relaxed);
    pendingSignalHead_.store(next, std::memory_order_release);
}

void Interpreter::processPendingSignals() {
    int tail = pendingSignalTail_.load(std::memory_order_relaxed);
    int head = pendingSignalHead_.load(std::memory_order_acquire);
    if (tail == head) return;  // empty

    // Get the external semaphore table once for the whole batch
    Oop semTable = memory_.specialObject(SpecialObjectIndex::ExternalObjectsArray);
    if (semTable.isNil() || !semTable.isObject()) {
        // Drain the queue even if we can't signal
        pendingSignalTail_.store(head, std::memory_order_release);
        return;
    }
    size_t tableSize = memory_.slotCountOf(semTable);

    while (tail != head) {
        int index = pendingSignals_[tail].load(std::memory_order_relaxed);
        tail = (tail + 1) % kPendingSignalCapacity;

        if (index <= 0) continue;
        size_t tableIndex = static_cast<size_t>(index - 1);
        if (tableIndex >= tableSize) continue;

        Oop semaphore = memory_.fetchPointer(tableIndex, semTable);
        if (semaphore.isNil() || !semaphore.isObject()) {
            fprintf(stderr, "[SEMA] processPendingSignals: index=%d → nil/invalid semaphore (tableIndex=%zu, tableSize=%zu)\n",
                    index, tableIndex, tableSize);
            continue;
        }

        synchronousSignal(semaphore);
    }
    pendingSignalTail_.store(tail, std::memory_order_release);
}

void Interpreter::signalSemaphoreDirectly(int externalIndex) {
    Oop semTable = memory_.specialObject(SpecialObjectIndex::ExternalObjectsArray);
    if (semTable.isNil() || !semTable.isObject()) return;
    size_t idx = static_cast<size_t>(externalIndex - 1);
    if (idx >= memory_.slotCountOf(semTable)) return;
    Oop semaphore = memory_.fetchPointer(idx, semTable);
    if (semaphore.isNil() || !semaphore.isObject()) return;
    synchronousSignal(semaphore);
}

// ===== FFI CALLBACK SUPPORT =====

extern int g_callbackSemaphoreIndex;

void Interpreter::enterInterpreterFromCallback(VMCallbackContext* vmcc) {
    // 1. Materialize frame stack (saves current execution to Smalltalk contexts).
    //    This may trigger GC — vmcc is on C heap, so it's safe.
    //    GC SAFETY: protect activeProcess across materializeFrameStack (it allocates).
    Oop savedGcTemp = gcTempOop_;
    gcTempOop_ = getActiveProcess();
    Oop savedCtx = materializeFrameStack();
    Oop activeProcess = gcTempOop_;
    gcTempOop_ = savedGcTemp;

    // 2. Save active process's suspended context
    if (!savedCtx.isNil() && savedCtx.isObject()) {
        memory_.storePointer(ProcessSuspendedContextIndex, activeProcess, savedCtx);
    }

    // 3. Push active process onto SuspendedProcessInCallout linked list
    //    (LIFO stack — head of list is the most recently suspended)
    Oop prevHead = memory_.specialObject(SpecialObjectIndex::SuspendedProcessInCallout);
    memory_.storePointer(ProcessNextLinkIndex, activeProcess, prevHead);
    memory_.setSpecialObject(SpecialObjectIndex::SuspendedProcessInCallout, activeProcess);

    // 4. Push vmcc onto callback context stack
    if (callbackDepth_ < MaxCallbackDepth) {
        callbackContextStack_[callbackDepth_++] = vmcc;
    }

    // 5. Signal callback semaphore to wake handler process
    if (g_callbackSemaphoreIndex > 0) {
        signalSemaphoreDirectly(g_callbackSemaphoreIndex);
    }

    // 6. Find and transfer to highest-priority ready process
    //    (The callback handler process should now be ready from the semaphore signal)
    Oop readyProcess = wakeHighestPriority();
    if (readyProcess.isObject() && !readyProcess.isNil()) {
        setActiveProcess(readyProcess);
        Oop ctx = memory_.fetchPointer(ProcessSuspendedContextIndex, readyProcess);
        memory_.storePointer(ProcessSuspendedContextIndex, readyProcess, memory_.nil());

        // Reset interpreter state for new process
        stackPointer_ = stackBase_;
        frameDepth_ = 0;
        executeFromContext(ctx);
    }

    // 7. Run a NESTED interpret loop on the C stack.
    //    primitiveCallbackReturn sets pendingCallbackReturn_ instead of doing
    //    siglongjmp directly, so the Smalltalk caller can release mutexes and
    //    clean up. We detect the flag after each batch of steps (giving the
    //    Smalltalk code time to finish), restore the original process, and
    //    siglongjmp back to the C trampoline.
    long nestedStepCount = 0;
    static constexpr long kCallbackTimeout = 10000000; // 10M steps ~1s
    while (running_) {
        for (int batch = 0; batch < 1000 && running_; batch++) {
            step();
            nestedStepCount++;
        }

        // Timeout: if the callback handler never calls primitiveCallbackReturn
        // (e.g., because an error killed the handler process), abandon the
        // callback to prevent infinite spinning.
        if (nestedStepCount >= kCallbackTimeout && !pendingCallbackReturn_) {
            // Pop suspended process from SuspendedProcessInCallout (LIFO)
            Oop suspendedProcess = memory_.specialObject(
                SpecialObjectIndex::SuspendedProcessInCallout);
            if (!suspendedProcess.isNil() && suspendedProcess.isObject()) {
                Oop nextInChain = memory_.fetchPointer(
                    ProcessNextLinkIndex, suspendedProcess);
                memory_.setSpecialObject(
                    SpecialObjectIndex::SuspendedProcessInCallout, nextInChain);
                memory_.storePointer(
                    ProcessNextLinkIndex, suspendedProcess, memory_.nil());

                // Restore the original process as active
                setActiveProcess(suspendedProcess);

                Oop ctx = memory_.fetchPointer(
                    ProcessSuspendedContextIndex, suspendedProcess);
                memory_.storePointer(
                    ProcessSuspendedContextIndex, suspendedProcess, memory_.nil());
                stackPointer_ = stackBase_;
                frameDepth_ = 0;
                if (!ctx.isNil() && ctx.isObject()) {
                    executeFromContext(ctx);
                }
            }

            // Return 0 to C by jumping back to the trampoline
            siglongjmp(vmcc->trampoline, 1);
            // DOES NOT RETURN
        }

        // Check for deferred callback return AFTER the batch completes.
        if (pendingCallbackReturn_) {
            VMCallbackContext* retVmcc = pendingCallbackReturn_;
            pendingCallbackReturn_ = nullptr;

            // COOLDOWN: Run extra steps so Smalltalk finishes cleanup.
            // primitiveCallbackReturn returns true inside stackProtect critical:.
            // The critical: block still needs to: release mutex, pop the
            // callbackInvocationStack, signal callbackReturnSemaphore, and
            // the forked process needs to terminate. ~50 bytecodes total,
            // but we give 500 for safety (process switches, GC, etc.).
            for (int cooldown = 0; cooldown < 500 && running_; cooldown++) {
                step();
                nestedStepCount++;
            }

            // Save the current process's execution state and RE-QUEUE it.
            // Without re-queuing, this process becomes permanently lost from
            // the scheduler, causing deadlocks on subsequent callbacks.
            {
                Oop savedGcTemp = gcTempOop_;
                gcTempOop_ = getActiveProcess();
                Oop currentCtx = materializeFrameStack();
                Oop currentProcess = gcTempOop_;
                gcTempOop_ = savedGcTemp;
                if (!currentCtx.isNil() && currentCtx.isObject()) {
                    memory_.storePointer(ProcessSuspendedContextIndex,
                                         currentProcess, currentCtx);
                }
                // Put the process back on its priority's ready queue
                putToSleep(currentProcess);
            }

            // Pop suspended process from SuspendedProcessInCallout (LIFO)
            Oop suspendedProcess = memory_.specialObject(
                SpecialObjectIndex::SuspendedProcessInCallout);
            if (!suspendedProcess.isNil() && suspendedProcess.isObject()) {
                Oop nextInChain = memory_.fetchPointer(
                    ProcessNextLinkIndex, suspendedProcess);
                memory_.setSpecialObject(
                    SpecialObjectIndex::SuspendedProcessInCallout, nextInChain);
                memory_.storePointer(
                    ProcessNextLinkIndex, suspendedProcess, memory_.nil());

                // Restore the original process as active
                setActiveProcess(suspendedProcess);

                // Restore execution state from saved context
                Oop ctx = memory_.fetchPointer(
                    ProcessSuspendedContextIndex, suspendedProcess);
                memory_.storePointer(
                    ProcessSuspendedContextIndex, suspendedProcess, memory_.nil());
                stackPointer_ = stackBase_;
                frameDepth_ = 0;
                if (!ctx.isNil() && ctx.isObject()) {
                    executeFromContext(ctx);
                }
            }

            // siglongjmp back to C (callbackClosureHandler's sigsetjmp)
            siglongjmp(retVmcc->trampoline, 1);
            // DOES NOT RETURN
        }

        if (hasPendingSignals() && !inExtension_) {
            processPendingSignals();
        }
        if (!inExtension_) {
            checkTimerSemaphore();
        }
    }
}

bool Interpreter::step() {
    if (!running_) {
        return false;
    }

    // GC safe point: between bytecodes, no C++ locals hold Oops.
    g_watchdogSubphase = 10;
    if (memory_.needsCompactGC()) {
        memory_.clearCompactGCFlag();
        // Skip ephemeron firing and weak processing during auto-compact GC.
        // This emulates scavenge behavior — a real generational GC wouldn't
        // fire old-space ephemerons during a minor collection. Without this,
        // auto-GC mourns weak key dictionary entries before tests can check
        // the dictionary size (WeakKeyDictionaryTest>>testClearing).
        memory_.fullGC(/* skipEphemerons */ true);
        flushMethodCache();
    }

    // Signal finalization promptly after GC. This one-shot flag fires on the step
    // immediately after a GC primitive (or auto-compact GC), rather than waiting
    // ~1M bytecodes. Without this, weak dictionary tests fail because the P51
    // mourning process never gets CPU time before the P40 test asserts dict.size.
    if (finalizationCheckAfterGC_ && !inExtension_) {
        finalizationCheckAfterGC_ = false;
        signalFinalizationIfNeeded();
    }

    // Check timer and process pending signals periodically.
    // CRITICAL: Skip process-switch-triggering checks when inExtension_ is true.
    // Extension bytes (0xE0/0xE1) set extA_/extB_ which the NEXT bytecode needs.
    // A process switch calls executeFromContext which resets extA_/extB_ = 0,
    // corrupting the next bytecode's argument (e.g., jump offset → IP past method end).
    // The forceYield handler already checks inExtension_, but timer/signal/preemption
    // checks did NOT — causing the non-deterministic "factorial returns receiver" bug.
    {
    stepCheckCounter_++;
    bool periodicCheckDue = (stepCheckCounter_ & 0x3FF) == 0;  // every 1024 steps

    if (periodicCheckDue && inExtension_) {
        // Can't run periodic checks now — defer to next non-extension step.
        // Without this, tight loops whose bytecode count divides evenly into 1024
        // (e.g. `[] repeat` = 2-byte ExtendB+Jump) permanently align the check
        // with extension bytes, starving timer and signal checks forever.
        deferredPeriodicCheck_ = true;
    }

    if ((periodicCheckDue || deferredPeriodicCheck_) && !inExtension_) {
        deferredPeriodicCheck_ = false;
        g_watchdogSubphase = 11;
        checkTimerSemaphore();
        if (hasPendingSignals()) {
            processPendingSignals();
        }

        // VM-level stuck process termination: track cumulative time a
        // low-priority process runs. If any process below P79 accumulates
        // > 90 seconds, terminate it. Uses cumulative time (not continuous)
        // to handle cases where context switches to the Delay scheduler
        // briefly interrupt the stuck process.
        {
            Oop currentActive = getActiveProcess();
            Oop prioOop = memory_.fetchPointer(ProcessPriorityIndex, currentActive);
            int prio = prioOop.isSmallInteger() ? (int)prioOop.asSmallInteger() : 0;

            if (prio >= 80) {
                startupGracePeriod_ = false;
                // High-priority process running — if we were tracking a low-pri
                // process, pause cumulative timer (don't reset)
                if (trackedProcess_.rawBits() == currentActive.rawBits()) {
                    trackedProcess_ = Oop::nil();  // stop tracking
                }
            } else if (!startupGracePeriod_ && prio < 79) {
                if (currentActive.rawBits() != trackedProcess_.rawBits()) {
                    // New low-priority process — start fresh tracking
                    trackedProcess_ = currentActive;
                    cumulativeMs_ = 0;
                    lastResumeTime_ = std::chrono::steady_clock::now();
                    trackStartTime_ = lastResumeTime_;
                } else {
                    // Same process still running
                    auto now = std::chrono::steady_clock::now();
                    auto wallMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - trackStartTime_).count();
                    // Use wall time (simpler, accounts for all elapsed time)
                    if (wallMs >= 600000) {
                        fprintf(stderr, "[VM-TIMEOUT] Process 0x%llx at P%d stuck for %lldms — terminating\n",
                                (unsigned long long)currentActive.rawBits(), prio, (long long)wallMs);
                        trackedProcess_ = Oop::nil();
                        // Mark process as terminated (clear suspendedContext)
                        memory_.storePointer(ProcessSuspendedContextIndex, currentActive, Oop::nil());
                        // Try to find another process to run
                        Oop nextProc = wakeHighestPriority();
                        if (!nextProc.isNil() && nextProc.isObject()) {
                            transferTo(nextProc);
                        }
                    }
                }
            }
        }

        g_watchdogSubphase = 12;

        // Preemption check: if a higher-priority process is waiting in the
        // scheduler queues, switch to it. Run at lower frequency (every 64K
        // steps, ~65ms) to avoid excessive context switching when a spin-wait
        // watchdog is on the ready queue. Timer/signal checks still run every
        // 1024 steps for responsiveness.
        if ((stepCheckCounter_ & 0xFFFF) == 0) {
            checkForPreemption();
        }

        // Signal finalization periodically for auto-GC mourners (not handled by the
        // one-shot flag which only fires after explicit GC primitives 130/131).
        signalFinalizationIfNeeded();

        // Display sync requested by heartbeat thread — safe to access heap here
        if (pendingDisplaySync_.load(std::memory_order_acquire)) {
            pendingDisplaySync_.store(false, std::memory_order_release);
            g_watchdogPhase.store(4, std::memory_order_relaxed);
            syncDisplayToSurface();
            g_watchdogPhase.store(0, std::memory_order_relaxed);
        }
    }
    }

    // If the previous step slept in relinquishProcessor, report as idle
    if (relinquishSlept_) {
        relinquishSlept_ = false;
        return false;  // Signal idle to caller
    }


    stepCountForDriver_++;

    // Process any pending external semaphore signals (from heartbeat/events).
    // Skip if in extension byte sequence to protect extA_/extB_.
    if (!inExtension_ && hasPendingSignals()) {
        processPendingSignals();
    }

    // Periodic preemption check - every 10000 bytecodes, check if we should
    // yield to a higher-priority or same-priority runnable process.
    // Skip if in extension byte sequence to protect extA_/extB_.
    bytecodeCount_++;
    if (bytecodeCount_ % 10000 == 0 && !inExtension_) {
        checkForPreemption();
    }

    // If IP has run past the end of bytecodes, force a return
    if (instructionPointer_ >= bytecodeEnd_) {
        returnValue(receiver_);
        return running_;
    }

    // NOTE: Do NOT reset extA_/extB_ here!
    // In Sista V1, extension bytecodes (0xE0/0xE1) set these values, then the
    // NEXT bytecode uses them. The consuming bytecodes reset them after use.
    // Resetting here would break extension byte chains.

    // Track step count
    g_stepNum++;
    // Update watchdog steps (used by heartbeat thread to detect stuck processes).
    // Must be updated here because test_load_image calls step() directly,
    // not interpret() which has its own loopCount.
    g_watchdogSteps.store(g_stepNum, std::memory_order_relaxed);

    // cannotReturn: deadline check. When a process hits cannotReturn:, we give
    // the Smalltalk error handler a step budget (set in returnFromMethod). If the
    // budget expires and the same process is still running, terminate it. This
    // prevents high-priority processes (like the P80 Delay scheduler) from
    // monopolizing the CPU during error handling and starving lower-priority
    // processes (like the P40 test runner/watchdog).
    if (cannotReturnDeadline_ > 0 && g_stepNum >= cannotReturnDeadline_ && !inExtension_) {
        Oop currentProcess = getActiveProcess();
        if (currentProcess.rawBits() == lastCannotReturnProcess_.rawBits()) {
            cannotReturnCount_ = 0;
            cannotReturnDeadline_ = 0;
            lastCannotReturnProcess_ = Oop::nil();
            lastCannotReturnCtx_ = Oop::nil();
            terminateCurrentProcess();
            if (tryReschedule()) {
                return running_;
            }
            if (bootstrapStartup()) {
                return running_;
            }
            stopVM("No runnable processes after cannotReturn: deadline termination");
        } else {
            // Different process is running — the cannotReturn: process was already
            // handled (switched away). Clear the deadline.
            cannotReturnDeadline_ = 0;
        }
    }

    // Check for forced process yield BEFORE fetching the next bytecode.
    // CRITICAL: Must happen before fetchByte() because fetchByte() advances
    // instructionPointer_. If we yield after fetching, the saved PC will point
    // past the fetched bytecode, causing it to be SKIPPED when the process
    // is later restored — leading to expression stack corruption and DNUs.
    g_watchdogSubphase = 14;
    bool shouldYield = forceYield_.load(std::memory_order_acquire);
    if (shouldYield) {
        // Per Cog VM: suppress context switch after activating methods with
        // primitive 198 (ensure:/ifCurtailed:). These methods must run their
        // setup bytecodes atomically to establish unwind protection.
        if (suppressContextSwitch_) {
            suppressContextSwitch_ = false;
            // Don't consume forceYield - retry on next step
            goto skip_yield;
        }
        // Don't yield between extension bytes (0xE0/0xE1) and their target bytecode.
        if (inExtension_) {
            // Don't consume forceYield - retry on next step
            goto skip_yield;
        }
        forceYield_.store(false, std::memory_order_release);
        Oop activeProcess = getActiveProcess();
        Oop activePriorityOop = memory_.fetchPointer(ProcessPriorityIndex, activeProcess);
        int activePriority = activePriorityOop.isSmallInteger() ?
                            static_cast<int>(activePriorityOop.asSmallInteger()) : 0;

        Oop nilObj = memory_.nil();
        Oop nextProcess = nilObj;

        {
            Oop schedulerAssoc = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);
            Oop scheduler = memory_.fetchPointer(1, schedulerAssoc);
            Oop schedLists = memory_.fetchPointer(SchedulerProcessListsIndex, scheduler);
            int maxPri = static_cast<int>(schedLists.asObjectPtr()->slotCount());

            // First: check for higher priority processes (preemption).
            // This is critical for timer/Delay scheduler to preempt test processes.
            for (int pri = maxPri; pri > activePriority; pri--) {
                Oop processList = memory_.fetchPointer(pri - 1, schedLists);
                Oop first = memory_.fetchPointer(LinkedListFirstLinkIndex, processList);
                if (first.isObject() && first.rawBits() != nilObj.rawBits()) {
                    nextProcess = removeFirstLinkOfList(processList);
                    break;
                }
            }

            // Then: round-robin at same priority level
            if (nextProcess.rawBits() == nilObj.rawBits() &&
                activePriority > 0 && activePriority <= maxPri) {
                Oop processList = memory_.fetchPointer(activePriority - 1, schedLists);
                Oop first = memory_.fetchPointer(LinkedListFirstLinkIndex, processList);
                if (first.isObject() && first.rawBits() != nilObj.rawBits() &&
                    first.rawBits() != activeProcess.rawBits()) {
                    nextProcess = removeFirstLinkOfList(processList);
                }
            }

            // Aging-based preemption: same logic as handleForceYield().
            // Needed here because step() is the yield path for JIT execution.
            // (Shares static state with handleForceYield's aging variables.)
        }

        bool foundProcess = nextProcess.isObject() &&
                           nextProcess.rawBits() != nilObj.rawBits() &&
                           nextProcess.rawBits() != activeProcess.rawBits();

        if (foundProcess) {
            int nextPri = safeProcessPriority(nextProcess);
            static int yieldLog = 0;
            if (yieldLog++ < 20) {
                fprintf(stderr, "[YIELD] step() P%d→P%d (0x%llx→0x%llx)\n",
                        activePriority, nextPri,
                        (unsigned long long)activeProcess.rawBits(),
                        (unsigned long long)nextProcess.rawBits());
            }
            putToSleep(activeProcess);
            transferTo(nextProcess);
        }

        if (hasPendingDriverInstall_) {
            executePendingDriverInstall();
            return running_;
        }

        // Convert pending driver setup to install if ready
        if (hasPendingDriverSetup_ && pendingDriverSetupMethod_.isObject()) {
            Oop nilObj2 = memory_.nil();
            Oop osWindowDriverClass = memory_.findGlobal("OSWindowDriver");
            if (osWindowDriverClass.isObject() && osWindowDriverClass.rawBits() != nilObj2.rawBits()) {
                Oop classPool = memory_.fetchPointer(7, osWindowDriverClass);
                if (classPool.isObject() && classPool.rawBits() != nilObj2.rawBits()) {
                    ObjectHeader* poolHdr = classPool.asObjectPtr();
                    if (poolHdr->slotCount() >= 2) {
                        Oop assocArray = memory_.fetchPointer(1, classPool);
                        if (assocArray.isObject()) {
                            ObjectHeader* arrayHdr = assocArray.asObjectPtr();
                            for (size_t i = 0; i < arrayHdr->slotCount(); i++) {
                                Oop assoc = memory_.fetchPointer(i, assocArray);
                                if (assoc.isObject() && assoc.rawBits() != nilObj2.rawBits()) {
                                    ObjectHeader* assocHdr = assoc.asObjectPtr();
                                    if (assocHdr->slotCount() >= 2) {
                                        Oop key = memory_.fetchPointer(0, assoc);
                                        if (key.isObject()) {
                                            ObjectHeader* keyHdr = key.asObjectPtr();
                                            if (keyHdr->isBytesObject() && keyHdr->byteSize() == 7) {
                                                std::string keyName((char*)keyHdr->bytes(), keyHdr->byteSize());
                                                if (keyName == "Current") {
                                                    Oop driverInstance = memory_.fetchPointer(1, assoc);
                                                    if (driverInstance.isObject() && driverInstance.rawBits() != nilObj2.rawBits()) {
                                                        pendingDriverInstallMethod_ = pendingDriverSetupMethod_;
                                                        pendingDriverInstallReceiver_ = driverInstance;
                                                        hasPendingDriverInstall_ = true;
                                                        pendingDriverMethodNeedsArg_ = false;
                                                    }
                                                    break;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            hasPendingDriverSetup_ = false;
            pendingDriverSetupMethod_ = Oop::nil();
            if (hasPendingDriverInstall_) {
                executePendingDriverInstall();
                return running_;
            }
        }
    }
skip_yield:

    // VM safety: terminate a process that the watchdog flagged as stuck
    if (terminateStuck_.load(std::memory_order_acquire)) {
        terminateStuck_.store(false, std::memory_order_relaxed);
        terminateAndSwitchProcess();
        return running_;
    }

    uint8_t bytecode = fetchByte();
    lastBytecode_ = bytecode;
    g_watchdogLastBytecode = bytecode;
    g_watchdogSubphase = 15;

    inExtension_ = false;

    dispatchBytecode(bytecode);

    return running_;
}

ExecuteResult Interpreter::stepDetailed() {
    if (!running_) {
        return ExecuteResult::Idle;
    }

    // Process any pending external semaphore signals (from heartbeat/events)
    if (hasPendingSignals()) {
        processPendingSignals();
    }

    // Check if we've run past the end of bytecodes
    if (instructionPointer_ >= bytecodeEnd_) {
        returnValue(receiver_);
        return running_ ? ExecuteResult::Active : ExecuteResult::Idle;
    }

    // Track what we're about to do
    uint8_t bytecode = fetchByte();

    // Check if this is a send bytecode (message send) per Sista V1 spec
    bool isSend = (bytecode >= 0x60 && bytecode <= 0x6F) ||  // Send Arithmetic Message
                  (bytecode >= 0x70 && bytecode <= 0x7F) ||  // Send Special Message
                  (bytecode >= 0x80 && bytecode <= 0x8F) ||  // Send Literal Selector, 0 args
                  (bytecode >= 0x90 && bytecode <= 0x9F) ||  // Send Literal Selector, 1 arg
                  (bytecode >= 0xA0 && bytecode <= 0xAF) ||  // Send Literal Selector, 2 args
                  bytecode == 0xEA || bytecode == 0xEB;       // Extended send / super send

    // Reset primitive tracking before dispatch
    lastPrimitiveIndex_ = 0;

    dispatchBytecode(bytecode);

    if (!running_) {
        return ExecuteResult::Idle;
    }

    // Check if a primitive was executed
    if (lastPrimitiveIndex_ > 0) {
        return ExecuteResult::PrimitiveExecuted;
    }

    // Check if a message was sent
    if (isSend) {
        return ExecuteResult::MessageSent;
    }

    return ExecuteResult::Active;
}

// ===== BYTECODE DISPATCH =====

void Interpreter::dispatchBytecode(uint8_t bytecode) {
    if constexpr (ENABLE_DEBUG_LOGGING) {
        recentBytecodes_[recentBytecodeIdx_ % 256] = bytecode;
        recentBytecodeIdx_++;
    }
    // ========================================================================
    // SISTA V1 BYTECODE DISPATCH (Pharo 10+)
    // Flat switch for O(1) jump-table dispatch. V3PlusClosures support removed.
    // Based on EncoderForSistaV1 specification.
    // ========================================================================

    switch (bytecode) {

    // 0x00-0x0F: Push Receiver Variable 0-15
    case 0x00: case 0x01: case 0x02: case 0x03:
    case 0x04: case 0x05: case 0x06: case 0x07:
    case 0x08: case 0x09: case 0x0A: case 0x0B:
    case 0x0C: case 0x0D: case 0x0E: case 0x0F:
        pushReceiverVariable(bytecode);
        break;

    // 0x10-0x1F: Push Literal Variable 0-15 (dereference Association)
    case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1A: case 0x1B:
    case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        pushLiteralVariable(bytecode - 0x10);
        break;

    // 0x20-0x3F: Push Literal Constant 0-31
    case 0x20: case 0x21: case 0x22: case 0x23:
    case 0x24: case 0x25: case 0x26: case 0x27:
    case 0x28: case 0x29: case 0x2A: case 0x2B:
    case 0x2C: case 0x2D: case 0x2E: case 0x2F:
    case 0x30: case 0x31: case 0x32: case 0x33:
    case 0x34: case 0x35: case 0x36: case 0x37:
    case 0x38: case 0x39: case 0x3A: case 0x3B:
    case 0x3C: case 0x3D: case 0x3E: case 0x3F:
        pushLiteralConstant(bytecode - 0x20);
        break;

    // 0x40-0x4B: Push Temp 0-11
    case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44: case 0x45: case 0x46: case 0x47:
        pushTemporary(bytecode - 0x40);
        break;
    case 0x48: case 0x49: case 0x4A: case 0x4B:
        pushTemporary(8 + bytecode - 0x48);
        break;

    // 0x4C-0x4F: Push specials
    case 0x4C: push(receiver_); break;              // push self
    case 0x4D: push(memory_.trueObject()); break;   // push true
    case 0x4E: push(memory_.falseObject()); break;  // push false
    case 0x4F: push(memory_.nil()); break;           // push nil

    // 0x50: Push 0
    case 0x50: push(Oop::fromSmallInteger(0)); break;
    // 0x51: Push 1
    case 0x51: push(Oop::fromSmallInteger(1)); break;
    // 0x52: Push thisContext / thisProcess
    case 0x52: {
        int savedExtB = extB_;
        extB_ = 0;
        if (savedExtB == 1) {
            push(getActiveProcess());
            break;
        }
        // Push thisContext - must materialize inline frames first
        Oop contextToPush = activeContext_;
        if (frameDepth_ > 0) {
            contextToPush = materializeFrameStack();
            activeContext_ = contextToPush;
            currentFrameMaterializedCtx_ = memory_.nil();
            frameDepth_ = 0;
        }
        push(contextToPush);
        break;
    }
    // 0x53: Duplicate top
    case 0x53: push(stackTop()); break;

    // 0x54-0x57: UNASSIGNED
    case 0x54: case 0x55: case 0x56: case 0x57:
        break;

    // 0x58: Return self
    case 0x58:
        push(receiver_);
        returnFromMethod();
        break;
    // 0x59: Return true
    case 0x59:
        push(memory_.trueObject());
        returnFromMethod();
        break;
    // 0x5A: Return false
    case 0x5A:
        push(memory_.falseObject());
        returnFromMethod();
        break;
    // 0x5B: Return nil
    case 0x5B:
        push(memory_.nil());
        returnFromMethod();
        break;
    // 0x5C: Return top
    case 0x5C:
        returnFromMethod();
        break;
    // 0x5D: BlockReturn nil
    case 0x5D: {
        bool inFullBlock = (method_.isObject() && method_.rawBits() > 0x10000 &&
                            method_.asObjectPtr()->classIndex() == compiledBlockClassIndex_);
        if (inFullBlock) {
            returnValue(memory_.nil());
        } else {
            push(memory_.nil());
            if (extB_ != 0) {
                instructionPointer_ += extB_;
                extB_ = 0;
            }
        }
        break;
    }
    // 0x5E: BlockReturn top
    case 0x5E: {
        int enclosingLevels = extA_;
        int jumpDist = extB_;
        extA_ = 0;
        extB_ = 0;
        if (enclosingLevels > 0) {
            returnFromBlock();
        } else {
            bool inFullBlock = (method_.isObject() && method_.rawBits() > 0x10000 &&
                                method_.asObjectPtr()->classIndex() == compiledBlockClassIndex_);
            if (!inFullBlock && frameDepth_ > 10) {
                static int blockRetLog = 0;
                if (blockRetLog++ < 5) {
                    uint32_t methodClsIdx = method_.isObject() ? method_.asObjectPtr()->classIndex() : 9999;
                    fprintf(stderr, "[BLOCKRET-FAIL] #%d: inFullBlock=false method_clsIdx=%u compiledBlockClassIndex_=%u fd=%zu method=#%s\n",
                            blockRetLog, methodClsIdx, compiledBlockClassIndex_,
                            frameDepth_, memory_.selectorOf(method_).c_str());
                    fflush(stderr);
                }
            }
            if (inFullBlock) {
                Oop value = pop();
                returnValue(value);
            } else {
                instructionPointer_ += jumpDist;
            }
        }
        break;
    }
    // 0x5F: Nop
    case 0x5F:
        break;

    // 0x60-0x6F: Send Arithmetic Message 0-15
    // (+, -, <, >, <=, >=, =, ~=, *, /, \\, @, bitShift:, //, bitAnd:, bitOr:)
    case 0x60: case 0x61: case 0x62: case 0x63:
    case 0x64: case 0x65: case 0x66: case 0x67:
    case 0x68: case 0x69: case 0x6A: case 0x6B:
    case 0x6C: case 0x6D: case 0x6E: case 0x6F:
        sendArithmetic(bytecode - 0x60);
        break;

    // 0x70-0x7F: Send Special Message 16-31
    // (at:, at:put:, size, next, nextPut:, atEnd, ==, class, ~~, value, value:, do:, new, new:, x, y)
    // 0x79 (value) and 0x7A (value:) have fast paths for FullBlockClosures.
    case 0x79: {
        // value (0 args) — fast path for FullBlockClosure
        Oop rcvr = stackValue(0);
        if (rcvr.isObject() && rcvr.rawBits() > 0x10000 &&
            rcvr.asObjectPtr()->classIndex() == fullBlockClosureClassIndex_) {
            argCount_ = 0;
            primitiveFailed_ = false;
            primFailCode_ = 0;
            if (primitiveFullClosureValue(0) == PrimitiveResult::Success)
                break;
        }
        commonSend(9);
        break;
    }
    case 0x7A: {
        // value: (1 arg) — fast path for FullBlockClosure
        Oop rcvr = stackValue(1);
        if (rcvr.isObject() && rcvr.rawBits() > 0x10000 &&
            rcvr.asObjectPtr()->classIndex() == fullBlockClosureClassIndex_) {
            argCount_ = 1;
            primitiveFailed_ = false;
            primFailCode_ = 0;
            if (primitiveFullClosureValue(1) == PrimitiveResult::Success)
                break;
        }
        commonSend(10);
        break;
    }
    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x7B: case 0x7C: case 0x7D:
    case 0x7E: case 0x7F:
        commonSend(bytecode - 0x70);
        break;

    // 0x80-0x8F: Send literal selector 0-15 with 0 args
    case 0x80: case 0x81: case 0x82: case 0x83:
    case 0x84: case 0x85: case 0x86: case 0x87:
    case 0x88: case 0x89: case 0x8A: case 0x8B:
    case 0x8C: case 0x8D: case 0x8E: case 0x8F:
        sendSelector(literal(bytecode & 0x0F), 0);
        break;

    // 0x90-0x9F: Send literal selector 0-15 with 1 arg
    case 0x90: case 0x91: case 0x92: case 0x93:
    case 0x94: case 0x95: case 0x96: case 0x97:
    case 0x98: case 0x99: case 0x9A: case 0x9B:
    case 0x9C: case 0x9D: case 0x9E: case 0x9F:
        sendSelector(literal(bytecode & 0x0F), 1);
        break;

    // 0xA0-0xAF: Send literal selector 0-15 with 2 args
    case 0xA0: case 0xA1: case 0xA2: case 0xA3:
    case 0xA4: case 0xA5: case 0xA6: case 0xA7:
    case 0xA8: case 0xA9: case 0xAA: case 0xAB:
    case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        sendSelector(literal(bytecode & 0x0F), 2);
        break;

    // 0xB0-0xB7: Short unconditional jump (1-8 bytes forward)
    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        shortJump((bytecode & 0x07) + 1);
        break;

    // 0xB8-0xBF: Short conditional jump if true (1-8 bytes)
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        shortJumpIfTrue((bytecode & 0x07) + 1);
        break;

    // 0xC0-0xC7: Short conditional jump if false (1-8 bytes)
    case 0xC0: case 0xC1: case 0xC2: case 0xC3:
    case 0xC4: case 0xC5: case 0xC6: case 0xC7:
        shortJumpIfFalse((bytecode & 0x07) + 1);
        break;

    // 0xC8-0xCF: Pop and Store Receiver Variable 0-7
    case 0xC8: case 0xC9: case 0xCA: case 0xCB:
    case 0xCC: case 0xCD: case 0xCE: case 0xCF: {
        Oop value = pop();
        setReceiverInstVar(bytecode & 0x07, value);
        break;
    }

    // 0xD0-0xD7: Pop and Store Temp 0-7
    case 0xD0: case 0xD1: case 0xD2: case 0xD3:
    case 0xD4: case 0xD5: case 0xD6: case 0xD7: {
        Oop value = pop();
        setTemporary(bytecode & 0x07, value);
        break;
    }

    // 0xD8: Pop stack (discard top)
    case 0xD8:
        pop();
        break;

    // 0xD9: Unconditional trap (debugging)
    case 0xD9:
        stopVM("Unconditional trap bytecode 0xD9");
        break;

    // 0xDA-0xDF: Reserved / no-op
    case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        break;

    // ====== 2-byte bytecodes: Extensions and Push operations (0xE0-0xE7) ======

    case 0xE0: { // Extend A (unsigned)
        uint8_t extByte = fetchByte();
        extA_ = (extA_ << 8) | extByte;
        inExtension_ = true;
        break;
    }
    case 0xE1: { // Extend B (signed)
        uint8_t extByte = fetchByte();
        if (extByte >= 128)
            extB_ = (extB_ << 8) | extByte | 0xFFFFFF00;
        else
            extB_ = (extB_ << 8) | extByte;
        inExtension_ = true;
        break;
    }
    case 0xE2: { // Push Receiver Variable #i (+ extA * 256)
        uint8_t indexByte = fetchByte();
        int fullIndex = (extA_ << 8) | indexByte;
        extA_ = 0;
        pushReceiverVariable(fullIndex);
        break;
    }
    case 0xE3: { // Push Literal Variable #i (+ extA * 256)
        uint8_t indexByte = fetchByte();
        int fullIndex = (extA_ << 8) | indexByte;
        extA_ = 0;
        pushLiteralVariable(fullIndex);
        break;
    }
    case 0xE4: { // Push Literal Constant #i (+ extA * 256)
        uint8_t indexByte = fetchByte();
        int fullIndex = (extA_ << 8) | indexByte;
        extA_ = 0;
        pushLiteralConstant(fullIndex);
        break;
    }
    case 0xE5: { // Push Temporary Variable #i
        uint8_t indexByte = fetchByte();
        pushTemporary(indexByte);
        break;
    }
    case 0xE6: // UNASSIGNED (was pushNClosureTemps)
        fetchByte();
        break;
    case 0xE7: { // Push Array (j=0) or Pop into Array (j=1)
        uint8_t desc = fetchByte();
        int arraySize = desc & 0x7F;
        bool popIntoArray = (desc >> 7) != 0;
        Oop arrayClass = memory_.specialObject(SpecialObjectIndex::ClassArray);
        uint32_t classIndex = memory_.indexOfClass(arrayClass);
        Oop array = memory_.allocateSlots(classIndex, arraySize, ObjectFormat::Indexable);
        if constexpr (ENABLE_DEBUG_LOGGING) {
            static FILE* e7Log = nullptr;
            static int e7Count = 0;
            if (!e7Log) e7Log = nullptr;
            if (e7Log && e7Count < 50) {
                e7Count++;
                std::string methodSel = "<unknown>";
                if (method_.isObject() && method_.rawBits() > 0x10000) {
                    ObjectHeader* mHdr = method_.asObjectPtr();
                    if (mHdr->isCompiledMethod()) {
                        Oop hdr = memory_.fetchPointer(0, method_);
                        if (hdr.isSmallInteger()) {
                            size_t numLits = hdr.asSmallInteger() & 0x7FFF;
                            if (numLits >= 2) {
                                Oop selLit = memory_.fetchPointer(numLits - 1, method_);
                                if (selLit.isObject() && selLit.rawBits() > 0x10000) {
                                    ObjectHeader* slHdr = selLit.asObjectPtr();
                                    if (slHdr->isBytesObject() && slHdr->byteSize() < 50) {
                                        methodSel = std::string((char*)slHdr->bytes(), slHdr->byteSize());
                                    }
                                }
                            }
                        }
                    }
                }
                fprintf(e7Log, "[E7 #%d] Created Array size=%d popInto=%s in #%s\n",
                        e7Count, arraySize, popIntoArray ? "YES" : "NO", methodSel.c_str());
                fprintf(e7Log, "  array=0x%llx\n", (unsigned long long)array.rawBits());
                fflush(e7Log);
            }
        }
        if (popIntoArray) {
            for (int i = arraySize - 1; i >= 0; i--)
                memory_.storePointer(i, array, pop());
        }
        push(array);
        break;
    }

    // ====== 2-byte bytecodes: Push/Send/Jump (0xE8-0xEF) ======

    case 0xE8: { // Push Integer #i (+ extB * 256, signed)
        uint8_t intByte = fetchByte();
        int value = intByte + static_cast<int>(static_cast<unsigned int>(extB_) << 8);
        extB_ = 0;
        push(Oop::fromSmallInteger(value));
        break;
    }
    case 0xE9: { // Push Character #i (+ extB * 256)
        uint8_t charByte = fetchByte();
        int codePoint = charByte + static_cast<int>(static_cast<unsigned int>(extB_) << 8);
        extB_ = 0;
        push(Oop::fromCharacter(codePoint));
        break;
    }
    case 0xEA: { // Send Literal Selector #iiiii (+ extA*32) with jjj (+ extB*8) args
        uint8_t desc = fetchByte();
        int selectorIndex = ((extA_ << 5) | (desc >> 3)) & 0xFFFF;
        int numArgs = ((extB_ << 3) | (desc & 0x07)) & 0xFF;
        extA_ = 0;
        extB_ = 0;
        sendSelector(literal(selectorIndex), numArgs);
        break;
    }
    case 0xEB: { // Send To Superclass
        uint8_t desc = fetchByte();
        int selectorIndex = ((extA_ << 5) | (desc >> 3)) & 0xFFFF;
        int effectiveExtB = extB_;
        extA_ = 0;
        extB_ = 0;
        Oop selector = literal(selectorIndex);
        Oop lookupClass;

        if (effectiveExtB >= 64) {
            // Directed super send (FullBlockClosures)
            int numArgs = (((effectiveExtB - 64) << 3) | (desc & 0x07)) & 0xFF;
            Oop definingClass = pop();
            lookupClass = superclassOf(definingClass);
            Oop method = lookupMethod(selector, lookupClass);
            if (method.isNil()) {
                sendDoesNotUnderstand(selector, numArgs);
            } else {
                int primIdx = primitiveIndexOf(method);
                if (primIdx > 0) {
                    argCount_ = numArgs;
                    primitiveFailed_ = false;
                    primFailCode_ = 0;
                    newMethod_ = method;
                    if (executePrimitive(primIdx, numArgs) == PrimitiveResult::Success) {
#if PHARO_JIT_ENABLED
                        patchJITICAfterSend(method, receiver_, selector);
#endif
                        break;
                    }
                }
#if PHARO_JIT_ENABLED
                patchJITICAfterSend(method, receiver_, selector);
#endif
                activateMethod(method, numArgs);
            }
        } else {
            // Normal super send
            int numArgs = ((effectiveExtB << 3) | (desc & 0x07)) & 0xFF;
            Oop methodClass = methodClassOf(method_);
            if (methodClass.isNil() || !methodClass.isObject())
                lookupClass = superclassOf(memory_.classOf(receiver_));
            else
                lookupClass = superclassOf(methodClass);
            Oop method = lookupMethod(selector, lookupClass);
            if (method.isNil()) {
                sendDoesNotUnderstand(selector, numArgs);
            } else {
                int primIdx = primitiveIndexOf(method);
                if (primIdx > 0) {
                    argCount_ = numArgs;
                    primitiveFailed_ = false;
                    primFailCode_ = 0;
                    newMethod_ = method;
                    if (executePrimitive(primIdx, numArgs) == PrimitiveResult::Success) {
#if PHARO_JIT_ENABLED
                        patchJITICAfterSend(method, receiver_, selector);
#endif
                        break;
                    }
                }
#if PHARO_JIT_ENABLED
                patchJITICAfterSend(method, receiver_, selector);
#endif
                activateMethod(method, numArgs);
            }
        }
        break;
    }
    case 0xEC: { // Call Mapped Inlined Primitive #i
        uint8_t primByte = fetchByte();
        (void)primByte; // Interpreter fallback — JIT handles these
        break;
    }
    case 0xED: { // Jump #i (+ extB * 256, signed)
        uint8_t offsetByte = fetchByte();
        int offset = offsetByte + static_cast<int>(static_cast<unsigned int>(extB_) << 8);
        extB_ = 0;
        instructionPointer_ += offset;
        break;
    }
    case 0xEE: { // Pop and Jump On True #i (+ extB * 256)
        uint8_t offsetByte = fetchByte();
        int offset = offsetByte + static_cast<int>(static_cast<unsigned int>(extB_) << 8);
        extA_ = 0;
        extB_ = 0;
        Oop value = pop();
        if (isTrue(value)) {
            instructionPointer_ += offset;
        } else if (!isFalse(value)) {
            push(value);
            sendMustBeBoolean(value);
        }
        break;
    }
    case 0xEF: { // Pop and Jump On False #i (+ extB * 256)
        uint8_t offsetByte = fetchByte();
        int offset = offsetByte + static_cast<int>(static_cast<unsigned int>(extB_) << 8);
        extA_ = 0;
        extB_ = 0;
        Oop value = pop();
        if (isFalse(value)) {
            instructionPointer_ += offset;
        } else if (!isTrue(value)) {
            push(value);
            sendMustBeBoolean(value);
        }
        break;
    }

    // ====== 2-byte bytecodes: Store operations (0xF0-0xF7) ======

    case 0xF0: { // Pop and Store Receiver Variable #i (+ extA * 256)
        uint8_t indexByte = fetchByte();
        int fullIndex = (extA_ << 8) | indexByte;
        extA_ = 0;
        Oop value = pop();
        setReceiverInstVar(fullIndex, value);
        break;
    }
    case 0xF1: { // Pop and Store Literal Variable #i (+ extA * 256)
        uint8_t indexByte = fetchByte();
        int fullIndex = (extA_ << 8) | indexByte;
        extA_ = 0;
        Oop value = pop();
        Oop assoc = literal(fullIndex);
        if (assoc.isObject())
            memory_.storePointer(1, assoc, value);
        break;
    }
    case 0xF2: { // Pop and Store Temporary Variable #i
        uint8_t indexByte = fetchByte();
        Oop value = pop();
        setTemporary(indexByte, value);
        break;
    }
    case 0xF3: { // Store Receiver Variable #i (+ extA * 256) - no pop
        uint8_t indexByte = fetchByte();
        int fullIndex = (extA_ << 8) | indexByte;
        extA_ = 0;
        setReceiverInstVar(fullIndex, stackTop());
        break;
    }
    case 0xF4: { // Store Literal Variable #i (+ extA * 256) - no pop
        uint8_t indexByte = fetchByte();
        int fullIndex = (extA_ << 8) | indexByte;
        extA_ = 0;
        Oop assoc = literal(fullIndex);
        if (assoc.isObject())
            memory_.storePointer(1, assoc, stackTop());
        break;
    }
    case 0xF5: { // Store Temporary Variable #i - no pop
        uint8_t indexByte = fetchByte();
        setTemporary(indexByte, stackTop());
        break;
    }
    case 0xF6: // UNASSIGNED
    case 0xF7: // UNASSIGNED
        fetchByte();
        break;

    // ====== 3-byte bytecodes (0xF8-0xFF) ======

    case 0xF8: { // Call Primitive
        uint8_t primLowByte = fetchByte();
        uint8_t flagsAndHigh = fetchByte();
        int primIndex = primLowByte | ((flagsAndHigh & 0x1F) << 8);
        (void)primIndex; // Skipped at activation time
        break;
    }
    case 0xF9: { // Push FullBlockClosure
        uint8_t litIndex = fetchByte();
        uint8_t flags = fetchByte();
        int fullLitIndex = (extA_ << 8) | litIndex;
        extA_ = 0;
        int numCopied = flags & 0x3F;
        bool receiverOnStack = (flags >> 7) & 1;
        bool ignoreOuterContext = (flags >> 6) & 1;
        createFullBlockWithLiteral(fullLitIndex, numCopied, receiverOnStack, ignoreOuterContext);
        break;
    }
    case 0xFA: { // Push Closure
        uint8_t desc = fetchByte();
        uint8_t blockSizeLow = fetchByte();
        int numCopied = ((desc >> 3) & 0x07) | ((extA_ >> 4) << 3);
        int numArgs = (desc & 0x07) | ((extA_ & 0x0F) << 3);
        int blockSize = blockSizeLow + static_cast<int>(static_cast<unsigned int>(extB_) << 8);
        extA_ = 0;
        extB_ = 0;
        createBlockWithArgs(numArgs, numCopied, blockSize);
        break;
    }
    case 0xFB: { // Push Temp At k In Temp Vector At j
        uint8_t tempIndex = fetchByte();
        uint8_t vectorIndex = fetchByte();
        Oop tempVector = temporary(vectorIndex);
        push(tempVector.isObject() ? memory_.fetchPointer(tempIndex, tempVector) : memory_.nil());
        break;
    }
    case 0xFC: { // Store Temp At k In Temp Vector At j (no pop)
        uint8_t tempIndex = fetchByte();
        uint8_t vectorIndex = fetchByte();
        Oop tempVector = temporary(vectorIndex);
        if (tempVector.isObject())
            memory_.storePointer(tempIndex, tempVector, stackTop());
        break;
    }
    case 0xFD: { // Pop and Store Temp At k In Temp Vector At j
        uint8_t tempIndex = fetchByte();
        uint8_t vectorIndex = fetchByte();
        Oop value = pop();
        Oop tempVector = temporary(vectorIndex);
        if (tempVector.isObject())
            memory_.storePointer(tempIndex, tempVector, value);
        break;
    }
    case 0xFE: // UNASSIGNED
    case 0xFF: // UNASSIGNED
        fetchByte();
        fetchByte();
        break;

    } // switch (bytecode)
}


// push/pop/stackTop/stackValue/stackValuePut/popN/fetchByte/fetchTwoBytes
// are defined inline in Interpreter.hpp for performance.

// ===== BYTECODE IMPLEMENTATIONS =====

void Interpreter::pushReceiverVariable(int index) {
    Oop result = memory_.fetchPointerUnchecked(index, receiver_);
    // Trace ReadStream instVar reads
    {
        static int rsReadCount = 0;
        if (rsReadCount < 50 && index <= 2) {
            std::string rcls = memory_.classNameOf(receiver_);
            if (rcls.find("ReadStream") != std::string::npos) {
                rsReadCount++;
                if (result.isSmallInteger()) {
                    fprintf(stderr, "[RS-READ] %s slot[%d] = %lld (method=%s)\n",
                            rcls.c_str(), index, result.asSmallInteger(),
                            memory_.selectorOf(method_).c_str());
                } else {
                    fprintf(stderr, "[RS-READ] %s slot[%d] = 0x%llx non-int (method=%s class=%s)\n",
                            rcls.c_str(), index, (unsigned long long)result.rawBits(),
                            memory_.selectorOf(method_).c_str(),
                            memory_.classNameOf(result).c_str());
                }
            }
        }
    }
    push(result);
}

void Interpreter::pushTemporary(int index) {
    Oop temp = temporary(index);

    // Trace nil temp pushes to understand value: with nil args
    if constexpr (ENABLE_DEBUG_LOGGING) {
        static FILE* nilTempLog = nullptr;
        static int nilTempCount = 0;
        if (temp.rawBits() == memory_.nil().rawBits()) {
            if (!nilTempLog) nilTempLog = nullptr;
            if (nilTempLog && nilTempCount < 100) {
                nilTempCount++;
                std::string methodSel = memory_.selectorOf(method_);
                fprintf(nilTempLog, "[NIL-TEMP #%d] pushTemporary(%d) = nil in %s frameDepth=%zu\n",
                        nilTempCount, index, methodSel.c_str(), frameDepth_);
                // Dump first few temps
                fprintf(nilTempLog, "  temps: ");
                for (int t = 0; t < 5; t++) {
                    Oop tv = temporary(t);
                    fprintf(nilTempLog, "[%d]=0x%llx ", t, (unsigned long long)tv.rawBits());
                }
                fprintf(nilTempLog, "\n");
                fflush(nilTempLog);
            }
        }
    }
    push(temp);
}

void Interpreter::pushLiteralConstant(int index) {
    // Simple literal push, no extensions
    // The index is already the full literal index (0-31 from bytecode 0x20-0x3F,
    // or 0-63 from extended push bytecode 0x80)
    Oop val = literal(index);
    push(val);
}

void Interpreter::pushLiteralVariable(int index) {
    // Simple literal variable push, no extensions
    // Literal variable is an Association, fetch its value
    Oop assoc = literal(index);

    // Validate that we actually have an Association-like object (pointer object with at least 2 slots)
    // NOT a Symbol/String (byte object)
    if (!assoc.isObject() || assoc.isNil()) {
        push(memory_.nil());
        return;
    }

    ObjectHeader* header = assoc.asObjectPtr();

    // Check if it's a byte object (Symbol, String) - unexpected but handle gracefully
    if (header->isBytesObject()) {
        // Push the symbol itself to avoid crash (wrong but won't corrupt)
        push(assoc);
        return;
    }

    // Normal case: Association with key in slot 0, value in slot 1
    // assoc validated above (isObject, not bytes, not nil)
    Oop value = memory_.fetchPointerUnchecked(1, assoc);  // Association>>value
    push(value);
}

void Interpreter::storeReceiverVariable(int index) {
    Oop value = pop();
    setReceiverInstVar(index, value);
}

void Interpreter::storeTemporary(int index) {
    Oop value = pop();

    // Trace stores in snapshot:andQuit:
    if constexpr (ENABLE_DEBUG_LOGGING) {
        static FILE* snapStoreLog = nullptr;
        static int snapStoreCount = 0;
        if (!snapStoreLog) snapStoreLog = nullptr;
        if (snapStoreLog && snapStoreCount < 100) {
            std::string methodSel = memory_.selectorOf(method_);
            if (methodSel.find("snapshot") != std::string::npos ||
                methodSel.find("Session") != std::string::npos ||
                snapStoreCount < 30) {
                snapStoreCount++;
                fprintf(snapStoreLog, "[TEMP-STORE #%d] temp[%d] := 0x%llx in #%s\n",
                        snapStoreCount, index,
                        (unsigned long long)value.rawBits(), methodSel.c_str());
                fflush(snapStoreLog);
            }
        }
    }

    setTemporary(index, value);
}

void Interpreter::pushSpecial(int which) {
    switch (which) {
        case 0: push(receiver_); break;
        case 1: push(memory_.trueObject()); break;
        case 2: push(memory_.falseObject()); break;
        case 3: push(memory_.nil()); break;
        case 4: push(Oop::fromSmallInteger(-1)); break;
        case 5: push(Oop::fromSmallInteger(0)); break;
        case 6: push(Oop::fromSmallInteger(1)); break;
        case 7: push(Oop::fromSmallInteger(2)); break;
    }
}

void Interpreter::returnValue(Oop value) {
    // Debug: trace during atAllPut: debugging
    if (benchMode_ && traceAtAllPut_ >= 2) {
        std::string sel = memory_.selectorOf(method_);
        fprintf(stderr, "[RV] returnValue from #%s fd=%zu val=0x%llx\n",
                sel.c_str(), frameDepth_, (unsigned long long)value.rawBits());
    }
    // If no frames to pop, check if we have a sender context to return to
    if (frameDepth_ == 0) {
        // Check for pending NLR through ensure:.
        // When a context-based NLR (fd=0) encounters an ensure: context,
        // handleContextNLRUnwind resumes the ensure: to run its cleanup block.
        // When ensure: returns (^ returnValue), we intercept here to continue
        // the NLR to the home method's sender, handling multiple ensure: contexts.
        if (nlrTargetCtx_.isObject() && !nlrTargetCtx_.isNil()) {
            Oop homeCtx = nlrTargetCtx_;
            Oop nilObj = memory_.nil();

            // Verify NLR target is reachable from current sender chain.
            // Context>>jump can transfer control to a completely different stack
            // (e.g., from a terminated process back to the terminator process via
            // runUntilReturnFrom:). If the NLR target is not reachable, the NLR
            // must be abandoned — continuing it would walk the wrong stack.
            {
                Oop check = memory_.fetchPointer(0, activeContext_);
                bool reachable = false;
                for (int i = 0; i < 2000 && check.isObject() && !check.isNil(); i++) {
                    if (check.rawBits() == homeCtx.rawBits()) {
                        reachable = true;
                        break;
                    }
                    check = memory_.fetchPointer(0, check);
                }
                if (!reachable) {
                    // NLR target is unreachable — abandon NLR and do normal return
                    nlrTargetCtx_ = Oop::nil();
                    nlrEnsureCtx_ = Oop::nil();
                    nlrHomeMethod_ = Oop::nil();
                    nlrValue_ = Oop::nil();
                    // Fall through to normal fd=0 return below
                    goto normalReturn;
                }
            }

            // Get sender of current context BEFORE killing it
            Oop senderOfCurrent = memory_.fetchPointer(0, activeContext_);

            // Kill the current context (ensure: is done)
            memory_.storePointer(0, activeContext_, nilObj);  // sender = nil
            memory_.storePointer(1, activeContext_, nilObj);  // pc = nil (dead)

            // Look for MORE ensure: (prim 198) contexts between here and homeCtx
            Oop nextEnsureCtx = Oop::nil();
            {
                Oop ctx = senderOfCurrent;
                int depth = 0;
                while (ctx.isObject() && !ctx.isNil() && depth < 200) {
                    if (ctx.rawBits() == homeCtx.rawBits()) break;
                    Oop method = memory_.fetchPointer(3, ctx);
                    if (method.isObject() && !method.isNil() && primitiveIndexOf(method) == 198) {
                        nextEnsureCtx = ctx;
                        break;
                    }
                    ctx = memory_.fetchPointer(0, ctx);
                    depth++;
                }
            }

            if (nextEnsureCtx.isObject() && !nextEnsureCtx.isNil()) {
                // Found another ensure: — kill contexts between current and it
                Oop c = senderOfCurrent;
                int safety = 0;
                while (c.isObject() && c.rawBits() != nilObj.rawBits() &&
                       c.rawBits() != nextEnsureCtx.rawBits() && safety++ < 200) {
                    Oop next = memory_.fetchPointer(0, c);
                    memory_.storePointer(0, c, nilObj);
                    memory_.storePointer(1, c, nilObj);
                    c = next;
                }

                // Update pending NLR to next ensure:
                nlrEnsureCtx_ = nextEnsureCtx;
                // nlrTargetCtx_ stays as homeCtx

                // Push NLR value onto next ensure:'s stack (as valueNoContextSwitch return)
                Oop stackpOop = memory_.fetchPointer(2, nextEnsureCtx);
                if (stackpOop.isSmallInteger()) {
                    int stackp = stackpOop.asSmallInteger();
                    stackp++;
                    memory_.storePointer(2, nextEnsureCtx, Oop::fromSmallInteger(stackp));
                    memory_.storePointer(5 + stackp, nextEnsureCtx, value);
                }

                // Resume next ensure: context
                executeFromContext(nextEnsureCtx);
                return;
            } else {
                // No more ensure: — complete the NLR
                nlrTargetCtx_ = Oop::nil();
                nlrEnsureCtx_ = Oop::nil();

                // Kill remaining contexts from senderOfCurrent to homeCtx (not including homeCtx)
                Oop c = senderOfCurrent;
                int safety = 0;
                while (c.isObject() && c.rawBits() != nilObj.rawBits() &&
                       c.rawBits() != homeCtx.rawBits() && safety++ < 200) {
                    Oop next = memory_.fetchPointer(0, c);
                    memory_.storePointer(0, c, nilObj);
                    memory_.storePointer(1, c, nilObj);
                    c = next;
                }

                // Get homeCtx's sender (NLR returns to caller of home method)
                Oop homeSender = memory_.fetchPointer(0, homeCtx);

                // Kill homeCtx itself
                memory_.storePointer(0, homeCtx, nilObj);
                memory_.storePointer(1, homeCtx, nilObj);

                // Return to homeCtx's sender with the NLR value
                if (homeSender.isObject() && !homeSender.isNil() &&
                    memory_.isValidPointer(homeSender)) {
                    stackPointer_ = stackBase_;
                    Oop hsStackp = memory_.fetchPointer(2, homeSender);
                    int hsOrigSp = hsStackp.isSmallInteger()
                        ? static_cast<int>(hsStackp.asSmallInteger()) : 0;
                    executeFromContext(homeSender);
                    framePointer_[1 + hsOrigSp] = value;
                    Oop* pastVal = framePointer_ + 1 + hsOrigSp + 1;
                    if (pastVal > stackPointer_) stackPointer_ = pastVal;
                    return;
                } else {
                    // No valid sender — terminate process
                    terminateCurrentProcess();
                    return;
                }
            }
        }

        // Safety net: inline NLR through ensure: lost its homeFrameDepth
        // due to a process switch (materialize→context→executeFromContext).
        // nlrHomeMethod_ was set by the inline ensure: handler. Search the
        // context chain for the home context and continue the NLR.
        if (nlrHomeMethod_.isObject() && !nlrHomeMethod_.isNil()) {
            Oop homeMethodOop = nlrHomeMethod_;
            Oop savedValue = nlrValue_;
            nlrHomeMethod_ = Oop::nil();
            nlrValue_ = Oop::nil();

            // Kill current context (ensure: is done)
            Oop nilObj = memory_.nil();
            Oop senderOfCurrent = memory_.fetchPointer(0, activeContext_);
            memory_.storePointer(0, activeContext_, nilObj);
            memory_.storePointer(1, activeContext_, nilObj);

            // Search sender chain for home context (method match)
            Oop homeCtx = Oop::nil();
            Oop ctx = senderOfCurrent;
            int depth = 0;
            while (ctx.isObject() && !ctx.isNil() && depth < 200) {
                if (memory_.fetchPointer(3, ctx).rawBits() == homeMethodOop.rawBits()) {
                    homeCtx = ctx;
                    break;
                }
                ctx = memory_.fetchPointer(0, ctx);
                depth++;
            }

            if (homeCtx.isObject() && !homeCtx.isNil()) {
                // Check for MORE ensure: between here and home
                Oop nextEnsure = Oop::nil();
                ctx = senderOfCurrent;
                depth = 0;
                while (ctx.isObject() && !ctx.isNil() && depth < 200) {
                    if (ctx.rawBits() == homeCtx.rawBits()) break;
                    Oop method = memory_.fetchPointer(3, ctx);
                    if (method.isObject() && !method.isNil() && primitiveIndexOf(method) == 198) {
                        nextEnsure = ctx;
                        break;
                    }
                    ctx = memory_.fetchPointer(0, ctx);
                    depth++;
                }

                if (nextEnsure.isObject() && !nextEnsure.isNil()) {
                    // Set up pending NLR for the next ensure:
                    // Kill contexts between senderOfCurrent and nextEnsure
                    Oop c = senderOfCurrent;
                    int safety = 0;
                    while (c.isObject() && c.rawBits() != nilObj.rawBits() &&
                           c.rawBits() != nextEnsure.rawBits() && safety++ < 200) {
                        Oop next = memory_.fetchPointer(0, c);
                        memory_.storePointer(0, c, nilObj);
                        memory_.storePointer(1, c, nilObj);
                        c = next;
                    }
                    nlrTargetCtx_ = homeCtx;
                    nlrEnsureCtx_ = nextEnsure;
                    // Push value onto next ensure:'s stack
                    Oop stackpOop = memory_.fetchPointer(2, nextEnsure);
                    if (stackpOop.isSmallInteger()) {
                        int stackp = stackpOop.asSmallInteger();
                        stackp++;
                        memory_.storePointer(2, nextEnsure, Oop::fromSmallInteger(stackp));
                        memory_.storePointer(5 + stackp, nextEnsure, savedValue);
                    }
                    executeFromContext(nextEnsure);
                } else {
                    // No more ensure: — complete the NLR directly
                    // Kill contexts between senderOfCurrent and homeCtx.
                    // Set PC to HasBeenReturnedFrom sentinel (SmallInteger -1)
                    // so attempts to resume detect the returned-from state.
                    Oop hasBeenReturnedPC = Oop::fromSmallInteger(-1);
                    Oop c = senderOfCurrent;
                    int safety = 0;
                    while (c.isObject() && c.rawBits() != nilObj.rawBits() &&
                           c.rawBits() != homeCtx.rawBits() && safety++ < 200) {
                        Oop next = memory_.fetchPointer(0, c);
                        memory_.storePointer(0, c, nilObj);
                        memory_.storePointer(1, c, hasBeenReturnedPC);
                        c = next;
                    }
                    Oop homeSender = memory_.fetchPointer(0, homeCtx);
                    memory_.storePointer(0, homeCtx, nilObj);
                    memory_.storePointer(1, homeCtx, hasBeenReturnedPC);

                    if (homeSender.isObject() && !homeSender.isNil() &&
                        memory_.isValidPointer(homeSender)) {
                        stackPointer_ = stackBase_;
                        Oop hs2Stackp = memory_.fetchPointer(2, homeSender);
                        int hs2OrigSp = hs2Stackp.isSmallInteger()
                            ? static_cast<int>(hs2Stackp.asSmallInteger()) : 0;
                        executeFromContext(homeSender);
                        framePointer_[1 + hs2OrigSp] = savedValue;
                        Oop* pastVal2 = framePointer_ + 1 + hs2OrigSp + 1;
                        if (pastVal2 > stackPointer_) stackPointer_ = pastVal2;
                    } else {
                        // homeSender is nil/invalid — send cannotReturn: per spec
                        // Back up IP past return bytecode to prevent dead code execution
                        if (instructionPointer_ > method_.asObjectPtr()->bytes()) {
                            instructionPointer_--;
                        }
                        stackPointer_ = stackBase_;
                        push(activeContext_);
                        push(savedValue);
                        sendSelector(selectors_.cannotReturn, 1);
                    }
                }
                return;
            }
            // Home context not found — fall through to normal return
            // (nlrHomeMethod_ was already cleared above)
        }

        normalReturn:
        // Check if current context has a sender
        Oop nilObj = memory_.specialObject(SpecialObjectIndex::NilObject);
        if (activeContext_.isObject() && activeContext_.rawBits() != nilObj.rawBits()) {
            Oop sender = memory_.fetchPointer(0, activeContext_);

            // DEFENSIVE: Check for corrupted sender (raw 0 or very low address)
            if (sender.rawBits() == 0 || sender.rawBits() < 0x10000) {
                // Corrupted sender - treat as end of context chain
                // Fall through to terminate current process
            } else if (sender.rawBits() == nilObj.rawBits()) {
                // Sender is nil - this method is at the top of the context chain
                // Fall through to terminate current process
            } else if (sender.isObject() && sender.rawBits() != nilObj.rawBits()) {
                // Verify sender is not an unrelocated pointer from old image base
                {
                    const uint64_t OLD_IMAGE_BASE = 0x10000000000ULL;
                    uint64_t sndAddr = sender.rawBits() & ~7ULL;
                    if (sndAddr >= OLD_IMAGE_BASE && sndAddr < OLD_IMAGE_BASE * 2) {
                        stopVM("Unrelocated sender pointer — ImageLoader bug");
                        return;
                    }
                }
                if (!memory_.isValidPointer(sender)) {
                    // Invalid sender - terminate process
                    goto terminate_process;
                }
                ObjectHeader* senderHdr = sender.asObjectPtr();

                // Check if sender looks like a Context (has enough slots and right format)
                bool hasEnoughSlots = senderHdr->slotCount() >= 6;
                bool isContextFormat = senderHdr->format() == ObjectFormat::IndexableWithFixed;

                if (hasEnoughSlots && isContextFormat) {
                    // Reset stack for new context
                    stackPointer_ = stackBase_;

                    // Mark the returning context as dead per Cog VM semantics:
                    // nil the sender and PC so isDead returns true and sender chain is broken.
                    memory_.storePointer(0, activeContext_, memory_.nil());  // sender = nil
                    memory_.storePointer(1, activeContext_, memory_.nil());  // pc = nil → isDead

                    // Read sender's stackp BEFORE executeFromContext (which uses it).
                    // We need this to place the return value at the correct
                    // Pharo stack position, especially when stackp < numTemps
                    // (e.g., after Context>>jump's "self pop" decrements stackp).
                    Oop senderStackp = memory_.fetchPointer(2, sender);
                    int origSp = senderStackp.isSmallInteger()
                        ? static_cast<int>(senderStackp.asSmallInteger()) : 0;

                    if (executeFromContext(sender)) {
                        // Place return value at framePointer_[1 + origSp],
                        // which is the Pharo context's stackp+1 position.
                        // In the normal case (origSp >= numTemps), this is the
                        // same as push(). In the jump case (origSp < numTemps),
                        // this correctly overwrites the temp slot instead of
                        // going above the padded nil temps.
                        framePointer_[1 + origSp] = value;
                        // Ensure stackPointer_ is past the written position
                        Oop* pastValue = framePointer_ + 1 + origSp + 1;
                        if (pastValue > stackPointer_) {
                            stackPointer_ = pastValue;
                        }
                        return;
                    }
                }
            }
        }

terminate_process:
        if (benchMode_) {
            handleBenchComplete();
            return;
        }

                // Per reference VM spec: send cannotReturn: instead of silently terminating.
        // This gives Smalltalk's exception handling a chance to handle the situation.
        // Guard: limit cannotReturn: events per process. The error handler from
        // cannotReturn: may itself try to return through nil sender, creating new
        // contexts each time (so context-identity checks don't work). It may also
        // trigger process switches (which would reset a per-switch counter). Track
        // by process identity instead: count cannotReturn: events for the same process
        // across any number of context switches.
        if (activeContext_.isObject() && !activeContext_.isNil()) {
            Oop currentProcess = getActiveProcess();
            if (currentProcess.rawBits() != lastCannotReturnProcess_.rawBits()) {
                // Different process — reset counter
                cannotReturnCount_ = 0;
                lastCannotReturnProcess_ = currentProcess;
            }
            cannotReturnCount_++;
            if (cannotReturnCount_ <= 2) {
                lastCannotReturnCtx_ = activeContext_;
                // Set a step deadline for error handling. The cannotReturn: handler
                // in Smalltalk sends error: which triggers exception handling. In our
                // interpreted VM this can take 35M+ steps at P80, monopolizing CPU
                // and preventing lower-priority test/watchdog processes from running.
                // Give error handling 2M steps (~2 seconds), then forcibly terminate.
                if (cannotReturnCount_ == 1) {
                    cannotReturnDeadline_ = g_stepNum + 2000000;
                }
                // Back up IP to the return bytecode. When cannotReturn: returns
                // and popFrame restores this IP, the return will be retried instead
                // of falling through into dead code after the return bytecode.
                // The return bytecodes (0x58-0x5C) are all single-byte, so IP-1
                // points to the return instruction that triggered this path.
                if (instructionPointer_ > method_.asObjectPtr()->bytes()) {
                    instructionPointer_--;
                }
                stackPointer_ = stackBase_;
                push(activeContext_);  // receiver: the context that cannot return
                push(value);           // arg: the value that was being returned
                sendSelector(selectors_.cannotReturn, 1);
                return;
            }
            // Too many cannotReturn: events — error handler is not terminating.
            // Fall through to terminate the process.
            cannotReturnCount_ = 0;
            cannotReturnDeadline_ = 0;
            lastCannotReturnProcess_ = Oop::nil();
            lastCannotReturnCtx_ = Oop::nil();
        }

        // Terminate the process (cannotReturn: failed or activeContext_ is nil)
        terminateCurrentProcess();
        if (tryReschedule()) {
            return;
        }
        if (bootstrapStartup()) {
            return;
        }
        stopVM("No runnable processes after terminate with nil activeContext");
    }


    // Debug: track method_ changes through popFrame
    if constexpr (ENABLE_DEBUG_LOGGING) {
        static FILE* methodChangeLog = nullptr;
        static int methodChangeCount = 0;
        if (!methodChangeLog) methodChangeLog = nullptr;

        std::string beforeMethod = memory_.selectorOf(method_);

        std::string willBeRestoredTo = "<none>";
        if (frameDepth_ > 0)
            willBeRestoredTo = memory_.selectorOf(savedFrames_[frameDepth_ - 1].savedMethod);

        if (!popFrame()) return;  // Process terminated and rescheduled

        std::string afterMethod = memory_.selectorOf(method_);

        // Log transitions involving fullCheck - get IP offset AFTER popFrame (where we'll resume)
        if (methodChangeLog && methodChangeCount < 5000) {
            if (beforeMethod == "fullCheck" || afterMethod == "fullCheck" || willBeRestoredTo == "fullCheck") {
                methodChangeCount++;
                // Get bytecode offset in afterMethod (the restored method)
                int ipOffset = -1;
                if (instructionPointer_ && method_.isObject() && method_.rawBits() > 0x10000) {
                    ObjectHeader* mHdr = method_.asObjectPtr();
                    uint8_t* methodStart = mHdr->bytes();
                    ipOffset = static_cast<int>(instructionPointer_ - methodStart);
                }
                fprintf(methodChangeLog, "[RETURN #%d] %s -> %s (resumeIP=%d)\n",
                        methodChangeCount, beforeMethod.c_str(), afterMethod.c_str(), ipOffset);
                fflush(methodChangeLog);
            }
        }
    } else {
        // Debug: track returns from atAllPut:-related methods
        if (benchMode_ && traceAtAllPut_ > 0) {
            std::string retSel = memory_.selectorOf(method_);
            if (retSel == "atAllPut:" || retSel == "from:to:put:" || retSel == "size") {
                std::string resumeSel = (frameDepth_ > 0) ? memory_.selectorOf(savedFrames_[frameDepth_ - 1].savedMethod) : "(base)";
                fprintf(stderr, "[RET] #%s → resuming #%s fd=%zu SP=%ld FP=%ld\n",
                        retSel.c_str(), resumeSel.c_str(), frameDepth_,
                        (long)(stackPointer_ - stackBase_), (long)(framePointer_ - stackBase_));
            }
        }
        // Pop frame and push result
        if (!popFrame()) return;  // Process terminated and rescheduled
    }

    // After popping, if execution is still running, push the result
    if (running_) {
        push(value);

        // Continue NLR through ensure: — nlrHomeMethod_ was set by
        // returnFromBlock when it encountered an ensure: (prim 198)
        // frame during NLR unwinding. The ensure: cleanup has now
        // completed; continue the NLR to the home method.
        if (nlrHomeMethod_.isObject() && !nlrHomeMethod_.isNil()) {
            pop();  // Discard ensure: return value
            Oop nlrVal = nlrValue_;
            Oop nlrHome = nlrHomeMethod_;
            nlrHomeMethod_ = Oop::nil();
            nlrValue_ = Oop::nil();

            // Find home method in saved frames
            size_t homeFrame = SIZE_MAX;
            for (size_t i = 0; i < frameDepth_; i++) {
                if (savedFrames_[i].savedMethod.rawBits() == nlrHome.rawBits()) {
                    homeFrame = i;
                    break;
                }
            }

            if (homeFrame != SIZE_MAX && homeFrame < frameDepth_) {
                // Unwind to home frame, checking for more ensure: frames
                while (frameDepth_ > homeFrame) {
                    if (frameDepth_ > 1) {
                        Oop rm = savedFrames_[frameDepth_ - 1].savedMethod;
                        if (rm.isObject() && !rm.isNil() &&
                            primitiveIndexOf(rm) == 198) {
                            // Another ensure: — pause NLR again
                            nlrHomeMethod_ = nlrHome;
                            nlrValue_ = nlrVal;
                            if (!popFrame()) return;
                            push(nlrVal);
                            return;
                        }
                    }
                    if (!popFrame()) return;
                }
                // At homeFrame: return FROM the home method
                returnValue(nlrVal);
                return;
            }

            // Home frame not in savedFrames_ (e.g., context materialization
            // during ensure: execution). Re-set for the fd==0 context-based
            // handler to pick up.
            nlrHomeMethod_ = nlrHome;
            nlrValue_ = nlrVal;
        }

#if PHARO_JIT_ENABLED
        // Try to re-enter JIT execution in the caller method.
        // IP is at the bytecode after the send that just returned.
        if (jitRuntime_.isInitialized()) {
            tryJITResumeInCaller();
        }
#endif
    }
}

void Interpreter::returnFromMethod() {
    // Debug: trace returns during atAllPut: debugging
    if (benchMode_ && traceAtAllPut_ >= 2) {
        std::string sel = memory_.selectorOf(method_);
        fprintf(stderr, "[RFM] returnFromMethod from #%s fd=%zu SP=%ld FP=%ld\n",
                sel.c_str(), frameDepth_,
                (long)(stackPointer_ - stackBase_), (long)(framePointer_ - stackBase_));
    }
    Oop value = pop();

    // Check if we're executing inside a block (CompiledBlock)
    // If so, a "return from method" (^) should actually return from the HOME method,
    // not just from this block.

    if (frameDepth_ > 0) {
        size_t homeFrame = savedFrames_[frameDepth_ - 1].homeFrameDepth;

        // If homeFrame is SIZE_MAX but we're in a CompiledBlock, we need to do
        // context-based NLR. This happens after exception handling when contexts
        // were materialized - the home method is in the context chain, not savedFrames_.
        if (homeFrame == SIZE_MAX) {
            // Check SIZE_MAX path — only trace when we're in a CompiledBlock (actual NLR)
            // (Normal methods with SIZE_MAX are just regular returns)

            // Check if we're in a CompiledBlock by looking at the method's last literal.
            // For NESTED blocks, the last literal is another CompiledBlock, not the
            // home method. Follow the chain until we reach the actual CompiledMethod
            // (whose last literal is NOT a CompiledMethod — it's the class binding).
            Oop homeMethodOop = Oop::nil();
            if (method_.isObject() && method_.rawBits() > 0x10000) {
                Oop hdr = memory_.fetchPointer(0, method_);
                if (hdr.isSmallInteger()) {
                    int numLits = hdr.asSmallInteger() & 0x7FFF;
                    if (numLits >= 1) {
                        Oop enclosing = memory_.fetchPointer(numLits, method_);
                        // Follow the chain of enclosing blocks/methods
                        int chainDepth = 0;
                        while (enclosing.isObject() && enclosing.rawBits() > 0x10000 && chainDepth < 20) {
                            ObjectHeader* ecHdr = enclosing.asObjectPtr();
                            if (!ecHdr->isCompiledMethod()) break;
                            // Check if this is a CompiledBlock or CompiledMethod
                            // by examining its last literal
                            Oop ecHeader = memory_.fetchPointer(0, enclosing);
                            if (!ecHeader.isSmallInteger()) break;
                            int ecNumLits = ecHeader.asSmallInteger() & 0x7FFF;
                            if (ecNumLits < 1) {
                                // No literals — treat as home method
                                homeMethodOop = enclosing;
                                break;
                            }
                            Oop ecLastLit = memory_.fetchPointer(ecNumLits, enclosing);
                            bool isBlock = false;
                            if (ecLastLit.isObject() && ecLastLit.rawBits() > 0x10000) {
                                ObjectHeader* llHdr = ecLastLit.asObjectPtr();
                                isBlock = llHdr->isCompiledMethod();
                            }
                            if (!isBlock) {
                                // Last literal is NOT compiled code — this is the home method
                                homeMethodOop = enclosing;
                                break;
                            }
                            // It's a CompiledBlock — follow the chain
                            enclosing = ecLastLit;
                            chainDepth++;
                        }
                        // Fallback: if chain exhausted, use whatever we have
                        if (homeMethodOop.isNil() && enclosing.isObject() && enclosing.rawBits() > 0x10000) {
                            ObjectHeader* ecHdr = enclosing.asObjectPtr();
                            if (ecHdr->isCompiledMethod()) {
                                homeMethodOop = enclosing;
                            }
                        }
                    }
                }
            }

            // If we found a home method, search context chain and do context-based NLR
            if (homeMethodOop.isObject() && !homeMethodOop.isNil()) {
                // First check if home method is in context chain
                Oop ctx = activeContext_;
                int searchDepth = 0;
                Oop homeCtx = Oop::nil();

                // ALSO check savedFrames_ (activateBlock sets SIZE_MAX but the home
                // might still be in inline frames if block was re-pushed after materialization)
                for (size_t si = 0; si < frameDepth_; si++) {
                    if (savedFrames_[si].savedMethod.rawBits() == homeMethodOop.rawBits()) {
                        // Home method IS in savedFrames_ — use inline NLR
                        size_t homeFrame = si;
                        while (frameDepth_ > homeFrame) {
                            // Check ensure: in unwind path
                            if (frameDepth_ > 1) {
                                Oop rm = savedFrames_[frameDepth_ - 1].savedMethod;
                                if (rm.isObject() && rm.rawBits() > 0x10000 &&
                                    primitiveIndexOf(rm) == 198) {
                                    nlrHomeMethod_ = savedFrames_[homeFrame].savedMethod;
                                    nlrValue_ = value;
                                    if (!popFrame()) return;
                                    push(value);
                                    if (frameDepth_ > 0) savedFrames_[frameDepth_ - 1].homeFrameDepth = homeFrame;
                                    return;
                                }
                            }
                            if (!popFrame()) return;
                        }
                        returnValue(value);
                        return;
                    }
                }

                while (ctx.isObject() && !ctx.isNil() && searchDepth < 200) {
                    Oop ctxMethod = memory_.fetchPointer(3, ctx);
                    if (ctxMethod.rawBits() == homeMethodOop.rawBits()) {
                        homeCtx = ctx;
                        break;
                    }
                    ctx = memory_.fetchPointer(0, ctx);
                    searchDepth++;
                }

                if (homeCtx.isObject() && !homeCtx.isNil()) {
                    // Found home context! Do context-based NLR
                    // Materialize all inline frames first
                    if (frameDepth_ > 0) {
                        Oop materializedCtx = materializeFrameStack();
                        activeContext_ = materializedCtx;
                        frameDepth_ = 0;
                    }

                    // Check for unwind (ensure:) contexts between here and home.
                    // If found, redirect through the ensure: context so its cleanup fires.
                    if (handleContextNLRUnwind(value, activeContext_, homeCtx)) {
                        return;
                    }

                    // No unwind contexts - return FROM the home context by executing from its sender
                    Oop sender = memory_.fetchPointer(0, homeCtx);
                    if (sender.isObject() && !sender.isNil()) {
                        // Mark all contexts from activeContext_ through homeCtx as dead
                        // (nil their sender and PC per Cog VM semantics)
                        {
                            Oop ctx = activeContext_;
                            Oop nilObj = memory_.nil();
                            int safety = 0;
                            while (ctx.isObject() && ctx.rawBits() != nilObj.rawBits() && safety++ < 200) {
                                Oop nextSender = memory_.fetchPointer(0, ctx);
                                memory_.storePointer(0, ctx, nilObj);  // sender = nil
                                memory_.storePointer(1, ctx, nilObj);  // pc = nil → isDead
                                if (ctx.rawBits() == homeCtx.rawBits()) break;
                                ctx = nextSender;
                            }
                        }
                        // Store the return value on sender's stack
                        Oop stackpOop = memory_.fetchPointer(2, sender);
                        if (stackpOop.isSmallInteger()) {
                            int stackp = stackpOop.asSmallInteger();
                            stackp++;
                            memory_.storePointer(2, sender, Oop::fromSmallInteger(stackp));
                            memory_.storePointer(5 + stackp, sender, value);
                        }
                        // Execute from the sender context
                        executeFromContext(sender);
                        return;
                    }
                }
            }
        }

        if (homeFrame != SIZE_MAX) {
            // Non-local return: unwind frames from current down to homeFrame
            // We want to return FROM the home method, so we pop down to homeFrame,
            // then returnValue pops one more and pushes the value to the caller
            while (frameDepth_ > homeFrame) {
                // Check if the frame we're about to restore has primitive 198 (ensure:/ifCurtailed:).
                // If so, we must fire its termination block before continuing the NLR.
                if (frameDepth_ > 1) {
                    Oop restoringMethod = savedFrames_[frameDepth_ - 1].savedMethod;
                    if (restoringMethod.isObject() && restoringMethod.rawBits() > 0x10000) {
                        Oop mHeader = memory_.fetchPointer(0, restoringMethod);
                        if (mHeader.isSmallInteger()) {
                            int64_t bits = mHeader.asSmallInteger();
                            bool hasPrim = (bits >> 16) & 1;
                            if (hasPrim) {
                                int numLits = bits & 0x7FFF;
                                ObjectHeader* mObj = restoringMethod.asObjectPtr();
                                uint8_t* bc = mObj->bytes() + (1 + numLits) * 8;
                                int primIndex = 0;
                                if (bc[0] == 0xF8) {
                                    primIndex = bc[1] | (bc[2] << 8);
                                }
                                if (primIndex == 198) {
                                    // Found an ensure: frame! Stop the NLR here.
                                    // Also set nlrHomeMethod_ as a safety net: if a
                                    // process switch happens during cleanup and the
                                    // savedFrame homeFrameDepth is lost, returnValue()
                                    // at fd=0 can use nlrHomeMethod_ to find the home
                                    // context and continue the NLR.
                                    nlrHomeMethod_ = savedFrames_[homeFrame].savedMethod;
                                    nlrValue_ = value;
                                    if (!popFrame()) return;
                                    push(value);
                                    if (frameDepth_ > 0) {
                                        savedFrames_[frameDepth_ - 1].homeFrameDepth = homeFrame;
                                    }
                                    return;
                                }
                            }
                        }
                    }
                }
                if (!popFrame()) return;
            }
            // Now we're at homeFrame, returnValue pops this frame and returns to caller
            nlrHomeMethod_ = Oop::nil();  // Clear safety net — inline NLR succeeded
            nlrValue_ = Oop::nil();
            returnValue(value);
            return;
        }
    }

    // Determine if we're executing in a CompiledBlock (vs CompiledMethod).
    // For blocks, a failed NLR (home method not found) must send cannotReturn:.
    // For methods, returnFromMethod() is just a regular return.
    bool isCompiledBlock = false;
    Oop homeMethodForCR = Oop::nil();
    if (method_.isObject() && method_.rawBits() > 0x10000) {
        ObjectHeader* mObj = method_.asObjectPtr();
        if (mObj->isCompiledMethod()) {
            Oop hdr = memory_.fetchPointer(0, method_);
            if (hdr.isSmallInteger()) {
                int numLits = hdr.asSmallInteger() & 0x7FFF;
                if (numLits >= 1) {
                    Oop enclosing = memory_.fetchPointer(numLits, method_);
                    // Follow chain of enclosing blocks to find home CompiledMethod
                    int chainDepth = 0;
                    while (enclosing.isObject() && enclosing.rawBits() > 0x10000 && chainDepth < 20) {
                        ObjectHeader* ecHdr = enclosing.asObjectPtr();
                        if (!ecHdr->isCompiledMethod()) break;
                        isCompiledBlock = true;
                        Oop ecHeader = memory_.fetchPointer(0, enclosing);
                        if (!ecHeader.isSmallInteger()) { homeMethodForCR = enclosing; break; }
                        int ecNumLits = ecHeader.asSmallInteger() & 0x7FFF;
                        if (ecNumLits < 1) { homeMethodForCR = enclosing; break; }
                        Oop ecLastLit = memory_.fetchPointer(ecNumLits, enclosing);
                        bool lastLitIsCode = false;
                        if (ecLastLit.isObject() && ecLastLit.rawBits() > 0x10000) {
                            lastLitIsCode = ecLastLit.asObjectPtr()->isCompiledMethod();
                        }
                        if (!lastLitIsCode) { homeMethodForCR = enclosing; break; }
                        enclosing = ecLastLit;
                        chainDepth++;
                    }
                    if (homeMethodForCR.isNil() && enclosing.isObject() && enclosing.rawBits() > 0x10000) {
                        if (enclosing.asObjectPtr()->isCompiledMethod()) {
                            isCompiledBlock = true;
                            homeMethodForCR = enclosing;
                        }
                    }
                }
            }
        }
    }

    // CONTEXT-BASED NLR: When frameDepth_ == 0 and we're in a CompiledBlock,
    // we need to use the context chain to find the home method and unwind to it.
    // This happens when exception handling (on:do:) caused context materialization.
    if (isCompiledBlock && frameDepth_ == 0 && homeMethodForCR.isObject() && !homeMethodForCR.isNil()) {
        // Walk up the context chain to find the context executing homeMethod
        Oop ctx = activeContext_;
        Oop homeCtx = Oop::nil();
        int depth = 0;
        while (ctx.isObject() && !ctx.isNil() && depth < 200) {
            Oop ctxMethod = memory_.fetchPointer(3, ctx);
            if (ctxMethod.rawBits() == homeMethodForCR.rawBits()) {
                homeCtx = ctx;
                break;
            }
            ctx = memory_.fetchPointer(0, ctx);
            depth++;
        }

        if (homeCtx.isObject() && !homeCtx.isNil()) {
            // Check for unwind (ensure:) contexts between here and home
            if (handleContextNLRUnwind(value, activeContext_, homeCtx)) {
                return;
            }

            // No unwind contexts - return FROM the home context
            Oop sender = memory_.fetchPointer(0, homeCtx);
            if (sender.isObject() && !sender.isNil()) {
                Oop stackpOop = memory_.fetchPointer(2, sender);
                if (stackpOop.isSmallInteger()) {
                    int stackp = stackpOop.asSmallInteger();
                    stackp++;
                    memory_.storePointer(2, sender, Oop::fromSmallInteger(stackp));
                    memory_.storePointer(5 + stackp, sender, value);
                }
                executeFromContext(sender);
                return;
            }
        }
    }

    // If we're in a CompiledBlock and couldn't find the home method in the call chain,
    // the home method has already returned. Send cannotReturn: per Pharo spec.
    if (isCompiledBlock) {
        // Materialize frames if needed so cannotReturn: has proper context
        if (frameDepth_ > 0) {
            Oop topCtx = materializeFrameStack();
            activeContext_ = topCtx;
            frameDepth_ = 0;
        }
        // Back up IP past return bytecode to prevent dead code execution
        if (method_.isObject() && instructionPointer_ > method_.asObjectPtr()->bytes()) {
            instructionPointer_--;
        }
        push(activeContext_);  // receiver: the context that cannot return
        push(value);           // arg: the value that was being returned
        sendSelector(selectors_.cannotReturn, 1);
        return;
    }

    returnValue(value);
}

void Interpreter::returnFromBlock() {
    // Non-local return from block (bytecode 0x5E with extA > 0)
    Oop value = pop();

    // Get the home frame depth from the current frame
    size_t homeFrame = SIZE_MAX;
    if (frameDepth_ > 0) {
        homeFrame = savedFrames_[frameDepth_ - 1].homeFrameDepth;
    }

    // If homeFrame is valid and we have inline frames, unwind via inline frame stack
    if (homeFrame != SIZE_MAX && homeFrame < frameDepth_) {
        // Unwind frames from current down to homeFrame, checking for ensure: at each level.
        // After the loop, fd == homeFrame and current == home method.
        // returnValue then pops the home method's frame and pushes value on the caller's stack.
        while (frameDepth_ > homeFrame) {
            // Check if the frame we're about to restore has primitive 198 (ensure:/ifCurtailed:).
            // If so, we must fire its termination block before continuing the NLR.
            if (frameDepth_ > 1) {
                Oop restoringMethod = savedFrames_[frameDepth_ - 1].savedMethod;
                if (restoringMethod.isObject() && restoringMethod.rawBits() > 0x10000) {
                    Oop mHeader = memory_.fetchPointer(0, restoringMethod);
                    if (mHeader.isSmallInteger()) {
                        int64_t bits = mHeader.asSmallInteger();
                        bool hasPrim = (bits >> 16) & 1;
                        if (hasPrim) {
                            int numLits = bits & 0x7FFF;
                            ObjectHeader* mObj = restoringMethod.asObjectPtr();
                            uint8_t* bc = mObj->bytes() + (1 + numLits) * 8;
                            int primIndex = 0;
                            if (bc[0] == 0xF8) {
                                primIndex = bc[1] | (bc[2] << 8);
                            }
                            if (primIndex == 198) {
                                nlrHomeMethod_ = savedFrames_[homeFrame].savedMethod;
                                nlrValue_ = value;
                                if (!popFrame()) return;
                                push(value);
                                if (frameDepth_ > 0) {
                                    savedFrames_[frameDepth_ - 1].homeFrameDepth = homeFrame;
                                }
                                return;
                            }
                        }
                    }
                }
            }
            if (!popFrame()) return;
        }
        // Now do a regular return which pops one more frame and pushes the value
        returnValue(value);
        return;
    }

    // When frameDepth_ > 0 but homeFrame == SIZE_MAX, the home method is in the
    // context chain (not in savedFrames_). This happens when exception handling caused
    // context materialization: the home method's frame was turned into a context object
    // and is no longer in the inline frame stack. Materialize all remaining inline
    // frames to contexts, then use the context-based NLR path below.
    if (frameDepth_ > 0 && homeFrame == SIZE_MAX) {
        Oop topCtx = materializeFrameStack();
        activeContext_ = topCtx;
        frameDepth_ = 0;
        // closure_ is still valid — fall through to context-based NLR
    }

    // CONTEXT-BASED NLR: When frameDepth_ == 0 (after thisContext materialization),
    // we need to use the context chain to find the home method and unwind to it.
    // This happens when exception handling (on:do:) caused context materialization.
    if (frameDepth_ == 0 && closure_.isObject() && !closure_.isNil()) {
        // Get the home method from the closure's CompiledBlock
        // FullBlockClosure: slot 1 = CompiledBlock
        // CompiledBlock: last literal = home CompiledMethod
        Oop compiledBlock = memory_.fetchPointer(1, closure_);
        Oop homeMethod = Oop::nil();

        if (compiledBlock.isObject() && !compiledBlock.isNil() && compiledBlock.rawBits() > 0x10000) {
            ObjectHeader* cbHdr = compiledBlock.asObjectPtr();
            if (cbHdr->isCompiledMethod()) {
                Oop header = memory_.fetchPointer(0, compiledBlock);
                if (header.isSmallInteger()) {
                    int numLits = header.asSmallInteger() & 0x7FFF;
                    if (numLits >= 1) {
                        // Last literal of CompiledBlock is the home method
                        homeMethod = memory_.fetchPointer(numLits, compiledBlock);
                    }
                }
            }
        }

        // Walk up the context chain to find the context executing homeMethod
        if (homeMethod.isObject() && !homeMethod.isNil()) {
            Oop ctx = activeContext_;
            Oop homeCtx = Oop::nil();
            int depth = 0;
            while (ctx.isObject() && !ctx.isNil() && depth < 200) {
                Oop ctxMethod = memory_.fetchPointer(3, ctx);
                if (ctxMethod.rawBits() == homeMethod.rawBits()) {
                    homeCtx = ctx;
                    break;
                }
                ctx = memory_.fetchPointer(0, ctx);
                depth++;
            }

            if (homeCtx.isObject() && !homeCtx.isNil()) {
                // Check for unwind (ensure:) contexts between here and home
                if (handleContextNLRUnwind(value, activeContext_, homeCtx)) {
                    return;
                }

                // No unwind contexts - return FROM the home context
                Oop sender = memory_.fetchPointer(0, homeCtx);
                if (sender.isObject() && !sender.isNil()) {
                    Oop stackpOop = memory_.fetchPointer(2, sender);
                    if (stackpOop.isSmallInteger()) {
                        int stackp = stackpOop.asSmallInteger();
                        stackp++;
                        memory_.storePointer(2, sender, Oop::fromSmallInteger(stackp));
                        memory_.storePointer(5 + stackp, sender, value);
                    }
                    executeFromContext(sender);
                    return;
                }
            }
        }
    }

    // Home method not found in context chain - send cannotReturn: to the active context
    // This happens when a block tries to return from a method that has already returned.
    // Per Pharo spec, Context >> cannotReturn: signals BlockCannotReturn.
    if (frameDepth_ > 0) {
        Oop topCtx = materializeFrameStack();
        activeContext_ = topCtx;
        frameDepth_ = 0;
    }
    // Back up IP past return bytecode to prevent dead code execution
    if (method_.isObject() && instructionPointer_ > method_.asObjectPtr()->bytes()) {
        instructionPointer_--;
    }
    push(activeContext_);  // receiver: the context that cannot return
    push(value);           // arg: the value that was being returned
    sendSelector(selectors_.cannotReturn, 1);
}

// Handle unwind (ensure:) contexts during context-based non-local returns.
// Walks the sender chain from startCtx to homeCtx looking for contexts whose
// method has primitive 198 (the ensure:/ifCurtailed: marker).
//
// When found, uses a "pending NLR" mechanism:
// 1. Kill all contexts from startCtx to just before ensureCtx
// 2. Push the NLR value onto ensureCtx's stack (as if valueNoContextSwitch returned)
// 3. Store homeCtx in nlrTargetCtx_ and ensureCtx in nlrEnsureCtx_
// 4. executeFromContext(ensureCtx) — ensure: runs its cleanup normally
// 5. When ensure: returns (detected in returnValue() via nlrTargetCtx_),
//    the NLR continues to homeCtx's sender
//
// This correctly handles multiple ensure: contexts in the chain: each ensure:
// runs its cleanup, and the NLR continues through all of them.
//
// Previous approach using aboutToReturn:through: was broken: it fired the
// ensure: cleanup but didn't continue the NLR. The NLR value was returned
// normally through the ensure: → critical: chain, and intern:'s `pop; returnSelf`
// discarded it, returning Symbol class instead of the interned symbol.
bool Interpreter::handleContextNLRUnwind(Oop value, Oop startCtx, Oop homeCtx) {
    Oop ctx = startCtx;
    int depth = 0;
    Oop ensureCtx = Oop::nil();

    // Find the first ensure: (prim 198) context between start and home
    while (ctx.isObject() && !ctx.isNil() && depth < 200) {
        if (ctx.rawBits() == homeCtx.rawBits()) break;

        Oop method = memory_.fetchPointer(3, ctx);
        if (method.isObject() && !method.isNil()) {
            if (primitiveIndexOf(method) == 198) {
                ensureCtx = ctx;
                break;
            }
        }
        ctx = memory_.fetchPointer(0, ctx);
        depth++;
    }

    // Also check if homeCtx itself is an ensure: context
    if (ensureCtx.isNil() && ctx.isObject() && !ctx.isNil() &&
        ctx.rawBits() == homeCtx.rawBits()) {
        Oop method = memory_.fetchPointer(3, homeCtx);
        if (method.isObject() && !method.isNil() && primitiveIndexOf(method) == 198) {
            ensureCtx = homeCtx;
        }
    }

    if (ensureCtx.isNil()) return false;

    Oop nilObj = memory_.nil();

    // Step 1: Kill all contexts from startCtx up to (but not including) ensureCtx.
    // Set PC to HasBeenReturnedFrom sentinel (SmallInteger -1) so resume detects it.
    {
        Oop hasBeenReturnedPC = Oop::fromSmallInteger(-1);
        Oop c = startCtx;
        int safety = 0;
        while (c.isObject() && c.rawBits() != nilObj.rawBits() &&
               c.rawBits() != ensureCtx.rawBits() && safety++ < 200) {
            Oop nextSender = memory_.fetchPointer(0, c);
            memory_.storePointer(0, c, nilObj);  // sender = nil
            memory_.storePointer(1, c, hasBeenReturnedPC);  // pc = sentinel
            c = nextSender;
        }
    }

    // Step 2: Store the NLR target so returnValue() can continue the NLR
    nlrTargetCtx_ = homeCtx;
    nlrEnsureCtx_ = ensureCtx;

    // Step 3: Push the NLR value onto ensureCtx's stack
    // This simulates valueNoContextSwitch returning the NLR value.
    // ensure: method: `returnValue := self valueNoContextSwitch`
    // The ensureCtx is waiting for the return of valueNoContextSwitch.
    // Push the NLR value as that return value.
    {
        Oop stackpOop = memory_.fetchPointer(2, ensureCtx);
        if (stackpOop.isSmallInteger()) {
            int stackp = stackpOop.asSmallInteger();
            stackp++;
            memory_.storePointer(2, ensureCtx, Oop::fromSmallInteger(stackp));
            memory_.storePointer(5 + stackp, ensureCtx, value);
        }
    }

    // Step 4: Execute from ensureCtx
    // ensure: resumes after valueNoContextSwitch:
    //   returnValue := <NLR value>  (assignment from the stack)
    //   complete := true
    //   aBlock value               (ensure block fires!)
    //   ^ returnValue              (returns NLR value — intercepted by returnValue())
    executeFromContext(ensureCtx);

    return true;
}

void Interpreter::extendedPush() {
    uint8_t descriptor = fetchByte();
    int type = (descriptor >> 6) & 3;
    int index = descriptor & 0x3F;

    switch (type) {
        case 0: pushReceiverVariable(index); break;
        case 1: pushTemporary(index); break;
        case 2: pushLiteralConstant(index); break;
        case 3: pushLiteralVariable(index); break;
    }
}

void Interpreter::extendedStore() {
    uint8_t descriptor = fetchByte();
    int type = (descriptor >> 6) & 3;
    int index = descriptor & 0x3F;

    Oop value = stackTop();  // Don't pop for store

    switch (type) {
        case 0: setReceiverInstVar(index, value); break;
        case 1: setTemporary(index, value); break;
        case 2: /* Can't store to literal constant */ break;
        case 3: {
            // Store to literal variable (association value)
            Oop assoc = literal(index);
            memory_.storePointer(1, assoc, value);
            break;
        }
    }
}

void Interpreter::extendedSend() {
    uint8_t descriptor = fetchByte();
    int literalIndex = descriptor & 0x1F;
    int argCount = (descriptor >> 5) & 0x7;
    sendSelector(literal(literalIndex), argCount);
}

void Interpreter::extendedSuperSend() {
    uint8_t descriptor = fetchByte();
    int literalIndex = descriptor & 0x1F;
    int argCount = (descriptor >> 5) & 0x7;

    // Super send: lookup from superclass of METHOD's defining class (not receiver's class)
    Oop selector = literal(literalIndex);
    Oop methodClass = methodClassOf(method_);
    Oop superclass;

    if (methodClass.isNil() || !methodClass.isObject()) {
        // Fallback to receiver's class superclass
        superclass = superclassOf(memory_.classOf(receiver_));
    } else {
        superclass = superclassOf(methodClass);
    }

    Oop method = lookupMethod(selector, superclass);
    if (method.isNil()) {
        sendDoesNotUnderstand(selector, argCount);
    } else {
        activateMethod(method, argCount);
    }
}

// ===== JUMPS =====

void Interpreter::shortJump(int offset) {
    instructionPointer_ += offset;
}

void Interpreter::shortJumpIfTrue(int offset) {
    Oop value = pop();
    if (isTrue(value)) {
        instructionPointer_ += offset;
    } else if (!isFalse(value)) {
        push(value);
        sendMustBeBoolean(value);
    }
}

void Interpreter::shortJumpIfFalse(int offset) {
    Oop value = pop();
    if (isFalse(value)) {
        instructionPointer_ += offset;
    } else if (!isTrue(value)) {
        push(value);
        sendMustBeBoolean(value);
    }
}

void Interpreter::longJump() {
    int16_t offset = static_cast<int16_t>(fetchTwoBytes());
    instructionPointer_ += offset;
}

void Interpreter::longJumpIfTrue() {
    int16_t offset = static_cast<int16_t>(fetchTwoBytes());
    Oop value = pop();
    if (isTrue(value)) {
        instructionPointer_ += offset;
    } else if (!isFalse(value)) {
        push(value);
        sendMustBeBoolean(value);
    }
}

void Interpreter::longJumpIfFalse() {
    int16_t offset = static_cast<int16_t>(fetchTwoBytes());
    Oop value = pop();
    if (isFalse(value)) {
        instructionPointer_ += offset;
    } else if (!isTrue(value)) {
        push(value);
        sendMustBeBoolean(value);
    }
}

// ===== SENDS =====

void Interpreter::arithmeticSend(int which) {
    // Arithmetic selectors: + - < > <= >= = ~= * / \\ @ bitShift: // bitAnd: bitOr:
    static const int argCounts[] = {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};
    int argCount = argCounts[which];

    // TRACE: Log arithmetic ops that involve non-SmallIntegers
    if constexpr (ENABLE_DEBUG_LOGGING) {
        static FILE* arithLog2 = nullptr;
        static int arithCount2 = 0;
        if (!arithLog2) arithLog2 = nullptr;
        if (arithLog2 && arithCount2 < 5000) {
            Oop rcvr = stackValue(1);
            Oop arg = stackValue(0);
            // Only log if receiver or arg is not a SmallInteger
            if (!rcvr.isSmallInteger() || !arg.isSmallInteger()) {
                arithCount2++;
                static const char* opNames[] = {"+", "-", "<", ">", "<=", ">=", "=", "~=",
                                                  "*", "/", "\\\\", "@", "bitShift:", "//", "bitAnd:", "bitOr:"};
                const char* op = (which >= 0 && which < 16) ? opNames[which] : "?";

                std::string rcvrClass = memory_.classNameOf(rcvr);
                std::string argClass = memory_.classNameOf(arg);

                fprintf(arithLog2, "[ARITH #%d] %s %s %s (fd=%zu)",
                        arithCount2, rcvrClass.c_str(), op, argClass.c_str(), frameDepth_);
                // For string = comparisons, dump actual values
                if (which == 6 && (rcvrClass.find("String") != std::string::npos || rcvrClass.find("Symbol") != std::string::npos)) {
                    auto getStr = [&](Oop o) -> std::string {
                        if (o.isImmediate()) return "<imm>";
                        ObjectHeader* h = o.asObjectPtr();
                        if (!h->isBytesObject()) return "<not-bytes>";
                        size_t sz = h->byteSize();
                        if (sz > 30) return "<long>";
                        return std::string((char*)h->bytes(), sz);
                    };
                    fprintf(arithLog2, " rcvr='%s'(0x%llx) arg='%s'(0x%llx)",
                            getStr(rcvr).c_str(), (unsigned long long)rcvr.rawBits(),
                            getStr(arg).c_str(), (unsigned long long)arg.rawBits());
                }
                // For FullBlockClosure, also log the raw values
                if (rcvrClass == "FullBlockClosure" || argClass == "FullBlockClosure") {
                    fprintf(arithLog2, " rcvr=0x%llx arg=0x%llx",
                            (unsigned long long)rcvr.rawBits(),
                            (unsigned long long)arg.rawBits());
                    if (rcvr.isSmallInteger()) {
                        fprintf(arithLog2, " (rcvr_val=%lld)", rcvr.asSmallInteger());
                    }
                    if (arg.isSmallInteger()) {
                        fprintf(arithLog2, " (arg_val=%lld)", arg.asSmallInteger());
                    }
                }
                fprintf(arithLog2, "\n");
                fflush(arithLog2);
            }
        }

        // TRACE: Log all <= sends to understand loop iteration
        if (which == 4) {  // <=
        }
    }

    // SmallInteger fast paths — matches reference Cog VM behavior.
    // These bypass the method dictionary entirely when both operands are SmallIntegers.
    // Required for correctness: tests like OCSpecialSelectorTest expect that the
    // compiler-optimized +/* etc. never go through the method dictionary.
    {
        Oop rcvr = stackValue(argCount);
        Oop arg = stackValue(0);

        if (rcvr.isSmallInteger() && arg.isSmallInteger()) {
            int64_t a = rcvr.asSmallInteger();
            int64_t b = arg.asSmallInteger();

            switch (which) {
                case 0: {  // +
                    int64_t result = a + b;
                    if (result >= Oop::smallIntegerMin() && result <= Oop::smallIntegerMax()) {
                        popN(2);
                        push(Oop::fromSmallInteger(result));
                        return;
                    }
                    break;
                }
                case 1: {  // -
                    int64_t result = a - b;
                    if (result >= Oop::smallIntegerMin() && result <= Oop::smallIntegerMax()) {
                        popN(2);
                        push(Oop::fromSmallInteger(result));
                        return;
                    }
                    break;
                }
                case 2:  // <
                    popN(2);
                    push(a < b ? memory_.trueObject() : memory_.falseObject());
                    return;
                case 3:  // >
                    popN(2);
                    push(a > b ? memory_.trueObject() : memory_.falseObject());
                    return;
                case 4:  // <=
                    popN(2);
                    push(a <= b ? memory_.trueObject() : memory_.falseObject());
                    return;
                case 5:  // >=
                    popN(2);
                    push(a >= b ? memory_.trueObject() : memory_.falseObject());
                    return;
                case 6:  // =
                    popN(2);
                    push(a == b ? memory_.trueObject() : memory_.falseObject());
                    return;
                case 7:  // ~=
                    popN(2);
                    push(a != b ? memory_.trueObject() : memory_.falseObject());
                    return;
                case 8: {  // *
                    // Check for overflow using __int128 or by checking bounds
                    __int128 result128 = static_cast<__int128>(a) * static_cast<__int128>(b);
                    int64_t result = static_cast<int64_t>(result128);
                    if (result128 == static_cast<__int128>(result) &&
                        result >= Oop::smallIntegerMin() && result <= Oop::smallIntegerMax()) {
                        popN(2);
                        push(Oop::fromSmallInteger(result));
                        return;
                    }
                    break;
                }
                case 9: {  // /
                    if (b != 0 && (a % b) == 0) {
                        int64_t result = a / b;
                        if (result >= Oop::smallIntegerMin() && result <= Oop::smallIntegerMax()) {
                            popN(2);
                            push(Oop::fromSmallInteger(result));
                            return;
                        }
                    }
                    break;
                }
                case 10: {  // \\  (modulo)
                    if (b != 0) {
                        int64_t result = a % b;
                        // Smalltalk modulo: result has sign of divisor
                        if (result != 0 && ((result ^ b) < 0)) {
                            result += b;
                        }
                        popN(2);
                        push(Oop::fromSmallInteger(result));
                        return;
                    }
                    break;
                }
                case 12: {  // bitShift:
                    if (b >= 0 && b < 63) {
                        // Left shift - check for overflow
                        int64_t result = a << b;
                        if ((result >> b) == a && result >= Oop::smallIntegerMin() && result <= Oop::smallIntegerMax()) {
                            popN(2);
                            push(Oop::fromSmallInteger(result));
                            return;
                        }
                    } else if (b < 0 && b > -64) {
                        // Right shift
                        int64_t result = a >> (-b);
                        popN(2);
                        push(Oop::fromSmallInteger(result));
                        return;
                    }
                    break;
                }
                case 13: {  // //  (integer division, truncates toward negative infinity)
                    if (b != 0) {
                        int64_t result;
                        if ((a ^ b) >= 0 || a == 0) {
                            result = a / b;  // Same sign or zero: C division is correct
                        } else {
                            result = ((a + 1) / b) - 1;  // Different signs: floor
                        }
                        if (result >= Oop::smallIntegerMin() && result <= Oop::smallIntegerMax()) {
                            popN(2);
                            push(Oop::fromSmallInteger(result));
                            return;
                        }
                    }
                    break;
                }
                case 14: {  // bitAnd:
                    popN(2);
                    push(Oop::fromSmallInteger(a & b));
                    return;
                }
                case 15: {  // bitOr:
                    popN(2);
                    push(Oop::fromSmallInteger(a | b));
                    return;
                }
                default:
                    break;  // @ (11) — fall through to method lookup
            }
        }
    }

    // Try to get cached well-known selector
    Oop selector;
    switch (which) {
        case 0: selector = selectors_.add; break;
        case 1: selector = selectors_.subtract; break;
        case 2: selector = selectors_.lessThan; break;
        case 3: selector = selectors_.greaterThan; break;
        case 4: selector = selectors_.lessEqual; break;
        case 5: selector = selectors_.greaterEqual; break;
        case 6: selector = selectors_.equal; break;
        case 7: selector = selectors_.notEqual; break;
        case 8: selector = selectors_.multiply; break;
        case 9: selector = selectors_.divide; break;
        default:
            // For arithmetic ops 10-15 (\\, @, bitShift:, //, bitAnd:, bitOr:),
            // look up from special selectors array, NOT from literals!
            {
                Oop specialSelectors = memory_.specialObject(SpecialObjectIndex::SpecialSelectorsArray);
                if (specialSelectors.isObject() && specialSelectors.rawBits() > 0x10000) {
                    ObjectHeader* ssHdr = specialSelectors.asObjectPtr();
                    size_t selectorSlot = which * 2;  // Each selector has 2 entries
                    if (selectorSlot < ssHdr->slotCount()) {
                        selector = ssHdr->slotAt(selectorSlot);
                    } else {
                        selector = Oop::nil();
                    }
                } else {
                    selector = Oop::nil();
                }
            }
            break;
    }

    if (selector.isNil()) {
        // Cached selector was nil — fall back to special selectors array
        Oop specialSelectors = memory_.specialObject(SpecialObjectIndex::SpecialSelectorsArray);
        if (specialSelectors.isObject() && specialSelectors.rawBits() > 0x10000) {
            ObjectHeader* ssHdr = specialSelectors.asObjectPtr();
            size_t selectorSlot = which * 2;
            if (selectorSlot < ssHdr->slotCount()) {
                selector = ssHdr->slotAt(selectorSlot);
            }
        }
    }

    if (selector.isNil()) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "arithmeticSend: special selector %d is nil at step %llu — "
                 "specialObjectsArray or SmallInteger method dict is corrupt",
                 which, (unsigned long long)g_stepNum);
        stopVM(buf);
        return;
    }

    sendSelector(selector, argCount);
}

void Interpreter::commonSend(int which) {
    // In Sista V1, bytecodes 112-127 (0x70-0x7F) are "send special selector 16-31"
    // These use the special selectors array (special object index 23)
    // The array format is: [selector0, argCount0, selector1, argCount1, ...]
    // Bytecode 192 sends special selector 16, bytecode 207 sends special selector 31

    int selectorIndex = which + 16;  // Offset by 16 from the arithmetic sends

    // Get special selectors array
    Oop specialSelectors = memory_.specialObject(SpecialObjectIndex::SpecialSelectorsArray);
    if (!specialSelectors.isObject() || specialSelectors.rawBits() < 0x10000) {
        push(receiver_);
        return;
    }

    ObjectHeader* ssArrayHdr = specialSelectors.asObjectPtr();
    size_t arraySlots = ssArrayHdr->slotCount();

    // Each selector has 2 entries: selector and argCount
    size_t selectorSlot = selectorIndex * 2;
    size_t argCountSlot = selectorIndex * 2 + 1;

    if (selectorSlot >= arraySlots || argCountSlot >= arraySlots) {
        returnValue(receiver_);
        return;
    }

    Oop selector = ssArrayHdr->slotAt(selectorSlot);
    Oop argCountOop = ssArrayHdr->slotAt(argCountSlot);

    int argCount = 0;
    if (argCountOop.isSmallInteger()) {
        argCount = static_cast<int>(argCountOop.asSmallInteger());
    } else {
        // Fallback: count colons in selector
        if (selector.isObject() && selector.rawBits() > 0x10000) {
            ObjectHeader* selHdr = selector.asObjectPtr();
            if (selHdr->isBytesObject()) {
                size_t len = selHdr->byteSize();
                const uint8_t* bytes = selHdr->bytes();
                for (size_t i = 0; i < len; i++) {
                    if (bytes[i] == ':') argCount++;
                }
            }
        }
    }

    sendSelector(selector, argCount);
}

void Interpreter::sendArithmetic(int which) {
    // Sista V1: Send arithmetic message (special selectors 0-15)
    // Delegates to existing arithmeticSend implementation
    arithmeticSend(which);
}

void Interpreter::sendSpecial(int which) {
    // Sista V1: Send special message (special selectors 0-15 map to selectors 16-31)

    if constexpr (ENABLE_DEBUG_LOGGING) {
        // Debug logging for value: and do: sends (disabled)
    }
    // Delegates to existing commonSend implementation which handles selectors 16-31
    commonSend(which);
}

void Interpreter::sendLiteralZeroArgs(int literalIndex) {
    sendSelector(literal(literalIndex), 0);
}

void Interpreter::sendLiteralOneArg(int literalIndex) {
    sendSelector(literal(literalIndex), 1);
}

void Interpreter::sendLiteralTwoArgs(int literalIndex) {
    sendSelector(literal(literalIndex), 2);
}

void Interpreter::sendSelector(Oop selector, int argCount) {
    Oop rcvr = stackValue(argCount);

    // Debug trace for sieve benchmark nil arg bug
    if (benchMode_ && selector.isObject() && selector.rawBits() > 0x10000) {
        ObjectHeader* sh = selector.asObjectPtr();
        size_t selLen = sh->isBytesObject() ? sh->byteSize() : 0;
        // Trace new: to verify Array allocation
        if (selLen == 4 && memcmp(sh->bytes(), "new:", 4) == 0) {
            fprintf(stderr, "[TRACE] new: rcvr=0x%llx arg=0x%llx (%s)\n",
                    (unsigned long long)rcvr.rawBits(),
                    (unsigned long long)stackValue(0).rawBits(),
                    stackValue(0).isSmallInteger() ? "SmallInt" : "object");
        }
        // Trace atAllPut:
        if (selLen == 9 && memcmp(sh->bytes(), "atAllPut:", 9) == 0) {
            traceAtAllPut_++;
            fprintf(stderr, "[TRACE] atAllPut: #%d rcvr=0x%llx (%s) arg=0x%llx SP=%ld FP=%ld fd=%zu\n",
                    traceAtAllPut_,
                    (unsigned long long)rcvr.rawBits(),
                    rcvr.isSmallInteger() ? "SmallInt" : memory_.classNameOf(rcvr).c_str(),
                    (unsigned long long)stackValue(0).rawBits(),
                    (long)(stackPointer_ - stackBase_), (long)(framePointer_ - stackBase_),
                    frameDepth_);
        }
        // Trace size sends on Arrays for debugging
        if (selLen == 4 && memcmp(sh->bytes(), "size", 4) == 0 && rcvr.isObject() && !rcvr.isNil()) {
            std::string rcls = memory_.classNameOf(rcvr);
            if (rcls == "Array") {
                fprintf(stderr, "[TRACE] Array>>size rcvr=0x%llx\n", (unsigned long long)rcvr.rawBits());
            }
        }
        // Trace #> sends
        if (selLen == 1 && sh->bytes()[0] == '>' && rcvr.rawBits() == memory_.nil().rawBits()) {
            fprintf(stderr, "[TRACE] nil > ... DETECTED! fd=%zu method=#%s\n",
                    frameDepth_, memory_.selectorOf(method_).c_str());
        }
        if (selLen == 12 && memcmp(sh->bytes(), "from:to:put:", 12) == 0) {
            fprintf(stderr, "[TRACE] from:to:put: argCount=%d rcvr=0x%llx\n",
                    argCount, (unsigned long long)rcvr.rawBits());
            for (int i = 0; i < argCount; i++) {
                Oop a = stackValue(argCount - 1 - i);
                fprintf(stderr, "[TRACE]   arg[%d]=0x%llx (%s)\n", i,
                        (unsigned long long)a.rawBits(),
                        a.isSmallInteger() ? "SmallInt" : a.isNil() ? "nil" : "object");
            }
            fprintf(stderr, "[TRACE]   SP=%p FP=%p base=%p spOff=%ld fpOff=%ld\n",
                    (void*)stackPointer_, (void*)framePointer_, (void*)stackBase_,
                    (long)(stackPointer_ - stackBase_), (long)(framePointer_ - stackBase_));
        }
    }

    // Selector sanity check: must be a bytes object (Symbol/ByteString)
    if (__builtin_expect(selector.isObject() && selector.rawBits() > 0x10000, 1)) {
        ObjectHeader* selHdr = selector.asObjectPtr();
        if (__builtin_expect(!selHdr->isBytesObject(), 0)) {
            static int badSelCount = 0;
            if (badSelCount++ < 10) {
                // Decode the send bytecode to find the literal index
                uint8_t bc = instructionPointer_ ? *instructionPointer_ : 0;
                int litIdx = -1;
                if (bc >= 0x80 && bc <= 0x8F) litIdx = bc & 0xF; // send short
                else if (bc >= 0x90 && bc <= 0x9F) litIdx = bc & 0xF;
                else if (bc >= 0xA0 && bc <= 0xAF) litIdx = bc & 0xF;
                else if (bc == 0xEA || bc == 0xEB) {
                    uint8_t ext = instructionPointer_ ? instructionPointer_[1] : 0;
                    litIdx = ext & 0x1F;
                }
                fprintf(stderr, "[SEL-CORRUPT #%d] selector fmt=%d cls=%u slots=%zu raw=0x%llx "
                        "bc=0x%02X litIdx=%d method=0x%llx (#%s) fd=%zu\n",
                        badSelCount, (int)selHdr->format(), selHdr->classIndex(),
                        selHdr->slotCount(), (unsigned long long)selector.rawBits(),
                        bc, litIdx, (unsigned long long)method_.rawBits(),
                        memory_.selectorOf(method_).c_str(), frameDepth_);
                // Dump the first 10 literals of the method
                if (method_.isObject() && method_.rawBits() > 0x10000) {
                    ObjectHeader* mH = method_.asObjectPtr();
                    Oop hdr = mH->slots()[0];
                    int nLit = hdr.isSmallInteger() ? ((int)hdr.asSmallInteger() & 0x7FFF) : 0;
                    fprintf(stderr, "[SEL-CORRUPT]   %d literals:", nLit);
                    for (int i = 0; i < nLit && i < 15; i++) {
                        Oop lit = mH->slots()[1 + i];
                        ObjectHeader* lH = lit.isObject() && lit.rawBits() > 0x10000
                            ? lit.asObjectPtr() : nullptr;
                        if (lH && lH->isBytesObject() && lH->byteSize() < 64)
                            fprintf(stderr, " [%d]='%.*s'", i,
                                    (int)lH->byteSize(), (const char*)lH->bytes());
                        else
                            fprintf(stderr, " [%d]=0x%llx", i,
                                    (unsigned long long)lit.rawBits());
                    }
                    fprintf(stderr, "\n");
                }
#if PHARO_JIT_ENABLED
                // Check if we just came from JIT
                fprintf(stderr, "[SEL-CORRUPT]   IP=0x%llx method bytes=%p\n",
                        (unsigned long long)(uintptr_t)instructionPointer_,
                        method_.isObject() ? (void*)method_.asObjectPtr()->bytes() : nullptr);
#endif
            }
        }
    }

    // Corruption check (cold path)
    if (__builtin_expect(rcvr.rawBits() == 0, 0)) {
        std::string selName = "(unknown)";
        if (selector.isObject() && selector.rawBits() > 0x10000) {
            ObjectHeader* selHdr = selector.asObjectPtr();
            if (selHdr->isBytesObject()) {
                selName = std::string((const char*)selHdr->bytes(), selHdr->byteSize());
            }
        }
        static int zeroCount = 0;
        zeroCount++;
        if (zeroCount <= 3) {
            fprintf(stderr, "[VM] BUG #%d: send #%s argCount=%d to receiver raw=0 in #%s fd=%zu\n",
                    zeroCount, selName.c_str(), argCount, memory_.selectorOf(method_).c_str(), frameDepth_);
            fprintf(stderr, "[VM]   method Oop=0x%llx\n", (unsigned long long)method_.rawBits());
            // Dump bytecodes around current IP
            if (method_.isObject() && method_.rawBits() > 0x10000) {
                ObjectHeader* mHdr = method_.asObjectPtr();
                uint8_t* mBytes = mHdr->bytes();
                ptrdiff_t ipOff = instructionPointer_ - mBytes;
                fprintf(stderr, "[VM]   IP offset: %td, bytecodes around:\n    ", ipOff);
                for (ptrdiff_t b = ipOff - 8; b < ipOff + 8; b++) {
                    if (b >= 0 && b < (ptrdiff_t)mHdr->byteSize())
                        fprintf(stderr, "%s%02x", b == ipOff ? "[" : " ", mBytes[b]);
                    if (b == ipOff) fprintf(stderr, "]");
                }
                fprintf(stderr, "\n");
                // Dump literals
                Oop mh = memory_.fetchPointer(0, method_);
                if (mh.isSmallInteger()) {
                    int nLit = mh.asSmallInteger() & 0x7FFF;
                    fprintf(stderr, "[VM]   %d literals:", nLit);
                    for (int i = 0; i < nLit && i < 10; i++) {
                        Oop lit = memory_.fetchPointer(1 + i, method_);
                        fprintf(stderr, " [%d]=0x%llx", i, (unsigned long long)lit.rawBits());
                    }
                    fprintf(stderr, "\n");
                }
            }
            // Full stack dump
            fprintf(stderr, "[VM]   stack (top 10):");
            for (int i = 0; i < 10; i++) {
                Oop sv = stackValue(i);
                fprintf(stderr, " [%d]=0x%llx", i, (unsigned long long)sv.rawBits());
            }
            fprintf(stderr, "\n");
            fprintf(stderr, "[VM]   Call stack (with receivers):\n");
            for (size_t f = 0; f <= frameDepth_ && f < 20; f++) {
                Oop frcv = savedFrames_[f].savedReceiver;
                fprintf(stderr, "[VM]     [%zu] #%s rcvr=0x%llx", f,
                        memory_.selectorOf(savedFrames_[f].savedMethod).c_str(),
                        (unsigned long long)frcv.rawBits());
                if (frcv.isObject() && frcv.rawBits() > 0x10000) {
                    ObjectHeader* fhdr = frcv.asObjectPtr();
                    fprintf(stderr, " (cls=%u fmt=%d)", fhdr->classIndex(), (int)fhdr->format());
                }
                fprintf(stderr, "\n");
            }
        }
        // Patch receiver to nil and continue (recoverable)
        rcvr = memory_.nil();
        stackValuePut(argCount, rcvr);
    }

    Oop rcvrClass = memory_.classOf(rcvr);

    // Trace matches: sends
    {
        static int matchTraceCount = 0;
        if (matchTraceCount < 20 && selector.isObject() && selector.rawBits() > 0x10000) {
            ObjectHeader* selHdr = selector.asObjectPtr();
            if (selHdr->isBytesObject() && selHdr->byteSize() == 8) {
                if (memcmp(selHdr->bytes(), "matches:", 8) == 0) {
                    matchTraceCount++;
                    std::string rcvrCls = memory_.classNameOf(rcvr);
                    std::string argCls = memory_.classNameOf(stackValue(0));
                    std::string argStr = "";
                    if (stackValue(0).isObject() && stackValue(0).rawBits() > 0x10000) {
                        ObjectHeader* ah = stackValue(0).asObjectPtr();
                        if (ah->isBytesObject() && ah->byteSize() <= 40) {
                            argStr = std::string((char*)ah->bytes(), ah->byteSize());
                        }
                    }
                    fprintf(stderr, "[MATCH-SEND] #matches: rcvr=%s arg='%s'(%s) method=%s\n",
                            rcvrCls.c_str(), argStr.c_str(), argCls.c_str(),
                            memory_.selectorOf(method_).c_str());
                }
            }
        }
    }

    // === GLOBAL METHOD CACHE: 2-way set-associative ===
    MethodCacheEntry* cached = probeCache(selector, rcvrClass);

    if (__builtin_expect(cached != nullptr, 1)) {

#if PHARO_JIT_ENABLED
        // Count ALL sends for JIT compilation, not just activateMethod calls
        jitRuntime_.noteMethodEntry(cached->method);

        // Populate megamorphic cache for JIT stencil probes
        {
            uint64_t tag = rcvr.rawBits() & 0x7;
            uint64_t megaKey = (tag == 0 && rcvr.rawBits() >= 0x10000)
                ? static_cast<uint64_t>(rcvr.asObjectPtr()->classIndex())
                : (tag != 0 ? (tag | 0x80000000ULL) : 0);
            if (megaKey != 0) {
                jitRuntime_.megaCacheAdd(selector.rawBits(), megaKey, cached->method.rawBits());
            }
        }
#endif

        // Getter fast path: pushRecvVar N + returnTop
        // Skip method activation — replace receiver with inst var value
        // Guard: byte objects have no named inst vars — reading their
        // "slots" returns raw byte data misinterpreted as Oops.
        if (cached->accessorIndex >= 0 && argCount == 0) {
            if (__builtin_expect(rcvr.isObject() && rcvr.rawBits() > 0x10000 &&
                                 rcvr.asObjectPtr()->isBytesObject(), 0)) {
                // Byte object: fall through to normal dispatch
            } else {
#if PHARO_JIT_ENABLED
                patchJITICAfterSend(cached->method, rcvr, selector);
#endif
                Oop ivar = memory_.fetchPointerUnchecked(cached->accessorIndex, rcvr);
                *(stackPointer_ - 1) = ivar;  // replace receiver in-place
                return;
            }
        }

        // Setter fast path: popStoreRecvVar N + returnReceiver
        // Skip method activation — store arg in inst var, leave receiver on stack
        // Same byte-object guard as getter path.
        if (cached->setterIndex >= 0 && argCount == 1) {
            // Debug: detect setter fast-path on unexpected selectors
            if (benchMode_ && selector.isObject() && selector.rawBits() > 0x10000) {
                ObjectHeader* _sh = selector.asObjectPtr();
                if (_sh->isBytesObject() && _sh->byteSize() == 9 && memcmp(_sh->bytes(), "atAllPut:", 9) == 0) {
                    fprintf(stderr, "[BUG-SETTER] atAllPut: hitting setter fast path! setterIdx=%d rcvr=0x%llx method=0x%llx\n",
                            cached->setterIndex, (unsigned long long)rcvr.rawBits(), (unsigned long long)cached->method.rawBits());
                }
            }
            if (__builtin_expect(rcvr.isObject() && rcvr.rawBits() > 0x10000 &&
                                 rcvr.asObjectPtr()->isBytesObject(), 0)) {
                // Byte object: fall through to normal dispatch
            } else {
#if PHARO_JIT_ENABLED
                patchJITICAfterSend(cached->method, rcvr, selector);
#endif
                Oop value = stackValue(0);  // the argument
                memory_.storePointerUnchecked(cached->setterIndex, rcvr, value);
                pop();  // pop argument, leave receiver as return value
                return;
            }
        }

        // Identity fast path: returnReceiver (yourself, asXxx identity methods)
        // Just pop args and leave receiver
        if (cached->returnsSelf && argCount == 0) {
#if PHARO_JIT_ENABLED
            patchJITICAfterSend(cached->method, rcvr, selector);
#endif
            return;
        }

        int primIdx = cached->primitiveIndex;
        // Debug: trace atAllPut: cache path
        if (benchMode_ && selector.isObject() && selector.rawBits() > 0x10000) {
            ObjectHeader* _sh2 = selector.asObjectPtr();
            if (_sh2->isBytesObject() && _sh2->byteSize() == 9 && memcmp(_sh2->bytes(), "atAllPut:", 9) == 0) {
                fprintf(stderr, "[CACHE-HIT] atAllPut: prim=%d getter=%d setter=%d retSelf=%d method=0x%llx\n",
                        primIdx, cached->accessorIndex, cached->setterIndex,
                        cached->returnsSelf ? 1 : 0, (unsigned long long)cached->method.rawBits());
            }
        }
        if (primIdx > 0) {
            argCount_ = argCount;
            primitiveFailed_ = false;
            primFailCode_ = 0;
            newMethod_ = cached->method;
            PrimitiveResult result = executePrimitive(primIdx, argCount);
            if (result == PrimitiveResult::Success) {
#if PHARO_JIT_ENABLED
                patchJITICAfterSend(cached->method, rcvr, selector);
#endif
                return;
            }
        }
        if (__builtin_expect(primIdx == 198, 0)) {
            suppressContextSwitch_ = true;
        }
        if (__builtin_expect(!cached->method.isObject() || cached->method.rawBits() < 0x10000 ||
                             !cached->method.asObjectPtr()->isCompiledMethod(), 0)) {
            invokeObjectAsMethod(cached->method, selector, argCount);
            return;
        }
#if PHARO_JIT_ENABLED
        patchJITICAfterSend(cached->method, rcvr, selector);
#endif
        activateMethod(cached->method, argCount);
        return;
    }

    // === FULL LOOKUP ===
    Oop method = lookupMethod(selector, rcvrClass);

    if (method.isNil()) {
        sendDoesNotUnderstand(selector, argCount);
        return;
    }

    if (__builtin_expect(!method.isObject() || method.rawBits() < 0x10000 ||
                         !method.asObjectPtr()->isCompiledMethod(), 0)) {
        invokeObjectAsMethod(method, selector, argCount);
        return;
    }

#if PHARO_JIT_ENABLED
    // Also count uncached sends for JIT
    jitRuntime_.noteMethodEntry(method);

    // Populate megamorphic cache for JIT stencil probes
    {
        uint64_t tag = rcvr.rawBits() & 0x7;
        uint64_t megaKey = (tag == 0 && rcvr.rawBits() >= 0x10000)
            ? static_cast<uint64_t>(rcvr.asObjectPtr()->classIndex())
            : (tag != 0 ? (tag | 0x80000000ULL) : 0);
        if (megaKey != 0) {
            jitRuntime_.megaCacheAdd(selector.rawBits(), megaKey, method.rawBits());
        }
    }
#endif

    // Cache the method
    cacheMethod(selector, rcvrClass, method);
    int primIndex = primitiveIndexOf(method);

    if (primIndex > 0) {
        argCount_ = argCount;
        primitiveFailed_ = false;
        primFailCode_ = 0;
        newMethod_ = method;
        PrimitiveResult result = executePrimitive(primIndex, argCount);
        if (result == PrimitiveResult::Success) {
#if PHARO_JIT_ENABLED
            patchJITICAfterSend(method, rcvr, selector);
#endif
            return;
        }
    }

    if (__builtin_expect(primIndex == 198, 0)) {
        suppressContextSwitch_ = true;
    }

#if PHARO_JIT_ENABLED
    patchJITICAfterSend(method, rcvr, selector);
#endif
    activateMethod(method, argCount);

    // Watchdog diagnostics (every 1024 sends)
    if (__builtin_expect((++sendCount_ & 0x3FF) == 0, 0)) {
        if (selector.isObject() && selector.rawBits() > 0x10000) {
            ObjectHeader* selHdr = selector.asObjectPtr();
            if (selHdr->isBytesObject() && selHdr->byteSize() < 63) {
                memcpy(g_watchdogSelector, selHdr->bytes(), selHdr->byteSize());
                g_watchdogSelector[selHdr->byteSize()] = '\0';
            }
        }
        Oop rcvrCls = memory_.classOf(rcvr);
        if (rcvrCls.isObject() && rcvrCls.rawBits() > 0x10000) {
            Oop nameOop = memory_.fetchPointer(6, rcvrCls);
            if (nameOop.isObject() && nameOop.rawBits() > 0x10000) {
                ObjectHeader* nameHdr = nameOop.asObjectPtr();
                if (nameHdr->isBytesObject() && nameHdr->byteSize() < 63) {
                    memcpy(g_watchdogReceiverClass, nameHdr->bytes(), nameHdr->byteSize());
                    g_watchdogReceiverClass[nameHdr->byteSize()] = '\0';
                }
            }
        }
    }
}

// ===== METHOD LOOKUP =====

Oop Interpreter::lookupMethod(Oop selector, Oop classOop) {
    Oop currentClass = classOop;
    int depth = 0;

    Oop nilObj = memory_.specialObject(SpecialObjectIndex::NilObject);
    auto isNilOrEnd = [nilObj](Oop o) -> bool {
        return o.isNil() || o.rawBits() == nilObj.rawBits() || o.rawBits() < 0x10000;
    };

    while (!isNilOrEnd(currentClass) && currentClass.isObject() && depth < 100) {
        Oop methodDict = methodDictOf(currentClass);

        if (!isNilOrEnd(methodDict) && methodDict.isObject()) {
            Oop method = lookupInMethodDict(methodDict, selector);
            if (!isNilOrEnd(method) && method.isObject()) {
                return method;
            }
        }
        currentClass = superclassOf(currentClass);
        depth++;
    }

    return Oop::nil();  // Not found
}

Oop Interpreter::lookupInMethodDict(Oop methodDict, Oop selector) const {
    // Pharo MethodDictionary layout (IndexableWithFixed, format 3):
    //   slot 0: tally (SmallInteger)
    //   slot 1: values array (Array of CompiledMethods)
    //   slots 2+: keys (Symbols) stored inline, indexed by hash
    //
    // Key at slot[i+2] corresponds to method at valuesArray[i]
    // Lookup uses open addressing with linear probing: hash & (size-1), wrap around.
    // Symbols are interned, so identity comparison (Oop equality) suffices.

    if (!methodDict.isObject()) return Oop::nil();

    ObjectHeader* mdHeader = methodDict.asObjectPtr();
    size_t mdSlotCount = mdHeader->slotCount();
    if (mdSlotCount < 3) return Oop::nil();

    // Get values array (slot 1) — methodDict validated above
    Oop valuesArray = memory_.fetchPointerUnchecked(1, methodDict);
    if (!valuesArray.isObject() || valuesArray.rawBits() < 0x10000) return Oop::nil();

    ObjectHeader* valuesHeader = valuesArray.asObjectPtr();
    size_t valuesSize = valuesHeader->slotCount();
    size_t keySlotCount = mdSlotCount - 2;  // number of key slots (power of 2)

    if (keySlotCount == 0) return Oop::nil();

    Oop nilObj = memory_.specialObject(SpecialObjectIndex::NilObject);
    uint64_t nilBits = nilObj.rawBits();

    // Hash-based probe: start at (identityHash & (keySlotCount - 1)), linear probe
    uint32_t selectorHash = 0;
    if (selector.isObject()) {
        selectorHash = selector.asObjectPtr()->identityHash();
    }
    size_t mask = keySlotCount - 1;  // keySlotCount is power of 2
    size_t startIndex = selectorHash & mask;
    size_t i = startIndex;

    do {
        // methodDict already validated above — skip isObject/isValidPointer
        Oop key = memory_.fetchPointerUnchecked(i + 2, methodDict);

        // nil slot = end of probe chain (key not found)
        if (key.isNil() || key.rawBits() == nilBits) {
            return Oop::nil();
        }

        // Identity match — Symbols are interned
        if (key.rawBits() == selector.rawBits()) {
            if (i < valuesSize) {
                return memory_.fetchPointerUnchecked(i, valuesArray);
            }
            return Oop::nil();
        }

        i = (i + 1) & mask;
    } while (i != startIndex);  // Full wrap = not found

    return Oop::nil();
}

MethodCacheEntry* Interpreter::probeCache(Oop selector, Oop classOop) {
    uint64_t selBits = selector.rawBits();
    uint64_t clsBits = classOop.rawBits();
    size_t mask = MethodCacheSize - 1;

    // Primary probe
    size_t h1 = static_cast<size_t>(selBits ^ clsBits) & mask;
    MethodCacheEntry& e1 = methodCache_[h1];
    if (e1.selector == selector && e1.classOop == classOop) {
        return &e1;
    }

    // Secondary probe (rotated hash reduces collision aliasing)
    size_t h2 = static_cast<size_t>((selBits >> 2) ^ (clsBits << 1) ^ clsBits) & mask;
    MethodCacheEntry& e2 = methodCache_[h2];
    if (e2.selector == selector && e2.classOop == classOop) {
        return &e2;
    }

    return nullptr;
}

// Detect trivial getter/setter methods from their bytecodes.
// Getter: pushRecvVar N + returnTop → returns inst var index
// Setter: popStoreRecvVar N + returnReceiver → returns inst var index
struct TrivialMethodInfo {
    int16_t getterIndex = -1;  // >=0: getter
    int16_t setterIndex = -1;  // >=0: setter
    bool returnsSelf = false;  // just returnReceiver (yourself)
};

static TrivialMethodInfo detectTrivialMethod(Oop method, ObjectMemory& memory) {
    TrivialMethodInfo info;
    if (!method.isObject() || method.rawBits() < 0x10000) return info;
    ObjectHeader* hdr = method.asObjectPtr();
    if (!hdr->isCompiledMethod()) return info;

    Oop header = memory.fetchPointer(0, method);
    if (!header.isSmallInteger()) return info;
    int64_t headerBits = header.asSmallInteger();
    int numLiterals = headerBits & 0x7FFF;

    uint8_t* bytes = hdr->bytes();
    size_t bcStart = (1 + numLiterals) * 8;
    size_t totalBytes = hdr->byteSize();
    size_t bcLen = totalBytes - bcStart;
    if (bcLen < 2) return info;

    uint8_t bc0 = bytes[bcStart];
    uint8_t bc1 = bytes[bcStart + 1];

    // Getter: pushRecvVar N (0x00-0x0F) + returnTop (0x5C)
    if (bc0 <= 0x0F && bc1 == 0x5C) {
        info.getterIndex = (int16_t)bc0;
        return info;
    }

    // Getter: extended pushRecvVar (0xE2 N) + returnTop (0x5C)
    if (bcLen >= 3 && bc0 == 0xE2 && bytes[bcStart + 2] == 0x5C) {
        info.getterIndex = (int16_t)bc1;
        return info;
    }

    // Setter: popStoreRecvVar N (0xC8-0xCF) + returnReceiver (0x58)
    // The 1-arg setter: receiver is at stackValue(1), arg at stackValue(0)
    // popStoreRecvVar pops arg and stores in inst var, returnReceiver returns self
    if (bc0 >= 0xC8 && bc0 <= 0xCF && bc1 == 0x58) {
        info.setterIndex = (int16_t)(bc0 - 0xC8);
        return info;
    }

    // Setter: extended (0xF0 N) + returnReceiver (0x58)
    if (bcLen >= 3 && bc0 == 0xF0 && bytes[bcStart + 2] == 0x58) {
        info.setterIndex = (int16_t)bc1;
        return info;
    }

    // Identity: returnReceiver (0x58) alone — "yourself" and similar
    if (bc0 == 0x58) {
        info.returnsSelf = true;
        return info;
    }

    return info;
}

void Interpreter::cacheMethod(Oop selector, Oop classOop, Oop method) {
    uint64_t selBits = selector.rawBits();
    uint64_t clsBits = classOop.rawBits();
    size_t mask = MethodCacheSize - 1;
    int primIndex = primitiveIndexOf(method);
    TrivialMethodInfo trivial = detectTrivialMethod(method, memory_);

    // Debug: log caching of specific selectors
    {
        static int cacheLogCount = 0;
        if (selector.isObject() && selector.rawBits() > 0x10000) {
            std::string selName = memory_.oopToString(selector);
            if (selName == "keys" || selName == "selectors" || selName == "methodDict" ||
                selName == "allSelectors" || selName == "keysDo:" || selName == "superclass" ||
                selName == "allTestSelectors") {
                cacheLogCount++;
                if (cacheLogCount <= 50) {
                    fprintf(stderr, "[CACHE-%d] #%s -> getter=%d setter=%d retSelf=%d prim=%d cls=0x%llx method=0x%llx\n",
                            cacheLogCount, selName.c_str(), trivial.getterIndex, trivial.setterIndex,
                            trivial.returnsSelf ? 1 : 0, primIndex,
                            (unsigned long long)clsBits,
                            (unsigned long long)method.rawBits());
                }
            }
        }
    }

    // Primary slot: use if empty or same key
    size_t h1 = static_cast<size_t>(selBits ^ clsBits) & mask;
    MethodCacheEntry& e1 = methodCache_[h1];
    if (e1.selector.isNil() || (e1.selector == selector && e1.classOop == classOop)) {
        e1.selector = selector;
        e1.classOop = classOop;
        e1.method = method;
        e1.primitiveIndex = primIndex;
        e1.primitive = nullptr;
        e1.accessorIndex = trivial.getterIndex;
        e1.setterIndex = trivial.setterIndex;
        e1.returnsSelf = trivial.returnsSelf;
        return;
    }

    // Secondary slot: evict
    size_t h2 = static_cast<size_t>((selBits >> 2) ^ (clsBits << 1) ^ clsBits) & mask;
    MethodCacheEntry& e2 = methodCache_[h2];
    e2.selector = selector;
    e2.classOop = classOop;
    e2.method = method;
    e2.primitiveIndex = primIndex;
    e2.primitive = nullptr;
    e2.accessorIndex = trivial.getterIndex;
    e2.setterIndex = trivial.setterIndex;
    e2.returnsSelf = trivial.returnsSelf;
}

size_t Interpreter::cacheHash(Oop selector, Oop classOop) const {
    uint64_t h = selector.rawBits() ^ classOop.rawBits();
    return static_cast<size_t>(h) & (MethodCacheSize - 1);
}

// ===== PROCESS TERMINATION AND RECOVERY =====

void Interpreter::terminateAndSwitchProcess() {
    // Terminate the current process and switch to the next runnable one.
    // Used by stack overflow handler and watchdog to prevent a single
    // runaway process from hanging the entire VM.
    terminateCurrentProcess();  // Mark process as dead, remove from scheduler

    Oop nextProcess = wakeHighestPriority();
    if (nextProcess.isNil() || !nextProcess.isObject()) {
        stopVM("No runnable process available after termination");
        return;
    }
    setActiveProcess(nextProcess);
    Oop newContext = memory_.fetchPointer(ProcessSuspendedContextIndex, nextProcess);
    memory_.storePointer(ProcessSuspendedContextIndex, nextProcess, memory_.nil());
    executeFromContext(newContext);
}

void Interpreter::handleStackOverflow(int argCount) {
    // Stack overflow — terminate this process and switch to the next one.
    // This is correct VM behavior: a runaway process should not kill the
    // entire VM. The scheduler continues with other processes.
    // Pop args+receiver that the send bytecode already pushed
    popN(argCount + 1);

    terminateAndSwitchProcess();
}

// ===== METHOD ACTIVATION =====

void Interpreter::activateMethod(Oop method, int argCount) {
    if (__builtin_expect(!pushFrame(method, argCount), 0)) {
        handleStackOverflow(argCount);
        return;
    }

    // Set up new method
    method_ = method;
    argCount_ = argCount;
    closure_ = memory_.nil();  // Method activations have no closure

    // Determine homeMethod_ based on whether this is a CompiledMethod or CompiledBlock.
    // From sendSelector(), method is always a CompiledMethod (method dicts don't contain
    // blocks). CompiledBlock activation is handled via primitiveFullClosureValue.
    // Check once and take the fast path for the common case.
    if (__builtin_expect(method.isObject(), 1)) {
        ObjectHeader* methodHdr = method.asObjectPtr();
        uint32_t classIdx = methodHdr->classIndex();

        if (__builtin_expect(classIdx == compiledBlockClassIndex_, 0)) {
            // CompiledBlock (rare from activateMethod — usually blocks go through primitiveFullClosureValue)
            homeMethod_ = method;

            Oop slot2 = memory_.fetchPointerUnchecked(2, method);
            if (slot2.isObject()) {
                ObjectHeader* slot2Hdr = slot2.asObjectPtr();
                if (slot2Hdr->classIndex() == compiledMethodClassIndex_) {
                    homeMethod_ = slot2;
                }
            }

            if (homeMethod_ == method) {
                Oop homeCandidate = memory_.fetchPointerUnchecked(0, method);
                int maxHops = 10;
                while (homeCandidate.isObject() && maxHops-- > 0) {
                    ObjectHeader* candidateHdr = homeCandidate.asObjectPtr();
                    uint32_t candidateCls = candidateHdr->classIndex();
                    if (candidateCls == compiledMethodClassIndex_) {
                        homeMethod_ = homeCandidate;
                        break;
                    } else if (candidateCls == compiledBlockClassIndex_) {
                        homeCandidate = memory_.fetchPointerUnchecked(0, homeCandidate);
                    } else {
                        break;
                    }
                }
            }
        } else {
            homeMethod_ = method;
        }
    } else {
        homeMethod_ = method;
    }

    // Get receiver from stack (now in the frame)
    receiver_ = argument(0);  // First "argument" slot is actually receiver




    // Trace fullCheck activation specifically (disabled for performance)
    if constexpr (ENABLE_DEBUG_LOGGING) {
        // TRACE: Inside debug logging
        std::string methodSelector = "";
        if (method.isObject() && method.rawBits() > 0x10000) {
            Oop hdr = memory_.fetchPointer(0, method);
            if (hdr.isSmallInteger()) {
                size_t numLits = hdr.asSmallInteger() & 0x7FFF;
                // Bound check: numLits should not exceed actual slots
                ObjectHeader* mH = method.asObjectPtr();
                size_t actualSlots = mH->slotCount();
                if (numLits >= 2 && numLits <= actualSlots) {
                    Oop sel = memory_.fetchPointer(numLits - 1, method);
                    if (sel.isObject() && sel.rawBits() > 0x10000) {
                        ObjectHeader* sHdr = sel.asObjectPtr();
                        if (sHdr->isBytesObject() && sHdr->byteSize() < 50) {
                            methodSelector = std::string((char*)sHdr->bytes(), sHdr->byteSize());
                        }
                    }
                }
            }
        }
        if (methodSelector == "fullCheck") {
            if constexpr (ENABLE_DEBUG_LOGGING) {
            }
        }
        // TRACE: After fullCheck section
    }

    if constexpr (ENABLE_DEBUG_LOGGING) {
    static FILE* actLog = nullptr;
    static int actCount = 0;
    if (!actLog) actLog = nullptr;
    if (actLog && actCount < 200) {
        // TRACE: Inside actLog && actCount condition
        std::string selStr = memory_.selectorOf(method);
        std::string rcvrName = memory_.classNameOf(receiver_);
        // Log class method activations - focus on 'default' and Session-related
        bool shouldLog = (selStr == "default" || selStr == "current" || selStr == "currentSession" ||
                          rcvrName == "SessionManager" || rcvrName.find("Session") != std::string::npos ||
                          actCount < 20);  // Also log first 20 for context
        if (shouldLog) {
            actCount++;
            fprintf(actLog, "[ACT #%d] #%s receiver=%s (0x%llx) argCount=%d\n",
                    actCount, selStr.c_str(), rcvrName.c_str(),
                    (unsigned long long)receiver_.rawBits(), argCount);
            // Also log first few slots of receiver if it looks like a class
            if (receiver_.isObject() && receiver_.rawBits() > 0x10000) {
                ObjectHeader* rh = receiver_.asObjectPtr();
                if (rh->slotCount() >= 12) {
                    Oop slot11 = memory_.fetchPointer(11, receiver_);
                    fprintf(actLog, "  slot[11]=0x%llx (likely class instvar)\n",
                            (unsigned long long)slot11.rawBits());
                }
            }
            fflush(actLog);
        }
    }
    } // end if constexpr (ENABLE_DEBUG_LOGGING)

    // Debug: trace atAllPut: activation
    if (benchMode_ && traceAtAllPut_ > 0) {
        std::string sel = memory_.selectorOf(method);
        if (sel == "atAllPut:") {
            fprintf(stderr, "[ACT-AAP] #%d activateMethod atAllPut: fd=%zu SP=%ld FP=%ld rcvr=0x%llx\n",
                    traceAtAllPut_, frameDepth_,
                    (long)(stackPointer_ - stackBase_), (long)(framePointer_ - stackBase_),
                    (unsigned long long)receiver_.rawBits());
            // Dump stack around FP
            for (int i = -2; i <= 5; i++) {
                Oop* slot = framePointer_ + i;
                if (slot >= stackBase_ && slot < stackBase_ + 1024) {
                    fprintf(stderr, "[ACT-AAP]   FP[%d]=0x%llx%s\n", i,
                            (unsigned long long)slot->rawBits(),
                            slot->isSmallInteger() ? " (smi)" : slot->isNil() ? " (nil)" : "");
                }
            }
            // Dump bytecodes of the method on first call
            if (traceAtAllPut_ == 1) {
                ObjectHeader* mHdr = method.asObjectPtr();
                Oop hdr = memory_.fetchPointer(0, method);
                if (hdr.isSmallInteger()) {
                    int64_t hBits = hdr.asSmallInteger();
                    int nLits = hBits & 0x7FFF;
                    int nTemps = (hBits >> 18) & 0x3F;
                    int nArgs = (hBits >> 24) & 0xF;
                    bool hasPrim = (hBits >> 16) & 1;
                    size_t bcStart = (1 + nLits) * 8;
                    size_t totalBytes = mHdr->byteSize();
                    fprintf(stderr, "[AAP-BC] method=0x%llx nLits=%d nTemps=%d nArgs=%d hasPrim=%d\n",
                            (unsigned long long)method.rawBits(), nLits, nTemps, nArgs, hasPrim);
                    fprintf(stderr, "[AAP-BC] bytecodes (%zu bytes):", totalBytes - bcStart);
                    uint8_t* bytes = mHdr->bytes();
                    for (size_t i = bcStart; i < totalBytes && i < bcStart + 30; i++) {
                        fprintf(stderr, " %02X", bytes[i]);
                    }
                    fprintf(stderr, "\n");
                    // Also dump literals
                    fprintf(stderr, "[AAP-BC] literals:");
                    for (int li = 1; li <= nLits && li <= 10; li++) {
                        Oop lit = memory_.fetchPointer(li, method);
                        if (lit.isObject() && lit.rawBits() > 0x10000) {
                            ObjectHeader* lh = lit.asObjectPtr();
                            if (lh->isBytesObject() && lh->byteSize() < 40) {
                                fprintf(stderr, " [%d]='%.*s'", li, (int)lh->byteSize(), (char*)lh->bytes());
                            } else {
                                fprintf(stderr, " [%d]=0x%llx", li, (unsigned long long)lit.rawBits());
                            }
                        } else {
                            fprintf(stderr, " [%d]=0x%llx", li, (unsigned long long)lit.rawBits());
                        }
                    }
                    fprintf(stderr, "\n");
                }
            }
        }
    }

    // Set instruction pointer to start of bytecodes
    ObjectHeader* methodObj = method_.asObjectPtr();

    Oop methodHeader = memory_.fetchPointer(0, method_);
    if (__builtin_expect(!methodHeader.isSmallInteger(), 0)) {
        // Method header must be a SmallInteger. If it's not, the method object
        // is corrupted (possibly by GC or by activating a non-method object).
        abort();
    }
    int64_t headerBits = methodHeader.asSmallInteger();
    int numLiterals = headerBits & 0x7FFF;  // bits 0-14 are numLiterals

    // Bytecode set: sign bit (bit 63) = 0 for V3PlusClosures, 1 for SistaV1.
    // Only Sista V1 (Pharo 10+) is supported.
    usesSistaV1_ = headerBits < 0;
    if (__builtin_expect(!usesSistaV1_, 0)) {
        stopVM("V3PlusClosures bytecode set not supported (requires Pharo 10+ / Sista V1)");
        return;
    }

    uint8_t* methodBytes = methodObj->bytes();
    size_t bytecodeStart = (1 + numLiterals) * 8;
    instructionPointer_ = methodBytes + bytecodeStart;

    // Skip past callPrimitive bytecode (0xF8 lowByte highByte) if present.
    // Primitive methods start with callPrimitive which should be skipped
    // when the primitive fails and we fall through to execute bytecodes.
    // If <primitive: N error: ec> is declared, 0xF5 (Store Temp #i) follows callPrimitive.
    // We must skip that too and store the error object directly in the temp.
    if (instructionPointer_[0] == 0xF8) {
        instructionPointer_ += 3;  // Skip 0xF8 + 2 bytes of primitive index

        // Check for "Store Temporary Variable #i" (0xF5 i) after callPrimitive
        // This is the <primitive: N error: ec> pattern — skip the store and write error directly
        if (instructionPointer_[0] == 0xF5) {
            int tempIndex = instructionPointer_[1];
            instructionPointer_ += 2;  // Skip 0xF5 + temp index byte

            // Store error object in the specified temp if primitive failed
            if (primFailCode_ != 0) {
                Oop errorObj = getErrorObjectFromPrimFailCode();
                *(framePointer_ + 1 + tempIndex) = errorObj;
            }
            primFailCode_ = 0;
            osErrorCode_ = 0;
        }
    }

    // Set bytecode end
    size_t totalBytes = methodObj->byteSize();
    bytecodeEnd_ = methodBytes + totalBytes;

              // << " rawHdr=0x" << std::hex << methodHeader.rawBits()
              // << " hdrBits=" << headerBits << std::dec
              // << " numLiterals=" << numLiterals << " bytecodeStart=" << bytecodeStart
              // << " totalBytes=" << totalBytes
              // << " homeMethod=" << (homeMethod_ == method_ ? "same" : "different");
    if (homeMethod_ != method_ && homeMethod_.isObject()) {
        Oop homeHeader = memory_.fetchPointer(0, homeMethod_);
        if (homeHeader.isSmallInteger()) {
            int64_t hBits = homeHeader.asSmallInteger();
            // std::cerr << " (homeLiterals=" << (hBits & 0x7FFF) << ")";
        }
    }
    // std::cerr; // DEBUG

    // Show first few bytecodes for debugging
    for (size_t i = 0; i < std::min((size_t)16, totalBytes - bytecodeStart); i++) {
        // std::cerr << std::hex << (int)methodBytes[bytecodeStart + i] << " ";
    }
    // std::cerr << std::dec; // DEBUG

#if PHARO_JIT_ENABLED
    // Try JIT execution. If it handles the method, it pops the frame
    // and pushes the return value — the dispatch loop continues with
    // the caller's next bytecode.
    if (tryJITActivation(method, argCount)) {
        return;  // JIT handled it
    }
    // Otherwise fall through to interpreter execution via the dispatch loop
#endif
}

void Interpreter::activateBlock(Oop block, int argCount) {
#if PHARO_JIT_ENABLED
    // Clear stale IC patch pointer — block activation means sends inside the
    // block are unrelated to the JIT send that set pendingICPatch_.
    pendingICPatch_ = nullptr;
#endif

    // BlockClosure/FullBlockClosure layout:
    // 0: outerContext
    // 1: startPC (SmallInteger) for old BlockClosure, OR
    //    compiledBlock (Object) for FullBlockClosure
    // 2: numArgs (SmallInteger)
    // 3+: copied values

    // block is a known-valid FullBlockClosure/BlockClosure
    Oop slot1 = memory_.fetchPointerUnchecked(1, block);
    Oop outerContext = memory_.fetchPointerUnchecked(0, block);

    Oop methodToExecute;
    uint8_t* startAddress = nullptr;
    Oop homeMethodForNLR = memory_.nil();  // The enclosing CompiledMethod for NLR home frame detection

    if (slot1.isSmallInteger()) {
        // Old-style BlockClosure: slot 1 is startPC
        int64_t startPC = slot1.asSmallInteger();
        // Get the method from outer context
        Oop outerMethod = memory_.fetchPointerUnchecked(3, outerContext);
        methodToExecute = outerMethod;
        ObjectHeader* methodObj = outerMethod.asObjectPtr();
        startAddress = methodObj->bytes() + startPC;
        // For old-style blocks, the outerMethod IS the home method for NLR
        // The block's bytecodes live inside outerMethod, so ^value should
        // return from outerMethod's frame
        homeMethodForNLR = outerMethod;
    } else if (slot1.isObject()) {
        // FullBlockClosure: slot 1 is compiledBlock (the actual method to execute)
        Oop compiledBlock = slot1;

        // Validate that compiledBlock is actually a CompiledMethod/CompiledBlock
        // (format >= 24 per Spur object format spec)
        ObjectHeader* blockObj = compiledBlock.asObjectPtr();
        if (!blockObj->isCompiledMethod()) {
            primitiveFail();
            return;
        }

        methodToExecute = compiledBlock;
        // compiledBlock validated above (isCompiledMethod check)
        Oop header = memory_.fetchPointerUnchecked(0, compiledBlock);
        int64_t headerBits = header.asSmallInteger();
        int numLiterals = headerBits & 0x7FFF;  // bits 0-14
        size_t bytecodeOffset = (1 + numLiterals) * 8;
        startAddress = blockObj->bytes() + bytecodeOffset;

        // For FullBlockClosure, get the home method (the enclosing CompiledMethod)
        // The last literal of a CompiledBlock is the enclosing method/block (outerCode).
        // For NESTED blocks, this might be another CompiledBlock, so we need to
        // follow the chain until we reach the actual CompiledMethod (not a block).
        //
        // We identify a CompiledBlock vs CompiledMethod by checking if its last literal
        // is also a CompiledMethod/Block. If so, it's a block (with outerCode).
        // If not, it's the home method.
        if (numLiterals >= 1) {
            Oop enclosingCode = memory_.fetchPointerUnchecked(numLiterals, compiledBlock);
            // Follow the chain of enclosing blocks until we reach the home method
            int chainDepth = 0;
            while (enclosingCode.isObject() && enclosingCode.rawBits() > 0x10000 && chainDepth < 20) {
                ObjectHeader* ecHdr = enclosingCode.asObjectPtr();
                if (!ecHdr->isCompiledMethod()) break;

                // Get this code's header and last literal
                Oop ecHeader = memory_.fetchPointerUnchecked(0, enclosingCode);
                if (!ecHeader.isSmallInteger()) break;
                int ecNumLits = ecHeader.asSmallInteger() & 0xFFFF;
                if (ecNumLits < 1) {
                    // No literals - this is the home method
                    homeMethodForNLR = enclosingCode;
                    break;
                }

                Oop ecLastLit = memory_.fetchPointerUnchecked(ecNumLits, enclosingCode);

                // Check if last literal is a CompiledMethod/Block
                bool lastLitIsCode = false;
                if (ecLastLit.isObject() && ecLastLit.rawBits() > 0x10000) {
                    ObjectHeader* llHdr = ecLastLit.asObjectPtr();
                    lastLitIsCode = llHdr->isCompiledMethod();
                }

                if (!lastLitIsCode) {
                    // Last literal is not compiled code - this is the home method
                    homeMethodForNLR = enclosingCode;
                    break;
                }

                // Last literal is compiled code - this is a CompiledBlock
                // Continue following the chain to find the home method
                enclosingCode = ecLastLit;
                chainDepth++;
            }

            // If we ran out of chain or hit an error, use whatever we have
            if (homeMethodForNLR.isNil() && enclosingCode.isObject() && enclosingCode.rawBits() > 0x10000) {
                ObjectHeader* ecHdr = enclosingCode.asObjectPtr();
                if (ecHdr->isCompiledMethod()) {
                    homeMethodForNLR = enclosingCode;
                }
            }
        }
    } else {
        primitiveFail();
        return;
    }

    if (!pushFrame(methodToExecute, argCount)) {
        handleStackOverflow(argCount);
        return;
    }

    // Set current closure for this block activation
    // (pushFrame already saved the caller's closure_ in the saved frame)
    closure_ = block;

    // For blocks: set the home frame depth for non-local returns
    // The home frame is where the block was LEXICALLY created, not just the
    // first non-block frame on the call stack.
    //
    // For FullBlockClosure, we need to find the frame executing the method
    // that lexically contains this block. We do this by:
    // 1. Getting the home method from the CompiledBlock (the last literal is the enclosing method)
    // 2. Walking up the frame stack to find a frame executing that home method
    {
    }
    if (frameDepth_ >= 1 && homeMethodForNLR.isObject() && !homeMethodForNLR.isNil()) {
        size_t homeFrame = SIZE_MAX;  // Default: not found

        // Use the home method we extracted from the CompiledBlock's last literal
        Oop homeMethodOop = homeMethodForNLR;

        // Walk up the frame stack looking for the frame executing our home method.
        //
        // Frame indexing:
        //   pushFrame saves current state to savedFrames_[old_fd], then increments fd.
        //   So savedFrames_[X].savedMethod is the method at depth X (saved when entering X+1).
        //
        // We search ALL saved frames (0 to fd-1). The topmost (savedFrames_[fd-1]) was just
        // saved by pushFrame in activateBlock — it's the CALLER's state, not the block's.
        //
        // When savedFrames_[X].savedMethod matches homeMethod:
        //   homeFrame = X
        //   NLR unwinds: while (fd > X) popFrame → fd = X
        //   returnValue: if X > 0, popFrame restores savedFrames_[X-1] (caller's caller), fd = X-1
        //                if X == 0, context-based return via activeContext_ sender chain
        for (size_t i = frameDepth_; i > 0; i--) {
            Oop savedMethod = savedFrames_[i - 1].savedMethod;

            // Check if this saved method matches our home method
            if (savedMethod.rawBits() == homeMethodOop.rawBits()) {
                homeFrame = i - 1;
                break;
            }

            // Also check if this frame is a block whose home is our home method
            // This handles nested blocks
            if (savedMethod.isObject() && savedMethod.rawBits() > 0x10000) {
                ObjectHeader* mHdr = savedMethod.asObjectPtr();
                if (mHdr->isCompiledMethod()) {
                    // Check if this is a CompiledBlock (last literal is a CompiledMethod)
                    Oop header = memory_.fetchPointer(0, savedMethod);
                    if (header.isSmallInteger()) {
                        int numLits = header.asSmallInteger() & 0x7FFF;
                        if (numLits >= 2) {
                            Oop outerLit = memory_.fetchPointer(numLits - 1, savedMethod);
                            // For CompiledBlock, last literal is the enclosing method
                            if (outerLit.rawBits() == homeMethodOop.rawBits()) {
                                // This frame's block was also created in our home method
                                // but we want the method frame itself, not another block frame
                                continue;
                            }
                        }
                    }
                }
            }
        }

        // If we couldn't find the home method in savedFrames_, also search the context chain.
        // This happens after exception handling: contexts were materialized and savedFrames_
        // doesn't contain the home method anymore - it's in the context chain.
        if (homeFrame == SIZE_MAX && activeContext_.isObject() && !activeContext_.isNil()) {
            // Search context chain for home method
            // If found, NLR will need to use context-based unwinding (frameDepth_=0 path)
            // We signal this by setting homeFrame to 0 and ensuring the NLR code handles it
            Oop ctx = activeContext_;
            int searchDepth = 0;
            while (ctx.isObject() && !ctx.isNil() && searchDepth < 200) {
                Oop ctxMethod = memory_.fetchPointer(3, ctx);
                if (ctxMethod.rawBits() == homeMethodOop.rawBits()) {
                    // Found! Set homeFrame to 0 - NLR will use context-based return
                    // Actually, we can't use inline NLR because the home is in context chain.
                    // The safest approach: leave homeFrame as SIZE_MAX but ensure returnValue
                    // handles this case via context-based NLR (which we already implemented).
                    // But that only works when frameDepth_==0. Here frameDepth_>0.
                    //
                    // Alternative: when we detect home method is in context chain,
                    // materialize the current frames and switch to context-based execution.
                    // This ensures all future NLRs work correctly.

                    // Don't materialize here — defer to returnFromBlock() which will
                    // materialize on demand when NLR actually happens.
                    // returnFromBlock() handles frameDepth_>0 + homeFrame==SIZE_MAX
                    // by materializing and falling through to context-based NLR.
                    break;
                }
                ctx = memory_.fetchPointer(0, ctx);
                searchDepth++;
            }
        }

        savedFrames_[frameDepth_ - 1].homeFrameDepth = homeFrame;
    }

    method_ = methodToExecute;
    // For FullBlockClosure, homeMethod should be from the compiledBlock's slot 2 or outerContext
    if (slot1.isObject()) {
        // CompiledBlock slot 2 is the home method
        Oop homeFromBlock = memory_.fetchPointer(2, slot1);
        if (homeFromBlock.isObject()) {
            homeMethod_ = homeFromBlock;
        } else {
            // Fall back to getting from outer context
            homeMethod_ = memory_.fetchPointer(3, outerContext);
        }
    } else {
        homeMethod_ = memory_.fetchPointer(3, outerContext);
    }

    argCount_ = argCount;

    // For FullBlockClosure, the receiver is stored directly in the closure
    // at slot 3 (after outerContext, compiledBlock, numArgs).
    // FullBlockClosure layout:
    //   0: outerContext
    //   1: compiledBlock
    //   2: numArgs
    //   3: receiver  <-- added by FullBlockClosure subclass
    //   4+: copied values
    if (slot1.isObject()) {
        // FullBlockClosure: receiver is at slot 3
        receiver_ = memory_.fetchPointer(3, block);
    } else if (outerContext.isObject() && !outerContext.isNil()) {
        // Old-style BlockClosure: receiver from outer context
        receiver_ = memory_.fetchPointer(5, outerContext);
    } else {
        receiver_ = memory_.nil();
    }


    // Copy the copied values from the closure into the temp area
    // BlockClosure layout (old style):
    //   0: outerContext
    //   1: startPC (SmallInteger)
    //   2: numArgs
    //   3+: copied values
    // FullBlockClosure layout:
    //   0: outerContext
    //   1: compiledBlock (Object)
    //   2: numArgs
    //   3: receiver  <-- EXTRA SLOT in FullBlockClosure
    //   4+: copied values
    size_t blockSlots = memory_.slotCountOf(block);

    // Determine if this is FullBlockClosure (slot1 is object) or BlockClosure (slot1 is SmallInteger)
    int firstCopiedSlot = slot1.isObject() ? 4 : 3;  // FullBlockClosure has receiver at slot 3
    int numCopied = static_cast<int>(blockSlots) - firstCopiedSlot;
    if (numCopied < 0) numCopied = 0;

    for (int i = 0; i < numCopied; i++) {
        Oop copiedValue = memory_.fetchPointer(firstCopiedSlot + i, block);
        setTemporary(argCount + i, copiedValue);
    }

    instructionPointer_ = startAddress;

    // Set bytecode end based on method size
    ObjectHeader* methodHdr = methodToExecute.asObjectPtr();
    bytecodeEnd_ = methodHdr->bytes() + methodHdr->byteSize();
}

// ===== FRAME MANAGEMENT =====

bool Interpreter::pushFrame(Oop method, int argCount) {
    // Frame-depth milestones: log the selector at key depths to trace recursion patterns
    if (__builtin_expect(frameDepth_ >= 50, 0)) {
        static size_t lastMilestone = 0;
        static int milestoneCount = 0;
        // Log at 50, 100, 200, 500, 1000, 2000, 3000, 4000
        size_t fd = frameDepth_;
        size_t milestone = (fd < 100) ? 50 : (fd < 200) ? 100 : (fd < 500) ? 200 :
                           (fd < 1000) ? 500 : (fd < 2000) ? 1000 : (fd < 3000) ? 2000 :
                           (fd < 4000) ? 3000 : 4000;
        if (fd == milestone && milestone != lastMilestone && milestoneCount < 50) {
            milestoneCount++;
            lastMilestone = milestone;
            std::string sel = memory_.selectorOf(method);
            fprintf(stderr, "[DEPTH] fd=%zu pushing #%s (argCount=%d)\n",
                    fd, sel.c_str(), argCount);
            // At fd=50, print ALL frames; at higher milestones, print last 10
            size_t start = (fd == 50) ? 0 : (fd > 10 ? fd - 10 : 0);
            for (size_t f = start; f < fd; f++) {
                Oop savedM = savedFrames_[f].savedMethod;
                std::string savedSel = memory_.selectorOf(savedM);
                fprintf(stderr, "  [%zu] #%s (oop=0x%llx)\n", f, savedSel.c_str(),
                        (unsigned long long)savedM.rawBits());
            }
            fflush(stderr);
        }
    }

    // Graceful stack overflow: StackOverflowLimit < MaxFrameDepth, so this
    // catches both infinite recursion (soft) and hard overflow.
    if (__builtin_expect(frameDepth_ >= StackOverflowLimit, 0)) {
        static int overflowLog = 0;
        if (overflowLog++ < 3) {
            fprintf(stderr, "[OVERFLOW] fd=%zu pushing #%s (argCount=%d)\n",
                    frameDepth_, memory_.selectorOf(method).c_str(), argCount);
            // Dump last 50 frames with raw bits for debugging
            fprintf(stderr, "[OVERFLOW] Call stack (last 50):\n");
            size_t start = frameDepth_ > 50 ? frameDepth_ - 50 : 0;
            for (size_t f = start; f < frameDepth_; f++) {
                Oop savedM = savedFrames_[f].savedMethod;
                std::string savedSel = memory_.selectorOf(savedM);
                fprintf(stderr, "  [%zu] #%s (oop=0x%llx)\n", f, savedSel.c_str(),
                        (unsigned long long)savedM.rawBits());
            }
            fflush(stderr);
        }
        if (frameDepth_ >= MaxFrameDepth) {
            stopVM("Frame depth overflow in pushFrame()");
        }
        return false;
    }

    // Save any cached materialized context for the current frame into the saved frame.
    // This preserves context identity: if materializeFrameStack() already created a
    // context for this activation, later materializations will reuse it.
    Oop cachedCtx = currentFrameMaterializedCtx_;

    SavedFrame& frame = savedFrames_[frameDepth_++];
    frame.savedIP = instructionPointer_;
    frame.savedBytecodeEnd = bytecodeEnd_;
    frame.savedMethod = method_;
    frame.savedHomeMethod = homeMethod_;
    frame.savedReceiver = receiver_;
    frame.savedActiveContext = activeContext_;  // Save active context for proper return chain
    frame.savedFP = framePointer_;
    frame.savedArgCount = argCount_;
    frame.savedClosure = closure_;  // Save current frame's closure (nil for methods, block for block activations)
    frame.homeFrameDepth = SIZE_MAX;  // Default: not a block (will be set by activateBlock if needed)
    frame.materializedContext = cachedCtx;  // Preserve cached context from current frame
    currentFrameMaterializedCtx_ = memory_.nil();  // New frame has no cached context

    // When pushing a frame on top of a heap context (fd 0→1), sync the return
    // address to the heap context's PC slot. Rare: only on first frame push.
    if (__builtin_expect(frameDepth_ == 1 && activeContext_.isObject() && activeContext_.rawBits() > 0x10000 &&
        frame.savedMethod.isObject() && frame.savedMethod.rawBits() > 0x10000, 0)) {
        ObjectHeader* mObj = frame.savedMethod.asObjectPtr();
        uint8_t* mBytes = mObj->bytes();
        if (frame.savedIP >= mBytes && frame.savedIP < mBytes + mObj->byteSize()) {
            int64_t pc = static_cast<int64_t>(frame.savedIP - mBytes) + 1;
            memory_.storePointer(1, activeContext_, Oop::fromSmallInteger(pc));
        }
    }

    // Calculate number of temporaries for the new method
    // method is a known-valid CompiledMethod at this point
    Oop newMethodHeader = memory_.fetchPointerUnchecked(0, method);
    if (__builtin_expect(!newMethodHeader.isSmallInteger(), 0)) {
        FILE* crashLog = fopen("/tmp/pharosmalltalk-crash.log", "w");
        if (!crashLog) crashLog = stderr;
        ObjectHeader* mObj = method.asObjectPtr();
        // Print raw header and overflow word for deep analysis
        uint64_t rawHdr = mObj->rawHeader();
        uint64_t* rawPtr = reinterpret_cast<uint64_t*>(mObj);
        uint64_t overflowWord = *(rawPtr - 1);  // word before header
        uint8_t slotCountByte = (rawHdr >> 56) & 0xFF;
        fprintf(crashLog, "[FATAL] pushFrame: method header not SmallInteger!\n"
                "  method=0x%llx rawHdr=0x%llx overflowWord=0x%llx slotCountByte=%u\n"
                "  cls=%u fmt=%d slots=%zu isCompiledMethod=%d\n"
                "  header(slot0)=0x%llx tag=%d isObj=%d\n"
                "  step=%llu frameDepth=%zu\n",
                (unsigned long long)method.rawBits(),
                (unsigned long long)rawHdr, (unsigned long long)overflowWord,
                (unsigned)slotCountByte,
                mObj->classIndex(), (int)mObj->format(), mObj->slotCount(),
                mObj->isCompiledMethod(),
                (unsigned long long)newMethodHeader.rawBits(),
                (int)(newMethodHeader.rawBits() & 7),
                newMethodHeader.isObject(),
                (unsigned long long)g_stepNum, frameDepth_);
        // Dump first 10 slots
        fprintf(crashLog, "  slots:");
        for (size_t i = 0; i < std::min(mObj->slotCount(), (size_t)10); ++i) {
            Oop s = mObj->slotAt(i);
            fprintf(crashLog, " [%zu]=0x%llx(%s)", i, (unsigned long long)s.rawBits(),
                    s.isSmallInteger() ? "smi" : s.isObject() ? "obj" : "imm");
        }
        fprintf(crashLog, "\n");
        // Check if the method object has a valid class
        Oop cls = memory_.classOf(method);
        if (cls.isObject()) {
            Oop clsName = memory_.fetchPointer(6, cls);
            if (clsName.isObject() && clsName.rawBits() > 0x10000) {
                ObjectHeader* nh = clsName.asObjectPtr();
                if (nh->isBytesObject() && nh->byteSize() < 80) {
                    fprintf(crashLog, "  className=%.*s\n", (int)nh->byteSize(), (char*)nh->bytes());
                }
            }
        }
        // Dump selector from the last literal
        if (mObj->isCompiledMethod() && mObj->slotCount() > 1) {
            Oop hdr = mObj->slotAt(0);
            if (hdr.isSmallInteger()) {
                size_t nLits = hdr.asSmallInteger() & 0x7FFF;
                if (nLits >= 2 && nLits < mObj->slotCount()) {
                    Oop selLit = mObj->slotAt(nLits);  // penultimate literal = methodClass assoc
                    Oop selLit2 = mObj->slotAt(nLits - 1);  // second-to-last = selector (usually)
                    fprintf(crashLog, "  lastLiteral[%zu]=0x%llx penultimate[%zu]=0x%llx\n",
                            nLits, (unsigned long long)selLit.rawBits(),
                            nLits-1, (unsigned long long)selLit2.rawBits());
                }
            }
        }
        // Current selector being sent
        fprintf(crashLog, "  newMethod_=0x%llx method_=0x%llx receiver_=0x%llx\n",
                (unsigned long long)newMethod_.rawBits(),
                (unsigned long long)method_.rawBits(),
                (unsigned long long)receiver_.rawBits());
        // GC info
        fprintf(crashLog, "  gcCount=%zu lastGCStep=%llu\n",
                0/*gcCount*/, (unsigned long long)0/*lastGCStep*/);
        // Check if method address is in valid heap range
        fprintf(crashLog, "  methodAddr in heap: old=%d perm=%d\n",
                memory_.isOldObject(mObj), memory_.isPermObject(mObj));
        // Walk the stack to show recent callers
        fprintf(crashLog, "  recent frames:\n");
        for (size_t fi = frameDepth_; fi > 0 && fi > frameDepth_ - 8; --fi) {
            SavedFrame& sf = savedFrames_[fi - 1];
            std::string selStr = memory_.selectorOf(sf.savedMethod);
            fprintf(crashLog, "    frame[%zu] #%s method=0x%llx\n",
                    fi-1, selStr.c_str(), (unsigned long long)sf.savedMethod.rawBits());
        }
        fflush(crashLog);
        if (crashLog != stderr) fclose(crashLog);
        abort();
    }
    int64_t headerBits = newMethodHeader.asSmallInteger();
    int numTemps = (headerBits >> 18) & 0x3F;

    // New frame pointer is at current position minus args (receiver is first "arg")
    Oop* newFP = stackPointer_ - argCount - 1;  // -1 for receiver position
    framePointer_ = newFP;

    // Initialize temporaries to nil (numTemps includes args, which are already on stack)
    int numExtraTemps = numTemps - argCount;
    for (int i = 0; i < numExtraTemps; ++i) {
        push(memory_.nil());
    }

    // Note: primFailCode_ error objects are stored by the callPrimitive/storeTemp skip
    // code in activateMethod, NOT here. The error temp is identified by bytecode analysis
    // (0xF5 storeTemp after 0xF8 callPrimitive), matching the reference VM's behavior.

    return true;  // Successfully created frame
}

Oop Interpreter::getErrorObjectFromPrimFailCode() {
    // Convert primFailCode_ to the appropriate error object.
    // Reference: StackInterpreter>>getErrorObjectFromPrimFailCode
    if (primFailCode_ > 0) {
        Oop table = memory_.specialObject(SpecialObjectIndex::PrimErrTableIndex);
        if (table.isObject() && !table.isNil()) {
            size_t tableSize = memory_.slotCountOf(table);
            if (static_cast<size_t>(primFailCode_) <= tableSize) {
                Oop errObj = memory_.fetchPointer(primFailCode_ - 1, table);  // 1-based to 0-based

                // For PrimErrOSError (21), clone the template and set slot 1 to osErrorCode
                if (primFailCode_ == PrimErrOSError && errObj.isObject() && !errObj.isNil()) {
                    size_t numSlots = memory_.slotCountOf(errObj);
                    if (numSlots >= 2) {
                        push(errObj);  // GC safety during shallowCopy
                        Oop clone = memory_.shallowCopy(errObj);
                        errObj = pop();
                        if (!clone.isNil()) {
                            memory_.storePointer(1, clone, Oop::fromSmallInteger(static_cast<int64_t>(osErrorCode_)));
                            return clone;
                        }
                    }
                }

                return errObj;
            }
        }
    }
    // Fallback: return primFailCode as SmallInteger
    return Oop::fromSmallInteger(primFailCode_);
}



bool Interpreter::popFrame() {
    // Restore previous execution state
    if (frameDepth_ == 0) {
        // No C++ frames to pop. This is NOT a fatal error — the process may
        // have more contexts in the heap chain. Return false so the caller
        // can handle the context-based return (follow sender chain) or
        // terminate the process if there's no sender.
        return false;
    }

    --frameDepth_;
    SavedFrame& frame = savedFrames_[frameDepth_];

    // Reset stack to frame pointer (discards temps and locals)
    stackPointer_ = framePointer_;

    // Restore saved execution state
    instructionPointer_ = frame.savedIP;
    bytecodeEnd_ = frame.savedBytecodeEnd;
    method_ = frame.savedMethod;
    homeMethod_ = frame.savedHomeMethod;
    receiver_ = frame.savedReceiver;
    closure_ = frame.savedClosure;  // Restore caller's closure (nil for methods, block for block activations)
    activeContext_ = frame.savedActiveContext;  // Restore active context for proper return chain
    currentFrameMaterializedCtx_ = frame.materializedContext;  // Restore cached context for this frame
    framePointer_ = frame.savedFP;
    argCount_ = frame.savedArgCount;

    return true;
}

// J2J diagnostics — lightweight per-method tracking
#if PHARO_JIT_ENABLED
namespace {
struct J2JMethodStats { size_t enters = 0; size_t returns = 0; };
static std::unordered_map<uint64_t, J2JMethodStats> g_j2jMethodStats;
static size_t g_j2jLastDump = 0;
}

void Interpreter::trackJ2JEntry(jit::JITState* state) {
    uint64_t methodBits = state->cachedTarget.rawBits();
    g_j2jMethodStats[methodBits].enters++;

    size_t total = jitJ2JStencilCalls_;
    if (total - g_j2jLastDump >= 50000) {
        g_j2jLastDump = total;
        fprintf(stderr, "[J2J-DIAG] === at %zu calls ===\n", total);
        for (auto& [mb, ms] : g_j2jMethodStats) {
            if (ms.enters > 10) {
                std::string sel = memory_.selectorOf(Oop::fromRawBits(mb));
                fprintf(stderr, "[J2J-DIAG]   #%-30s E=%zu R=%zu (%.0f%%)\n",
                        sel.c_str(), ms.enters, ms.returns,
                        ms.enters ? 100.0 * ms.returns / ms.enters : 0.0);
            }
        }
    }
}

void Interpreter::trackJ2JReturn(jit::JITState* state) {
    uint64_t methodBits = method_.rawBits();
    g_j2jMethodStats[methodBits].returns++;
}
#endif

// ===== J2J FRAME MANAGEMENT =====
#if PHARO_JIT_ENABLED

// pushFrameForJIT is now inline in Interpreter.hpp for cross-TU inlining into j2j_call.
#endif // PHARO_JIT_ENABLED

#if PHARO_JIT_ENABLED
void Interpreter::popFrameForJIT(jit::JITState* state) {
    // Lightweight frame pop for J2J direct calls.
    if (frameDepth_ == 0) return;
    (void)state;

    --frameDepth_;
    SavedFrame& frame = savedFrames_[frameDepth_];

    // Restore interpreter state from saved frame
    stackPointer_ = framePointer_;  // Discard callee's locals
    instructionPointer_ = frame.savedIP;
    bytecodeEnd_ = frame.savedBytecodeEnd;
    method_ = frame.savedMethod;
    homeMethod_ = frame.savedHomeMethod;
    receiver_ = frame.savedReceiver;
    closure_ = frame.savedClosure;
    activeContext_ = frame.savedActiveContext;
    currentFrameMaterializedCtx_ = frame.materializedContext;
    framePointer_ = frame.savedFP;
    argCount_ = frame.savedArgCount;
}
#endif // PHARO_JIT_ENABLED

// ===== VARIABLE ACCESS =====

Oop Interpreter::literal(size_t index) const {
    // In Pharo 10+ with FullBlockClosure model, both CompiledMethods and CompiledBlocks
    // have their own literal frames. Each compiled object (method or block) contains
    // its own literals - blocks do NOT share literals with their home method.
    //
    // So we always use method_ (the currently executing CompiledMethod or CompiledBlock)
    // for literal access, NOT homeMethod_.
    Oop literalMethod = method_;

    // Safety check (cold path — method_ should always be valid during execution)
    if (__builtin_expect(literalMethod.isNil() || !literalMethod.isObject(), 0)) {
        return memory_.specialObject(SpecialObjectIndex::NilObject);
    }

    // method_ is a known-valid CompiledMethod/CompiledBlock
    Oop methodHeader = memory_.fetchPointerUnchecked(0, literalMethod);
    if (__builtin_expect(methodHeader.isSmallInteger(), 1)) {
        int64_t headerBits = methodHeader.asSmallInteger();
        size_t numLiterals = headerBits & 0x7FFF;  // bits 0-14

        if (__builtin_expect(index >= numLiterals, 0)) {
            return memory_.specialObject(SpecialObjectIndex::NilObject);
        }
    } else {
        return memory_.specialObject(SpecialObjectIndex::NilObject);
    }

    return memory_.fetchPointerUnchecked(index + 1, literalMethod);
}

Oop Interpreter::temporary(int index) const {
    // In Sista bytecodes, temp indices 0..argCount-1 are the arguments,
    // and indices argCount+ are local temps/copied values.
    // Frame layout: [receiver, arg0, arg1, ..., temp0, temp1, ...]
    // So all are accessed at framePointer_[1 + index]
    Oop result = *(framePointer_ + 1 + index);
    return result;
}

bool Interpreter::isExecutingBlock() const {
    // Check if we're executing a CompiledBlock (as opposed to a CompiledMethod).
    // CompiledBlock has an outer CompiledMethod at its penultimate literal.
    if (!method_.isObject() || method_.rawBits() <= 0x10000) return false;
    Oop header = memory_.fetchPointer(0, method_);
    if (!header.isSmallInteger()) return false;
    size_t numLits = header.asSmallInteger() & 0x7FFF;
    if (numLits < 2) return false;
    // Penultimate literal: for CompiledBlock, this is the outer CompiledMethod
    Oop penultLit = memory_.fetchPointer(numLits - 1, method_);
    if (!penultLit.isObject() || penultLit.rawBits() <= 0x10000) return false;
    ObjectHeader* plHdr = penultLit.asObjectPtr();
    return plHdr->isCompiledMethod();
}

Oop Interpreter::outerTemporary(int index) const {
    // Read a temp from the outer context (for remote temp access in blocks).
    // Context layout: slot 0=sender, 1=pc, 2=sp, 3=method, 4=closureOrNil, 5=receiver, 6+=temps
    if (activeContext_.isObject() && activeContext_.rawBits() > 0x10000) {
        return memory_.fetchPointer(6 + index, activeContext_);
    }
    // Fallback to local temps if no outer context
    return temporary(index);
}

void Interpreter::setOuterTemporary(int index, Oop value) {
    // Store a temp into the outer context (for remote temp store in blocks).
    if (activeContext_.isObject() && activeContext_.rawBits() > 0x10000) {
        memory_.storePointer(6 + index, activeContext_, value);
    } else {
        // Fallback to local temps
        setTemporary(index, value);
    }
}

void Interpreter::setTemporary(int index, Oop value) {
    *(framePointer_ + 1 + index) = value;

    // Write-through to context when materialized (frameDepth_==0).
    // After thisContext materializes, the context object is exposed to Smalltalk.
    // Interpreter temp stores must also update the context so that Smalltalk code
    // reading context temps (via tempNamed:) sees current values.
    if (frameDepth_ == 0 && activeContext_.isObject() && activeContext_.rawBits() > 0x10000) {
        ObjectHeader* ctxHdr = activeContext_.asObjectPtr();
        if (static_cast<size_t>(6 + index) < ctxHdr->slotCount()) {
            memory_.storePointer(6 + index, activeContext_, value);
        }
    }
}

Oop Interpreter::argument(int index) const {
    // Arguments are at frame pointer
    return *(framePointer_ + index);
}

Oop Interpreter::receiverInstVar(size_t index) const {
    if (!receiver_.isObject()) {
        return memory_.nil();  // Immediate receiver — no instance variables
    }
    ObjectHeader* hdr = receiver_.asObjectPtr();
    if (__builtin_expect(hdr->isBytesObject() || hdr->isCompiledMethod(), 0)) {
        return memory_.nil();
    }
    if (__builtin_expect(index >= hdr->slotCount(), 0)) {
        return memory_.nil();  // Out-of-bounds — method/receiver class mismatch
    }
    return memory_.fetchPointerUnchecked(index, receiver_);
}

void Interpreter::setReceiverInstVar(size_t index, Oop value) {

    // Check immutability - send attemptToAssign:withIndex: if receiver is immutable
    if (receiver_.isObject()) {
        ObjectHeader* hdr = receiver_.asObjectPtr();
        if (hdr->isImmutable()) {
            Oop selector = memory_.specialObject(SpecialObjectIndex::SelectorAttemptToAssign);
            push(receiver_);                                                // receiver of message
            push(value);                                                    // arg 1: value being assigned
            push(Oop::fromSmallInteger(static_cast<int64_t>(index + 1)));   // arg 2: 1-based index
            sendSelector(selector, 2);
            return;
        }
    }

    // Check if receiver is a byte object - can't store to byte objects
    if (receiver_.isObject()) {
        ObjectHeader* hdr = receiver_.asObjectPtr();
        if (__builtin_expect(hdr->isBytesObject() || hdr->isCompiledMethod(), 0)) {
            return;
        }
        // Bounds check: don't write past the end of the object
        if (__builtin_expect(index >= hdr->slotCount(), 0)) {
            return;
        }
    } else {
        // Immediate receiver (SmallInteger, Character) — can't have instance variables
        return;
    }

    // Temporary debug: trace ALL writes to ANY SnapshotOperation
    if (receiver_.isObject() && !receiver_.isNil()) {
        static int snapopCount = 0;
        std::string clsName = memory_.classNameOf(receiver_);
        if (clsName == "SnapshotOperation" && snapopCount < 50) {
            snapopCount++;
            Oop trueObj = memory_.specialObject(SpecialObjectIndex::TrueObject);
            Oop falseObj = memory_.specialObject(SpecialObjectIndex::FalseObject);
            const char* valDesc = "?";
            if (value.rawBits() == trueObj.rawBits()) valDesc = "true";
            else if (value.rawBits() == falseObj.rawBits()) valDesc = "false";
            else if (value.isNil()) valDesc = "nil";
            else valDesc = "obj";
            fprintf(stderr, "[SNAPOP-%d] 0x%llx slot[%zu] := %s in method=%s\n",
                    snapopCount, (unsigned long long)receiver_.rawBits(),
                    index, valDesc, memory_.selectorOf(method_).c_str());
        }
    }
    // Trace ReadStream instVar writes (position = index 1) — delayed activation
    {
        static int rsTraceCount = 0;
        static bool traceActive = false;
        // Activate after ~180M bytecodes (near the regex matching phase)
        if (!traceActive && g_stepNum > 100000000ULL) {
            traceActive = true;
            fprintf(stderr, "[RS-TRACE] Activated at step %llu\n", g_stepNum);
        }
        if (traceActive && rsTraceCount < 200 && index <= 2 && value.isSmallInteger()) {
            std::string rcls = memory_.classNameOf(receiver_);
            if (rcls.find("ReadStream") != std::string::npos) {
                rsTraceCount++;
                // Also get the collection string if it's small
                std::string collStr = "";
                if (index == 1) {  // position write
                    Oop coll = memory_.fetchPointerUnchecked(0, receiver_);
                    if (coll.isObject() && coll.rawBits() > 0x10000) {
                        ObjectHeader* ch = coll.asObjectPtr();
                        if (ch->isBytesObject() && ch->byteSize() <= 30) {
                            collStr = std::string((char*)ch->bytes(), ch->byteSize());
                        }
                    }
                }
                fprintf(stderr, "[RS-STORE] slot[%zu] := %lld (method=%s coll='%s')\n",
                        index, value.asSmallInteger(),
                        memory_.selectorOf(method_).c_str(),
                        collStr.c_str());
            }
        }
    }
    memory_.storePointerUnchecked(index, receiver_, value);
}

// ===== SPECIAL SENDS =====

void Interpreter::sendDoesNotUnderstand(Oop selector, int argCount) {
    const int MAX_DNU_DEPTH = 10;

    // Fast path: nil findNextHandlerContext → return nil
    // UndefinedObject doesn't implement this method in Pharo 13,
    // but the expected behavior is to return nil (terminate handler chain).
    // Without this, each DNU triggers a cascade of exception handling DNUs,
    // creating ~400-frame deep stacks and wasting huge amounts of CPU.
    {
        Oop dnuReceiver = stackValue(argCount);  // actual DNU receiver (on stack)
        if (argCount == 0 && dnuReceiver.isNil()) {
            if (selector.isObject() && selector.rawBits() > 0x10000) {
                ObjectHeader* selHdr = selector.asObjectPtr();
                if (selHdr->isBytesObject()) {
                    size_t selLen = selHdr->byteSize();
                    const char* bytes = (const char*)selHdr->bytes();
                    if (selLen == 25 && memcmp(bytes, "findNextHandlerContext", 22) == 0) {
                        popN(argCount + 1);
                        push(memory_.nil());
                        return;
                    }
                    // nil asSymbol → return the symbol 'nil'
                    // UndefinedObject doesn't implement #asSymbol in Pharo 13.
                    // This is consistent with nil asString → 'nil'. Without this,
                    // FFI struct field compilation (ExternalStructure>>recompileStructures)
                    // triggers a fatal DNU during startup: Class>>bindingOf: sends
                    // varName asSymbol where varName is nil due to FFI class variable
                    // resolution failing for test struct types (Char5, Byte10).
                    // CommandLineUIManager catches this and calls exitFailure.
                    if (selLen == 8 && memcmp(bytes, "asSymbol", 8) == 0) {
                        static int asSymbolLogCount = 0;
                        if (++asSymbolLogCount <= 3) {
                            fprintf(stderr, "[DNU] nil>>asSymbol → #nil (in %s fd=%zu)\n",
                                    memory_.selectorOf(method_).c_str(), frameDepth_);
                        }
                        // Look up the symbol 'nil' in the symbol table
                        Oop nilSymbol = memory_.lookupSymbol("nil");
                        if (!nilSymbol.isNil()) {
                            popN(argCount + 1);
                            push(nilSymbol);
                            return;
                        }
                        // If symbol creation failed, fall through to normal DNU
                    }
                }
            }
        }
    }

        dnuDepth_++;

    // If the selector IS doesNotUnderstand:, we're in a recursive DNU cascade.
    // The standard VM terminates the process in this case — there's no way to recover
    // because the receiver's class doesn't implement doesNotUnderstand: itself.
    {
        if (selectors_.doesNotUnderstand.rawBits() == selector.rawBits()) {
            dnuDepth_--;
            fprintf(stderr, "[DNU] CASCADE: receiver can't handle doesNotUnderstand:\n");
            // Log what selector triggered the original DNU
            if (frameDepth_ > 0) {
                SavedFrame& prev = savedFrames_[frameDepth_ - 1];
                fprintf(stderr, "[DNU]   caller=#%s fd=%zu\n",
                        memory_.selectorOf(prev.savedMethod).c_str(), frameDepth_);
            }
            Oop nextProcess = wakeHighestPriority();
            if (nextProcess.isNil() || !nextProcess.isObject()) {
                stopVM("Recursive doesNotUnderstand: and no other runnable process");
                return;
            }
            transferTo(nextProcess);
            return;
        }
    }

    // Log first 20 DNU messages to debug startup issues
    {
        static int dnuLogCount = 0;
        if (dnuLogCount++ < 20) {
            std::string selName = "(unknown)";
            try {
                if (selector.isObject() && selector.rawBits() > 0x10000) {
                    ObjectHeader* sH = selector.asObjectPtr();
                    if (sH->isBytesObject() && sH->byteSize() < 256) {
                        selName = std::string((const char*)sH->bytes(), sH->byteSize());
                    } else if (sH->isWordsObject() && (sH->byteSize() / 4) < 128) {
                        // WideSymbol: 32-bit chars, extract ASCII portion
                        uint32_t* words = (uint32_t*)sH->bytes();
                        size_t nw = sH->byteSize() / 4;
                        selName = "";
                        for (size_t i = 0; i < nw; i++) {
                            uint32_t ch = words[i];
                            selName += (ch < 128) ? (char)ch : '?';
                        }
                        selName = "#wide:" + selName;
                    } else {
                        char buf[64];
                        snprintf(buf, sizeof(buf), "(sel fmt=%d size=%zu cls=%u raw=0x%llx)",
                                 (int)sH->format(), sH->byteSize(), sH->classIndex(),
                                 (unsigned long long)selector.rawBits());
                        selName = buf;
                    }
                } else if (selector.isSmallInteger()) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "(sel=SmallInt %lld)", selector.asSmallInteger());
                    selName = buf;
                } else {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "(sel raw=0x%llx)", (unsigned long long)selector.rawBits());
                    selName = buf;
                }
            } catch (...) { selName = "(corrupt)"; }
            // Full stack dump for first 10 DNUs
            if (dnuLogCount <= 10) {
                try {
                    fprintf(stderr, "[DNU-STACK] Full call stack for #%s (DNU #%d):\n", selName.c_str(), dnuLogCount);
                    for (size_t f = 0; f <= frameDepth_ && f < 30; f++) {
                        SavedFrame& sf = savedFrames_[f];
                        std::string mSel = memory_.selectorOf(sf.savedMethod);
                        std::string rCls = memory_.classNameOf(sf.savedReceiver);
                        fprintf(stderr, "[DNU-STACK]   [%zu] %s>>%s\n", f, rCls.c_str(), mSel.c_str());
                    }
                    fprintf(stderr, "[DNU-STACK]   [current] #%s fd=%zu\n", memory_.selectorOf(method_).c_str(), frameDepth_);
                } catch (...) {
                    fprintf(stderr, "[DNU-STACK]   (stack dump failed)\n");
                }
            }
            Oop rcvr = stackValue(argCount);
            Oop currentProc = getActiveProcess();
            Oop procPri = memory_.fetchPointer(ProcessPriorityIndex, currentProc);
            int pri = procPri.isSmallInteger() ? (int)procPri.asSmallInteger() : -1;
            fprintf(stderr, "[DNU] #%d: #%s not understood by rcvr=0x%llx argCount=%d fd=%zu in #%s P%d\n",
                    dnuLogCount, selName.c_str(), (unsigned long long)rcvr.rawBits(), argCount, frameDepth_,
                    memory_.selectorOf(method_).c_str(), pri);
            if (rcvr.isObject() && rcvr.rawBits() > 0x10000) {
                ObjectHeader* rH = rcvr.asObjectPtr();
                fprintf(stderr, "[DNU]   rcvr cls=%u fmt=%d class=%s\n",
                        rH->classIndex(), (int)rH->format(), memory_.classNameOf(rcvr).c_str());
            } else if (rcvr.isSmallInteger()) {
                fprintf(stderr, "[DNU]   rcvr is SmallInteger %lld\n", rcvr.asSmallInteger());
            }
            // Dump args and frame info for debugging
            if (dnuLogCount <= 3) {
                for (int ai = 0; ai < argCount && ai < 5; ai++) {
                    Oop arg = stackValue(argCount - 1 - ai);
                    fprintf(stderr, "[DNU]   arg[%d] = 0x%llx (%s)\n", ai,
                            (unsigned long long)arg.rawBits(),
                            arg.isSmallInteger() ? "SmallInt" : arg.isNil() ? "nil" : "object");
                }
                fprintf(stderr, "[DNU]   FP=%p SP=%p base=%p\n", (void*)framePointer_, (void*)stackPointer_, (void*)stackBase_);
                fprintf(stderr, "[DNU]   receiver_=0x%llx method_=0x%llx\n",
                        (unsigned long long)receiver_.rawBits(), (unsigned long long)method_.rawBits());
                // Dump a few frame pointer values
                for (int fi = -2; fi <= 10; fi++) {
                    Oop v = framePointer_[fi];
                    fprintf(stderr, "[DNU]   FP[%d] = 0x%llx\n", fi, (unsigned long long)v.rawBits());
                }
            }
        }
    }

    // Depth limit — stop VM if stuck in DNU recursion
    if (dnuDepth_ > MAX_DNU_DEPTH) {
        dnuDepth_--;
        stopVM("DNU recursion depth exceeded — infinite doesNotUnderstand: loop");
        return;
    }

    // GC SAFETY: allocateSlots may trigger fullGC, invalidating all C++ locals
    // that hold Oops. Push selector onto the operand stack so it's a GC root.
    // Stack currently: ... receiver arg1 arg2 ... argN
    push(selector);
    // Stack now: ... receiver arg1 arg2 ... argN selector

    // Create Message object (may trigger GC)
    Oop messageClass = memory_.specialObject(SpecialObjectIndex::ClassMessage);
    uint32_t messageClassIdx = memory_.indexOfClass(messageClass);
    if (messageClassIdx == 0)
        messageClassIdx = memory_.registerClass(messageClass);
    Oop message = memory_.allocateSlots(messageClassIdx, 3, ObjectFormat::FixedSize);

    if (message.rawBits() == memory_.nil().rawBits()) {
        pop();  // selector
        for (int i = 0; i < argCount + 1; i++) pop();
        dnuDepth_--;
        stopVM("DNU: Failed to allocate Message object");
        return;
    }

    // Push message onto stack to protect it during second allocation
    push(message);
    // Stack now: ... receiver arg1 arg2 ... argN selector message

    // Create arguments array (may trigger GC)
    Oop arrayClass = memory_.specialObject(SpecialObjectIndex::ClassArray);
    uint32_t arrayClassIdx = memory_.indexOfClass(arrayClass);
    if (arrayClassIdx == 0)
        arrayClassIdx = memory_.registerClass(arrayClass);
    Oop args = memory_.allocateSlots(arrayClassIdx, argCount, ObjectFormat::Indexable);

    if (args.rawBits() == memory_.nil().rawBits() && argCount > 0) {
        pop();  // message
        pop();  // selector
        for (int i = 0; i < argCount + 1; i++) pop();
        dnuDepth_--;
        stopVM("DNU: Failed to allocate args Array");
        return;
    }

    // Pop message and selector from stack (now GC-updated)
    message = pop();
    selector = pop();
    // Stack restored: ... receiver arg1 arg2 ... argN

    memory_.storePointer(0, message, selector);

    for (int i = argCount - 1; i >= 0; --i) {
        memory_.storePointer(i, args, pop());
    }
    memory_.storePointer(1, message, args);

    Oop originalReceiver = pop();

    // Set lookupClass (slot 2) to the receiver's class.
    // MessageNotUnderstood >> messageText uses message lookupClass printString.
    memory_.storePointer(2, message, memory_.classOf(originalReceiver));

    // Send doesNotUnderstand: to the original receiver
    push(originalReceiver);
    push(message);

    sendSelector(selectors_.doesNotUnderstand, 1);

    dnuDepth_--;
}

void Interpreter::invokeObjectAsMethod(Oop nonMethod, Oop selector, int argCount) {
    // Reference VM behavior: when a non-CompiledMethod is found in a method dictionary
    // (e.g. ReflectiveMethod, metalink wrapper), send #run:with:in: to it.
    //
    // Stack on entry: ... receiver arg1 arg2 ... argN
    // We must:
    //   1. Pop argCount args into a fresh Array
    //   2. Pop receiver
    //   3. Push: nonMethod, selector, argsArray, receiver
    //   4. Send #run:with:in: to nonMethod (3 args)

    // GC SAFETY: Push nonMethod and selector onto stack before allocation,
    // since allocateSlots may trigger fullGC which invalidates C++ locals.
    // Stack on entry: ... receiver arg1 arg2 ... argN
    push(nonMethod);
    push(selector);
    // Stack: ... receiver arg1 arg2 ... argN nonMethod selector

    // Allocate Array for arguments (may trigger GC)
    Oop arrayClass = memory_.specialObject(SpecialObjectIndex::ClassArray);
    uint32_t arrayClassIndex = memory_.indexOfClass(arrayClass);
    Oop argsArray = memory_.allocateSlots(arrayClassIndex, argCount, ObjectFormat::Indexable);

    // Pop GC-safe nonMethod and selector
    selector = pop();
    nonMethod = pop();
    // Stack restored: ... receiver arg1 arg2 ... argN

    // Pop arguments into the array (top of stack = last arg)
    for (int i = argCount - 1; i >= 0; --i) {
        Oop arg = pop();
        memory_.storePointer(i, argsArray, arg);
    }

    // Pop receiver
    Oop receiver = pop();

    // Push: nonMethod (becomes the new receiver of #run:with:in:)
    push(nonMethod);
    // Push args: selector, argsArray, receiver
    push(selector);
    push(argsArray);
    push(receiver);

    // Send #run:with:in: (special object 49)
    Oop runWithInSelector = memory_.specialObject(SpecialObjectIndex::SelectorRunWithIn);
    sendSelector(runWithInSelector, 3);
}

void Interpreter::sendMustBeBoolean(Oop value) {
    // Send mustBeBoolean to the non-boolean value, let Smalltalk handle it.
    static int logCount = 0;
    logCount++;
    if (logCount <= 30 || logCount % 1000 == 0) {
        std::string valClass = memory_.nameOfClass(memory_.classOf(value));
        // Compute IP offset relative to bytecode start
        long ipOff = -1;
        size_t numLits = memory_.numLiteralsOf(method_);
        if (method_.isObject()) {
            uint8_t* bcStart = method_.asObjectPtr()->bytes() + (1 + numLits) * 8;
            ipOff = instructionPointer_ - bcStart;
        }
        fprintf(stderr, "[MUSTBOOL] #%d fd=%zu value_class=%s value=0x%llx in=#%s "
                "rcv_class=%s method=0x%llx numLits=%zu ipOff=%ld\n",
                logCount, frameDepth_, valClass.c_str(),
                (unsigned long long)value.rawBits(),
                memory_.selectorOf(method_).c_str(),
                memory_.classNameOf(receiver_).c_str(),
                (unsigned long long)method_.rawBits(), numLits, ipOff);
        // Dump literals of current method (once per unique method)
        static std::set<uint64_t> dumpedMethods;
        bool firstForMethod = method_.isObject() &&
            dumpedMethods.insert(method_.rawBits()).second;
        if (firstForMethod) {
            ObjectHeader* mh = method_.asObjectPtr();
            fprintf(stderr, "[MUSTBOOL] method header=0x%llx slotCount=%zu "
                    "method_class=%s method_format=%u method_classIdx=%u\n",
                    (unsigned long long)mh->slotAt(0).rawBits(), mh->slotCount(),
                    memory_.nameOfClass(memory_.classOf(method_)).c_str(),
                    (unsigned)mh->format(), mh->classIndex());
            // For CompiledMethod/CompiledBlock, slots 0..numLits are
            // header+literals (Oop), slots numLits+1..end are raw bytecode
            // bytes (NOT Oops — never call asObjectPtr on them).
            size_t totalSlots = mh->slotCount();
            size_t maxOopSlot = (mh->isCompiledMethod()) ? numLits : (totalSlots - 1);
            for (size_t i = 0; i < totalSlots && i < 16; i++) {
                Oop lit = mh->slotAt(i);
                fprintf(stderr, "[MUSTBOOL]   slot[%zu]=0x%llx", i,
                        (unsigned long long)lit.rawBits());
                if (i > maxOopSlot) {
                    fprintf(stderr, " (raw bytecode bytes)\n");
                    continue;
                }
                if (lit.isObject() && lit.rawBits() >= 0x10000) {
                    ObjectHeader* lh = lit.asObjectPtr();
                    fprintf(stderr, " class=%u format=%u",
                            lh->classIndex(), (unsigned)lh->format());
                    if (lh->isBytesObject() && lh->byteSize() < 80) {
                        fprintf(stderr, " bytes=\"%.*s\"",
                                (int)lh->byteSize(),
                                (const char*)lh->bytes());
                    }
                }
                fprintf(stderr, "\n");
            }
            // Dump bytecodes near ipOff
            if (numLits > 0) {
                uint8_t* bcStart = mh->bytes() + (1 + numLits) * 8;
                fprintf(stderr, "[MUSTBOOL] bytecodes near ipOff=%ld:", ipOff);
                long lo = std::max(0L, ipOff - 8);
                long hi = ipOff + 4;
                for (long i = lo; i <= hi; i++) {
                    fprintf(stderr, " %02x", bcStart[i]);
                }
                fprintf(stderr, "\n");
                // Dump full bytecodes
                size_t bcLen = mh->byteSize() - (1 + numLits) * 8;
                // Compute trailing pad bytes (trailer byte gives count - 1)
                if (bcLen > 0) {
                    uint8_t trailer = bcStart[bcLen - 1];
                    long pad = (long)(trailer & 0x7) + 1;
                    if (pad < (long)bcLen) bcLen -= pad;
                }
                fprintf(stderr, "[MUSTBOOL] full bytecodes (%zu):", bcLen);
                for (size_t i = 0; i < bcLen && i < 200; i++) {
                    if (i % 32 == 0) fprintf(stderr, "\n  %3zu:", i);
                    fprintf(stderr, " %02x", bcStart[i]);
                }
                fprintf(stderr, "\n");
            }
            // Dump stack neighborhood
            fprintf(stderr, "[MUSTBOOL] stack: sp=%p fp=%p depth=%zu\n",
                    (void*)stackPointer_, (void*)framePointer_, frameDepth_);
            for (int k = -3; k <= 1; k++) {
                Oop* slot = stackPointer_ + k;
                Oop v = *slot;
                std::string vc = (v.rawBits() == 0) ? "<zero>" : memory_.nameOfClass(memory_.classOf(v));
                fprintf(stderr, "[MUSTBOOL]   sp[%d]=0x%llx (%s)\n", k,
                        (unsigned long long)v.rawBits(), vc.c_str());
            }
        }
        // Print short stack for first 10 — show MOST RECENT frames (the
        // immediate caller is at savedFrames_[frameDepth_-1]).
        if (logCount <= 10) {
            size_t lo = frameDepth_ >= 10 ? frameDepth_ - 10 : 0;
            for (size_t f = lo; f < frameDepth_; f++) {
                SavedFrame& sf = savedFrames_[f];
                fprintf(stderr, "[MUSTBOOL]   recent[%zu] %s>>%s\n", f,
                        memory_.classNameOf(sf.savedReceiver).c_str(),
                        memory_.selectorOf(sf.savedMethod).c_str());
            }
        }
    }
    (void)value;
    sendSelector(selectors_.mustBeBoolean, 0);
}

// ===== HELPER METHODS =====

bool Interpreter::isTrue(Oop value) const {
    return value == memory_.trueObject();
}

bool Interpreter::isFalse(Oop value) const {
    return value == memory_.falseObject();
}

Oop Interpreter::superclassOf(Oop classOop) const {
    // Class layout: superclass is slot 0
    // classOop validated by lookupMethod loop guard
    return memory_.fetchPointerUnchecked(0, classOop);
}

Oop Interpreter::methodDictOf(Oop classOop) const {
    // Class layout: methodDict is slot 1
    return memory_.fetchPointerUnchecked(1, classOop);
}

Oop Interpreter::methodClassOf(Oop method) const {
    // Get the class that defines this CompiledMethod by reading the last literal.
    // In Pharo, the last literal is an Association/ClassBinding whose value (slot 1)
    // is the defining class. This matches the reference VM's methodClassOf:.
    if (!method.isObject()) return memory_.nil();

    // Get numLiterals from method header
    Oop methodHeader = memory_.fetchPointer(0, method);
    if (!methodHeader.isSmallInteger()) return memory_.nil();

    int64_t headerBits = methodHeader.asSmallInteger();
    size_t numLiterals = headerBits & 0x7FFF;  // bits 0-14

    if (numLiterals < 2) return memory_.nil();

    // The LAST literal (slot numLiterals) is the class binding.
    Oop lastLiteral = memory_.fetchPointer(numLiterals, method);

    if (!lastLiteral.isObject() || lastLiteral.isNil()) return memory_.nil();

    // Reference VM approach: if it's a pointer object with > 1 slots,
    // slot 1 is the value (defining class). No string comparisons needed.
    ObjectHeader* litHdr = lastLiteral.asObjectPtr();
    if (!litHdr->isBytesObject() && litHdr->slotCount() > 1) {
        return memory_.fetchPointer(1, lastLiteral);
    }

    return memory_.nil();
}

int Interpreter::primitiveIndexOf(Oop method) const {
    if (!method.isObject()) return 0;

    Oop header = memory_.fetchPointer(0, method);
    if (!header.isSmallInteger()) return 0;

    int64_t bits = header.asSmallInteger();

    // CompiledMethod header format (after SmallInteger decoding):
    //   bits 0-14: numLiterals (15 bits)
    //   bit 15: requiresCounters / needsLargeFrame
    //   bit 16: hasPrimitive
    //   bit 17: isOptimized / needsLargeFrame
    //   bits 18-23: numTemps (6 bits)
    //   bits 24-27: numArgs (4 bits)
    //   bits 28-29: accessModifier
    //   bit 30: alternate header format flag
    //
    // The primitive number is encoded in the bytecode stream.
    // When hasPrimitive is set, bytecodes start with a callPrimitive bytecode.

    // Check hasPrimitive flag (bit 16 after SmallInteger decoding)
    bool hasPrimitive = (bits >> 16) & 1;
    if (!hasPrimitive) return 0;

    ObjectHeader* methodObj = method.asObjectPtr();
    int numLiterals = bits & 0x7FFF;  // bits 0-14 are numLiterals
    uint8_t* bytecodes = methodObj->bytes() + (1 + numLiterals) * 8;

    // In Sista V1, primitive call is encoded as:
    // 248 iiiiiiii mssjjjjj (callPrimitive)
    // The primitive number = iiiiiiii | (jjjjj << 8), i.e. lowByte | ((highByte & 0x1F) << 8)
    if (bytecodes[0] == 248) {
        int primIndex = bytecodes[1] | ((bytecodes[2] & 0x1F) << 8);
        return primIndex;
    }

    // hasPrimitive is set but first bytecode isn't callPrimitive
    {
        static int noCallPrimCount = 0;
        noCallPrimCount++;
        if (noCallPrimCount <= 20) {
        }
    }
    return 0;
}

void Interpreter::duplicateTop() {
    push(stackTop());
}

void Interpreter::popStack() {
    pop();
}

void Interpreter::createBlock() {
    // Extended block creation bytecode
    uint8_t descriptor = fetchByte();
    int numArgs = descriptor & 0x0F;
    int numCopied = (descriptor >> 4) & 0x0F;
    uint16_t blockSize = fetchTwoBytes();

    // Create BlockClosure
    Oop blockClass = memory_.specialObject(SpecialObjectIndex::ClassBlockClosure);
    size_t slots = 3 + numCopied;  // outerContext, startPC, numArgs, copied...
    // BlockClosure has 3 fixed fields (outerContext, startPC, numArgs) plus
    // variable indexed fields (copied values). Must use IndexableWithFixed.
    Oop block = memory_.allocateSlots(
        memory_.indexOfClass(blockClass), slots, ObjectFormat::IndexableWithFixed);

    // Set fields
    // Ensure proper context identity by materializing if running inline
    Oop outerContextForBlock = activeContext_;
    if (frameDepth_ > 0) {
        // GC SAFETY: materializeFrameStack allocates contexts, which may trigger GC.
        // Protect block on the operand stack during allocation.
        push(block);
        outerContextForBlock = materializeFrameStack();
        block = pop();
        activeContext_ = outerContextForBlock;
        frameDepth_ = 0;  // Reset after materialization to prevent duplicate contexts
    }
    memory_.storePointer(0, block, outerContextForBlock);  // outerContext
    // GC SAFETY: method_ is a GC root, so method_.asObjectPtr() is always valid
    memory_.storePointer(1, block, Oop::fromSmallInteger(
        instructionPointer_ - method_.asObjectPtr()->bytes()));
    memory_.storePointer(2, block, Oop::fromSmallInteger(numArgs));

    // Copy values from stack
    for (int i = numCopied - 1; i >= 0; --i) {
        memory_.storePointer(3 + i, block, pop());
    }

    // Skip block bytecodes
    instructionPointer_ += blockSize;

    push(block);
}

void Interpreter::createFullBlock() {
    // Dead code: createFullBlockWithLiteral() handles all FullBlockClosure creation.
    // Kept as fallback but never called in practice.
    createBlock();
}

void Interpreter::createFullBlockWithLiteral(int litIndex, int numCopied, bool receiverOnStack, bool ignoreOuterContext) {
    // Sista V1 0xF9: Push FullBlockClosure
    // The closure's code is in a CompiledBlock literal at litIndex
    Oop compiledBlock = literal(litIndex);

    // Create FullBlockClosure
    // Get the class from special objects - index 59 is ClassFullBlockClosure
    Oop blockClass = memory_.specialObject(SpecialObjectIndex::ClassFullBlockClosure);

    // Fall back to BlockClosure if FullBlockClosure not found
    if (blockClass.isNil() || !blockClass.isObject()) {
        blockClass = memory_.specialObject(SpecialObjectIndex::ClassBlockClosure);
    }

    // Use the class index cached at initialization time
    uint32_t classIdx = fullBlockClosureClassIndex_;

    // FullBlockClosure layout:
    // slot 0: outerContext
    // slot 1: compiledBlock
    // slot 2: numArgs
    // slot 3: receiver
    // slot 4+: copied values
    size_t slots = 4 + numCopied;  // 4 fixed slots + copied values
    // FullBlockClosure has 4 fixed fields (outerContext, compiledBlock, numArgs, receiver)
    // plus variable indexed fields (copied values). Must use IndexableWithFixed so that
    // at:/at:put:/basicSize correctly skip the fixed fields when accessing copied values.
    Oop block = memory_.allocateSlots(classIdx, slots, ObjectFormat::IndexableWithFixed);

    // GC SAFETY: compiledBlock may be stale after allocation triggered GC compaction.
    // Re-read from method literals (method_ is a GC root, so literal() is always valid).
    compiledBlock = literal(litIndex);

    // Set outerContext field.
    // For blocks that need outerContext (ignoreOuterContext=false), we materialize
    // the frame stack to create proper Context objects.
    Oop outerContextForBlock;
    if (!ignoreOuterContext && frameDepth_ > 0) {
        // GC SAFETY: materializeFrameStack allocates contexts, which may trigger GC.
        // Protect block on the operand stack during allocation.
        push(block);
        outerContextForBlock = materializeFrameStack();
        block = pop();
        // Re-read compiledBlock again (MFS may have triggered another GC)
        compiledBlock = literal(litIndex);
    } else {
        outerContextForBlock = activeContext_;
    }
    memory_.storePointer(0, block, outerContextForBlock);  // outerContext
    memory_.storePointer(1, block, compiledBlock);   // compiledBlock (instead of startPC)

    // Get numArgs from the CompiledBlock's header (first slot)
    // In Pharo, CompiledCode header format has numArgs in bits 24-27:
    // numArgs = (header bitAnd: 16rF000000) >> 24
    int numArgs = 0;
    if (compiledBlock.isObject()) {
        Oop methodHeader = memory_.fetchPointer(0, compiledBlock);
        if (methodHeader.isSmallInteger()) {
            int64_t headerBits = methodHeader.asSmallInteger();
            numArgs = (headerBits >> 24) & 0x0F;  // numArgs in bits 24-27
        }
    }
    memory_.storePointer(2, block, Oop::fromSmallInteger(numArgs));

    // Store receiver at slot 3
    // If receiverOnStack, pop it from stack; otherwise use current receiver_
    Oop blockReceiver;
    if (receiverOnStack) {
        blockReceiver = pop();
    } else {
        blockReceiver = receiver_;
    }
    memory_.storePointer(3, block, blockReceiver);

    // Copy values from stack - these go into slot 4+ (after the 4 fixed slots)
    for (int i = numCopied - 1; i >= 0; --i) {
        Oop copiedValue = pop();
        memory_.storePointer(4 + i, block, copiedValue);
    }

    push(block);
}

void Interpreter::createBlockWithArgs(int numArgs, int numCopied, int blockSize) {
    // Sista V1 0xFA: Push Closure (inline block)
    // Create BlockClosure
    Oop blockClass = memory_.specialObject(SpecialObjectIndex::ClassBlockClosure);
    size_t slots = 3 + numCopied;  // outerContext, startPC, numArgs, copied...
    // BlockClosure has 3 fixed fields (outerContext, startPC, numArgs) plus
    // variable indexed fields (copied values). Must use IndexableWithFixed.
    Oop block = memory_.allocateSlots(
        memory_.indexOfClass(blockClass), slots, ObjectFormat::IndexableWithFixed);

    // Set fields
    // Ensure proper context identity by materializing if running inline
    Oop outerContextForBlock = activeContext_;
    if (frameDepth_ > 0) {
        // GC SAFETY: materializeFrameStack allocates contexts, which may trigger GC.
        // Protect block on the operand stack during allocation.
        push(block);
        outerContextForBlock = materializeFrameStack();
        block = pop();
        activeContext_ = outerContextForBlock;
        frameDepth_ = 0;  // Reset after materialization to prevent duplicate contexts
    }
    memory_.storePointer(0, block, outerContextForBlock);  // outerContext
    // GC SAFETY: method_ is a GC root, so method_.asObjectPtr() is always valid
    memory_.storePointer(1, block, Oop::fromSmallInteger(
        instructionPointer_ - method_.asObjectPtr()->bytes()));  // startPC
    memory_.storePointer(2, block, Oop::fromSmallInteger(numArgs));

    // Copy values from stack
    for (int i = numCopied - 1; i >= 0; --i) {
        memory_.storePointer(3 + i, block, pop());
    }

    // Skip block bytecodes
    instructionPointer_ += blockSize;

    push(block);
}

uint32_t Interpreter::lookupClassIndexByName(const char* name) {
    size_t nameLen = strlen(name);
    for (uint32_t i = 1; i < 10000; i++) {
        Oop cls = memory_.classAtIndex(i);
        if (cls.isNil() || !cls.isObject()) continue;
        // Class layout: slot 6 = name (a Symbol/String)
        Oop clsName = memory_.fetchPointer(6, cls);
        if (!clsName.isObject()) continue;
        ObjectHeader* nameHdr = clsName.asObjectPtr();
        if (!nameHdr->isBytesObject()) continue;
        size_t bytes = nameHdr->byteSize();
        if (bytes != nameLen) continue;
        if (memcmp(nameHdr->bytes(), name, nameLen) == 0) {
            return i;
        }
    }
    return 0;
}

void Interpreter::initializeClassIndexCache() {
    compiledMethodClassIndex_ = lookupClassIndexByName("CompiledMethod");
    compiledBlockClassIndex_ = lookupClassIndexByName("CompiledBlock");
    fullBlockClosureClassIndex_ = lookupClassIndexByName("FullBlockClosure");
}

void Interpreter::initializeSelectors() {

    // Get selectors from special objects array
    selectors_.doesNotUnderstand = memory_.specialObject(SpecialObjectIndex::SelectorDoesNotUnderstand);
    selectors_.mustBeBoolean = memory_.specialObject(SpecialObjectIndex::SelectorMustBeBoolean);
    selectors_.cannotReturn = memory_.specialObject(SpecialObjectIndex::SelectorCannotReturn);
    selectors_.aboutToReturn = memory_.specialObject(SpecialObjectIndex::SelectorAboutToReturn);


    // For arithmetic selectors, search SmallInteger's method dictionary
    Oop smallIntClass = memory_.specialObject(SpecialObjectIndex::ClassSmallInteger);
    if (!smallIntClass.isObject() || smallIntClass.isNil()) {
        // std::cerr << "[WARN] initializeSelectors: SmallInteger class not found"; // DEBUG
        return;
    }

    // Get the actual nil object for comparison
    Oop nilObj = memory_.specialObject(SpecialObjectIndex::NilObject);

    // Helper to find a selector Symbol in a class hierarchy by name.
    // Uses the same MethodDictionary layout as lookupInMethodDict:
    //   slot 0 = tally, slot 1 = values array, slots 2+ = keys (Symbols)
    // Scans keys (slots 2+) by string content, returns the interned Symbol Oop.
    auto findSelectorInClass = [this, nilObj](Oop startClass, const char* name) -> Oop {
        Oop currentClass = startClass;
        int depth = 0;

        auto isNilOrEmpty = [nilObj](Oop o) -> bool {
            return o.isNil() || o.rawBits() == nilObj.rawBits();
        };

        while (!isNilOrEmpty(currentClass) && currentClass.isObject() && depth < 30) {
            depth++;
            ObjectHeader* classHeader = currentClass.asObjectPtr();

            size_t classSlots = classHeader->slotCount();
            if (classSlots < 2) {
                break;
            }

            // Class layout: slot 0 = superclass, slot 1 = methodDict
            Oop methodDict = memory_.fetchPointer(1, currentClass);
            if (methodDict.isObject() && !isNilOrEmpty(methodDict)) {
                ObjectHeader* mdHeader = methodDict.asObjectPtr();
                size_t mdSlots = mdHeader->slotCount();

                // Keys are at slots 2..mdSlots-1 of the MethodDictionary itself
                if (mdSlots > 2) {
                    size_t keyCount = mdSlots - 2;
                    for (size_t i = 0; i < keyCount; i++) {
                        Oop key = memory_.fetchPointer(i + 2, methodDict);
                        if (isNilOrEmpty(key) || !key.isObject()) continue;
                        if (memory_.symbolEquals(key, name)) {
                            return key;
                        }
                    }
                }
            }

            // Move to superclass
            currentClass = memory_.fetchPointer(0, currentClass);
        }
        return Oop::nil();
    };

    // Find arithmetic selectors in SmallInteger class hierarchy (skip "/" which causes hang)
    selectors_.add = findSelectorInClass(smallIntClass, "+");
    selectors_.subtract = findSelectorInClass(smallIntClass, "-");
    selectors_.lessThan = findSelectorInClass(smallIntClass, "<");
    selectors_.greaterThan = findSelectorInClass(smallIntClass, ">");
    selectors_.lessEqual = findSelectorInClass(smallIntClass, "<=");
    selectors_.greaterEqual = findSelectorInClass(smallIntClass, ">=");
    selectors_.equal = findSelectorInClass(smallIntClass, "=");
    selectors_.notEqual = findSelectorInClass(smallIntClass, "~=");
    selectors_.multiply = findSelectorInClass(smallIntClass, "*");
    // Enable "/" - SmallInteger doesn't have / so look it up from special selectors array
    selectors_.divide = findSelectorInClass(smallIntClass, "/");
    if (selectors_.divide.isNil() || selectors_.divide.rawBits() == 0) {
        // SmallInteger doesn't have / - get it from special selectors array
        Oop specialSelectors = memory_.specialObject(SpecialObjectIndex::SpecialSelectorsArray);
        if (specialSelectors.isObject() && specialSelectors.rawBits() > 0x10000) {
            ObjectHeader* ssHdr = specialSelectors.asObjectPtr();
            // Index 9 is for / (which=9, slot=9*2=18)
            if (ssHdr->slotCount() > 18) {
                Oop divSel = ssHdr->slotAt(18);
                if (divSel.isObject() && divSel.rawBits() > 0x10000) {
                    selectors_.divide = divSel;
                }
            }
        }
    }

    // Skip these for now to avoid potential hangs
    selectors_.at = Oop::nil();
    selectors_.atPut = Oop::nil();
    selectors_.size = Oop::nil();
    selectors_.eq = Oop::nil();
    selectors_.class_ = Oop::nil();
    selectors_.value = Oop::nil();
    selectors_.value_ = Oop::nil();
    selectors_.valueValue = Oop::nil();

}

// ===== PROCESS SCHEDULING =====

void Interpreter::terminateCurrentProcess() {
    {
        Oop proc = getActiveProcess();
        Oop pri = memory_.fetchPointer(ProcessPriorityIndex, proc);
        // If high-priority process terminates, dump extended context
        if (pri.isSmallInteger() && pri.asSmallInteger() >= 60) {
            fprintf(stderr, "[TERM-P%lld] HIGH PRIORITY PROCESS TERMINATING!\n", pri.asSmallInteger());
            // Walk the context chain to find the original error
            if (activeContext_.isObject() && !activeContext_.isNil()) {
                Oop ctx = activeContext_;
                for (int i = 0; i < 30 && ctx.isObject() && !ctx.isNil(); i++) {
                    Oop method = memory_.fetchPointer(3, ctx);
                    std::string sel = method.isObject() ? memory_.selectorOf(method) : "?";
                    Oop receiver = memory_.fetchPointer(5, ctx);
                    std::string rcvrClass = memory_.classNameOf(receiver);
                    fprintf(stderr, "[TERM-P%lld]   ctx[%d]: %s>>%s\n",
                            pri.asSmallInteger(), i, rcvrClass.c_str(), sel.c_str());
                    ctx = memory_.fetchPointer(0, ctx);
                }
            }
        }
        fprintf(stderr, "[TERM] terminateCurrentProcess: proc=0x%llx pri=%lld fd=%zu method=#%s\n",
                (unsigned long long)proc.rawBits(),
                pri.isSmallInteger() ? pri.asSmallInteger() : -1,
                frameDepth_, memory_.selectorOf(method_).c_str());
        // Print call stack
        for (size_t i = frameDepth_; i > 0 && i > (frameDepth_ > 15 ? frameDepth_ - 15 : 0); i--) {
            fprintf(stderr, "[TERM]   fd=%zu #%s\n", i, memory_.selectorOf(savedFrames_[i].savedMethod).c_str());
        }
        fprintf(stderr, "[TERM]   fd=0 #%s (current)\n", memory_.selectorOf(method_).c_str());
        // Print C++ callsite
        void* callsite = __builtin_return_address(0);
        Dl_info info;
        if (dladdr(callsite, &info) && info.dli_sname) {
            fprintf(stderr, "[TERM]   C++ caller: %s+%ld\n", info.dli_sname,
                    (long)((char*)callsite - (char*)info.dli_saddr));
        }
    }
    // Clear any pending NLR state
    nlrTargetCtx_ = Oop::nil();
    nlrEnsureCtx_ = Oop::nil();
    nlrHomeMethod_ = Oop::nil();
    nlrValue_ = Oop::nil();

    // Also remove any saved NLR state for this process
    Oop currentProcess = getActiveProcess();
    for (int i = 0; i < savedNlrCount_; ++i) {
        if (savedNlrStates_[i].process.rawBits() == currentProcess.rawBits()) {
            savedNlrStates_[i] = savedNlrStates_[--savedNlrCount_];
            break;
        }
    }

    Oop nilObj = memory_.specialObject(SpecialObjectIndex::NilObject);
    Oop schedulerAssoc = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);

    if (!schedulerAssoc.isObject() || schedulerAssoc.rawBits() == nilObj.rawBits()) {
        return;
    }

    Oop scheduler = memory_.fetchPointer(1, schedulerAssoc);
    if (!scheduler.isObject()) {
        return;
    }

    // ProcessScheduler: slot 1 = activeProcess
    Oop activeProcess = memory_.fetchPointer(1, scheduler);
    if (!activeProcess.isObject() || activeProcess.rawBits() == nilObj.rawBits()) {
        return;
    }

    // Note: for the ACTIVE process, suspendedContext is always nil because
    // the context is in the interpreter's registers (not saved to the heap).
    // So we do NOT skip based on suspendedContext == nil here.

    // Get the list this process belongs to and remove it properly
    Oop myList = memory_.fetchPointer(ProcessMyListIndex, activeProcess);
    if (myList.isObject() && myList.rawBits() != nilObj.rawBits()) {
        // Remove from its linked list properly
        removeProcessFromList(activeProcess, myList);
    }

    // Process: slot 1 = suspendedContext - set it to nil to mark as terminated
    memory_.storePointer(ProcessSuspendedContextIndex, activeProcess, nilObj);

    // Clear nextLink and myList (should already be done by removeProcessFromList)
    memory_.storePointer(ProcessNextLinkIndex, activeProcess, nilObj);
    memory_.storePointer(ProcessMyListIndex, activeProcess, nilObj);
}

Oop Interpreter::getActiveProcess() {
    Oop schedulerAssoc = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);
    Oop scheduler = memory_.fetchPointer(1, schedulerAssoc);  // value of Association
    return memory_.fetchPointer(SchedulerActiveProcessIndex, scheduler);
}

void Interpreter::setActiveProcess(Oop process) {
    Oop schedulerAssoc = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);
    Oop scheduler = memory_.fetchPointer(1, schedulerAssoc);
    memory_.storePointer(SchedulerActiveProcessIndex, scheduler, process);
}

void Interpreter::addLastLinkToList(Oop process, Oop list) {
    // Validate inputs
    if (!process.isObject() || !list.isObject()) {
        return;
    }

    ObjectHeader* procHdr = process.asObjectPtr();

    // Verify process has enough slots for Process layout
    if (procHdr->slotCount() < 4) {
        return;
    }

    Oop nilObj = memory_.nil();

    // Set process.nextLink = nil (it's the last one)
    memory_.storePointer(ProcessNextLinkIndex, process, nilObj);

    // Set process.myList = list
    memory_.storePointer(ProcessMyListIndex, process, list);

    // Check if list is empty
    Oop firstLink = memory_.fetchPointer(LinkedListFirstLinkIndex, list);

    if (firstLink.isNil() || firstLink.rawBits() == nilObj.rawBits()) {
        // Empty list - process becomes both first and last
        memory_.storePointer(LinkedListFirstLinkIndex, list, process);
    } else {
        // Non-empty list - append to last element
        Oop lastLink = memory_.fetchPointer(LinkedListLastLinkIndex, list);
        memory_.storePointer(ProcessNextLinkIndex, lastLink, process);
    }

    memory_.storePointer(LinkedListLastLinkIndex, list, process);
}

Oop Interpreter::removeFirstLinkOfList(Oop list) {
    Oop nilObj = memory_.nil();

    Oop first = memory_.fetchPointer(LinkedListFirstLinkIndex, list);
    Oop last = memory_.fetchPointer(LinkedListLastLinkIndex, list);

    if (first.rawBits() == last.rawBits()) {
        // Only one element - list becomes empty
        memory_.storePointer(LinkedListFirstLinkIndex, list, nilObj);
        memory_.storePointer(LinkedListLastLinkIndex, list, nilObj);
    } else {
        // Multiple elements - advance firstLink to next
        Oop next = memory_.fetchPointer(ProcessNextLinkIndex, first);
        memory_.storePointer(LinkedListFirstLinkIndex, list, next);
    }

    // Clear removed process's links
    memory_.storePointer(ProcessNextLinkIndex, first, nilObj);
    memory_.storePointer(ProcessMyListIndex, first, nilObj);

    return first;
}

bool Interpreter::removeProcessFromList(Oop process, Oop list) {
    Oop nilObj = memory_.nil();
    Oop first = memory_.fetchPointer(LinkedListFirstLinkIndex, list);

    if (first.rawBits() == process.rawBits()) {
        // Process is first in list
        removeFirstLinkOfList(list);
        return true;
    }

    // Search for process in list
    Oop prev = first;
    Oop current = memory_.fetchPointer(ProcessNextLinkIndex, prev);

    while (!current.isNil() && current.rawBits() != nilObj.rawBits()) {
        if (current.rawBits() == process.rawBits()) {
            // Found it - unlink
            Oop next = memory_.fetchPointer(ProcessNextLinkIndex, current);
            memory_.storePointer(ProcessNextLinkIndex, prev, next);

            // Update lastLink if needed
            Oop lastLink = memory_.fetchPointer(LinkedListLastLinkIndex, list);
            if (lastLink.rawBits() == process.rawBits()) {
                memory_.storePointer(LinkedListLastLinkIndex, list, prev);
            }

            // Clear process's links
            memory_.storePointer(ProcessNextLinkIndex, process, nilObj);
            memory_.storePointer(ProcessMyListIndex, process, nilObj);
            return true;
        }
        prev = current;
        current = memory_.fetchPointer(ProcessNextLinkIndex, current);
    }
    return false;
}

Oop Interpreter::wakeHighestPriority() {

    Oop nilObj = memory_.nil();
    Oop schedulerAssoc = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);
    Oop scheduler = memory_.fetchPointer(1, schedulerAssoc);
    Oop schedLists = memory_.fetchPointer(SchedulerProcessListsIndex, scheduler);

    ObjectHeader* listsHeader = schedLists.asObjectPtr();
    size_t numPriorities = listsHeader->slotCount();


    // Search from highest to lowest priority
    for (int p = static_cast<int>(numPriorities) - 1; p >= 0; p--) {
        Oop processList = memory_.fetchPointer(p, schedLists);
        Oop first = memory_.fetchPointer(LinkedListFirstLinkIndex, processList);

        if (!first.isNil() && first.rawBits() != nilObj.rawBits()) {
            // Found a runnable process - remove and return it
            Oop result = removeFirstLinkOfList(processList);
            return result;
        }
    }

    // No runnable process found - this should not happen in a working system
    return nilObj;
}

Oop Interpreter::wakeLowerPriorityProcess(int currentPriority) {
    // Similar to wakeHighestPriority but only considers processes at LOWER priorities
    // This is used for force-yield to give lower priority processes CPU time

    Oop nilObj = memory_.nil();
    Oop schedulerAssoc = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);
    Oop scheduler = memory_.fetchPointer(1, schedulerAssoc);
    Oop schedLists = memory_.fetchPointer(SchedulerProcessListsIndex, scheduler);

    ObjectHeader* listsHeader = schedLists.asObjectPtr();
    size_t numPriorities = listsHeader->slotCount();

    // Current priority is 1-based, array index is 0-based
    int maxPriorityIndex = currentPriority - 2;  // One below current priority

    // Every 5th call, specifically try lowIOPriority (10) first to prevent starvation
    // lowIOPriority is where the event loop runs
    wakeLowerCount_++;
    if (wakeLowerCount_ % 5 == 0 && maxPriorityIndex >= 9) {
        int lowIOIndex = 9;  // Priority 10 = index 9
        Oop processList = memory_.fetchPointer(lowIOIndex, schedLists);
        Oop first = memory_.fetchPointer(LinkedListFirstLinkIndex, processList);
        if (!first.isNil() && first.rawBits() != nilObj.rawBits()) {
            Oop result = removeFirstLinkOfList(processList);
            return result;
        }
    }

    // Search from highest to lowest priority, but only below current priority
    for (int p = maxPriorityIndex; p >= 0; p--) {
        Oop processList = memory_.fetchPointer(p, schedLists);
        Oop first = memory_.fetchPointer(LinkedListFirstLinkIndex, processList);

        if (!first.isNil() && first.rawBits() != nilObj.rawBits()) {
            // Found a runnable process at lower priority - remove and return it
            Oop result = removeFirstLinkOfList(processList);
            return result;
        }
    }

    // No lower priority process found
    return nilObj;
}

int Interpreter::safeProcessPriority(Oop process) {
    Oop priorityOop = memory_.fetchPointer(ProcessPriorityIndex, process);
    if (!priorityOop.isSmallInteger()) {
        fprintf(stderr, "[CORRUPT-PRI] non-SmallInt priority: bits=0x%llx process=0x%llx\n",
                (unsigned long long)priorityOop.rawBits(), (unsigned long long)process.rawBits());
        return -1;
    }
    int pri = static_cast<int>(priorityOop.asSmallInteger());
    if (pri < 1 || pri > 80) {
        fprintf(stderr, "[CORRUPT-PRI] priority %d out of range process=0x%llx\n",
                pri, (unsigned long long)process.rawBits());
        return -1;
    }
    return pri;
}

void Interpreter::putToSleep(Oop process) {
    Oop schedulerAssoc = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);
    Oop scheduler = memory_.fetchPointer(1, schedulerAssoc);
    Oop schedLists = memory_.fetchPointer(SchedulerProcessListsIndex, scheduler);

    // Get and validate process priority
    int priority = safeProcessPriority(process);
    if (priority < 0) return;  // Corrupted process - cannot schedule

    // Get the appropriate priority list (0-indexed in array)
    Oop processList = memory_.fetchPointer(priority - 1, schedLists);


    addLastLinkToList(process, processList);
}

// Materialize the inline frame stack into context objects
// Returns the topmost context (current execution point)
Oop Interpreter::materializeFrameStack() {
    if (frameDepth_ == 0) {
        // No inline frames — sync the interpreter's current state with activeContext_.
        // The context's stored PC and stackp may be stale if bytecodes have
        // executed since the context was restored via executeFromContext.
        //
        // IMPORTANT: After thisContext materializes (setting frameDepth_=0), the context
        // object is exposed to Smalltalk. Code like tempNamed:put: can modify context
        // temp slots directly. We must NOT blindly overwrite context temps from the
        // C++ stack, as that would destroy Smalltalk-side modifications.
        //
        // Strategy:
        // - PC: sync C++ → context (interpreter has the current position)
        // - Temps: sync context → C++ (preserves Smalltalk modifications;
        //   interpreter modifications are already in context via write-through)
        // - Expression stack: sync C++ → context (interpreter manages the stack)
        // - stackp: sync C++ → context
        if (activeContext_.isObject() && activeContext_.rawBits() > 0x10000 &&
            method_.isObject() && method_.rawBits() > 0x10000) {
            ObjectHeader* methodObj = method_.asObjectPtr();
            uint8_t* methodBytes = methodObj->bytes();

            // Save current IP as 1-based byte offset into method bytes
            int64_t pc = (instructionPointer_ - methodBytes) + 1;
            memory_.storePointer(1, activeContext_, Oop::fromSmallInteger(pc));

            // Get numTemps from method header to distinguish temps from expression stack
            Oop methodHeader = memory_.fetchPointer(0, method_);
            int numTemps = 0;
            if (methodHeader.isSmallInteger()) {
                numTemps = (methodHeader.asSmallInteger() >> 18) & 0x3F;
            }

            // Total items on C++ stack above receiver
            int numItems = static_cast<int>(stackPointer_ - framePointer_) - 1;
            if (numItems < 0) numItems = 0;

            static const int ContextFixedFields = 6;
            ObjectHeader* ctxHdr = activeContext_.asObjectPtr();
            size_t ctxSlots = ctxHdr->slotCount();

            // Clamp to actual context capacity instead of arbitrary constant
            int maxItems = static_cast<int>(ctxSlots) - ContextFixedFields;
            if (maxItems < 0) maxItems = 0;
            if (numItems > maxItems) {
                static int stackpWarnCount = 0;
                if (stackpWarnCount < 5)  {
                    stackpWarnCount++;
                    fprintf(stderr, "[VM] Warning: stackp %d exceeds context capacity %d, clamping (sp=%p fp=%p)\n",
                            numItems, maxItems, (void*)stackPointer_, (void*)framePointer_);
                }
                numItems = maxItems;
            }
            memory_.storePointer(2, activeContext_, Oop::fromSmallInteger(numItems));

            // Sync ALL items (temps + expression stack): C++ → context.
            // The C++ stack is the canonical source: bytecodes modify temps
            // directly on the C++ stack without updating the context object.
            // Previously this synced temps context→C++ "to preserve Smalltalk
            // modifications like tempNamed:put:", but that was WRONG: it
            // overwrote the C++ stack's latest values with the context's stale
            // values from the last executeFromContext, destroying all temp
            // modifications made by bytecodes since then. This caused process
            // switching corruption (tests passing sequentially but failing
            // when forked with Processor yield).
            for (int i = 0; i < numItems && (ContextFixedFields + i) < static_cast<int>(ctxSlots); i++) {
                Oop item = *(framePointer_ + 1 + i);
                memory_.storePointer(ContextFixedFields + i, activeContext_, item);
            }
        }
        return activeContext_;
    }


    // Build contexts from bottom to top (oldest to newest)
    // CRITICAL: frame[0] represents the same activation as activeContext_ when
    // frame[0].savedActiveContext == activeContext_. In that case, we must UPDATE
    // activeContext_ with frame[0]'s data instead of creating a duplicate context.
    // Without this, the sender chain has two contexts for the same method activation,
    // causing loops to execute extra iterations after exception handling returns.
    Oop sender = activeContext_;
    size_t startFrame = 0;

    // J2J FIX: When activeContext_ is nil (set by pushFrameForJIT), use
    // savedFrames_[0].savedActiveContext as the sender for frame[0]. This
    // reconnects the context chain so exception handlers in ancestor contexts
    // can be found during exception propagation.
    if (sender.isNil() && frameDepth_ > 0) {
        Oop savedCtx = savedFrames_[0].savedActiveContext;
        if (savedCtx.isObject() && savedCtx.rawBits() > 0x10000) {
            sender = savedCtx;
        }
    }

    if (frameDepth_ > 0 && savedFrames_[0].savedActiveContext.rawBits() == activeContext_.rawBits() &&
        activeContext_.isObject() && activeContext_.rawBits() > 0x10000) {
        // frame[0] IS activeContext_'s inline continuation. Update the heap context
        // with frame[0]'s saved state instead of creating a new context.
        // This is the first materialization: activeContext_ matches what was saved when frame 0 was pushed.
        const auto& frame0 = savedFrames_[0];
        ObjectHeader* acHdr = activeContext_.asObjectPtr();
        if (acHdr->slotCount() >= 6 && acHdr->format() == ObjectFormat::IndexableWithFixed &&
            frame0.savedMethod.isObject()) {
            // DO NOT update PC here. The heap context's PC was already synced
            // by pushFrame (when going fd 0→1). If Smalltalk code modified the PC
            // between pushFrame and this materialization (e.g. Context>>privRefresh
            // setting pc := startpc for restart), we must NOT overwrite it with the
            // stale savedIP from the inline frame.

            // Update method, closure, receiver
            memory_.storePointer(3, activeContext_, frame0.savedMethod);
            memory_.storePointer(4, activeContext_, frame0.savedClosure);
            memory_.storePointer(5, activeContext_, frame0.savedReceiver);

            // Update temps from inline stack
            Oop methodHeader = memory_.fetchPointer(0, frame0.savedMethod);
            int numTemps = 0;
            if (methodHeader.isSmallInteger()) {
                numTemps = (methodHeader.asSmallInteger() >> 18) & 0x3F;
            }

            int savedCount = 0;
            static const int CtxFixed = 6;

            if (frame0.savedFP != nullptr) {
                // Sync temps: C++ stack → context (C++ stack is canonical).
                // Same fix as the frameDepth_==0 path: bytecodes modify temps
                // on the C++ stack, not on the context object.
                size_t ctxSlots = acHdr->slotCount();
                for (int t = 0; t < numTemps && t < 32; t++) {
                    if (static_cast<size_t>(CtxFixed + t) < ctxSlots) {
                        memory_.storePointer(CtxFixed + t, activeContext_, *(frame0.savedFP + 1 + t));
                    }
                    savedCount++;
                }
                // Save expression stack items
                Oop* exprStart = frame0.savedFP + 1 + numTemps;
                Oop* exprEnd;
                if (1 < frameDepth_) {
                    exprEnd = savedFrames_[1].savedFP;
                } else {
                    exprEnd = framePointer_;
                }
                if (exprEnd > exprStart && (exprEnd - exprStart) < 100) {
                    int nextArgCount;
                    if (1 < frameDepth_) {
                        Oop nextMH = memory_.fetchPointer(0, savedFrames_[1].savedMethod);
                        nextArgCount = nextMH.isSmallInteger() ? ((nextMH.asSmallInteger() >> 24) & 0xF) : 0;
                    } else {
                        nextArgCount = argCount_;
                    }
                    Oop* exprEndPtr = exprEnd;  // expression ends before callee receiver
                    ptrdiff_t exprCount = exprEndPtr - exprStart;
                    for (ptrdiff_t e = 0; e < exprCount && e < 100; e++) {
                        memory_.storePointer(CtxFixed + numTemps + e, activeContext_, *(exprStart + e));
                        savedCount++;
                    }
                }
            }
            memory_.storePointer(2, activeContext_, Oop::fromSmallInteger(savedCount));

            // Use activeContext_ as the context for frame[0], skip creating a new one
            sender = activeContext_;
            startFrame = 1;
            // CRITICAL: Store activeContext_ as the materialized context for frame[0].
            // Without this, the GC safety code in the current-frame allocation below
            // re-derives sender from savedFrames_[0].materializedContext, which is nil
            // (no separate context was created for frame[0]). This caused
            // thisContext sender == nil inside Context>>jump, crashing startup.
            savedFrames_[0].materializedContext = activeContext_;
        }
    }

    for (size_t i = startFrame; i < frameDepth_; i++) {
        auto& frame = savedFrames_[i];  // non-const: may update materializedContext


        // Get method info
        if (!frame.savedMethod.isObject()) {
            continue;
        }
        ObjectHeader* methodHdr = frame.savedMethod.asObjectPtr();
        Oop methodHeader = memory_.fetchPointer(0, frame.savedMethod);
        if (!methodHeader.isSmallInteger()) {
            continue;
        }
        int64_t headerValue = methodHeader.asSmallInteger();
        int numLiterals = headerValue & 0x7FFF;
        int numTemps = (headerValue >> 18) & 0x3F;  // Fixed: was using wrong bit offset
        int numArgs = (headerValue >> 24) & 0xF;

        // GC SAFETY: Compute IP offset BEFORE any allocation that could trigger GC.
        // frame.savedIP is a raw uint8_t* into the method's byte array. GC compaction
        // moves methods but does NOT update raw pointers (only Oop fields via forEachRoot).
        // After GC, savedIP is stale. Capture the offset now while both savedIP and
        // methodHdr point to the same (pre-GC) address space.
        int ipOffset = 0;
        {
            uint8_t* mBytes = methodHdr->bytes();
            if (frame.savedIP >= mBytes && frame.savedIP < mBytes + methodHdr->byteSize()) {
                ipOffset = static_cast<int>(frame.savedIP - mBytes);
            }
        }

        // Reuse previously materialized context for this frame if available.
        // This ensures context identity: block closures created in a frame get
        // the same context object as thisContext returns for the same activation.
        Oop context = frame.materializedContext;
        if (context.isObject() && !context.isNil() && context.rawBits() > 0x10000) {
            // Reuse existing context — just update sender and state
            memory_.storePointer(0, context, sender);  // update sender
        } else {
            // Calculate context size (6 fixed + temps + some stack)
            size_t contextSize = 6 + numTemps + 32;

            // Get Context class and its index in the class table
            Oop contextClass = memory_.specialObject(SpecialObjectIndex::ClassMethodContext);
            // Use indexOfClass to get the class table index, NOT the object's own classIndex (which is the metaclass)
            uint32_t classIndex = contextClass.isObject() ? memory_.indexOfClass(contextClass) : 0;
            if (classIndex == 0) {
                classIndex = 36;  // Fallback to typical Context class index
            }

            // Allocate context - use IndexableWithFixed (format 3) for contexts
            // Contexts have fixed fields (sender, pc, stackp, method, closure, receiver)
            // plus indexed temps/stack area
            context = memory_.allocateSlots(classIndex, contextSize, ObjectFormat::IndexableWithFixed);
            if (context.isNil()) {
                return activeContext_;  // Fall back to old behavior
            }
            // Cache this context for future materializations of the same frame
            frame.materializedContext = context;

            // GC SAFETY: allocateSlots may trigger fullGC, which compacts the heap
            // and moves objects. All C++ locals holding Oops or raw pointers into
            // heap objects are now stale. Re-derive them from GC roots.
            // - sender: re-read from the previous frame's materializedContext (a root)
            //   or from activeContext_ (a root) if this is the first frame
            if (i == startFrame) {
                sender = activeContext_;  // activeContext_ is a GC root
            } else {
                sender = savedFrames_[i - 1].materializedContext;  // also a GC root
            }
            // - methodHdr: re-derive from frame.savedMethod (a GC root)
            methodHdr = frame.savedMethod.asObjectPtr();
        }

        // Calculate PC using the pre-computed IP offset (GC safe).
        // ipOffset was computed before any allocation that could trigger GC.
        uint8_t* methodBytes = methodHdr->bytes();
        int pc = ipOffset + 1;  // 1-based PC from 0-based offset

        // Initialize context
        memory_.storePointer(0, context, sender);                           // sender
        memory_.storePointer(1, context, Oop::fromSmallInteger(pc));        // pc
        // stackp is set below after we know how many items we saved
        memory_.storePointer(3, context, frame.savedMethod);                // method
        memory_.storePointer(4, context, frame.savedClosure);                // closureOrNil
        memory_.storePointer(5, context, frame.savedReceiver);              // receiver

        // Copy temps AND expression stack items from the inline stack.
        // The saved FP points to the receiver; temps start at savedFP + 1.
        // Expression stack items sit above the temps, ending just before the
        // next frame's receiver (or the current frame's FP for the last saved frame).
        int savedCount = 0;
        if (frame.savedFP != nullptr) {
            // Save temps
            for (int t = 0; t < numTemps && t < 32; t++) {
                Oop temp = *(frame.savedFP + 1 + t);
                memory_.storePointer(6 + t, context, temp);
                savedCount++;
            }

            // Save expression stack items above the temps.
            Oop* exprStart = frame.savedFP + 1 + numTemps;
            Oop* nextFrameStart;
            int nextArgCount;
            if (i + 1 < frameDepth_) {
                nextFrameStart = savedFrames_[i + 1].savedFP;
                Oop nextMethodHdr = memory_.fetchPointer(0, savedFrames_[i + 1].savedMethod);
                nextArgCount = nextMethodHdr.isSmallInteger()
                    ? static_cast<int>((nextMethodHdr.asSmallInteger() >> 24) & 0xF) : 0;
            } else {
                nextFrameStart = framePointer_;
                nextArgCount = argCount_;
            }
            Oop* exprEndPtr = nextFrameStart;  // expression ends before callee receiver
            if (exprEndPtr > exprStart && (exprEndPtr - exprStart) < 100) {
                ptrdiff_t exprCount = exprEndPtr - exprStart;
                for (ptrdiff_t e = 0; e < exprCount; e++) {
                    memory_.storePointer(6 + numTemps + e, context, *(exprStart + e));
                    savedCount++;
                }
            }
        } else {
            // Initialize temps to nil
            for (int t = 0; t < numTemps; t++) {
                memory_.storePointer(6 + t, context, memory_.nil());
                savedCount++;
            }
        }

        // Set stackp to actual number of items saved
        memory_.storePointer(2, context, Oop::fromSmallInteger(savedCount)); // stackp

        // This context becomes the sender for the next frame
        sender = context;
    }

    // Also create a context for the CURRENT frame (the one we're executing)
    // This uses the current method_, receiver_, instructionPointer_, etc.
    if (method_.isObject()) {
        ObjectHeader* methodHdr = method_.asObjectPtr();
        Oop methodHeader = memory_.fetchPointer(0, method_);
        if (methodHeader.isSmallInteger()) {
            int64_t headerValue = methodHeader.asSmallInteger();
            int numTemps = (headerValue >> 18) & 0x3F;  // Fixed: was using wrong bit offset

            // GC SAFETY: Compute IP offset BEFORE any allocation (same pattern as saved-frame loop).
            int ipOffset = 0;
            {
                uint8_t* mBytes = methodHdr->bytes();
                if (instructionPointer_ >= mBytes && instructionPointer_ < mBytes + methodHdr->byteSize()) {
                    ipOffset = static_cast<int>(instructionPointer_ - mBytes);
                }
            }

            // Reuse previously materialized context for the current frame if available.
            // This ensures context identity across multiple materialize calls.
            Oop context = currentFrameMaterializedCtx_;
            bool reusingContext = false;
            if (context.isObject() && !context.isNil() && context.rawBits() > 0x10000) {
                // Reuse existing context — just update sender and state
                memory_.storePointer(0, context, sender);  // update sender
                reusingContext = true;
            } else {
                size_t contextSize = 6 + numTemps + 32;
                Oop contextClass = memory_.specialObject(SpecialObjectIndex::ClassMethodContext);
                uint32_t classIndex = contextClass.isObject() ? memory_.indexOfClass(contextClass) : 0;
                if (classIndex == 0) {
                    classIndex = 36;  // Fallback to typical Context class index
                }

                // Use IndexableWithFixed for contexts (format 3)
                context = memory_.allocateSlots(classIndex, contextSize, ObjectFormat::IndexableWithFixed);
                // Cache for future materializations of this frame
                currentFrameMaterializedCtx_ = context;

                // GC SAFETY: allocation may have triggered GC. Re-derive from roots.
                methodHdr = method_.asObjectPtr();
                if (frameDepth_ > 0) {
                    sender = savedFrames_[frameDepth_ - 1].materializedContext;
                }
                // (activeContext_ case: sender was already activeContext_, which is a GC root)
            }
            if (!context.isNil()) {
                int pc = ipOffset + 1;  // 1-based PC from 0-based offset

                memory_.storePointer(0, context, sender);                       // sender
                memory_.storePointer(1, context, Oop::fromSmallInteger(pc));    // pc
                // stackp is set below after we know how many items we saved
                memory_.storePointer(3, context, method_);                      // method
                memory_.storePointer(4, context, closure_);                      // closureOrNil
                memory_.storePointer(5, context, receiver_);                    // receiver

                // Copy current temps from frame
                // Temps are at framePointer_[1..numTemps] (receiver is at framePointer_[0])
                int savedCount = 0;
                for (int t = 0; t < numTemps && t < 32; t++) {
                    Oop temp = *(framePointer_ + 1 + t);
                    memory_.storePointer(6 + t, context, temp);
                    savedCount++;
                }

                // Also save operand stack items (above temps)
                // The operand stack is from framePointer_ + 1 + numTemps to stackPointer_ - 1
                Oop* operandBase = framePointer_ + 1 + numTemps;
                ptrdiff_t operandCount = stackPointer_ - operandBase;
                if (operandCount > 0 && operandCount < 100) {
                    for (ptrdiff_t o = 0; o < operandCount; o++) {
                        Oop item = *(operandBase + o);
                        memory_.storePointer(6 + numTemps + o, context, item);
                        savedCount++;
                    }
                }

                // Set stackp to actual number of items saved
                memory_.storePointer(2, context, Oop::fromSmallInteger(savedCount)); // stackp

                // Ensure context identity: if this frame has a closure (block context),
                // update the closure's outerContext to point to the freshly materialized
                // context for the SAME activation. This guarantees that thisContext home
                // (via closure >> outerContext) and thisContext sender return the same
                // object when the block was called directly from its creating method.
                //
                // IMPORTANT: Only update when sender.method == oldOuterContext.method,
                // meaning the sender is a re-materialization of the same activation.
                // Blocks passed through other methods (e.g., RecursionStopper during:
                // which calls ensure: which calls the block) have a sender that's a
                // DIFFERENT method — updating outerContext would break homeMethod navigation.
                if (closure_.isObject() && !closure_.isNil() && closure_.rawBits() > 0x10000) {
                    Oop oldOuterCtx = memory_.fetchPointer(0, closure_);
                    if (oldOuterCtx.isObject() && oldOuterCtx.rawBits() > 0x10000 &&
                        sender.isObject() && sender.rawBits() > 0x10000) {
                        Oop oldMethod = memory_.fetchPointer(3, oldOuterCtx);
                        Oop senderMethod = memory_.fetchPointer(3, sender);
                        if (oldMethod.rawBits() == senderMethod.rawBits()) {
                            memory_.storePointer(0, closure_, sender);
                        }
                    }
                }

                return context;
            }
        }
    }

    return sender;  // Return the last successfully created context
}

void Interpreter::transferTo(Oop newProcess) {
    Oop oldProcess = getActiveProcess();

    if (oldProcess.rawBits() == newProcess.rawBits()) {
        return;  // Already running this process
    }

    static int xferLog = 0;
    if (xferLog < 50) {
        xferLog++;
        int oldPri = safeProcessPriority(oldProcess);
        int newPri = safeProcessPriority(newProcess);
        fprintf(stderr, "[XFER-%d] old=0x%llx pri=%d -> new=0x%llx pri=%d\n",
                xferLog,
                (unsigned long long)oldProcess.rawBits(), oldPri,
                (unsigned long long)newProcess.rawBits(), newPri);
    }

    // Validate newProcess
    if (!newProcess.isObject()) {
        return;
    }

    ObjectHeader* newProcHdr = newProcess.asObjectPtr();
    if (newProcHdr->slotCount() < 2) {
        return;
    }


    // Save outgoing process's NLR state if any is active.
    // NLR state is global but logically per-process. Without saving/restoring,
    // a process switch during NLR through ensure: would either:
    // - Leak stale NLR state to the incoming process (killing P80 scheduler), or
    // - Clear it, losing the NLR for the outgoing process (Symbol class bug).
    bool hasActiveNlr = (nlrTargetCtx_.isObject() && !nlrTargetCtx_.isNil()) ||
                        (nlrHomeMethod_.isObject() && !nlrHomeMethod_.isNil());
    if (hasActiveNlr) {
        // Save NLR state for the outgoing process
        // First check if this process already has saved state (update in place)
        bool saved = false;
        for (int i = 0; i < savedNlrCount_; ++i) {
            if (savedNlrStates_[i].process.rawBits() == oldProcess.rawBits()) {
                savedNlrStates_[i].targetCtx = nlrTargetCtx_;
                savedNlrStates_[i].ensureCtx = nlrEnsureCtx_;
                savedNlrStates_[i].homeMethod = nlrHomeMethod_;
                savedNlrStates_[i].value = nlrValue_;
                saved = true;
                break;
            }
        }
        if (!saved && savedNlrCount_ < MAX_SAVED_NLR) {
            auto& s = savedNlrStates_[savedNlrCount_++];
            s.process = oldProcess;
            s.targetCtx = nlrTargetCtx_;
            s.ensureCtx = nlrEnsureCtx_;
            s.homeMethod = nlrHomeMethod_;
            s.value = nlrValue_;
        }
    }

    // Save current execution state to old process's suspendedContext
    // If we have inline frames, materialize them into context objects.
    //
    // GC SAFETY: materializeFrameStack() allocates context objects, which can
    // trigger GC compaction. After compaction, all C++ locals holding Oops are
    // stale. We store newProcess in a GC-root member field to protect it, and
    // re-derive oldProcess from getActiveProcess() after.
    Oop savedNewProcess = gcTempOop_;  // Save previous value
    gcTempOop_ = newProcess;  // Protect from GC (gcTempOop_ is a GC root)
    Oop contextToSave = materializeFrameStack();
    newProcess = gcTempOop_;  // Restore after potential GC
    gcTempOop_ = savedNewProcess;  // Restore previous value
    oldProcess = getActiveProcess();  // Re-derive from GC root

    // Save context unconditionally (matches official Pharo VM behavior).
    // The official VM does NOT check for nil sender — it always saves the
    // context. Process termination state is detected by Smalltalk code via
    // suspendedContext method == Process>>#endProcess, not via nil.
    if (!contextToSave.isNil() && contextToSave.isObject()) {
        memory_.storePointer(ProcessSuspendedContextIndex, oldProcess, contextToSave);
    }

    // Switch to new process
    setActiveProcess(newProcess);

    // Get new process's suspended context
    Oop newContext = memory_.fetchPointer(ProcessSuspendedContextIndex, newProcess);

    // Nil out the new process's suspendedContext now that we've read it.
    // This prevents GC from tracing stale context chains that keep objects alive.
    // The reference VM (cointerp.c transferTo:from:) does the same.
    memory_.storePointer(ProcessSuspendedContextIndex, newProcess, memory_.nil());

    // Reset interpreter state
    stackPointer_ = stackBase_;
    frameDepth_ = 0;
    lastCannotReturnCtx_ = Oop::nil();  // Clear per-process guard
    // Note: cannotReturnCount_ is NOT reset here — it tracks per process identity,
    // not per process switch. This prevents the error handler from resetting the
    // counter via intermediate process switches.

    // Resume execution from the new context
    executeFromContext(newContext);

    // Restore NLR state for the incoming process (if any was saved)
    bool restored = false;
    for (int i = 0; i < savedNlrCount_; ++i) {
        if (savedNlrStates_[i].process.rawBits() == newProcess.rawBits()) {
            nlrTargetCtx_ = savedNlrStates_[i].targetCtx;
            nlrEnsureCtx_ = savedNlrStates_[i].ensureCtx;
            nlrHomeMethod_ = savedNlrStates_[i].homeMethod;
            nlrValue_ = savedNlrStates_[i].value;
            // Remove this entry (swap with last)
            savedNlrStates_[i] = savedNlrStates_[--savedNlrCount_];
            restored = true;
            break;
        }
    }
    if (!restored) {
        // No saved NLR state for this process — clear (prevents stale leaks)
        nlrHomeMethod_ = Oop::nil();
        nlrValue_ = Oop::nil();
        nlrTargetCtx_ = Oop::nil();
        nlrEnsureCtx_ = Oop::nil();
    }
}

bool Interpreter::tryReschedule() {
    Oop nilObj = memory_.specialObject(SpecialObjectIndex::NilObject);
    Oop schedulerAssoc = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);

    if (!schedulerAssoc.isObject() || schedulerAssoc.rawBits() == nilObj.rawBits()) {
        std::cerr << "[RESCHEDULE] No valid scheduler association\n";
        return false;
    }

    Oop scheduler = memory_.fetchPointer(1, schedulerAssoc);
    if (!scheduler.isObject()) {
        return false;
    }

    // ProcessScheduler: slot 0 = quiescentProcessLists, slot 1 = activeProcess
    Oop activeProcess = memory_.fetchPointer(1, scheduler);
    Oop queues = memory_.fetchPointer(0, scheduler);


    if (!queues.isObject()) {
        return false;
    }

    ObjectHeader* queuesHeader = queues.asObjectPtr();
    size_t numQueues = queuesHeader->slotCount();

    // Search from highest to lowest priority
    for (int i = static_cast<int>(numQueues) - 1; i >= 0; i--) {
        Oop queue = queuesHeader->slotAt(i);
        if (!queue.isObject() || queue.rawBits() == nilObj.rawBits()) continue;

        // LinkedList: slot 0 = firstLink, slot 1 = lastLink
        Oop process = memory_.fetchPointer(0, queue);
        if (!process.isObject() || process.rawBits() == nilObj.rawBits()) continue;

        // Skip if this is the same process that just finished
        if (process.rawBits() == activeProcess.rawBits()) {
            continue;
        }

        // Process: slot 0 = nextLink, slot 1 = suspendedContext, slot 2 = priority
        Oop context = memory_.fetchPointer(1, process);
        if (!context.isObject() || context.rawBits() == nilObj.rawBits()) {
            continue;
        }

        ObjectHeader* ctxHeader = context.asObjectPtr();
        if (ctxHeader->format() != ObjectFormat::IndexableWithFixed) {
            continue;
        }

        // Log the process we're about to schedule

        // Remove process from ready queue BEFORE making it active.
        // Without this, the process stays in the queue AND runs as activeProcess,
        // causing queue corruption after hundreds of fork/terminate cycles.
        removeFirstLinkOfList(queue);

        // Update the active process in scheduler
        memory_.storePointer(1, scheduler, process);

        // Reset stack for new process
        stackPointer_ = stackBase_;
        frameDepth_ = 0;

        // Execute from the new process's context
        if (executeFromContext(context)) {
            return true;
        }
    }

    return false;
}

void Interpreter::checkForPreemption() {
    // Periodic preemption check - allow other runnable processes to run
    // This simulates the timer-based preemption of CogVM
    if constexpr (ENABLE_DEBUG_LOGGING) {
    }

    Oop nilObj = memory_.specialObject(SpecialObjectIndex::NilObject);
    Oop schedulerAssoc = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);

    if (!schedulerAssoc.isObject() || schedulerAssoc.rawBits() == nilObj.rawBits()) {
        return;
    }

    Oop scheduler = memory_.fetchPointer(1, schedulerAssoc);
    if (!scheduler.isObject()) {
        return;
    }

    // ProcessScheduler: slot 0 = quiescentProcessLists, slot 1 = activeProcess
    Oop activeProcess = memory_.fetchPointer(1, scheduler);
    Oop queues = memory_.fetchPointer(0, scheduler);

    if (!queues.isObject()) {
        return;
    }

    // Get active process priority
    Oop activePriorityOop = memory_.fetchPointer(ProcessPriorityIndex, activeProcess);
    int activePriority = activePriorityOop.isSmallInteger() ?
                         static_cast<int>(activePriorityOop.asSmallInteger()) : 0;

    ObjectHeader* queuesHeader = queues.asObjectPtr();
    size_t numQueues = queuesHeader->slotCount();

    // Log periodically

    // Check for runnable processes at STRICTLY higher priority only.
    // Same-priority round-robin is handled by relinquishProcessor/yield.
    // Priorities are 1-based, queue indices are 0-based: queue[p-1] = priority p.
    size_t startIdx = (activePriority > 0) ? static_cast<size_t>(activePriority) : 0;
    for (size_t i = startIdx; i < numQueues; i++) {
        Oop queue = queuesHeader->slotAt(i);
        if (!queue.isObject() || queue.rawBits() == nilObj.rawBits()) continue;

        Oop firstProcess = memory_.fetchPointer(0, queue);
        if (!firstProcess.isObject() || firstProcess.rawBits() == nilObj.rawBits()) continue;

        // Skip if it's the current process
        if (firstProcess.rawBits() == activeProcess.rawBits()) continue;

        // Found a higher-priority runnable process - preempt!
        if (++preemptCount_ <= 5) {
            fprintf(stderr, "[PREEMPT] P%d→P%zu (active=0x%llx → 0x%llx)\n",
                    activePriority, i + 1,
                    (unsigned long long)activeProcess.rawBits(),
                    (unsigned long long)firstProcess.rawBits());
        }

        // Remove process from ready queue using the proper helper
        // (removeFirstLinkOfList clears both nextLink and myList)
        removeFirstLinkOfList(queue);

        // Put current process back on ready queue
        putToSleep(activeProcess);

        // Switch to new process
        transferTo(firstProcess);
        return;
    }
}

// ===== STARTUP SUPPORT =====

void Interpreter::installOSiOSDriver() {
    // Try to install OSiOSDriver to start the event loop.
    // Called from step() after the image has had time to initialize.

    Oop nilObj = memory_.specialObject(SpecialObjectIndex::NilObject);
    Oop osDriverClass = memory_.findGlobal("OSiOSDriver");

    // If OSiOSDriver not found, nothing to do
    if (osDriverClass.rawBits() == nilObj.rawBits()) {
        return;
    }

    // Lambda to find a method by name in a class's method dictionary
    auto findMethodInClass = [&](Oop classObj, const char* selectorName) -> Oop {
        if (!classObj.isObject()) return Oop::nil();
        Oop methodDict = memory_.fetchPointer(1, classObj);
        if (!methodDict.isObject()) return Oop::nil();
        ObjectHeader* mdHeader = methodDict.asObjectPtr();
        size_t mdSlots = mdHeader->slotCount();
        for (size_t mi = 2; mi < mdSlots; mi++) {
            Oop key = mdHeader->slotAt(mi);
            if (!key.isObject() || key.isNil()) continue;
            ObjectHeader* keyHdr = key.asObjectPtr();
            if (!keyHdr->isBytesObject()) continue;
            size_t keyLen = keyHdr->byteSize();
            if (keyLen == strlen(selectorName) &&
                memcmp(keyHdr->bytes(), selectorName, keyLen) == 0) {
                Oop values = memory_.fetchPointer(1, methodDict);
                if (values.isObject()) {
                    ObjectHeader* valHdr = values.asObjectPtr();
                    size_t valueIdx = mi - 2;
                    if (valueIdx < valHdr->slotCount()) {
                        return valHdr->slotAt(valueIdx);
                    }
                }
            }
        }
        return Oop::nil();
    };

    // Walk the class hierarchy looking for setupEventLoop
    Oop setupMethod = Oop::nil();
    {
        Oop currentClass = osDriverClass;
        int depth = 0;
        while (currentClass.isObject() && !currentClass.isNil() && depth < 10) {
            setupMethod = findMethodInClass(currentClass, "setupEventLoop");
            if (setupMethod.isObject() && !setupMethod.isNil()) {
                break;
            }
            currentClass = memory_.fetchPointer(0, currentClass);  // superclass
            depth++;
        }
    }

    // Check if we're using OSiOSDriver (vs. some other driver class)
    bool usingOSiOSDriver = false;
    {
        Oop nameOop = memory_.fetchPointer(6, osDriverClass);
        if (nameOop.isObject()) {
            ObjectHeader* nameHdr = nameOop.asObjectPtr();
            if (nameHdr->isBytesObject() && nameHdr->byteSize() == 11) {
                if (memcmp(nameHdr->bytes(), "OSiOSDriver", 11) == 0) {
                    usingOSiOSDriver = true;
                }
            }
        }
    }

    if (setupMethod.isNil() || !setupMethod.isObject()) {
        // setupEventLoop not found - enable VM-based event processing
        enableDirectInputSignaling_ = true;
    }

    if (setupMethod.isObject() && !setupMethod.isNil()) {
        pendingDriverSetupMethod_ = setupMethod;
        pendingDriverSetupReceiver_ = Oop::nil();  // Will be filled in later
        hasPendingDriverSetup_ = true;
    }
}
bool Interpreter::executePendingDriverInstall() {
    if (!hasPendingDriverInstall_) {
        return false;
    }

    hasPendingDriverInstall_ = false;  // Clear flag before executing


    if (pendingDriverInstallMethod_.isNil() || !pendingDriverInstallMethod_.isObject()) {
        return false;
    }

    // Create a context for OSiOSDriver install and execute it
    Oop context;
    if (pendingDriverMethodNeedsArg_) {
        // startUp: needs a boolean argument (resuming = true)
        context = memory_.createStartupContextWithArg(pendingDriverInstallMethod_, pendingDriverInstallReceiver_, memory_.trueObject());
    } else {
        context = memory_.createStartupContext(pendingDriverInstallMethod_, pendingDriverInstallReceiver_);
    }
    pendingDriverMethodNeedsArg_ = false;  // Reset flag

    if (context.isNil()) {
        return false;
    }


    // DON'T restore state after executeFromContext - we want the method to actually run!
    // The method will run in subsequent step() calls. When it returns (to nil sender),
    // the scheduler will pick up another process.
    //
    // NOTE: The previous bug was that we reset stackPointer_ and frameDepth_ but the
    // original process's context was lost. The fix is to save the original process's
    // suspended context BEFORE switching.

    // Get the current active process and save its state
    Oop activeProcess = getActiveProcess();
    if (activeProcess.isObject() && !activeProcess.isNil()) {
        // Save current context as the process's suspendedContext
        // This allows resumption later when the driver method completes
        if (activeContext_.isObject() && !activeContext_.isNil()) {
            // Update PC in the context before suspending
            // Note: the context's PC needs to point to next instruction
            // For now, we'll leave this to the normal process switching mechanism
        }
    }

    // Set up fresh stack for driver method
    stackPointer_ = stackBase_;
    frameDepth_ = 0;

    bool result = executeFromContext(context);


    // Clear the pending install (method is now set up to run)
    pendingDriverInstallMethod_ = Oop::nil();
    pendingDriverInstallReceiver_ = Oop::nil();

    return result;
}

// autoLoadDriver removed — was a dead no-op called from hardcoded selector matching

bool Interpreter::bootstrapStartup() {

    // In Spur, nil is an actual object at heap start, not 0
    Oop nilObj = memory_.specialObject(SpecialObjectIndex::NilObject);

    // Approach 1 (standard VM behavior): Resume the scheduler's active process.
    // This is the process that called primitiveSnapshot. Its suspendedContext
    // holds the return point where the snapshot method checks true/false to
    // decide whether to fire session startup. If we skip this and run a random
    // queued process instead, session startup never fires and the display is
    // never reinitialized — causing corrupted/frozen screen on reload.
    Oop schedulerAssoc = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);
    if (schedulerAssoc.isObject()) {
        Oop scheduler = memory_.fetchPointer(1, schedulerAssoc);
        if (scheduler.isObject()) {
            Oop activeProcess = memory_.fetchPointer(1, scheduler);
            if (activeProcess.isObject()) {
                ObjectHeader* procHeader = activeProcess.asObjectPtr();

                if (procHeader->slotCount() > 1) {
                    Oop suspendedCtx = procHeader->slotAt(1);

                    if (!suspendedCtx.isNil() && suspendedCtx.isObject()) {
                        ObjectHeader* ctxHdr = suspendedCtx.asObjectPtr();
                        if (ctxHdr->format() == ObjectFormat::IndexableWithFixed) {
                            fprintf(stderr, "[BOOTSTRAP] Resuming active process (standard path)\n");
                            imageBooted_ = true;
                            return executeFromContext(suspendedCtx);
                        }
                    }
                }
            }
        }
    }

    // Approach 2 (fallback): Scan scheduler queues for a runnable process.
    // Only reached if the active process has no valid suspendedContext.
    if (schedulerAssoc.rawBits() != nilObj.rawBits() && schedulerAssoc.isObject()) {
        Oop scheduler = memory_.fetchPointer(1, schedulerAssoc);
        if (scheduler.isObject()) {
            Oop queues = memory_.fetchPointer(0, scheduler);
            if (queues.isObject()) {
                ObjectHeader* queuesHeader = queues.asObjectPtr();
                size_t numQueues = queuesHeader->slotCount();

                for (int i = static_cast<int>(numQueues) - 1; i >= 0; i--) {
                    Oop queue = queuesHeader->slotAt(i);
                    if (queue.rawBits() == nilObj.rawBits() || !queue.isObject()) continue;

                    Oop firstProcess = memory_.fetchPointer(0, queue);
                    if (firstProcess.rawBits() == nilObj.rawBits() || !firstProcess.isObject()) continue;

                    Oop context = memory_.fetchPointer(1, firstProcess);
                    if (context.rawBits() != nilObj.rawBits() && context.isObject()) {
                        ObjectHeader* ctxHeader = context.asObjectPtr();
                        if (ctxHeader->format() == ObjectFormat::IndexableWithFixed) {
                            fprintf(stderr, "[BOOTSTRAP] Resuming from queue (fallback path)\n");
                            imageBooted_ = true;
                            return executeFromContext(context);
                        }
                    }
                }
            }
        }
    }

    // Once the image has booted, don't retry Approach 3 startup methods.
    // The caller's idle loop handles the "nothing to run" case properly.
    if (imageBooted_) {
        return false;
    }

    // Approach 3: Try to find and call a startup method directly
    // Use static tracking to prevent infinite loops

    startupAttempt_++;
    // Log every startup attempt to verify the code is being reached
    if constexpr (ENABLE_DEBUG_LOGGING) {
        fprintf(stderr, "[BOOTSTRAP] Attempt #%d\n", startupAttempt_);
        static FILE* startupLog = nullptr;
        if (startupLog) {
            fprintf(startupLog, "[BOOTSTRAP] Attempt #%d\n", startupAttempt_);
            fflush(startupLog);
        }
    }

    // Initialize the platform display ONCE with a test pattern
    // Skip Smalltalk Form creation - just write directly to platform buffer
    if (!displayInitialized_ && pharo::gDisplaySurface) {
        displayInitialized_ = true;

        uint32_t* pixels = pharo::gDisplaySurface->pixels();
        int width = pharo::gDisplaySurface->width();
        int height = pharo::gDisplaySurface->height();

        // Fill with a blue gradient pattern to verify display works
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                uint8_t r = static_cast<uint8_t>(128 + (x * 127 / width));
                uint8_t g = static_cast<uint8_t>(128 + (y * 127 / height));
                uint8_t b = 255;
                pixels[y * width + x] = (255 << 24) | (r << 16) | (g << 8) | b;  // ARGB
            }
        }

        pharo::gDisplaySurface->update();

        // Try to find and activate the Display Form from the image
        // This connects Pharo's Display object to our display surface
        Oop displayObj = memory_.findGlobal("Display");
        if (!displayObj.isNil() && displayObj.isObject()) {
            // Call beDisplay on it (primitive 126)
            setDisplayForm(displayObj);

            // Get the Form's dimensions and update our screen size
            // Form slots: 0=bits, 1=width, 2=height, 3=depth
            // (dimensions used internally for screen sizing)
        } else {
            // Display object not found, creating one

            // Try to create a display Form directly
            // Form structure: 0=bits, 1=width, 2=height, 3=depth, 4=offset
            int dispWidth = pharo::gDisplaySurface ? pharo::gDisplaySurface->width() : 1024;
            int dispHeight = pharo::gDisplaySurface ? pharo::gDisplaySurface->height() : 768;

            // Find Form and Bitmap classes
            Oop formClass = memory_.findGlobal("Form");
            Oop bitmapClass = memory_.findGlobal("Bitmap");

            if (!formClass.isNil() && formClass.isObject() &&
                !bitmapClass.isNil() && bitmapClass.isObject()) {

                uint32_t formClassIdx = memory_.indexOfClass(formClass);
                uint32_t bitmapClassIdx = memory_.indexOfClass(bitmapClass);

                if (formClassIdx > 0 && bitmapClassIdx > 0) {
                    // Allocate bitmap for pixels (32-bit pixels = 1 word each)
                    size_t pixelCount = static_cast<size_t>(dispWidth) * dispHeight;
                    Oop bitmapObj = memory_.allocateWords(bitmapClassIdx, pixelCount);

                    if (!bitmapObj.isNil()) {
                        // GC SAFETY: push bitmapObj before second allocation
                        push(bitmapObj);
                        // Allocate form with 5 slots
                        Oop formObj = memory_.allocateSlots(formClassIdx, 5);
                        bitmapObj = pop();

                        if (!formObj.isNil()) {
                            // Set form slots: bits, width, height, depth, offset
                            memory_.storePointer(0, formObj, bitmapObj);
                            memory_.storePointer(1, formObj, Oop::fromSmallInteger(dispWidth));
                            memory_.storePointer(2, formObj, Oop::fromSmallInteger(dispHeight));
                            memory_.storePointer(3, formObj, Oop::fromSmallInteger(32));
                            memory_.storePointer(4, formObj, Oop::fromSmallInteger(0));  // offset = 0@0

                            // Set as display form locally
                            setDisplayForm(formObj);
                            setScreenSize(dispWidth, dispHeight);
                            setScreenDepth(32);

                            // Bind to 'Display' global so Morphic can find it
                            memory_.setGlobal("Display", formObj);
                        }
                    }
                }
            }
        }
    }

    // If we've already completed all initial startup steps, just return false.
    // The caller's idle loop handles the "nothing to run" case properly.
    if (startupAttempt_ > 100) {
        return false;
    }


    // Helper lambda to look up method directly from a class's methodDict
    // (bypasses classOf which may fail for metaclasses not in class table)
    auto lookupMethodInClass = [&](Oop classObj, const char* selectorName) -> Oop {
        if (!classObj.isObject()) return Oop::nil();

        // Get the method dictionary (slot 1 of the class)
        Oop methodDict = memory_.fetchPointer(1, classObj);
        if (!methodDict.isObject()) return Oop::nil();

        ObjectHeader* mdHeader = methodDict.asObjectPtr();
        size_t mdSlots = mdHeader->slotCount();

        // Find the selector in the methodDict
        for (size_t i = 2; i < mdSlots; i++) {
            Oop key = mdHeader->slotAt(i);
            if (!key.isObject() || key.isNil()) continue;

            ObjectHeader* keyHdr = key.asObjectPtr();
            if (!keyHdr->isBytesObject()) continue;

            size_t keyLen = keyHdr->byteSize();
            const char* keyBytes = (const char*)keyHdr->bytes();

            if (keyLen == strlen(selectorName) && memcmp(keyBytes, selectorName, keyLen) == 0) {
                // Found the selector! Get the method from values array (slot 1)
                // MethodDictionary layout: slot 0 = tally, slot 1 = values array, slot 2+ = keys
                // Keys at slot i correspond to values at index i-2 in the values array
                Oop values = memory_.fetchPointer(1, methodDict);
                if (values.isObject()) {
                    ObjectHeader* valHdr = values.asObjectPtr();
                    size_t valueIdx = i - 2;  // Offset by 2 (skip tally and values slots)
                    if (valueIdx < valHdr->slotCount()) {
                        Oop method = valHdr->slotAt(valueIdx);
                                  // << " key@slot " << i << " -> value@" << valueIdx
                                  // << " = 0x" << std::hex << method.rawBits() << std::dec;
                        return method;
                    }
                }
            }
        }
        return Oop::nil();
    };


    // First try: SmalltalkImage >> recordStartupStamp
    if (startupAttempt_ == 1) {
        // SmalltalkImage is the class; we need to find "Smalltalk" which is the instance
        Oop smalltalk = memory_.findGlobal("Smalltalk");
        Oop smalltalkImageClass = memory_.findGlobal("SmalltalkImage");

        // Debug logging
        if constexpr (ENABLE_DEBUG_LOGGING) {
        }
        Oop nilObj = memory_.specialObject(SpecialObjectIndex::NilObject);

        // Use Smalltalk instance if available, otherwise try to get instance from class
        Oop receiver = smalltalk;
        Oop classForLookup = smalltalkImageClass;

        if (receiver.isNil() && smalltalkImageClass.isObject()) {
            // Try to call "current" on SmalltalkImage class to get the instance
            // But for now, just use the class as receiver and look up class-side method
            receiver = smalltalkImageClass;
        }

        if (!receiver.isNil() && receiver.isObject() && classForLookup.isObject()) {
            // recordStartupStamp is an instance method, look it up in the instance's class
            Oop method = lookupMethodInClass(classForLookup, "recordStartupStamp");
            if (!method.isNil() && method.isObject()) {
                // Use the actual instance as receiver
                Oop context = memory_.createStartupContext(method, receiver);
                if (!context.isNil()) {
                    stackPointer_ = stackBase_;
                    frameDepth_ = 0;
                    if (executeFromContext(context)) {
                        return true;
                    }
                }
            }
        }
    }

    // Second try: restartMethods
    if (startupAttempt_ == 2) {
        // Find Smalltalk instance and SmalltalkImage class
        Oop smalltalk = memory_.findGlobal("Smalltalk");
        Oop smalltalkImageClass = memory_.findGlobal("SmalltalkImage");

        if constexpr (ENABLE_DEBUG_LOGGING) {
        }

        Oop receiver = smalltalk.isObject() ? smalltalk : smalltalkImageClass;
        if (smalltalkImageClass.isObject()) {
            Oop method = lookupMethodInClass(smalltalkImageClass, "restartMethods");
            if (!method.isNil() && method.isObject()) {
                Oop context = memory_.createStartupContext(method, receiver);
                if (!context.isNil()) {
                    stackPointer_ = stackBase_;
                    frameDepth_ = 0;
                    if (executeFromContext(context)) {
                        return true;
                    }
                }
            } else {
            }
        }
    }

    // Attempt 3: Ensure UIManager is initialized (spawns UI process)
    // This is needed because UIManager may not be registered as a startup handler
    // in some Pharo images, preventing the Morphic event loop from starting.
    if (startupAttempt_ == 3) {
        if constexpr (ENABLE_DEBUG_LOGGING) {
        }

        if (!uiManagerStarted_) {
            uiManagerStarted_ = true;


            // Find UIManager class
            Oop uiManagerClass = memory_.findGlobal("UIManager");
            Oop nilObj = memory_.specialObject(SpecialObjectIndex::NilObject);


            if (uiManagerClass.isObject() && uiManagerClass.rawBits() != nilObj.rawBits()) {
                // Get the metaclass (UIManager class) for class-side method lookup
                Oop metaclass = memory_.classOf(uiManagerClass);


                // Look up startUp: method on the class side
                Oop method = lookupMethodInClass(metaclass, "startUp:");


                if (!method.isNil() && method.isObject()) {
                    // Create context for UIManager class>>startUp: true
                    // The receiver is UIManager class, argument is true
                    Oop context = memory_.createStartupContextWithArg(method, uiManagerClass, memory_.trueObject());

                    if (!context.isNil()) {
                        stackPointer_ = stackBase_;
                        frameDepth_ = 0;
                        if (executeFromContext(context)) {
                            return true;
                        }
                    }
                }
            }
        }
    }

    // Fourth try and beyond: Keep calling World>>doOneCycle for UI loop
    if (startupAttempt_ >= 4 && startupAttempt_ <= 100) {
        // One-time: Try to start InputEventSensor's event loop
        if (!sensorStartAttempted_) {
            sensorStartAttempted_ = true;


            // Find the Sensor global (InputEventSensor instance)
            Oop sensor = memory_.findGlobal("Sensor");
            if (!sensor.isNil() && sensor.isObject()) {
                Oop sensorClass = memory_.classOf(sensor);
                if (!sensorClass.isNil() && sensorClass.isObject()) {
                    // Try several possible method names for starting the event loop
                    const char* methodNames[] = {
                        "startUp", "startEventLoop", "installEventLoop",
                        "startUp:", "install", "eventLoopProcess", nullptr
                    };
                    Oop startUpMethod = Oop::nil();
                    const char* foundMethodName = nullptr;
                    for (int i = 0; methodNames[i] != nullptr; i++) {
                        startUpMethod = lookupMethodInClass(sensorClass, methodNames[i]);
                        if (!startUpMethod.isNil() && startUpMethod.isObject()) {
                            foundMethodName = methodNames[i];
                            break;
                        }
                    }
                    if (!startUpMethod.isNil() && startUpMethod.isObject()) {
                        Oop context = memory_.createStartupContext(startUpMethod, sensor);
                        if (!context.isNil()) {
                            // Execute startUp in the current context
                            // Push it onto the context stack for execution
                            stackPointer_ = stackBase_;
                            frameDepth_ = 0;
                            if (executeFromContext(context)) {
                                return true;  // Let startUp complete before doing doOneCycle
                            }
                        }
                    } else {
                    }
                }
            }
        }

        // Find World class - Note: World is a global that holds the current WorldMorph
        Oop world = memory_.findGlobal("World");

        if (!world.isNil() && world.isObject()) {
            // World is an instance of WorldMorph - get its class for method lookup
            Oop worldClass = memory_.classOf(world);

            // If classOf failed, try looking up WorldMorph by name
            if (worldClass.isNil() || worldClass.rawBits() == 0) {
                worldClass = memory_.findGlobal("WorldMorph");
            }

            // Try to call doOneCycle on the World instance
            Oop method = lookupMethodInClass(worldClass, "doOneCycle");
            if (!method.isNil() && method.isObject()) {
                Oop context = memory_.createStartupContext(method, world);  // Pass instance, not class
                if (!context.isNil()) {
                    stackPointer_ = stackBase_;
                    frameDepth_ = 0;
                    if (executeFromContext(context)) {
                        return true;
                    }
                }
            }
        }
    }

    // Last resort: Try Object >> yourself just to prove basic execution works
    if (startupAttempt_ == 4) {
        Oop arrayClass = memory_.specialObject(SpecialObjectIndex::ClassArray);
        if (arrayClass.isObject()) {
            Oop selector = findSelector("yourself");
            if (!selector.isNil()) {
                Oop method = lookupMethod(selector, arrayClass);
                if (!method.isNil() && method.isObject()) {
                    Oop receiver = memory_.allocateSlots(arrayClass.asObjectPtr()->classIndex(), 0);
                    if (receiver.isObject()) {
                        Oop context = memory_.createStartupContext(method, receiver);
                        if (!context.isNil()) {
                            stackPointer_ = stackBase_;
                            frameDepth_ = 0;
                            if (executeFromContext(context)) {
                                return true;
                            }
                        }
                    }
                }
            }
        }
    }

    // Attempt 5: If SUnitRunner is installed and "test" image arg is present,
    // directly call SUnitRunner>>runAllTests to bypass SessionManager.
    if (startupAttempt_ == 5) {
        bool isTestMode = false;
        for (auto& arg : imageArguments_) {
            if (arg == "test") { isTestMode = true; break; }
        }
        if (isTestMode) {
            Oop sunitRunner = memory_.findGlobal("SUnitRunner");
            if (sunitRunner.isObject() && !sunitRunner.isNil()) {
                // Look up runAllTests on the metaclass (class-side method)
                Oop metaclass = memory_.classOf(sunitRunner);
                Oop method = lookupMethodInClass(metaclass, "runAllTests");
                if (!method.isNil() && method.isObject()) {
                    fprintf(stderr, "[BOOTSTRAP] SUnitRunner found, calling runAllTests\n");
                    Oop context = memory_.createStartupContext(method, sunitRunner);
                    if (!context.isNil()) {
                        stackPointer_ = stackBase_;
                        frameDepth_ = 0;
                        if (executeFromContext(context)) {
                            return true;
                        }
                    }
                }
            }
        }
    }

    // If we get here, we've exhausted our startup options
    // Note: This is normal for headless images - the startup methods executed
    // successfully in earlier attempts, but the Smalltalk code returned because
    // there's no GUI event loop to run.
    return false;
}

Oop Interpreter::findSelector(const char* name) {
    // Find a selector symbol by searching through method dictionaries
    // Modern Pharo MethodDictionary stores keys INLINE at slot 2+

    // Search through several well-known classes to find the selector
    // Also search Morphic classes for UI selectors like comeToFront, activate
    // And startup-related classes for deferred startup actions
    Oop morphClass = memory_.findGlobal("Morph");
    Oop systemWindowClass = memory_.findGlobal("SystemWindow");
    Oop spWindowClass = memory_.findGlobal("SpWindow");
    Oop worldMorphClass = memory_.findGlobal("WorldMorph");
    Oop worldStateClass = memory_.findGlobal("WorldState");
    Oop menuMorphClass = memory_.findGlobal("MenuMorph");
    Oop sessionManagerClass = memory_.findGlobal("SessionManager");
    Oop workingSessionClass = memory_.findGlobal("WorkingSession");
    Oop pharoCommonToolsClass = memory_.findGlobal("PharoCommonTools");

    Oop classesToSearch[] = {
        memory_.specialObject(SpecialObjectIndex::ClassArray),
        memory_.specialObject(SpecialObjectIndex::ClassByteString),
        memory_.specialObject(SpecialObjectIndex::ClassSmallInteger),
        memory_.specialObject(SpecialObjectIndex::ClassMethodContext),
        memory_.specialObject(SpecialObjectIndex::ClassBlockClosure),
        memory_.specialObject(SpecialObjectIndex::ClassProcess),
        morphClass,
        systemWindowClass,
        spWindowClass,
        worldMorphClass,
        worldStateClass,
        menuMorphClass,
        sessionManagerClass,
        workingSessionClass,
        pharoCommonToolsClass,
        Oop::nil()
    };

    // Also search the class of SmalltalkImage
    Oop smalltalkImage = memory_.findGlobal("SmalltalkImage");
    if (smalltalkImage.isObject()) {
        // Debug: show what SmalltalkImage looks like
        ObjectHeader* siHdr = smalltalkImage.asObjectPtr();
                  // << " classIdx=" << std::dec << siHdr->classIndex()
                  // << " slots=" << siHdr->slotCount();

        // SmalltalkImage is a class, so search its metaclass (class of the class)
        Oop metaclass = memory_.classOf(smalltalkImage);
                  // << metaclass.rawBits() << std::dec;

        // If classOf returns nil, try directly accessing the classIndex
        if (metaclass.isNil() || metaclass.rawBits() == 0) {
            metaclass = memory_.classAtIndex(siHdr->classIndex());
                      // << ") = 0x" << std::hex << metaclass.rawBits() << std::dec;

            // Still nil? Try searching the method dictionary of SmalltalkImage directly
            // SmalltalkImage is a class, slot 1 = methodDict for instance methods
            // For class methods, we'd need the metaclass, but since that's not available,
            // let's search the class's own method dictionary for selectors
            if (metaclass.isNil() || metaclass.rawBits() == 0) {
                Oop methodDict = memory_.fetchPointer(1, smalltalkImage);
                if (methodDict.isObject()) {
                    ObjectHeader* mdHeader = methodDict.asObjectPtr();
                              // << mdHeader->slotCount() << " slots, cls="

                    // Debug: list ALL selectors looking for startup-related ones
                    static bool selectorsDumped = false;
                    if (!selectorsDumped) {
                        selectorsDumped = true;
                        for (size_t i = 2; i < mdHeader->slotCount(); i++) {
                            Oop key = mdHeader->slotAt(i);
                            if (key.isObject() && !key.isNil()) {
                                ObjectHeader* keyHdr = key.asObjectPtr();
                                if (keyHdr->isBytesObject() && keyHdr->byteSize() <= 80) {
                                    std::string keyStr((char*)keyHdr->bytes(), keyHdr->byteSize());
                                    // Only print startup/session related
                                    if (keyStr.find("start") != std::string::npos ||
                                        keyStr.find("Start") != std::string::npos ||
                                        keyStr.find("session") != std::string::npos ||
                                        keyStr.find("Session") != std::string::npos ||
                                        keyStr.find("current") != std::string::npos ||
                                        keyStr.find("initialize") != std::string::npos) {
                                    }
                                }
                            }
                        }
                    }

                    // Search for selector in method dict (keys at slot 2+)
                              // << mdHeader->slotCount() << " slots...";
                    for (size_t i = 2; i < mdHeader->slotCount(); i++) {
                        Oop key = mdHeader->slotAt(i);
                        if (key.isObject() && !key.isNil()) {
                            ObjectHeader* keyHdr = key.asObjectPtr();
                            // Direct string comparison
                            if (keyHdr->isBytesObject()) {
                                size_t keyLen = keyHdr->byteSize();
                                const char* keyBytes = (const char*)keyHdr->bytes();
                                if (keyLen == strlen(name) && memcmp(keyBytes, name, keyLen) == 0) {
                                              // << "' at slot " << i << " in SmalltalkImage methodDict!";
                                    return key;
                                }
                            }
                        }
                    }
                }
            }
        }

        if (metaclass.isObject()) {
            // Search metaclass hierarchy
            Oop currentClass = metaclass;
            int depth = 0;
            while (currentClass.isObject() && !currentClass.isNil() && depth < 20) {
                ObjectHeader* classHdr = currentClass.asObjectPtr();
                if (classHdr->slotCount() < 2) break;

                Oop methodDict = memory_.fetchPointer(1, currentClass);
                          // << methodDict.rawBits() << std::dec;
                if (methodDict.isObject()) {
                    ObjectHeader* mdHeader = methodDict.asObjectPtr();
                    size_t mdSlots = mdHeader->slotCount();
                    // Keys are stored inline from slot 2 onwards
                    for (size_t i = 2; i < mdSlots; i++) {
                        Oop key = mdHeader->slotAt(i);
                        if (key.isObject() && !key.isNil()) {
                            if (memory_.symbolEquals(key, name)) {
                                return key;
                            }
                        }
                    }
                }

                // Move to superclass
                currentClass = memory_.fetchPointer(0, currentClass);
                depth++;
            }
        }

        // Also search SmalltalkImage class itself (for instance methods)
        Oop currentClass = smalltalkImage;
        int depth = 0;
        while (currentClass.isObject() && !currentClass.isNil() && depth < 20) {
            ObjectHeader* classHdr = currentClass.asObjectPtr();
            if (classHdr->slotCount() < 2) break;

            Oop methodDict = memory_.fetchPointer(1, currentClass);
            if (methodDict.isObject()) {
                ObjectHeader* mdHeader = methodDict.asObjectPtr();
                size_t mdSlots = mdHeader->slotCount();

                // Keys are stored inline from slot 2 onwards
                for (size_t i = 2; i < mdSlots; i++) {
                    Oop key = mdHeader->slotAt(i);
                    if (key.isObject() && !key.isNil()) {
                        if (memory_.symbolEquals(key, name)) {
                            return key;
                        }
                    }
                }
            }

            // Move to superclass
            currentClass = memory_.fetchPointer(0, currentClass);
            depth++;
        }
    }

    // Search through common classes (both instance and class side)
    for (int ci = 0; !classesToSearch[ci].isNil(); ci++) {
        Oop classObj = classesToSearch[ci];
        if (!classObj.isObject()) continue;

        // Search instance methods (in the class itself)
        Oop currentClass = classObj;
        int depth = 0;

        while (currentClass.isObject() && !currentClass.isNil() && depth < 20) {
            ObjectHeader* classHdr = currentClass.asObjectPtr();
            if (classHdr->slotCount() < 2) break;

            Oop methodDict = memory_.fetchPointer(1, currentClass);
            if (methodDict.isObject()) {
                ObjectHeader* mdHeader = methodDict.asObjectPtr();
                size_t mdSlots = mdHeader->slotCount();

                // Keys are stored inline from slot 2 onwards
                for (size_t i = 2; i < mdSlots; i++) {
                    Oop key = mdHeader->slotAt(i);
                    if (key.isObject() && !key.isNil()) {
                        if (memory_.symbolEquals(key, name)) {
                            return key;
                        }
                    }
                }
            }

            // Move to superclass
            currentClass = memory_.fetchPointer(0, currentClass);
            depth++;
        }

        // Also search class methods (in the metaclass)
        // The metaclass is the class of the class object
        Oop metaclass = memory_.classOf(classObj);
        if (metaclass.isNil() || !metaclass.isObject()) {
            // Try direct class index lookup
            ObjectHeader* classHdr = classObj.asObjectPtr();
            metaclass = memory_.classAtIndex(classHdr->classIndex());
        }

        currentClass = metaclass;
        depth = 0;
        while (currentClass.isObject() && !currentClass.isNil() && depth < 20) {
            ObjectHeader* classHdr = currentClass.asObjectPtr();
            if (classHdr->slotCount() < 2) break;

            Oop methodDict = memory_.fetchPointer(1, currentClass);
            if (methodDict.isObject()) {
                ObjectHeader* mdHeader = methodDict.asObjectPtr();
                size_t mdSlots = mdHeader->slotCount();

                for (size_t i = 2; i < mdSlots; i++) {
                    Oop key = mdHeader->slotAt(i);
                    if (key.isObject() && !key.isNil()) {
                        if (memory_.symbolEquals(key, name)) {
                            return key;
                        }
                    }
                }
            }

            // Move to superclass (of metaclass)
            currentClass = memory_.fetchPointer(0, currentClass);
            depth++;
        }
    }

    return Oop::nil();
}


bool Interpreter::executeFromContext(Oop context) {
    // Set up SIGSEGV recovery point - if we crash accessing unrelocated pointers,
    // we'll longjmp back here and return false instead of terminating the VM
    if (sigsetjmp(g_sigsegvRecovery, 1) != 0) {
        // Returned from SIGSEGV recovery
        g_sigsegvRecoveryEnabled = 0;
        stackPointer_ = stackBase_;
        frameDepth_ = 0;
        return false;
    }
    g_sigsegvRecoveryEnabled = 1;
    // RAII guard to disable recovery on any return path
    struct SigsegvGuard { ~SigsegvGuard() { g_sigsegvRecoveryEnabled = 0; } } sigsegvGuard;

    // Reset interpreter stack - each context execution starts fresh
    // The context object stores the Smalltalk stack state, which we'll restore below
    stackPointer_ = stackBase_;
    frameDepth_ = 0;
    currentFrameMaterializedCtx_ = memory_.nil();

    // Reset bytecode extension registers - they are per-bytecode-sequence state
    // and must not leak between processes during a process switch.
    // Without this, a process switch after an extension byte (0xE0/0xE1)
    // leaves stale extA_/extB_ values that corrupt the next process's
    // argument counts and selector indices.
    extA_ = 0;
    extB_ = 0;

    if (context.isNil()) {
        return false;
    }

    if (!context.isObject()) {
        return false;
    }

    // Verify context is not an unrelocated pointer from old image base
    {
        const uint64_t OLD_IMAGE_BASE = 0x10000000000ULL;
        uint64_t ctxAddr = context.rawBits() & ~7ULL;
        if (context.isObject() && ctxAddr >= OLD_IMAGE_BASE && ctxAddr < OLD_IMAGE_BASE * 2) {
            stopVM("Unrelocated context pointer in executeFromContext — ImageLoader bug");
            return false;
        }
    }

    // Validate the context pointer is in valid heap memory
    if (!memory_.isValidPointer(context)) {
        return false;
    }

    // Context layout:
    // slot 0: sender
    // slot 1: pc (1-based byte offset into method bytes)
    // slot 2: stackp (index of top of stack within context, 0 means empty)
    // slot 3: method
    // slot 4: closureOrNil
    // slot 5: receiver
    // slot 6+: temps and stack values

    ObjectHeader* ctxHeader = context.asObjectPtr();
    size_t slotCount = ctxHeader->slotCount();

    if (slotCount < 6) {
        return false;
    }

    method_ = memory_.fetchPointer(3, context);
    closure_ = memory_.fetchPointer(4, context);  // closureOrNil
    receiver_ = memory_.fetchPointer(5, context);

    // Verify no unrelocated pointers from old image base remain
    {
        const uint64_t OLD_IMAGE_BASE = 0x10000000000ULL;
        auto checkUnrelocated = [&](Oop o, const char* name) {
            if (o.isObject()) {
                uint64_t addr = o.rawBits() & ~7ULL;
                if (addr >= OLD_IMAGE_BASE && addr < OLD_IMAGE_BASE * 2) {
                    fprintf(stderr, "[VM] Unrelocated %s pointer 0x%llx — ImageLoader bug\n",
                            name, (unsigned long long)o.rawBits());
                    stopVM("Unrelocated pointer in context slots — ImageLoader bug");
                }
            }
        };
        checkUnrelocated(method_, "method");
        checkUnrelocated(receiver_, "receiver");
        Oop sender = memory_.fetchPointer(0, context);
        checkUnrelocated(sender, "sender");
    }

    activeContext_ = context;  // Track for sender chain on return

    // Set homeMethod_ for literal access
    // For CompiledMethods, homeMethod_ = method_
    // For CompiledBlocks, find the home method through the closure's outer context chain
    homeMethod_ = method_;
    if (method_.isObject()) {
        if (!memory_.isValidPointer(method_)) return false;
        ObjectHeader* methodHdr = method_.asObjectPtr();
        uint32_t methodClsIdx = methodHdr->classIndex();

        if (methodClsIdx == compiledMethodClassIndex_) {
            // CompiledMethod - this is the home method
            homeMethod_ = method_;
        } else if (methodClsIdx == compiledBlockClassIndex_) {
            // CompiledBlock - get home method
            // In FullBlockClosure model (Pharo 11+), CompiledBlock layout:
            // slot 0: block header (SmallInteger with numArgs, etc.)
            // slot 1: selector (Symbol)
            // slot 2: home method (CompiledMethod)
            // slot 3+: bytecodes as raw data

            // First try slot 2 which should be the home CompiledMethod
            Oop slot2 = memory_.fetchPointer(2, method_);
            if (slot2.isObject() && slot2.rawBits() > 0x10000) {
                // Additional safety check: ensure the pointer is in valid memory range
                uintptr_t slot2Addr = slot2.rawBits() & ~7ULL;
                uintptr_t oldStart = reinterpret_cast<uintptr_t>(memory_.oldSpaceStart());
                uintptr_t oldEnd = reinterpret_cast<uintptr_t>(memory_.oldSpaceEnd());
                if (slot2Addr >= oldStart && slot2Addr < oldEnd) {
                    ObjectHeader* slot2Hdr = slot2.asObjectPtr();
                    if (slot2Hdr->classIndex() == compiledMethodClassIndex_) {
                        homeMethod_ = slot2;
                    }
                }
            }

            // Fallback: try slot 0 chain (for older formats)
            if (homeMethod_ == method_) {
                Oop homeCandidate = memory_.fetchPointer(0, method_);
                int maxHops = 10;
                while (homeCandidate.isObject() && memory_.isValidPointer(homeCandidate) && maxHops-- > 0) {
                    ObjectHeader* candidateHdr = homeCandidate.asObjectPtr();
                    uint32_t candidateCls = candidateHdr->classIndex();
                    if (candidateCls == compiledMethodClassIndex_) {
                        homeMethod_ = homeCandidate;
                        break;
                    } else if (candidateCls == compiledBlockClassIndex_) {
                        homeCandidate = memory_.fetchPointer(0, homeCandidate);
                    } else {
                        break;
                    }
                }
            }

            // If we couldn't find home method, try the closure chain as fallback
            if (homeMethod_ == method_) {
                Oop closure = memory_.fetchPointer(4, context);
                int maxHops = 10;

                while (closure.isObject() && memory_.isValidPointer(closure) && maxHops-- > 0) {
                    ObjectHeader* closureHdr = closure.asObjectPtr();
                    uint32_t closureCls = closureHdr->classIndex();

                    // FullBlockClosure or BlockClosure layout:
                    // slot 0: outerContext
                    // slot 1: compiledBlock (or startPC for old closures)
                    // slot 2: numArgs
                    Oop blockClosureClass = memory_.specialObject(SpecialObjectIndex::ClassBlockClosure);
                    Oop fullBlockClosureClass = memory_.specialObject(SpecialObjectIndex::ClassFullBlockClosure);
                    uint32_t blockClosureIdx = 0, fullBlockClosureIdx = 0;
                    if (blockClosureClass.isObject()) {
                        blockClosureIdx = blockClosureClass.asObjectPtr()->classIndex();
                    }
                    if (fullBlockClosureClass.isObject()) {
                        fullBlockClosureIdx = fullBlockClosureClass.asObjectPtr()->classIndex();
                    }
                    bool isBlockClosure = (closureCls == blockClosureIdx && blockClosureIdx != 0) ||
                                          (closureCls == fullBlockClosureIdx && fullBlockClosureIdx != 0) ||
                                          closureCls == 38 || closureCls == 3079 || closureCls == 3213;
                    if (isBlockClosure) {
                        Oop outerContext = memory_.fetchPointer(0, closure);
                        if (outerContext.isNil() || !outerContext.isObject() || !memory_.isValidPointer(outerContext)) {
                            break;
                        }

                        Oop outerMethod = memory_.fetchPointer(3, outerContext);
                        if (!outerMethod.isObject() || !memory_.isValidPointer(outerMethod)) {
                            break;
                        }

                        ObjectHeader* outerMethodHdr = outerMethod.asObjectPtr();
                        uint32_t outerMethodCls = outerMethodHdr->classIndex();

                        if (outerMethodCls == compiledMethodClassIndex_) {
                            // Found home CompiledMethod
                            homeMethod_ = outerMethod;
                            break;
                        } else if (outerMethodCls == compiledBlockClassIndex_) {
                            // Still a block - get closure from outer context
                            closure = memory_.fetchPointer(4, outerContext);
                        } else {
                            break;
                        }
                    } else {
                        break;
                    }
                }
            }
        }
    }

              // << " method=0x" << method_.rawBits()
              // << " receiver=0x" << receiver_.rawBits() << std::dec;

    // If method is a CompiledBlock, we need to check if the context has a closure
    // In modern Pharo, BlockContext/FullBlockClosure contexts may need special handling
    if (method_.isObject() && memory_.isValidPointer(method_)) {
        ObjectHeader* methodHdr = method_.asObjectPtr();
        if (methodHdr->classIndex() == compiledBlockClassIndex_) {
            Oop closure = memory_.fetchPointer(4, context);  // closureOrNil
            if (closure.isObject() && memory_.isValidPointer(closure)) {
                ObjectHeader* closureHdr = closure.asObjectPtr();
                          // << " slots=" << closureHdr->slotCount();
            }
        }
    }

    if (method_.isNil() || !method_.isObject() || !memory_.isValidPointer(method_)) {
        return false;
    }

    // Get method header to calculate bytecode start
    ObjectHeader* methodObj = method_.asObjectPtr();

    Oop methodHeader = memory_.fetchPointer(0, method_);
    // std::cerr; // DEBUG

    if (!methodHeader.isSmallInteger()) {
        // ERROR_LOG("[ERROR] executeFromContext: Invalid method header (not SmallInteger)";
        return false;
    }

    int64_t headerBits = methodHeader.asSmallInteger();
    int numLiterals = headerBits & 0x7FFF;  // bits 0-14 are numLiterals
    int numTemps = (headerBits >> 18) & 0x3F;

    // Bytecode set: only Sista V1 (Pharo 10+) is supported.
    usesSistaV1_ = headerBits < 0;
    if (__builtin_expect(!usesSistaV1_, 0)) {
        fprintf(stderr, "[VM] V3PlusClosures bytecode set not supported\n");
        return false;
    }

    uint8_t* methodBytes = methodObj->bytes();
    size_t bytecodeStart = (1 + numLiterals) * 8;

    // Calculate bytecode end - CompiledMethod format encodes unused bytes
    size_t totalBytes = methodObj->byteSize();
    bytecodeEnd_ = methodBytes + totalBytes;

    // Get the saved PC from the context
    // In Pharo, PC is 1-based byte offset from start of method bytes (absolute, includes header+literals)
    // Initial PC = (1 + numLiterals) * 8 + 1 = bytecodeStart + 1
    Oop savedPC = memory_.fetchPointer(1, context);

    // Check for HasBeenReturnedFrom sentinel: SmallInteger(-1) means this context
    // has already returned and cannot be resumed. Send cannotReturn: per spec.
    if (savedPC.isSmallInteger() && savedPC.asSmallInteger() == -1) {
        // Context has been returned from — cannot resume.
        // Push the context as receiver and the return value as argument,
        // then send cannotReturn: to trigger proper error handling.
        activeContext_ = context;
        stackPointer_ = framePointer_ + 1;
        push(context);
        push(memory_.specialObject(SpecialObjectIndex::NilObject));
        sendSelector(selectors_.cannotReturn, 1);
        return true;  // context was activated (cannotReturn: handler will run)
    }

    int64_t pcOffset = 0;
    if (savedPC.isSmallInteger()) {
        pcOffset = savedPC.asSmallInteger();
        if (pcOffset > 0) {
            // Validate: PC must be within bytecode range [bytecodeStart+1, totalBytes+1]
            // (since PC is 1-based, valid range for 0-based is [bytecodeStart, totalBytes-1])
            size_t absOffset = static_cast<size_t>(pcOffset - 1);  // Convert to 0-based
            if (absOffset >= bytecodeStart && absOffset < totalBytes) {
                instructionPointer_ = methodBytes + absOffset;
            } else {
                // Invalid PC - reset to start of bytecodes
                instructionPointer_ = methodBytes + bytecodeStart;
                if (instructionPointer_[0] == 0xF8) {
                    instructionPointer_ += 3;
                }
            }
        } else {
            instructionPointer_ = methodBytes + bytecodeStart;
            // Skip past callPrimitive if at start
            if (instructionPointer_[0] == 0xF8) {
                instructionPointer_ += 3;
            }
        }
    } else {
        instructionPointer_ = methodBytes + bytecodeStart;
        // Skip past callPrimitive if at start
        if (instructionPointer_[0] == 0xF8) {
            instructionPointer_ += 3;
        }
    }

    // Get saved stackp - in Pharo, stackp is the 1-based index into the temp/stack area
    // stackp = 0 means empty (no temps/stack), stackp = 1 means 1 item, etc.
    // The temps/stack start at slot 6 (after fixed fields: sender, pc, stackp, method, closure, receiver)
    static const int ContextFixedFields = 6;

    Oop savedStackp = memory_.fetchPointer(2, context);
    int stackp = 0;
    if (savedStackp.isSmallInteger()) {
        stackp = static_cast<int>(savedStackp.asSmallInteger());
    }

    // Push receiver first - this establishes our frame
    push(receiver_);
    framePointer_ = stackPointer_ - 1;


    // Now restore the context's saved stack
    // stackp indicates how many slots are valid in the temp/stack area (1-based count)
    // So if stackp=1, there's 1 valid item at slot 6
    // If stackp=5, there are 5 valid items at slots 6,7,8,9,10
    //
    // CRITICAL: We must ensure at least numTemps slots are on the inline stack.
    // The method's temp area (args + locals) occupies FP[1..numTemps].
    // The expression stack sits ABOVE the temps at FP[numTemps+1..].
    // If stackp < numTemps, the context only stored the "live" portion,
    // but we must still reserve space for all temps so that pops from the
    // expression stack don't underflow into the temp area.
    {
        // First, push the saved items from the context
        int numSaved = (stackp > 0 && stackp < 1000) ? stackp : 0;
        for (int i = 0; i < numSaved; i++) {
            Oop item = memory_.fetchPointer(ContextFixedFields + i, context);
            push(item);
        }
        // If fewer items were saved than numTemps, pad with nil
        // so that the temp area is fully allocated on the inline stack
        for (int i = numSaved; i < numTemps; i++) {
            push(memory_.nil());
        }
    }

    // For block contexts with a closure, restore copied values from the closure.
    // This is critical when the context's stackp was reduced (e.g., by Context>>jump
    // popping from the block context) but the block's bytecodes still expect copied
    // values in their temp positions. Copied values in FullBlockClosure are immutable
    // captures, so the closure is the canonical source. This matches what
    // Context>>privRefresh does in Smalltalk.
    {
        Oop closureOop = memory_.fetchPointer(4, context); // slot 4 = closureOrNil
        if (closureOop.isObject() && !closureOop.isNil()) {
            size_t closureSlots = memory_.slotCountOf(closureOop);

            // FullBlockClosure: slot 0=outerContext, 1=compiledBlock(obj), 2=numArgs, 3=receiver, 4+=copied
            // BlockClosure:     slot 0=outerContext, 1=startpc(smi),      2=numArgs, 3+=copied
            Oop slot1 = memory_.fetchPointer(1, closureOop);
            int firstCopiedSlot = slot1.isObject() ? 4 : 3;
            int numCopied = static_cast<int>(closureSlots) - firstCopiedSlot;
            if (numCopied < 0) numCopied = 0;

            if (numCopied > 0) {
                Oop numArgsOop = memory_.fetchPointer(2, closureOop);
                int closureNumArgs = numArgsOop.isSmallInteger()
                    ? static_cast<int>(numArgsOop.asSmallInteger()) : 0;

                // Restore copied values from closure ONLY for positions beyond
                // the context's saved stackp. Positions within stackp were already
                // restored from the context's own slots (which may have been
                // modified during execution, e.g. firstTimeThrough := false).
                // Positions beyond stackp were padded with nil above, but should
                // have the closure's captured values instead.
                for (int i = 0; i < numCopied; i++) {
                    int tempIndex = closureNumArgs + i;
                    if (tempIndex < numTemps && tempIndex >= stackp) {
                        Oop copiedValue = memory_.fetchPointer(firstCopiedSlot + i, closureOop);
                        framePointer_[1 + tempIndex] = copiedValue;
                    }
                }
            }
        }
    }

    argCount_ = 0;  // We're resuming a context, not calling a method

    // Only initialize selectors once (not on every executeFromContext call)
    if (!selectorsInitialized_) {
        initializeSelectors();
        selectorsInitialized_ = true;
    }
    running_ = true;

    return true;
}

// ===== PRIMITIVE SUPPORT =====

void Interpreter::primitiveSuccess(Oop result) {
    primitiveFailed_ = false;
    // Pop args and receiver, push result
    popN(argCount_ + 1);
    push(result);
}

void Interpreter::primitiveFail() {
    primitiveFailed_ = true;
}

void Interpreter::initializePrimitives() {
    // Clear all entries first
    for (auto& entry : primitiveTable_) {
        entry = nullptr;
    }

    // Load primitive table from VMMaker-generated source
    // This ensures the table matches what the Pharo image expects
    #include "../ios/generated_primitives.inc"

    // Debug print primitive (slot 255, unused by standard Pharo image)
    primitiveTable_[255] = &Interpreter::primitiveDebugPrint;

    // Primitives 256-519: In Sista V1 / Spur, the callPrimitive bytecode encodes
    // "quick primitives" that return constants (256-263) or instance variables (264+).
    // These are NOT external/named primitives.
    // External/named primitives use primitive 117 (primitiveExternalCall) which is
    // in the primitive table and reads the method's literals for the plugin spec.
    // Do NOT override 256-519 in the table - they're handled by quick primitive code.

    // NOTE: The generated file maps VMMaker primitive names to C++ method names.
    // If a primitive method doesn't exist, it will cause a compile error here,
    // which is intentional - it means we need to implement that primitive.
    //
    // The old hand-written table had many errors (wrong primitive numbers).
    // Using the generated table ensures correctness.

    // ByteArray data access primitives (600-629)
    // Read typed data from byte-format objects (ByteArray, etc.)
    primitiveTable_[600] = &Interpreter::primitiveBytesBoolean8Read;   // boolean8AtOffset:
    primitiveTable_[601] = &Interpreter::primitiveBytesUint8Read;      // uint8AtOffset:
    primitiveTable_[602] = &Interpreter::primitiveBytesInt8Read;       // int8AtOffset:
    primitiveTable_[603] = &Interpreter::primitiveBytesUint16Read;     // uint16AtOffset:
    primitiveTable_[604] = &Interpreter::primitiveBytesInt16Read;      // int16AtOffset:
    primitiveTable_[605] = &Interpreter::primitiveBytesUint32Read;     // uint32AtOffset:
    primitiveTable_[606] = &Interpreter::primitiveBytesInt32Read;      // int32AtOffset:
    primitiveTable_[607] = &Interpreter::primitiveBytesUint64Read;     // uint64AtOffset:
    primitiveTable_[608] = &Interpreter::primitiveBytesInt64Read;      // int64AtOffset:
    primitiveTable_[609] = &Interpreter::primitiveBytesPointerRead;    // pointerAtOffset:
    primitiveTable_[610] = &Interpreter::primitiveBytesChar8Read;      // char8AtOffset:
    primitiveTable_[611] = &Interpreter::primitiveBytesChar16Read;     // char16AtOffset:
    primitiveTable_[612] = &Interpreter::primitiveBytesChar32Read;     // char32AtOffset:
    primitiveTable_[613] = &Interpreter::primitiveFloat32Read;         // float32AtOffset:
    primitiveTable_[614] = &Interpreter::primitiveFloat64Read;         // float64AtOffset:
    // Write typed data into byte-format objects (with immutability check)
    primitiveTable_[615] = &Interpreter::primitiveBytesBoolean8Write;  // boolean8AtOffset:put:
    primitiveTable_[616] = &Interpreter::primitiveBytesUint8Write;     // uint8AtOffset:put:
    primitiveTable_[617] = &Interpreter::primitiveBytesInt8Write;      // int8AtOffset:put:
    primitiveTable_[618] = &Interpreter::primitiveBytesUint16Write;    // uint16AtOffset:put:
    primitiveTable_[619] = &Interpreter::primitiveBytesInt16Write;     // int16AtOffset:put:
    primitiveTable_[620] = &Interpreter::primitiveBytesUint32Write;    // uint32AtOffset:put:
    primitiveTable_[621] = &Interpreter::primitiveBytesInt32Write;     // int32AtOffset:put:
    primitiveTable_[622] = &Interpreter::primitiveBytesUint64Write;    // uint64AtOffset:put:
    primitiveTable_[623] = &Interpreter::primitiveBytesInt64Write;     // int64AtOffset:put:
    primitiveTable_[624] = &Interpreter::primitiveBytesPointerWrite;   // pointerAtOffset:put:
    primitiveTable_[625] = &Interpreter::primitiveBytesChar8Write;     // char8AtOffset:put:
    primitiveTable_[626] = &Interpreter::primitiveBytesChar16Write;    // char16AtOffset:put:
    primitiveTable_[627] = &Interpreter::primitiveBytesChar32Write;    // char32AtOffset:put:
    primitiveTable_[628] = &Interpreter::primitiveStoreFloat32IntoBytes; // float32AtOffset:put:
    primitiveTable_[629] = &Interpreter::primitiveStoreFloat64IntoBytes; // float64AtOffset:put:

    // ExternalAddress read primitives (numbered 631-639)
    // These read from external memory pointed to by ExternalAddress.
    // Used by FFI for output parameter dereferencing (void**, int*, etc.)
    primitiveTable_[631] = &Interpreter::primitiveExternalUint8Read;   // uint8AtOffset:
    primitiveTable_[633] = &Interpreter::primitiveExternalUint16Read;  // uint16AtOffset:
    primitiveTable_[635] = &Interpreter::primitiveExternalUint32Read;  // uint32AtOffset:
    primitiveTable_[636] = &Interpreter::primitiveExternalInt32Read;   // int32AtOffset:
    primitiveTable_[639] = &Interpreter::primitiveExternalPointerRead; // pointerAtOffset:

    // ExternalAddress write primitives (write to external memory)
    primitiveTable_[646] = &Interpreter::primitiveExternalUint8Write;    // uint8AtOffset:put:
    primitiveTable_[648] = &Interpreter::primitiveExternalUint16Write;   // uint16AtOffset:put:
    primitiveTable_[650] = &Interpreter::primitiveExternalUint32Write;   // uint32AtOffset:put:
    primitiveTable_[651] = &Interpreter::primitiveExternalInt32Write;    // int32AtOffset:put:
    primitiveTable_[652] = &Interpreter::primitiveExternalUint64Write;   // uint64AtOffset:put:
    primitiveTable_[654] = &Interpreter::primitiveExternalPointerWrite;  // pointerAtOffset:put:

    // Also initialize named primitives for module-based lookup
    initializeNamedPrimitives();

    // Initialize FFI early to register SDL2 stubs before image tries to use them
    // This makes OSWindow think SDL2 is available, so it starts InputEventSensor
    ffi::initializeFFI();

    // Initialize B2DPlugin (BalloonEngine) for vector graphics rendering
    initializeB2DPlugin(this);

    // Initialize JPEG plugins
    initializeJPEGReaderPlugin(this);
    initializeJPEGReadWriter2Plugin(this);

#if PHARO_WITH_CRYPTO
    // Initialize crypto plugins (guarded by PHARO_WITH_CRYPTO build option)
    initializeDSAPrims(this);
    initializeSqueakSSL(this);
#endif

    // Initialize SocketPlugin (TCP sockets with I/O monitor thread)
    initializeSocketPlugin(this);
}

void Interpreter::registerNamedPrimitive(const std::string& module, const std::string& name, PrimitiveFunc func) {
    std::string key = module + ":" + name;
    namedPrimitives_[key] = func;
}

void Interpreter::registerNamedPrimitive(const std::string& module, const std::string& name, ExternalPrimFunc func) {
    std::string key = module + ":" + name;
    externalPrimitives_[key] = func;
}

PrimitiveResult Interpreter::callExternalPrimitive(ExternalPrimFunc fn) {
    resetProxyFailure();
    fn();
    bool failed = proxyFailed();
    if (failed) {
        return PrimitiveResult::Failure;
    }
    return PrimitiveResult::Success;
}

void Interpreter::initializeNamedPrimitives() {
    // Register iOS-specific primitives by name
    // These can be called via <primitive: 'name' module: 'iOSPlugin'>

    // Event primitives - register under all module names the image might use
    registerNamedPrimitive("iOSPlugin", "primitiveGetNextEvent", &Interpreter::primitiveGetNextEvent);
    registerNamedPrimitive("iOSPlugin", "primitiveInputSemaphore", &Interpreter::primitiveInputSemaphore2);
    registerNamedPrimitive("SecurityPlugin", "primitiveGetNextEvent", &Interpreter::primitiveGetNextEvent);
    registerNamedPrimitive("SecurityPlugin", "primitiveInputSemaphore", &Interpreter::primitiveInputSemaphore2);
    registerNamedPrimitive("", "primitiveGetNextEvent", &Interpreter::primitiveGetNextEvent);
    registerNamedPrimitive("", "primitiveInputSemaphore", &Interpreter::primitiveInputSemaphore2);

    // Display primitives
    registerNamedPrimitive("iOSPlugin", "primitiveShowDisplayRect", &Interpreter::primitiveShowDisplayRect);
    registerNamedPrimitive("iOSPlugin", "primitiveScreenSize", &Interpreter::primitiveScreenSize);
    registerNamedPrimitive("iOSPlugin", "primitiveScreenDepth", &Interpreter::primitiveScreenDepth);

    // Also register under SqueakPlugin/SurfacePlugin for compatibility
    registerNamedPrimitive("SqueakPlugin", "primitiveGetNextEvent", &Interpreter::primitiveGetNextEvent);
    registerNamedPrimitive("SurfacePlugin", "primitiveShowDisplayRect", &Interpreter::primitiveShowDisplayRect);
    registerNamedPrimitive("SurfacePlugin", "primitiveCreateManualSurface", &Interpreter::primitiveCreateManualSurface);
    registerNamedPrimitive("SurfacePlugin", "primitiveDestroyManualSurface", &Interpreter::primitiveDestroyManualSurface);
    registerNamedPrimitive("SurfacePlugin", "primitiveSetManualSurfacePointer", &Interpreter::primitiveSetManualSurfacePointer);
    registerNamedPrimitive("SurfacePlugin", "primitiveRegisterSurface", &Interpreter::primitiveRegisterSurface);
    registerNamedPrimitive("SurfacePlugin", "primitiveUnregisterSurface", &Interpreter::primitiveUnregisterSurface);

    // VM info primitives (called with empty module name)
    registerNamedPrimitive("", "primitiveImageFormatVersion", &Interpreter::primitiveImageFormatVersion);

    // Display primitives (called with empty module name)
    registerNamedPrimitive("", "primitiveForceDisplayUpdate", &Interpreter::primitiveForceDisplayUpdate);
    registerNamedPrimitive("", "primitiveShowDisplayRect", &Interpreter::primitiveShowDisplayRect);

    // BitBlt plugin
    registerNamedPrimitive("BitBltPlugin", "primitiveCopyBits", &Interpreter::primitiveCopyBits);
    registerNamedPrimitive("BitBltPlugin", "primitiveDrawLoop", &Interpreter::primitiveDrawLoop);
    registerNamedPrimitive("BitBltPlugin", "primitiveWarpBits", &Interpreter::primitiveWarpBits);

    // FloatArrayPlugin
    registerNamedPrimitive("FloatArrayPlugin", "primitiveAt", &Interpreter::primitiveFloatArrayAt);
    registerNamedPrimitive("FloatArrayPlugin", "primitiveAtPut", &Interpreter::primitiveFloatArrayAtPut);

    // MiscPrimitivePlugin
    registerNamedPrimitive("MiscPrimitivePlugin", "primitiveStringHash", &Interpreter::primitiveStringHashInitialHash);
    registerNamedPrimitive("MiscPrimitivePlugin", "primitiveFindSubstring", &Interpreter::primitiveFindSubstring);
    registerNamedPrimitive("MiscPrimitivePlugin", "primitiveIndexOfAsciiInString", &Interpreter::primitiveIndexOfAscii);
    registerNamedPrimitive("MiscPrimitivePlugin", "primitiveTranslateStringWithTable", &Interpreter::primitiveTranslateStringWithTable);
    registerNamedPrimitive("MiscPrimitivePlugin", "primitiveCompareString", &Interpreter::primitiveCompareStringCollated);
    registerNamedPrimitive("MiscPrimitivePlugin", "primitiveFindFirstInString", &Interpreter::primitiveFindFirstInString);
    registerNamedPrimitive("MiscPrimitivePlugin", "primitiveDecompressFromByteArray", &Interpreter::primitiveDecompressFromByteArray);
    registerNamedPrimitive("MiscPrimitivePlugin", "primitiveCompressToByteArray", &Interpreter::primitiveCompressToByteArray);

    // FFI Module/Symbol Loading - these are VM built-in primitives used by UFFI
    // They are called without a module (empty module name) because they're VM internals
    registerNamedPrimitive("", "primitiveGetCurrentWorkingDirectory", &Interpreter::primitiveGetCurrentWorkingDirectory);
    registerNamedPrimitive("", "primitiveLoadSymbolFromModule", &Interpreter::primitiveLoadSymbolFromModule);
    registerNamedPrimitive("", "primitiveLoadModule", &Interpreter::primitiveLoadModule);
    // Also register with explicit module name for compatibility
    registerNamedPrimitive("SqueakFFIPrims", "primitiveLoadSymbolFromModule", &Interpreter::primitiveLoadSymbolFromModule);
    registerNamedPrimitive("SqueakFFIPrims", "primitiveLoadModule", &Interpreter::primitiveLoadModule);

    // FFI memory access primitives (required by TFFIBackend)
    registerNamedPrimitive("", "primitiveFFIAllocate", &Interpreter::primitiveFFIAllocate);
    registerNamedPrimitive("", "primitiveFFIFree", &Interpreter::primitiveFFIFree);
    registerNamedPrimitive("", "primitiveFFIIntegerAt", &Interpreter::primitiveFFIIntegerAt);
    registerNamedPrimitive("", "primitiveFFIIntegerAtPut", &Interpreter::primitiveFFIIntegerAtPut);
    registerNamedPrimitive("SqueakFFIPrims", "primitiveFFIAllocate", &Interpreter::primitiveFFIAllocate);
    registerNamedPrimitive("SqueakFFIPrims", "primitiveFFIFree", &Interpreter::primitiveFFIFree);
    registerNamedPrimitive("SqueakFFIPrims", "primitiveFFIIntegerAt", &Interpreter::primitiveFFIIntegerAt);
    registerNamedPrimitive("SqueakFFIPrims", "primitiveFFIIntegerAtPut", &Interpreter::primitiveFFIIntegerAtPut);

    // FFI address primitives
    registerNamedPrimitive("", "primitiveGetAddressOfOOP", &Interpreter::primitiveGetAddressOfOOP);
    registerNamedPrimitive("SqueakFFIPrims", "primitiveGetAddressOfOOP", &Interpreter::primitiveGetAddressOfOOP);

    // FilePlugin - file I/O primitives
    registerNamedPrimitive("FilePlugin", "primitiveFileStdioHandles", &Interpreter::primitiveFileStdioHandles);
    registerNamedPrimitive("FilePlugin", "primitiveFileOpen", &Interpreter::primitiveFileOpen);
    registerNamedPrimitive("FilePlugin", "primitiveFileClose", &Interpreter::primitiveFileClose);
    registerNamedPrimitive("FilePlugin", "primitiveFileRead", &Interpreter::primitiveFileRead);
    registerNamedPrimitive("FilePlugin", "primitiveFileWrite", &Interpreter::primitiveFileWrite);
    registerNamedPrimitive("FilePlugin", "primitiveFileAtEnd", &Interpreter::primitiveFileAtEnd);
    registerNamedPrimitive("FilePlugin", "primitiveFileGetPosition", &Interpreter::primitiveFileGetPosition);
    registerNamedPrimitive("FilePlugin", "primitiveFileSetPosition", &Interpreter::primitiveFileSetPosition);
    registerNamedPrimitive("FilePlugin", "primitiveFileSize", &Interpreter::primitiveFileSize);
    registerNamedPrimitive("FilePlugin", "primitiveFileFlush", &Interpreter::primitiveFileFlush);
    registerNamedPrimitive("FilePlugin", "primitiveFileTruncate", &Interpreter::primitiveFileTruncate);
    registerNamedPrimitive("FilePlugin", "primitiveFileDelete", &Interpreter::primitiveFileDelete);
    registerNamedPrimitive("FilePlugin", "primitiveFileRename", &Interpreter::primitiveFileRename);
    registerNamedPrimitive("FilePlugin", "primitiveFileDescriptorType", &Interpreter::primitiveFileDescriptorType);
    registerNamedPrimitive("FilePlugin", "primitiveDirectoryDelimitor", &Interpreter::primitiveDirectoryDelimitor);
    registerNamedPrimitive("FilePlugin", "primitiveDirectoryCreate", &Interpreter::primitiveDirectoryCreate);
    registerNamedPrimitive("FilePlugin", "primitiveDirectoryLookup", &Interpreter::primitiveDirectoryLookup);
    registerNamedPrimitive("FilePlugin", "primitiveDirectoryDelete", &Interpreter::primitiveDirectoryDelete);
    registerNamedPrimitive("FilePlugin", "primitiveDirectoryGetMacTypeAndCreator", &Interpreter::primitiveDirectoryGetMacTypeAndCreator);
    registerNamedPrimitive("FilePlugin", "primitiveGetHomeDirectory", &Interpreter::primitiveGetHomeDirectory);
    registerNamedPrimitive("FilePlugin", "primitiveGetTempDirectory", &Interpreter::primitiveGetTempDirectory);

    // FileAttributesPlugin
    registerNamedPrimitive("FileAttributesPlugin", "primitiveFileMasks", &Interpreter::primitiveFileMasks);
    registerNamedPrimitive("FileAttributesPlugin", "primitiveFileAttribute", &Interpreter::primitiveFileAttribute);
    registerNamedPrimitive("FileAttributesPlugin", "primitiveFileExists", &Interpreter::primitiveFileExists);
    registerNamedPrimitive("FileAttributesPlugin", "primitiveOpendir", &Interpreter::primitiveOpendir);
    registerNamedPrimitive("FileAttributesPlugin", "primitiveReaddir", &Interpreter::primitiveReaddir);
    registerNamedPrimitive("FileAttributesPlugin", "primitiveClosedir", &Interpreter::primitiveClosedir);
    registerNamedPrimitive("FileAttributesPlugin", "primitiveRewinddir", &Interpreter::primitiveRewinddir);
    registerNamedPrimitive("FileAttributesPlugin", "primitiveChangeMode", &Interpreter::primitiveChangeMode);
    registerNamedPrimitive("FileAttributesPlugin", "primitiveChangeOwner", &Interpreter::primitiveChangeOwner);
    registerNamedPrimitive("FileAttributesPlugin", "primitiveSymlinkChangeOwner", &Interpreter::primitiveSymlinkChangeOwner);
    registerNamedPrimitive("FileAttributesPlugin", "primitiveFileAttributes", &Interpreter::primitiveFileAttributes);
    registerNamedPrimitive("FileAttributesPlugin", "primitivePlatToStPath", &Interpreter::primitivePlatToStPath);
    registerNamedPrimitive("FileAttributesPlugin", "primitiveStToPlatPath", &Interpreter::primitiveStToPlatPath);
    registerNamedPrimitive("FileAttributesPlugin", "primitivePathMax", &Interpreter::primitivePathMax);

    // VM info
    registerNamedPrimitive("", "primitiveInterpreterSourceVersion", &Interpreter::primitiveInterpreterSourceVersion);

    // Environment access
    registerNamedPrimitive("", "primitiveGetenv", &Interpreter::primitiveGetenv);

    // SDL2 display detection - CRITICAL for OSSDL2Driver to start its event loop
    registerNamedPrimitive("", "isVMDisplayUsingSDL2", &Interpreter::primitiveIsVMDisplayUsingSDL2);
    registerNamedPrimitive("", "primitiveIsVMDisplayUsingSDL2", &Interpreter::primitiveIsVMDisplayUsingSDL2);
    registerNamedPrimitive("SqueakPlugin", "isVMDisplayUsingSDL2", &Interpreter::primitiveIsVMDisplayUsingSDL2);
    registerNamedPrimitive("SDL2DisplayPlugin", "primitiveHasDisplayPlugin", &Interpreter::primitiveIsVMDisplayUsingSDL2);

    // High-resolution clock (used by Time class>>primNanoClock)
    registerNamedPrimitive("", "primitiveHighResClock", &Interpreter::primitiveHighResClock);

    // SecurityPlugin primitives
    registerNamedPrimitive("SecurityPlugin", "primitiveCanWriteImage", &Interpreter::primitiveCanWriteImage);
    registerNamedPrimitive("SecurityPlugin", "primitiveDisableImageWrite", &Interpreter::primitiveDisableImageWrite);
    registerNamedPrimitive("SecurityPlugin", "primitiveGetSecureUserDirectory", &Interpreter::primitiveGetSecureUserDirectory);
    registerNamedPrimitive("SecurityPlugin", "primitiveGetUntrustedUserDirectory", &Interpreter::primitiveGetUntrustedUserDirectory);

    // LocalePlugin primitives
    registerNamedPrimitive("LocalePlugin", "primitiveLanguage", &Interpreter::primitiveLocaleLanguage);
    registerNamedPrimitive("LocalePlugin", "primitiveCountry", &Interpreter::primitiveLocaleCountry);
    registerNamedPrimitive("LocalePlugin", "primitiveCurrencySymbol", &Interpreter::primitiveLocaleCurrencySymbol);
    registerNamedPrimitive("LocalePlugin", "primitiveDecimalSeparator", &Interpreter::primitiveLocaleDecimalSeparator);
    registerNamedPrimitive("LocalePlugin", "primitiveDigitGroupingSeparator", &Interpreter::primitiveLocaleThousandsSeparator);
    registerNamedPrimitive("LocalePlugin", "primitiveDateFormat", &Interpreter::primitiveLocaleDateFormat);
    registerNamedPrimitive("LocalePlugin", "primitiveTimeFormat", &Interpreter::primitiveLocaleTimeFormat);
    registerNamedPrimitive("LocalePlugin", "primitiveTimezone", &Interpreter::primitiveLocaleTimezone);
    registerNamedPrimitive("LocalePlugin", "primitiveTimezoneOffset", &Interpreter::primitiveLocaleTimezoneOffset);
    registerNamedPrimitive("LocalePlugin", "primitiveDaylightSavingTimeActive", &Interpreter::primitiveLocaleDaylightSaving);

    // DateAndTime>>now uses this named primitive (module: '')
    registerNamedPrimitive("", "primitiveUtcWithOffset", &Interpreter::primitiveUtcWithOffset);

    // LargeIntegers plugin primitives
    registerNamedPrimitive("LargeIntegers", "primDigitMultiplyNegative", &Interpreter::primDigitMultiplyNegative);
    registerNamedPrimitive("LargeIntegers", "primDigitAdd", &Interpreter::primDigitAddLargeIntegers);
    registerNamedPrimitive("LargeIntegers", "primNormalizePositive", &Interpreter::primNormalizePositive);
    registerNamedPrimitive("LargeIntegers", "primNormalizeNegative", &Interpreter::primNormalizeNegative);
    registerNamedPrimitive("LargeIntegers", "primDigitDivNegative", &Interpreter::primDigitDivNegative);
    registerNamedPrimitive("LargeIntegers", "primDigitSubtract", &Interpreter::primDigitSubtractLargeIntegers);
    registerNamedPrimitive("LargeIntegers", "primDigitBitAnd", &Interpreter::primitiveBitAndLargeIntegers);
    registerNamedPrimitive("LargeIntegers", "primDigitBitOr", &Interpreter::primitiveBitOrLargeIntegers);
    registerNamedPrimitive("LargeIntegers", "primDigitBitXor", &Interpreter::primitiveBitXorLargeIntegers);
    registerNamedPrimitive("LargeIntegers", "primDigitBitShiftMagnitude", &Interpreter::primitiveBitShiftLargeIntegers);
    registerNamedPrimitive("LargeIntegers", "primDigitCompare", &Interpreter::primDigitCompare);
    registerNamedPrimitive("LargeIntegers", "primAnyBitFromTo", &Interpreter::primAnyBitFromTo);
    registerNamedPrimitive("LargeIntegers", "primMontgomeryDigitLength", &Interpreter::primMontgomeryDigitLength);
    registerNamedPrimitive("LargeIntegers", "primMontgomeryTimesModulo", &Interpreter::primMontgomeryTimesModulo);

    // CoreMotionPlugin — accelerometer, gyroscope, magnetometer, attitude
    registerNamedPrimitive("CoreMotionPlugin", "primitiveMotionData", &Interpreter::primitiveMotionData);
    registerNamedPrimitive("CoreMotionPlugin", "primitiveMotionAvailable", &Interpreter::primitiveMotionAvailable);
    registerNamedPrimitive("CoreMotionPlugin", "primitiveMotionStart", &Interpreter::primitiveMotionStart);
    registerNamedPrimitive("CoreMotionPlugin", "primitiveMotionStop", &Interpreter::primitiveMotionStop);

    // SDL2 input semaphore - enables SDL2 event polling
    // The image calls this to register a semaphore for SDL2 event notification
    registerNamedPrimitive("", "primitiveSetVMSDL2Input:", &Interpreter::primitiveSetVMSDL2Input);
    registerNamedPrimitive("SDL_Event", "primitiveSetVMSDL2Input:", &Interpreter::primitiveSetVMSDL2Input);

    // SocketPlugin stubs (NetNameResolver DNS + UUID generation)
    registerNamedPrimitive("SocketPlugin", "primitiveInitializeNetwork", &Interpreter::primitiveInitializeNetwork);
    registerNamedPrimitive("SocketPlugin", "primitiveResolverStatus", &Interpreter::primitiveResolverStatus);
    registerNamedPrimitive("SocketPlugin", "primitiveResolverLocalAddress", &Interpreter::primitiveResolverLocalAddress);
    registerNamedPrimitive("SocketPlugin", "primitiveResolverStartNameLookup", &Interpreter::primitiveResolverStartNameLookup);
    registerNamedPrimitive("SocketPlugin", "primitiveResolverNameLookupResult", &Interpreter::primitiveResolverNameLookupResult);
    registerNamedPrimitive("SocketPlugin", "primitiveResolverAbortLookup", &Interpreter::primitiveResolverAbortLookup);

    // UUIDPlugin
    registerNamedPrimitive("UUIDPlugin", "primitiveMakeUUID", &Interpreter::primitiveMakeUUID);

    // ThreadedFFI (TFFI) primitives - used by TFFIBackend in Pharo 13+
    // These must be registered under "" (empty module) because the image looks them up that way.
    registerNamedPrimitive("", "primitiveFillBasicType", &Interpreter::primitiveFillBasicType);
    registerNamedPrimitive("", "primitiveTypeByteSize", &Interpreter::primitiveTypeByteSize);
    registerNamedPrimitive("", "primitiveDefineFunction", &Interpreter::primitiveDefineFunction);
    registerNamedPrimitive("", "primitiveFreeDefinition", &Interpreter::primitiveFreeDefinition);
    registerNamedPrimitive("", "primitiveDefineVariadicFunction", &Interpreter::primitiveDefineVariadicFunction);
    registerNamedPrimitive("", "primitiveGetSameThreadRunnerAddress", &Interpreter::primitiveGetSameThreadRunnerAddress);
    registerNamedPrimitive("", "primitiveSameThreadCallout", &Interpreter::primitiveSameThreadCallout);
    registerNamedPrimitive("", "primitiveSameThreadCallbackInvoke", &Interpreter::primitiveSameThreadCallout);
    registerNamedPrimitive("", "primitiveCopyFromTo", &Interpreter::primitiveCopyFromTo);
    registerNamedPrimitive("", "primitiveInitializeStructType", &Interpreter::primitiveInitializeStructType);
    registerNamedPrimitive("", "primitiveFreeStruct", &Interpreter::primitiveFreeStruct);
    registerNamedPrimitive("", "primitiveStructByteSize", &Interpreter::primitiveStructByteSize);
    registerNamedPrimitive("", "primitiveInitilizeCallbacks", &Interpreter::primitiveInitilizeCallbacks);
    registerNamedPrimitive("", "primitiveReadNextCallback", &Interpreter::primitiveReadNextCallback);
    registerNamedPrimitive("", "primitiveRegisterCallback", &Interpreter::primitiveRegisterCallback);
    registerNamedPrimitive("", "primitiveUnregisterCallback", &Interpreter::primitiveUnregisterCallback);
    registerNamedPrimitive("", "primitiveCallbackReturn", &Interpreter::primitiveCallbackReturn);
}

PrimitiveResult Interpreter::executePrimitive(int primitiveIndex, int argCount) {
    // Temporary debug: count prim 63 calls
    static int prim63count = 0;
    if (primitiveIndex == 63) {
        prim63count++;
        if (prim63count <= 5 || (prim63count % 10000 == 0)) {
            fprintf(stderr, "[EXEC-P63] call #%d argCount=%d\n", prim63count, argCount);
        }
    }

    // Named primitives (index >= 32768) are dispatched via registerNamedPrimitive()
    // during initializePrimitiveTable(). If we see one here, it means it wasn't
    // registered — fail so the method body executes as fallback.
    if (primitiveIndex >= 32768) {
        return PrimitiveResult::Failure;
    }

    // Check primitive table bounds first
    if (primitiveIndex < 0 || primitiveIndex >= static_cast<int>(primitiveTable_.size())) {
        return PrimitiveResult::Failure;
    }

    // Quick primitives (256-519): return constants or instance variables.
    // In the standard VM, indices 256-519 are ALWAYS quick primitives, never
    // dispatched through the primitive table. Handle them first.
    if (primitiveIndex >= 256 && primitiveIndex <= 519) {
        Oop receiver = stackTop();

        if (__builtin_expect(primitiveIndex >= 264, 1)) {
            // Return instance variable at index (primitiveIndex - 264)
            if (__builtin_expect(!receiver.isObject(), 0)) {
                return PrimitiveResult::Failure;
            }
            size_t instVarIndex = static_cast<size_t>(primitiveIndex - 264);
            // receiver is a known-valid heap object — use unchecked access
            Oop value = memory_.fetchPointerUnchecked(instVarIndex, receiver);
            *(stackPointer_ - 1) = value;  // Replace stack top
            return PrimitiveResult::Success;
        }

        switch (primitiveIndex) {
            case 256:  // return self
                return PrimitiveResult::Success;
            case 257:  // return true
                *(stackPointer_ - 1) = memory_.trueObject();
                return PrimitiveResult::Success;
            case 258:  // return false
                *(stackPointer_ - 1) = memory_.falseObject();
                return PrimitiveResult::Success;
            case 259:  // return nil
                *(stackPointer_ - 1) = memory_.nil();
                return PrimitiveResult::Success;
            case 260:  // return -1
                *(stackPointer_ - 1) = Oop::fromSmallInteger(-1);
                return PrimitiveResult::Success;
            case 261:  // return 0
                *(stackPointer_ - 1) = Oop::fromSmallInteger(0);
                return PrimitiveResult::Success;
            case 262:  // return 1
                *(stackPointer_ - 1) = Oop::fromSmallInteger(1);
                return PrimitiveResult::Success;
            case 263:  // return 2
                *(stackPointer_ - 1) = Oop::fromSmallInteger(2);
                return PrimitiveResult::Success;
            default:
                return PrimitiveResult::Failure;
        }
    }

    // Regular primitives (0-255): dispatch through the primitive table
    {
        PrimitiveFunc prim = primitiveTable_[primitiveIndex];
        if (prim) {
            PrimitiveResult result = (this->*prim)(argCount);
            if (result == PrimitiveResult::Success) {
                lastPrimitiveIndex_ = primitiveIndex;
            }
            return result;
        }
    }

    // No primitive function and not a quick primitive
    // Log interesting unimplemented primitives
    return PrimitiveResult::Failure;
}

Oop Interpreter::activeContext() const {
    // Returns the reified context object for the current frame.
    // Currently returns nil — context materialization is done
    // on-demand in primitiveThisContext() / ensureFrameIsContext().
    return Oop::nil();
}

// ===== GC SUPPORT =====

void Interpreter::prepareForGC() {
    // Convert current frame's raw IP pointers to offsets from method bytes start.
    // This is needed because method objects may move during GC.
    if (method_.isObject() && instructionPointer_) {
        uint8_t* methodBytes = method_.asObjectPtr()->bytes();
        ipOffset_ = instructionPointer_ - methodBytes;
        bytecodeEndOffset_ = bytecodeEnd_ - methodBytes;
        // Save bytecodes around IP for post-GC verification
        gcVerifyBytecodeAtIP_ = *instructionPointer_;
        gcVerifyMethodOop_ = method_.rawBits();
    } else {
        ipOffset_ = 0;
        bytecodeEndOffset_ = 0;
        gcVerifyBytecodeAtIP_ = 0xFF;
        gcVerifyMethodOop_ = 0;
    }

    // Convert saved frames' IPs to offsets
    for (size_t i = 0; i < frameDepth_; ++i) {
        SavedFrame& frame = savedFrames_[i];
        if (frame.savedMethod.isObject() && frame.savedIP) {
            uint8_t* methodBytes = frame.savedMethod.asObjectPtr()->bytes();
            frame.savedIPOffset = frame.savedIP - methodBytes;
            frame.savedBytecodeEndOffset = frame.savedBytecodeEnd - methodBytes;
        } else {
            frame.savedIPOffset = 0;
            frame.savedBytecodeEndOffset = 0;
        }
    }

    // Sync materialized context temps with C++ stack.
    // Materialized contexts are GC roots (scanned via forEachRoot). Their temp
    // slots are snapshots from materialization time and may be stale — e.g., a
    // temp that was nilled on the C++ stack still holds the old value in the
    // context. This causes weak references to survive GC incorrectly because
    // the context keeps the old object marked.
    static constexpr int ContextFixedFields = 6;
    for (size_t i = 0; i < frameDepth_; ++i) {
        SavedFrame& frame = savedFrames_[i];
        Oop matCtx = frame.materializedContext;
        if (!matCtx.isObject() || matCtx.isNil() || matCtx.rawBits() <= 0x10000) continue;
        if (!frame.savedMethod.isObject() || frame.savedMethod.rawBits() <= 0x10000) continue;

        ObjectHeader* ctxHdr = matCtx.asObjectPtr();
        size_t ctxSlots = ctxHdr->slotCount();
        if (ctxSlots <= ContextFixedFields) continue;

        // Get numTemps from method header (includes args)
        Oop methodHeader = memory_.fetchPointer(0, frame.savedMethod);
        int numTemps = 0;
        if (methodHeader.isSmallInteger()) {
            numTemps = (methodHeader.asSmallInteger() >> 18) & 0x3F;
        }

        // Sync each temp: C++ stack → context
        for (int t = 0; t < numTemps && (ContextFixedFields + t) < static_cast<int>(ctxSlots); t++) {
            Oop* stackSlot = frame.savedFP + 1 + t;
            if (stackSlot >= stackBase_ && stackSlot < stackPointer_) {
                memory_.storePointer(ContextFixedFields + t, matCtx, *stackSlot);
            }
        }

        // Update stackp to cover all synced temps so GC traces them.
        // Without this, a stale stackp from materialization time could cause
        // the GC to skip valid pointer slots during marking/compaction.
        Oop currentStackp = memory_.fetchPointer(2, matCtx);
        int64_t currentSP = currentStackp.isSmallInteger() ? currentStackp.asSmallInteger() : 0;
        if (numTemps > currentSP) {
            memory_.storePointer(2, matCtx, Oop::fromSmallInteger(numTemps));
        }
    }

    // Also sync current frame's materialized context
    if (currentFrameMaterializedCtx_.isObject() && !currentFrameMaterializedCtx_.isNil() &&
        currentFrameMaterializedCtx_.rawBits() > 0x10000 &&
        method_.isObject() && method_.rawBits() > 0x10000) {

        ObjectHeader* ctxHdr = currentFrameMaterializedCtx_.asObjectPtr();
        size_t ctxSlots = ctxHdr->slotCount();
        if (ctxSlots > ContextFixedFields) {
            Oop methodHeader = memory_.fetchPointer(0, method_);
            int numTemps = 0;
            if (methodHeader.isSmallInteger()) {
                numTemps = (methodHeader.asSmallInteger() >> 18) & 0x3F;
            }

            for (int t = 0; t < numTemps && (ContextFixedFields + t) < static_cast<int>(ctxSlots); t++) {
                Oop* stackSlot = framePointer_ + 1 + t;
                if (stackSlot >= stackBase_ && stackSlot < stackPointer_) {
                    memory_.storePointer(ContextFixedFields + t, currentFrameMaterializedCtx_, *stackSlot);
                }
            }

            // Update stackp to cover all synced temps
            Oop currentStackp = memory_.fetchPointer(2, currentFrameMaterializedCtx_);
            int64_t currentSP = currentStackp.isSmallInteger() ? currentStackp.asSmallInteger() : 0;
            if (numTemps > currentSP) {
                memory_.storePointer(2, currentFrameMaterializedCtx_, Oop::fromSmallInteger(numTemps));
            }
        }
    }
}

void Interpreter::afterGC() {
    // Convert current frame's offsets back to pointers (method may have moved).
    if (method_.isObject()) {
        uint8_t* methodBytes = method_.asObjectPtr()->bytes();
        instructionPointer_ = methodBytes + ipOffset_;
        bytecodeEnd_ = methodBytes + bytecodeEndOffset_;

        // Verify: bytecode at restored IP must match what was saved
        if (gcVerifyMethodOop_ != 0 && gcVerifyBytecodeAtIP_ != 0xFF &&
            instructionPointer_ && instructionPointer_ < bytecodeEnd_) {
            uint8_t actualBC = *instructionPointer_;
            if (actualBC != gcVerifyBytecodeAtIP_) {
                static int gcMismatchCount = 0;
                if (++gcMismatchCount <= 10) {
                    fprintf(stderr, "[GC-VERIFY-FAIL #%d] BC mismatch! saved=0x%02X actual=0x%02X "
                            "oldMethod=0x%llx newMethod=0x%llx ipOff=%lld fd=%zu gcCount=%d\n",
                            gcMismatchCount, gcVerifyBytecodeAtIP_, actualBC,
                            (unsigned long long)gcVerifyMethodOop_,
                            (unsigned long long)method_.rawBits(),
                            (long long)ipOffset_, frameDepth_,
                            memory_.statistics().gcCount);
                }
            }
        }
    }

    // Convert saved frames' offsets back to pointers
    for (size_t i = 0; i < frameDepth_; ++i) {
        SavedFrame& frame = savedFrames_[i];
        if (frame.savedMethod.isObject()) {
            uint8_t* methodBytes = frame.savedMethod.asObjectPtr()->bytes();
            frame.savedIP = methodBytes + frame.savedIPOffset;
            frame.savedBytecodeEnd = methodBytes + frame.savedBytecodeEndOffset;
        }
    }

    // GC may move method and class objects, invalidating cached lookups
    flushMethodCache();
    recoverJITAfterGC();
}

void Interpreter::logCurrentMethod(FILE* out) {
    if (!method_.isObject() || method_.rawBits() <= 0x10000) {
        fprintf(out, "[INTERP-STATE] method_=0x%llx (not valid)\n",
                (unsigned long long)method_.rawBits());
        return;
    }
    std::string selName = memory_.selectorOf(method_);
    std::string rcvrClassName = memory_.classNameOf(receiver_);
    int pc = 0;
    if (instructionPointer_ && method_.isObject()) {
        pc = (int)(instructionPointer_ - method_.asObjectPtr()->bytes());
    }
    fprintf(out, "[INTERP-STATE] %s>>%s pc=%d receiver_=0x%llx (%s) frameDepth=%zu\n",
            rcvrClassName.c_str(), selName.c_str(), pc,
            (unsigned long long)receiver_.rawBits(), rcvrClassName.c_str(),
            frameDepth_);

    // Also log what was on the stack (the bad receiver is likely a stack value)
    int numStack = (int)(stackPointer_ - framePointer_) - 1;
    if (numStack > 10) numStack = 10;
    for (int i = 0; i < numStack; i++) {
        Oop val = *(framePointer_ + 1 + i);
        fprintf(out, "[INTERP-STATE]   stack[%d] = 0x%llx%s\n",
                i, (unsigned long long)val.rawBits(),
                val.rawBits() == 0 ? " (ZERO!)" :
                (val.isSmallInteger() ? " (SI)" :
                (val.isNil() ? " (nil)" : "")));
    }
    fflush(out);
}

// Explicit instantiation of forEachRoot is not needed since the template is in the header.
// The implementation is below, but since it references private members,
// it needs to be visible from the header. We'll put it here and include
// this section from the header via a separate impl file pattern.
// Actually, since the template must be in the header for the compiler to see it,
// we implement it there. See Interpreter.hpp.

// ===== JIT INTEGRATION =====

#if PHARO_JIT_ENABLED

void Interpreter::initializeJIT() {
    if (jitInitialized_) return;
    jitInitialized_ = true;

    static bool jitDisabled = (getenv("PHARO_NO_JIT") != nullptr);
    if (jitDisabled) {
        fprintf(stderr, "[JIT] Disabled via PHARO_NO_JIT env var\n");
        return;
    }

    if (!jitRuntime_.initialize(memory_, *this)) {
        fprintf(stderr, "[JIT] Failed to initialize — running interpreted only\n");
        return;
    }
    // Expose code zone for crash diagnostics
    ::g_jitCodeZone = &jitRuntime_.codeZone();
}

uint32_t Interpreter::computeCurrentBCOffset() {
    if (!method_.isObject() || method_.rawBits() < 0x10000) return UINT32_MAX;
    ObjectHeader* methObj = method_.asObjectPtr();
    Oop methodHeader = methObj->slots()[0];
    if (!methodHeader.isSmallInteger()) return UINT32_MAX;
    int64_t headerBits = methodHeader.asSmallInteger();
    int numLiterals = headerBits & 0x7FFF;
    uint8_t* bytecodeStart = methObj->bytes() + (1 + numLiterals) * 8;
    if (instructionPointer_ < bytecodeStart) return UINT32_MAX;
    return static_cast<uint32_t>(instructionPointer_ - bytecodeStart);
}

void Interpreter::tryJITResumeInCaller() {
    // After a send returns, we're in the caller's frame with IP at the
    // bytecode after the send and the return value on the stack. If the
    // caller has JIT code, resume execution in JIT from this bytecode.
    if (inJITResume_) return;  // Prevent re-entrancy from returnValue or tryJITActivation
    inJITResume_ = true;
    int resumeIter = 0;
    while (running_ && jitRuntime_.isInitialized()) {
        if (++resumeIter > 10000) break;  // Safety limit
        // Break out if checkCountdown_ expired — let interpret() periodic
        // checks run (GC, timer, process scheduling, test triggers, etc.)
        if (checkCountdown_ <= 0) break;

        // Validate method_ before using it
        if (!method_.isObject() || method_.rawBits() < 0x10000) break;

        // FAST PATH: check if caller method is compiled before expensive setup
        jit::JITMethod* jm = jitRuntime_.methodMap().lookup(method_.rawBits());
        if (!jm || !jm->isExecutable()) break;

        uint32_t bcOffset = computeCurrentBCOffset();
        if (bcOffset == UINT32_MAX) break;

        // Set up JIT state from current interpreter state
        jit::JITState state;
        state.sp = stackPointer_;
        state.receiver = receiver_;

        ObjectHeader* methObj = method_.asObjectPtr();
        state.literals = methObj->slots() + 1;
        state.tempBase = framePointer_ + 1;
        state.memory = &memory_;
        state.interp = this;
        {
            Oop hdr = methObj->slots()[0];
            int numLits = hdr.isSmallInteger() ? (hdr.asSmallInteger() & 0x7FFF) : 0;
            state.ip = methObj->bytes() + (1 + numLits) * 8;
        }
        state.method = method_;
        state.argCount = argCount_;
        state.icDataPtr = nullptr;
        state.sendArgCount = 0;

        if (!jitRuntime_.tryResume(method_, bcOffset, state)) {
            break;  // No re-entry at this offset
        }

        // Charge the periodic check countdown for JIT-executed bytecodes.
        // Without this, the resume loop starves interpreter periodic checks
        // (GC, timer semaphores, process scheduling) when enough methods are
        // JIT-compiled that tryResume always succeeds.
        if (state.jitMethod) {
            checkCountdown_ -= state.jitMethod->numBytecodes;
            g_stepNum += state.jitMethod->numBytecodes;
        }

        switch (state.exitReason) {
        case jit::ExitReturn:
            // JIT completed the rest of the method and returned.
            if (!popFrame()) {
                // fd=0: no C++ frames left. Follow context sender chain.
                if (activeContext_.isObject() && !activeContext_.isNil()) {
                    Oop sender = memory_.fetchPointer(0, activeContext_);
                    if (sender.isObject() && !sender.isNil() && memory_.isValidPointer(sender)) {
                        // Kill current context (it returned)
                        memory_.storePointer(0, activeContext_, memory_.nil());
                        memory_.storePointer(1, activeContext_, memory_.nil());
                        // Load sender context
                        stackPointer_ = stackBase_;
                        Oop senderStackp = memory_.fetchPointer(2, sender);
                        int origSp = senderStackp.isSmallInteger()
                            ? static_cast<int>(senderStackp.asSmallInteger()) : 0;
                        executeFromContext(sender);
                        // Push return value at correct position
                        framePointer_[1 + origSp] = state.returnValue;
                        Oop* pastVal = framePointer_ + 1 + origSp + 1;
                        if (pastVal > stackPointer_) stackPointer_ = pastVal;
                        continue;  // Try to resume in sender's JIT code
                    }
                }
                // No valid sender — top of context chain
                if (benchMode_) {
                    inJITResume_ = false;
                    handleBenchComplete();
                    return;
                }
                terminateCurrentProcess();
                if (tryReschedule()) {
                    inJITResume_ = false;
                    return;
                }
                stopVM("No runnable processes after JIT return at fd=0");
                inJITResume_ = false;
                return;
            }
            if (running_) {
                push(state.returnValue);
            }
            continue;  // Try to resume in the next caller

        case jit::ExitSend: {
            // JIT hit a send. Let interpreter handle it.
            instructionPointer_ = state.ip;
            stackPointer_ = state.sp;
            jitICMisses_++;

            // Patch IC on miss if any slot is empty
            if (state.icDataPtr) {
                bool hasEmpty = false;
                for (int e = 0; e < 4; e++) {
                    if (state.icDataPtr[e * 3] == 0) { hasEmpty = true; break; }
                }
                if (hasEmpty) {
                    pendingICPatch_ = state.icDataPtr;
                    pendingICSendArgCount_ = state.sendArgCount;
                }
            }
            inJITResume_ = false;
            return;
        }

        case jit::ExitSendCached: {
            // IC hit during resume — activate cached method, then resume caller
            Oop cached = state.cachedTarget;
            if (!cached.isObject() || cached.rawBits() < 0x10000 ||
                cached.asObjectPtr()->classIndex() != compiledMethodClassIndex_) {
                // Stale IC — fall through to normal send
                jitICStale_++;
                instructionPointer_ = state.ip;
                stackPointer_ = state.sp;
                pendingICPatch_ = nullptr;
                inJITResume_ = false;
                return;
            }
            jitICHits_++;
            instructionPointer_ = state.ip;
            stackPointer_ = state.sp;

            // Upgrade IC entry to J2J if target is now JIT-compiled
            upgradeICToJ2J(state.icDataPtr, cached, state.sendArgCount, state.method);

            uint8_t sendOp = *instructionPointer_;
            if (sendOp >= 0x80 && sendOp <= 0xAF) instructionPointer_ += 1;
            else if (sendOp == 0xEA || sendOp == 0xEB) instructionPointer_ += 2;
            else instructionPointer_ += 1;

            jitRuntime_.noteMethodEntry(cached);  // Count for JIT compilation

            // Try primitive before activateMethod — primitive methods should
            // execute their primitive, not fallback bytecodes.
            {
                int primIdx = primitiveIndexOf(cached);

                // DEBUG: detect P60 on byte objects from resume-cached path
                if (primIdx == 60 && state.sendArgCount == 1) {
                    Oop rcRcv = stackValue(1);
                    if (rcRcv.isObject() && rcRcv.rawBits() > 0x10000 &&
                        rcRcv.asObjectPtr()->isBytesObject()) {
                        Oop rcIdx = stackValue(0);
                        fprintf(stderr, "[RESUME-CACHED-P60-BYTE] idx=0x%llx(isInt=%d) "
                                "rcv byteSize=%zu callerMethod='%s'\n",
                                (unsigned long long)rcIdx.rawBits(), rcIdx.isSmallInteger(),
                                rcRcv.asObjectPtr()->byteSize(),
                                memory_.selectorOf(state.method).c_str());
                    }
                }

                if (primIdx > 0) {
                    size_t primCallerDepth = frameDepth_;
                    argCount_ = state.sendArgCount;
                    primitiveFailed_ = false;
                    primFailCode_ = 0;
                    newMethod_ = cached;
                    PrimitiveResult result = executePrimitive(primIdx, state.sendArgCount);
                    if (result == PrimitiveResult::Success) {
                        // Frame-pushing primitives (closure activation prims 81/82/
                        // 201-209, perform: prims 83/84, etc.) call activateBlock/
                        // pushFrame inside executePrimitive. The new frame must run
                        // before the caller resumes — bail to interpreter so the
                        // dispatch loop drives the activated frame to completion.
                        if (frameDepth_ != primCallerDepth) {
                            jitJ2JFallbacks_++;
                            inJITResume_ = false;
                            return;
                        }
                        // Primitive completed in place — resume JIT at bytecode after send
                        continue;
                    }

                    // DEBUG: P60 failure on byte object
                    if (primIdx == 60 && state.sendArgCount == 1) {
                        Oop rcRcv = stackValue(1); // after failure, stack unchanged
                        if (rcRcv.isObject() && rcRcv.rawBits() > 0x10000 &&
                            rcRcv.asObjectPtr()->isBytesObject()) {
                            fprintf(stderr, "[RESUME-CACHED-P60-BYTE] FAILED! failCode=%d\n",
                                    primFailCode_);
                        }
                    }
                }
            }

            size_t callerDepth = frameDepth_;
            activateMethod(cached, state.sendArgCount);
            if (frameDepth_ == callerDepth) {
                // Target completed (JIT handled it end-to-end).
                // We're back in the caller's frame — resume JIT.
                jitJ2JChains_++;
                continue;
            }
            // Target has an active frame — let dispatch loop handle it
            jitJ2JFallbacks_++;
            inJITResume_ = false;
            return;
        }

        case jit::ExitBlockCreate: {
            // PushFullBlock during resume: create closure, then continue resume loop
            instructionPointer_ = state.ip;
            stackPointer_ = state.sp;

            uint64_t packed = state.cachedTarget.rawBits();
            int litIndex = static_cast<int>(packed & 0xFFFF);
            int flags = static_cast<int>((packed >> 32) & 0xFF);
            int numCopied = flags & 0x3F;
            bool receiverOnStack = (flags >> 7) & 1;
            bool ignoreOuterContext = (flags >> 6) & 1;

            createFullBlockWithLiteral(litIndex, numCopied, receiverOnStack, ignoreOuterContext);
            instructionPointer_ += 3;
            continue;  // Try to resume JIT at next bytecode
        }

        case jit::ExitArrayCreate: {
            // PushArray during resume: allocate array, then continue resume loop
            instructionPointer_ = state.ip;
            stackPointer_ = state.sp;

            int desc = static_cast<int>(state.cachedTarget.rawBits());
            int arraySize = desc & 0x7F;
            bool popIntoArray = (desc >> 7) != 0;

            Oop arrayClass = memory_.specialObject(SpecialObjectIndex::ClassArray);
            uint32_t classIndex = memory_.indexOfClass(arrayClass);
            Oop array = memory_.allocateSlots(classIndex, arraySize, ObjectFormat::Indexable);
            if (popIntoArray) {
                for (int i = arraySize - 1; i >= 0; i--)
                    memory_.storePointer(i, array, pop());
            }
            push(array);
            instructionPointer_ += 2;  // Past PushArray (2 bytes)
            continue;  // Resume JIT at next bytecode
        }

        case jit::ExitArithOverflow:
            instructionPointer_ = state.ip;
            stackPointer_ = state.sp;
            inJITResume_ = false;
            return;

        case jit::ExitJ2JCall: {
            // J2J IC hit during resume — ip is already past the send bytecode.
            // Handle like ExitSendCached: activate the cached method directly.
            Oop cached = state.cachedTarget;
            if (!cached.isObject() || cached.rawBits() < 0x10000 ||
                cached.asObjectPtr()->classIndex() != compiledMethodClassIndex_) {
                // Stale IC entry — sync state and bail to interpreter
                instructionPointer_ = state.ip;
                stackPointer_ = state.sp;
                inJITResume_ = false;
                return;
            }
            jitICHits_++;
            instructionPointer_ = state.ip;  // Already past the send
            stackPointer_ = state.sp;

            jitRuntime_.noteMethodEntry(cached);

            // Try primitive before activateMethod — primitive methods should
            // execute their primitive, not fallback bytecodes.
            {
                int primIdx = primitiveIndexOf(cached);
                if (primIdx > 0) {
                    size_t primCallerDepth = frameDepth_;
                    argCount_ = state.sendArgCount;
                    primitiveFailed_ = false;
                    primFailCode_ = 0;
                    newMethod_ = cached;
                    PrimitiveResult result = executePrimitive(primIdx, state.sendArgCount);
                    if (result == PrimitiveResult::Success) {
                        if (frameDepth_ != primCallerDepth) {
                            jitJ2JFallbacks_++;
                            inJITResume_ = false;
                            return;
                        }
                        // Primitive completed in place — resume JIT
                        continue;
                    }
                }
            }

            size_t callerDepth = frameDepth_;
            activateMethod(cached, state.sendArgCount);
            if (frameDepth_ == callerDepth) {
                jitJ2JChains_++;
                continue;
            }
            jitJ2JFallbacks_++;
            inJITResume_ = false;
            return;
        }

        default:
            inJITResume_ = false;
            return;
        }
    }
    inJITResume_ = false;
}

// Map primitive index to inline primKind for lightweight J2J dispatch.
// Returns 0 if the primitive can't be inlined.
static uint8_t inlinePrimKind(int primIndex) {
    switch (primIndex) {
    case 1:  return 1;   // add
    case 2:  return 2;   // sub
    case 3:  return 3;   // lessThan
    case 4:  return 4;   // greaterThan
    case 5:  return 5;   // lessEqual
    case 6:  return 6;   // greaterEqual
    case 7:  return 7;   // equal
    case 8:  return 8;   // notEqual
    case 9:  return 9;   // mul
    case 110: return 10; // identical
    case 14: return 11;  // bitAnd
    case 15: return 12;  // bitOr
    case 17: return 13;  // bitShift
    case 60: return 14;  // at:
    case 61: return 15;  // at:put:
    case 62: return 16;  // size
    default: return 0;
    }
}

void Interpreter::patchJITICAfterSend(Oop resolvedMethod, Oop receiver, Oop selector) {
    if (!pendingICPatch_) return;
    uint64_t* icData = pendingICPatch_;
    pendingICPatch_ = nullptr;

    // DEBUG: Selector-based J2J fill skip (PHARO_J2J_SKIP_SELECTORS).
    // Same skip used by upgradeICToJ2J — uses the IC's send-site selector
    // (passed in directly here), which is reliable.
    {
        static const char* skipEnv = getenv("PHARO_J2J_SKIP_SELECTORS");
        if (skipEnv && *skipEnv) {
            std::string sel;
            if (selector.isObject() && selector.rawBits() > 0x10000) {
                ObjectHeader* sh = selector.asObjectPtr();
                if (sh->isBytesObject() && sh->byteSize() < 80) {
                    sel = std::string((char*)sh->bytes(), sh->byteSize());
                }
            }
            const char* p = skipEnv;
            while (*p) {
                const char* end = p;
                while (*end && *end != ',') end++;
                if ((size_t)(end - p) == sel.size() &&
                    std::memcmp(p, sel.data(), sel.size()) == 0) {
                    return;
                }
                p = (*end == ',') ? end + 1 : end;
            }
        }
    }

    // Verify the IC belongs to this send by checking that the IC's stored
    // selector matches the send's selector. If they don't match, the
    // pendingICPatch_ was stale (set by a different send in a nested JIT
    // execution or process switch) — patching would corrupt the IC.
    uint64_t icSelectorBits = icData[12];
    if (icSelectorBits != 0 && icSelectorBits != selector.rawBits()) {
        return;  // IC belongs to a different send site — skip
    }

    // Compute lookup key matching stencil_sendJ2J:
    // objects → classIndex, immediates → (tag & 7) | 0x80000000
    uint64_t lookupKey;
    uint64_t tag = receiver.rawBits() & 0x7;
    if (tag == 0 && receiver.rawBits() >= 0x10000) {
        lookupKey = receiver.asObjectPtr()->classIndex();
    } else if (tag != 0) {
        lookupKey = tag | 0x80000000ULL;
    } else {
        return;  // Invalid object pointer (< 0x10000)
    }

    // Check if this key is already cached (avoid duplicates)
    for (int e = 0; e < 4; e++) {
        if (icData[e * 3] == lookupKey) return;
    }

    // Detect inline getter/setter/yourself for J2J dispatch.
    // extra word bit layout:
    //   bit 63: getter — slot index in bits 15:0
    //   bit 62: setter — slot index in bits 15:0
    //   bit 61: returnsSelf
    //   bit 60: hasJITEntry — bits 47:0 = JIT code entry address
    uint64_t extra = 0;
    {
        TrivialMethodInfo tmi = detectTrivialMethod(resolvedMethod, memory_);
        if (tmi.getterIndex >= 0)
            extra = (1ULL << 63) | (uint16_t)tmi.getterIndex;
        else if (tmi.setterIndex >= 0)
            extra = (1ULL << 62) | (uint16_t)tmi.setterIndex;
        else if (tmi.returnsSelf)
            extra = (1ULL << 61);
    }

    // Set inline primKind bits for methods with inlineable primitives,
    // regardless of JIT compilation status. This allows the stencil to
    // handle SmallInteger arithmetic inline without any function call.
    if (extra == 0) {  // Not a getter/setter/yourself
        int primIdx = primitiveIndexOf(resolvedMethod);
        uint8_t pk = inlinePrimKind(primIdx);
        if (pk) extra |= (uint64_t)pk << 48;
    }

    // If not a trivial method, check for JIT-compiled target for J2J direct calls.
    // IMPORTANT: Don't set J2J for methods with primitives but no prologue stencil —
    // J2J skips CallPrimitive (it compiles to stencil_nop), so the primitive never runs.
    // Also skip J2J for class/metaclass receivers (format 1) — these trigger
    // errorNotIndexable bugs during path resolution. Instance J2J stays enabled.
    static bool j2jEnabled = !getenv("PHARO_NO_J2J");
    bool isClassReceiver = false;
    if (receiver.isObject() && receiver.rawBits() > 0x10000) {
        ObjectHeader* rcvObj = receiver.asObjectPtr();
        if (static_cast<uint8_t>(rcvObj->format()) == 1)
            isClassReceiver = true;
    }
    if ((extra & (1ULL << 60)) == 0 && j2jEnabled && !isClassReceiver) {
        jit::JITMethod* target = jitRuntime_.methodMap().lookup(resolvedMethod.rawBits());
        if (target && target->isExecutable()) {
            // Check if target has a primitive but no prologue
            bool unsafePrim = false;
            if (!target->hasPrimPrologue) {
                ObjectHeader* methObj = resolvedMethod.asObjectPtr();
                Oop headerOop = methObj->slotAt(0);
                if (headerOop.isSmallInteger()) {
                    int64_t hdr = headerOop.asSmallInteger();
                    if ((hdr >> 16) & 1)  // hasPrimitive flag
                        unsafePrim = true;
                }
            }
            if (!unsafePrim && !isJ2JBanned(resolvedMethod.rawBits())) {
                uint64_t entryAddr = reinterpret_cast<uint64_t>(target->codeStart());
                // Preserve primKind bits (52:48) already set above
                extra |= (1ULL << 60) | (entryAddr & 0x0000FFFFFFFFFFFFULL);
                jitJ2JDirectPatches_++;
            }
        }
    }

    // Find the first empty slot and fill it
    for (int e = 0; e < 4; e++) {
        if (icData[e * 3] == 0) {
            icData[e * 3] = lookupKey;
            icData[e * 3 + 1] = resolvedMethod.rawBits();
            icData[e * 3 + 2] = extra;
            jitICPatches_++;
            // Debug: log J2J patches for high-frequency methods
            static int logCount = 0;
            if ((extra & (1ULL << 60)) && logCount < 30) {
                logCount++;
                std::string sel = memory_.selectorOf(resolvedMethod);
                fprintf(stderr, "[IC-PATCH] #%s J2J=1 key=0x%llx extra=0x%llx\n",
                        sel.c_str(), (unsigned long long)lookupKey, (unsigned long long)extra);
            }
            return;
        }
    }
    // All 4 slots full — megamorphic, don't patch
}

void Interpreter::upgradeICToJ2J(uint64_t* icData, Oop cachedMethod, int sendArgCount,
                                  Oop callerMethod) {
    static bool j2jEnabled = !getenv("PHARO_NO_J2J");
    static bool fillEnabled = !getenv("PHARO_NO_IC_FILL");
    if (!j2jEnabled || !icData) return;

    // DEBUG: Selector-based J2J upgrade skip (PHARO_J2J_SKIP_SELECTORS).
    // Lets us bisect which method's J2J upgrade triggers a bug. Note that
    // PHARO_JIT_SKIP_SELECTORS prevents JIT compilation entirely; this only
    // prevents the J2J fast-path patch.
    //
    // We compare against the IC's selector (icData[12]) rather than
    // selectorOf(cachedMethod), because the latter is unreliable for some
    // primitive methods (e.g. at:/at:put: returned "?" via numLiteralsOf,
    // making the bisection mis-fire).
    {
        static const char* skipEnv = getenv("PHARO_J2J_SKIP_SELECTORS");
        if (skipEnv && *skipEnv) {
            std::string sel;
            uint64_t icSelBits = icData[12];
            if (icSelBits != 0 && icSelBits > 0x10000) {
                Oop sOop = Oop::fromRawBits(icSelBits);
                if (sOop.isObject()) {
                    ObjectHeader* sh = sOop.asObjectPtr();
                    if (sh->isBytesObject() && sh->byteSize() < 80) {
                        sel = std::string((char*)sh->bytes(), sh->byteSize());
                    }
                }
            }
            if (sel.empty()) sel = memory_.selectorOf(cachedMethod);
            const char* p = skipEnv;
            while (*p) {
                const char* end = p;
                while (*end && *end != ',') end++;
                if ((size_t)(end - p) == sel.size() &&
                    std::memcmp(p, sel.data(), sel.size()) == 0) {
                    return;
                }
                p = (*end == ',') ? end + 1 : end;
            }
        }
    }

    jit::JITMethod* target = jitRuntime_.methodMap().lookup(cachedMethod.rawBits());

    // Eager compilation: if the target has a supported primitive prologue but
    // isn't JIT-compiled yet, compile it now. Primitive methods never reach the
    // compile threshold via noteMethodEntry because the primitive succeeds before
    // bytecodes execute. Without eager compilation, at:/at:put:/size/arithmetic
    // methods can never be called via J2J with their fast-path prologues.
    static bool noEagerCompile = !!getenv("PHARO_NO_EAGER_COMPILE");
    if (!noEagerCompile && (!target || !target->isExecutable()) && jitRuntime_.compiler()) {
        // Check if method has a primitive
        ObjectHeader* methObj = cachedMethod.asObjectPtr();
        Oop hdr = methObj->slotAt(0);
        bool hasPrim = hdr.isSmallInteger() && ((hdr.asSmallInteger() >> 16) & 1);
        if (hasPrim) {
            int primIdx = primitiveIndexOf(cachedMethod);
            if (primIdx > 0 && primIdx < 200) {
                target = jitRuntime_.compiler()->compile(cachedMethod);
                if (target && !target->hasPrimPrologue) target = nullptr;
            }
        }
    }

    if (!target || !target->isExecutable()) return;
    if (isJ2JBanned(cachedMethod.rawBits())) return;

    // Check unsafe prim: has primitive but no JIT prologue
    if (!target->hasPrimPrologue) {
        ObjectHeader* methObj = cachedMethod.asObjectPtr();
        Oop hdr = methObj->slotAt(0);
        if (hdr.isSmallInteger() && ((hdr.asSmallInteger() >> 16) & 1))
            return;  // unsafe primitive
    }

    // Find the receiver on the stack to compute the lookup key
    Oop receiver = stackPointer_[-(sendArgCount + 1)];
    uint64_t lookupKey;
    uint64_t tag = receiver.rawBits() & 0x7;
    if (tag == 0 && receiver.rawBits() >= 0x10000) {
        ObjectHeader* rcvObj = receiver.asObjectPtr();
        // Skip J2J for class/metaclass receivers (format 1) — known bug source
        if (static_cast<uint8_t>(rcvObj->format()) == 1) return;
        lookupKey = rcvObj->classIndex();
    } else if (tag != 0) {
        lookupKey = tag | 0x80000000ULL;
    } else {
        return;
    }

    // Find the matching IC entry and upgrade, or fill an empty slot
    int firstEmpty = -1;
    for (int e = 0; e < 4; e++) {
        if (icData[e * 3] == lookupKey) {
            uint64_t extra = icData[e * 3 + 2];
            if (extra == 0) {
                uint64_t entryAddr = reinterpret_cast<uint64_t>(target->codeStart());
                uint64_t newExtra = (1ULL << 60) | (entryAddr & 0x0000FFFFFFFFFFFFULL);
                if (target->hasPrimPrologue) {
                    int primIdx = primitiveIndexOf(cachedMethod);
                    uint8_t pk = inlinePrimKind(primIdx);
                    if (pk) newExtra |= (uint64_t)pk << 48;
                }
                icData[e * 3 + 2] = newExtra;
                jitJ2JDirectPatches_++;
            }
            return;
        }
        if (firstEmpty < 0 && icData[e * 3] == 0) firstEmpty = e;
    }
    // No matching entry found — fill an empty slot with J2J entry.
    if (firstEmpty >= 0 && fillEnabled) {
        // Detect trivial getter/setter/returnsSelf — for these, set inline
        // bits 63/62/61 instead of the J2J direct call bit 60. The inline path
        // in stencil_sendJ2J's J2J_IC_HIT macro reads receiver->slotAt(idx)
        // directly without calling the JIT-compiled method, avoiding bugs in
        // the J2J trampoline call path for trivial methods (e.g., the
        // Context>>sender / findContextSuchThat: mustBeBoolean bug).
        uint64_t newExtra = 0;
        {
            TrivialMethodInfo tmi = detectTrivialMethod(cachedMethod, memory_);
            if (tmi.getterIndex >= 0)
                newExtra = (1ULL << 63) | (uint16_t)tmi.getterIndex;
            else if (tmi.setterIndex >= 0)
                newExtra = (1ULL << 62) | (uint16_t)tmi.setterIndex;
            else if (tmi.returnsSelf)
                newExtra = (1ULL << 61);
        }
        if (newExtra == 0) {
            // Not trivial — set J2J direct-call entry plus inline primKind bits
            uint64_t entryAddr = reinterpret_cast<uint64_t>(target->codeStart());
            newExtra = (1ULL << 60) | (entryAddr & 0x0000FFFFFFFFFFFFULL);
            if (target->hasPrimPrologue) {
                int primIdx = primitiveIndexOf(cachedMethod);
                uint8_t pk = inlinePrimKind(primIdx);
                if (pk) newExtra |= (uint64_t)pk << 48;
            }
        }

        icData[firstEmpty * 3] = lookupKey;
        icData[firstEmpty * 3 + 1] = cachedMethod.rawBits();
        icData[firstEmpty * 3 + 2] = newExtra;
        if (newExtra & (1ULL << 60)) jitJ2JDirectPatches_++;
        jitICPatches_++;
    }
}

bool Interpreter::tryJITActivation(Oop method, int argCount) {
    if (!jitRuntime_.isInitialized()) return false;
    static bool noJit = getenv("PHARO_NOJIT") != nullptr;
    if (noJit) return false;

    // Suppress tryJITResumeInCaller while the chain loop is active.
    // Both mechanisms resume JIT after sends return; having both active
    // simultaneously creates infinite mutual recursion.
    bool wasInJITResume = inJITResume_;
    inJITResume_ = true;
    struct ResumeGuard {
        bool& flag; bool prev;
        ~ResumeGuard() { flag = prev; }
    } resumeGuard{inJITResume_, wasInJITResume};

    static int jitActivationDepth = 0;
    jitActivationDepth++;
    struct DepthGuard { ~DepthGuard() { jitActivationDepth--; } } depthGuard;
    // Guard: method must be a valid object pointer
    if (!method.isObject() || method.rawBits() < 0x10000) return false;

    // FAST PATH: check if method is compiled BEFORE expensive JITState setup.
    // This avoids ~20 pointer writes per send for non-compiled methods.
    jit::JITMethod* jm = jitRuntime_.methodMap().lookup(method.rawBits());
    if (!jm || !jm->isExecutable()) return false;

    // Method is compiled — set up JITState
    jit::JITState state;
    state.sp = stackPointer_;
    state.receiver = receiver_;

    ObjectHeader* methObj = method.asObjectPtr();
    state.literals = methObj->slots() + 1;
    state.tempBase = framePointer_ + 1;

    state.memory = &memory_;
    state.interp = this;

    // IP = bytecodeStart (stencils use ip + bcOffset where bcOffset is from method start).
    {
        Oop hdr = methObj->slots()[0];
        int numLits = hdr.isSmallInteger() ? (hdr.asSmallInteger() & 0x7FFF) : 0;
        state.ip = methObj->bytes() + (1 + numLits) * 8;
    }
    state.method = method;
    state.argCount = argCount;
    state.icDataPtr = nullptr;
    state.sendArgCount = 0;
    state.trueOop = memory_.trueObject();
    state.falseOop = memory_.falseObject();

    // J2J stencil-to-stencil save stack — must be set up BEFORE tryExecute
    // because stencils may hit J2J sends during the initial execution.
    struct J2JSave {
        // --- Always stored (hot path) ---
        Oop* sp;                  // 0
        Oop receiver;             // 8
        Oop* tempBase;            // 16  (pair with ip)
        uint8_t* ip;              // 24
        jit::JITMethod* jitMethod;// 32  (pair with resumeAddr)
        uint8_t* resumeAddr;      // 40  Precomputed JIT code to resume at
        int sendArgCount;         // 48  (packed with argCount)
        int argCount;             // 52  (non-self-recursive only)
        // --- Non-self-recursive only (cold-ish) ---
        Oop* literals;            // 56
        uint8_t* bcStart;         // 64  Precomputed bytecodeStart
    };
    static_assert(sizeof(struct J2JSave) == 72, "J2JSave should be 72 bytes");
    static constexpr int MaxJ2JDepth = 256;
    J2JSave j2jStack[MaxJ2JDepth];

    state.j2jSaveCursor = reinterpret_cast<uint8_t*>(&j2jStack[0]);
    state.j2jSaveLimit  = reinterpret_cast<uint8_t*>(&j2jStack[MaxJ2JDepth]);
    state.j2jDepth = 0;
    state.j2jTotalCalls = 0;

    // Save entry SP so we can restore it on ExitDeopt/ExitPrimFail.
    Oop* entrySP = stackPointer_;

    if (!jitRuntime_.tryExecute(method, state, jm)) {
        return false;  // Should not happen — jm was already validated
    }

    // Charge the periodic check countdown for JIT-executed bytecodes.
    // Without this, JIT execution starves the interpreter's periodic checks
    // (GC, timer semaphores, process scheduling, heartbeat) because the
    // countdown only decrements on interpreter bytecode dispatch.
    auto chargeJITBytecodes = [this](const jit::JITState& s) {
        if (s.jitMethod) {
            checkCountdown_ -= s.jitMethod->numBytecodes;
            g_stepNum += s.jitMethod->numBytecodes;
        }
    };

    chargeJITBytecodes(state);

    // ===== J2J TRAMPOLINE =====
    // Handles ExitJ2JCall / ExitReturn in a tight loop WITHOUT recursive C++
    // calls. Eliminates jit_rt_j2j_call's 12-register prologue/epilogue per
    // send. Frames are "lazy" — only frameDepth_ is incremented, SavedFrame
    // is NOT written. On bailout we materialize frames from the save stack.
    //
    // Safe because GC/process-switch cannot trigger during JIT execution
    // (stencils don't allocate, no timer checks).
    {
        // j2jStack and MaxJ2JDepth are declared above (before tryExecute)
        // so stencils can use them during initial execution.
        int j2jDepth = 0;
        size_t j2jBaseFrameDepth = frameDepth_;

        // Cache Interpreter member fields into locals so the compiler keeps
        // them in registers across JIT_CALL invocations. The asm volatile
        // with "memory" clobber in JIT_CALL would otherwise force a reload
        // through `this` on every loop iteration.
        size_t localFrameDepth = frameDepth_;
        size_t localCalls = 0;
        size_t localReturns = 0;

        // Toggle W^X to executable once for the entire trampoline loop.
        // The loop only reads JIT code (executable) and writes to C stack (always writable).
#if defined(__APPLE__) && defined(__arm64__)
        if (state.exitReason == jit::ExitJ2JCall ||
            (state.exitReason == jit::ExitReturn && j2jDepth > 0)) {
            pthread_jit_write_protect_np(1);
        }
#endif

#if defined(PHARO_ASM_TRAMPOLINE) && defined(__aarch64__)
        // Hand-written ARM64 loop: pins state/save-cursor/counters in
        // callee-saved registers across BLR to stencils. Only runs the loop
        // if we're actually entering it; otherwise the C++ fall-through below
        // is a no-op (matches the while-condition in the fallback version).
        if (state.exitReason == jit::ExitJ2JCall ||
            (state.exitReason == jit::ExitReturn && j2jDepth > 0)) {
            pharo_jit_j2j_trampoline(
                &state,
                j2jStack,
                &localFrameDepth,
                &localCalls,
                &localReturns,
                memory_.nil().rawBits());
            // Recover j2jDepth from the frameDepth delta — the asm keeps
            // both counters in lockstep, so `(localFrameDepth -
            // j2jBaseFrameDepth)` is the number of unpopped save slots.
            j2jDepth = static_cast<int>(localFrameDepth - j2jBaseFrameDepth);
        }
#else
        while (state.exitReason == jit::ExitJ2JCall ||
               (state.exitReason == jit::ExitReturn && j2jDepth > 0)) {

            if (state.exitReason == jit::ExitJ2JCall) {
                // --- J2J Call: save caller, push lazy frame, enter callee ---
                if (j2jDepth >= MaxJ2JDepth) {
                    // Too deep — fall back to interpreter
                    state.exitReason = jit::ExitSendCached;
                    break;
                }

                localCalls++;

                // Pre-load fields used both for the save and the callee setup.
                jit::JITMethod* callerJM = reinterpret_cast<jit::JITMethod*>(state.jitMethod);
                uint8_t* entryAddr = reinterpret_cast<uint8_t*>(state.returnValue.rawBits());
                jit::JITMethod* calleeJM = reinterpret_cast<jit::JITMethod*>(
                    entryAddr - sizeof(jit::JITMethod));
                int nArgs = state.sendArgCount;

                // Save caller JITState to save stack. For self-recursive calls
                // (caller JIT method == callee JIT method), we skip saving
                // literals/argCount because the callee will not change them.
                // Marker: low bit of save.jitMethod set to 1 means
                // "self-recursive; skip literals/argCount restores on return".
                // JITMethod* is 8-byte aligned so bit 0 is always free.
                J2JSave& save = j2jStack[j2jDepth++];
                save.sp = state.sp;
                save.receiver = state.receiver;
                save.tempBase = state.tempBase;
                save.ip = state.ip;
                save.sendArgCount = nArgs;

                bool selfRecursive = (callerJM == calleeJM);
                // Compute callerBCStart (re-used below to precompute resume).
                int callerNumLits = static_cast<int>(callerJM->methodHeader & 0x7FFF);
                ObjectHeader* callerMethObj =
                    Oop::fromRawBits(callerJM->compiledMethodOop).asObjectPtr();
                uint8_t* callerBCStart =
                    callerMethObj->bytes() + (1 + callerNumLits) * 8;
                if (__builtin_expect(selfRecursive, 1)) {
                    // Self-recursive: calleeBCStart == callerBCStart. On a
                    // clean return, state.ip will still equal callerBCStart
                    // (stencils don't touch state.ip on the fast path), so
                    // we don't need to save bcStart OR restore state.ip on
                    // return. Skip both stores.
                    save.jitMethod = reinterpret_cast<jit::JITMethod*>(
                        reinterpret_cast<uintptr_t>(callerJM) | 1ULL);
                } else {
                    save.jitMethod = callerJM;
                    save.literals = state.literals;
                    save.argCount = state.argCount;
                    save.bcStart = callerBCStart;
                }
                // Precompute resume JIT code address (avoids bcToCode lookup on return)
                {
                    uint32_t bcOffset = static_cast<uint32_t>(state.ip - callerBCStart);
                    uint32_t codeOffset = callerJM->codeOffsetForBC(bcOffset);
                    save.resumeAddr = (codeOffset == 0 || codeOffset >= callerJM->codeSize)
                        ? nullptr
                        : callerJM->codeStart() + codeOffset;
                }

                // Lazy frame: just increment local depth, no SavedFrame write.
                // frameDepth_ is synced back to the member at loop exit.
                if (__builtin_expect(localFrameDepth >= StackOverflowLimit, 0)) {
                    j2jDepth--;
                    state.exitReason = jit::ExitStackOverflow;
                    break;
                }
                localFrameDepth++;

                // Set up callee in JITState (lightweight — no interpreter sync)
                Oop targetMethod = state.cachedTarget;
                ObjectHeader* methObj = targetMethod.asObjectPtr();
                Oop calleeRecv = state.sp[-(nArgs + 1)];
                Oop* fp = state.sp - (nArgs + 1);

                state.receiver = calleeRecv;
                state.tempBase = fp + 1;
                // Note: state.exitReason is NOT cleared here. Stencils only
                // WRITE exitReason — they never read it on entry. The callee
                // will set exitReason before RETing (via return or exit-send
                // stencils), so clearing it beforehand is redundant.
                if (__builtin_expect(!selfRecursive, 0)) {
                    state.literals = methObj->slots() + 1;
                    state.argCount = nArgs;
                    state.jitMethod = calleeJM;
                }
                // Note: state.method is NOT updated here. Stencils don't read it,
                // and we reconstruct from state.jitMethod->compiledMethodOop
                // after the trampoline loop exits.

                // IP = bytecodeStart of callee. For self-recursive calls this
                // equals the caller's bcStart we just computed, so reuse it.
                if (__builtin_expect(selfRecursive, 1)) {
                    state.ip = callerBCStart;
                } else {
                    int numLits = static_cast<int>(calleeJM->methodHeader & 0x7FFF);
                    state.ip = methObj->bytes() + (1 + numLits) * 8;
                }

                // Allocate temps if needed
                int totalTemps = calleeJM->tempCount;
                if (__builtin_expect(nArgs < totalTemps, 0)) {
                    Oop nil = memory_.nil();
                    for (int i = nArgs; i < totalTemps; i++) {
                        *state.sp = nil;
                        state.sp++;
                    }
                }

                // Enter callee JIT code (already executable from loop start)
                JIT_CALL(entryAddr, &state);

            } else {
                // --- J2J Return: pop frame, resume caller ---
                localReturns++;
                j2jDepth--;
                localFrameDepth--;

                Oop retVal = state.returnValue;
                J2JSave& save = j2jStack[j2jDepth];

                // Restore caller JITState (state.method is NOT stored in J2JSave
                // — stencils don't read it; reconstructed on bailout from jitMethod).
                state.sp = save.sp;
                state.receiver = save.receiver;
                state.tempBase = save.tempBase;
                // Low bit of save.jitMethod = 1 marks a self-recursive save:
                // literals, argCount, jitMethod are unchanged. Also, state.ip
                // is already equal to callerBCStart (same method, stencils
                // don't modify ip on the fast path), so we don't need to
                // restore it — and save.bcStart wasn't even written for
                // self-recursive saves.
                uintptr_t savedJMBits =
                    reinterpret_cast<uintptr_t>(save.jitMethod);
                if (__builtin_expect((savedJMBits & 1) == 0, 0)) {
                    state.literals = save.literals;
                    state.jitMethod = save.jitMethod;
                    state.argCount = save.argCount;
                    // Non-self: must reset state.ip from calleeBCStart to
                    // callerBCStart before resuming caller's stencils.
                    state.ip = save.bcStart;
                }

                // Pop receiver+args, push return value
                // Stack layout: sp[-(nArgs+1)]=receiver, sp[-nArgs]=arg1, ..., sp[-1]=TOS
                // sp points to next free slot. Replace receiver with retVal, adjust down.
                state.sp[-(save.sendArgCount + 1)] = retVal;
                state.sp -= save.sendArgCount;

                // Resume caller's JIT at bytecode after send (precomputed on call path)
                if (__builtin_expect(save.resumeAddr == nullptr, 0)) {
                    state.ip = save.ip;  // interpreter needs post-send IP
                    state.exitReason = jit::ExitReturn;
                    break;
                }

                // exitReason NOT cleared — stencils only write it, never read.
                JIT_CALL(save.resumeAddr, &state);
            }

            // No checkCountdown_ check here: nothing inside the loop body
            // modifies it (stencils don't touch Interpreter member fields,
            // and we only charge cumulative counts at loop exit). Reading it
            // forced a memory reload every iteration via JIT_CALL's "memory"
            // clobber. The outer interpret() loop handles countdown expiry
            // between trampoline sessions.
        }
#endif // PHARO_ASM_TRAMPOLINE
        // Toggle back to writable after trampoline loop
#if defined(__APPLE__) && defined(__arm64__)
        pthread_jit_write_protect_np(0);
#endif
        // Merge stencil-managed J2J depth with trampoline-managed depth.
        // Stencils push/pop frames via state.j2jDepth; the trampoline uses
        // j2jDepth directly.  Take whichever is larger (normally only one
        // mechanism is active at a time).
        if (state.j2jDepth > j2jDepth) {
            j2jDepth = state.j2jDepth;
            // Stencil-managed calls also need frameDepth_ adjustment
            localFrameDepth = j2jBaseFrameDepth + j2jDepth;
        }

        // Sync cached locals back to Interpreter member fields.
        frameDepth_ = localFrameDepth;
        jitJ2JStencilCalls_ += localCalls + state.j2jTotalCalls;
        jitJ2JStencilReturns_ += localReturns;
        // Charge bytecodes for all J2J calls + returns in bulk
        checkCountdown_ -= static_cast<int>(localCalls + localReturns) * 10;
        checkCountdown_ -= state.j2jTotalCalls * 10;

        // Reconstruct state.method from state.jitMethod — we skip updating it
        // in the hot loop for speed, but fall-through paths need it current.
        if (state.jitMethod) {
            // Mask off possible self-recursive bit 0 from stencil J2J
            uintptr_t jmBits = reinterpret_cast<uintptr_t>(state.jitMethod);
            jmBits &= ~static_cast<uintptr_t>(1);
            state.jitMethod = reinterpret_cast<jit::JITMethod*>(jmBits);
            state.method = Oop::fromRawBits(
                reinterpret_cast<jit::JITMethod*>(jmBits)->compiledMethodOop);
        }

        // If we bailed out with pending J2J frames, materialize them
        // so the interpreter can see them.
        if (j2jDepth > 0) {
            // Materialize SavedFrames from save stack (oldest first).
            // For self-recursive saves (low bit of save.jitMethod set), the
            // effective method is the SAME as the caller. Mask the bit off
            // and derive argCount from JITMethod::argCount since we skipped
            // saving it on the hot path.
            Oop nil = memory_.nil();
            for (int i = 0; i < j2jDepth; i++) {
                J2JSave& save = j2jStack[i];
                SavedFrame& frame = savedFrames_[j2jBaseFrameDepth + i];

                uintptr_t jmBits = reinterpret_cast<uintptr_t>(save.jitMethod);
                bool isSelfRecursive = (jmBits & 1) != 0;
                jit::JITMethod* saveJM = reinterpret_cast<jit::JITMethod*>(
                    jmBits & ~static_cast<uintptr_t>(1));
                Oop saveMethod = Oop::fromRawBits(saveJM->compiledMethodOop);
                ObjectHeader* saveMethObj = saveMethod.asObjectPtr();
                int saveNumLits = static_cast<int>(saveJM->methodHeader & 0x7FFF);
                uint8_t* saveBCStart = saveMethObj->bytes() + (1 + saveNumLits) * 8;
                int frameArgCount =
                    isSelfRecursive ? saveJM->argCount : save.argCount;

                frame.savedIP = save.ip;
                frame.savedBytecodeEnd = saveBCStart + saveJM->numBytecodes;
                frame.savedMethod = saveMethod;
                frame.savedHomeMethod = saveMethod;
                frame.savedReceiver = save.receiver;
                frame.savedClosure = nil;
                frame.savedActiveContext = nil;
                frame.materializedContext = nil;
                frame.savedFP = save.tempBase - 1;
                frame.savedArgCount = frameArgCount;
                frame.homeFrameDepth = SIZE_MAX;
            }
            // Sync interpreter from current JITState (innermost frame)
            method_ = state.method;
            homeMethod_ = state.method;
            receiver_ = state.receiver;
            stackPointer_ = state.sp;
            instructionPointer_ = state.ip;
            framePointer_ = state.tempBase - 1;
            argCount_ = state.argCount;
        }
    }

    // Loop to handle chained JIT execution: when an IC-hit send's target
    // completes, resume JIT execution at the next bytecode instead of
    // falling back to the interpreter dispatch loop.
    //
    // IMPORTANT: The countdown check is NOT at the top of the loop.
    // After tryResume+continue, state holds an unprocessed exit reason
    // that MUST be handled before breaking. The countdown is checked at
    // each continue site instead (after chargeJITBytecodes).
    static bool noChain = !!getenv("PHARO_NO_CHAIN");
    int maxChain = noChain ? 1 : 100;

    for (int chainLimit = 0; chainLimit < maxChain; chainLimit++) {

        // Validate JIT output state — detect stencil corruption early
        if (state.exitReason == jit::ExitSend || state.exitReason == jit::ExitArithOverflow ||
            state.exitReason == jit::ExitSendCached) {
            uint64_t ipVal = reinterpret_cast<uint64_t>(state.ip);
            uint64_t spVal = reinterpret_cast<uint64_t>(state.sp);
            if (ipVal < 0x10000 || ipVal > 0x1000000000000ULL) {
                fprintf(stderr, "[JIT] BAD state.ip=0x%llx after exit %d, method=0x%llx\n",
                        (unsigned long long)ipVal, state.exitReason,
                        (unsigned long long)method.rawBits());
                return false;
            }
            if (spVal < 0x10000 || spVal > 0x1000000000000ULL) {
                fprintf(stderr, "[JIT] BAD state.sp=0x%llx after exit %d\n",
                        (unsigned long long)spVal, state.exitReason);
                return false;
            }
        }

        // Handle exit reason
        Oop chainTarget;  // Set by ExitSend/ExitSendCached/ExitJ2JCall, used by shared chain code after switch
        bool ipAlreadyAdvanced = false;  // ExitJ2JCall: stencil already advanced IP past send
        switch (state.exitReason) {
        case jit::ExitReturn: {
            if (!popFrame()) {
                // fd=0: follow context sender chain
                if (activeContext_.isObject() && !activeContext_.isNil()) {
                    Oop sender = memory_.fetchPointer(0, activeContext_);
                    if (sender.isObject() && !sender.isNil() && memory_.isValidPointer(sender)) {
                        memory_.storePointer(0, activeContext_, memory_.nil());
                        memory_.storePointer(1, activeContext_, memory_.nil());
                        stackPointer_ = stackBase_;
                        Oop senderStackp = memory_.fetchPointer(2, sender);
                        int origSp = senderStackp.isSmallInteger()
                            ? static_cast<int>(senderStackp.asSmallInteger()) : 0;
                        executeFromContext(sender);
                        framePointer_[1 + origSp] = state.returnValue;
                        Oop* pastVal = framePointer_ + 1 + origSp + 1;
                        if (pastVal > stackPointer_) stackPointer_ = pastVal;
                        return true;
                    }
                }
                // No sender — top of context chain.
                if (benchMode_) {
                    // PHARO_BENCH: handle benchmark completion inline
                    handleBenchComplete();
                    return true;
                }
                terminateCurrentProcess();
                tryReschedule();
                return true;
            }
            push(state.returnValue);
            return true;
        }

        case jit::ExitSend: {
            // IC miss: do method lookup, patch IC, and chain into callee
            // instead of bailing to interpreter (which loses JIT continuity).
            instructionPointer_ = state.ip;
            stackPointer_ = state.sp;
            jitICMisses_++;
            // Get selector from IC data (stored at offset 12 by compiler)
            if (!state.icDataPtr) {
                static int noIC = 0;
                if (++noIC <= 5) fprintf(stderr, "[CHAIN-EXIT-SEND] no icDataPtr! returning false\n");
                return false;
            }
            Oop sendSel = Oop::fromRawBits(state.icDataPtr[12]);
            if (!sendSel.isObject() || sendSel.rawBits() < 0x10000) {
                static int badSel = 0;
                if (++badSel <= 5) fprintf(stderr, "[CHAIN-EXIT-SEND] bad selector bits=0x%llx! returning false\n",
                    (unsigned long long)sendSel.rawBits());
                return false;
            }

            int nArgs = state.sendArgCount;
            Oop rcvr = stackValue(nArgs);
            Oop rcvrClass = memory_.classOf(rcvr);

            // Method lookup — global method cache first, then full lookup
            Oop resolved;
            MethodCacheEntry* ce = probeCache(sendSel, rcvrClass);
            if (ce) {
                resolved = ce->method;
            } else {
                resolved = lookupMethod(sendSel, rcvrClass);
                if (resolved.isNil()) return false;  // DNU — interpreter handles
                cacheMethod(sendSel, rcvrClass, resolved);
            }

            if (!resolved.isObject() || resolved.rawBits() < 0x10000 ||
                resolved.asObjectPtr()->classIndex() != compiledMethodClassIndex_) {
                return false;  // Non-standard method — interpreter handles
            }

            // Patch IC immediately so next hit is ExitSendCached
            pendingICPatch_ = state.icDataPtr;
            pendingICSendArgCount_ = nArgs;
            patchJITICAfterSend(resolved, rcvr, sendSel);

            // Populate mega cache for JIT stencil probes
            {
                uint64_t tag = rcvr.rawBits() & 0x7;
                uint64_t megaKey = (tag == 0 && rcvr.rawBits() >= 0x10000)
                    ? static_cast<uint64_t>(rcvr.asObjectPtr()->classIndex())
                    : (tag != 0 ? (tag | 0x80000000ULL) : 0);
                if (megaKey != 0)
                    jitRuntime_.megaCacheAdd(sendSel.rawBits(), megaKey, resolved.rawBits());
            }

            chainTarget = resolved;
            jitRuntime_.noteMethodEntry(resolved);
            break;  // → shared send-chain code after switch
        }

        case jit::ExitSendCached: {
            // IC hit: cached method is in state.cachedTarget. Skip method lookup.
            Oop cached = state.cachedTarget;
            if (!cached.isObject() || cached.rawBits() < 0x10000 ||
                cached.asObjectPtr()->classIndex() != compiledMethodClassIndex_) {
                // Stale IC — invalidate and fall back to normal send
                jitICStale_++;
                if (state.icDataPtr) {
                    for (int e = 0; e < 4; e++) {
                        state.icDataPtr[e * 3] = 0;
                        state.icDataPtr[e * 3 + 1] = 0;
                        state.icDataPtr[e * 3 + 2] = 0;
                    }
                }
                instructionPointer_ = state.ip;
                stackPointer_ = state.sp;
                return false;
            }
            jitICHits_++;
            instructionPointer_ = state.ip;
            stackPointer_ = state.sp;

            // Upgrade IC entry to J2J if target is now JIT-compiled
            upgradeICToJ2J(state.icDataPtr, cached, state.sendArgCount, state.method);

            chainTarget = cached;
            jitRuntime_.noteMethodEntry(cached);
            break;  // → shared send-chain code after switch
        }

        case jit::ExitBlockCreate: {
            // PushFullBlock exit: create the closure, then resume JIT.
            instructionPointer_ = state.ip;
            stackPointer_ = state.sp;

            // Extract block parameters from cachedTarget
            uint64_t packed = state.cachedTarget.rawBits();
            int litIndex = static_cast<int>(packed & 0xFFFF);
            int flags = static_cast<int>((packed >> 32) & 0xFF);
            int numCopied = flags & 0x3F;
            bool receiverOnStack = (flags >> 7) & 1;
            bool ignoreOuterContext = (flags >> 6) & 1;

            // Create the closure using the interpreter's existing method
            createFullBlockWithLiteral(litIndex, numCopied, receiverOnStack, ignoreOuterContext);

            // Advance IP past PushFullBlock (3 bytes)
            instructionPointer_ += 3;

            // Try to resume JIT at next bytecode
            method = method_;  // Refresh: GC may have moved the method
            uint32_t bcOffset = computeCurrentBCOffset();
            if (bcOffset == UINT32_MAX) return true;

            // Re-setup JIT state from current interpreter state
            state.sp = stackPointer_;
            state.receiver = receiver_;
            methObj = method.asObjectPtr();
            state.literals = methObj->slots() + 1;
            state.tempBase = framePointer_ + 1;
            {
                Oop hdr = methObj->slots()[0];
                int numLits = hdr.isSmallInteger() ? (hdr.asSmallInteger() & 0x7FFF) : 0;
                state.ip = methObj->bytes() + (1 + numLits) * 8;
            }
            state.method = method;
            state.argCount = argCount;
            state.icDataPtr = nullptr;
            state.sendArgCount = 0;
            state.exitReason = jit::ExitNone;

            if (!jitRuntime_.tryResume(method, bcOffset, state)) {
                return true;  // Can't resume; interpreter handles rest
            }
            chargeJITBytecodes(state);
            if (checkCountdown_ <= 0) goto jit_loop_exit;
            continue;  // Loop to handle the new exit reason
        }

        case jit::ExitArrayCreate: {
            // PushArray exit: allocate array, then resume JIT.
            instructionPointer_ = state.ip;
            stackPointer_ = state.sp;

            int desc = static_cast<int>(state.cachedTarget.rawBits());
            int arraySize = desc & 0x7F;
            bool popIntoArray = (desc >> 7) != 0;

            Oop arrayClass = memory_.specialObject(SpecialObjectIndex::ClassArray);
            uint32_t classIndex = memory_.indexOfClass(arrayClass);
            Oop array = memory_.allocateSlots(classIndex, arraySize, ObjectFormat::Indexable);
            if (popIntoArray) {
                for (int i = arraySize - 1; i >= 0; i--)
                    memory_.storePointer(i, array, pop());
            }
            push(array);

            // Advance IP past PushArray (2 bytes)
            instructionPointer_ += 2;

            // Try to resume JIT at next bytecode
            method = method_;  // Refresh: GC may have moved the method
            uint32_t bcOffset = computeCurrentBCOffset();
            if (bcOffset == UINT32_MAX) return true;

            // Re-setup JIT state from current interpreter state
            state.sp = stackPointer_;
            state.receiver = receiver_;
            methObj = method.asObjectPtr();
            state.literals = methObj->slots() + 1;
            state.tempBase = framePointer_ + 1;
            {
                Oop hdr = methObj->slots()[0];
                int numLits = hdr.isSmallInteger() ? (hdr.asSmallInteger() & 0x7FFF) : 0;
                state.ip = methObj->bytes() + (1 + numLits) * 8;
            }
            state.method = method;
            state.argCount = argCount;
            state.icDataPtr = nullptr;
            state.sendArgCount = 0;
            state.exitReason = jit::ExitNone;

            if (!jitRuntime_.tryResume(method, bcOffset, state)) {
                return true;  // Can't resume; interpreter handles rest
            }
            chargeJITBytecodes(state);
            if (checkCountdown_ <= 0) goto jit_loop_exit;
            continue;  // Loop to handle the new exit reason
        }

        case jit::ExitArithOverflow: {
            instructionPointer_ = state.ip;
            stackPointer_ = state.sp;
            return false;
        }

        case jit::ExitJ2JCall: {
            // J2J IC hit during chain resume — stencil set bit 60 in a prior
            // upgradeICToJ2J, but the J2J trampoline only runs once (before the
            // chain loop).  Handle as a regular method activation.  IP is already
            // past the send bytecode (stencil sets ip = ip + bcOffset + bcLen).
            Oop j2jCached = state.cachedTarget;
            if (!j2jCached.isObject() || j2jCached.rawBits() < 0x10000 ||
                j2jCached.asObjectPtr()->classIndex() != compiledMethodClassIndex_) {
                instructionPointer_ = state.ip;
                stackPointer_ = state.sp;
                return false;
            }
            jitICHits_++;
            instructionPointer_ = state.ip;
            stackPointer_ = state.sp;
            chainTarget = j2jCached;
            jitRuntime_.noteMethodEntry(j2jCached);
            ipAlreadyAdvanced = true;
            break;  // → shared send-chain code after switch
        }

        case jit::ExitPrimFail:
        case jit::ExitDeopt:
            stackPointer_ = entrySP;
            return false;

        default:
            return false;
        }

        // --- Shared send-chain code (reached via break from ExitSend/ExitSendCached/ExitJ2JCall) ---
        // chainTarget holds the resolved method. Advance IP past the send bytecode,
        // try primitive execution, then activateMethod and resume JIT.
        {
            // Advance IP past send bytecode (ExitJ2JCall: stencil already did this)
            if (!ipAlreadyAdvanced) {
                uint8_t sendOp = *instructionPointer_;
                if (sendOp >= 0x80 && sendOp <= 0xAF) {
                    instructionPointer_ += 1;
                } else if (sendOp == 0xEA || sendOp == 0xEB) {
                    instructionPointer_ += 2;  // ExtSend / ExtSuperSend
                } else {
                    instructionPointer_ += 1;
                }
            }

            int nArgs = state.sendArgCount;

            // Try primitive before activateMethod — primitive methods should
            // execute their primitive, not fallback bytecodes.
            {
                int primIdx = primitiveIndexOf(chainTarget);

                if (primIdx > 0) {
                    size_t primCallerDepth = frameDepth_;
                    argCount_ = nArgs;
                    primitiveFailed_ = false;
                    primFailCode_ = 0;
                    newMethod_ = chainTarget;
                    PrimitiveResult result = executePrimitive(primIdx, nArgs);
                    chainTarget = newMethod_;  // Refresh: GC during prim may have moved it

                    if (result == PrimitiveResult::Success) {
                        // Frame-pushing primitives (closure activation prims 81/82/
                        // 201-209, perform: prims 83/84, etc.) call activateBlock/
                        // pushFrame inside executePrimitive. The new frame must run
                        // before the caller resumes — bail to interpreter so the
                        // dispatch loop drives the activated frame to completion.
                        if (frameDepth_ != primCallerDepth) {
                            jitJ2JActFalls_++;
                            return true;
                        }
                        // Primitive completed in place — resume JIT at bytecode after send
                        jitJ2JActChains_++;
                        method = method_;
                        uint32_t bcOffset = computeCurrentBCOffset();
                        if (bcOffset == UINT32_MAX) return true;
                        state.sp = stackPointer_;
                        state.receiver = receiver_;
                        methObj = method.asObjectPtr();
                        state.literals = methObj->slots() + 1;
                        state.tempBase = framePointer_ + 1;
                        {
                            Oop hdr = methObj->slots()[0];
                            int numLits = hdr.isSmallInteger() ? (hdr.asSmallInteger() & 0x7FFF) : 0;
                            state.ip = methObj->bytes() + (1 + numLits) * 8;
                        }
                        state.method = method;
                        state.argCount = argCount;
                        state.icDataPtr = nullptr;
                        state.sendArgCount = 0;
                        state.exitReason = jit::ExitNone;
                        if (!jitRuntime_.tryResume(method, bcOffset, state)) {
                            return true;
                        }
                        chargeJITBytecodes(state);
                        if (checkCountdown_ <= 0) goto jit_loop_exit;
                        continue;
                    }
                }
            }

            // Non-primitive or primitive failed — activate the method
            size_t callerDepth = frameDepth_;
            activateMethod(chainTarget, nArgs);

            if (frameDepth_ != callerDepth) {
                // Target pushed a frame — interpreter dispatch loop handles it
                jitJ2JActFalls_++;
                return true;
            }

            // Target completed (JIT or trivial method handled it end-to-end).
            // Resume JIT execution at the bytecode after the send.
            jitJ2JActChains_++;
            {
                method = method_;  // Refresh: GC may have moved the method
                uint32_t bcOffset = computeCurrentBCOffset();
                if (bcOffset == UINT32_MAX) return true;

                state.sp = stackPointer_;
                state.receiver = receiver_;
                methObj = method.asObjectPtr();
                state.literals = methObj->slots() + 1;
                state.tempBase = framePointer_ + 1;
                {
                    Oop hdr = methObj->slots()[0];
                    int numLits = hdr.isSmallInteger() ? (hdr.asSmallInteger() & 0x7FFF) : 0;
                    state.ip = methObj->bytes() + (1 + numLits) * 8;
                }
                state.method = method;
                state.argCount = argCount;
                state.icDataPtr = nullptr;
                state.sendArgCount = 0;
                state.exitReason = jit::ExitNone;

                if (!jitRuntime_.tryResume(method, bcOffset, state)) {
                    return true;
                }
                chargeJITBytecodes(state);
                if (checkCountdown_ <= 0) goto jit_loop_exit;
                continue;
            }
        }
    }
    // Chain limit reached — fall through to handle unprocessed exit

jit_loop_exit:
    // The loop exited (chain limit or countdown expired) with an
    // unprocessed exit reason in state.  Process it now so the
    // interpreter state is consistent.
    switch (state.exitReason) {
    case jit::ExitReturn:
        if (!popFrame()) {
            if (activeContext_.isObject() && !activeContext_.isNil()) {
                Oop sender = memory_.fetchPointer(0, activeContext_);
                if (sender.isObject() && !sender.isNil() && memory_.isValidPointer(sender)) {
                    memory_.storePointer(0, activeContext_, memory_.nil());
                    memory_.storePointer(1, activeContext_, memory_.nil());
                    stackPointer_ = stackBase_;
                    Oop senderStackp = memory_.fetchPointer(2, sender);
                    int origSp = senderStackp.isSmallInteger()
                        ? static_cast<int>(senderStackp.asSmallInteger()) : 0;
                    executeFromContext(sender);
                    framePointer_[1 + origSp] = state.returnValue;
                    Oop* pastVal = framePointer_ + 1 + origSp + 1;
                    if (pastVal > stackPointer_) stackPointer_ = pastVal;
                    return true;
                }
            }
            if (benchMode_) {
                handleBenchComplete();
                return true;
            }
            terminateCurrentProcess();
            tryReschedule();
            return true;
        }
        push(state.returnValue);
        return true;

    case jit::ExitSend:
    case jit::ExitSendCached:
    case jit::ExitArithOverflow:
        // Sync interpreter state from JIT and let interpreter handle
        instructionPointer_ = state.ip;
        stackPointer_ = state.sp;
        return false;

    case jit::ExitBlockCreate:
    case jit::ExitArrayCreate:
        // These exits need interpreter handling; sync state
        instructionPointer_ = state.ip;
        stackPointer_ = state.sp;
        return false;

    default:
        // ExitNone or unknown — JIT handled everything
        return true;
    }
}

#endif // PHARO_JIT_ENABLED

} // namespace pharo
