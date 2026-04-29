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
#include "../platform/Platform.hpp"
#include "DebugSettings.hpp"
#include "InterpreterProxy.h"
#include "FFI.hpp"
#include "jit/TrampolineAsm.hpp"
#include "jit/Tier2Compiler.hpp"
#include "jit/SistaV1.hpp"
#include "jit/sista/SistaRuntime.hpp"
#include "jit/sista/SistaBuilder.hpp"
#include "plugins/sqMemoryAccess.h"
#include "../include/vmCallback.h"
#include "../platform/DisplaySurface.hpp"
#include "../platform/EventQueue.hpp"
#include <cstring>
#include <cmath>
#include <csetjmp>
#include <csignal>
#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <set>
#include <unordered_set>
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

// B5 diagnostic: ring buffer dump from JITRuntime.cpp (extern "C").
// JIT-only — no-op when JIT is disabled (iOS Device builds).
#if PHARO_JIT_ENABLED
extern "C" void jit_b5_dump_ring(const char* tag);
static inline void pharo_jit_b5_dump_ring(const char* tag) {
    jit_b5_dump_ring(tag);
}
#else
static inline void pharo_jit_b5_dump_ring(const char*) {}
#endif

namespace pharo {

// Set to false to disable all debug file logging for performance
constexpr bool ENABLE_DEBUG_LOGGING = false;

uint64_t g_stepNum = 0;  // Global step counter for hang debugging (non-static for use in Primitives.cpp)

#if PHARO_JIT_ENABLED
// Handle to the Sista runtime created lazily inside activateMethod.
// recoverJITAfterGC() clears its cache — raw oop keys become stale
// after Spur compaction.  Nullptr when Sista was never invoked.
//
// Visible to JITRuntime via extern decl so JITRuntime::noteMethodEntry
// can ask Sista whether a method has a splice (and skip T1 counting
// if so — avoids the T1-vs-Sista race in
// memory/project_t1_vs_sista_race.md).  Lives in pharo:: namespace,
// not file-scoped, so extern works.
sista::Runtime* sistaRuntimeForGCHook_ = nullptr;

// Per-method gate decision cache.  Values:
//   0 = eligible, no bails recorded (hot path — skip bailCounter lookup)
//   1 = hasUnsafeOp (rejected by bytecode scan)
//   2 = blacklisted (bail counter exceeded threshold)
// Avoids re-scanning the method's bytecodes on every activation.
// State 2 lets the dispatch path skip the bail-counter hashmap lookup
// entirely on every activation — saves ~1ns/dispatch on hot benches.
// Reset on GC alongside the Sista fn cache.
static std::unordered_map<uint64_t, uint8_t> sistaGateCache_;
static constexpr uint8_t kSistaGateAdmitted    = 0;
static constexpr uint8_t kSistaGateRejected    = 1;
static constexpr uint8_t kSistaGateBlacklisted = 2;

// Per-method consecutive-bail counter.  When a method ExitSends 8
// times in a row without a single ExitReturn, mark it ineligible
// so we stop paying the dispatch overhead on it.  Any subsequent
// successful ExitReturn resets the count (unlikely for bail-only
// methods, but handles pathological-then-recovered cases).
//
// Counter is only consulted/maintained for methods that have actually
// bailed; the hot dispatch path checks sistaGateCache_ value first and
// skips this map when state == kSistaGateAdmitted.
static std::unordered_map<uint64_t, uint16_t> sistaBailCounter_;
static constexpr uint16_t kSistaBailBlacklistThreshold = 8;

// Forward decl — defined after sistaRing_ struct.
static void sistaRingDump(const char* tag, class Interpreter* interp);

// Fixed-size ring of recent Sista dispatches — dumped when a DNU
// fires so the correlation between a bad Sista bail/return and the
// downstream DNU is visible without global tracing.
struct SistaRingEntry {
    uint64_t methodBits;
    uint64_t receiverBits;
    uint64_t retOrTopBits;   // sstate.returnValue (ret) or top-of-sp (send)
    uint32_t exitReason;     // jit::ExitReturn / jit::ExitSend / other
    uint16_t sendArgCount;
    uint8_t  bailOp;         // the bc at sstate.ip (0 for ExitReturn)
    int8_t   spDelta;        // sstate.sp - stackPointer_ (clamped)
};
static constexpr size_t kSistaRingSize = 32;
static SistaRingEntry sistaRing_[kSistaRingSize] = {};
static size_t sistaRingHead_ = 0;
static uint64_t sistaRingSeq_ = 0;

void Interpreter::recoverSistaAfterGC() {
    if (sistaRuntimeForGCHook_) sistaRuntimeForGCHook_->reset();
    sistaGateCache_.clear();
    sistaBailCounter_.clear();
    for (size_t i = 0; i < kSistaRingSize; i++) sistaRing_[i] = {};
}

// Dump the Sista ring buffer — called from the DNU logging site to
// correlate the DNU with recent Sista dispatches.
static void sistaRingDump(const char* tag, Interpreter* interp) {
    if (sistaRingSeq_ == 0) return;  // Sista never dispatched
    size_t count = std::min<uint64_t>(sistaRingSeq_, kSistaRingSize);
    size_t start = sistaRingSeq_ > kSistaRingSize
                       ? sistaRingHead_
                       : 0;
    fprintf(stderr, "[SISTA-RING] tag=%s seq=%llu count=%zu:\n",
            tag ? tag : "(null)",
            (unsigned long long)sistaRingSeq_, count);
    for (size_t i = 0; i < count; i++) {
        size_t idx = (start + i) % kSistaRingSize;
        const SistaRingEntry& e = sistaRing_[idx];
        Oop mO = Oop::fromRawBits(e.methodBits);
        std::string sel = interp
                              ? interp->memory().selectorOf(mO)
                              : std::string("?");
        const char* rsn =
            e.exitReason == (uint32_t)jit::ExitReturn     ? "RET"  :
            e.exitReason == (uint32_t)jit::ExitSend       ? "SEND" :
            e.exitReason == (uint32_t)jit::ExitSendCached ? "SENDC":
            e.exitReason == (uint32_t)jit::ExitJ2JCall    ? "J2J"  :
            e.exitReason == (uint32_t)jit::ExitYield      ? "YLD"  :
            e.exitReason == (uint32_t)jit::ExitStackOverflow ? "OVF":
            e.exitReason == (uint32_t)jit::ExitNone       ? "NONE" :
            "???";
        fprintf(stderr, "  [%zu] #%-28s %-5s rcvr=0x%llx ret/top=0x%llx "
                        "bailOp=0x%02x argc=%u spΔ=%d\n",
                i, sel.c_str(), rsn,
                (unsigned long long)e.receiverBits,
                (unsigned long long)e.retOrTopBits,
                (unsigned)e.bailOp, (unsigned)e.sendArgCount,
                (int)e.spDelta);
    }
}
#endif






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

    if (g_debug.debugDispLeak) {
        dispatchTraceLeakOn_ = true;
    }
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

    // Snapshot-resume deadlock cleanup — see unblockStuckSnapshotCallers()
    // for the rationale.  Called both here (once on resume to clear any
    // residual from the saved image) and periodically from handleForceYield
    // (to catch callers that arise post-resume when startup.st triggers
    // exitSuccess → snapshot:andQuit:).
    unblockStuckSnapshotCallers();

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
    if (g_debug.bench) {
        const char* benchType = g_debug.benchType;
        fprintf(stderr, "[BENCH] PHARO_BENCH=%s detected\n", benchType);
        int fibN = (g_debug.fibN >= 0) ? g_debug.fibN : 28;

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
        // PHARO_AWFY_ONLY=Name1,Name2 — run only named AWFY benchmarks
        const char* awfyOnly = g_debug.awfyOnly;
        if (wantAWFY) {
            // Are We Fast Yet benchmarks — each is a class with innerBenchmarkLoop:
            struct AWFYSpec { const char* name; const char* cls; int64_t arg; };
            AWFYSpec allAWFY[] = {
                {"Richards",   "Richards",   100},
                {"DeltaBlue",  "DeltaBlue",  12000},
                {"Mandelbrot", "Mandelbrot", 500},
                {"NBody",      "NBody",      250000},
                {"Bounce",     "Bounce",     1500},
                {"Permute",    "Permute",    1000},
                {"Queens",     "Queens",     1000},
                {"Sieve",      "Sieve",      3000},
                {"Storage",    "Storage",    1000},
                {"Towers",     "Towers",     600},
                {"List",       "List",       1500},
            };
            std::string filter(awfyOnly ? awfyOnly : "");
            for (auto& a : allAWFY) {
                if (filter.empty() || filter.find(a.name) != std::string::npos)
                    benchSpecs_.push_back({a.name, a.cls, "innerBenchmarkLoop:", a.arg, 5, true});
            }
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
    checkCountdown_ = 1024;
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

void Interpreter::handleBenchComplete(Oop returnValue) {
    const char* retTag =
        returnValue.rawBits() == memory_.trueObject().rawBits() ? "true" :
        returnValue.rawBits() == memory_.falseObject().rawBits() ? "false" :
        returnValue.isNil() ? "nil" :
        returnValue.isSmallInteger() ? "int" : "obj";
    long long intVal = returnValue.isSmallInteger() ? returnValue.asSmallInteger() : 0;
    fprintf(stderr, "[BENCH] handleBenchComplete called (specIdx=%d runCount=%d) ret=0x%llx (%s%s%lld)\n",
            benchSpecIdx_, benchRunCount_,
            (unsigned long long)returnValue.rawBits(), retTag,
            returnValue.isSmallInteger() ? " " : "",
            returnValue.isSmallInteger() ? intVal : 0LL);
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
        fprintf(stderr, "[BENCH] JIT stats: activations=%zu/%zu (%.1f%% hit) | IC hits=%zu misses=%zu stale=%zu | J2J patches=%zu stencilCalls=%zu/%zu\n",
            jitActivationHits_, jitActivations_,
            jitActivations_ > 0 ? 100.0 * jitActivationHits_ / jitActivations_ : 0.0,
            jitICHits_, jitICMisses_, jitICStale_,
            jitJ2JDirectPatches_, jitJ2JStencilCalls_, jitJ2JStencilReturns_);
        fprintf(stderr, "[BENCH] Chain paths: actChains=%zu actFalls=%zu (%.1f%% continuity)\n",
            jitJ2JActChains_, jitJ2JActFalls_,
            (jitJ2JActChains_ + jitJ2JActFalls_) > 0
                ? 100.0 * jitJ2JActChains_ / (jitJ2JActChains_ + jitJ2JActFalls_) : 0.0);
        fprintf(stderr, "[BENCH]   stencil-falls: cached=%zu send=%zu j2j=%zu other=%zu\n",
            jitStencilFallSendCached_, jitStencilFallSend_, jitStencilFallJ2JCall_, jitStencilFallOther_);
        fprintf(stderr, "[BENCH]   prim: chains=%zu falls=%zu | activate: chains=%zu falls=%zu | yields=%zu\n",
            jitChainPrimChains_, jitChainPrimFalls_,
            jitChainActivateChains_, jitChainActivateFalls_, jitYieldCount_);
        fprintf(stderr, "[BENCH]   OSR: entries=%zu\n", jitOSREntries_);
        // Top prim-fall primitives
        {
            struct PF { int idx; size_t count; };
            PF top[10] = {};
            for (int i = 0; i < 600; i++) {
                if (jitPrimFallHisto_[i] > 0) {
                    for (int t = 0; t < 10; t++) {
                        if (jitPrimFallHisto_[i] > top[t].count) {
                            for (int s = 9; s > t; s--) top[s] = top[s-1];
                            top[t] = {i, jitPrimFallHisto_[i]};
                            break;
                        }
                    }
                }
            }
            fprintf(stderr, "[BENCH]   top prim-falls:");
            for (int t = 0; t < 10 && top[t].count > 0; t++)
                fprintf(stderr, " p%d=%zu", top[t].idx, top[t].count);
            fprintf(stderr, "\n");
        }
        // Top prim-chain primitives (in-place successes)
        {
            struct PC { int idx; size_t count; };
            PC top[20] = {};
            for (int i = 0; i < 600; i++) {
                if (jitPrimChainHisto_[i] > 0) {
                    for (int t = 0; t < 20; t++) {
                        if (jitPrimChainHisto_[i] > top[t].count) {
                            for (int s = 19; s > t; s--) top[s] = top[s-1];
                            top[t] = {i, jitPrimChainHisto_[i]};
                            break;
                        }
                    }
                }
            }
            fprintf(stderr, "[BENCH]   top prim-chains:");
            for (int t = 0; t < 20 && top[t].count > 0; t++)
                fprintf(stderr, " p%d=%zu", top[t].idx, top[t].count);
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "[BENCH]   materialize: count=%zu totalDepth=%zu\n",
                jitMaterializeCount_, jitMaterializeTotalDepth_);
        // Sista Phase 1 diagnostic — IC polymorphism distribution.
        // Gated on PHARO_IC_HISTOGRAM=1.  Reports what fraction of
        // compiled send sites are monomorphic (inlining candidates)
        // vs polymorphic vs megamorphic.
        if (std::getenv("PHARO_IC_HISTOGRAM")) {
            jitRuntime_.dumpICHistogram();
        }
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

// A3 DIAGNOSTIC: instrument IC hits to see if icData[18] (selectorBits
// for megacache) is 0. If it's NON-zero on hits but 0 on misses, the
// stencil's IC-hit path and ExitSend miss path are reading different
// addresses. See memory/project_ic_selbits_mystery.md.
static inline void countICHitDbg(const uint64_t* ic) {
#if PHARO_JIT_ENABLED
    if (!g_debug.icHitDbg) return;
    static size_t withSel = 0, noSel = 0, noIC = 0;
    if (ic) {
        if (ic[18] == 0) noSel++;
        else withSel++;
    } else {
        noIC++;
    }
    size_t total = withSel + noSel + noIC;
    if (total % 1000000 == 1) {
        fprintf(stderr, "[IC-HIT-DBG] hitSel18=%zu hitNoSel18=%zu noIC=%zu\n",
                withSel, noSel, noIC);
    }
#else
    (void)ic;
#endif
}

void Interpreter::dumpJITStats() {
#if PHARO_JIT_ENABLED
    size_t icTotal = jitICHits_ + jitICMisses_;
    size_t actTotal = jitActivations_;
    size_t resumeTotal = jitJ2JChains_ + jitJ2JFallbacks_;
    fprintf(stderr, "\n=== JIT Stats ===\n");
    fprintf(stderr, "  compiled: %zu methods\n",
            jitRuntime_.methodMap().count());
    fprintf(stderr, "  IC: %zu hits / %zu total (%.1f%%), patches=%zu stale=%zu\n",
            jitICHits_, icTotal,
            icTotal > 0 ? 100.0 * jitICHits_ / icTotal : 0.0,
            jitICPatches_, jitICStale_);
    fprintf(stderr, "  IC miss breakdown: noSelBits=%zu cold=%zu poly=%zu noICData=%zu\n",
            jitICMissNoSelBits_, jitICMissCold_,
            jitICMissPolymorphic_, jitICMissNoICData_);
    fprintf(stderr, "  activations: %zu hits / %zu total (%.1f%%)\n",
            jitActivationHits_, actTotal,
            actTotal > 0 ? 100.0 * jitActivationHits_ / actTotal : 0.0);
    fprintf(stderr, "  resume (J2J-r): %zu chains / %zu total (%.1f%%)\n",
            jitJ2JChains_, resumeTotal,
            resumeTotal > 0 ? 100.0 * jitJ2JChains_ / resumeTotal : 0.0);
    fprintf(stderr, "  J2J stencil: calls=%zu returns=%zu patches=%zu\n",
            jitJ2JStencilCalls_, jitJ2JStencilReturns_, jitJ2JDirectPatches_);
    fprintf(stderr, "  materialize: count=%zu totalDepth=%zu\n",
            jitMaterializeCount_, jitMaterializeTotalDepth_);
    fprintf(stderr, "  chain: actChain=%zu actFall=%zu | primChain=%zu primFall=%zu\n",
            jitChainActivateChains_, jitChainActivateFalls_,
            jitChainPrimChains_, jitChainPrimFalls_);
    fprintf(stderr, "  yields=%zu OSR=%zu\n", jitYieldCount_, jitOSREntries_);
    {
        int t2ICTotal = jit::g_t2ICHits + jit::g_t2ICMisses;
        fprintf(stderr, "  T2-IC: %d hits / %d total (%.1f%%)\n",
                jit::g_t2ICHits, t2ICTotal,
                t2ICTotal > 0 ? 100.0 * jit::g_t2ICHits / t2ICTotal : 0.0);
    }
    jit::Tier2Compiler::dumpBailStats();
    fprintf(stderr, "=================\n");
#endif
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

    // Generational-GC young-gen enabler.  PHARO_YOUNG_GEN=1 opts
    // in to eden allocation + scavenge; default off until the
    // scavenge path is verified against the full test matrix.
    if (g_debug.youngGenEnabled) {
        memory_.enableYoungGen_ = true;
    }

    // Entry point for callback re-entry via siglongjmp(reenterInterpreter_, 1)
    if (sigsetjmp(reenterInterpreter_, 0) != 0) {
        // Re-entered from enterInterpreterFromCallback().
        // Active process has been switched; just fall into the loop.
        //
        // siglongjmp skips C++ destructors between the source and here.
        // If asmjit's ProtectJitReadWriteScope was active when the longjmp
        // fired, the thread's MAP_JIT W^X bit was left in writable mode
        // (the destructor that would have flipped to executable never
        // ran).  Subsequent JIT entry SIGBUSes with PC == fault_addr in
        // the code zone.  Force back to executable here.
        // See docs/jit-uncovered-bugs.md bug 11.
#if PHARO_JIT_ENABLED
        if (jitRuntime_.isInitialized()) {
            jit::makeExecutable(jitRuntime_.codeZone().rawStart(),
                                jitRuntime_.codeZone().totalBytes());
        }
#endif
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

    checkCountdown_ = 1024;
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
            if (__builtin_expect(dispatchTraceLeakOn_, 0) && framePointer_) { \
                long long spAboveFP = (long long)(stackPointer_ - framePointer_); \
                if (spAboveFP >= 500 && spAboveFP <= 520) { \
                    ObjectHeader* _mObj = method_.isObject() ? method_.asObjectPtr() : nullptr; \
                    const uint8_t* _bcBase = nullptr; \
                    if (_mObj) { \
                        Oop _hdr = _mObj->slots()[0]; \
                        int _nLit = _hdr.isSmallInteger() ? (_hdr.asSmallInteger() & 0x7FFF) : 0; \
                        _bcBase = _mObj->bytes() + (1 + _nLit) * 8; \
                    } \
                    long long _bcOff = (_bcBase) ? (long long)(instructionPointer_ - 1 - _bcBase) : -1; \
                    fprintf(stderr, "[DISP] fd=%zu sp-fp=%lld bcOff=%lld bc=%02x method=#%s\n", \
                            frameDepth_, spAboveFP, _bcOff, bytecode, \
                            memory_.selectorOf(method_).c_str()); \
                } \
            } \
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
        // B5 diagnostic: watch for decodeBytes: storing SmallInt to byteStream.
        if (g_debug.b5Trace && (bytecode & 0x07) == 1 &&
            method_.isObject() && value.isSmallInteger()) {
            std::string sel = memory_.selectorOf(method_);
            if (sel == "decodeBytes:") {
                fprintf(stderr, "[B5-STORE] decodeBytes: popStoreTemp 1 "
                               "value=SmallInt(%lld) method=0x%llx ip=%p\n",
                        (long long)value.asSmallInteger(),
                        (unsigned long long)method_.rawBits(),
                        instructionPointer_);
                pharo_jit_b5_dump_ring("popstore-smallint");
            }
        }
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
        checkCountdown_ = 1024;
        totalSteps += 1024;
        g_stepNum += 1024;
        g_watchdogSteps.store(g_stepNum, std::memory_order_relaxed);

        // Sampling profiler tick (installed by primitiveProfileStart).
        // Counter is "interrupt checks" in Cog terminology — each periodic
        // check here counts as one.  When it hits 0, snapshot active
        // process + current method (if primitive) and signal sem.
        if (__builtin_expect(profileInterval_ > 0, 0)) {
            if (--profileCounter_ <= 0) {
                profileCounter_ = profileInterval_;
                profileSample_ = getActiveProcess();
                // Current method is the primitive if any (method_ holds it
                // during its fallback; for non-prim methods it holds the
                // bytecode method).  Approximate what "last primitive"
                // means — stock Cog distinguishes, we sample whichever
                // method is currently executing.
                profilePrimitive_ = method_;
                if (profileSemaphore_.isObject() && !profileSemaphore_.isNil()) {
                    synchronousSignal(profileSemaphore_);
                }
            }
        }

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
            // Force re-check immediately after the consumer runs. Without this,
            // tight loops like `[] repeat` (bytecodes `E1 FF ED FC`) whose length
            // evenly divides 1024 lock alignment to E1 forever, starving timer
            // checks. Setting checkCountdown_=1 means the next DISPATCH_NEXT
            // after the consumer decrements to 0 and re-enters periodic_checks
            // with inExtension_=false.
            checkCountdown_ = 1;
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

        // -- Scavenge safe point (young-gen) --
        if (__builtin_expect(memory_.needsScavenge(), 0)) {
            memory_.clearScavengeFlag();
            if (!g_debug.ygNoScavenge) {
                prepareForGC();
                memory_.scavenge();
                afterGC();
            }
        }

        // -- GC safe point --
        if (__builtin_expect(memory_.needsCompactGC(), 0)) {
            memory_.clearCompactGCFlag();
            memory_.fullGC(/* skipEphemerons */ true);
            flushMethodCache();
        }

        // Finalization deferred signal: NOT consumed here.  Fires only at the
        // NEXT activateMethod entry (after pushing caller's args) or at
        // primitiveWait entry (for testFinalization's `sema wait` pattern).
        // Firing here — at every step() — would preempt testClearing's
        // `dict size` read before its value reached the argument stack.

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
                // Don't let heartbeat preempt finalization drain.
                // FinalizationProcess wakes (P50) and forks a P51 worker
                // that runs mournLoopWith:.  A mid-drain preemption leaves
                // WeakKeyDictionary entries partially processed, causing
                // races in testClearing / testFinalize.  Keep the force-
                // yield flag set (handle on next tick) until mournQueue is
                // empty.
                Oop active = getActiveProcess();
                int pri = safeProcessPriority(active);
                bool inFinalizer = (pri == 50 || pri == 51) &&
                                   memory_.mournQueueSize() > 0;
                if (!inFinalizer) {
                    forceYield_.store(false, std::memory_order_release);
                    handleForceYield();
                }
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
    // Periodic diagnostic: default 10s, but PHARO_DIAG_FAST=1 cuts to 1s
    // for finding suspended high-pri processes during chain-loop hangs.
    {
        static int diagInterval = std::getenv("PHARO_DIAG_FAST") != nullptr ? 1 : 10;
        static auto lastDiagTime = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastDiagTime).count();
        if (elapsed >= diagInterval) {
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
            // Scheduler queue diagnostic: dump all ready processes across priorities.
            // Helps diagnose starvation/deadlock where active looks fine but a higher-
            // priority process is blocked on a semaphore that will never signal.
            try {
                Oop schedAssoc = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);
                Oop scheduler = memory_.fetchPointer(1, schedAssoc);
                Oop schedLists = memory_.fetchPointer(SchedulerProcessListsIndex, scheduler);
                int maxPri = (int)schedLists.asObjectPtr()->slotCount();
                Oop nilO = memory_.nil();
                for (int pri = maxPri; pri >= 1; pri--) {
                    Oop procList = memory_.fetchPointer(pri - 1, schedLists);
                    if (!procList.isObject() || procList.rawBits() == nilO.rawBits()) continue;
                    Oop first = memory_.fetchPointer(LinkedListFirstLinkIndex, procList);
                    if (!first.isObject() || first.rawBits() == nilO.rawBits()) continue;
                    Oop p = first;
                    int idx = 0;
                    while (p.isObject() && p.rawBits() != nilO.rawBits() && idx < 8) {
                        // Get top frame's method if suspended context is a Context
                        std::string topSel = "?";
                        std::string topRcvrCls = "?";
                        Oop susp = memory_.fetchPointer(ProcessSuspendedContextIndex, p);
                        if (susp.isObject() && susp.rawBits() != nilO.rawBits()) {
                            // Context has instVars: sender(0) pc(1) stackp(2) method(3) ...
                            Oop mth = memory_.fetchPointer(3, susp);
                            if (mth.isObject() && mth.rawBits() != nilO.rawBits() &&
                                memory_.isValidPointer(mth)) {
                                topSel = memory_.selectorOf(mth);
                            }
                            Oop rcvr = memory_.fetchPointer(5, susp);
                            if (rcvr.isObject() && rcvr.rawBits() != nilO.rawBits() &&
                                memory_.isValidPointer(rcvr))
                                topRcvrCls = memory_.classNameOf(rcvr);
                            else if (rcvr.isSmallInteger()) topRcvrCls = "SmallInteger";
                        } else {
                            topSel = "(nil ctx)";
                        }
                        fprintf(stderr, "[DIAG-QUEUE] P%d proc=0x%llx susp=%s>>%s\n",
                                pri, (unsigned long long)p.rawBits(),
                                topRcvrCls.c_str(), topSel.c_str());
                        // Follow next link
                        Oop nxt = memory_.fetchPointer(ProcessNextLinkIndex, p);
                        if (nxt.isObject() && nxt.rawBits() != nilO.rawBits() &&
                            nxt.rawBits() != p.rawBits()) {
                            p = nxt;
                        } else {
                            break;
                        }
                        idx++;
                    }
                }
            } catch (...) {
                fprintf(stderr, "[DIAG-QUEUE] (enumeration failed)\n");
            }

            // Every DIAG cycle (10s), unblock any process stuck in
            // SessionManager>>snapshot:andQuit: on a dead Semaphore.  This is
            // the safety-net for the headless-eval deadlock — see
            // unblockStuckSnapshotCallers() and docs/fixed_priority_workarounds.md.
            try {
                unblockStuckSnapshotCallers();
            } catch (...) {
                fprintf(stderr, "[UNBLOCK-SNAPSHOT] (failed)\n");
            }

            // PHARO_PROC_DUMP=1 — full enumeration of every Process
            // instance in the heap, including those waiting on
            // Semaphores (invisible to the scheduler-queue walk above).
            // Needed to diagnose deadlocks where a higher-priority
            // process is blocked on a semaphore that nobody signals.
            static bool procDumpInit = false;
            static bool procDump = false;
            if (!procDumpInit) {
                procDumpInit = true;
                procDump = std::getenv("PHARO_PROC_DUMP") != nullptr;
            }
            if (procDump) {
                try {
                    Oop active = getActiveProcess();
                    uint32_t processClsIdx = 0;
                    if (active.isObject() && active.rawBits() > 0x10000) {
                        processClsIdx = active.asObjectPtr()->classIndex();
                    }
                    fprintf(stderr, "[PROC-DUMP] active=0x%llx cls=%u (Process class)\n",
                            (unsigned long long)active.rawBits(), processClsIdx);
                    size_t procCount = 0;
                    Oop nilO = memory_.nil();
                    Oop o = memory_.firstObject();
                    while (o.isObject() && o.rawBits() != 0) {
                        ObjectHeader* h = o.asObjectPtr();
                        if (h->classIndex() == processClsIdx && processClsIdx != 0) {
                            procCount++;
                            Oop prioOop = memory_.fetchPointer(ProcessPriorityIndex, o);
                            int prio = prioOop.isSmallInteger() ?
                                (int)prioOop.asSmallInteger() : -1;
                            Oop myList = memory_.fetchPointer(ProcessMyListIndex, o);
                            const char* state;
                            std::string listCls = "?";
                            if (!myList.isObject() || myList.rawBits() == nilO.rawBits()) {
                                state = (o.rawBits() == active.rawBits())
                                    ? "ACTIVE" : "terminated/running";
                            } else {
                                listCls = memory_.classNameOf(myList);
                                if (listCls == "Semaphore") state = "on-sem";
                                else if (listCls == "ProcessList" || listCls == "LinkedList")
                                    state = "ready";
                                else state = "on-list";
                            }
                            // Decode suspended-context top frame
                            std::string topSel = "(nil)";
                            std::string topCls = "?";
                            Oop susp = memory_.fetchPointer(ProcessSuspendedContextIndex, o);
                            if (susp.isObject() && susp.rawBits() != nilO.rawBits() &&
                                memory_.isValidPointer(susp)) {
                                Oop mth = memory_.fetchPointer(3, susp);
                                if (mth.isObject() && mth.rawBits() != nilO.rawBits() &&
                                    memory_.isValidPointer(mth))
                                    topSel = memory_.selectorOf(mth);
                                Oop rcvr = memory_.fetchPointer(5, susp);
                                if (rcvr.isObject() && rcvr.rawBits() != nilO.rawBits() &&
                                    memory_.isValidPointer(rcvr))
                                    topCls = memory_.classNameOf(rcvr);
                                else if (rcvr.isSmallInteger()) topCls = "SmallInteger";
                                else if (rcvr.isNil()) topCls = "nil";
                            }
                            fprintf(stderr, "[PROC-DUMP] P%d proc=0x%llx %s list=0x%llx(%s) top=%s>>%s\n",
                                    prio, (unsigned long long)o.rawBits(), state,
                                    (unsigned long long)myList.rawBits(), listCls.c_str(),
                                    topCls.c_str(), topSel.c_str());
                        }
                        o = memory_.objectAfter(o);
                        if (procCount > 256) break;  // safety cap
                    }
                    fprintf(stderr, "[PROC-DUMP] total=%zu\n", procCount);
                } catch (...) {
                    fprintf(stderr, "[PROC-DUMP] (enumeration failed)\n");
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

            // P51 finalization worker must complete mournLoopWith:
            // uninterrupted — aging it out mid-drain leaves
            // WeakKeyDictionary entries unmourned.
            // FinalizationProcess>>finalizationProcess forks the
            // worker at `activePriority + 1` = 50 + 1 = 51.
            bool isFinalizer =
                (activePriority == 51 && memory_.mournQueueSize() > 0);

            if (!isFinalizer && nextProcess.rawBits() == nilObj.rawBits() &&
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
            int64_t firedDeadline = nextWakeupUsec_;
            lastKnownTimerSemaphore_ = semaphore;  // save for recovery
            timerSemaphore_ = Oop::nil();
            nextWakeupUsec_ = INT64_MAX;
            lastTimerSignalTime_ = std::chrono::steady_clock::now();
            timerWasArmed_ = false;
            schedulerDeathLogged_ = false;
            if (g_debug.delayDebug) {
                fprintf(stderr, "[DELAY-FIRE] cur=%lld deadline=%lld lateUs=%lld sema=0x%llx\n",
                        (long long)currentUsec, (long long)firedDeadline,
                        (long long)(currentUsec - firedDeadline),
                        (unsigned long long)semaphore.rawBits());
            }
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

void Interpreter::unblockStuckSnapshotCallers() {
    // Pharo's `SessionManager >> snapshot:andQuit:` forks a P79 worker that
    // does the actual save/quit, and the caller does `wait wait` on a fresh
    // Semaphore.  In a normal run the worker eventually calls `wait signal`
    // after the snapshot primitive, unblocking the caller.
    //
    // In our headless-eval + eager-JIT (PHARO_JIT_DEFER=0) path we've seen
    // the worker's chain not reach `wait signal`: the worker becomes the
    // new active process, startup handlers fire, control never returns to
    // the fork-block tail.  Net effect: the caller stays suspended
    // forever on a Semaphore nobody signals — the eval runs, 'result'
    // gets printed... and then the VM spins indefinitely.
    //
    // Since we just loaded the image (or are headless-exiting), the
    // snapshot either already happened or doesn't matter.  Signaling
    // the caller's wait Semaphore unblocks it so it returns from
    // `snapshot:andQuit:` and the exit path completes (primitiveQuit
    // eventually fires, setting running_=false, loop exits).
    //
    // Safe because:
    //   1. We only signal Semaphores, not scheduler ready queues.
    //   2. We only touch processes whose top-frame is exactly
    //      `snapshot:andQuit:` — a very narrow window.
    //   3. `synchronousSignal` is idempotent: if the waiter has already
    //      been processed it's a no-op.
    //
    // Idempotency guard: once we've signaled a process-oop we remember it,
    // so we don't re-signal on every DIAG cycle.
    static std::unordered_set<uint64_t> alreadySignaled;

    Oop active = getActiveProcess();
    uint32_t processClsIdx = 0;
    if (active.isObject() && active.rawBits() > 0x10000) {
        processClsIdx = active.asObjectPtr()->classIndex();
    }
    if (processClsIdx == 0) return;

    Oop nilO = memory_.nil();
    Oop o = memory_.firstObject();
    int scanned = 0;
    while (o.isObject() && o.rawBits() != 0) {
        ObjectHeader* h = o.asObjectPtr();
        if (h->classIndex() == processClsIdx) {
            uint64_t procBits = o.rawBits();
            if (alreadySignaled.find(procBits) == alreadySignaled.end()) {
                Oop myList = memory_.fetchPointer(ProcessMyListIndex, o);
                Oop susp = memory_.fetchPointer(ProcessSuspendedContextIndex, o);
                if (myList.isObject() && myList.rawBits() != nilO.rawBits() &&
                    susp.isObject() && susp.rawBits() != nilO.rawBits() &&
                    memory_.isValidPointer(myList) &&
                    memory_.isValidPointer(susp)) {
                    std::string listCls = memory_.classNameOf(myList);
                    if (listCls == "Semaphore") {
                        Oop mth = memory_.fetchPointer(3, susp);
                        if (mth.isObject() && mth.rawBits() != nilO.rawBits() &&
                            memory_.isValidPointer(mth)) {
                            std::string sel = memory_.selectorOf(mth);
                            if (sel == "snapshot:andQuit:") {
                                fprintf(stderr,
                                        "[UNBLOCK-SNAPSHOT] signal sem=0x%llx "
                                        "for stuck proc=0x%llx\n",
                                        (unsigned long long)myList.rawBits(),
                                        (unsigned long long)procBits);
                                synchronousSignal(myList);
                                alreadySignaled.insert(procBits);
                            }
                        }
                    }
                }
            }
        }
        o = memory_.objectAfter(o);
        if (++scanned > 10000) break;  // safety cap
    }
}

void Interpreter::synchronousSignal(Oop semaphore) {
    Oop firstLink = memory_.fetchPointer(LinkedListFirstLinkIndex, semaphore);

    // PHARO_SEM_SIGNAL_TRACE=1 — log every synchronousSignal with a waiter.
    // Default logs first 50 only; flag raises to 5000 for scheduler-hang
    // investigations where the first 50 are consumed by normal startup.
    static bool semTraceInit = false;
    static int signalLogCap = 50;
    if (!semTraceInit) {
        semTraceInit = true;
        if (std::getenv("PHARO_SEM_SIGNAL_TRACE")) signalLogCap = 5000;
    }
    static int signalLog = 0;
    if (signalLog < signalLogCap) {
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
    // Signal FinalizationSemaphore via synchronousSignal, which will
    // transferTo FP if it has higher priority than the active process.
    // For the testClearing-style case the caller is mid-activateMethod of
    // `assert:equals:` — its args (pre-drain tally) are already on the
    // operand stack, so preemption here doesn't disturb them.
    //
    // For the testFinalization-style case the caller is inside primitiveWait.
    // primitiveWait must flush this signal AFTER suspending itself on the
    // waitSemaphore, not before — see primitiveWait for the two-step
    // sequence.  The top-level convention: if primitiveWait wants to defer,
    // it calls signalFinalizationIfNeededDeferred() which just queues into
    // ready without transferTo.
    size_t pending = memory_.pendingFinalizationSignals();
    if (pending <= 0) return;

    memory_.clearPendingFinalizationSignals();

    Oop sema = memory_.specialObject(SpecialObjectIndex::TheFinalizationSemaphore);
    if (!sema.isObject() || sema == memory_.nil()) return;

    finalizationSignalCount_++;
    finalizationPendingTotal_ += pending;
    lastFinalizationSignalTime_ = std::chrono::steady_clock::now();

    if (g_debug.gcEphDebug) {
        Oop firstLink = memory_.fetchPointer(LinkedListFirstLinkIndex, sema);
        Oop excessOop = memory_.fetchPointer(SemaphoreExcessSignalsIndex, sema);
        bool hasWaiter = !(firstLink.isNil() || firstLink.rawBits() == memory_.nil().rawBits());
        fprintf(stderr, "[SIG-FIN] #%zu pending=%zu total=%zu sema=0x%llx hasWaiter=%d excess=%lld\n",
                finalizationSignalCount_, pending, finalizationPendingTotal_,
                (unsigned long long)sema.rawBits(), (int)hasWaiter,
                (long long)(excessOop.isSmallInteger() ? excessOop.asSmallInteger() : -1));
    }

    synchronousSignal(sema);
}

void Interpreter::signalFinalizationIfNeededDeferred() {
    // Variant for primitiveWait: moves the FinalizationSemaphore waiter to
    // its priority's ready queue without transferTo.  The caller (primitiveWait)
    // is about to suspend on its own waitSemaphore; doing synchronousSignal
    // here would transferTo FP while primitiveWait's own suspension logic is
    // still mid-flight, corrupting the wait list.
    size_t pending = memory_.pendingFinalizationSignals();
    if (pending <= 0) return;

    memory_.clearPendingFinalizationSignals();

    Oop sema = memory_.specialObject(SpecialObjectIndex::TheFinalizationSemaphore);
    if (!sema.isObject() || sema == memory_.nil()) return;

    finalizationSignalCount_++;
    finalizationPendingTotal_ += pending;
    lastFinalizationSignalTime_ = std::chrono::steady_clock::now();

    Oop firstLink = memory_.fetchPointer(LinkedListFirstLinkIndex, sema);
    bool hasWaiter = !(firstLink.isNil() || firstLink.rawBits() == memory_.nil().rawBits());

    if (hasWaiter) {
        Oop waiter = removeFirstLinkOfList(sema);
        if (waiter.isObject() && !waiter.isNil()) {
            putToSleep(waiter);
        }
    } else {
        Oop excessOop = memory_.fetchPointer(SemaphoreExcessSignalsIndex, sema);
        int64_t excess = excessOop.isSmallInteger() ? excessOop.asSmallInteger() : 0;
        memory_.storePointer(SemaphoreExcessSignalsIndex, sema,
                             Oop::fromSmallInteger(excess + 1));
    }
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
        callbackContextStack_[callbackDepth_] = vmcc;
        callbackHandlerStack_[callbackDepth_] = memory_.nil();
        callbackDepth_++;
    }

    // 5. Signal callback semaphore to wake handler process
    if (g_debug.callbackDebug) {
        fflush(stdout);
        fprintf(stderr, "[CALLBACK-ENTER] semIdx=%d vmcc=%p\n",
                g_callbackSemaphoreIndex, (void*)vmcc);
        fflush(stderr);
    }
    if (g_callbackSemaphoreIndex > 0) {
        if (g_debug.callbackDebug) {
            Oop semTable = memory_.specialObject(SpecialObjectIndex::ExternalObjectsArray);
            if (semTable.isObject() && !semTable.isNil()) {
                size_t idx = static_cast<size_t>(g_callbackSemaphoreIndex - 1);
                if (idx < memory_.slotCountOf(semTable)) {
                    Oop semaphore = memory_.fetchPointer(idx, semTable);
                    Oop firstLink = memory_.nil();
                    if (semaphore.isObject() && !semaphore.isNil()) {
                        firstLink = memory_.fetchPointer(LinkedListFirstLinkIndex, semaphore);
                    }
                    fprintf(stderr, "[CALLBACK-SEM] sem=0x%llx firstLink=0x%llx (waiter %s)\n",
                            (unsigned long long)semaphore.rawBits(),
                            (unsigned long long)firstLink.rawBits(),
                            (firstLink.isNil() || firstLink.rawBits() == memory_.nil().rawBits())
                              ? "NONE" : "present");
                    fflush(stderr);
                }
            }
        }
        // CUSTOM callback-signal path: we cannot use the generic
        // signalSemaphoreDirectly because synchronousSignal calls putToSleep
        // on the active process, which would add the test process to the
        // ready queue. The test process is ALREADY suspended via
        // SuspendedProcessInCallout (step 3); double-booking causes
        // wakeHighestPriority to later pull the test process out of the
        // ready queue and run it concurrently with the (still-active)
        // restored caller, leading to intermittent hangs and failures.
        //
        // Instead:
        //   - If a waiter is on the semaphore, remove it and transferTo it
        //     WITHOUT calling putToSleep on the current active process.
        //   - If no waiter, increment excessSignals, then pick the next
        //     ready process via wakeHighestPriority.
        bool transferred = false;
        Oop semTable = memory_.specialObject(SpecialObjectIndex::ExternalObjectsArray);
        if (semTable.isObject() && !semTable.isNil()) {
            size_t idx = static_cast<size_t>(g_callbackSemaphoreIndex - 1);
            if (idx < memory_.slotCountOf(semTable)) {
                Oop semaphore = memory_.fetchPointer(idx, semTable);
                if (semaphore.isObject() && !semaphore.isNil()) {
                    Oop firstLink = memory_.fetchPointer(LinkedListFirstLinkIndex, semaphore);
                    bool hasWaiter = !(firstLink.isNil() || firstLink.rawBits() == memory_.nil().rawBits());
                    if (hasWaiter) {
                        int waiterPri = safeProcessPriority(firstLink);
                        if (waiterPri >= 0) {
                            Oop waiter = removeFirstLinkOfList(semaphore);
                            // NOTE: intentionally skip putToSleep(activeProcess) —
                            // activeProcess is suspended via SuspendedProcessInCallout.
                            // Skip transferTo — step 1+2 already materialized and
                            // stored the active process's context. Calling
                            // materializeFrameStack again would duplicate work or
                            // double-save contexts. Inline setActive + executeFromContext.
                            setActiveProcess(waiter);
                            if (callbackDepth_ > 0) {
                                callbackHandlerStack_[callbackDepth_ - 1] = waiter;
                            }
                            Oop waiterCtx = memory_.fetchPointer(ProcessSuspendedContextIndex, waiter);
                            memory_.storePointer(ProcessSuspendedContextIndex, waiter, memory_.nil());
                            stackPointer_ = stackBase_;
                            frameDepth_ = 0;
                            executeFromContext(waiterCtx);
                            transferred = true;
                        }
                    } else {
                        // No waiter: bump excessSignals.
                        Oop excessOop = memory_.fetchPointer(SemaphoreExcessSignalsIndex, semaphore);
                        int64_t excess = excessOop.isSmallInteger() ? excessOop.asSmallInteger() : 0;
                        memory_.storePointer(SemaphoreExcessSignalsIndex, semaphore,
                                            Oop::fromSmallInteger(excess + 1));
                    }
                }
            }
        }

        if (g_debug.callbackDebug) {
            fprintf(stderr, "[CALLBACK-CUSTOM-SIGNAL] transferred=%d\n", (int)transferred);
            fflush(stderr);
        }

        // If we didn't transfer via the semaphore (no waiter), switch off
        // the now-suspended test process to something ready. Without this,
        // step() would re-execute the test process that's already in
        // SuspendedProcessInCallout.
        if (!transferred) {
            Oop readyProcess = wakeHighestPriority();
            if (readyProcess.isObject() && !readyProcess.isNil()) {
                setActiveProcess(readyProcess);
                Oop ctx = memory_.fetchPointer(ProcessSuspendedContextIndex, readyProcess);
                memory_.storePointer(ProcessSuspendedContextIndex, readyProcess, memory_.nil());
                stackPointer_ = stackBase_;
                frameDepth_ = 0;
                executeFromContext(ctx);
            }
        }
    }

    // 7. Run a NESTED interpret loop on the C stack.
    //    primitiveCallbackReturn sets pendingCallbackReturn_ instead of doing
    //    siglongjmp directly, so the Smalltalk caller can release mutexes and
    //    clean up. We detect the flag after each batch of steps (giving the
    //    Smalltalk code time to finish), restore the original process, and
    //    siglongjmp back to the C trampoline.
    long nestedStepCount = 0;
    static constexpr long kCallbackTimeout = 10000000; // 10M steps ~1s
    bool cbDbg = g_debug.callbackDebug;
    long lastDbgStep = 0;
    while (running_) {
        for (int batch = 0; batch < 1000 && running_; batch++) {
            step();
            nestedStepCount++;
        }
        // Dense trace for first 10k steps, sparse after.
        long dbgInterval = nestedStepCount < 10000 ? 1000 : 1000000;
        if (cbDbg && nestedStepCount - lastDbgStep >= dbgInterval) {
            Oop activeProc = getActiveProcess();
            Oop prioOop = activeProc.isObject() && !activeProc.isNil()
                ? memory_.fetchPointer(ProcessPriorityIndex, activeProc) : memory_.nil();
            int activePri = prioOop.isSmallInteger() ? (int)prioOop.asSmallInteger() : -1;
            std::string rcvrClass = "?";
            if (receiver_.isObject() && !receiver_.isNil())
                rcvrClass = memory_.classNameOf(receiver_);
            else if (receiver_.isSmallInteger()) rcvrClass = "SmallInteger";
            std::string selector = "?";
            if (method_.isObject() && !method_.isNil()) {
                int numLits = (int)memory_.numLiteralsOf(method_);
                if (numLits >= 2) {
                    Oop penLit = memory_.fetchPointer(numLits - 2, method_);
                    if (penLit.isObject() && !penLit.isNil()) {
                        int fmt = (int)penLit.asObjectPtr()->format();
                        Oop symOop = (fmt >= 16) ? penLit
                                                 : memory_.fetchPointer(1, penLit);
                        if (symOop.isObject() && !symOop.isNil()
                            && (int)symOop.asObjectPtr()->format() >= 16) {
                            size_t sz = memory_.byteSizeOf(symOop);
                            if (sz < 200) {
                                selector.clear();
                                for (size_t i = 0; i < sz && i < 60; i++)
                                    selector += (char)memory_.fetchByte(i, symOop);
                            }
                        }
                    }
                }
            }
            fprintf(stderr, "[CALLBACK-PROGRESS] steps=%ld pending=%d active=0x%llx pri=%d %s>>%s fd=%d\n",
                    nestedStepCount, (int)(pendingCallbackReturn_ != nullptr),
                    (unsigned long long)activeProc.rawBits(), activePri,
                    rcvrClass.c_str(), selector.c_str(), frameDepth_);
            try {
                for (size_t fi = 1; fi <= 15 && fi <= frameDepth_; fi++) {
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
                    fprintf(stderr, "[CB-STACK]   [-%zu] %s>>%s\n", fi, cname.c_str(), ssel.c_str());
                }
            } catch (...) { fprintf(stderr, "[CB-STACK]   (failed)\n"); }
            fflush(stderr);
            lastDbgStep = nestedStepCount;
        }

        // Timeout: if the callback handler never calls primitiveCallbackReturn
        // (e.g., because an error killed the handler process), abandon the
        // callback to prevent infinite spinning.
        if (nestedStepCount >= kCallbackTimeout && !pendingCallbackReturn_) {
            if (g_debug.callbackDebug) {
                fprintf(stderr, "[CALLBACK-TIMEOUT] nestedStepCount=%ld vmcc=%p\n",
                        nestedStepCount, (void*)vmcc);
                fflush(stderr);
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

            // Return 0 to C by jumping back to the trampoline.
            // C++ exception (not siglongjmp): unwinds through all C++
            // RAII destructors so any asmjit ProtectJitReadWriteScope
            // active higher up runs cleanup (restoring MAP_JIT W^X to
            // executable).  See pharo::CallbackComplete declaration.
            throw pharo::CallbackComplete{};
        }

        // Check for deferred callback return AFTER the batch completes.
        if (pendingCallbackReturn_) {
            if (g_debug.callbackDebug) {
                fprintf(stderr, "[CALLBACK-RETURN] nestedStepCount=%ld vmcc=%p\n",
                        nestedStepCount, (void*)pendingCallbackReturn_);
                fflush(stderr);
            }
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

            // Save the current process's execution state and re-queue it,
            // but ONLY if the active process is still the callback handler.
            // If the handler already called `sem wait` during cooldown, it
            // transferred away and the active process is now something else
            // (e.g., the next-highest-priority process legitimately running).
            // We must NOT putToSleep that process — it's not lost, it's
            // running. Doing so corrupts its state and causes intermittent
            // hangs (observed: UI process ending up in an infinite FFI loop
            // calling SDL_GetVersion when its context was double-queued).
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
                // primitiveCallbackReturn already decremented callbackDepth_,
                // so the just-popped handler is at slot callbackDepth_
                // (not callbackDepth_ - 1).
                Oop savedHandler = (callbackDepth_ < MaxCallbackDepth)
                    ? callbackHandlerStack_[callbackDepth_]
                    : memory_.nil();
                if (callbackDepth_ < MaxCallbackDepth) {
                    callbackHandlerStack_[callbackDepth_] = memory_.nil();
                }
                bool stillHandler = savedHandler.isObject() &&
                                    !savedHandler.isNil() &&
                                    savedHandler.rawBits() == currentProcess.rawBits();
                if (stillHandler) {
                    putToSleep(currentProcess);
                }
                if (g_debug.callbackDebug) {
                    fprintf(stderr, "[CALLBACK-RETURN-REQUEUE] active=0x%llx handler=0x%llx stillHandler=%d\n",
                            (unsigned long long)currentProcess.rawBits(),
                            (unsigned long long)savedHandler.rawBits(),
                            (int)stillHandler);
                    fflush(stderr);
                }
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

            // Return to C (callbackClosureHandler).  Same rationale as
            // the other callsite — exception runs RAII destructors so
            // asmjit's W^X scope (if active higher up) cleans up.
            throw pharo::CallbackComplete{};
        }

        if (hasPendingSignals() && !inExtension_) {
            processPendingSignals();
        }
        if (!inExtension_) {
            checkTimerSemaphore();
        }
    }
    if (g_debug.callbackDebug) {
        fflush(stdout);
        fprintf(stderr, "[CALLBACK-EXIT-LOOP] running_=%d nestedStepCount=%ld vmcc=%p\n",
                (int)running_, nestedStepCount, (void*)vmcc);
        fflush(stderr);
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

    // Finalization deferred signal: NOT consumed here.  Moved to activateMethod
    // entry and primitiveWait entry so testClearing's `dict size` read (a quick
    // primitive returning pre-drain tally) completes before FP preemption
    // drains WKAs.  See primitiveFullGC.

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

    // 2026-04-29 (B7 fix): when a Sista helper-send is driving step()
    // to completion, defer periodic checks entirely.  Timer signals,
    // pending semaphores, and preemption can all switch the active
    // process mid-helper, breaking the helper's frameDepth bookkeeping.
    // Defer rather than skip so the work runs as soon as the helper
    // returns.
    if (periodicCheckDue && inSyncSend_) {
        deferredPeriodicCheck_ = true;
    }
    if ((periodicCheckDue || deferredPeriodicCheck_)
        && !inExtension_ && !inSyncSend_) {
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
        // When finalizeDeferred is on, this is handled by
        // backwardBranchInterruptCheck instead.
        if (!g_debug.finalizeDeferred) {
            signalFinalizationIfNeeded();
        }

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
    // Skip during inSyncSend_ — defer until helper-send returns.
    if (!inExtension_ && !inSyncSend_ && hasPendingSignals()) {
        processPendingSignals();
    }

    // Periodic preemption check - every 10000 bytecodes, check if we should
    // yield to a higher-priority or same-priority runnable process.
    // Skip if in extension byte sequence to protect extA_/extB_.
    // Skip during inSyncSend_ — defer until helper-send returns.
    bytecodeCount_++;
    if (bytecodeCount_ % 10000 == 0 && !inExtension_ && !inSyncSend_) {
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
    bool isSend = jit::SistaV1::isSendBytecode(bytecode);

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
    case jit::SistaV1::PushReceiver: push(receiver_); break;
    case jit::SistaV1::PushTrue:     push(memory_.trueObject()); break;
    case jit::SistaV1::PushFalse:    push(memory_.falseObject()); break;
    case jit::SistaV1::PushNil:      push(memory_.nil()); break;

    case jit::SistaV1::PushZero:     push(Oop::fromSmallInteger(0)); break;
    case jit::SistaV1::PushOne:      push(Oop::fromSmallInteger(1)); break;
    case jit::SistaV1::PushThisContext: {
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
    case jit::SistaV1::Dup: push(stackTop()); break;

    // 0x54-0x57: UNASSIGNED
    case 0x54: case 0x55: case 0x56: case 0x57:
        break;

    case jit::SistaV1::ReturnReceiver:
        push(receiver_);
        returnFromMethod();
        break;
    case jit::SistaV1::ReturnTrue:
        push(memory_.trueObject());
        returnFromMethod();
        break;
    case jit::SistaV1::ReturnFalse:
        push(memory_.falseObject());
        returnFromMethod();
        break;
    // 0x5B: Return nil
    case jit::SistaV1::ReturnNil:
        push(memory_.nil());
        returnFromMethod();
        break;
    case jit::SistaV1::ReturnTop:
        returnFromMethod();
        break;
    case jit::SistaV1::BlockReturnNil: {
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
    case jit::SistaV1::BlockReturnTop: {
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
    case jit::SistaV1::Pop:
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

    case jit::SistaV1::ExtendA: { // Extend A (unsigned)
        uint8_t extByte = fetchByte();
        extA_ = (extA_ << 8) | extByte;
        inExtension_ = true;
        break;
    }
    case jit::SistaV1::ExtendB: { // Extend B (signed)
        uint8_t extByte = fetchByte();
        if (extByte >= 128)
            extB_ = (extB_ << 8) | extByte | 0xFFFFFF00;
        else
            extB_ = (extB_ << 8) | extByte;
        inExtension_ = true;
        break;
    }
    case jit::SistaV1::ExtPushRecvVar: { // Push Receiver Variable #i (+ extA * 256)
        uint8_t indexByte = fetchByte();
        int fullIndex = (extA_ << 8) | indexByte;
        extA_ = 0;
        pushReceiverVariable(fullIndex);
        break;
    }
    case jit::SistaV1::ExtPushLitVar: { // Push Literal Variable #i (+ extA * 256)
        uint8_t indexByte = fetchByte();
        int fullIndex = (extA_ << 8) | indexByte;
        extA_ = 0;
        pushLiteralVariable(fullIndex);
        break;
    }
    case jit::SistaV1::ExtPushLitConst: { // Push Literal Constant #i (+ extA * 256)
        uint8_t indexByte = fetchByte();
        int fullIndex = (extA_ << 8) | indexByte;
        extA_ = 0;
        pushLiteralConstant(fullIndex);
        break;
    }
    case jit::SistaV1::ExtPushTemp: { // Push Temporary Variable #i
        uint8_t indexByte = fetchByte();
        pushTemporary(indexByte);
        break;
    }
    case 0xE6: // UNASSIGNED (was pushNClosureTemps)
        fetchByte();
        break;
    case jit::SistaV1::PushArray: { // Push Array (j=0) or Pop into Array (j=1)
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

    case jit::SistaV1::PushInteger: { // Push Integer #i (+ extB * 256, signed)
        uint8_t intByte = fetchByte();
        int value = intByte + static_cast<int>(static_cast<unsigned int>(extB_) << 8);
        extB_ = 0;
        push(Oop::fromSmallInteger(value));
        break;
    }
    case jit::SistaV1::PushCharacter: { // Push Character #i (+ extB * 256)
        uint8_t charByte = fetchByte();
        int codePoint = charByte + static_cast<int>(static_cast<unsigned int>(extB_) << 8);
        extB_ = 0;
        push(Oop::fromCharacter(codePoint));
        break;
    }
    case jit::SistaV1::ExtSend: { // Send Literal Selector #iiiii (+ extA*32) with jjj (+ extB*8) args
        uint8_t desc = fetchByte();
        int selectorIndex = ((extA_ << 5) | (desc >> 3)) & 0xFFFF;
        int numArgs = ((extB_ << 3) | (desc & 0x07)) & 0xFF;
        extA_ = 0;
        extB_ = 0;
        sendSelector(literal(selectorIndex), numArgs);
        break;
    }
    case jit::SistaV1::ExtSuperSend: { // Send To Superclass
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
    case jit::SistaV1::InlinedPrimitive: { // Call Mapped Inlined Primitive #i
        uint8_t primByte = fetchByte();
        (void)primByte; // Interpreter fallback — JIT handles these
        break;
    }
    case jit::SistaV1::ExtJump: { // Jump #i (+ extB * 256, signed)
        uint8_t offsetByte = fetchByte();
        int offset = offsetByte + static_cast<int>(static_cast<unsigned int>(extB_) << 8);
        extB_ = 0;
        instructionPointer_ += offset;
        if (offset < 0) {
#if PHARO_JIT_ENABLED
            tryOSRAtBackwardJump();
#endif
            backwardBranchInterruptCheck();
        }
        break;
    }
    case jit::SistaV1::ExtJumpTrue: { // Pop and Jump On True #i (+ extB * 256)
        uint8_t offsetByte = fetchByte();
        int offset = offsetByte + static_cast<int>(static_cast<unsigned int>(extB_) << 8);
        extA_ = 0;
        extB_ = 0;
        Oop value = pop();
        if (isTrue(value)) {
            instructionPointer_ += offset;
            if (offset < 0) {
#if PHARO_JIT_ENABLED
                tryOSRAtBackwardJump();
#endif
                backwardBranchInterruptCheck();
            }
        } else if (!isFalse(value)) {
            push(value);
            sendMustBeBoolean(value);
        }
        break;
    }
    case jit::SistaV1::ExtJumpFalse: { // Pop and Jump On False #i (+ extB * 256)
        uint8_t offsetByte = fetchByte();
        int offset = offsetByte + static_cast<int>(static_cast<unsigned int>(extB_) << 8);
        extA_ = 0;
        extB_ = 0;
        Oop value = pop();
        if (isFalse(value)) {
            instructionPointer_ += offset;
            if (offset < 0) {
#if PHARO_JIT_ENABLED
                tryOSRAtBackwardJump();
#endif
                backwardBranchInterruptCheck();
            }
        } else if (!isTrue(value)) {
            push(value);
            sendMustBeBoolean(value);
        }
        break;
    }

    // ====== 2-byte bytecodes: Store operations (0xF0-0xF7) ======

    case jit::SistaV1::ExtPopStoreRecv: { // Pop and Store Receiver Variable #i (+ extA * 256)
        uint8_t indexByte = fetchByte();
        int fullIndex = (extA_ << 8) | indexByte;
        extA_ = 0;
        Oop value = pop();
        setReceiverInstVar(fullIndex, value);
        break;
    }
    case jit::SistaV1::ExtPopStoreLitVar: { // Pop and Store Literal Variable #i (+ extA * 256)
        uint8_t indexByte = fetchByte();
        int fullIndex = (extA_ << 8) | indexByte;
        extA_ = 0;
        Oop value = pop();
        Oop assoc = literal(fullIndex);
        if (assoc.isObject())
            memory_.storePointer(1, assoc, value);
        break;
    }
    case jit::SistaV1::ExtPopStoreTemp: { // Pop and Store Temporary Variable #i
        uint8_t indexByte = fetchByte();
        Oop value = pop();
        setTemporary(indexByte, value);
        break;
    }
    case jit::SistaV1::ExtStoreRecv: { // Store Receiver Variable #i (+ extA * 256) - no pop
        uint8_t indexByte = fetchByte();
        int fullIndex = (extA_ << 8) | indexByte;
        extA_ = 0;
        setReceiverInstVar(fullIndex, stackTop());
        break;
    }
    case jit::SistaV1::ExtStoreLitVar: { // Store Literal Variable #i (+ extA * 256) - no pop
        uint8_t indexByte = fetchByte();
        int fullIndex = (extA_ << 8) | indexByte;
        extA_ = 0;
        Oop assoc = literal(fullIndex);
        if (assoc.isObject())
            memory_.storePointer(1, assoc, stackTop());
        break;
    }
    case jit::SistaV1::ExtStoreTemp: { // Store Temporary Variable #i - no pop
        uint8_t indexByte = fetchByte();
        setTemporary(indexByte, stackTop());
        break;
    }
    case 0xF6: // UNASSIGNED
    case 0xF7: // UNASSIGNED
        fetchByte();
        break;

    // ====== 3-byte bytecodes (0xF8-0xFF) ======

    case jit::SistaV1::CallPrimitive: { // Call Primitive
        uint8_t primLowByte = fetchByte();
        uint8_t flagsAndHigh = fetchByte();
        int primIndex = primLowByte | ((flagsAndHigh & 0x1F) << 8);
        (void)primIndex; // Skipped at activation time
        break;
    }
    case jit::SistaV1::PushFullBlock: { // Push FullBlockClosure
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
    case jit::SistaV1::PushClosure: { // Push Closure
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
    case jit::SistaV1::PushTempAtInVec: { // Push Temp At k In Temp Vector At j
        uint8_t tempIndex = fetchByte();
        uint8_t vectorIndex = fetchByte();
        Oop tempVector = temporary(vectorIndex);
        push(tempVector.isObject() ? memory_.fetchPointer(tempIndex, tempVector) : memory_.nil());
        break;
    }
    case jit::SistaV1::StoreTempAtInVec: { // Store Temp At k In Temp Vector At j (no pop)
        uint8_t tempIndex = fetchByte();
        uint8_t vectorIndex = fetchByte();
        Oop tempVector = temporary(vectorIndex);
        if (tempVector.isObject())
            memory_.storePointer(tempIndex, tempVector, stackTop());
        break;
    }
    case jit::SistaV1::PopStoreTempAtInVec: { // Pop and Store Temp At k In Temp Vector At j
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
    // Legacy ReadStream instVar trace — left-over from B5 debugging.
    // Gated on PHARO_RS_READ_TRACE=1 now; was always-on noise before.
    static const bool rsTrace = std::getenv("PHARO_RS_READ_TRACE") != nullptr;
    if (__builtin_expect(rsTrace, 0)) {
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

            // PHARO_NLR_NULL_TRACE diag: log NLR ensure-cleanup that nulls
            // the activeContext_'s sender — this is one of several sites.
            static bool nlrNullTrace = std::getenv("PHARO_NLR_NULL_TRACE") != nullptr;
            if (nlrNullTrace) {
                Oop p = getActiveProcess();
                Oop pri = p.isObject() ? memory_.fetchPointer(ProcessPriorityIndex, p) : Oop::nil();
                long pVal = pri.isSmallInteger() ? pri.asSmallInteger() : -1;
                if (pVal >= 60) {
                    static int n4603 = 0;
                    n4603++;
                    if (n4603 <= 30)
                        fprintf(stderr,
                                "[NLR-NULL@4603] proc-pri=%ld activeCtx=0x%llx senderWas=0x%llx\n",
                                pVal,
                                (unsigned long long)activeContext_.rawBits(),
                                (unsigned long long)senderOfCurrent.rawBits());
                }
            }

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
                    // No valid sender — terminate process.  Diagnostic
                    // (PHARO_TERM_TRACE=1): dump homeCtx info, value
                    // being NLR'd, and active context's method/receiver
                    // so we can find what NLR is killing the bench process.
                    static bool termTrace2 = std::getenv("PHARO_TERM_TRACE") != nullptr;
                    if (termTrace2) {
                        Oop proc = getActiveProcess();
                        Oop pri = memory_.fetchPointer(ProcessPriorityIndex, proc);
                        long pVal = pri.isSmallInteger() ? pri.asSmallInteger() : -1;
                        if (pVal >= 60) {
                            Oop hcMethod = memory_.fetchPointer(3, homeCtx);
                            std::string hsel = hcMethod.isObject()
                                ? memory_.selectorOf(hcMethod) : "?";
                            std::string hcls = "?";
                            {
                                Oop hcRecv = memory_.fetchPointer(5, homeCtx);
                                if (hcRecv.isObject() && hcRecv.rawBits() > 0x10000)
                                    hcls = memory_.classNameOf(hcRecv);
                                else if (hcRecv.isSmallInteger()) hcls = "SmI";
                            }
                            Oop acMethod = activeContext_.isObject()
                                ? memory_.fetchPointer(3, activeContext_) : Oop::nil();
                            std::string asel = acMethod.isObject()
                                ? memory_.selectorOf(acMethod) : "?";
                            std::string vcls = "?";
                            if (value.isSmallInteger()) vcls = "SmI";
                            else if (value.isObject() && value.rawBits() > 0x10000)
                                vcls = memory_.classNameOf(value);
                            fprintf(stderr,
                                    "[TERM-NLR-P%ld] NLR fd=0: home=#%s rcvr=%s "
                                    "homeCtx=0x%llx homeSender=0x%llx(%s) "
                                    "active=#%s value=0x%llx(%s) — terminating\n",
                                    pVal, hsel.c_str(), hcls.c_str(),
                                    (unsigned long long)homeCtx.rawBits(),
                                    (unsigned long long)homeSender.rawBits(),
                                    homeSender.isNil() ? "nil"
                                        : !homeSender.isObject() ? "imm"
                                        : !memory_.isValidPointer(homeSender) ? "INVALID"
                                        : "?",
                                    asel.c_str(),
                                    (unsigned long long)value.rawBits(),
                                    vcls.c_str());
                        }
                    }
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
            handleBenchComplete(value);
            return;
        }

        // When a method returns but has no sender (nil or no valid context),
        // this is the top of the process's execution chain. The correct action
        // is to terminate the process, per Smalltalk spec.
        //
        // First check: if we're at fd=0 and the context's sender is nil,
        // this is a normal process-top return. Terminate immediately without
        // entering the expensive cannotReturn: exception handling path.
        // The cannotReturn: error handling involves copyTo: which recurses
        // O(n) on the context chain length, causing O(n²) behavior that
        // can monopolize the CPU for millions of steps.
        if (frameDepth_ == 0 && activeContext_.isObject() && !activeContext_.isNil()) {
            Oop sender = memory_.fetchPointer(0, activeContext_);
            if (sender.isNil() || sender.rawBits() == 0 || sender.rawBits() == memory_.nil().rawBits()) {
                // Top of process chain — terminate directly.  Diagnostic
                // (PHARO_TERM_TRACE=1): if the to-be-terminated process is
                // at high priority (>= 60), dump value being returned and
                // active context's method/receiver so we can find what
                // sent control to the bench's top context prematurely.
                static bool termTrace = std::getenv("PHARO_TERM_TRACE") != nullptr;
                if (termTrace) {
                    Oop proc = getActiveProcess();
                    Oop pri = memory_.fetchPointer(ProcessPriorityIndex, proc);
                    long pVal = pri.isSmallInteger() ? pri.asSmallInteger() : -1;
                    if (pVal >= 60) {
                        Oop methodOop = memory_.fetchPointer(3, activeContext_);
                        std::string sel = methodOop.isObject()
                            ? memory_.selectorOf(methodOop) : "?";
                        Oop recv = memory_.fetchPointer(5, activeContext_);
                        std::string rcls = recv.isObject() && recv.rawBits() > 0x10000
                            ? memory_.classNameOf(recv) : "imm";
                        // value being returned (the NLR/return value)
                        Oop valOop = value;
                        std::string vcls = "?";
                        if (valOop.isSmallInteger()) vcls = "SmI";
                        else if (valOop.isObject() && valOop.rawBits() > 0x10000)
                            vcls = memory_.classNameOf(valOop);
                        fprintf(stderr,
                                "[TERM-TOP-P%ld] returning value=0x%llx(%s) "
                                "from active=#%s rcvr=%s ctx=0x%llx — "
                                "this is the top context (sender=nil), "
                                "process about to terminate\n",
                                pVal,
                                (unsigned long long)valOop.rawBits(),
                                vcls.c_str(),
                                sel.c_str(),
                                rcls.c_str(),
                                (unsigned long long)activeContext_.rawBits());
                    }
                }
                terminateCurrentProcess();
                if (tryReschedule()) return;
                if (bootstrapStartup()) return;
                stopVM("No runnable processes after top-of-chain return");
                return;
            }
        }

        // Per reference VM spec: send cannotReturn: instead of silently terminating.
        // This gives Smalltalk's exception handling a chance to handle the situation.
        if (activeContext_.isObject() && !activeContext_.isNil()) {
            Oop currentProcess = getActiveProcess();
            if (currentProcess.rawBits() != lastCannotReturnProcess_.rawBits()) {
                cannotReturnCount_ = 0;
                lastCannotReturnProcess_ = currentProcess;
            }
            cannotReturnCount_++;
            if (cannotReturnCount_ <= 2) {
                lastCannotReturnCtx_ = activeContext_;
                // Set a step deadline for error handling.
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
        // Pop frame and push result
        if (!popFrame()) return;  // Process terminated and rescheduled
    }

    // After popping, if execution is still running, push the result
    if (running_) {
        push(value);

        if (__builtin_expect(dispatchTraceLeakOn_, 0) && framePointer_) {
            long long spAboveFP = (long long)(stackPointer_ - framePointer_);
            if (spAboveFP >= 500 && spAboveFP <= 520) {
                ObjectHeader* _mObj = method_.isObject() ? method_.asObjectPtr() : nullptr;
                const uint8_t* _bcBase = nullptr;
                if (_mObj) {
                    Oop _hdr = _mObj->slots()[0];
                    int _nLit = _hdr.isSmallInteger() ? (_hdr.asSmallInteger() & 0x7FFF) : 0;
                    _bcBase = _mObj->bytes() + (1 + _nLit) * 8;
                }
                long long _bcOff = (_bcBase && instructionPointer_) ?
                    (long long)(instructionPointer_ - _bcBase) : -1;
                fprintf(stderr, "[RET-PUSH] fd=%zu sp-fp=%lld val=0x%llx method=#%s bcOff=%lld bc=%02x\n",
                        frameDepth_, spAboveFP, (unsigned long long)value.rawBits(),
                        memory_.selectorOf(method_).c_str(), _bcOff,
                        (instructionPointer_ ? *instructionPointer_ : 0));
            }
        }

        // NOTE: nlrHomeMethod_/nlrValue_ globals are only consumed in the fd=0
        // safety net (for process-switch during ensure: cleanup). For fd>0,
        // NLR continuation after ensure: cleanup is handled by the
        // savedFrames_[].homeFrameDepth mechanism (set by NLR-ENSURE handler in
        // returnFromMethod), which triggers the inline NLR path when ensure:
        // itself returns. A previous implementation hijacked fd>0 returns
        // here based on the globals — that incorrectly fired on ordinary
        // returns during cleanup block execution, skipping remaining cleanup.

#if PHARO_JIT_ENABLED
        // Try to re-enter JIT execution in the caller method.
        // IP is at the bytecode after the send that just returned.
        if (jitRuntime_.isInitialized()) {
            long long _spBefore = 0, _bcBefore = -1;
            if (__builtin_expect(dispatchTraceLeakOn_, 0) && framePointer_) {
                _spBefore = (long long)(stackPointer_ - framePointer_);
                ObjectHeader* _mObj = method_.isObject() ? method_.asObjectPtr() : nullptr;
                if (_mObj) {
                    Oop _hdr = _mObj->slots()[0];
                    int _nLit = _hdr.isSmallInteger() ? (_hdr.asSmallInteger() & 0x7FFF) : 0;
                    const uint8_t* _bcBase = _mObj->bytes() + (1 + _nLit) * 8;
                    if (instructionPointer_)
                        _bcBefore = (long long)(instructionPointer_ - _bcBase);
                }
            }
            // B5 diagnostic: log the return value pushed on the caller's
            // stack when returning INTO decodeBytes:.
            if (g_debug.b5Trace && method_.isObject() &&
                stackPointer_ > framePointer_) {
                std::string sel = memory_.selectorOf(method_);
                if (sel == "decodeBytes:") {
                    Oop top = stackPointer_[-1];
                    fprintf(stderr, "[B5-RESUME] returning into decodeBytes: "
                                   "top=0x%llx (%s) sp=%p fp=%p\n",
                            (unsigned long long)top.rawBits(),
                            top.isSmallInteger() ? "SmallInt"
                              : top.isObject() ? memory_.classNameOf(top).c_str()
                              : "other",
                            stackPointer_, framePointer_);
                }
            }
            tryJITResumeInCaller();
            if (__builtin_expect(dispatchTraceLeakOn_, 0) && framePointer_) {
                long long _spAfter = (long long)(stackPointer_ - framePointer_);
                long long _bcAfter = -1;
                ObjectHeader* _mObj = method_.isObject() ? method_.asObjectPtr() : nullptr;
                if (_mObj) {
                    Oop _hdr = _mObj->slots()[0];
                    int _nLit = _hdr.isSmallInteger() ? (_hdr.asSmallInteger() & 0x7FFF) : 0;
                    const uint8_t* _bcBase = _mObj->bytes() + (1 + _nLit) * 8;
                    if (instructionPointer_)
                        _bcAfter = (long long)(instructionPointer_ - _bcBase);
                }
                if (_spBefore >= 500 && _spBefore <= 520) {
                    fprintf(stderr, "[JITRESUME] fd=%zu method=#%s sp_before=%lld bc_before=%lld sp_after=%lld bc_after=%lld\n",
                            frameDepth_, memory_.selectorOf(method_).c_str(),
                            _spBefore, _bcBefore, _spAfter, _bcAfter);
                }
            }
        }
#endif
    }
}

void Interpreter::returnFromMethod() {
    Oop value = pop();

    // Check if we're executing inside a block (CompiledBlock).
    // If so, a "return from method" (^) should actually return from the HOME method,
    // not just from this block.

    if (frameDepth_ > 0) {
        size_t homeFrame = savedFrames_[frameDepth_ - 1].homeFrameDepth;

        // Inline NLR: homeFrame is a valid saved frame index.
        // homeFrameDepth is set by activateBlock (for block NLR) or by the
        // ensure: NLR handler (for NLR continuation after ensure: cleanup).
        // In both cases, unwind frames to homeFrame and return from it.
        if (homeFrame != SIZE_MAX && homeFrame < frameDepth_) {
            // For blocks, this is the standard NLR (^ inside a block returns
            // from the enclosing method). For ensure: methods, this is the
            // NLR continuation after cleanup — ensure: set homeFrameDepth
            // so the NLR resumes when ensure: does ^ returnValue. In both
            // cases, `value` on the stack is already the correct return
            // value (the block's ^ expr, or ensure:'s returnValue local).
            // Do NOT substitute nlrValue_: it may be stale from an outer
            // paused NLR while this inner block return happens during
            // ensure: cleanup.
            Oop nlrVal = value;
            while (frameDepth_ > homeFrame) {
                if (frameDepth_ > 1) {
                    Oop rm = savedFrames_[frameDepth_ - 1].savedMethod;
                    if (rm.isObject() && !rm.isNil() &&
                        primitiveIndexOf(rm) == 198) {
                        // ensure: frame — pause NLR, run cleanup block.
                        // Save NLR state: both in the frame (robust against
                        // process switches) and as globals (fallback for fd=0).
                        nlrHomeMethod_ = savedFrames_[homeFrame].savedMethod;
                        nlrValue_ = nlrVal;
                        if (!popFrame()) return;
                        push(nlrVal);
                        // Store homeFrame in the ensure: frame's saved state so
                        // returnFromMethod can continue the NLR when ensure:
                        // returns (even if nlrHomeMethod_ gets clobbered by
                        // process switches during cleanup).
                        if (frameDepth_ > 0) {
                            savedFrames_[frameDepth_ - 1].homeFrameDepth = homeFrame;
                        }
                        return;
                    }
                }
                if (!popFrame()) return;
            }
            // At home frame — return from it. Clear NLR state.
            nlrHomeMethod_ = Oop::nil();
            nlrValue_ = Oop::nil();
            returnValue(nlrVal);
            return;
        }

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
                                    if (frameDepth_ > 0) {
                                        savedFrames_[frameDepth_ - 1].homeFrameDepth = homeFrame;
                                    }
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

    // NOTE: do NOT consume nlrHomeMethod_/nlrValue_ here for fd>0. The
    // savedFrames_[].homeFrameDepth mechanism (set by NLR-ENSURE handler
    // when pausing an NLR at an ensure: frame) already triggers the inline
    // NLR path when ensure: itself returns. A previous implementation
    // hijacked any fd>0 return here based on the globals — that incorrectly
    // fired on ordinary returns during cleanup block execution (e.g. helper
    // methods called from the cleanup block), skipping remaining cleanup.
    // The fd=0 path in returnValue still consumes the globals as a
    // process-switch safety net.

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
    if (offset < 0) {
#if PHARO_JIT_ENABLED
        tryOSRAtBackwardJump();
#endif
        backwardBranchInterruptCheck();
    }
}

void Interpreter::longJumpIfTrue() {
    int16_t offset = static_cast<int16_t>(fetchTwoBytes());
    Oop value = pop();
    if (isTrue(value)) {
        instructionPointer_ += offset;
        if (offset < 0) {
#if PHARO_JIT_ENABLED
            tryOSRAtBackwardJump();
#endif
            backwardBranchInterruptCheck();
        }
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
        if (offset < 0) {
#if PHARO_JIT_ENABLED
            tryOSRAtBackwardJump();
#endif
            backwardBranchInterruptCheck();
        }
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

    // PHARO_DETECT_ERRORS=1: catch the moment any "this is an error
    // being signalled" selector is sent, dump the JIT method + IP +
    // receiver + recent frame chain.  Pinpoints the JIT codegen
    // site that produced the bad value or out-of-bounds index.
    {
        static bool detect = std::getenv("PHARO_DETECT_ERRORS") != nullptr;
        static int hits = 0;
        static int hitLimit = []{
            const char* v = std::getenv("PHARO_DETECT_ERRORS_LIMIT");
            return v ? atoi(v) : 30;
        }();
        if (detect && hits < hitLimit && selector.isObject() &&
            selector.rawBits() > 0x10000) {
            ObjectHeader* sh = selector.asObjectPtr();
            if (sh->isBytesObject() && sh->byteSize() < 64) {
                const char* sb = (const char*)sh->bytes();
                size_t sz = sh->byteSize();
                bool errorSel =
                    (sz == 22 && memcmp(sb, "errorImproperStore:in:", 22) == 0) ||
                    (sz == 21 && memcmp(sb, "errorSubscriptBounds:", 21) == 0) ||
                    (sz == 6  && memcmp(sb, "error:", 6) == 0) ||
                    (sz == 32 && memcmp(sb,
                        "signalFor:lowerBound:upperBound:", 32) == 0) ||
                    (sz == 35 && memcmp(sb,
                        "signalFor:lowerBound:upperBound:in:", 35) == 0) ||
                    (sz == 19 && memcmp(sb, "signalForException:", 19) == 0);
                // signal/signal: are also Semaphore methods.  Only
                // accept them when the receiver class walks up to
                // Exception via the superclass chain.
                bool signalSel =
                    (sz == 6 && memcmp(sb, "signal", 6) == 0) ||
                    (sz == 7 && memcmp(sb, "signal:", 7) == 0);
                bool match = errorSel;
                if (signalSel) {
                    Oop rcvrClass = memory_.classOf(rcvr);
                    Oop walker = rcvrClass;
                    int safety = 30;
                    while (walker.isObject() && safety-- > 0) {
                        std::string cname = memory_.classNameOf(walker);
                        if (cname == "Exception" || cname == "Exception class" ||
                            cname == "Error" || cname == "Error class") {
                            match = true;
                            break;
                        }
                        if (cname == "Object" || cname == "ProtoObject" ||
                            cname == "Behavior" || cname == "nil") break;
                        // Walk up: superclass slot is at index 0 typically
                        ObjectHeader* wh = walker.asObjectPtr();
                        if (!wh || wh->slotCount() < 1) break;
                        walker = wh->slots()[0];
                    }
                }
                if (match) {
                    hits++;
                    fprintf(stderr,
                        "\n=== [ERROR-DETECT #%d] sel='%.*s' (argc=%d) ===\n",
                        hits, (int)sz, sb, argCount);
                    fprintf(stderr,
                        "  receiver: 0x%llx class=%s\n",
                        (unsigned long long)rcvr.rawBits(),
                        memory_.classNameOf(memory_.classOf(rcvr)).c_str());
                    // For an Exception instance receiver, dump its
                    // first 4 slots (subscript/lowerBound/upperBound/
                    // messageText for SubscriptOutOfBounds).
                    if (rcvr.isObject() && rcvr.rawBits() > 0x10000) {
                        ObjectHeader* rh = rcvr.asObjectPtr();
                        size_t nSlots = rh->slotCount();
                        size_t toShow = std::min(nSlots, (size_t)4);
                        for (size_t si = 0; si < toShow; si++) {
                            Oop slot = rh->slots()[si];
                            if (slot.isSmallInteger()) {
                                fprintf(stderr,
                                    "  rcvr.slot[%zu] = SmI(%lld)\n", si,
                                    (long long)slot.asSmallInteger());
                            } else if (slot.isNil()) {
                                fprintf(stderr, "  rcvr.slot[%zu] = nil\n", si);
                            } else if (slot.isObject() && slot.rawBits() > 0x10000) {
                                std::string cn = memory_.classNameOf(memory_.classOf(slot));
                                fprintf(stderr,
                                    "  rcvr.slot[%zu] = 0x%llx (%s)\n", si,
                                    (unsigned long long)slot.rawBits(), cn.c_str());
                            }
                        }
                    }
                    for (int ai = 0; ai < argCount && ai < 5; ai++) {
                        Oop arg = stackValue(argCount - 1 - ai);
                        std::string argDesc;
                        if (arg.isSmallInteger()) {
                            char buf[64];
                            snprintf(buf, sizeof(buf), "SmI(%lld)",
                                (long long)arg.asSmallInteger());
                            argDesc = buf;
                        } else if (arg.isNil()) {
                            argDesc = "nil";
                        } else {
                            argDesc = memory_.classNameOf(memory_.classOf(arg));
                        }
                        fprintf(stderr,
                            "  arg[%d]: 0x%llx %s\n",
                            ai, (unsigned long long)arg.rawBits(),
                            argDesc.c_str());
                        if (arg.isObject() && arg.rawBits() > 0x10000) {
                            ObjectHeader* ah = arg.asObjectPtr();
                            if (ah->isBytesObject() && ah->byteSize() < 256) {
                                fprintf(stderr,
                                    "  arg[%d] text: '%.*s'\n", ai,
                                    (int)ah->byteSize(), (const char*)ah->bytes());
                            }
                        }
                    }
                    // Calling method + IP
                    if (method_.isObject() && method_.rawBits() > 0x10000) {
                        long long bcOff = -1;
                        if (instructionPointer_) {
                            ObjectHeader* mh = method_.asObjectPtr();
                            Oop hdr = mh->slots()[0];
                            int nLit = hdr.isSmallInteger()
                                ? (hdr.asSmallInteger() & 0x7FFF) : 0;
                            const uint8_t* bcBase =
                                mh->bytes() + (1 + nLit) * 8;
                            bcOff = instructionPointer_ - bcBase;
                        }
                        std::string mc = classNameOfMethod(method_);
                        std::string ms = memory_.selectorOf(method_);
                        bool isJIT = jitRuntime_.methodMap().lookup(
                            method_.rawBits()) != nullptr;
                        fprintf(stderr,
                            "  caller: %s>>%s bcOff=%lld JIT=%s\n",
                            mc.c_str(), ms.c_str(), bcOff,
                            isJIT ? "yes" : "no");
                    }
                    // Recent frames (up to 10)
                    fprintf(stderr, "  stack frames (top → bottom):\n");
                    int dumpedFrames = 0;
                    for (size_t i = frameDepth_; i > 0 && dumpedFrames < 10; i--) {
                        SavedFrame& f = savedFrames_[i - 1];
                        if (f.savedMethod.isObject() &&
                            f.savedMethod.rawBits() > 0x10000) {
                            std::string fmc = classNameOfMethod(f.savedMethod);
                            std::string fms =
                                memory_.selectorOf(f.savedMethod);
                            bool fJIT = jitRuntime_.methodMap().lookup(
                                f.savedMethod.rawBits()) != nullptr;
                            fprintf(stderr,
                                "    [%d] %s>>%s JIT=%s\n",
                                (int)(i - 1), fmc.c_str(), fms.c_str(),
                                fJIT ? "yes" : "no");
                            dumpedFrames++;
                        }
                    }
                    fprintf(stderr, "=== end ERROR-DETECT #%d ===\n\n", hits);
                }
            }
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

    // Legacy trace of #matches: sends — left-over from regex debugging.
    // Gated on PHARO_MATCH_TRACE=1.
    static const bool matchTrace = std::getenv("PHARO_MATCH_TRACE") != nullptr;
    if (__builtin_expect(matchTrace, 0)) {
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
                uint64_t jitAddr = 0;
                auto* jm = jitRuntime_.methodMap().lookup(cached->method.rawBits());
                if (jm) {
                    // Only store jitEntry for methods safe for J2J
                    // (same check as pharo_jit_convert_send)
                    bool hasPrim = (jm->methodHeader >> 16) & 1;
                    if (!hasPrim || jm->hasPrimPrologue)
                        jitAddr = reinterpret_cast<uint64_t>(jm->codeStart());
                }
                jitRuntime_.megaCacheAdd(selector.rawBits(), megaKey,
                                        cached->method.rawBits(), jitAddr);
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
            uint64_t jitAddr = 0;
            auto* jm = jitRuntime_.methodMap().lookup(method.rawBits());
            if (jm) {
                bool hasPrim = (jm->methodHeader >> 16) & 1;
                if (!hasPrim || jm->hasPrimPrologue)
                    jitAddr = reinterpret_cast<uint64_t>(jm->codeStart());
            }
            jitRuntime_.megaCacheAdd(selector.rawBits(), megaKey,
                                    method.rawBits(), jitAddr);
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

    // Cog-spec finalization drain trigger: Cog's forceInterruptCheck
    // sets stackLimit = -1 in fireEphemeron, so the next method
    // activation's stack-overflow check fires and — assuming
    // canContextSwitchIfActivating holds (primitive != 198) — runs
    // checkForEvents which signals TheFinalizationSemaphore.  P50
    // drains the mourn queue in that flow.
    //
    // Our VM: if PHARO_FINALIZE_DEFERRED is on and the mourn queue
    // is non-empty, drain it synchronously (natively, no P50/P51
    // scheduling) right here at activation.  Quick primitives
    // (256-519 — prim-slot-at, literal returns, etc.) don't reach
    // activateMethod, so `dict size` returns the pre-drain tally
    // before the first real activation arms the drain — this is
    // the timing Cog's test suite relies on.
    if (__builtin_expect(g_debug.finalizeDeferred &&
                         memory_.pendingFinalizationSignals() > 0, 0)) {
        drainMournQueueNatively();
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
    // Sista speculative-compile + dispatch hook.
    //   PHARO_SISTA_COMPILE=1  — compile on every activation; don't dispatch.
    //   PHARO_SISTA_DISPATCH=1 — compile and, on success, invoke compiled code.
    //
    // Compile-only mode populates the cache so we can measure
    // success rate on a real workload without changing VM behavior.
    // Dispatch mode also calls the compiled function and handles
    // its two exit reasons: ExitReturn (method returned a value) and
    // ExitSend (Sista bailed mid-method — interpreter resumes at the
    // bail bytecode in the same frame).
    if ((g_debug.sistaCompile || g_debug.sistaDispatch) && !inSyncSend_) {
        // 2026-04-29 (B7 fix): skip Sista compile + dispatch while a
        // helper-send is driving step() to completion.  Otherwise
        // each inner activation may JIT-emit kSendCallHelper, which
        // immediately bails (depth-1 cap), and the resulting deopt
        // chains expand the C-stack and frame-depth without ever
        // letting the outer helper return.
        //
        // File-scope pointer so recoverJITAfterGC can invalidate the
        // cache — raw oop keys become stale after Spur compaction.
        static sista::Runtime* sista = [this]() {
            auto* r = new sista::Runtime();
            sistaRuntimeForGCHook_ = r;
            // Phase 4 Step 5: register the hint provider so the Sista
            // Builder can fetch a method's monomorphic IC entries
            // when recursively inlining inner self-sends.
            sista::Builder::setHintProvider([this](Oop methodOop) {
                return extractInlineHintsForMethod(methodOop);
            });
            return r;
        }();
        static size_t attempts = 0, hits = 0;
        static size_t dispatched = 0, sistaReturns = 0, sistaSends = 0, sistaUnknown = 0;
        static bool banner = false;
        if (!banner) {
            fprintf(stderr, "[SISTA] compile=%d dispatch=%d hook active\n",
                    (int)g_debug.sistaCompile, (int)g_debug.sistaDispatch);
            banner = true;
        }
        attempts++;
        // Phase 4 Step 1: build inline hints from T1's IC info.
        // Pass them to Sista so it can identify monomorphic send
        // sites.  Today only used for the inline-hint stat counters;
        // Step 2 will use them for actual IR splicing.
        //
        // PHARO_SISTA_AFTER_T1=1: defer Sista compile until T1 has
        // compiled the method.  Without this, Sista runs on first
        // activation and the IC table is always empty (T1 hasn't
        // hit its compile threshold yet), so hints are never present.
        //
        // IC layout note: the ICEntry struct in JITMethod.hpp is sized
        // for IC bookkeeping but its `kind`/`bytecodeOffset` fields are
        // not populated by the runtime.  The actual IC data lives in
        // the flat `[key0,method0,extra0,key1,...]` block past the
        // bcToCode table.  We read it directly here.  bcOffset comes
        // from the compiler's send-site map, indexed by send-site
        // ordinal.  classKey0 is a classIndex (GC-stable Spur key),
        // not an Oop — Phase 4 Step 2 will treat it as such.
        static const bool sistaAfterT1 =
            std::getenv("PHARO_SISTA_AFTER_T1") != nullptr;
        std::vector<sista::InlineHint> inlineHints =
            extractInlineHintsForMethod(method);
        auto* jm = jitRuntime_.methodMap().lookup(method.rawBits());
        if (sistaAfterT1) {
            // No T1 entry yet — let T1 warm up first.  Skip Sista
            // for this activation; fall through to the regular T1
            // path below (must NOT return — that would skip
            // tryJITActivation too).
            //
            // Even with a T1 entry, hold off until the method has
            // executed enough times to populate its IC table.
            // PHARO_SISTA_T1_WARMUP=N (default 100) sets the
            // threshold.  Without it, Sista compiles the instant T1
            // finishes — IC table is still empty — and we cache a
            // useless no-hint result.
            static const uint32_t t1Warmup = []() {
                if (const char* v = std::getenv("PHARO_SISTA_T1_WARMUP")) {
                    int n = atoi(v);
                    return n > 0 ? (uint32_t)n : 100u;
                }
                return 100u;
            }();
            if (!jm || !jm->stats || jm->stats->executionCount < t1Warmup) {
                goto past_sista_block;
            }
        }
        sista::Lowering::CompiledFn fn = sista->compile(method, memory_,
            inlineHints.empty() ? nullptr : &inlineHints);
        if (fn) hits++;
        // Optional inline-hint observability: PHARO_SISTA_INLINE_STATS=1
        // dumps cumulative counters every 100 compile attempts to stderr.
        // Used to size Phase 4 work — if hints-consumed is tiny vs
        // sends-lifted, monomorphic inlining isn't worth the complexity.
        static const bool dumpInlineStats =
            std::getenv("PHARO_SISTA_INLINE_STATS") != nullptr;
        if (dumpInlineStats && (attempts % 100) == 0) {
            sista::Builder::dumpInlineHintStats();
        }
        // Defensive: asmjit's compile internally uses RAII W^X scopes;
        // if any path leaks the writable state on this thread, the next
        // T1 JIT activation crashes SIGBUS.  Force back to executable.
        jit::makeExecutable(jitRuntime_.codeZone().rawStart(),
                            jitRuntime_.codeZone().totalBytes());

        // Only dispatch when the interpreter's IP is at the raw start of
        // bytecodes.  Methods with `<primitive: N error: ec>` cause
        // activateMethod to advance past the ExtStoreTemp that captures
        // the error — Sista's lifter skips only the CallPrimitive header,
        // so its starting offset wouldn't match.  The mismatch would
        // double-execute the error-capture store.  Skip for now; we can
        // teach Sista about the error-temp skip later.
        const bool ipAtBytecodeStart =
            (instructionPointer_ == methodBytes + bytecodeStart);

        // MVP restriction: only dispatch when the method has no
        // bytecodes that Sista bails on.  Sista's ExitSend state-sync
        // path currently causes image-startup divergence (wrong
        // receiver reaches `setGCParameters`), so we limit dispatch
        // to methods whose IR never emits kSendUnspeculated.
        //
        // Allowed send-like bytecodes Sista inlines without bailing:
        //   ArithAdd 0x60, ArithSub 0x61, ArithMul 0x68
        // Everything else in the 0x60-0xAF / ExtSend / ExtSuperSend
        // / PushFullBlock / PushClosure / PushArray / PushThisContext
        // range causes a bail and is disallowed.
        //
        // Note: the inlined arith ops have no type guard today (see
        // docs/sista-inlining-plan.md Phase 2 remaining work), so
        // admitting them is a bet that hot callers pass SmallInt
        // receivers + args.  Silent miscompile is possible if a
        // subclass overrides one of these methods with non-numeric
        // arith; that risk lands with Phase 3 deopt, not here.
        // Gate-decision cache: scanning the method's bytecodes for
        // bail ops on every activation is expensive on hot
        // callers.  Cache the result by raw oop bits; invalidated
        // the same way the Sista fn cache is (recoverJITAfterGC
        // resets both).
        uint64_t gateKey = method.rawBits();
        bool hasUnsafeOp = false;
        // Cached state tracking — three distinct hot-path outcomes
        // off a single gate-cache hashmap find:
        //   gateCachedAdmitted   — admit, skip bailCounter find too
        //   gateCachedBlacklist  — skip dispatch, skip bailCounter find
        //   neither              — slow path or rejected
        bool gateCachedAdmitted = false;
        bool gateCachedBlacklist = false;
        {
            auto gateIt = sistaGateCache_.find(gateKey);
            if (gateIt != sistaGateCache_.end()) {
                uint8_t st = gateIt->second;
                if (st == kSistaGateRejected) {
                    hasUnsafeOp = true;
                } else if (st == kSistaGateBlacklisted) {
                    gateCachedBlacklist = true;
                } else { // kSistaGateAdmitted
                    gateCachedAdmitted = true;
                }
                goto gateDecided;
            }
        }
        {
            size_t bcLen = totalBytes > bytecodeStart
                           ? totalBytes - bytecodeStart : 0;

            // PHARO_SISTA_SIZE_PEEPHOLE=1: the size peephole replaces
            // the SpecialSend(size) with kPrimSize, whose lowering
            // includes a deopt-on-zero check that bails to the
            // interpreter at the size send when the helper returns
            // 0 (non-indexable receiver).  Method's bytecode still
            // contains 0x72 which would normally mark unsafe;
            // explicitly admit the recognized shape.
            static const bool sizePeephole =
                std::getenv("PHARO_SISTA_SIZE_PEEPHOLE") != nullptr;
            if (sizePeephole && bcLen == 3
                && methodBytes[bytecodeStart + 0] == 0x4C
                && methodBytes[bytecodeStart + 1] == 0x72
                && methodBytes[bytecodeStart + 2] == 0x5C) {
                hasUnsafeOp = false;
                goto sizePeepholeAdmitted;
            }
            // PHARO_SISTA_AT_PEEPHOLE=1: same gate exception for
            // `^ self at: i` shape (4 bytes).  kPrimAt's lowering
            // deopts on miss too.
            static const bool atPeephole =
                std::getenv("PHARO_SISTA_AT_PEEPHOLE") != nullptr;
            if (atPeephole && bcLen == 4
                && methodBytes[bytecodeStart + 0] == 0x4C  // PushReceiver
                && methodBytes[bytecodeStart + 1] == 0x40  // PushTemp 0
                && methodBytes[bytecodeStart + 2] == 0x70  // SpecialSend at:
                && methodBytes[bytecodeStart + 3] == 0x5C) { // ReturnTop
                hasUnsafeOp = false;
                goto sizePeepholeAdmitted;  // Reuse the bypass.
            }
            for (size_t i = 0; i < bcLen; i++) {
                uint8_t op = methodBytes[bytecodeStart + i];
                // Inlined arith — safe ONLY when operands are
                // SmallInt.  Today's lowerer has no tag check, so
                // calling these methods on Float/LargeInteger/etc.
                // produces silent-miscompile garbage that causes
                // cascading DNUs downstream (e.g., #* not understood
                // by a malformed tagged value).  Gate off by
                // default; re-admit when Phase 3 deopt lands.
                // Phase 3 deopt admission: PHARO_SISTA_INLINE_ARITH=1
                // makes the builder emit kPrimTagCheckInt before each
                // arith op, with a deopt landing pad that bails to
                // the interpreter at the source bytecode on non-SmallInt
                // operands.  With type checks present, the arith ops
                // are SAFE under any operand type — admit them.
                // Default ON 2026-04-29 (re-flip).  Two prior fixes
                // closed the 1M-getter regression that blocked this:
                //   e90a6ba4 — dropped redundant pre-dispatch
                //              makeExecutable syscall (~13ns/call)
                //   aafd201a — gated dispatch invariant check (~10ns/call)
                // Best-of-3 1M getter under INLINE_ARITH=1 (103) is now
                // BETTER than the opt-out (111) on the bench panel.
                // Opt-out: PHARO_SISTA_NO_INLINE_ARITH=1.
                static const bool inlineArith =
                    std::getenv("PHARO_SISTA_NO_INLINE_ARITH") == nullptr;
                if (inlineArith || g_debug.sistaUnsafeArith) {
                    // + - * (inlined arith) and < <= > >= = ~=
                    // (inlined SmallInt comparison)
                    if (op == jit::SistaV1::ArithBase + 0   // +
                     || op == jit::SistaV1::ArithBase + 1   // -
                     || op == jit::SistaV1::ArithBase + 8   // *
                     || (op >= jit::SistaV1::ArithBase + 2
                         && op <= jit::SistaV1::ArithBase + 7)) {
                        continue;
                    }
                }
                // Diagnostic gate: block all state-mutating ops to
                // test if the Send1 divergence is caused by stores
                // (temp or instVar).
                if (getenv("PHARO_SISTA_NO_STORES")) {
                    if ((op >= 0xC8 && op <= 0xD7)       // PopStoreRecv/Temp
                     || (op >= 0xF0 && op <= 0xF5)) {    // Ext store variants
                        hasUnsafeOp = true;
                        break;
                    }
                }
                // ExitSend-triggering bytecodes — gated off by
                // default because state.sp/state.ip sync to the
                // interpreter currently causes image-startup
                // divergence.  PHARO_SISTA_ALLOW_SENDS=1 bypasses
                // this gate for diagnosis.
                //
                // During diagnosis we can also whitelist specific
                // send categories only — Send0 (0xF80 + N*0x10?)
                // wait, Send0 = 0x80-0x8F (zero args).  If we only
                // allow Send0 bails, we narrow which ExitSend
                // encoding is broken.  PHARO_SISTA_SEND0_ONLY=1.
                const bool isSend0 = (op >= jit::SistaV1::Send0Base
                                   && op <= jit::SistaV1::Send0Last);
                const bool isSend1 = (op >= jit::SistaV1::Send1Base
                                   && op <= jit::SistaV1::Send1Last);
                const bool isSend2 = (op >= jit::SistaV1::Send2Base
                                   && op <= jit::SistaV1::Send2Last);
                // Inlined arith (0x60=+, 0x61=-, 0x68=*) and
                // SmallInt comparisons (0x62-0x67) run TAG-PRESERVING
                // without overflow or type guards — still gated
                // behind PHARO_SISTA_UNSAFE_ARITH.
                if ((op == jit::SistaV1::ArithBase + 0
                  || op == jit::SistaV1::ArithBase + 1
                  || op == jit::SistaV1::ArithBase + 8
                  || (op >= jit::SistaV1::ArithBase + 2
                      && op <= jit::SistaV1::ArithBase + 7))) {
                    hasUnsafeOp = true;
                    break;
                }
                // Generic bail ops (PushFullBlock / PushClosure /
                // PushArray / PushThisContext) are now safe —
                // Sista's generic bailToInterpreter already flushes
                // the full IR stack, and the adaptive bail-blacklist
                // culls them if they over-bail.  Let them through.
                //
                // DIAG gate (PHARO_SISTA_NO_FULLBLOCK=1): exclude
                // methods with PushFullBlock to test whether those
                // bails are the source of image-startup DNU cascades.
                if (getenv("PHARO_SISTA_NO_FULLBLOCK")) {
                    if (op == jit::SistaV1::PushFullBlock
                     || op == jit::SistaV1::PushClosure) {
                        hasUnsafeOp = true;
                        break;
                    }
                }
                // DIAG gate (PHARO_SISTA_NO_REMOTE_TEMP=1): exclude
                // methods that access closure-captured temp vectors
                // (PushTempAtInVec / StoreTempInVec / PopStoreTempInVec).
                if (getenv("PHARO_SISTA_NO_REMOTE_TEMP")) {
                    if (op == jit::SistaV1::PushTempAtInVec
                     || op == 0xFC
                     || op == 0xFD) {
                        hasUnsafeOp = true;
                        break;
                    }
                }
                // DIAG: log methods with remote-temp ops on first
                // admission.  Tells us which methods the
                // NO_REMOTE_TEMP fix excludes.
                if (getenv("PHARO_SISTA_LOG_REMOTE_TEMP")) {
                    if (op == jit::SistaV1::PushTempAtInVec
                     || op == 0xFC
                     || op == 0xFD) {
                        static std::unordered_set<uint64_t> loggedMethods_;
                        if (loggedMethods_.insert(gateKey).second) {
                            fprintf(stderr,
                                "[SISTA-HAS-REMOTE-TEMP] method=0x%llx "
                                "sel=#%s op=0x%02x offset=%zu bcLen=%zu bc=[",
                                (unsigned long long)gateKey,
                                memory_.selectorOf(method).c_str(),
                                (unsigned)op, i, bcLen);
                            for (size_t j = 0; j < std::min(bcLen, (size_t)64); j++) {
                                fprintf(stderr, " %02x",
                                    methodBytes[bytecodeStart + j]);
                                if (j == i) fprintf(stderr, "|");
                            }
                            fprintf(stderr, "]\n");
                        }
                    }
                }
                // Finer-grained: only block READS (no effect on state)
                // versus only block WRITES (store side).
                if (getenv("PHARO_SISTA_NO_REMOTE_TEMP_READ")) {
                    if (op == jit::SistaV1::PushTempAtInVec) {
                        hasUnsafeOp = true;
                        break;
                    }
                }
                if (getenv("PHARO_SISTA_NO_REMOTE_TEMP_WRITE")) {
                    if (op == 0xFC || op == 0xFD) {
                        hasUnsafeOp = true;
                        break;
                    }
                }
                // By-selector exclusion — PHARO_SISTA_EXCLUDE_SELS
                // is a comma-separated list of selector names;
                // matching methods get rejected from dispatch.
                if (const char* excl = getenv("PHARO_SISTA_EXCLUDE_SELS")) {
                    static const std::string excludeList = excl;
                    std::string sel = memory_.selectorOf(method);
                    size_t start = 0;
                    while (start < excludeList.size()) {
                        size_t comma = excludeList.find(',', start);
                        std::string token = excludeList.substr(
                            start, comma == std::string::npos
                                       ? std::string::npos : comma - start);
                        if (!token.empty() && sel == token) {
                            hasUnsafeOp = true;
                            break;
                        }
                        if (comma == std::string::npos) break;
                        start = comma + 1;
                    }
                    if (hasUnsafeOp) break;
                }
                //
                // InstVar-store bytecodes bypass the immutability
                // check in Sista's kStoreInstVar lowering (a direct
                // `str val, [recv+8+N*8]` without the
                // `attemptToAssign:withIndex:` callout the
                // interpreter's setReceiverInstVar does).  Reject
                // methods with any ivar-store bytecode so immutable
                // receivers still raise ModificationForbidden.
                // (Fix for ObjectTest>>testBeRecursivelyReadOnlyObject
                // and related.)
                if ((op >= jit::SistaV1::PopStoreRecvBase
                  && op <= jit::SistaV1::PopStoreRecvLast)
                 || op == jit::SistaV1::ExtPopStoreRecv
                 || op == jit::SistaV1::ExtStoreRecv) {
                    hasUnsafeOp = true;
                    break;
                }
                // Sends bail cleanly via the fixed lifter.
                // Adaptive runtime blacklist (sistaBailCounter_)
                // removes methods that bail consecutively, so we
                // can admit all send-bytes without paying the
                // dispatch overhead forever on bail-only methods.
                // PHARO_SISTA_NO_BAIL=1 restores the conservative
                // scan-reject behavior.
                if (g_debug.sistaNoBail) {
                    if (jit::SistaV1::isSendBytecode(op)
                     || op == jit::SistaV1::ExtSend
                     || op == jit::SistaV1::ExtSuperSend) {
                        hasUnsafeOp = true;
                        break;
                    }
                }
                (void)isSend0; (void)isSend1; (void)isSend2;
            }
        sizePeepholeAdmitted:
            sistaGateCache_[gateKey] = hasUnsafeOp
                ? kSistaGateRejected : kSistaGateAdmitted;
        }
    gateDecided:

        // Runtime blacklist: if this method has bailed via
        // ExitSend consecutively for > threshold activations,
        // skip dispatch.  Avoids paying the per-call dispatch
        // overhead for methods Sista can't meaningfully speed up.
        // Hot-path optimization: if the gate cache hit and the
        // state was kSistaGateAdmitted, the method has never had
        // a recorded bail (bail-promote routes go through
        // kSistaGateBlacklisted instead).  Skip the bailCounter
        // hashmap find entirely on every dispatch — saves ~1 ns
        // per call on hot benches.
        bool blacklisted = gateCachedBlacklist;
        if (!gateCachedAdmitted && !gateCachedBlacklist) {
            auto bailIt = sistaBailCounter_.find(gateKey);
            if (bailIt != sistaBailCounter_.end()
                && bailIt->second >= kSistaBailBlacklistThreshold) {
                blacklisted = true;
            }
        }

        const bool dispatchGateOpen =
            ipAtBytecodeStart
            && (!hasUnsafeOp || g_debug.sistaAllowSends)
            && !blacklisted;
        if (fn && g_debug.sistaDispatch && dispatchGateOpen) {
            // Tier-up dispatch.  Full JITState init — trimming was
            // attempted but breaks execution (some downstream path
            // reads an uninitialized field when Sista returns).
            // Cost ~20 stores per dispatch, amortized by the
            // adaptive bail-blacklist and gate cache.
            jit::JITState sstate;
            sstate.sp = stackPointer_;
            sstate.receiver = receiver_;
            sstate.literals = methodObj->slots() + 1;
            sstate.tempBase = framePointer_ + 1;
            sstate.memory = &memory_;
            sstate.interp = this;
            sstate.ip = instructionPointer_;
            sstate.method = method;
            sstate.argCount = argCount;
            sstate.jitMethod = nullptr;
            sstate.exitReason = jit::ExitNone;
            sstate.icDataPtr = nullptr;
            sstate.sendArgCount = 0;
            sstate.trueOop = memory_.trueObject();
            sstate.falseOop = memory_.falseObject();
            sstate.j2jSaveCursor = nullptr;
            sstate.j2jSaveLimit = nullptr;
            sstate.j2jDepth = 0;
            sstate.j2jTotalCalls = 0;
            sstate.methodMapPtr = nullptr;
            sstate.yieldCountdown = 0;

            // Direct call — Sista's asmjit-generated code emits a
            // standard AArch64 prologue/epilogue via asmjit's
            // Compiler, so it saves/restores any callee-saved
            // registers it uses.  No need for JIT_CALL's broad
            // clobber list (which exists for T1 stencils that
            // stash x19-x22 without save/restore).
            //
            // W^X discipline: asmjit and our CodeZone share the
            // per-thread MAP_JIT toggle on Apple Silicon.  AFTER-call
            // makeExecutable (below, after fn) protects the next T1
            // JIT entry from any stray writable state left behind by
            // asmjit-internal bookkeeping.  The BEFORE call here used
            // to be defensive but the dispatch path is always reached
            // with the JIT zone already executable (the only
            // makeWritable/makeExecutable pairs in the hot path are
            // RAII-scoped at IC-patch sites and re-execute on every
            // exit).  Skipping the redundant pre-call toggle saves
            // one syscall per dispatch — measured ~13ns/call on the
            // 1M getter regression.  If a future code path drops the
            // toggle without restoring, the symptom is SIGBUS in
            // Sista code; reinstate this line first when debugging.
            // Sanity-check invariants across Sista dispatch — the
            // interpreter-owned frame state must not mutate during
            // asmjit-generated code.  Failure = ABI/callee-save bug.
            //
            // Gated behind PHARO_SISTA_INVARIANT_CHECK=1 (default
            // off): each dispatch otherwise pays 4 saves + 4
            // compares + branch (~10ns).  Across the bench panel's
            // ~5M Sista activations that's ~50ms.  The check is
            // useful when bringing up a new lowering or chasing an
            // ABI bug; unnecessary in steady state.
            static const bool invariantCheck =
                std::getenv("PHARO_SISTA_INVARIANT_CHECK") != nullptr;
            Oop savedReceiver{};
            Oop* savedFP = nullptr;
            Oop savedMethod{};
            size_t savedFrameDepth = 0;
            if (invariantCheck) {
                savedReceiver = receiver_;
                savedFP = framePointer_;
                savedMethod = method_;
                savedFrameDepth = frameDepth_;
            }
            // Snapshot frame temp slots (args + locals) — log any
            // slot Sista writes.  Only active with PHARO_SISTA_TEMP_WATCH=1.
            static const bool tempWatch =
                getenv("PHARO_SISTA_TEMP_WATCH") != nullptr;
            // Snapshot stack slots BELOW current stackPointer — those
            // belong to the caller's frame.  Sista must not write there.
            static const bool stackWatch =
                getenv("PHARO_SISTA_STACK_WATCH") != nullptr;
            Oop savedBelowSp[16];
            if (stackWatch) {
                for (int k = 0; k < 16; k++) {
                    Oop* p = stackPointer_ - 1 - k;
                    if (p >= stackBase_) savedBelowSp[k] = *p;
                    else                 savedBelowSp[k] = Oop::fromRawBits(0);
                }
            }
            // Compute temp count from method header.
            Oop savedTemps[64];
            int numTempsWatch = 0;
            if (tempWatch) {
                ObjectHeader* mh = method.asObjectPtr();
                Oop hdr = mh->slots()[0];
                if (hdr.isSmallInteger()) {
                    int64_t hb = hdr.asSmallInteger();
                    int nArgs  = (int)((hb >> 24) & 0x0F);
                    int nTemps = (int)((hb >> 18) & 0x3F);
                    numTempsWatch = std::min(nTemps, 64);
                    (void)nArgs;
                    for (int k = 0; k < numTempsWatch; k++) {
                        savedTemps[k] = *(framePointer_ + 1 + k);
                    }
                }
            }
            fn(&sstate);
            jit::makeExecutable(jitRuntime_.codeZone().rawStart(),
                                jitRuntime_.codeZone().totalBytes());
            if (invariantCheck && __builtin_expect(
                    savedReceiver.rawBits() != receiver_.rawBits()
                    || savedFP != framePointer_
                    || savedMethod.rawBits() != method_.rawBits()
                    || savedFrameDepth != frameDepth_, 0)) {
                fprintf(stderr,
                    "[SISTA-CORRUPT] sel=#%s: frame state mutated "
                    "by fn(&sstate)\n"
                    "  rcvr:  was=0x%llx now=0x%llx\n"
                    "  fp:    was=%p now=%p\n"
                    "  method:was=0x%llx now=0x%llx\n"
                    "  fdepth:was=%zu now=%zu\n",
                    memory_.selectorOf(method).c_str(),
                    (unsigned long long)savedReceiver.rawBits(),
                    (unsigned long long)receiver_.rawBits(),
                    (void*)savedFP, (void*)framePointer_,
                    (unsigned long long)savedMethod.rawBits(),
                    (unsigned long long)method_.rawBits(),
                    savedFrameDepth, frameDepth_);
            }
            // Stack-below-SP check: Sista must not write to caller's
            // frame slots below stackPointer_.  Most bugs flagged here
            // would be lowering bugs writing to sp[-N] instead of sp[N].
            if (stackWatch) {
                for (int k = 0; k < 16; k++) {
                    Oop* p = stackPointer_ - 1 - k;
                    if (p < stackBase_) break;
                    if (p->rawBits() != savedBelowSp[k].rawBits()) {
                        fprintf(stderr,
                            "[SISTA-BELOW-SP] sel=#%s slot=-%d "
                            "was=0x%llx now=0x%llx\n",
                            memory_.selectorOf(method).c_str(), k+1,
                            (unsigned long long)savedBelowSp[k].rawBits(),
                            (unsigned long long)p->rawBits());
                    }
                }
            }
            // Log every temp slot Sista wrote (via kStoreTemp).
            if (tempWatch && numTempsWatch > 0) {
                for (int k = 0; k < numTempsWatch; k++) {
                    Oop now = *(framePointer_ + 1 + k);
                    if (now.rawBits() != savedTemps[k].rawBits()) {
                        fprintf(stderr,
                            "[SISTA-TEMP-WRITE] sel=#%s slot=%d "
                            "was=0x%llx now=0x%llx exit=%d bailOp=0x%02x\n",
                            memory_.selectorOf(method).c_str(), k,
                            (unsigned long long)savedTemps[k].rawBits(),
                            (unsigned long long)now.rawBits(),
                            (int)sstate.exitReason,
                            (sstate.exitReason == jit::ExitSend
                             && sstate.ip >= methodBytes + bytecodeStart
                             && sstate.ip < methodBytes + totalBytes)
                                ? (unsigned)*sstate.ip : 0);
                    }
                }
            }

            // Ring-buffer record — cheap, always-on.  Dumped on DNU.
            {
                SistaRingEntry& e = sistaRing_[sistaRingHead_];
                e.methodBits   = method.rawBits();
                e.receiverBits = receiver_.rawBits();
                e.exitReason   = (uint32_t)sstate.exitReason;
                e.sendArgCount = (uint16_t)sstate.sendArgCount;
                if (sstate.exitReason == jit::ExitReturn) {
                    e.retOrTopBits = sstate.returnValue.rawBits();
                    e.bailOp = 0;
                } else {
                    ptrdiff_t delta = sstate.sp - stackPointer_;
                    e.retOrTopBits = (delta > 0)
                        ? sstate.sp[-1].rawBits()
                        : 0;
                    uint8_t* bcEnd = methodBytes + totalBytes;
                    e.bailOp = (sstate.ip >= methodBytes + bytecodeStart
                                && sstate.ip < bcEnd)
                                  ? *sstate.ip : 0;
                }
                ptrdiff_t d = sstate.sp - stackPointer_;
                if (d > 127) d = 127;
                if (d < -128) d = -128;
                e.spDelta = (int8_t)d;
                sistaRingHead_ = (sistaRingHead_ + 1) % kSistaRingSize;
                sistaRingSeq_++;
            }

            dispatched++;
            // Charge the periodic-check machinery for bytecodes
            // Sista executed silently — without this, checkCountdown_
            // drifts high on ExitReturn paths (Sista never touched
            // the interpreter's step counter), delaying timer
            // signals and process switches.  Estimate with the
            // method's bytecode length; overcounts slightly for
            // ExitSend (Sista only ran prefix bytecodes, not all),
            // but biasing early is harmless — just fires checks
            // sooner, never later.
            {
                size_t bcLen = totalBytes > bytecodeStart
                               ? totalBytes - bytecodeStart : 0;
                checkCountdown_ -= static_cast<int>(bcLen);
                stepCheckCounter_ += static_cast<int>(bcLen);
                g_stepNum += bcLen;
            }
            // Per-selector dispatch counter, periodically dumped —
            // useful for seeing which getters/setters are hot-path
            // and whether dispatch is exercising the real workload
            // or just cold startup code.
            if (g_debug.sistaVerbose) {
                static std::unordered_map<std::string, size_t> bySelector;
                std::string sel = memory_.selectorOf(method);
                bySelector[sel]++;
                if ((dispatched & 0x3FFF) == 0) {
                    std::vector<std::pair<std::string, size_t>> vec(
                        bySelector.begin(), bySelector.end());
                    std::sort(vec.begin(), vec.end(),
                              [](auto& a, auto& b){ return a.second > b.second; });
                    fprintf(stderr, "[SISTA-TOP] @disp=%zu:", dispatched);
                    for (size_t i = 0; i < vec.size() && i < 10; i++) {
                        fprintf(stderr, " #%s=%zu",
                                vec[i].first.c_str(), vec[i].second);
                    }
                    fprintf(stderr, "\n");
                }
            }
            // Reference-check ExitReturn: replay the bytecodes using
            // the interpreter's own read primitives and compute what
            // the interpreter would have returned.  If it differs
            // from Sista's returnValue, log it.  Covers simple
            // load-and-return methods (PushX + ReturnTop, or direct
            // ReturnReceiver/Nil/True/False).  Always runs under
            // verbose mode (not just the first N) so divergent
            // returns far into a run are still caught.
            if (g_debug.sistaVerbose && sstate.exitReason == jit::ExitReturn) {
                uint8_t* bcStart = methodBytes + bytecodeStart;
                size_t bcLen = totalBytes > bytecodeStart
                               ? totalBytes - bytecodeStart : 0;
                Oop refVal;
                bool haveRef = false;
                if (bcLen >= 1) {
                    uint8_t op = bcStart[0];
                    uint8_t op2 = bcLen >= 2 ? bcStart[1] : 0;
                    // Single-bytecode returns.
                    if (op == 0x58) { refVal = receiver_; haveRef = true; }
                    else if (op == 0x59) { refVal = memory_.trueObject(); haveRef = true; }
                    else if (op == 0x5A) { refVal = memory_.falseObject(); haveRef = true; }
                    else if (op == 0x5B) { refVal = memory_.nil(); haveRef = true; }
                    // Two-bytecode: PushX + ReturnTop.
                    else if (op2 == 0x5C) {
                        if (op >= 0x00 && op <= 0x0F) {
                            refVal = memory_.fetchPointerUnchecked(op, receiver_);
                            haveRef = true;
                        } else if (op >= 0x10 && op <= 0x1F) {
                            Oop assoc = methodObj->slots()[1 + (op - 0x10)];
                            if (assoc.isObject()) {
                                refVal = assoc.asObjectPtr()->slotAt(1);
                                haveRef = true;
                            }
                        } else if (op >= 0x20 && op <= 0x3F) {
                            refVal = methodObj->slots()[1 + (op - 0x20)];
                            haveRef = true;
                        } else if (op >= 0x40 && op <= 0x4B) {
                            refVal = *(framePointer_ + 1 + (op - 0x40));
                            haveRef = true;
                        } else if (op == 0x4C) {
                            refVal = receiver_;
                            haveRef = true;
                        } else if (op == 0x4D) { refVal = memory_.trueObject(); haveRef = true; }
                        else if (op == 0x4E)   { refVal = memory_.falseObject(); haveRef = true; }
                        else if (op == 0x4F)   { refVal = memory_.nil(); haveRef = true; }
                        else if (op == 0x50) { refVal = Oop::fromSmallInteger(0); haveRef = true; }
                        else if (op == 0x51) { refVal = Oop::fromSmallInteger(1); haveRef = true; }
                    }
                }
                if (haveRef
                    && refVal.rawBits() != sstate.returnValue.rawBits()) {
                    std::string sel = memory_.selectorOf(method);
                    fprintf(stderr,
                        "[SISTA-RET-MISMATCH] sel=#%s "
                        "sista=0x%llx ref=0x%llx bc=[",
                        sel.c_str(),
                        (unsigned long long)sstate.returnValue.rawBits(),
                        (unsigned long long)refVal.rawBits());
                    for (size_t i = 0; i < std::min(bcLen, (size_t)4); i++) {
                        fprintf(stderr, " %02x", bcStart[i]);
                    }
                    fprintf(stderr, "]\n");
                }
            }
            if (g_debug.sistaVerbose && sstate.exitReason == jit::ExitReturn
                && sistaReturns < 20) {
                std::string sel = memory_.selectorOf(method);
                size_t bcLen = totalBytes > bytecodeStart
                               ? totalBytes - bytecodeStart : 0;
                fprintf(stderr, "[SISTA-RET] #%zu sel=#%s retVal=0x%llx bcLen=%zu bc=[",
                        sistaReturns + 1, sel.c_str(),
                        (unsigned long long)sstate.returnValue.rawBits(),
                        bcLen);
                for (size_t i = 0; i < std::min(bcLen, (size_t)8); i++) {
                    fprintf(stderr, " %02x", methodBytes[bytecodeStart + i]);
                }
                fprintf(stderr, "] lits=[");
                ObjectHeader* mh = method.asObjectPtr();
                Oop hdrOop = mh->slots()[0];
                int numLits = hdrOop.isSmallInteger()
                               ? (hdrOop.asSmallInteger() & 0x7FFF) : 0;
                for (int i = 0; i < std::min(numLits, 4); i++) {
                    fprintf(stderr, " 0x%llx",
                            (unsigned long long)mh->slots()[1 + i].rawBits());
                }
                fprintf(stderr, "]\n");
            }

            switch (sstate.exitReason) {
            case jit::ExitReturn: {
                sistaReturns++;
                // Reset this method's bail counter — it completed
                // successfully, so don't blacklist it.  Erase
                // unconditionally: gate state stays admitted on
                // sub-threshold bails (only promoted to blacklisted
                // once cnt >= threshold), so the cache hit path
                // alone can't tell whether a stale entry exists.
                sistaBailCounter_.erase(gateKey);
                // Pop Sista's frame, push return value — caller resumes.
                if (!popFrame()) {
                    if (benchMode_) {
                        handleBenchComplete(sstate.returnValue);
                        return;
                    }
                    terminateCurrentProcess();
                    tryReschedule();
                    return;
                }
                push(sstate.returnValue);
                return;
            }
            case jit::ExitSend: {
                sistaSends++;
                // Increment this method's consecutive-bail count;
                // blacklist on threshold.  Only cleared by ExitReturn.
                uint16_t& cnt = sistaBailCounter_[gateKey];
                if (cnt < UINT16_MAX) cnt++;
                // Once we hit the threshold, promote the gate cache
                // to kSistaGateBlacklisted so future dispatches can
                // skip the bailCounter find entirely.  Drop the
                // counter entry — it's redundant with the gate.
                if (cnt >= kSistaBailBlacklistThreshold) {
                    sistaGateCache_[gateKey] = kSistaGateBlacklisted;
                    sistaBailCounter_.erase(gateKey);
                }
                // Under verbose mode, log the first few bails with
                // full context so Send1/Send2 divergence can be
                // caught against the baseline expectation.
                // Reference check: replay the bytecodes BEFORE the
                // bail and compute what the interpreter would have
                // pushed.  If it differs from what's on the stack now,
                // Sista miscompiled something.
                if (g_debug.sistaVerbose && sistaSends < 100000) {
                    uint8_t* bcStart = methodBytes + bytecodeStart;
                    ptrdiff_t bailBcOff = sstate.ip - bcStart;
                    // Replay simulator: track an actual stack with pushes
                    // AND pops, plus temp writes (since later pushes may
                    // reload from temps).  Without modeling stores, any
                    // method that does push/pop pairs produces a bogus
                    // MISMATCH (the refStack accumulates unpopped values).
                    std::vector<Oop> refStack;
                    std::vector<Oop> refTemps(16, memory_.nil());
                    // Seed temps with current frame's actual temps so
                    // later PushTemp loads see the right value.  Temps
                    // 0..numArgs-1 are arg values; up to ~12 temps total.
                    for (int t = 0; t < 12; t++) {
                        Oop v = *(framePointer_ + 1 + t);
                        refTemps[t] = v;
                    }
                    int extA = 0;
                    bool replayOk = true;
                    for (ptrdiff_t i = 0; i < bailBcOff && replayOk; ) {
                        uint8_t op = bcStart[i];
                        if (op >= 0x00 && op <= 0x0F) {
                            refStack.push_back(
                                memory_.fetchPointerUnchecked(op, receiver_));
                            i++;
                        } else if (op >= 0x10 && op <= 0x1F) {
                            // PushLitVar N: literals[N].value
                            Oop assoc = methodObj->slots()[1 + (op - 0x10)];
                            Oop val = assoc.isObject()
                                ? assoc.asObjectPtr()->slotAt(1)
                                : memory_.nil();
                            refStack.push_back(val);
                            i++;
                        } else if (op >= 0x20 && op <= 0x3F) {
                            // PushLitConst N: literals[N]
                            refStack.push_back(
                                methodObj->slots()[1 + (op - 0x20)]);
                            i++;
                        } else if (op == 0x4C) {
                            refStack.push_back(receiver_);
                            i++;
                        } else if (op == 0x4D) {
                            refStack.push_back(memory_.trueObject()); i++;
                        } else if (op == 0x4E) {
                            refStack.push_back(memory_.falseObject()); i++;
                        } else if (op == 0x4F) {
                            refStack.push_back(memory_.nil()); i++;
                        } else if (op >= 0x40 && op <= 0x4B) {
                            // PushTemp N — load from refTemps so later
                            // pushes see the value as updated by stores.
                            int tN = op - 0x40;
                            refStack.push_back(refTemps[tN]);
                            i++;
                        } else if (op == 0x50) {
                            refStack.push_back(Oop::fromSmallInteger(0));
                            i++;
                        } else if (op == 0x51) {
                            refStack.push_back(Oop::fromSmallInteger(1));
                            i++;
                        } else if (op >= 0xC8 && op <= 0xCF) {
                            // PopStoreRecvVar N — pop, ignore-effects
                            // (refStack pop only; we don't mutate
                            // receiver_ to avoid side effects).
                            if (!refStack.empty()) refStack.pop_back();
                            i++;
                        } else if (op >= 0xD0 && op <= 0xD7) {
                            // PopStoreTemp N — pop, write to refTemps.
                            int tN = op - 0xD0;
                            if (!refStack.empty()) {
                                refTemps[tN] = refStack.back();
                                refStack.pop_back();
                            }
                            i++;
                        } else if (op == 0xD8) {
                            // Pop — discards top.
                            if (!refStack.empty()) refStack.pop_back();
                            i++;
                        } else if (op == 0x53) {
                            // Dup
                            if (!refStack.empty()) {
                                refStack.push_back(refStack.back());
                            }
                            i++;
                        } else if (op == 0xE0) {
                            extA = (extA << 8) | bcStart[i + 1];
                            i += 2;
                        } else if (op >= 0xB0 && op <= 0xB7) {
                            // ShortJump +1..+8 — unconditional forward,
                            // affects next bytecode but doesn't change
                            // stack.  Simulator can't follow control
                            // flow correctly without full interp; abort.
                            replayOk = false;
                        } else if ((op >= 0x60 && op <= 0xAF)
                                || op == 0xEA || op == 0xEB) {
                            // Sends: would terminate this lifted block in
                            // Sista's IR.  At a send the simulator can't
                            // know the result.  Stop replay; we'll only
                            // compare the prefix.
                            replayOk = false;
                        } else {
                            // Unsupported op — give up replay.
                            replayOk = false;
                        }
                        // Cap stack depth defensively.
                        if (refStack.size() > 32) replayOk = false;
                    }
                    ptrdiff_t sd = sstate.sp - stackPointer_;
                    bool mismatch = false;
                    // Only trust the comparison if the replay completed
                    // through every bytecode up to the bail.  Aborted
                    // replays (sends, branches, unsupported ops) produce
                    // bogus refStacks.
                    if (replayOk) {
                        for (ptrdiff_t i = 0;
                             i < sd && (size_t)i < refStack.size(); i++) {
                            if (stackPointer_[i].rawBits()
                                != refStack[i].rawBits()) {
                                mismatch = true;
                                break;
                            }
                        }
                    }
                    if (mismatch) {
                        std::string sel = memory_.selectorOf(method);
                        fprintf(stderr, "[SISTA-MISMATCH] sel=#%s bc=[", sel.c_str());
                        for (ptrdiff_t i = 0; i < bailBcOff && i < 8; i++) {
                            fprintf(stderr, " %02x", bcStart[i]);
                        }
                        fprintf(stderr, "] sista=[");
                        for (ptrdiff_t i = 0; i < sd; i++) {
                            fprintf(stderr, " 0x%llx",
                                (unsigned long long)stackPointer_[i].rawBits());
                        }
                        fprintf(stderr, "] ref=[");
                        for (size_t i = 0; i < refStack.size(); i++) {
                            fprintf(stderr, " 0x%llx",
                                (unsigned long long)refStack[i].rawBits());
                        }
                        fprintf(stderr, "]\n");
                    }
                }
                if (g_debug.sistaVerbose && sistaSends < 100000) {
                    std::string sel = memory_.selectorOf(method);
                    uint8_t bailOp = 0;
                    if (sstate.ip >= methodBytes + bytecodeStart
                     && sstate.ip < methodBytes + totalBytes) {
                        bailOp = *sstate.ip;
                    }
                    ptrdiff_t sd = sstate.sp - stackPointer_;
                    fprintf(stderr,
                        "[SISTA-SEND] #%zu sel=#%s bailOp=0x%02x "
                        "ipOff=%td spDelta=%td argc=%d rcvr=0x%llx",
                        sistaSends, sel.c_str(), bailOp,
                        sstate.ip - (methodBytes + bytecodeStart),
                        sd, sstate.sendArgCount,
                        (unsigned long long)receiver_.rawBits());
                    for (ptrdiff_t i = 0; i < sd && i < 4; i++) {
                        fprintf(stderr, " push[%td]=0x%llx", i,
                                (unsigned long long)stackPointer_[i].rawBits());
                    }
                    fprintf(stderr, " bc=[");
                    size_t bcLen = totalBytes > bytecodeStart
                                   ? totalBytes - bytecodeStart : 0;
                    for (size_t i = 0; i < std::min(bcLen, (size_t)8); i++) {
                        fprintf(stderr, " %02x",
                                methodBytes[bytecodeStart + i]);
                    }
                    fprintf(stderr, "] temps=[");
                    for (int i = 0; i < 3; i++) {
                        fprintf(stderr, " 0x%llx", (unsigned long long)
                            (framePointer_ + 1 + i)->rawBits());
                    }
                    fprintf(stderr, "]\n");
                }
                // Sista bailed mid-method: rcvr+args (or the full IR
                // stack, for generic bailToInterpreter) are pushed to
                // state.sp; state.ip points to the bail bytecode.
                // Sync interpreter registers and fall through — the
                // dispatch loop picks up execution at state.ip in the
                // same (still-active) Sista frame.
                //
                // Sanity-check Sista's output before trusting it —
                // catches lifter/lowerer bugs early instead of
                // letting them silently corrupt interpreter state.
                if (g_debug.sistaVerbose) {
                    uint8_t* bcEnd = methodBytes + totalBytes;
                    bool ipInBounds = (sstate.ip >= methodBytes + bytecodeStart
                                     && sstate.ip < bcEnd);
                    bool spInBounds = (sstate.sp >= stackPointer_
                                     && sstate.sp < stackPointer_ + 256);
                    if (!ipInBounds || !spInBounds) {
                        fprintf(stderr,
                            "[SISTA-BAIL-INVALID] #disp=%zu sel=#%s "
                            "ip=%p (bc=%p..%p) sp=%p (was %p) argc=%d\n",
                            dispatched, memory_.selectorOf(method).c_str(),
                            (void*)sstate.ip,
                            (void*)(methodBytes + bytecodeStart), (void*)bcEnd,
                            (void*)sstate.sp, (void*)stackPointer_,
                            sstate.sendArgCount);
                    }
                }
                stackPointer_ = sstate.sp;
                instructionPointer_ = sstate.ip;
                return;
            }
            default:
                sistaUnknown++;
                // Shouldn't happen — Sista only emits ExitReturn /
                // ExitSend today.  Fall through to interpreter for
                // safety; the frame is still set up correctly.
                break;
            }
        }

        if (attempts == 1 || (attempts & 0xFFFFF) == 0) {
            fprintf(stderr,
                "[SISTA] hits=%zu/%zu cache=%zu disp=%zu ret=%zu send=%zu unk=%zu\n",
                hits, attempts, sista->compiledCount(),
                dispatched, sistaReturns, sistaSends, sistaUnknown);
        }
    }
past_sista_block:

    // Try JIT execution. If it handles the method, it pops the frame
    // and pushes the return value — the dispatch loop continues with
    // the caller's next bytecode.
    //
    // Inline fast-reject (todo.md §2.9): ~90% of activations miss the
    // JIT method map.  Skipping the tryJITActivation call entirely on
    // the miss path avoids its prologue/epilogue + trace-guard costs
    // for the common case.
    if (canJITActivate(method) && tryJITActivation(method, argCount)) {
        return;  // JIT handled it
    }
    // Otherwise fall through to interpreter execution via the dispatch loop
#endif

    // Deferred FinalizationSemaphore signal: primitiveFullGC/primitiveIncrementalGC
    // set `finalizationCheckAfterGC_` instead of signaling immediately.  Fire at
    // the END of activateMethod — method setup is complete, so if synchronousSignal
    // transferTo's to FP, the test process's frame state is consistent and can
    // resume from the beginning of its (newly activated) method body when
    // scheduled again.
    //
    // testClearing invariant: `dict size` is prim 264 (quick) — doesn't go
    // through activateMethod.  First method activation after garbageCollect
    // is typically `self assert:equals:` whose args (pre-drain tally) are
    // already on the operand stack evaluated by caller.  Preemption here
    // doesn't disturb them.
    if (__builtin_expect(finalizationCheckAfterGC_, 0)) {
        finalizationCheckAfterGC_ = false;
        signalFinalizationIfNeeded();
    }
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

        // Primary home: from the CompiledBlock's last literal (static/lexical home).
        // Fallback home: the method of the closure's outerContext — needed for the case
        // where the CompiledBlock is shared between different CompiledMethods (notably
        // FBDBytecodeDecompiler reuses the original CompiledBlock when regenerating a
        // method, so the CompiledBlock's last literal points at the ORIGINAL method,
        // but execution is happening in a frame for the REGENERATED method).  Without
        // this fallback, NLR from the regenerated method's inner block fails with
        // BlockCannotReturn because the static home isn't on savedFrames_.
        Oop homeMethodOop = homeMethodForNLR;
        Oop altHomeMethod = Oop::nil();
        if (outerContext.isObject() && !outerContext.isNil()) {
            Oop ocMethod = memory_.fetchPointer(3, outerContext);
            if (ocMethod.isObject() && !ocMethod.isNil() && ocMethod.rawBits() > 0x10000 &&
                ocMethod.rawBits() != homeMethodOop.rawBits()) {
                // Follow outerContext.method's enclosing-code chain to a non-block
                // CompiledMethod (same walk pattern as the static chain above).
                Oop walk = ocMethod;
                int chain = 0;
                while (walk.isObject() && walk.rawBits() > 0x10000 && chain < 20) {
                    ObjectHeader* wHdr = walk.asObjectPtr();
                    if (!wHdr->isCompiledMethod()) break;
                    Oop wHeader = memory_.fetchPointer(0, walk);
                    if (!wHeader.isSmallInteger()) break;
                    int wNumLits = wHeader.asSmallInteger() & 0xFFFF;
                    if (wNumLits < 1) { altHomeMethod = walk; break; }
                    Oop wLastLit = memory_.fetchPointer(wNumLits, walk);
                    bool lastIsCode = wLastLit.isObject() && wLastLit.rawBits() > 0x10000 &&
                                      wLastLit.asObjectPtr()->isCompiledMethod();
                    if (!lastIsCode) { altHomeMethod = walk; break; }
                    walk = wLastLit;
                    chain++;
                }
            }
        }

        for (size_t i = frameDepth_; i > 0; i--) {
            Oop savedMethod = savedFrames_[i - 1].savedMethod;

            // Match primary home, then fallback home (for shared CompiledBlocks).
            if (savedMethod.rawBits() == homeMethodOop.rawBits()) {
                homeFrame = i - 1;
                break;
            }
            if (!altHomeMethod.isNil() && savedMethod.rawBits() == altHomeMethod.rawBits()) {
                homeFrame = i - 1;
                homeMethodOop = altHomeMethod;  // use fallback as the canonical home from here on
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
    // Graceful stack overflow: StackOverflowLimit < MaxFrameDepth, so this
    // catches both infinite recursion (soft) and hard overflow.
    if (__builtin_expect(frameDepth_ >= StackOverflowLimit, 0)) {
        static int overflowLog = 0;
        if (overflowLog++ < 3) {
            fprintf(stderr, "[OVERFLOW] fd=%zu pushing #%s (argCount=%d)\n",
                    frameDepth_, memory_.selectorOf(method).c_str(), argCount);
            // Dump first 40 frames (to see what started the chain)
            fprintf(stderr, "[OVERFLOW] Call stack (first 40):\n");
            size_t earlyEnd = frameDepth_ < 40 ? frameDepth_ : 40;
            for (size_t f = 0; f < earlyEnd; f++) {
                Oop savedM = savedFrames_[f].savedMethod;
                Oop savedR = savedFrames_[f].savedReceiver;
                std::string savedSel = memory_.selectorOf(savedM);
                fprintf(stderr, "  [%zu] #%s rcvr=0x%llx method=0x%llx\n",
                        f, savedSel.c_str(),
                        (unsigned long long)savedR.rawBits(),
                        (unsigned long long)savedM.rawBits());
            }
            // Dump first transition: where does copyTo:/recursion start?
            // Walk forward from frame 40 and show any selector change.
            fprintf(stderr, "[OVERFLOW] Selector transitions (40..fd):\n");
            std::string prevSel;
            size_t runStart = 40;
            for (size_t f = 40; f < frameDepth_; f++) {
                Oop savedM = savedFrames_[f].savedMethod;
                std::string sel = memory_.selectorOf(savedM);
                if (f == 40) { prevSel = sel; runStart = f; continue; }
                if (sel != prevSel) {
                    fprintf(stderr, "  [%zu..%zu] #%s (run len=%zu)\n",
                            runStart, f - 1, prevSel.c_str(), f - runStart);
                    prevSel = sel;
                    runStart = f;
                }
            }
            if (frameDepth_ > 40) {
                fprintf(stderr, "  [%zu..%zu] #%s (run len=%zu)\n",
                        runStart, frameDepth_ - 1, prevSel.c_str(),
                        frameDepth_ - runStart);
            }
            // If we're in a Context>>copyTo:-style chain walk, the receiver at
            // frame[2] is the root of the "errored" chain we're walking. Walk
            // its sender chain directly and print each Context's method
            // selector to see what the ORIGINAL call stack looked like.
            if (frameDepth_ >= 3) {
                Oop c2rcvr = savedFrames_[2].savedReceiver;
                std::string f2sel = memory_.selectorOf(savedFrames_[2].savedMethod);
                if (f2sel == "copyTo:" && c2rcvr.isObject() && c2rcvr.rawBits() > 0x10000) {
                    fprintf(stderr, "[OVERFLOW] Walking errored-context sender chain from frame[2].rcvr=0x%llx:\n",
                            (unsigned long long)c2rcvr.rawBits());
                    Oop cur = c2rcvr;
                    int depth = 0;
                    int maxWalk = 8192;
                    std::string prevCtxSel;
                    int runStartIdx = 0;
                    while (cur.isObject() && cur.rawBits() > 0x10000 && depth < maxWalk) {
                        Oop ctxMethod = memory_.fetchPointerUnchecked(3, cur);
                        std::string ctxSel = (ctxMethod.isObject() && ctxMethod.rawBits() > 0x10000)
                            ? memory_.selectorOf(ctxMethod) : "(nil-method)";
                        if (depth < 40) {
                            Oop ctxRcvr = memory_.fetchPointerUnchecked(5, cur);
                            std::string rcls = memory_.classNameOf(ctxRcvr);
                            fprintf(stderr, "  ctx[%d]=0x%llx method=0x%llx #%s rcvrClass=%s\n",
                                    depth, (unsigned long long)cur.rawBits(),
                                    (unsigned long long)ctxMethod.rawBits(),
                                    ctxSel.c_str(), rcls.c_str());
                        } else {
                            if (depth == 40) { prevCtxSel = ctxSel; runStartIdx = 40; }
                            else if (ctxSel != prevCtxSel) {
                                fprintf(stderr, "  ctx[%d..%d] #%s (run len=%d)\n",
                                        runStartIdx, depth - 1, prevCtxSel.c_str(),
                                        depth - runStartIdx);
                                prevCtxSel = ctxSel;
                                runStartIdx = depth;
                            }
                        }
                        Oop nextSender = memory_.fetchPointerUnchecked(0, cur);
                        if (!nextSender.isObject() || nextSender.rawBits() <= 0x10000) {
                            fprintf(stderr, "  ctx chain ends at depth %d (sender=0x%llx)\n",
                                    depth + 1, (unsigned long long)nextSender.rawBits());
                            break;
                        }
                        cur = nextSender;
                        depth++;
                    }
                    if (depth > 40 && !prevCtxSel.empty()) {
                        fprintf(stderr, "  ctx[%d..%d] #%s (run len=%d)\n",
                                runStartIdx, depth - 1, prevCtxSel.c_str(),
                                depth - runStartIdx);
                    }
                    if (depth >= maxWalk) {
                        fprintf(stderr, "  ctx walk truncated at maxWalk=%d\n", maxWalk);
                    }
                }
            }
            // Dump last 50 frames with raw bits for debugging
            fprintf(stderr, "[OVERFLOW] Call stack (last 50):\n");
            size_t start = frameDepth_ > 50 ? frameDepth_ - 50 : 0;
            for (size_t f = start; f < frameDepth_; f++) {
                Oop savedM = savedFrames_[f].savedMethod;
                Oop savedR = savedFrames_[f].savedReceiver;
                std::string savedSel = memory_.selectorOf(savedM);
                Oop rcvrSender = memory_.nil();
                if (savedR.isObject() && savedR.rawBits() > 0x10000) {
                    rcvrSender = memory_.fetchPointer(0, savedR);
                }
                fprintf(stderr, "  [%zu] #%s rcvr=0x%llx rcvr.sender=0x%llx method=0x%llx\n",
                        f, savedSel.c_str(),
                        (unsigned long long)savedR.rawBits(),
                        (unsigned long long)rcvrSender.rawBits(),
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
                    // Note: "findNextHandlerContext" is 22 chars (not 25 — prior typo).
                    if (selLen == 22 && memcmp(bytes, "findNextHandlerContext", 22) == 0) {
                        static int log = 0;
                        if (++log <= 3)
                            fprintf(stderr, "[DNU-FIX] nil findNextHandlerContext → nil\n");
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

    // Log first 60 DNU messages to debug startup issues
    {
        static int dnuLogCount = 0;
        if (dnuLogCount++ < 60) {
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
                // B5 diagnostic: dump last J2J save/return ring buffer.
                pharo_jit_b5_dump_ring(selName.c_str());
#if PHARO_JIT_ENABLED
                // Correlate DNU with recent Sista dispatches.
                sistaRingDump(selName.c_str(), this);
#endif
            }
            Oop rcvr = stackValue(argCount);
            Oop currentProc = getActiveProcess();
            Oop procPri = memory_.fetchPointer(ProcessPriorityIndex, currentProc);
            int pri = procPri.isSmallInteger() ? (int)procPri.asSmallInteger() : -1;
            fprintf(stderr, "[DNU] #%d: #%s not understood by rcvr=0x%llx argCount=%d fd=%zu in #%s P%d\n",
                    dnuLogCount, selName.c_str(), (unsigned long long)rcvr.rawBits(), argCount, frameDepth_,
                    memory_.selectorOf(method_).c_str(), pri);
            bool suspiciousRcvr = false;
            if (rcvr.isObject() && rcvr.rawBits() > 0x10000) {
                ObjectHeader* rH = rcvr.asObjectPtr();
                uint32_t cls = rH->classIndex();
                fprintf(stderr, "[DNU]   rcvr cls=%u fmt=%d class=%s\n",
                        cls, (int)rH->format(), memory_.classNameOf(rcvr).c_str());
                if (cls == 0) suspiciousRcvr = true;  // Invalid object
            } else if (rcvr.isSmallInteger()) {
                fprintf(stderr, "[DNU]   rcvr is SmallInteger %lld\n", rcvr.asSmallInteger());
                suspiciousRcvr = true;
            }
            // Dump JIT provenance for suspicious receivers (SmallInt or classIdx=0)
#if PHARO_JIT_ENABLED
            if (suspiciousRcvr && lastJitReturn_.methodBits != 0) {
                Oop ljm = Oop::fromRawBits(lastJitReturn_.methodBits);
                Oop ljr = Oop::fromRawBits(lastJitReturn_.returnBits);
                fprintf(stderr, "[DNU-JIT] lastJitReturn: method=#%s(0x%llx) retVal=0x%llx(%s%lld) fd=%zu %s\n",
                        memory_.selectorOf(ljm).c_str(),
                        (unsigned long long)lastJitReturn_.methodBits,
                        (unsigned long long)lastJitReturn_.returnBits,
                        ljr.isSmallInteger() ? "SmallInt " : "obj ",
                        ljr.isSmallInteger() ? ljr.asSmallInteger() : 0LL,
                        lastJitReturn_.frameDepth,
                        lastJitReturn_.wasResume ? "(resume)" : "(activation)");
                // Check if caller method_ is JIT-compiled
                jit::JITMethod* callerJM = jitRuntime_.methodMap().lookup(method_.rawBits());
                fprintf(stderr, "[DNU-JIT] caller=#%s jitCompiled=%s\n",
                        memory_.selectorOf(method_).c_str(),
                        callerJM ? "YES" : "no");
                // Dump frame stack with JIT annotations
                for (size_t f = 0; f <= frameDepth_ && f < 20; f++) {
                    SavedFrame& sf = savedFrames_[f];
                    std::string mSel = memory_.selectorOf(sf.savedMethod);
                    jit::JITMethod* fjm = jitRuntime_.methodMap().lookup(sf.savedMethod.rawBits());
                    fprintf(stderr, "[DNU-JIT]   [%zu] #%s %s\n", f, mSel.c_str(),
                            fjm ? "<<JIT>>" : "");
                }
                // Identify where the corrupt receiver lives in memory
                uint64_t rv = rcvr.rawBits();
                uint64_t sBase = (uint64_t)stackBase_;
                uint64_t sTop  = (uint64_t)(stackBase_ + MaxStackDepth);
                uint64_t cStart = (uint64_t)jitRuntime_.codeZone().rawStart();
                uint64_t cEnd   = cStart + jitRuntime_.codeZone().totalBytes();
                bool inHeap = rcvr.isObject() && memory_.isValidPointer(rcvr);
                fprintf(stderr, "[DNU-JIT] rcvr 0x%llx: inHeap=%d stack=[0x%llx,0x%llx) code=[0x%llx,0x%llx)\n",
                        (unsigned long long)rv, inHeap,
                        (unsigned long long)sBase, (unsigned long long)sTop,
                        (unsigned long long)cStart, (unsigned long long)cEnd);
                if (rv >= sBase && rv < sTop)
                    fprintf(stderr, "[DNU-JIT]   -> IN INTERPRETER STACK (offset %lld slots)\n", (long long)((rv - sBase) / 8));
                else if (inHeap)
                    fprintf(stderr, "[DNU-JIT]   -> IN HEAP (valid pointer)\n");
                else if (rv >= cStart && rv < cEnd)
                    fprintf(stderr, "[DNU-JIT]   -> IN JIT CODE ZONE\n");
                else
                    fprintf(stderr, "[DNU-JIT]   -> UNKNOWN REGION\n");
                // Also dump this, SP/FP, and offset analysis
                fprintf(stderr, "[DNU-JIT] this=0x%llx SP=0x%llx FP=0x%llx fd=%zu\n",
                        (unsigned long long)(uint64_t)this,
                        (unsigned long long)(uint64_t)stackPointer_,
                        (unsigned long long)(uint64_t)framePointer_,
                        frameDepth_);
                fprintf(stderr, "[DNU-JIT] rcvr offset from this: 0x%llx, from stackBase: 0x%llx\n",
                        (unsigned long long)(rv - (uint64_t)this),
                        (unsigned long long)(rv - sBase));
                uint64_t megaBase = (uint64_t)jitRuntime_.megaCache();
                uint64_t megaEnd = megaBase + 65536 * 32;
                fprintf(stderr, "[DNU-JIT] megaCache=[0x%llx,0x%llx) rcvr offset from mega: %lld (entry %lld, field %lld)\n",
                        (unsigned long long)megaBase, (unsigned long long)megaEnd,
                        (long long)(rv - megaBase),
                        (long long)((rv - megaBase) / 32),
                        (long long)((rv - megaBase) % 32));
            }
#endif // PHARO_JIT_ENABLED
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

std::string Interpreter::classNameOfMethod(Oop method) const {
    Oop cls = methodClassOf(method);
    if (!cls.isObject() || cls.isNil()) return "?";
    return memory_.nameOfClass(cls);
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

    // B5 diagnostic: when creating a block in decodeBytes:, check that
    // the copied byteStream is not a SmallInt.  If it is, print the
    // stream corresponding to temp[1] and the current frame state.
    if (g_debug.b5Trace && numCopied > 0 && method_.isObject()) {
        std::string sel = memory_.selectorOf(method_);
        if (sel == "decodeBytes:") {
            Oop byteStream = memory_.fetchPointer(4, block);  // copied[0]
            if (byteStream.isSmallInteger()) {
                Oop temp1 = *(framePointer_ + 1 + 1);  // temp 1 directly
                fprintf(stderr, "[B5-BUG] decodeBytes: block created with "
                               "byteStream=SmallInt(%lld) temp1=0x%llx "
                               "sp=%p fp=%p\n",
                        (long long)byteStream.asSmallInteger(),
                        (unsigned long long)temp1.rawBits(),
                        stackPointer_, framePointer_);
                // Walk the frame to show all temps
                for (int ti = 0; ti < 5; ti++) {
                    Oop t = *(framePointer_ + 1 + ti);
                    fprintf(stderr, "[B5-BUG]   temp[%d] = 0x%llx %s\n",
                            ti, (unsigned long long)t.rawBits(),
                            t.isSmallInteger() ? "SmallInt"
                              : t.isObject() ? memory_.classNameOf(t).c_str()
                              : "other");
                }
                pharo_jit_b5_dump_ring("block-copied-smallint");
            }
        }
    }

    push(block);
}

uint64_t Interpreter::jitSistaBasicSize(jit::JITState* state,
                                         uint64_t rcvBits) {
    (void)state;
    // Receiver must be an object pointer (tag 0, not immediate).
    if ((rcvBits & 7) != 0 || rcvBits < 0x10000) return 0;
    auto* hdr = reinterpret_cast<pharo::ObjectHeader*>(rcvBits);
    uint32_t fmt = (uint32_t)hdr->format();
    size_t slotCount = hdr->slotCount();
    int64_t size;
    if (fmt == 2) {
        // Pure indexable Array.
        size = (int64_t)slotCount;
    } else if (fmt >= 16 && fmt <= 23) {
        // Byte-indexable (ByteString, ByteArray, Symbol).
        size = (int64_t)(slotCount * 8 - (fmt - 16));
    } else if (fmt >= 24 && fmt <= 31) {
        // CompiledMethod (rare for `do:` but supported for parity).
        // Actual byte size is past header literals; we use slotCount
        // here as a coarse upper bound.
        size = (int64_t)(slotCount * 8 - (fmt - 24));
    } else if (fmt == 9) {
        // 64-bit indexable (DoubleWordArray).  Size in 64-bit words.
        size = (int64_t)slotCount;
    } else if (fmt == 3 || fmt == 4 || fmt == 5) {
        // IndexableWithFixed.  Size = slotCount - fixedFields.
        // Fixed fields stored in the receiver's class instSpec.
        Oop rcv = Oop::fromRawBits(rcvBits);
        Oop classOop = memory_.classOf(rcv);
        if (!classOop.isObject()) return 0;
        Oop fmtSlot = memory_.fetchPointer(2, classOop);
        if (!fmtSlot.isSmallInteger()) return 0;
        int64_t classFmt = fmtSlot.asSmallInteger();
        size_t fixedFields = (size_t)(classFmt & 0xFFFF);
        if (slotCount < fixedFields) return 0;
        size = (int64_t)(slotCount - fixedFields);
    } else {
        return 0;
    }
    if (size < 0) return 0;
    if ((uint64_t)size > 0x07FFFFFFFFFFFFFFULL) return 0;  // SmI range
    Oop result = Oop::fromSmallInteger(size);
    return result.rawBits();
}

uint64_t Interpreter::jitSistaBasicAt(jit::JITState* state,
                                       uint64_t rcvBits,
                                       uint64_t idxBits) {
    (void)state;
    if ((rcvBits & 7) != 0 || rcvBits < 0x10000) return 0;
    if ((idxBits & 7) != 1) return 0;  // index must be SmallInteger
    int64_t i = (int64_t)idxBits >> 3;
    auto* hdr = reinterpret_cast<pharo::ObjectHeader*>(rcvBits);
    uint32_t fmt = (uint32_t)hdr->format();
    size_t slotCount = hdr->slotCount();
    if (fmt == 2) {
        if (i < 1 || (uint64_t)i > slotCount) return 0;
        Oop* slots = reinterpret_cast<Oop*>(rcvBits + 8);
        return slots[i - 1].rawBits();
    }
    if (fmt >= 16 && fmt <= 23) {
        uint64_t byteSize = slotCount * 8 - (fmt - 16);
        if (i < 1 || (uint64_t)i > byteSize) return 0;
        const uint8_t* bytes =
            reinterpret_cast<const uint8_t*>(rcvBits + 8);
        uint64_t b = bytes[i - 1];
        return Oop::fromSmallInteger((int64_t)b).rawBits();
    }
    // Other formats (3/4/5/9/24-31) bail to interpreter for now —
    // less common in `do:` hot paths.
    return 0;
}

uint64_t Interpreter::jitSistaCallSend(jit::JITState* state,
                                         uint64_t selBits,
                                         uint64_t nArgs) {
    // Recursion-depth guard.  Sista helper-sends nest on the C stack:
    // step() inside the helper may activate another Sista method whose
    // helper-send re-enters here.  fib(28) has 514229 recursive calls
    // — that blows the C stack instantly.  Cap depth at 1: an outer
    // helper-send is fine, but any nested helper-send returns 0 to
    // signal deopt; the lowering's deopt-on-zero fallback hands the
    // send back to the bail-to-interpreter path.
    static constexpr int kMaxSistaHelperDepth = 1;
    if (sistaHelperDepth_ >= kMaxSistaHelperDepth) {
        return 0;
    }
    sistaHelperDepth_++;
    struct DepthGuard {
        int* d;
        ~DepthGuard() { (*d)--; }
    } depthGuard{&sistaHelperDepth_};

    // Save caller state.  state->sp/ip/method belong to the JIT'd
    // method; we'll restore them on return so the caller's compiled
    // code can continue.
    Oop* savedSP = stackPointer_;
    uint8_t* savedIP = instructionPointer_;
    Oop savedMethod = method_;
    Oop savedReceiver = receiver_;
    Oop* savedFP = framePointer_;
    int savedArgCount = argCount_;
    Oop savedClosure = closure_;
    Oop savedHome = homeMethod_;
    size_t startFrameDepth = frameDepth_;

    // The compiled caller pushed [rcvr, arg0..arg_{n-1}] onto state->sp
    // before invoking us.  At successful exit we want state->sp to point
    // BELOW those operands (the result lives in the caller's `dst`
    // register, not on state->sp).  Capture the target SP up front so
    // we don't rely on running-stack arithmetic that drifts when
    // primitives or intermediate ops touch stackPointer_ unexpectedly.
    Oop* targetExitSP = state->sp - (nArgs + 1);

    // Sync interp stack from JIT state.  After this, sendSelector's
    // stackValue() / popN() see the right values.
    stackPointer_ = state->sp;

    // Drive step() to run the activated method to completion.
    // inSyncSend_=true gates step()'s periodic check so process
    // switches don't reset frameDepth_ underneath us.
    bool savedInSync = inSyncSend_;
    inSyncSend_ = true;

    Oop sel = Oop::fromRawBits(selBits);
    sendSelector(sel, (int)nArgs);

    // sendSelector either:
    //   - completed synchronously (primitive): frameDepth_ unchanged,
    //     result on stack.
    //   - pushed a frame (normal method): drive step() until that
    //     frame returns (frameDepth_ back to startFrameDepth) OR
    //     frame popped past us (NLR) OR VM stops.
    while (running_ && frameDepth_ > startFrameDepth) {
        if (!step()) {
            inSyncSend_ = savedInSync;
            stackPointer_ = savedSP;
            instructionPointer_ = savedIP;
            method_ = savedMethod;
            receiver_ = savedReceiver;
            framePointer_ = savedFP;
            argCount_ = savedArgCount;
            closure_ = savedClosure;
            homeMethod_ = savedHome;
            frameDepth_ = startFrameDepth;
            return 0;
        }
    }

    inSyncSend_ = savedInSync;

    if (frameDepth_ < startFrameDepth) {
        // NLR through us — restore and bail.  Source bcOffset deopt
        // by the lowering will let the interp pick up NLR handling.
        state->sp = stackPointer_;
        stackPointer_ = savedSP;
        instructionPointer_ = savedIP;
        method_ = savedMethod;
        receiver_ = savedReceiver;
        framePointer_ = savedFP;
        argCount_ = savedArgCount;
        closure_ = savedClosure;
        homeMethod_ = savedHome;
        frameDepth_ = startFrameDepth;
        return 0;
    }

    // Normal return: primitive completed.  Result on top of interp
    // stack.
    Oop result = stackTop();
    popN(1);
    // Force state->sp to the pre-computed target rather than trusting
    // stackPointer_ (which can drift if the activated method or any of
    // its callees temporarily pushes/pops without restoring exact
    // balance — e.g., materializeFrameStack rewrites stackPointer_).
    state->sp = targetExitSP;

    stackPointer_ = savedSP;
    instructionPointer_ = savedIP;
    method_ = savedMethod;
    receiver_ = savedReceiver;
    framePointer_ = savedFP;
    argCount_ = savedArgCount;
    closure_ = savedClosure;
    homeMethod_ = savedHome;

    return result.rawBits();
}

// JIT helper for kStoreInstVar: write `val` into `recv`'s instVar at
// `ivarIdx` with the same safety guards setReceiverInstVar uses.
//
// Returns 1 on success, 0 if the store was refused.  Refusal cases:
//   - non-object receiver (immediate)
//   - immutable receiver (skips the #attemptToAssign:withIndex: send for
//     now — Sista's setter inline currently guards by class, so the
//     common path is mutable; a future enhancement can route to the
//     send-helper when the deopt path is wired up)
//   - bytes object / CompiledMethod receiver
//   - ivarIdx out of bounds
//
// The store goes through ObjectMemory::storePointerUnchecked which
// already includes the generational write barrier (rememberObject when
// an old object gains a young pointer), so this is GC-safe.
uint64_t Interpreter::jitStoreInstVar(Oop recv, uint64_t ivarIdx, Oop val) {
    if (!recv.isObject()) return 0;
    ObjectHeader* hdr = recv.asObjectPtr();
    if (hdr->isImmutable()) return 0;
    if (hdr->isBytesObject() || hdr->isCompiledMethod()) return 0;
    if (ivarIdx >= hdr->slotCount()) return 0;
    memory_.storePointerUnchecked(static_cast<size_t>(ivarIdx), recv, val);
    return 1;
}

uint64_t Interpreter::jitSistaAllocArray(jit::JITState* state,
                                           uint64_t size) {
    (void)state;
    // Cap at a sane upper bound to avoid runaway alloc on a corrupted
    // size value.  Real arrays in benchmarks are well below this; for
    // unusual sizes the splice's caller can size-check up front.
    if (size > 0x100000) return 0;
    Oop arrayClass = memory_.specialObject(SpecialObjectIndex::ClassArray);
    uint32_t classIndex = memory_.indexOfClass(arrayClass);
    Oop arr = memory_.allocateSlots(classIndex, (size_t)size,
                                     ObjectFormat::Indexable);
    return arr.rawBits();
}

uint64_t Interpreter::jitSistaCreateFullBlock(jit::JITState* state,
                                                int litIndex,
                                                int numCopied,
                                                int flags) {
    bool receiverOnStack = ((flags >> 7) & 1) != 0;
    bool ignoreOuterContext = ((flags >> 6) & 1) != 0;

    // Sync our stackPointer_ from JIT state's sp.  JIT'd code has
    // pushed numCopied (+ optional receiver) onto state->sp; we move
    // our stackPointer_ to that position so createFullBlockWithLiteral
    // pops from the right place.
    Oop* savedSP = stackPointer_;
    stackPointer_ = state->sp;
    createFullBlockWithLiteral(litIndex, numCopied, receiverOnStack,
                                ignoreOuterContext);
    // The new block is now on top of stackPointer_.  Pop it off — the
    // JIT'd caller takes ownership via the return value, not the stack
    // (so subsequent compiled IR sees a clean stack).
    Oop block = pop();
    state->sp = stackPointer_;
    stackPointer_ = savedSP;
    return block.rawBits();
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
        // Dump extended context for high-priority processes OR for
        // resume:through: / ensure:-family terminations at any priority
        // (those indicate an exception/NLR walk hit the top of the
        // sender chain — usually a real bug, not a normal exit).
        std::string termSel = memory_.selectorOf(method_);
        bool highPri = (pri.isSmallInteger() && pri.asSmallInteger() >= 60);
        bool exceptionalTerm = (termSel == "resume:through:" ||
                                termSel == "return:through:" ||
                                termSel == "cannotReturn:" ||
                                termSel == "ensure:" ||
                                termSel == "aboutToReturn:through:");
        if (highPri || exceptionalTerm) {
            fprintf(stderr, "[TERM-P%lld] PROCESS TERMINATING via #%s\n",
                    pri.isSmallInteger() ? pri.asSmallInteger() : -1L,
                    termSel.c_str());
            // Walk the context chain to find the original error
            if (activeContext_.isObject() && !activeContext_.isNil()) {
                Oop ctx = activeContext_;
                // Cycle detection: use Floyd's tortoise and hare
                Oop slow = ctx, fast = ctx;
                bool hasCycle = false;
                for (int i = 0; i < 30 && ctx.isObject() && !ctx.isNil(); i++) {
                    Oop method = memory_.fetchPointer(3, ctx);
                    std::string sel = method.isObject() ? memory_.selectorOf(method) : "?";
                    Oop receiver = memory_.fetchPointer(5, ctx);
                    std::string rcvrClass = memory_.classNameOf(receiver);
                    Oop sdr = memory_.fetchPointer(0, ctx);
                    const char* sdrTag = sdr.isNil() ? " (nil)" : "";
                    fprintf(stderr, "[TERM-P%lld]   ctx[%d]: %s>>%s (ctx=0x%llx sender=0x%llx%s)\n",
                            pri.asSmallInteger(), i, rcvrClass.c_str(), sel.c_str(),
                            (unsigned long long)ctx.rawBits(),
                            (unsigned long long)sdr.rawBits(), sdrTag);
                    ctx = sdr;
                }
                // Floyd's cycle detection on sender chain
                for (int i = 0; i < 200; i++) {
                    if (!slow.isObject() || slow.isNil()) break;
                    slow = memory_.fetchPointer(0, slow);  // 1 step
                    if (!fast.isObject() || fast.isNil()) break;
                    fast = memory_.fetchPointer(0, fast);  // 2 steps
                    if (!fast.isObject() || fast.isNil()) break;
                    fast = memory_.fetchPointer(0, fast);
                    if (slow.rawBits() == fast.rawBits()) {
                        hasCycle = true;
                        fprintf(stderr, "[TERM-P%lld] SENDER CHAIN CYCLE DETECTED at ctx=0x%llx!\n",
                                pri.asSmallInteger(), (unsigned long long)slow.rawBits());
                        // Walk to find cycle length
                        Oop c = slow;
                        int len = 0;
                        do {
                            c = memory_.fetchPointer(0, c);
                            len++;
                            if (len > 200) break;
                        } while (c.rawBits() != slow.rawBits());
                        fprintf(stderr, "[TERM-P%lld] Cycle length: %d\n", pri.asSmallInteger(), len);
                        break;
                    }
                }
                if (!hasCycle) {
                    // Count chain length
                    Oop c = activeContext_;
                    int len = 0;
                    for (; len < 500 && c.isObject() && !c.isNil(); len++)
                        c = memory_.fetchPointer(0, c);
                    fprintf(stderr, "[TERM-P%lld] Sender chain length: %d (terminated=%s)\n",
                            pri.asSmallInteger(), len, c.isNil() ? "nil" : "non-nil/limit");
                }
            }
        }
        fprintf(stderr, "[TERM] terminateCurrentProcess: proc=0x%llx pri=%lld fd=%zu method=#%s\n",
                (unsigned long long)proc.rawBits(),
                pri.isSmallInteger() ? pri.asSmallInteger() : -1,
                frameDepth_, memory_.selectorOf(method_).c_str());
        // Print call stack with receiver classes
        for (size_t i = frameDepth_; i > 0 && i > (frameDepth_ > 30 ? frameDepth_ - 30 : 0); i--) {
            Oop savedRcv = savedFrames_[i].savedReceiver;
            std::string cls = "?";
            if (savedRcv.isSmallInteger()) cls = "SmallInteger";
            else if (savedRcv.isNil()) cls = "nil";
            else if (savedRcv.isObject() && memory_.isValidPointer(savedRcv))
                cls = memory_.classNameOf(savedRcv);
            fprintf(stderr, "[TERM]   fd=%zu %s>>%s\n", i, cls.c_str(), memory_.selectorOf(savedFrames_[i].savedMethod).c_str());
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

    // FIX: If activeContext_ IS frame 0's materialized context (from a prior
    // materialization), using it as sender would create a self-referential
    // chain (Context sender == self). Use the saved active context instead —
    // it records what was active when frame 0 was pushed (the true sender).
    if (sender.isObject() && !sender.isNil() && frameDepth_ > 0) {
        Oop frame0Ctx = savedFrames_[0].materializedContext;
        if (frame0Ctx.isObject() && frame0Ctx.rawBits() == sender.rawBits()) {
            // Use savedActiveContext as the sender — it's the context that was
            // active before frame 0 was pushed, i.e. the real parent context.
            Oop savedCtx = savedFrames_[0].savedActiveContext;
            if (savedCtx.isObject() && savedCtx.rawBits() != sender.rawBits()) {
                sender = savedCtx;
            } else {
                // Fall back to the context's heap sender (might be nil)
                Oop heapSender = memory_.fetchPointer(0, sender);
                if (heapSender.rawBits() != sender.rawBits()) {
                    sender = heapSender;
                } else {
                    sender = memory_.nil();  // break the cycle
                }
            }
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
            // FIX: If reusing a context that IS the sender (e.g. frame 0 was
            // activeContext_ on a prior materialization), do NOT update its sender
            // — that would create a self-referential chain. The context's existing
            // sender (set at creation or by a prior correct materialization) is
            // already correct.
            if (context.rawBits() != sender.rawBits()) {
                memory_.storePointer(0, context, sender);  // update sender
            }
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
                if (context.rawBits() != sender.rawBits()) {
                    memory_.storePointer(0, context, sender);  // update sender
                }
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

                // Only set sender for new contexts; reused contexts had sender
                // handled above (with self-reference guard)
                if (!reusingContext) {
                    memory_.storePointer(0, context, sender);                   // sender
                }
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
    xferLog++;
    // PHARO_XFER_TRACE=1 — log every process switch, for debugging bug 14
    static bool xferTrace = getenv("PHARO_XFER_TRACE") != nullptr;
    if (xferLog < 50 || xferTrace) {
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
    //
    // 2026-04-29 (B7 fix): suppress process switches while a Sista
    // helper-send is driving step() to completion.  Switching the
    // active process changes frameDepth_ underneath the helper's
    // `while (frameDepth_ > startFrameDepth)` loop, which then exits
    // (or never exits) with the wrong receiver / method / stack.
    // Higher-priority processes stay queued and pick up after the
    // helper returns.
    if (inSyncSend_) return;
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
    // Sampling profiler (AndreasSystemProfiler / MessageTally)
    registerNamedPrimitive("", "primitiveProfileSemaphore", &Interpreter::primitiveProfileSemaphore);
    registerNamedPrimitive("", "primitiveProfileStart",     &Interpreter::primitiveProfileStart);
    registerNamedPrimitive("", "primitiveProfileSample",    &Interpreter::primitiveProfileSample);
    registerNamedPrimitive("", "primitiveProfilePrimitive", &Interpreter::primitiveProfilePrimitive);
}

PrimitiveResult Interpreter::executePrimitive(int primitiveIndex, int argCount) {
    // Legacy prim 63 (basicAt:put:) counter — gated on PHARO_P63_TRACE.
    static const bool p63Trace = std::getenv("PHARO_P63_TRACE") != nullptr;
    if (__builtin_expect(p63Trace, 0) && primitiveIndex == 63) {
        static int prim63count = 0;
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

#if PHARO_JIT_ENABLED
    // Convert live JITState's IP to an offset into state.method's bytes.
    // state.ip/literals are absolute pointers that break if GC moves the
    // CompiledMethod. forEachRoot updates state.method; afterGC re-derives
    // state.ip and state.literals from the new method address.
    if (currentJITState_ && currentJITState_->method.isObject() &&
        currentJITState_->method.rawBits() > 0x10000 &&
        currentJITState_->ip) {
        uint8_t* methodBytes = currentJITState_->method.asObjectPtr()->bytes();
        jitStateIpOffset_ = currentJITState_->ip - methodBytes;
    } else {
        jitStateIpOffset_ = 0;
    }
#endif

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

#if PHARO_JIT_ENABLED
    // Restore live JITState pointers against the (possibly moved) method.
    // state.method was GC-updated via forEachRoot; now recompute ip and
    // literals which are absolute pointers into the method's allocation.
    if (currentJITState_ && currentJITState_->method.isObject() &&
        currentJITState_->method.rawBits() > 0x10000) {
        ObjectHeader* methObj = currentJITState_->method.asObjectPtr();
        currentJITState_->ip = methObj->bytes() + jitStateIpOffset_;
        currentJITState_->literals = methObj->slots() + 1;
    }
#endif

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

    // PHARO_NO_JIT=0 is NOT treated as disable — preserves historical
    // behavior where "=0" meant "off" conceptually.  All other sites
    // check only presence, but this init path is the gate, so be
    // explicit here.
    if (g_debug.noJit) {
        const char* v = std::getenv("PHARO_NO_JIT");
        if (!v || v[0] != '0') {
            fprintf(stderr, "[JIT] Disabled via PHARO_NO_JIT env var\n");
            return;
        }
    }

    if (!jitRuntime_.initialize(memory_, *this)) {
        fprintf(stderr, "[JIT] Failed to initialize — running interpreted only\n");
        return;
    }
    // Expose code zone for crash diagnostics
    ::g_jitCodeZone = &jitRuntime_.codeZone();
}

#endif // PHARO_JIT_ENABLED — finalization helpers below run regardless

void Interpreter::backwardBranchInterruptCheck() {
    if (!g_debug.finalizeDeferred) return;

    if (--backwardBranchCountdown_ > 0) return;
    backwardBranchCountdown_ = kBackwardBranchCheckReload;

    if (memory_.pendingFinalizationSignals() > 0) {
        drainMournQueueNatively();
    }
}

// ===== NATIVE MOURN DRAIN =====
// Reimplements FinalizationProcess>>finalizationProcess ->
// mournLoopWith: + WKA>>mourn + Dictionary>>removeKey:ifAbsent: +
// fixCollisionsFrom: directly in C++.  Avoids the P50/P51 scheduling
// race: drain runs synchronously within the current process's
// bytecode dispatch, no semaphore/transferTo involved.
void Interpreter::drainMournQueueNatively() {
    // Lazy one-time resolution of WeakKeyAssociation and WeakArray class indices.
    // WKA mourners: processed natively (dict cleanup).
    // WeakArray mourners: dropped (slots already nilled by GC; keeping them in
    //                     mournQueue_ as a GC root would make their slots'
    //                     objects strongly-reachable via pointer chase,
    //                     preventing later objects from becoming weak-alone
    //                     — specifically, it blocks obsolete-class cleanup
    //                     because Metaclass's subclasses array is a WeakArray
    //                     and its mourner-queue presence keeps obsolete
    //                     classes live across test boundaries).
    // Other non-WKA mourners (ObjectFinalizer, FinalizationRegistryEntry,
    //                     WeakSubscription, Ephemeron, etc.): re-pushed for
    //                     image-side dispatch via primitive 172.  These are
    //                     small "finalizer" objects that ObjectFinalizerTest,
    //                     WeakAnnouncerTest, FinalizationRegistryTest rely on.
    if (weakKeyAssociationClassIndex_ == 0) {
        Oop cls = memory_.findGlobal(std::string("WeakKeyAssociation"));
        if (cls.isObject() && !cls.isNil()) {
            weakKeyAssociationClassIndex_ = memory_.indexOfClass(cls);
        }
        if (weakKeyAssociationClassIndex_ == 0) {
            // Mark as "tried" so we don't retry every call.
            weakKeyAssociationClassIndex_ = 0xFFFFFFFF;
        }
    }
    if (weakKeyAssociationClassIndex_ == 0xFFFFFFFF) {
        // Couldn't find WKA class — fall back to P50/P51 path.
        signalFinalizationIfNeeded();
        return;
    }
    if (weakArrayClassIndex_ == 0) {
        Oop cls = memory_.findGlobal(std::string("WeakArray"));
        if (cls.isObject() && !cls.isNil()) {
            weakArrayClassIndex_ = memory_.indexOfClass(cls);
        }
        if (weakArrayClassIndex_ == 0) {
            weakArrayClassIndex_ = 0xFFFFFFFF;
        }
    }
    if (weakValueAssociationClassIndex_ == 0) {
        Oop cls = memory_.findGlobal(std::string("WeakValueAssociation"));
        if (cls.isObject() && !cls.isNil()) {
            weakValueAssociationClassIndex_ = memory_.indexOfClass(cls);
        }
        if (weakValueAssociationClassIndex_ == 0) {
            weakValueAssociationClassIndex_ = 0xFFFFFFFF;
        }
    }

    Oop nilOop = memory_.nil();

    size_t initialQueueSize = memory_.mournQueueSize();
    size_t processed = 0;
    size_t wkaProcessed = 0;
    size_t decremented = 0;
    size_t weakArraysDropped = 0;
    size_t nonWkaKept = 0;
    static int drainLog = 0;

    // Filter-drain: process WKAs natively, drop WeakArrays (no-op mourn),
    // re-push other finalizer mourners for FP to dispatch via prim 172.
    std::vector<Oop> keepers;
    size_t initialPending = memory_.pendingFinalizationSignals();

    while (memory_.hasMourners()) {
        processed++;
        Oop mourner = memory_.popMourner();
        if (!mourner.isObject()) continue;
        ObjectHeader* mournerHdr = mourner.asObjectPtr();
        uint32_t clsIdx = mournerHdr->classIndex();
        if (clsIdx == weakArrayClassIndex_) {
            // WeakArray mourner: slots already nilled by GC, no further
            // action needed.  DO NOT keep — keeping WeakArray in mournQueue_
            // (a GC root) makes its non-nil slots strongly reachable, which
            // prevents referenced classes from becoming weak-alone.
            weakArraysDropped++;
            continue;
        }
        if (clsIdx != weakKeyAssociationClassIndex_ &&
            clsIdx != weakValueAssociationClassIndex_) {
            // Other non-WKA (ObjectFinalizer, FinalizationRegistryEntry,
            // WeakSubscription, Ephemeron): keep for image-side dispatch.
            keepers.push_back(mourner);
            nonWkaKept++;
            continue;
        }
        wkaProcessed++;
        if (mournerHdr->slotCount() < 3) continue;

        // WKA / WVA layout: both are Association subclasses with
        //   slot 0 = key, slot 1 = value, slot 2 = container.
        Oop container = mournerHdr->slotAt(2);
        if (!container.isObject() || container.rawBits() == nilOop.rawBits()) continue;
        ObjectHeader* dictHdr = container.asObjectPtr();
        // Dictionary layout: slot 0 = tally, slot 1 = array.
        if (dictHdr->slotCount() < 2) continue;
        Oop arrayOop = dictHdr->slotAt(1);
        if (!arrayOop.isObject() || arrayOop.rawBits() == nilOop.rawBits()) continue;
        ObjectHeader* arrHdr = arrayOop.asObjectPtr();
        size_t arrSize = arrHdr->slotCount();
        if (arrSize == 0) continue;

        // Find this WKA in the backing array by identity.
        size_t foundIdx = SIZE_MAX;
        for (size_t i = 0; i < arrSize; ++i) {
            if (arrHdr->slotAt(i).rawBits() == mourner.rawBits()) {
                foundIdx = i;
                break;
            }
        }
        if (foundIdx == SIZE_MAX) continue;

        // Nil the slot.  The array is old-space and nil is perm —
        // no write-barrier needed.
        arrHdr->slotAtPut(foundIdx, nilOop);

        // Decrement tally (Dictionary>>removeKey:ifAbsent: does
        // `tally := tally - 1`).
        Oop tallyOop = dictHdr->slotAt(0);
        if (tallyOop.isSmallInteger()) {
            int64_t tally = tallyOop.asSmallInteger();
            dictHdr->slotAtPut(0, Oop::fromSmallInteger(tally - 1));
            decremented++;
        }

        // fixCollisionsFrom: replicate Dictionary>>fixCollisionsFrom:
        // using identityHash for probing.  Correct for Object-keyed
        // dictionaries (Object>>hash returns identityHash); for
        // String-keyed dicts the probe positions will be slightly
        // wrong because String>>hash is content-based — acceptable
        // since testClearing doesn't exercise includesKey: after
        // mourn.
        auto findElementOrNilIdx = [&](Oop key) -> size_t {
            if (!key.isObject()) return SIZE_MAX;
            uint32_t hash = key.asObjectPtr()->identityHash();
            size_t start = hash % arrSize;
            for (size_t probe = 0; probe < arrSize; ++probe) {
                size_t i = (start + probe) % arrSize;
                Oop slot = arrHdr->slotAt(i);
                if (slot.rawBits() == nilOop.rawBits()) return i;
                if (slot.isObject() && slot.rawBits() > 0x10000) {
                    ObjectHeader* sHdr = slot.asObjectPtr();
                    if (sHdr->slotCount() >= 1 &&
                        sHdr->slotAt(0).rawBits() == key.rawBits()) {
                        return i;
                    }
                }
            }
            return SIZE_MAX;
        };
        size_t idx = foundIdx;
        for (size_t steps = 0; steps < arrSize; ++steps) {
            idx = (idx + 1) % arrSize;
            Oop entry = arrHdr->slotAt(idx);
            if (entry.rawBits() == nilOop.rawBits()) break;
            if (!entry.isObject() || entry.rawBits() < 0x10000) continue;
            ObjectHeader* entHdr = entry.asObjectPtr();
            if (entHdr->slotCount() < 1) continue;
            Oop entKey = entHdr->slotAt(0);
            size_t correctIdx = findElementOrNilIdx(entKey);
            if (correctIdx != SIZE_MAX && correctIdx != idx) {
                // Swap entry into its correct position.
                Oop atCorrect = arrHdr->slotAt(correctIdx);
                arrHdr->slotAtPut(correctIdx, entry);
                arrHdr->slotAtPut(idx, atCorrect);
            }
        }
    }

    // Re-push finalizer mourners (ObjectFinalizer, FinalizationRegistryEntry,
    // WeakSubscription, Ephemeron, etc.) so FP can dispatch them via prim 172.
    // If any kept, preserve pendingFinalizationSignals so the
    // activateMethod-entry signal still fires a signal.  If all drops/WKAs,
    // clear pending to avoid a spurious signal.
    if (!keepers.empty()) {
        for (Oop m : keepers) {
            memory_.pushMourner(m);
        }
        // pendingFinalizationSignals retained (no clear).  Signal will fire
        // at next activateMethod-end check, so FP wakes and drains.
    } else {
        // Queue fully drained (all WKAs processed natively + all WeakArrays
        // dropped).  No mourners left for FP to process.
        memory_.clearPendingFinalizationSignals();
    }

    if (drainLog++ < 5) {
        fprintf(stderr, "[DRAIN-%d] initial=%zu processed=%zu wka=%zu wkaarr-drop=%zu kept=%zu dec=%zu pending=%zu->%zu\n",
                drainLog, initialQueueSize, processed, wkaProcessed,
                weakArraysDropped, nonWkaKept, decremented,
                initialPending, memory_.pendingFinalizationSignals());
    }
}

#if PHARO_JIT_ENABLED  // re-open: tryOSRAtBackwardJump and below are JIT-only

void Interpreter::tryOSRAtBackwardJump() {
    if (g_debug.noOSR) return;
    if (!jitRuntime_.isInitialized()) return;
    if (inJITResume_) return;  // Already inside JIT resume loop

    // Sampling: only check every 64 backward jumps to amortize overhead.
    // Counter is reset to 64 after each successful OSR entry (JIT takes over
    // and handles subsequent backward jumps via stencil yield checks).
    if (--osrCountdown_ > 0) return;
    osrCountdown_ = 64;

    if (!method_.isObject() || method_.rawBits() < 0x10000) return;

    jit::JITMethod* jm = jitRuntime_.methodMap().lookup(method_.rawBits());
    if (!jm || !jm->isExecutable()) {
        // Not compiled yet — trigger compilation via noteMethodEntry.
        // With threshold=2, this compiles after 2 backward jumps of the
        // 64-sample window (128 actual backward jumps).
        jitRuntime_.noteMethodEntry(method_);
        // After noteMethodEntry, re-check: was the method just compiled?
        jm = jitRuntime_.methodMap().lookup(method_.rawBits());
        if (!jm || !jm->isExecutable()) return;
    }

    // Method is compiled — enter JIT at current IP (OSR)
    jitOSREntries_++;
    tryJITResumeInCaller();
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

    // Bug-14 diagnostic: log each invocation with caller method, retVal
    // on stack top, and next bc.  Rate-limited.
    if (g_debug.b5Trace && method_.isObject() && method_.rawBits() >= 0x10000) {
        static size_t cResume = 0;
        cResume++;
        if (cResume <= 2500) {
            std::string mcls = classNameOfMethod(method_);
            std::string msel = memory_.selectorOf(method_);
            long long bcOff = -1;
            uint8_t nextBC = 0;
            if (instructionPointer_) {
                ObjectHeader* mo = method_.asObjectPtr();
                Oop hdr = mo->slots()[0];
                int nLit = hdr.isSmallInteger()
                    ? (hdr.asSmallInteger() & 0x7FFF) : 0;
                const uint8_t* bcBase = mo->bytes() + (1 + nLit) * 8;
                bcOff = (long long)(instructionPointer_ - bcBase);
                nextBC = *instructionPointer_;
            }
            long long spFromFP = framePointer_
                ? (long long)(stackPointer_ - framePointer_) : -1;
            Oop top = (stackPointer_ > framePointer_)
                ? stackPointer_[-1] : Oop::fromRawBits(0);
            std::string topKind = top.isSmallInteger() ? "SmI"
                : top.isObject() ? memory_.classNameOf(top).c_str()
                : "other";
            fprintf(stderr, "[B5-RESUME-CALLER] #%zu sp-fp=%lld bcOff=%lld "
                            "nextBC=0x%02x top=0x%llx(%s) "
                            "cls=%s sel=#%s method_=0x%llx\n",
                    cResume, spFromFP, bcOff, nextBC,
                    (unsigned long long)top.rawBits(), topKind.c_str(),
                    mcls.c_str(), msel.c_str(),
                    (unsigned long long)method_.rawBits());
        }
    }

    int resumeIter = 0;
    while (running_ && jitRuntime_.isInitialized()) {
        if (++resumeIter > 10000) break;  // Safety limit
        // Break out if checkCountdown_ expired — let interpret() periodic
        // checks run (GC, timer, process scheduling, test triggers, etc.)
        if (checkCountdown_ <= 0) break;

        if (__builtin_expect(dispatchTraceLeakOn_, 0) && framePointer_) {
            long long _sp = (long long)(stackPointer_ - framePointer_);
            if (_sp >= 500 && _sp <= 520) {
                ObjectHeader* _mObj = method_.isObject() ? method_.asObjectPtr() : nullptr;
                long long _bc = -1;
                if (_mObj) {
                    Oop _hdr = _mObj->slots()[0];
                    int _nLit = _hdr.isSmallInteger() ? (_hdr.asSmallInteger() & 0x7FFF) : 0;
                    const uint8_t* _bcBase = _mObj->bytes() + (1 + _nLit) * 8;
                    if (instructionPointer_)
                        _bc = (long long)(instructionPointer_ - _bcBase);
                }
                fprintf(stderr, "[RESUME-ITER] iter=%d fd=%zu method=#%s(0x%llx) sp=%lld bc=%lld\n",
                        resumeIter, frameDepth_, memory_.selectorOf(method_).c_str(),
                        (unsigned long long)method_.rawBits(), _sp, _bc);
            }
        }

        // Validate method_ before using it
        if (!method_.isObject() || method_.rawBits() < 0x10000) break;

        // FAST PATH: check if caller method is compiled before expensive setup
        jit::JITMethod* jm = jitRuntime_.methodMap().lookup(method_.rawBits());
        if (!jm || !jm->isExecutable()) break;

        uint32_t bcOffset = computeCurrentBCOffset();
        if (bcOffset == UINT32_MAX) break;
        // B5 diagnostic: log the resume point in decodeBytes:.
        if (g_debug.b5Trace && method_.isObject()) {
            std::string sel = memory_.selectorOf(method_);
            if (sel == "decodeBytes:") {
                Oop top = stackPointer_ > framePointer_ ?
                    stackPointer_[-1] : Oop::fromRawBits(0);
                fprintf(stderr, "[B5-RESUME-JIT] decodeBytes: bcOffset=%u "
                               "top=0x%llx (%s)\n",
                        bcOffset, (unsigned long long)top.rawBits(),
                        top.isSmallInteger() ? "SmallInt"
                          : top.isObject() ? memory_.classNameOf(top).c_str()
                          : "other");
            }
        }

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
        // External J2J: stencils always exit with ExitSendCached (j2jSaveCursor
        // is null). The trampoline below converts to J2JCall when the callee is
        // compiled, avoiding the internal save-cursor mechanism which caused
        // severe slowdowns (stencil-managed J2J + frequent materialization from
        // inherited yieldCountdown and non-return exits).
        int rj2jBase = j2jPoolCursor_;
        state.j2jSaveCursor = nullptr;
        state.j2jSaveLimit = nullptr;
        state.j2jDepth = 0;
        state.j2jTotalCalls = 0;
        state.methodMapPtr = &jitRuntime_.methodMap();
        state.yieldCountdown = 1000;

        // Register this state as GC-reachable (see tryJITActivation for why).
        jit::JITState* prevJITState = currentJITState_;
        currentJITState_ = &state;
        struct JITStateGuard {
            Interpreter* self;
            jit::JITState* prev;
            ~JITStateGuard() { self->currentJITState_ = prev; }
        } jitStateGuard{this, prevJITState};

        // --- DIAGNOSTIC: scan stack for megaCache pointers before resume ---
        Oop* preResumeSP = stackPointer_;
        uint64_t megaBase = (uint64_t)jitRuntime_.megaCache();
        uint64_t megaEnd  = megaBase + 65536 * 32;
        {
            static bool megaScan = g_debug.jitMegaScan;
            if (megaScan) {
                // Scan from FP to SP for any values in megaCache range
                for (Oop* p = framePointer_; p < stackPointer_; p++) {
                    uint64_t v = p->rawBits();
                    if (v >= megaBase && v < megaEnd) {
                        fprintf(stderr, "[MEGA-PRE] CORRUPT at FP+%lld: 0x%llx (megaEntry %lld) BEFORE resume in #%s bc=%u\n",
                                (long long)(p - framePointer_),
                                (unsigned long long)v,
                                (long long)((v - megaBase) / 32),
                                memory_.selectorOf(method_).c_str(), bcOffset);
                    }
                }
            }
        }

        if (!jitRuntime_.tryResume(method_, bcOffset, state)) {
            j2jPoolCursor_ = rj2jBase;  // Release pool slice
            break;  // No re-entry at this offset
        }

        // --- DIAGNOSTIC: scan stack for megaCache pointers after resume ---
        {
            static bool megaScan = g_debug.jitMegaScan;
            if (megaScan) {
                for (Oop* p = framePointer_; p < state.sp; p++) {
                    uint64_t v = p->rawBits();
                    if (v >= megaBase && v < megaEnd) {
                        fprintf(stderr, "[MEGA-POST] CORRUPT at FP+%lld: 0x%llx (megaEntry %lld) AFTER resume in #%s bc=%u exit=%d\n",
                                (long long)(p - framePointer_),
                                (unsigned long long)v,
                                (long long)((v - megaBase) / 32),
                                memory_.selectorOf(method_).c_str(), bcOffset,
                                (int)state.exitReason);
                    }
                }
            }
        }
        jitJ2JStencilCalls_ += state.j2jTotalCalls;

        if (__builtin_expect(dispatchTraceLeakOn_, 0) && framePointer_) {
            long long _sp = (long long)(state.sp - framePointer_);
            if (_sp >= 498 && _sp <= 522) {
                ObjectHeader* _mObj = method_.isObject() ? method_.asObjectPtr() : nullptr;
                long long _bc = -1;
                if (_mObj) {
                    Oop _hdr = _mObj->slots()[0];
                    int _nLit = _hdr.isSmallInteger() ? (_hdr.asSmallInteger() & 0x7FFF) : 0;
                    const uint8_t* _bcBase = _mObj->bytes() + (1 + _nLit) * 8;
                    if (state.ip)
                        _bc = (long long)(state.ip - _bcBase);
                }
                fprintf(stderr, "[RESUME-EXIT] iter=%d fd=%zu method=#%s exit=%d state.sp=%lld state.bc=%lld\n",
                        resumeIter, frameDepth_, memory_.selectorOf(method_).c_str(),
                        (int)state.exitReason, _sp, _bc);
            }
        }

        // Charge the periodic check countdown for JIT-executed bytecodes.
        // Without this, the resume loop starves interpreter periodic checks
        // (GC, timer semaphores, process scheduling) when enough methods are
        // JIT-compiled that tryResume always succeeds.
        if (state.jitMethod) {
            checkCountdown_ -= state.jitMethod->numBytecodes;
            g_stepNum += state.jitMethod->numBytecodes;
        }

        // External J2J trampoline: when the stencil exits with ExitSendCached,
        // try to convert the send to a J2JCall (if callee is compiled). This
        // chains sends in JIT without materializing frames for each one.
        // Modeled on tryJITActivation's C++ trampoline.
        //
        // Default ON.  Removed the PHARO_RESUME_J2J workaround that was
        // gating this off (claim was 18% slower at low compile coverage,
        // but compile coverage now reaches 600+ methods immediately and
        // the workaround was bleeding 100x perf on send-heavy benches by
        // forcing every send through tryResume → C++ → re-entry.  Bench
        // block(500K) on Pi 5 is currently 134ms; should drop sharply
        // when sends stay in JIT).  PHARO_NO_RESUME_J2J is the new
        // bisection escape hatch if a regression appears.
        {
            // Chain loop default-OFF after multiple failed attempts to make
            // it default-on (see project_fib_hang_chainloop.md).  Opt in
            // with PHARO_RESUME_J2J=1 — known to hang the bench process
            // due to context-chain corruption that resume:through:
            // exclusions etc. don't fully fix.
            static bool noResumeJ2J = !g_debug.resumeJ2J;
            int rj2jDepth = 0;
            size_t rj2jCalls = 0, rj2jReturns = 0;
            J2JSave* rj2jSaves = &j2jPool_[rj2jBase];
            int rj2jMaxDepth = std::min(J2JSlotPerEntry,
                                        MaxJ2JPoolSize - rj2jBase);

          if (!noResumeJ2J) {
            // No flip — codebase invariant (W^X audit 2026-04-26): thread is in X mode.
            // PHARO_RJ2J_VALIDATE=1 enables state validation around every
            // JIT_CALL in the chain loop.  Catches the moment state.
            // receiver/sp/ip become corrupt — required for tracking down
            // the chain-loop induced JIT-state corruption that crashes
            // SDL_Renderer / Transcript-related JIT calls.
            static bool rj2jValidate = std::getenv("PHARO_RJ2J_VALIDATE") != nullptr;
            // Track sp drift across chain iterations.  For a balanced
            // call/return cycle, sp should net-decrease by sendArgCount
            // per J2J pair.  If sp grows faster than expected, there's
            // a leak somewhere — JIT pushing without popping.  This is
            // the suspected cause of "stopVM Corrupt stackPointer_" in
            // chain-loop default-on (signal pushes overflow stack end).
            Oop* spAtChainEntry = state.sp;
            Oop* spAtLastJ2JCall = nullptr;  // sp recorded just before each JIT_CALL
            int  spLastNArgs = 0;            // expected pop count for that call
            const char* spLastSite = "?";

            auto validateState = [&](const char* where, jit::JITMethod* expectedJM) {
                if (!rj2jValidate) return;
                bool bad = false;
                const char* badReason = "?";
                if (state.sp == nullptr || (uint64_t)state.sp < 0x10000) {
                    bad = true; badReason = "sp_low";
                }
                if (state.ip == nullptr || (uint64_t)state.ip < 0x10000) {
                    bad = true; badReason = "ip_low";
                }
                if (state.tempBase == nullptr || (uint64_t)state.tempBase < 0x10000) {
                    bad = true; badReason = "tempBase_low";
                }
                if (state.receiver.rawBits() == 0) {
                    bad = true; badReason = "receiver_null";
                }
                if ((state.receiver.rawBits() & 0x7) == 0 &&
                    state.receiver.rawBits() != 0 &&
                    state.receiver.rawBits() < 0x10000) {
                    bad = true; badReason = "receiver_bad_obj_addr";
                }
                if (expectedJM && state.jitMethod != expectedJM &&
                    !((uintptr_t)state.jitMethod & 1)) {
                    bad = true; badReason = "jitMethod_mismatch";
                }
                if (bad) {
                    static int rj2jBadCount = 0;
                    rj2jBadCount++;
                    if (rj2jBadCount <= 20) {
                        fprintf(stderr,
                                "[RJ2J-BAD] #%d at %s: %s sp=%p ip=%p tempBase=%p "
                                "receiver=0x%llx jm=%p exit=%d depth=%d sp_drift_from_entry=%lld\n",
                                rj2jBadCount, where, badReason,
                                (void*)state.sp, (void*)state.ip,
                                (void*)state.tempBase,
                                (unsigned long long)state.receiver.rawBits(),
                                (void*)state.jitMethod, state.exitReason,
                                rj2jDepth,
                                (long long)(state.sp - spAtChainEntry));
                    }
                }
                // SP-drift check: after a J2J-return, sp should equal
                // spAtLastJ2JCall - spLastNArgs (we pushed args+recv,
                // got back retval which replaced recv, popped args).
                if (rj2jValidate && spAtLastJ2JCall &&
                    std::strcmp(where, "post-Return-resume") == 0) {
                    long long actual = state.sp - spAtLastJ2JCall;
                    long long expected = -(long long)spLastNArgs;
                    if (actual != expected) {
                        static int spDriftCount = 0;
                        spDriftCount++;
                        if (spDriftCount <= 20) {
                            fprintf(stderr,
                                    "[RJ2J-SPDRIFT] #%d after %s (last_call_at %s): "
                                    "actual=%lld expected=%lld diff=%lld nArgs=%d depth=%d\n",
                                    spDriftCount, where, spLastSite,
                                    actual, expected, actual - expected,
                                    spLastNArgs, rj2jDepth);
                        }
                    }
                }
            };
            while (state.exitReason == jit::ExitSendCached ||
                   state.exitReason == jit::ExitYield ||
                   (state.exitReason == jit::ExitReturn && rj2jDepth > 0)) {

                // SAFE-POINT BAIL: bail to interpreter when checkCountdown_
                // expired and chain is at a Return-with-saves boundary.
                // The post-loop materialize+switch handles this cleanly:
                // materialize pushes rj2jSaves as savedFrames_, switch's
                // ExitReturn case pops the topmost saved frame (most
                // recent caller), pushes state.returnValue (callee's
                // retval), continues outer loop.  Outer loop's countdown
                // check breaks, returning to interpreter so periodic_check
                // can run the scheduler.
                //
                // Without this bail, the chain stays in JIT through 100K+
                // iterations of fib's recursive returns, starving the
                // bench process at P79 (Morphic at lower priority
                // dominates).  See project_fib_hang_chainloop.md.
                //
                // Bailing only on ExitReturn — Send/SendCached/Yield/
                // J2JCall have mid-flow state requirements the post-loop
                // switch can't reconstruct safely.
                if (state.exitReason == jit::ExitReturn &&
                    checkCountdown_ <= 0) {
                    static size_t bailCount = 0;
                    bailCount++;
                    if ((bailCount & 0x3FFF) == 1) {
                        Oop activeProc = getActiveProcess();
                        Oop pri = activeProc.isObject()
                            ? memory_.fetchPointer(ProcessPriorityIndex, activeProc) : Oop::nil();
                        long pVal = pri.isSmallInteger() ? pri.asSmallInteger() : -1;
                        fprintf(stderr,
                                "[CHAIN-BAIL] #%zu rj2jDepth=%d ccd=%d active_proc_pri=%ld\n",
                                bailCount, rj2jDepth, checkCountdown_, pVal);
                    }
                    break;
                }

                // --- ExitYield: charge countdown and re-enter callee ---
                if (state.exitReason == jit::ExitYield) {
                    jitYieldCount_++;
                    auto* yJM = reinterpret_cast<jit::JITMethod*>(
                        reinterpret_cast<uintptr_t>(state.jitMethod)
                        & ~static_cast<uintptr_t>(1));
                    int yNumBC = yJM ? yJM->numBytecodes : 20;
                    checkCountdown_ -= 1000 * yNumBC;
                    g_stepNum += 1000 * yNumBC;
                    if (checkCountdown_ <= 0) break;  // Scheduler needs to run

                    // Compute callee's resume address from yield IP
                    if (!yJM) break;
                    ObjectHeader* yMO =
                        Oop::fromRawBits(yJM->compiledMethodOop)
                            .asObjectPtr();
                    int yNL = static_cast<int>(yJM->methodHeader & 0x7FFF);
                    uint8_t* yBCStart = yMO->bytes() + (1 + yNL) * 8;
                    uint32_t yBCOff =
                        static_cast<uint32_t>(state.ip - yBCStart);
                    uint32_t yCodeOff = yJM->codeOffsetForBC(yBCOff);
                    if (yCodeOff == 0 || yCodeOff >= yJM->codeSize) break;

                    state.yieldCountdown = 1000;
                    JIT_CALL(yJM->codeStart() + yCodeOff, &state);
                    continue;
                }

                // --- ExitSendCached → ExitJ2JCall conversion ---
                if (state.exitReason == jit::ExitSendCached) {
                    if (!pharo_jit_convert_send(&state)) break;
                }

                if (state.exitReason == jit::ExitJ2JCall) {
                    // --- J2J Call: save caller, enter callee ---
                    if (rj2jDepth >= rj2jMaxDepth) {
                        state.exitReason = jit::ExitSendCached;
                        break;
                    }

                    rj2jCalls++;

                    jit::JITMethod* callerJM =
                        reinterpret_cast<jit::JITMethod*>(state.jitMethod);
                    uint8_t* entryAddr =
                        reinterpret_cast<uint8_t*>(state.returnValue.rawBits());
                    jit::JITMethod* calleeJM =
                        reinterpret_cast<jit::JITMethod*>(
                            entryAddr - sizeof(jit::JITMethod));
                    int nArgs = state.sendArgCount;

                    // Advance IP past send bytecode
                    {
                        uint8_t sendOp = *state.ip;
                        if (sendOp >= 0xEA && sendOp <= 0xEB) state.ip += 2;
                        else state.ip += 1;
                    }

                    // Save caller state
                    J2JSave& save = rj2jSaves[rj2jDepth++];
                    save.sp = state.sp;
                    save.receiver = state.receiver;
                    save.tempBase = state.tempBase;
                    save.ip = state.ip;
                    save.sendArgCount = nArgs;

                    bool selfRecursive = (callerJM == calleeJM);
                    int callerNumLits =
                        static_cast<int>(callerJM->methodHeader & 0x7FFF);
                    ObjectHeader* callerMethObj =
                        Oop::fromRawBits(callerJM->compiledMethodOop)
                            .asObjectPtr();
                    uint8_t* callerBCStart =
                        callerMethObj->bytes() + (1 + callerNumLits) * 8;

                    // literals/argCount/bcStart are derived from
                    // save.jitMethod on return (see J2J_INLINE_RETURN).
                    // The self-recursive marker is obsolete.
                    (void)selfRecursive;
                    save.jitMethod = callerJM;

                    // Precompute resume JIT code address
                    {
                        uint32_t bcOff =
                            static_cast<uint32_t>(state.ip - callerBCStart);
                        // Safety: refuse register-reading entry offsets —
                        // see JITRuntime::tryResume / deferred.md A1.
                        if (jitRuntime_.getBcEntryState(callerJM, bcOff) != 0) {
                            save.resumeAddr = nullptr;
                        } else {
                            uint32_t codeOff = callerJM->codeOffsetForBC(bcOff);
                            save.resumeAddr =
                                (codeOff == 0 || codeOff >= callerJM->codeSize)
                                    ? nullptr
                                    : callerJM->codeStart() + codeOff;
                        }
                    }

                    // Stack overflow check
                    if (__builtin_expect(
                            frameDepth_ + rj2jDepth >= StackOverflowLimit,
                            0)) {
                        rj2jDepth--;
                        state.exitReason = jit::ExitStackOverflow;
                        break;
                    }

                    // Setup callee in JITState
                    Oop targetMethod = state.cachedTarget;
                    ObjectHeader* methObj = targetMethod.asObjectPtr();
                    Oop calleeRecv = state.sp[-(nArgs + 1)];
                    Oop* fp = state.sp - (nArgs + 1);

                    state.receiver = calleeRecv;
                    state.tempBase = fp + 1;

                    if (__builtin_expect(!selfRecursive, 0)) {
                        state.literals = methObj->slots() + 1;
                        state.argCount = nArgs;
                        state.jitMethod = calleeJM;
                    }

                    if (__builtin_expect(selfRecursive, 1)) {
                        state.ip = callerBCStart;
                    } else {
                        int numLits = static_cast<int>(
                            calleeJM->methodHeader & 0x7FFF);
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

                    // SAFETY: refuse J2J call if state.sp is too close
                    // to the stack end.  Reserve 4096 slots of headroom
                    // (was 1024 — interpreter post-bail still pushed
                    // 5+ slots past end with smaller margin).  Without
                    // this, accumulated drift across many chain iterations
                    // causes "Corrupt stackPointer_" stopVM.
                    if ((uintptr_t)state.sp + 4096 * sizeof(Oop) >=
                        (uintptr_t)(stack_.data() + MaxStackDepth)) {
                        rj2jDepth--;
                        state.exitReason = jit::ExitStackOverflow;
                        break;
                    }
                    validateState("pre-J2JCall", calleeJM);
                    spAtLastJ2JCall = state.sp;
                    spLastNArgs = nArgs;
                    spLastSite = "J2JCall";
                    JIT_CALL(entryAddr, &state);
                    validateState("post-J2JCall", nullptr);

                } else {
                    // --- ExitReturn: pop save, re-enter caller ---
                    rj2jReturns++;
                    rj2jDepth--;

                    Oop retVal = state.returnValue;
                    J2JSave& save = rj2jSaves[rj2jDepth];

                    state.sp = save.sp;
                    state.receiver = save.receiver;
                    state.tempBase = save.tempBase;

                    // Derive literals/argCount/ip from save.jitMethod —
                    // see docs/jit-j2j-reduction-plan.md.  The old
                    // self-recursive marker is gone.
                    {
                        auto* savedJM = save.jitMethod;
                        state.jitMethod = savedJM;
                        state.literals = reinterpret_cast<Oop*>(savedJM->literals());
                        state.argCount = savedJM->argCount;
                        state.ip = savedJM->bcStart();
                    }

                    // Pop receiver+args, push return value
                    state.sp[-(save.sendArgCount + 1)] = retVal;
                    state.sp -= save.sendArgCount;

                    if (__builtin_expect(save.resumeAddr == nullptr, 0)) {
                        state.ip = save.ip;
                        state.exitReason = jit::ExitReturn;
                        break;
                    }

                    // SAFETY: same stack-headroom check as J2JCall path.
                    if ((uintptr_t)state.sp + 4096 * sizeof(Oop) >=
                        (uintptr_t)(stack_.data() + MaxStackDepth)) {
                        state.ip = save.ip;
                        state.exitReason = jit::ExitStackOverflow;
                        break;
                    }
                    validateState("pre-Return-resume", save.jitMethod);
                    spAtLastJ2JCall = state.sp;
                    spLastNArgs = 0;  // resume isn't a call — no args to pop
                    spLastSite = "Return-resume";
                    JIT_CALL(save.resumeAddr, &state);
                    validateState("post-Return-resume", nullptr);
                }

                // Charge bytecodes for each trampoline iteration
                if (state.jitMethod) {
                    auto* jm = state.jitMethod;
                    checkCountdown_ -= jm->numBytecodes;
                    g_stepNum += jm->numBytecodes;
                }
            }

            // Stay in X — W^X audit 2026-04-26.  All JITMethod-field
            // writes outside this loop now go to the heap stats struct
            // or into RAII-protected scopes (patchJITICAfterSend,
            // upgradeICToJ2J).
          } // !noResumeJ2J

            // Reconstruct state.method from state.jitMethod.
            // (Previously masked out the obsolete self-recursive marker bit.)
            if (state.jitMethod) {
                state.method = Oop::fromRawBits(
                    state.jitMethod->compiledMethodOop);
            }

            // Merge stats
            jitJ2JStencilCalls_ += rj2jCalls + state.j2jTotalCalls;
            jitJ2JStencilReturns_ += rj2jReturns;
            checkCountdown_ -= static_cast<int>(rj2jCalls + rj2jReturns) * 10;

            // Materialize remaining J2J saves if trampoline bailed with depth>0
            if (rj2jDepth > 0) {
                Oop nil = memory_.nil();
                for (int i = 0; i < rj2jDepth; i++) {
                    if (frameDepth_ >= StackOverflowLimit) break;
                    J2JSave& save = rj2jSaves[i];
                    jit::JITMethod* saveJM = save.jitMethod;
                    if (!saveJM) {
                        // Half-materialized state — frameDepth_ has been
                        // incremented for prior iterations but state.method
                        // never got synced.  Bail the entire materialize so
                        // the post-loop code that reads state.method->bytes()
                        // doesn't crash.  Restore frameDepth_ to baseline by
                        // un-pushing the in-progress slot count.
                        static int warns = 0;
                        if (++warns <= 5)
                            fprintf(stderr, "[JIT] WARN: null save.jitMethod at rj2j materialize (warn #%d) — bailing materialize\n", warns);
                        // Roll back the i frames we pushed.
                        for (int j = 0; j < i; j++) {
                            if (frameDepth_ > 0) frameDepth_--;
                        }
                        j2jPoolCursor_ = rj2jBase;
                        inJITResume_ = false;
                        return;
                    }
                    SavedFrame& frame = savedFrames_[frameDepth_++];
                    Oop saveMethod = Oop::fromRawBits(saveJM->compiledMethodOop);
                    frame.savedIP = save.ip;
                    frame.savedBytecodeEnd =
                        saveJM->bcStart() + saveJM->numBytecodes;
                    frame.savedMethod = saveMethod;
                    frame.savedHomeMethod = saveMethod;
                    frame.savedReceiver = save.receiver;
                    frame.savedClosure = nil;
                    frame.savedActiveContext = nil;
                    frame.materializedContext = nil;
                    frame.savedFP = save.tempBase - 1;
                    frame.savedArgCount = saveJM->argCount;
                    frame.homeFrameDepth = SIZE_MAX;
                }
                // Sync interpreter state from innermost callee
                method_ = state.method;
                homeMethod_ = state.method;
                receiver_ = state.receiver;
                stackPointer_ = state.sp;
                instructionPointer_ = state.ip;
                framePointer_ = state.tempBase - 1;
                argCount_ = state.argCount;
                {
                    ObjectHeader* mObj = state.method.asObjectPtr();
                    if (mObj) bytecodeEnd_ = mObj->bytes() + mObj->byteSize();
                }
                jitMaterializeCount_++;
                jitMaterializeTotalDepth_ += rj2jDepth;
                j2jPoolCursor_ = rj2jBase;
                inJITResume_ = false;
                return;
            }
        }

        j2jPoolCursor_ = rj2jBase;

        switch (state.exitReason) {
        case jit::ExitReturn:
            // Bug-14 diagnostic (tryResume path)
            if (g_debug.b5Trace) {
                static size_t c12791 = 0;
                c12791++;
                if (c12791 <= 2500) {
                    std::string mcls = classNameOfMethod(state.method);
                    std::string msel = memory_.selectorOf(state.method);
                    Oop rv = state.returnValue;
                    std::string rvKind = rv.isSmallInteger() ? "SmI"
                        : rv.isObject() && rv.rawBits() >= 0x10000
                            ? memory_.classNameOf(rv).c_str()
                            : "other";
                    fprintf(stderr, "[B5-EXIT-tryResume] #%zu sp=%p retVal=0x%llx(%s) "
                                    "localFrameDepth=%zu method=0x%llx cls=%s sel=#%s\n",
                            c12791, state.sp,
                            (unsigned long long)rv.rawBits(), rvKind.c_str(),
                            frameDepth_,
                            (unsigned long long)state.method.rawBits(),
                            mcls.c_str(), msel.c_str());
                }
            }
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
                    handleBenchComplete(state.returnValue);
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
                lastJitReturn_.methodBits = state.method.rawBits();
                lastJitReturn_.returnBits = state.returnValue.rawBits();
                lastJitReturn_.frameDepth = frameDepth_;
                lastJitReturn_.wasResume = true;
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
                for (int e = 0; e < 6; e++) {
                    if (state.icDataPtr[e * 3] == 0) { hasEmpty = true; break; }
                }
                if (hasEmpty) {
                    pendingICPatch_ = state.icDataPtr;
                    pendingICSendArgCount_ = state.sendArgCount;
                    pendingICOwnerMethod_ = state.method;
                    static size_t setCount1 = 0; setCount1++;
                    if (g_debug.icPatchDebug && (setCount1 & 0xFFF) == 1) {
                        fprintf(stderr, "[IC-PEND-SET1] tryResume ExitSend count=%zu ic=%p\n",
                                setCount1, (void*)state.icDataPtr);
                    }
                } else {
                    static size_t fullCount1 = 0; fullCount1++;
                    if (g_debug.icPatchDebug && (fullCount1 & 0xFFF) == 1) {
                        fprintf(stderr, "[IC-PEND-FULL1] tryResume IC full count=%zu ic=%p\n",
                                fullCount1, (void*)state.icDataPtr);
                    }
                }
            } else {
                static size_t noICDataCount = 0; noICDataCount++;
                if (g_debug.icPatchDebug && (noICDataCount & 0xFFF) == 1) {
                    fprintf(stderr, "[IC-PEND-NONE] tryResume ExitSend no icDataPtr count=%zu\n",
                            noICDataCount);
                }
            }
            inJITResume_ = false;
            return;
        }

        case jit::ExitSendCached: {
            // IC hit during resume — activate cached method, then resume caller
            Oop cached = state.cachedTarget;
            if (!cached.isObject() || cached.rawBits() < 0x10000 ||
                !memory_.isValidPointer(cached) ||
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
            countICHitDbg(state.icDataPtr);
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
            {
                const bool dbgOn = g_debug.debugArithExit;
                if (__builtin_expect(dbgOn, 0) && framePointer_) {
                    long long spFromFP = (long long)(state.sp - framePointer_);
                    if (spFromFP >= 400 && spFromFP <= 700) {
                        ObjectHeader* mObj = method_.isObject() ? method_.asObjectPtr() : nullptr;
                        const uint8_t* bcBase = nullptr;
                        if (mObj) {
                            Oop hdr = mObj->slots()[0];
                            int numLits = hdr.isSmallInteger() ? (hdr.asSmallInteger() & 0x7FFF) : 0;
                            bcBase = mObj->bytes() + (1 + numLits) * 8;
                        }
                        long long bcOff = (bcBase && state.ip) ? (long long)(state.ip - bcBase) : -1;
                        fprintf(stderr, "[ARITH-EXIT-R] fd=%zu sp-fp=%lld state.ip bcOff=%lld bc=%02x method=#%s\n",
                                frameDepth_, spFromFP, bcOff,
                                (state.ip ? *state.ip : 0),
                                memory_.selectorOf(method_).c_str());
                    }
                }
            }
            instructionPointer_ = state.ip;
            stackPointer_ = state.sp;
            inJITResume_ = false;
            return;

        case jit::ExitYield: {
            // Backward-jump yield during resume — charge countdown and continue
            jitYieldCount_++;
            instructionPointer_ = state.ip;
            stackPointer_ = state.sp;
            // Each yield = ~1000 backward jumps. Estimate bytecodes executed.
            int numBC = state.jitMethod ? state.jitMethod->numBytecodes : 20;
            int charge = 1000 * numBC;
            checkCountdown_ -= charge;
            g_stepNum += charge;
            if (checkCountdown_ <= 0) {
                inJITResume_ = false;
                return;
            }
            continue;  // Re-enter resume loop (will setup new JITState + tryResume)
        }

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
            countICHitDbg(state.icDataPtr);
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
    case 70: return 17;  // new (basicNew)
    case 71: return 18;  // new: (basicNew:)
    default: return 0;
    }
}

std::vector<sista::InlineHint>
Interpreter::extractInlineHintsForMethod(Oop method) {
    std::vector<sista::InlineHint> hints;
    auto* jm = jitRuntime_.methodMap().lookup(method.rawBits());
    if (!jm || jm->numICEntries == 0) return hints;
    const auto* sendBCs = jitRuntime_.compiler()
        ? jitRuntime_.compiler()->getSendSiteBCOffsets(method.rawBits())
        : nullptr;
    const uint8_t* icStart =
        jm->codeStart() + jm->codeSize
        - jm->numICEntries * jit::IC_BYTES_PER_SITE;
    for (uint16_t sendIdx = 0; sendIdx < jm->numICEntries; ++sendIdx) {
        const uint8_t* icBase =
            icStart + sendIdx * jit::IC_BYTES_PER_SITE;
        const uint64_t* ic =
            reinterpret_cast<const uint64_t*>(icBase);
        uint64_t classKey0 = ic[0];
        uint64_t method0   = ic[1];
        uint64_t classKey1 = ic[3];
        // Phase 5 Step 1 (2026-04-29): count polymorphic site degree
        // for capacity planning.  Dominant entry is ic[0..2]; chain
        // emission (Step 2) reads ic[3..5], ic[6..8], etc.
        uint8_t polyDegree = 0;
        for (uint32_t e = 0; e < jit::IC_ENTRIES_PER_SITE; ++e) {
            if (ic[e * 3] != 0) polyDegree++;
            else break;
        }
        sista::Builder::recordPolyDegree(polyDegree);
        // Phase 5 Step 2 (2026-04-29): under PHARO_SISTA_INLINE_POLY,
        // emit a hint for the dominant entry of polymorphic sites too.
        // The existing kGuardClass + inline path handles dominant-class
        // hits and bails to interp on alt-class (where T1's full IC
        // routes correctly).  Net: dominant class fast-paths into
        // compiled code; cold-tail invocations cost the same deopt
        // they cost today (when the whole site bailed unspeculated).
        // Gated behind PHARO_SISTA_INLINE_POLY=1 for soak.
        //
        // Default-on attempt 2026-04-29 hit a dict 50K regression
        // (154→176, +14%) even with a degree ≤ 3 cap.  Issue: the
        // IC's first entry isn't actually the most-frequently-hit
        // class — it's just the first observed.  Guard misses pay
        // deopt cost per miss.  Real fix needs hit-count tracking
        // per IC entry to identify the *actual* dominant class.
        // Until then, leave opt-in.
        static const bool inlinePoly =
            std::getenv("PHARO_SISTA_INLINE_POLY") != nullptr;
        bool emit = (classKey0 != 0
                     && (classKey1 == 0 || inlinePoly));
        if (emit) {
            uint16_t bcOff = (sendBCs && sendIdx < sendBCs->size())
                ? (*sendBCs)[sendIdx] : UINT16_MAX;
            if (bcOff != UINT16_MAX) {
                hints.push_back({bcOff, classKey0, method0});
            }
        }
    }
    return hints;
}

void Interpreter::patchJITICAfterSend(Oop resolvedMethod, Oop receiver, Oop selector) {
    static bool patchDbg = g_debug.icPatchDebug;
    static size_t dbgCalled = 0, dbgNoPending = 0, dbgSelMismatch = 0,
                  dbgDup = 0, dbgPatched = 0, dbgFullNoSlot = 0;
    dbgCalled++;
    if (!pendingICPatch_) { dbgNoPending++;
        if (patchDbg && (dbgNoPending & 0xFFFF) == 1) {
            fprintf(stderr, "[IC-PATCH-DBG] call=%zu noPending=%zu selMis=%zu dup=%zu patched=%zu full=%zu\n",
                    dbgCalled, dbgNoPending, dbgSelMismatch, dbgDup, dbgPatched, dbgFullNoSlot);
        }
        return; }
    uint64_t* icData = pendingICPatch_;
    pendingICPatch_ = nullptr;

    // IC data lives in the MAP_JIT code zone; ensure writable for the patch.
    // RAII guard flips the whole zone back to executable on every exit path.
    // On Apple Silicon pthread_jit_write_protect_np is a per-thread toggle
    // that affects the ENTIRE MAP_JIT region — leaving it writable here
    // means this thread cannot execute JIT code on return, causing a
    // SIGBUS "Fault addr = PC in code zone" as soon as JIT code resumes.
    jit::makeWritable(icData, 1);
    struct RestoreExec {
        jit::CodeZone& z;
        ~RestoreExec() { jit::makeExecutable(z.rawStart(), z.totalBytes()); }
    } restoreExec{jitRuntime_.codeZone()};

    // DEBUG: Selector-based J2J fill skip (PHARO_J2J_SKIP_SELECTORS).
    // Same skip used by upgradeICToJ2J — uses the IC's send-site selector
    // (passed in directly here), which is reliable.
    {
        static const char* skipEnv = g_debug.j2jSkipSelectors;
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
    //
    // FIX 2026-04-28 (megamorphic-dispatch / nextHandlerContext crash):
    // After GC, recoverAfterGC zeroes icData[18] to disarm the megacache
    // probe's deref of stale selector bytes.  When slot 18 is 0, the
    // simple `icSelectorBits != 0` gate below is bypassed and a stale
    // pendingICPatch_ from a different send site can poison this IC
    // (e.g., #method getter classification leaks into a #sender site,
    // making the inline-getter read slot 3 instead of slot 0).
    // Fix: when icData[18] is 0, consult the side-channel selBitsArray
    // (set at compile time, never zeroed) to recover this site's expected
    // selector and compare against the send's selector.
    uint64_t icSelectorBits = icData[18];
    if (icSelectorBits == 0 && pendingICOwnerMethod_.isObject()
            && pendingICOwnerMethod_.rawBits() > 0x10000) {
        if (auto* jm = jitRuntime_.methodMap().lookup(
                pendingICOwnerMethod_.rawBits())) {
            if (jm->numICEntries > 0) {
                uint8_t* icStart = jm->codeStart() + jm->codeSize
                                 - jm->numICEntries * jit::IC_BYTES_PER_SITE;
                ptrdiff_t off = reinterpret_cast<uint8_t*>(icData) - icStart;
                if (off >= 0
                        && (off % jit::IC_BYTES_PER_SITE) == 0) {
                    uint32_t siteIdx = (uint32_t)(off / jit::IC_BYTES_PER_SITE);
                    if (siteIdx < jm->numICEntries) {
                        if (uint64_t* sba = jm->selBitsArray()) {
                            icSelectorBits = sba[siteIdx];
                        }
                    }
                }
            }
        }
    }
    if (icSelectorBits != 0 && icSelectorBits != selector.rawBits()) {
        dbgSelMismatch++;
        if (patchDbg && dbgSelMismatch <= 5) {
            std::string got = memory_.selectorOf(selector);
            fprintf(stderr, "[IC-PATCH-DBG] selector MISMATCH: ic=0x%llx send=#%s\n",
                    (unsigned long long)icSelectorBits, got.c_str());
        }
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
    for (int e = 0; e < 6; e++) {
        if (icData[e * 3] == lookupKey) { dbgDup++;
            if (patchDbg && dbgDup <= 5) {
                std::string sel = memory_.selectorOf(selector);
                fprintf(stderr, "[IC-PATCH-DBG] DUP #%s key=0x%llx slot=%d (IC already has)\n",
                        sel.c_str(), (unsigned long long)lookupKey, e);
            }
            return;
        }
    }

    // Detect inline getter/setter/yourself for J2J dispatch.
    // extra word bit layout:
    //   bit 63: getter — slot index in bits 15:0
    //   bit 62: setter — slot index in bits 15:0
    //   bit 61: returnsSelf
    //   bit 60: hasJITEntry — bits 47:0 = JIT code entry address
    uint64_t extra = 0;
    {
        // PHARO_NO_GETTER_BIT=1: disable the inline-getter/setter/returnsSelf
        // bit-63/62/61 classification (diagnostic only since the cross-site
        // poisoning was fixed 2026-04-28; left as a kill-switch).
        // PHARO_NO_GETTER_BIT_BISECT=63|62|61 disables only one bit.
        static const char* bisect = std::getenv("PHARO_NO_GETTER_BIT_BISECT");
        static const bool noGetterBit =
            std::getenv("PHARO_NO_GETTER_BIT") != nullptr;
        // Only set bit-63/62/61 trivial classifications for heap-class entries.
        // For immediate (SmI / Char / etc., tag != 0) receivers, the
        // stencil's `tag == 0` gate skips the inline-getter path, so the
        // classification is useless AND would conflict with bit-60 J2J
        // entry encoding (low 48 bits hold jitAddr; OR-merge corrupts
        // slotIdx in bits 15:0).  Skip the classification for non-heap
        // receivers so bit-60 J2J can take over cleanly below.
        bool receiverIsHeap = (receiver.rawBits() & 0x7) == 0
                              && receiver.rawBits() >= 0x10000;
        if (!noGetterBit && receiverIsHeap) {
            TrivialMethodInfo tmi = detectTrivialMethod(resolvedMethod, memory_);
            bool skip63 = bisect && std::strcmp(bisect, "63") == 0;
            bool skip62 = bisect && std::strcmp(bisect, "62") == 0;
            bool skip61 = bisect && std::strcmp(bisect, "61") == 0;
            if (!skip63 && tmi.getterIndex >= 0)
                extra = (1ULL << 63) | (uint16_t)tmi.getterIndex;
            else if (!skip62 && tmi.setterIndex >= 0)
                extra = (1ULL << 62) | (uint16_t)tmi.setterIndex;
            else if (!skip61 && tmi.returnsSelf)
                extra = (1ULL << 61);
        }
    }

    // Quick primitives (256-519): map to inline getter/returnsSelf bits
    if (extra == 0) {
        int primIdx = primitiveIndexOf(resolvedMethod);
        if (primIdx >= 264 && primIdx <= 519)
            extra = (1ULL << 63) | static_cast<uint16_t>(primIdx - 264);
        else if (primIdx == 256)
            extra = (1ULL << 61);

        // Set inline primKind bits for methods with inlineable primitives,
        // regardless of JIT compilation status. This allows the stencil to
        // handle SmallInteger arithmetic inline without any function call.
        if (extra == 0) {
            uint8_t pk = inlinePrimKind(primIdx);
            if (pk) extra |= (uint64_t)pk << 48;
        }

        // Block value primitives (207, 209): mark for stencil block evaluation.
        // The stencil does a method map lookup to find the block's JIT code
        // and does a J2J call directly, avoiding executePrimitive/activateBlock.
        if (primIdx == 207 || primIdx == 209) {
            // PHARO_NO_BLOCK_BIT=1: omit the BLOCK_VALUE_BIT to force
            // the stencil's slow path.  Used to A/B test whether the
            // fast path is actually taken in benchmarks.
            static const bool noBlockBit =
                std::getenv("PHARO_NO_BLOCK_BIT") != nullptr;
            if (!noBlockBit) {
                extra |= (1ULL << 59);  // BLOCK_VALUE_BIT
            }
        }
    }

    // If not a trivial method, check for JIT-compiled target for J2J direct calls.
    // IMPORTANT: Don't set J2J for methods with primitives but no prologue stencil —
    // J2J skips CallPrimitive (it compiles to stencil_nop), so the primitive never runs.
    //
    // FIX 2026-04-28: also don't OR-in J2J bits when the entry is already
    // classified as a trivial inline-getter/setter/returnsSelf (bits 63/62/61).
    // Without this guard, extra ends up with BOTH bit 63 AND bit 60 set, plus
    // bits 47:0 = jitAddr — and the inline-getter fast path reads the LOW 16
    // BITS as slotIdx, which equals jitAddr & 0xFFFF (a huge value).  The
    // resulting recvObj->slotAt(huge) reads way past the receiver, returning
    // garbage that gets passed as the result of #sender / #method / etc.
    //
    // For heap-class entries (tag==0): inline-getter / setter / returnsSelf
    // ALWAYS fires (the IC lookupKey == receiver classIndex match guarantees
    // the receiver IS the cached class), so J2J is never reached anyway.
    //
    // For immediate (SmI / Char / etc., tag != 0) receivers: inline-getter
    // path is gated by `tag == 0` and never fires.  In that case bit 63 is
    // useless and we should let bit-60 J2J take over instead — so we skip
    // setting bit 63 entirely up front for non-heap receivers (handled in
    // the trivial-method classification below — see PHARO_NO_GETTER_BIT
    // gating).  The TRIVIAL_BITS guard here is the second line of defense
    // for heap-class entries.
    static bool j2jEnabled = !g_debug.noJ2J;
    constexpr uint64_t TRIVIAL_BITS =
        (1ULL << 63) | (1ULL << 62) | (1ULL << 61);
    if ((extra & TRIVIAL_BITS) == 0 &&
        (extra & (1ULL << 60)) == 0 && j2jEnabled) {
        jit::JITMethod* target = jitRuntime_.methodMap().lookup(resolvedMethod.rawBits());
        if (target && target->isExecutable()) {
            // Check if target has a primitive but no prologue.
            // Quick constant prims (257-263: return true/false/nil/-1/0/1/2)
            // are safe for J2J — their bytecodes produce the same result.
            bool unsafePrim = false;
            if (!target->hasPrimPrologue) {
                ObjectHeader* methObj = resolvedMethod.asObjectPtr();
                Oop headerOop = methObj->slotAt(0);
                if (headerOop.isSmallInteger()) {
                    int64_t hdr = headerOop.asSmallInteger();
                    if ((hdr >> 16) & 1) {  // hasPrimitive flag
                        int pi = primitiveIndexOf(resolvedMethod);
                        if (!(pi >= 257 && pi <= 263))
                            unsafePrim = true;
                    }
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

    // Selector cross-check: `selector` is the send's selector (already compared
    // to icData[18] above). But we ALSO need the resolvedMethod's own selector
    // to match — if a caller hands us a method whose selector differs, we'd
    // poison the IC. Unwrap AdditionalMethodState when penultimate lit is
    // a non-bytes object with slotCount>=2.
    {
        size_t nLits = memory_.numLiteralsOf(resolvedMethod);
        if (nLits >= 2) {
            Oop rmSel = memory_.fetchPointer(nLits - 1, resolvedMethod);
            uint64_t rmSelBits = rmSel.rawBits();
            if (rmSel.isObject() && rmSelBits >= 0x10000) {
                ObjectHeader* h = rmSel.asObjectPtr();
                if (!h->isBytesObject() && h->slotCount() >= 2) {
                    rmSelBits = h->slotAt(1).rawBits();
                }
            }
            if (rmSelBits != 0 && rmSelBits != selector.rawBits()) {
                return;  // Selector mismatch — don't poison the IC
            }
        }
    }


    // Find the first empty slot and fill it
    for (int e = 0; e < 6; e++) {
        if (icData[e * 3] == 0) {
            icData[e * 3] = lookupKey;
            icData[e * 3 + 1] = resolvedMethod.rawBits();
            icData[e * 3 + 2] = extra;
            jitICPatches_++;
            dbgPatched++;
            // Debug: log J2J patches for high-frequency methods
            static int logCount = 0;
            if ((extra & (1ULL << 60)) && logCount < 30) {
                logCount++;
                std::string sel = memory_.selectorOf(resolvedMethod);
                fprintf(stderr, "[IC-PATCH] #%s J2J=1 key=0x%llx extra=0x%llx\n",
                        sel.c_str(), (unsigned long long)lookupKey, (unsigned long long)extra);
            }
            // Smart Sista invalidation: only re-compile the caller's
            // entry if it was originally compiled WITHOUT hints.
            // invalidateIfHintless() is a no-op for hint-bearing
            // entries.  Only fires on the FIRST IC slot (slot 0)
            // because subsequent slots don't change peephole
            // eligibility (hints are monotonic).
            if (e == 0 && sistaRuntimeForGCHook_
                && pendingICOwnerMethod_.isObject()
                && pendingICOwnerMethod_.rawBits() > 0x10000) {
                sistaRuntimeForGCHook_->invalidateIfHintless(
                    pendingICOwnerMethod_);
            }
            return;
        }
    }
    dbgFullNoSlot++;
    if (patchDbg && dbgFullNoSlot <= 5) {
        std::string sel = memory_.selectorOf(selector);
        fprintf(stderr, "[IC-PATCH-DBG] FULL (all 4 slots used) #%s key=0x%llx\n",
                sel.c_str(), (unsigned long long)lookupKey);
    }
    // All 4 slots full — megamorphic, don't patch
}

void Interpreter::upgradeICToJ2J(uint64_t* icData, Oop cachedMethod, int sendArgCount,
                                  Oop callerMethod) {
    static bool j2jEnabled = !g_debug.noJ2J;
    static bool fillEnabled = !g_debug.noICFill;
    if (!j2jEnabled || !icData) return;

    // IC data lives in the MAP_JIT code zone.  Open a W window for the
    // duration of this function, restored to X on every exit path.
    // Required by the W^X audit 2026-04-26 default-X invariant — bare
    // icData[] writes below would SIGBUS without this guard.
    jit::makeWritable(icData, 19 * sizeof(uint64_t));
    struct RestoreExec {
        jit::CodeZone& z;
        ~RestoreExec() { jit::makeExecutable(z.rawStart(), z.totalBytes()); }
    } restoreExec{jitRuntime_.codeZone()};

    // DEBUG: Selector-based J2J upgrade skip (PHARO_J2J_SKIP_SELECTORS).
    // Lets us bisect which method's J2J upgrade triggers a bug. Note that
    // PHARO_JIT_SKIP_SELECTORS prevents JIT compilation entirely; this only
    // prevents the J2J fast-path patch.
    //
    // We compare against the IC's selector (icData[18]) rather than
    // selectorOf(cachedMethod), because the latter is unreliable for some
    // primitive methods (e.g. at:/at:put: returned "?" via numLiteralsOf,
    // making the bisection mis-fire).
    {
        static const char* skipEnv = g_debug.j2jSkipSelectors;
        if (skipEnv && *skipEnv) {
            std::string sel;
            uint64_t icSelBits = icData[18];
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
    static bool noEagerCompile = g_debug.noEagerCompile;
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
                // compile() ends in EXECUTABLE mode (per the W^X audit
                // 2026-04-26 invariant).  Re-open our W window so the
                // icData[] writes below succeed.
                jit::makeWritable(icData, 19 * sizeof(uint64_t));
            }
        }
    }

    if (!target || !target->isExecutable()) return;
    if (isJ2JBanned(cachedMethod.rawBits())) return;

    // Check unsafe prim: has primitive but no JIT prologue.
    // Quick primitives (256-519) are trivial — handle them via existing
    // inline getter/returnsSelf IC paths instead of bailing out.
    uint64_t quickPrimExtra = 0;  // Non-zero if quick prim detected
    if (!target->hasPrimPrologue) {
        ObjectHeader* methObj = cachedMethod.asObjectPtr();
        Oop hdr = methObj->slotAt(0);
        if (hdr.isSmallInteger() && ((hdr.asSmallInteger() >> 16) & 1)) {
            int primIdx = primitiveIndexOf(cachedMethod);
            if (primIdx >= 264 && primIdx <= 519) {
                // Quick instVar getter → inline getter (bit 63)
                quickPrimExtra = (1ULL << 63) | static_cast<uint16_t>(primIdx - 264);
            } else if (primIdx == 256) {
                // Quick returnSelf → inline returnsSelf (bit 61)
                quickPrimExtra = (1ULL << 61);
            }
            if (quickPrimExtra == 0) {
                // Quick constant prims (257-263) are safe for J2J:
                // bytecodes produce the same result as the primitive.
                // Fall through to J2J entry setting below.
                if (!(primIdx >= 257 && primIdx <= 263)) {
                    return;  // genuinely unsafe primitive
                }
            }
            // Fall through to IC search with quickPrimExtra
        }
    }

    // Find the receiver on the stack to compute the lookup key
    Oop receiver = stackPointer_[-(sendArgCount + 1)];
    uint64_t lookupKey;
    uint64_t tag = receiver.rawBits() & 0x7;
    if (tag == 0 && receiver.rawBits() >= 0x10000) {
        lookupKey = receiver.asObjectPtr()->classIndex();
    } else if (tag != 0) {
        lookupKey = tag | 0x80000000ULL;
    } else {
        return;
    }

    // Find the matching IC entry and upgrade, or fill an empty slot.
    // Note: todo.md §2.7 proposes layering J2J bits onto entries that
    // already have inline-primKind bits (52:48).  Attempted 2026-04-18:
    // regressed benchmarks badly (T2=1 went from 5/8 fast-mode to 0/8
    // with average ~410ms).  The interaction between primKind inline
    // dispatch and J2J direct call in stencil_sendJ2J is subtle —
    // layering them creates a slower path on some branch.  Reverted.
    // Left as-is: only upgrade when extra==0.
    int firstEmpty = -1;
    for (int e = 0; e < 6; e++) {
        if (icData[e * 3] == lookupKey) {
            uint64_t extra = icData[e * 3 + 2];
            if (extra == 0) {
                uint64_t newExtra;
                if (quickPrimExtra != 0) {
                    newExtra = quickPrimExtra;
                } else {
                    uint64_t entryAddr = reinterpret_cast<uint64_t>(target->codeStart());
                    newExtra = (1ULL << 60) | (entryAddr & 0x0000FFFFFFFFFFFFULL);
                    if (target->hasPrimPrologue) {
                        int primIdx = primitiveIndexOf(cachedMethod);
                        uint8_t pk = inlinePrimKind(primIdx);
                        if (pk) newExtra |= (uint64_t)pk << 48;
                    }
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
        uint64_t newExtra = 0;
        if (quickPrimExtra != 0) {
            newExtra = quickPrimExtra;
        } else {
            // Detect trivial getter/setter/returnsSelf — for these, set inline
            // bits 63/62/61 instead of the J2J direct call bit 60.
            // Also gated by PHARO_NO_GETTER_BIT for the same reason as above.
            static const bool noGetterBit2 =
                std::getenv("PHARO_NO_GETTER_BIT") != nullptr;
            TrivialMethodInfo tmi = detectTrivialMethod(cachedMethod, memory_);
            if (!noGetterBit2 && tmi.getterIndex >= 0)
                newExtra = (1ULL << 63) | (uint16_t)tmi.getterIndex;
            else if (!noGetterBit2 && tmi.setterIndex >= 0)
                newExtra = (1ULL << 62) | (uint16_t)tmi.setterIndex;
            else if (!noGetterBit2 && tmi.returnsSelf)
                newExtra = (1ULL << 61);
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
        }

        if (newExtra == 0) return;  // non-trivial method without JIT target

        // Probe: verify cachedMethod's selector matches the IC's recorded selector.
        // If they differ, this is cross-site IC poisoning — DON'T write.
        // FIX 2026-04-28: when icData[18] is 0 (post-GC zeroed), fall back to
        // the side-channel selBitsArray so the cross-site check still fires.
        // Without this, a stale state.icDataPtr pointing at a different
        // send site's IC slips through and gets a wrong slotIdx written.
        {
            uint64_t icSelBits = icData[18];
            if (icSelBits == 0 && callerMethod.isObject()
                    && callerMethod.rawBits() > 0x10000) {
                if (auto* jm = jitRuntime_.methodMap().lookup(
                        callerMethod.rawBits())) {
                    if (jm->numICEntries > 0) {
                        uint8_t* icStart = jm->codeStart() + jm->codeSize
                                - jm->numICEntries * jit::IC_BYTES_PER_SITE;
                        ptrdiff_t off = reinterpret_cast<uint8_t*>(icData)
                                - icStart;
                        if (off >= 0
                                && (off % jit::IC_BYTES_PER_SITE) == 0) {
                            uint32_t siteIdx = (uint32_t)(off / jit::IC_BYTES_PER_SITE);
                            if (siteIdx < jm->numICEntries) {
                                if (uint64_t* sba = jm->selBitsArray()) {
                                    icSelBits = sba[siteIdx];
                                }
                            }
                        }
                    }
                }
            }
            if (icSelBits != 0 && icSelBits > 0x10000) {
                size_t nLits = memory_.numLiteralsOf(cachedMethod);
                if (nLits >= 2) {
                    Oop cmSel = memory_.fetchPointer(nLits - 1, cachedMethod);
                    uint64_t cmSelBits = cmSel.rawBits();
                    // If the penultimate literal isn't a Symbol, it's an
                    // AdditionalMethodState — slot 1 holds the real selector.
                    if (cmSel.isObject() && cmSelBits >= 0x10000) {
                        ObjectHeader* h = cmSel.asObjectPtr();
                        if (!h->isBytesObject() && h->slotCount() >= 2) {
                            cmSelBits = h->slotAt(1).rawBits();
                        }
                    }
                    if (cmSelBits != 0 && cmSelBits != icSelBits) {
                        return;  // Selector mismatch — don't poison the IC
                    }
                }
            }
        }

        icData[firstEmpty * 3] = lookupKey;
        icData[firstEmpty * 3 + 1] = cachedMethod.rawBits();
        icData[firstEmpty * 3 + 2] = newExtra;
        if (newExtra & (1ULL << 60)) jitJ2JDirectPatches_++;
        jitICPatches_++;
    }
}

// ===== Trampoline helper: convert ExitSendCached → ExitJ2JCall =====
// Called from the ASM trampoline via BL (callee-saved regs preserved).
// Looks up cachedTarget in the MethodMap. If the target is compiled and
// has no unsafe primitive, sets returnValue = entry addr, exitReason =
// ExitJ2JCall, and returns 1. Otherwise returns 0.
// NOTE: Does NOT advance IP — the trampoline call path does that.
extern "C" int pharo_jit_convert_send(jit::JITState* state) {
    auto* mm = reinterpret_cast<jit::MethodMap*>(state->methodMapPtr);
    if (!mm) return 0;

    uint64_t targetBits = state->cachedTarget.rawBits();
    if (targetBits < 0x10000 || (targetBits & 7) != 0) return 0;

    jit::JITMethod* target = mm->lookup(targetBits);
    if (!target || !target->isExecutable()) return 0;

    // Check unsafe prim: has primitive flag set but no JIT prologue stencil
    bool hasPrim = (target->methodHeader >> 16) & 1;
    if (hasPrim && !target->hasPrimPrologue) return 0;

    // Convert to J2J call
    state->returnValue = Oop::fromRawBits(
        reinterpret_cast<uint64_t>(target->codeStart()));
    state->exitReason = jit::ExitJ2JCall;
    return 1;
}

bool Interpreter::tryJITActivation(Oop method, int argCount) {
    if (!jitRuntime_.isInitialized()) return false;
    // Match initializeJIT's gate: PHARO_NO_JIT=0 means "JIT on",
    // so noJit only blocks when the env var is set to a non-"0" value.
    // Without this, compiles happen but activations are silently disabled
    // when PHARO_NO_JIT=0 is set, which is confusing during bug-hunting.
    static const bool noJit = []() {
        if (!g_debug.noJit) return false;
        const char* v = std::getenv("PHARO_NO_JIT");
        return !(v && v[0] == '0');
    }();
    if (noJit) return false;
    jitActivations_++;

    // PHARO_BASICAT_TRACE=1: log basicAt: activations from JIT'd
    // scanFor: callers, with the receiver+index args.  Catches the
    // bench's failing basicAt: call (which goes through interpreter
    // dispatch when JIT bails on IC miss / J2J chain bail).
    {
        static bool basicAtTrace = std::getenv("PHARO_BASICAT_TRACE") != nullptr;
        static int batCount = 0;
        if (basicAtTrace && batCount < 20) {
            std::string sel = memory_.selectorOf(method);
            if (sel == "basicAt:" && argCount == 1) {
                bool callerIsScanFor = false;
                if (frameDepth_ > 0) {
                    Oop callerMethod = savedFrames_[frameDepth_ - 1].savedMethod;
                    callerIsScanFor =
                        memory_.selectorOf(callerMethod) == "scanFor:";
                }
                if (callerIsScanFor) {
                    batCount++;
                    Oop rcv = stackValue(argCount);
                    Oop idx = stackValue(0);
                    std::string rcls = rcv.isObject()
                        ? memory_.classNameOf(memory_.classOf(rcv)) : "(imm)";
                    size_t slotCnt = 0;
                    if (rcv.isObject() && rcv.rawBits() > 0x10000) {
                        slotCnt = rcv.asObjectPtr()->slotCount();
                    }
                    fprintf(stderr,
                        "[BAT #%d] basicAt: from JIT scanFor: "
                        "rcv=0x%llx (%s, slots=%zu) idx=%lld\n",
                        batCount, (unsigned long long)rcv.rawBits(),
                        rcls.c_str(), slotCnt,
                        idx.isSmallInteger() ? (long long)idx.asSmallInteger() : -999);
                }
            }
        }
    }

    // PHARO_JIT_TRACE_OOP=0xhex — log entry/exit/exitReason for a specific
    // method oop, and track call/return counts to surface imbalance.
    static uint64_t traceOop = 0;
    static bool traceInit = false;
    if (!traceInit) {
        traceInit = true;
        if (const char* env = g_debug.jitTraceOop; env) {
            traceOop = strtoull(env, nullptr, 0);
            fprintf(stderr, "[JIT-TRACE] watching oop 0x%llx\n",
                    (unsigned long long)traceOop);
        }
    }
    const bool traceThis = (traceOop != 0 && method.rawBits() == traceOop);
    static size_t traceCalls = 0, traceRets = 0, traceNoCompile = 0;
    if (traceThis) {
        traceCalls++;
        if (traceCalls <= 20 || (traceCalls & 0xFFF) == 0) {
            // Dump receiver and arg for hang-debugging.
            Oop receiverOop = stackValue(argCount);
            Oop arg0 = argCount > 0 ? stackValue(argCount - 1) : Oop::nil();
            std::string rcls = "?";
            if (receiverOop.isObject() && receiverOop.rawBits() > 0x10000) {
                rcls = memory_.classNameOf(receiverOop);
            } else if (receiverOop.isSmallInteger()) {
                rcls = "SmallInt";
            }
            // For WriteStream, slots 0=collection, 1=position, 3=writeLimit.
            // Log them so we can see if position/writeLimit advance.
            int64_t pos = -1, wl = -1;
            if (receiverOop.isObject() && receiverOop.rawBits() > 0x10000) {
                ObjectHeader* h = receiverOop.asObjectPtr();
                if (h->slotCount() >= 4) {
                    Oop p = memory_.fetchPointer(1, receiverOop);
                    Oop w = memory_.fetchPointer(3, receiverOop);
                    if (p.isSmallInteger()) pos = p.asSmallInteger();
                    if (w.isSmallInteger()) wl = w.asSmallInteger();
                }
            }
            fprintf(stderr, "[JIT-TRACE] CALL #%zu (ret=%zu nc=%zu) "
                    "rcvr=%s(0x%llx) pos=%lld wl=%lld arg0=0x%llx\n",
                    traceCalls, traceRets, traceNoCompile, rcls.c_str(),
                    (unsigned long long)receiverOop.rawBits(),
                    (long long)pos, (long long)wl,
                    (unsigned long long)arg0.rawBits());
        }
    }
    struct TraceRetGuard {
        bool active; size_t& rets; size_t& calls;
        ~TraceRetGuard() {
            if (!active) return;
            rets++;
            if (rets <= 20 || (rets & 0xFFF) == 0) {
                fprintf(stderr, "[JIT-TRACE] RETURN #%zu (call=%zu delta=%lld)\n",
                        rets, calls, (long long)(calls - rets));
            }
        }
    } traceRetGuard{traceThis, traceRets, traceCalls};

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
    if (!jm || !jm->isExecutable()) {
        if (traceThis) {
            traceNoCompile++;
            if (traceNoCompile <= 10) {
                fprintf(stderr, "[JIT-TRACE] NO-COMPILE path for traced oop (call #%zu)\n",
                        traceCalls);
            }
        }
        return false;
    }

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
    state.jitMethod = nullptr;  // Tier 2 path doesn't set this; must be null for chain loop
    state.exitReason = jit::ExitNone;
    state.icDataPtr = nullptr;
    state.sendArgCount = 0;
    state.trueOop = memory_.trueObject();
    state.falseOop = memory_.falseObject();

    // Register this state as GC-reachable for the lifetime of this call.
    // forEachRoot will update state.receiver / state.method / etc. in place
    // if GC runs while JIT code is on the C stack; prepareForGC/afterGC
    // re-derive state.ip and state.literals against the moved method.
    jit::JITState* prevJITState = currentJITState_;
    currentJITState_ = &state;
    struct JITStateGuard {
        Interpreter* self;
        jit::JITState* prev;
        ~JITStateGuard() { self->currentJITState_ = prev; }
    } jitStateGuard{this, prevJITState};

    // J2J stencil-to-stencil save stack — carved from shared j2jPool_ to avoid
    // per-call stack allocation (was 18KB at depth 256, now zero stack cost).
    int j2jPoolBase = j2jPoolCursor_;
    int j2jPoolEnd = std::min(j2jPoolBase + J2JSlotPerEntry, MaxJ2JPoolSize);
    J2JSave* j2jStack = &j2jPool_[j2jPoolBase];
    j2jPoolCursor_ = j2jPoolEnd;  // Reserve our slice; recursive entries continue after
    struct J2JPoolGuard {
        int& cursor; int base;
        ~J2JPoolGuard() { cursor = base; }
    } j2jPoolGuard{j2jPoolCursor_, j2jPoolBase};

    state.j2jSaveCursor = reinterpret_cast<uint8_t*>(&j2jPool_[j2jPoolBase]);
    state.j2jSaveLimit  = reinterpret_cast<uint8_t*>(&j2jPool_[j2jPoolEnd]);
    state.j2jDepth = 0;
    state.j2jTotalCalls = 0;
    state.methodMapPtr = &jitRuntime_.methodMap();
    state.yieldCountdown = 1000;

    // Save entry SP so we can restore it on ExitDeopt/ExitPrimFail.
    Oop* entrySP = stackPointer_;

    if (!jitRuntime_.tryExecute(method, state, jm)) {
        return false;  // Should not happen — jm was already validated
    }
    jitActivationHits_++;

    // Diagnostic: detect when JIT produces obviously wrong return values.
    // A small SmallInteger (<256) as returnValue from a non-arithmetic method
    // suggests a stencil bug (returning index/tag instead of actual value).
    if (__builtin_expect(state.exitReason == jit::ExitReturn, 0)) {
        static bool dbg = g_debug.jitRetvalDbg;
        if (dbg && state.returnValue.isSmallInteger()) {
            int64_t rv = state.returnValue.asSmallInteger();
            if (rv >= 0 && rv < 256) {
                static size_t retSmallCount = 0;
                retSmallCount++;
                if (retSmallCount <= 50 || (retSmallCount & 0xFFF) == 0) {
                    std::string sel = memory_.selectorOf(method);
                    fprintf(stderr, "[JIT-RETVAL] #%zu method=#%s retVal=%lld(0x%llx) exit=Return\n",
                            retSmallCount, sel.c_str(), (long long)rv,
                            (unsigned long long)state.returnValue.rawBits());
                }
            }
        }
    }
    // Diagnostic: when ExitSend fires, check if receiver on stack is a small int
    // where an object would be expected (selector is not arithmetic).
    if (__builtin_expect(state.exitReason == jit::ExitSend, 0)) {
        static bool dbg2 = g_debug.jitRetvalDbg;
        if (dbg2 && state.sp && state.sendArgCount >= 0 && state.sendArgCount < 20) {
            Oop rcvr = state.sp[-(state.sendArgCount + 1)];
            if (rcvr.isSmallInteger()) {
                int64_t rv = rcvr.asSmallInteger();
                if (rv >= 0 && rv < 256) {
                    static size_t sendSmallCount = 0;
                    sendSmallCount++;
                    if (sendSmallCount <= 50 || (sendSmallCount & 0xFFF) == 0) {
                        Oop sendSel = state.icDataPtr
                            ? Oop::fromRawBits(state.icDataPtr[18])
                            : state.cachedTarget;
                        std::string sel = sendSel.isObject() ? memory_.selectorOf(sendSel) : "?";
                        std::string meth = memory_.selectorOf(method);
                        fprintf(stderr, "[JIT-SEND-SMALLRCVR] #%zu send=#%s rcvr=%lld method=#%s\n",
                                sendSmallCount, sel.c_str(), (long long)rv, meth.c_str());
                    }
                }
            }
        }
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
    bool j2jMaterialized = false;  // Set when J2J bail materializes frames
    {
        // j2jStack is carved from j2jPool_ above (before tryExecute);
        // J2JSlotPerEntry limits depth per entry.
        int j2jDepth = 0;
        size_t j2jBaseFrameDepth = frameDepth_;

        // Cache Interpreter member fields into locals so the compiler keeps
        // them in registers across JIT_CALL invocations. The asm volatile
        // with "memory" clobber in JIT_CALL would otherwise force a reload
        // through `this` on every loop iteration.
        size_t localFrameDepth = frameDepth_;
        size_t localCalls = 0;
        size_t localReturns = 0;

        // No flip — codebase invariant (W^X audit 2026-04-26): thread is in X mode.

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
               state.exitReason == jit::ExitSendCached ||
               (state.exitReason == jit::ExitReturn && j2jDepth > 0)) {

            // --- ExitSendCached → ExitJ2JCall conversion ---
            if (state.exitReason == jit::ExitSendCached) {
                if (!pharo_jit_convert_send(&state)) break;
                // Fall through to J2JCall handler
            }

            if (state.exitReason == jit::ExitJ2JCall) {
                // --- J2J Call: save caller, push lazy frame, enter callee ---
                if (j2jDepth >= J2JSlotPerEntry) {
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

                // Advance IP past send bytecode. Stencils set state.ip
                // to the send bytecode; we need it past the send for both
                // save.ip (used on null-resume bailout) and resume address.
                {
                    uint8_t sendOp = *state.ip;
                    if (sendOp >= 0xEA && sendOp <= 0xEB) state.ip += 2;
                    else state.ip += 1;
                }

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
                // literals/argCount/bcStart are derived from save.jitMethod
                // on return (see J2J_INLINE_RETURN).
                (void)selfRecursive;
                save.jitMethod = callerJM;
                // Precompute resume JIT code address from the advanced IP.
                // bcOffset = (advancedIP - callerBCStart) gives the next
                // bytecode, so bcToCode maps to the stencil AFTER the send.
                {
                    uint32_t bcOffset = static_cast<uint32_t>(state.ip - callerBCStart);
                    // Safety: refuse register-reading entry offsets —
                    // see JITRuntime::tryResume / deferred.md A1.
                    if (jitRuntime_.getBcEntryState(callerJM, bcOffset) != 0) {
                        save.resumeAddr = nullptr;
                    } else {
                        uint32_t codeOffset = callerJM->codeOffsetForBC(bcOffset);
                        save.resumeAddr = (codeOffset == 0 || codeOffset >= callerJM->codeSize)
                            ? nullptr
                            : callerJM->codeStart() + codeOffset;
                    }
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

                // Bug-14 diagnostic: log every trampoline J2J-call setup with
                // callee method + its receiver + args.  Rate-limited.
                if (g_debug.b5Trace) {
                    static size_t cTcall = 0;
                    cTcall++;
                    if (cTcall <= 3000) {
                        std::string ccls = classNameOfMethod(targetMethod);
                        std::string csel = memory_.selectorOf(targetMethod);
                        std::string rkind = calleeRecv.isSmallInteger() ? "SmI"
                            : calleeRecv.isObject()
                              ? memory_.classNameOf(calleeRecv).c_str()
                              : "other";
                        fprintf(stderr, "[B5-TRAMP-CALL] #%zu nArgs=%d "
                                        "calleeRecv=0x%llx(%s) "
                                        "callee=0x%llx cls=%s sel=#%s\n",
                                cTcall, nArgs,
                                (unsigned long long)calleeRecv.rawBits(),
                                rkind.c_str(),
                                (unsigned long long)targetMethod.rawBits(),
                                ccls.c_str(), csel.c_str());
                    }
                }

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
                // Derive literals/argCount/ip from save.jitMethod — see
                // docs/jit-j2j-reduction-plan.md.  The old self-recursive
                // marker bit is gone.
                {
                    auto* savedJM = save.jitMethod;
                    state.jitMethod = savedJM;
                    state.literals = reinterpret_cast<Oop*>(savedJM->literals());
                    state.argCount = savedJM->argCount;
                    state.ip = savedJM->bcStart();
                }

                // Pop receiver+args, push return value
                // Stack layout: sp[-(nArgs+1)]=receiver, sp[-nArgs]=arg1, ..., sp[-1]=TOS
                // sp points to next free slot. Replace receiver with retVal, adjust down.
                state.sp[-(save.sendArgCount + 1)] = retVal;
                state.sp -= save.sendArgCount;

                // Bug-14 diagnostic: log every trampoline J2J-return —
                // method being returned TO (caller), nArgs popped, retVal,
                // restored sp, whether resume is valid.
                if (g_debug.b5Trace) {
                    static size_t cTramp = 0;
                    cTramp++;
                    if (cTramp <= 2500) {
                        auto* savedJM = save.jitMethod;
                        uint64_t callerOop = savedJM
                            ? savedJM->compiledMethodOop : 0;
                        Oop callerOop_ = Oop::fromRawBits(callerOop);
                        std::string cls = callerOop
                            ? classNameOfMethod(callerOop_) : "?";
                        std::string sel = callerOop
                            ? memory_.selectorOf(callerOop_) : "?";
                        fprintf(stderr, "[B5-TRAMP-RET] #%zu sp=%p retVal=0x%llx "
                                        "nArgsPopped=%d resumeAddr=%p "
                                        "localFrameDepth=%zu "
                                        "callerCM=0x%llx cls=%s sel=#%s\n",
                                cTramp, state.sp,
                                (unsigned long long)retVal.rawBits(),
                                (int)save.sendArgCount, save.resumeAddr,
                                frameDepth_,
                                (unsigned long long)callerOop,
                                cls.c_str(), sel.c_str());
                    }
                }

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
        // Stay in X — W^X audit 2026-04-26.

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
            state.method = Oop::fromRawBits(
                state.jitMethod->compiledMethodOop);
        }

        // If we bailed out with pending J2J frames, materialize them
        // so the interpreter can see them.  literals/argCount/bcStart
        // are derived from save.jitMethod; the old self-recursive
        // marker bit is gone.
        if (j2jDepth > 0) {
            j2jMaterialized = true;
            Oop nil = memory_.nil();
            for (int i = 0; i < j2jDepth; i++) {
                J2JSave& save = j2jStack[i];
                jit::JITMethod* saveJM = save.jitMethod;
                if (!saveJM) {
                    static int warns = 0;
                    if (++warns <= 5)
                        fprintf(stderr, "[JIT] WARN: null save.jitMethod at j2jBase materialize (warn #%d)\n", warns);
                    j2jMaterialized = false;
                    break;
                }
                SavedFrame& frame = savedFrames_[j2jBaseFrameDepth + i];
                Oop saveMethod = Oop::fromRawBits(saveJM->compiledMethodOop);
                frame.savedIP = save.ip;
                frame.savedBytecodeEnd =
                    saveJM->bcStart() + saveJM->numBytecodes;
                frame.savedMethod = saveMethod;
                frame.savedHomeMethod = saveMethod;
                frame.savedReceiver = save.receiver;
                frame.savedClosure = nil;
                frame.savedActiveContext = nil;
                frame.materializedContext = nil;
                frame.savedFP = save.tempBase - 1;
                frame.savedArgCount = saveJM->argCount;
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
            // Set bytecodeEnd_ from the innermost frame's method.
            // Without this, fetchByte() sees stale bytecodeEnd_ from the
            // method that was active BEFORE the J2J chain, hits the
            // ip >= bytecodeEnd_ guard, and returns 0x5C (fake returnTop).
            //
            // Defensive: state.method can be nil if the chain loop's saved
            // jitMethod was evicted+reallocated between save and
            // materialize (compiledMethodOop field bytes no longer identify
            // a live CompiledMethod).  Pin-on-evict (see JITCompiler.cpp)
            // fixes the common case; this guard catches any residual and
            // falls back to the outer method's bytecode range.
            if (state.method.isObject() && state.method.rawBits() != 0) {
                ObjectHeader* mObj = state.method.asObjectPtr();
                if (mObj) bytecodeEnd_ = mObj->bytes() + mObj->byteSize();
            } else {
                static int warnCount = 0;
                if (++warnCount <= 5) {
                    fprintf(stderr, "[JIT] WARN: materialized J2J inner method is nil (warn #%d)\n", warnCount);
                }
                // Leave method_/ip_/bytecodeEnd_ as they were — the outer
                // activation will handle the return path, not this one.
                j2jMaterialized = false;
            }
        }
    }

    // When J2J materialization set up interpreter state for the innermost
    // J2J frame, the chain loop must not run: any bail path (return false)
    // would cause activateMethod to fall through with method_/ip_ pointing
    // to the J2J callee instead of the original method — corrupting state.
    // Instead, let the interpreter dispatch loop pick up from the
    // materialized state (method_, ip_, framePointer_ are all valid).
    if (j2jMaterialized) {
        // state.ip / state.sp were already synced to interpreter by materialization.
        // Handle the exit reason that caused the J2J trampoline to bail.
        switch (state.exitReason) {
        case jit::ExitReturn:
            // Bug-14 diagnostic (materialized J2J exit)
            if (g_debug.b5Trace) {
                static size_t c14088 = 0;
                c14088++;
                if (c14088 <= 2500) {
                    std::string mcls = classNameOfMethod(state.method);
                    std::string msel = memory_.selectorOf(state.method);
                    fprintf(stderr, "[B5-EXIT-materialized] #%zu sp=%p retVal=0x%llx "
                                    "localFrameDepth=%zu method=0x%llx cls=%s sel=#%s\n",
                            c14088, state.sp,
                            (unsigned long long)state.returnValue.rawBits(),
                            frameDepth_,
                            (unsigned long long)state.method.rawBits(),
                            mcls.c_str(), msel.c_str());
                }
            }
            // Innermost J2J frame returned — pop one materialized frame
            if (!popFrame()) {
                if (benchMode_) { handleBenchComplete(state.returnValue); return true; }
                terminateCurrentProcess();
                tryReschedule();
                return true;
            }
            push(state.returnValue);
            return true;
        default:
            // ExitSend, ExitSendCached, ExitBlockCreate, etc. —
            // interpreter state points to the bytecode that caused the exit.
            // Return true so activateMethod doesn't fall through; the
            // interpreter dispatch loop re-executes this bytecode normally.
            return true;
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
    static bool noChain = g_debug.noChain;
    int maxChain = noChain ? 1 : 10000;
    int chainCallDepth = 0;  // Inline activation depth (frames pushed by chain loop itself)

    // Helper: set J2J state for a chain loop resume.
    auto enableJ2J = [&]() {
        state.j2jSaveCursor = reinterpret_cast<uint8_t*>(j2jStack);
        state.j2jSaveLimit  = reinterpret_cast<uint8_t*>(&j2jPool_[j2jPoolEnd]);
        state.j2jDepth = 0;
        state.j2jTotalCalls = 0;
        state.yieldCountdown = 1000;
    };

    // Helper: materialize pending J2J frames from stencil execution.
    // After a JIT_CALL/tryResume/tryExecute with J2J enabled, stencils
    // may have accumulated J2J save frames (j2jDepth > 0).  Materialize
    // them as SavedFrames so the interpreter can see them.
    auto materializeJ2J = [&]() {
        if (state.j2jDepth == 0) return;
        jitMaterializeCount_++;
        jitMaterializeTotalDepth_ += state.j2jDepth;
        jitJ2JStencilCalls_ += state.j2jTotalCalls;
        checkCountdown_ -= state.j2jTotalCalls * 10;
        Oop nil = memory_.nil();
        for (int i = 0; i < state.j2jDepth; i++) {
            if (frameDepth_ >= StackOverflowLimit) break;
            J2JSave& save = j2jStack[i];
            jit::JITMethod* saveJM = save.jitMethod;
            if (!saveJM) {
                // Half-materialized state — frameDepth_ has been
                // incremented for prior iterations, but we can't trust
                // any of them now (they may also have been corrupted).
                // The post-loop code reads state.method->bytes() which
                // SIGSEGVs if state.method is nil.  Roll back the
                // pushed frames and bail entirely.
                static int warns = 0;
                if (++warns <= 5)
                    fprintf(stderr, "[JIT] WARN: null save.jitMethod in materializeJ2J lambda (warn #%d) — bailing\n", warns);
                for (int j = 0; j < i; j++) {
                    if (frameDepth_ > 0) frameDepth_--;
                }
                return;
            }
            SavedFrame& frame = savedFrames_[frameDepth_++];
            Oop saveMethod = Oop::fromRawBits(saveJM->compiledMethodOop);
            frame.savedIP = save.ip;
            frame.savedBytecodeEnd =
                saveJM->bcStart() + saveJM->numBytecodes;
            frame.savedMethod = saveMethod;
            frame.savedHomeMethod = saveMethod;
            frame.savedReceiver = save.receiver;
            frame.savedClosure = nil;
            frame.savedActiveContext = nil;
            frame.materializedContext = nil;
            frame.savedFP = save.tempBase - 1;
            frame.savedArgCount = saveJM->argCount;
            frame.homeFrameDepth = SIZE_MAX;
        }
        chainCallDepth += state.j2jDepth;
        if (state.jitMethod) {
            state.method = Oop::fromRawBits(
                state.jitMethod->compiledMethodOop);
        }
        method_ = state.method;
        method = state.method;
        homeMethod_ = state.method;
        receiver_ = state.receiver;
        stackPointer_ = state.sp;
        instructionPointer_ = state.ip;
        framePointer_ = state.tempBase - 1;
        argCount_ = state.argCount;
        {
            ObjectHeader* mObj = state.method.asObjectPtr();
            if (mObj) bytecodeEnd_ = mObj->bytes() + mObj->byteSize();
        }
        methObj = method.asObjectPtr();
    };

    // Adaptive J2J depth: per-method depth limit that promotes on clean
    // stencil re-entries and demotes on bail (materialization).  Towers
    // bails frequently → stays at depth 2.  List runs clean → promotes
    // up to depth 8.  Avoids the 5.8x Towers regression that a global
    // depth >= 3 caused while still unlocking deeper J2J for other code.
    auto adaptJ2JDepth = [&](Oop meth, bool bailed) {
        jit::JITMethod* jm = jitRuntime_.methodMap().lookup(meth.rawBits());
        if (!jm || !jm->stats) return;
        // Writes go to the heap-allocated stats struct (W^X audit
        // 2026-04-26) — no MAP_JIT writes, no flips needed.  This is
        // the hot path: ~4.5M calls per bench.
        jit::JITMethodStats* s = jm->stats;
        if (bailed) {
            s->j2jDepthLimit = 2;
            s->j2jCleanRuns = 0;
        } else {
            if (s->j2jCleanRuns < 255) s->j2jCleanRuns++;
            if (s->j2jCleanRuns >= 8 && s->j2jDepthLimit < 8) {
                s->j2jDepthLimit++;
                s->j2jCleanRuns = 0;
            }
        }
    };

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
            // Bug-14 diagnostic: PHARO_B5_TRACE=1 logs where JIT landed on
            // every ExitReturn — sp/retVal/chainCallDepth/localFrameDepth
            // plus caller-method identity so we can pinpoint the transition
            // from reset's bail → interpreter resume.  Rate-limited.
            if (g_debug.b5Trace) {
                static size_t count = 0;
                count++;
                if (count <= 2500) {
                    std::string mcls = classNameOfMethod(state.method);
                    std::string msel = memory_.selectorOf(state.method);
                    // Compute bc offset of state.ip relative to method start,
                    // so we can tell which bytecode the exit came from.
                    long long bcOff = -1;
                    if (state.method.isObject() && state.ip) {
                        ObjectHeader* mo = state.method.asObjectPtr();
                        Oop hdr = mo->slots()[0];
                        int nLit = hdr.isSmallInteger()
                            ? (hdr.asSmallInteger() & 0x7FFF) : 0;
                        const uint8_t* bcBase = mo->bytes() + (1 + nLit) * 8;
                        bcOff = (long long)(state.ip - bcBase);
                    }
                    // Also log state.receiver — at a returnReceiver bail,
                    // retVal should equal state.receiver.  If state.receiver
                    // itself is a SmallInt, we know the receiver was
                    // corrupted (not the stack-slot math).
                    std::string rcvrKind = state.receiver.isSmallInteger() ? "SmI"
                        : state.receiver.isObject()
                          ? memory_.classNameOf(state.receiver).c_str()
                          : "other";
                    fprintf(stderr, "[B5-EXIT] #%zu ExitReturn sp=%p ip=%p bcOff=%lld "
                                    "j2jDepth=%d retVal=0x%llx "
                                    "receiver=0x%llx(%s) "
                                    "chainCallDepth=%d localFrameDepth=%zu "
                                    "method=0x%llx cls=%s sel=#%s\n",
                            count, state.sp, state.ip, bcOff,
                            state.j2jDepth,
                            (unsigned long long)state.returnValue.rawBits(),
                            (unsigned long long)state.receiver.rawBits(),
                            rcvrKind.c_str(),
                            (int)chainCallDepth, frameDepth_,
                            (unsigned long long)state.method.rawBits(),
                            mcls.c_str(), msel.c_str());
                }
            }
            if (chainCallDepth > 0) {
                // Inline callee returned — pop frame, resume caller in JIT
                popFrame();
                push(state.returnValue);
                chainCallDepth--;

                // Restore locals from interpreter state (set by popFrame)
                method = method_;
                argCount = argCount_;
                methObj = method.asObjectPtr();
                entrySP = stackPointer_;

                // Rebuild JITState for caller and resume
                state.sp = stackPointer_;
                state.receiver = receiver_;
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

                uint32_t bcOffset = computeCurrentBCOffset();
                if (bcOffset == UINT32_MAX) return true;

                // Inline tryResume — skip redundant lookup/validation
                {
                    jit::JITMethod* callerJM = jitRuntime_.methodMap().lookup(
                        method.rawBits());
                    if (!callerJM || !callerJM->isExecutable()) return true;
                    // Safety: refuse register-reading (_N) entry offsets —
                    // see JITRuntime::tryResume / deferred.md A1.
                    if (jitRuntime_.getBcEntryState(callerJM, bcOffset) != 0) return true;
                    uint32_t codeOff = callerJM->codeOffsetForBC(bcOffset);
                    if (codeOff == 0 || codeOff >= callerJM->codeSize) return true;
                    state.jitMethod = callerJM;
                    state.exitReason = jit::ExitNone;
                    state.j2jDepth = 0;
                    state.j2jTotalCalls = 0;
                    state.j2jSaveCursor = reinterpret_cast<uint8_t*>(&j2jPool_[j2jPoolBase]);
                    state.j2jSaveLimit  = reinterpret_cast<uint8_t*>(&j2jPool_[j2jPoolEnd]);
                    state.yieldCountdown = 1000;
                    // No flips — W^X audit 2026-04-26.
                    JIT_CALL(callerJM->codeStart() + codeOff, &state);
                }
                jitJ2JStencilCalls_ += state.j2jTotalCalls;
                chargeJITBytecodes(state);
                // Materialize J2J frames from the resumed caller
                if (state.j2jDepth > 0) {
                    Oop nil = memory_.nil();
                    for (int i = 0; i < state.j2jDepth; i++) {
                        if (frameDepth_ >= StackOverflowLimit) break;
                        J2JSave& save = j2jStack[i];
                        jit::JITMethod* saveJM = save.jitMethod;
                        if (!saveJM) {
                            static int warns = 0;
                            if (++warns <= 5)
                                fprintf(stderr, "[JIT] WARN: null save.jitMethod at site4 (warn #%d)\n", warns);
                            break;
                        }
                        SavedFrame& frame = savedFrames_[frameDepth_++];
                        Oop saveMethod = Oop::fromRawBits(saveJM->compiledMethodOop);
                        frame.savedIP = save.ip;
                        frame.savedBytecodeEnd =
                            saveJM->bcStart() + saveJM->numBytecodes;
                        frame.savedMethod = saveMethod;
                        frame.savedHomeMethod = saveMethod;
                        frame.savedReceiver = save.receiver;
                        frame.savedClosure = nil;
                        frame.savedActiveContext = nil;
                        frame.materializedContext = nil;
                        frame.savedFP = save.tempBase - 1;
                        frame.savedArgCount = saveJM->argCount;
                        frame.homeFrameDepth = SIZE_MAX;
                    }
                    chainCallDepth += state.j2jDepth;
                    if (state.jitMethod) {
                        state.method = Oop::fromRawBits(
                            state.jitMethod->compiledMethodOop);
                    }
                    method_ = state.method;
                    method = state.method;
                    homeMethod_ = state.method;
                    receiver_ = state.receiver;
                    stackPointer_ = state.sp;
                    instructionPointer_ = state.ip;
                    framePointer_ = state.tempBase - 1;
                    argCount_ = state.argCount;
                    {
                        ObjectHeader* mObj = state.method.asObjectPtr();
                        if (mObj) bytecodeEnd_ = mObj->bytes() + mObj->byteSize();
                    }
                }
                if (checkCountdown_ <= 0) goto jit_loop_exit;
                continue;
            }

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
                    handleBenchComplete(state.returnValue);
                    return true;
                }
                terminateCurrentProcess();
                tryReschedule();
                return true;
            }
            lastJitReturn_.methodBits = state.method.rawBits();
            lastJitReturn_.returnBits = state.returnValue.rawBits();
            lastJitReturn_.frameDepth = frameDepth_;
            lastJitReturn_.wasResume = false;
            push(state.returnValue);
            // Bug-14 diagnostic: print the interpreter state we just set
            // up for the caller (post-popFrame, post-push).  Compare to
            // the pre-popFrame state.sp for reset's exit to pinpoint the
            // bogus-sp case.
            if (g_debug.b5Trace) {
                static size_t count2 = 0;
                count2++;
                if (count2 <= 2500) {
                    std::string mcls = memory_.classNameOf(receiver_);
                    std::string msel = memory_.selectorOf(method_);
                    long long bcOff = -1;
                    if (method_.isObject() && instructionPointer_) {
                        ObjectHeader* mo = method_.asObjectPtr();
                        Oop hdr = mo->slots()[0];
                        int nLit = hdr.isSmallInteger()
                            ? (hdr.asSmallInteger() & 0x7FFF) : 0;
                        const uint8_t* bcBase = mo->bytes() + (1 + nLit) * 8;
                        bcOff = (long long)(instructionPointer_ - bcBase);
                    }
                    long long spFromFP = framePointer_
                        ? (long long)(stackPointer_ - framePointer_) : -1;
                    fprintf(stderr, "[B5-RESUME] #%zu post-pop stackPointer_=%p "
                                    "framePointer_=%p sp-fp=%lld "
                                    "method_=0x%llx rcvrCls=%s sel=#%s "
                                    "ip=%p bcOff=%lld (next bc=0x%02x)\n",
                            count2, (void*)stackPointer_, (void*)framePointer_,
                            spFromFP,
                            (unsigned long long)method_.rawBits(),
                            mcls.c_str(), msel.c_str(),
                            (void*)instructionPointer_, bcOff,
                            instructionPointer_ ? *instructionPointer_ : 0);
                }
            }
            return true;
        }

        case jit::ExitSend: {
            // IC miss: do method lookup, patch IC, and chain into callee
            // instead of bailing to interpreter (which loses JIT continuity).
            instructionPointer_ = state.ip;
            stackPointer_ = state.sp;
            jitICMisses_++;

            // DIAGNOSTIC: distinguish "IC had room" vs "IC full" vs
            // "icData[18] was 0 (megacache skip)" to pin down why IC
            // hit rate is only 50%.
            if (state.icDataPtr) {
                bool anyFilled = false;
                for (int e = 0; e < 6; e++) if (state.icDataPtr[e*3]) { anyFilled = true; break; }
                uint64_t selBits18 = state.icDataPtr[18];
                if (selBits18 == 0) {
                    jitICMissNoSelBits_++;
                } else if (!anyFilled) {
                    jitICMissCold_++;
                } else {
                    jitICMissPolymorphic_++;
                }
            } else {
                jitICMissNoICData_++;
            }

            // Get selector: from IC data (Tier 1) or cachedTarget (Tier 2).
            // Task #41: if icDataPtr[18] is zero (post-GC memset), fall back
            // to the JIT method's side-channel selBits array which survives
            // GC.  Warm-IC cases pay no overhead — they read slot 18 as
            // before.  The side-array is only consulted when the primary
            // slot is 0, which happens right after GC until the IC rewarms.
            Oop sendSel;
            if (state.icDataPtr) {
                uint64_t selBits = state.icDataPtr[18];
                if (selBits == 0) {
                    if (auto* jm = jitRuntime_.methodMap().lookup(state.method.rawBits())) {
                        if (jm->numICEntries > 0) {
                            uint8_t* icStart = jm->codeStart() + jm->codeSize
                                             - jm->numICEntries * jit::IC_BYTES_PER_SITE;
                            ptrdiff_t offset = reinterpret_cast<uint8_t*>(state.icDataPtr) - icStart;
                            if (offset >= 0 && (offset % jit::IC_BYTES_PER_SITE) == 0) {
                                uint32_t siteIdx = static_cast<uint32_t>(offset / jit::IC_BYTES_PER_SITE);
                                if (siteIdx < jm->numICEntries) {
                                    if (uint64_t* sba = jm->selBitsArray()) {
                                        selBits = sba[siteIdx];
                                    }
                                }
                            }
                        }
                    }
                }
                sendSel = Oop::fromRawBits(selBits);
            } else {
                // Inline stencil bail or Tier 2 (MIR) exits — selector in cachedTarget.
                sendSel = state.cachedTarget;
            }
            if (!sendSel.isObject() || sendSel.rawBits() < 0x10000) {
                return false;
            }

            int nArgs = state.sendArgCount;
            Oop rcvr = stackValue(nArgs);

            // Check if this is a super send (0xEB at ip)
            bool isSuperSend = (state.ip && *state.ip == 0xEB);

            // Method lookup — global method cache first, then full lookup
            Oop resolved;
            if (isSuperSend) {
                // Super send: lookup from superclass of method's defining class
                Oop methodClass = methodClassOf(state.method);
                Oop superclass;
                if (methodClass.isNil() || !methodClass.isObject()) {
                    superclass = superclassOf(memory_.classOf(rcvr));
                } else {
                    superclass = superclassOf(methodClass);
                }
                resolved = lookupMethod(sendSel, superclass);
                if (resolved.isNil()) return false;  // DNU — interpreter handles
            } else {
                Oop rcvrClass = memory_.classOf(rcvr);
                MethodCacheEntry* ce = probeCache(sendSel, rcvrClass);
                if (ce) {
                    resolved = ce->method;
                } else {
                    resolved = lookupMethod(sendSel, rcvrClass);
                    if (resolved.isNil()) return false;  // DNU — interpreter handles
                    cacheMethod(sendSel, rcvrClass, resolved);
                }
            }

            if (!resolved.isObject() || resolved.rawBits() < 0x10000 ||
                resolved.asObjectPtr()->classIndex() != compiledMethodClassIndex_) {
                return false;  // Non-standard method — interpreter handles
            }

            if (!isSuperSend) {
                // Patch IC immediately so next hit is ExitSendCached
                // (super sends don't use ICs — lookup always starts from superclass)
                pendingICPatch_ = state.icDataPtr;
                pendingICSendArgCount_ = nArgs;
                pendingICOwnerMethod_ = state.method;
                patchJITICAfterSend(resolved, rcvr, sendSel);

                // Populate mega cache for JIT stencil probes
                {
                    uint64_t tag = rcvr.rawBits() & 0x7;
                    uint64_t megaKey = (tag == 0 && rcvr.rawBits() >= 0x10000)
                        ? static_cast<uint64_t>(rcvr.asObjectPtr()->classIndex())
                        : (tag != 0 ? (tag | 0x80000000ULL) : 0);
                    if (megaKey != 0) {
                        uint64_t jitAddr = 0;
                        auto* jm = jitRuntime_.methodMap().lookup(resolved.rawBits());
                        if (jm) {
                            bool hasPrim = (jm->methodHeader >> 16) & 1;
                            if (!hasPrim || jm->hasPrimPrologue)
                                jitAddr = reinterpret_cast<uint64_t>(jm->codeStart());
                        }
                        jitRuntime_.megaCacheAdd(sendSel.rawBits(), megaKey,
                                                resolved.rawBits(), jitAddr);
                    }
                }
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
                    for (int e = 0; e < 6; e++) {
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
            countICHitDbg(state.icDataPtr);
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
            enableJ2J();

            if (!jitRuntime_.tryResume(method, bcOffset, state)) {
                return true;  // Can't resume; interpreter handles rest
            }
            chargeJITBytecodes(state);
            materializeJ2J();
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
            enableJ2J();

            if (!jitRuntime_.tryResume(method, bcOffset, state)) {
                return true;  // Can't resume; interpreter handles rest
            }
            chargeJITBytecodes(state);
            materializeJ2J();
            if (checkCountdown_ <= 0) goto jit_loop_exit;
            continue;  // Loop to handle the new exit reason
        }

        case jit::ExitArithOverflow: {
            {
                const bool dbgOn = g_debug.debugArithExit;
                if (__builtin_expect(dbgOn, 0) && framePointer_) {
                    long long spFromFP = (long long)(state.sp - framePointer_);
                    if (spFromFP >= 400 && spFromFP <= 700) {
                        ObjectHeader* mObj = method_.isObject() ? method_.asObjectPtr() : nullptr;
                        const uint8_t* bcBase = nullptr;
                        if (mObj) {
                            Oop hdr = mObj->slots()[0];
                            int numLits = hdr.isSmallInteger() ? (hdr.asSmallInteger() & 0x7FFF) : 0;
                            bcBase = mObj->bytes() + (1 + numLits) * 8;
                        }
                        long long bcOff = (bcBase && state.ip) ? (long long)(state.ip - bcBase) : -1;
                        fprintf(stderr, "[ARITH-EXIT] fd=%zu sp-fp=%lld state.ip bcOff=%lld bc=%02x method=#%s\n",
                                frameDepth_, spFromFP, bcOff,
                                (state.ip ? *state.ip : 0),
                                memory_.selectorOf(method_).c_str());
                    }
                }
            }
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
            countICHitDbg(state.icDataPtr);
            instructionPointer_ = state.ip;
            stackPointer_ = state.sp;
            chainTarget = j2jCached;
            jitRuntime_.noteMethodEntry(j2jCached);
            ipAlreadyAdvanced = true;
            break;  // → shared send-chain code after switch
        }

        case jit::ExitYield: {
            // Backward-jump yield: JIT decremented yieldCountdown to 0.
            // Charge checkCountdown_ for the elapsed JIT bytecodes, reset
            // yieldCountdown, and resume JIT — or bail if scheduler needs to run.
            jitYieldCount_++;
            instructionPointer_ = state.ip;
            stackPointer_ = state.sp;
            {
                // Each yield = ~1000 backward jumps. Estimate bytecodes executed.
                int numBC = state.jitMethod ? state.jitMethod->numBytecodes : 20;
                int charge = 1000 * numBC;
                checkCountdown_ -= charge;
                g_stepNum += charge;
            }
            if (checkCountdown_ <= 0) goto jit_loop_exit;

            // Reset yield counter and resume with J2J enabled. This lets
            // stencils handle sends directly via stencil-to-stencil calls
            // instead of exiting to C++ on every send.
            state.yieldCountdown = 1000;
            state.exitReason = jit::ExitNone;
            state.j2jDepth = 0;
            state.j2jTotalCalls = 0;
            state.j2jSaveCursor = reinterpret_cast<uint8_t*>(&j2jPool_[j2jPoolBase]);
            state.j2jSaveLimit  = reinterpret_cast<uint8_t*>(&j2jPool_[j2jPoolEnd]);
            {
                uint32_t bcOffset = computeCurrentBCOffset();
                if (bcOffset == UINT32_MAX) return true;
                jit::JITMethod* jm = jitRuntime_.methodMap().lookup(
                    method.rawBits());
                if (!jm || !jm->isExecutable()) return true;
                // Safety: refuse register-reading entry offsets —
                // see JITRuntime::tryResume / deferred.md A1.
                if (jitRuntime_.getBcEntryState(jm, bcOffset) != 0) return true;
                uint32_t codeOff = jm->codeOffsetForBC(bcOffset);
                if (codeOff == 0 || codeOff >= jm->codeSize) return true;
                state.jitMethod = jm;
                // No flips — W^X audit 2026-04-26.
                JIT_CALL(jm->codeStart() + codeOff, &state);
            }
            // Charge stencil J2J calls from this segment
            jitJ2JStencilCalls_ += state.j2jTotalCalls;
            checkCountdown_ -= state.j2jTotalCalls * 10;

            // If stencil J2J calls left pending frames, materialize them
            // so the interpreter/chain loop can see them.
            if (state.j2jDepth > 0) {
                Oop nil = memory_.nil();
                size_t baseDepth = frameDepth_;
                for (int i = 0; i < state.j2jDepth; i++) {
                    J2JSave& save = j2jStack[i];
                    jit::JITMethod* saveJM = save.jitMethod;
                    if (!saveJM) {
                        static int warns = 0;
                        if (++warns <= 5)
                            fprintf(stderr, "[JIT] WARN: null save.jitMethod at site5 (warn #%d)\n", warns);
                        break;
                    }
                    SavedFrame& frame = savedFrames_[baseDepth + i];
                    Oop saveMethod = Oop::fromRawBits(saveJM->compiledMethodOop);
                    frame.savedIP = save.ip;
                    frame.savedBytecodeEnd =
                        saveJM->bcStart() + saveJM->numBytecodes;
                    frame.savedMethod = saveMethod;
                    frame.savedHomeMethod = saveMethod;
                    frame.savedReceiver = save.receiver;
                    frame.savedClosure = nil;
                    frame.savedActiveContext = nil;
                    frame.materializedContext = nil;
                    frame.savedFP = save.tempBase - 1;
                    frame.savedArgCount = saveJM->argCount;
                    frame.homeFrameDepth = SIZE_MAX;
                }
                frameDepth_ = baseDepth + state.j2jDepth;
                chainCallDepth += state.j2jDepth;
                // Sync interpreter from innermost frame
                if (state.jitMethod) {
                    state.method = Oop::fromRawBits(
                        state.jitMethod->compiledMethodOop);
                }
                method_ = state.method;
                method = state.method;
                homeMethod_ = state.method;
                receiver_ = state.receiver;
                stackPointer_ = state.sp;
                instructionPointer_ = state.ip;
                framePointer_ = state.tempBase - 1;
                argCount_ = state.argCount;
                {
                    ObjectHeader* mObj = state.method.asObjectPtr();
                    if (mObj) bytecodeEnd_ = mObj->bytes() + mObj->byteSize();
                }
            }

            chargeJITBytecodes(state);
            if (checkCountdown_ <= 0) goto jit_loop_exit;
            continue;
        }

        case jit::ExitPrimFail:
        case jit::ExitDeopt:
            // Unwind any inline chain frames
            while (chainCallDepth > 0) {
                popFrame();
                chainCallDepth--;
            }
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

            // ===== Inline one-shot J2J call =====
            // If the target is JIT-compiled and safe, call it directly from
            // the chain loop using the existing JITState. Avoids activateMethod
            // overhead (pushFrame, full JITState setup per recursive
            // tryJITActivation).
            //
            // Fast path (~93%): callee returns with ExitReturn — restore
            // caller state inline and resume JIT.
            //
            // Fallback (~7%): callee exits with non-ExitReturn — push a
            // SavedFrame for the caller and bail to interpreter.
#if PHARO_JIT_ENABLED
            {
                jit::JITMethod* chainJM =
                    jitRuntime_.methodMap().lookup(chainTarget.rawBits());
                if (chainJM && chainJM->isExecutable()) {
                    bool chainHasPrim = (chainJM->methodHeader >> 16) & 1;
                    if (!chainHasPrim || chainJM->hasPrimPrologue) {
                        // --- Save caller state + precompute resume ---
                        Oop* savedSP = state.sp;
                        Oop savedRecv = state.receiver;
                        Oop* savedTempBase = state.tempBase;
                        Oop* savedLiterals = state.literals;
                        jit::JITMethod* savedJitMethod =
                            reinterpret_cast<jit::JITMethod*>(state.jitMethod);
                        int savedArgCount = state.argCount;
                        Oop savedMethod = state.method;
                        // instructionPointer_ is already past-send (caller resume point)

                        // Precompute caller's resume address to avoid expensive
                        // tryResume on the fast path (~93% of calls).
                        uint8_t* savedBcStart = nullptr;
                        uint8_t* savedResumeEntry = nullptr;
                        // Guard: savedJitMethod->compiledMethodOop can be 0
                        // when the JITMethod slot's underlying memory is
                        // zero-initialized or holds FreeBlock metadata
                        // (e.g., after eviction reuse).  Bug 11b layer 3
                        // crashed here as `savedMethod.asObjectPtr() == nullptr`
                        // → SEGV reading slot 0.
                        if (savedJitMethod
                            && savedMethod.isObject()
                            && savedMethod.rawBits() != 0) {
                            ObjectHeader* sMO = savedMethod.asObjectPtr();
                            Oop sHdr = sMO->slots()[0];
                            int sNL = sHdr.isSmallInteger() ? (sHdr.asSmallInteger() & 0x7FFF) : 0;
                            savedBcStart = sMO->bytes() + (1 + sNL) * 8;
                            uint32_t pastSendOff = static_cast<uint32_t>(
                                instructionPointer_ - savedBcStart);
                            // Safety: never precompute a resume into a
                            // register-reading (_N) stencil offset —
                            // see JITRuntime::tryResume / deferred.md A1.
                            if (jitRuntime_.getBcEntryState(savedJitMethod, pastSendOff) == 0) {
                                uint32_t codeOff = savedJitMethod->codeOffsetForBC(pastSendOff);
                                if (codeOff > 0 && codeOff < savedJitMethod->codeSize)
                                    savedResumeEntry = savedJitMethod->codeStart() + codeOff;
                            }
                        }

                        // --- Set up callee in JITState ---
                        Oop calleeRecv = stackPointer_[-(nArgs + 1)];
                        ObjectHeader* calleeMethObj = chainTarget.asObjectPtr();
                        Oop* fp = stackPointer_ - (nArgs + 1);

                        state.sp = stackPointer_;
                        state.receiver = calleeRecv;
                        state.literals = calleeMethObj->slots() + 1;
                        state.tempBase = fp + 1;
                        state.argCount = nArgs;
                        state.jitMethod = chainJM;
                        state.method = chainTarget;

                        int numLits = static_cast<int>(chainJM->methodHeader & 0x7FFF);
                        state.ip = calleeMethObj->bytes() + (1 + numLits) * 8;

                        // Allocate temps if needed
                        int totalTemps = chainJM->tempCount;
                        if (__builtin_expect(nArgs < totalTemps, 0)) {
                            Oop nil = memory_.nil();
                            for (int i = nArgs; i < totalTemps; i++) {
                                *state.sp = nil;
                                state.sp++;
                            }
                        }

                        // Enable stencil J2J for callee — lets it chain
                        // sends directly instead of falling back to C++.
                        state.j2jSaveCursor = reinterpret_cast<uint8_t*>(&j2jPool_[j2jPoolBase]);
                        state.j2jSaveLimit  = reinterpret_cast<uint8_t*>(&j2jPool_[j2jPoolEnd]);
                        state.j2jDepth = 0;
                        state.j2jTotalCalls = 0;
                        state.yieldCountdown = 1000;

                        // --- Enter callee JIT code ---
                        // No flip — W^X audit 2026-04-26.
                        JIT_CALL(chainJM->codeStart(), &state);
                        chargeJITBytecodes(state);
                        jitJ2JStencilCalls_ += 1 + state.j2jTotalCalls;

                        // Bug-14 leak detector (kept as regression guard):
                        // fires if a callee ever returns with unpopped
                        // stencil saves.  After the primitive-stencil fix
                        // that wires every prim* exit through
                        // J2J_INLINE_RETURN_NO_TRACE, this should stay 0.
                        if (g_debug.b5Trace && state.exitReason == jit::ExitReturn
                            && state.j2jDepth > 0) {
                            static size_t leakCount = 0;
                            leakCount++;
                            if (leakCount <= 20) {
                                std::string calleeCls = classNameOfMethod(chainTarget);
                                std::string calleeSel = memory_.selectorOf(chainTarget);
                                fprintf(stderr, "[B5-LEAK] #%zu callee=0x%llx cls=%s sel=#%s "
                                                "returned with stencil state.j2jDepth=%d, "
                                                "retVal=0x%llx — REGRESSION: a prim stencil "
                                                "exited without J2J_INLINE_RETURN_NO_TRACE\n",
                                        leakCount,
                                        (unsigned long long)chainTarget.rawBits(),
                                        calleeCls.c_str(), calleeSel.c_str(),
                                        state.j2jDepth,
                                        (unsigned long long)state.returnValue.rawBits());
                            }
                        }

                        // Post-bug-14 diagnostic: general retVal-mismatch detector.
                        // For #on: and #readStream callees, retVal should be
                        // an object of their "expected" class (a stream).  If
                        // we get a raw receiver/arg back (Array, String, etc.)
                        // that's a symptom of the receiver-slot corruption
                        // being chased in round 14+.  Logs once per (cls,sel,
                        // retCls) combo to avoid spam.
                        if (g_debug.b5Trace && state.exitReason == jit::ExitReturn) {
                            std::string calleeSel = memory_.selectorOf(chainTarget);
                            if (calleeSel == "on:" || calleeSel == "readStream") {
                                Oop rv = state.returnValue;
                                std::string retCls = rv.isSmallInteger() ? "SmI"
                                    : rv.isObject() && rv.rawBits() >= 0x10000
                                        ? memory_.classNameOf(rv).c_str()
                                        : "other";
                                // Array/SmallInteger/String as retVal from on:
                                // or readStream is highly suspicious.
                                bool suspicious =
                                    retCls == "Array" || retCls == "SmI" ||
                                    retCls == "ByteArray" || retCls == "ByteString" ||
                                    retCls == "String";
                                if (suspicious) {
                                    static size_t badCount = 0;
                                    badCount++;
                                    if (badCount <= 20) {
                                        std::string calleeCls =
                                            classNameOfMethod(chainTarget);
                                        fprintf(stderr, "[B5-BADRET] #%zu sel=#%s cls=%s "
                                                "retVal=0x%llx(%s) — receiver-slot corruption "
                                                "likely\n",
                                                badCount, calleeSel.c_str(),
                                                calleeCls.c_str(),
                                                (unsigned long long)rv.rawBits(),
                                                retCls.c_str());
                                    }
                                }
                            }
                        }

                        if (__builtin_expect(
                                state.exitReason == jit::ExitReturn, 1)) {
                            // === FAST PATH: callee returned ===
                            // Still in executable mode — no JIT page writes needed
                            Oop retVal = state.returnValue;
                            jitJ2JStencilReturns_++;

                            // Restore caller state
                            state.sp = savedSP;
                            state.receiver = savedRecv;
                            state.tempBase = savedTempBase;
                            state.literals = savedLiterals;
                            state.jitMethod = savedJitMethod;
                            state.argCount = savedArgCount;
                            state.method = savedMethod;

                            // Pop receiver+args, push return value
                            state.sp[-(nArgs + 1)] = retVal;
                            state.sp -= nArgs;

                            // Sync interpreter state for resume
                            stackPointer_ = state.sp;

                            // Use precomputed resume to skip tryResume overhead
                            if (__builtin_expect(savedResumeEntry != nullptr, 1)) {
                                state.ip = savedBcStart;
                                state.sp = stackPointer_;
                                state.icDataPtr = nullptr;
                                state.sendArgCount = 0;
                                state.exitReason = jit::ExitNone;
                                state.j2jDepth = 0;
                                state.j2jTotalCalls = 0;
                                state.j2jSaveCursor = reinterpret_cast<uint8_t*>(&j2jPool_[j2jPoolBase]);
                                state.j2jSaveLimit  = reinterpret_cast<uint8_t*>(&j2jPool_[j2jPoolEnd]);
                                state.yieldCountdown = 1000;
                                // Direct resume — no hash lookup or codeOffsetForBC.
                                // Stay in X — W^X audit 2026-04-26.
                                JIT_CALL(savedResumeEntry, &state);
                                jitJ2JActChains_++;
                                jitJ2JStencilCalls_ += state.j2jTotalCalls;
                                chargeJITBytecodes(state);
                                // Materialize any J2J frames from the resumed caller
                                if (state.j2jDepth > 0) {
                                    Oop nil = memory_.nil();
                                    for (int i = 0; i < state.j2jDepth; i++) {
                                        if (frameDepth_ >= StackOverflowLimit) break;
                                        J2JSave& save = j2jStack[i];
                                        jit::JITMethod* saveJM = save.jitMethod;
                                        if (!saveJM) {
                                            static int warns = 0;
                                            if (++warns <= 5)
                                                fprintf(stderr, "[JIT] WARN: null save.jitMethod at site6 (warn #%d)\n", warns);
                                            break;
                                        }
                                        SavedFrame& frame = savedFrames_[frameDepth_++];
                                        Oop saveMethod = Oop::fromRawBits(saveJM->compiledMethodOop);
                                        frame.savedIP = save.ip;
                                        frame.savedBytecodeEnd =
                                            saveJM->bcStart() + saveJM->numBytecodes;
                                        frame.savedMethod = saveMethod;
                                        frame.savedHomeMethod = saveMethod;
                                        frame.savedReceiver = save.receiver;
                                        frame.savedClosure = nil;
                                        frame.savedActiveContext = nil;
                                        frame.materializedContext = nil;
                                        frame.savedFP = save.tempBase - 1;
                                        frame.savedArgCount = saveJM->argCount;
                                        frame.homeFrameDepth = SIZE_MAX;
                                    }
                                    chainCallDepth += state.j2jDepth;
                                    if (state.jitMethod) {
                                        state.method = Oop::fromRawBits(
                                            state.jitMethod->compiledMethodOop);
                                    }
                                    method_ = state.method;
                                    method = state.method;
                                    homeMethod_ = state.method;
                                    receiver_ = state.receiver;
                                    stackPointer_ = state.sp;
                                    instructionPointer_ = state.ip;
                                    framePointer_ = state.tempBase - 1;
                                    argCount_ = state.argCount;
                                    {
                                        ObjectHeader* mObj = state.method.asObjectPtr();
                                        if (mObj) bytecodeEnd_ = mObj->bytes() + mObj->byteSize();
                                    }
                                }
                                if (checkCountdown_ <= 0) goto jit_loop_exit;
                                continue;
                            }

                            // Precompute failed — fall back to tryResume.
                            // Stay in X — W^X audit 2026-04-26.
                            {
                                ObjectHeader* cMO = savedMethod.asObjectPtr();
                                Oop hdr = cMO->slots()[0];
                                int cNL = hdr.isSmallInteger() ? (hdr.asSmallInteger() & 0x7FFF) : 0;
                                state.ip = cMO->bytes() + (1 + cNL) * 8;
                            }
                            jitJ2JActChains_++;
                            method = method_;
                            uint32_t bcOffset = computeCurrentBCOffset();
                            if (bcOffset == UINT32_MAX) return true;
                            state.sp = stackPointer_;
                            state.icDataPtr = nullptr;
                            state.sendArgCount = 0;
                            state.exitReason = jit::ExitNone;
                            // J2J disabled: precompute-failed is rare (0 chains in AWFY)
                            state.j2jSaveCursor = nullptr;
                            state.j2jSaveLimit = nullptr;
                            state.j2jDepth = 0;
                            state.j2jTotalCalls = 0;
                            state.yieldCountdown = 1000;
                            if (!jitRuntime_.tryResume(method, bcOffset, state)) {
                                return true;
                            }
                            chargeJITBytecodes(state);
                            materializeJ2J();
                            if (checkCountdown_ <= 0) goto jit_loop_exit;
                            continue;
                        }
                        // Stay in X — W^X audit 2026-04-26.

                        // === FALLBACK: callee exited with non-ExitReturn ===
                        // Push a SavedFrame for the caller so the interpreter
                        // can return to it, then bail to interpreter for the
                        // callee's remaining work.
                        // Guard: savedMethod can be nil if the chain
                        // loop's saved state was zero-initialized or
                        // FreeBlock-overwritten (eviction reuse).
                        // Bug 11b layer 4 — without the guard sMO is
                        // null and sMO->byteSize() SEGVs at offset 0.
                        ObjectHeader* sMO = savedMethod.isObject() && savedMethod.rawBits() != 0
                            ? savedMethod.asObjectPtr()
                            : nullptr;
                        if (sMO && frameDepth_ < StackOverflowLimit) {
                            SavedFrame& frame = savedFrames_[frameDepth_++];
                            frame.savedIP = instructionPointer_;  // past-send
                            frame.savedBytecodeEnd = sMO->bytes() + sMO->byteSize();
                            frame.savedMethod = savedMethod;
                            frame.savedHomeMethod = savedMethod;
                            frame.savedReceiver = savedRecv;
                            frame.savedClosure = memory_.nil();
                            frame.savedActiveContext = memory_.nil();
                            frame.materializedContext = memory_.nil();
                            frame.savedFP = savedTempBase - 1;
                            frame.savedArgCount = savedArgCount;
                            frame.homeFrameDepth = SIZE_MAX;
                        }

                        // Materialize any J2J frames the callee accumulated
                        if (state.j2jDepth > 0) {
                            Oop nil = memory_.nil();
                            for (int i = 0; i < state.j2jDepth; i++) {
                                if (frameDepth_ >= StackOverflowLimit) break;
                                J2JSave& save = j2jStack[i];
                                jit::JITMethod* saveJM = save.jitMethod;
                                if (!saveJM) {
                                    static int warns = 0;
                                    if (++warns <= 5)
                                        fprintf(stderr, "[JIT] WARN: null save.jitMethod at site7 (warn #%d)\n", warns);
                                    break;
                                }
                                SavedFrame& frame = savedFrames_[frameDepth_++];
                                Oop saveMethod = Oop::fromRawBits(saveJM->compiledMethodOop);
                                frame.savedIP = save.ip;
                                frame.savedBytecodeEnd =
                                    saveJM->bcStart() + saveJM->numBytecodes;
                                frame.savedMethod = saveMethod;
                                frame.savedHomeMethod = saveMethod;
                                frame.savedReceiver = save.receiver;
                                frame.savedClosure = nil;
                                frame.savedActiveContext = nil;
                                frame.materializedContext = nil;
                                frame.savedFP = save.tempBase - 1;
                                frame.savedArgCount = saveJM->argCount;
                                frame.homeFrameDepth = SIZE_MAX;
                            }
                            chainCallDepth += state.j2jDepth;
                            // Sync from innermost J2J frame
                            if (state.jitMethod) {
                                state.method = Oop::fromRawBits(
                                    state.jitMethod->compiledMethodOop);
                            }
                        }

                        // Set up interpreter state from innermost exit frame
                        Oop exitMethod = (state.j2jDepth > 0) ? state.method : chainTarget;
                        method_ = exitMethod;
                        homeMethod_ = exitMethod;
                        receiver_ = state.receiver;
                        stackPointer_ = state.sp;
                        instructionPointer_ = state.ip;
                        framePointer_ = state.tempBase - 1;
                        argCount_ = (state.j2jDepth > 0) ? state.argCount : nArgs;
                        if (exitMethod.isObject() && exitMethod.rawBits() != 0) {
                            ObjectHeader* tMO = exitMethod.asObjectPtr();
                            if (tMO) bytecodeEnd_ = tMO->bytes() + tMO->byteSize();
                        }
                        closure_ = memory_.nil();
                        jitJ2JActFalls_++;
                        switch (state.exitReason) {
                        case jit::ExitSendCached: jitStencilFallSendCached_++; break;
                        case jit::ExitSend:       jitStencilFallSend_++; break;
                        case jit::ExitJ2JCall:    jitStencilFallJ2JCall_++; break;
                        default:                  jitStencilFallOther_++; break;
                        }

                        // Bail: let interpreter dispatch handle the callee
                        return true;
                    }
                }
            }
#endif // PHARO_JIT_ENABLED

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
                            // Block evaluation (prim 207/209): try executing
                            // the block's stencils inline in the chain loop.
                            // activateBlock has pushed a frame and set up
                            // method_, ip_, sp_, receiver_ for the block.
                            // ExitReturn from a block is always a local return
                            // (NLR compiles to stencil_send, not a return).
                            if (primIdx == 207 || primIdx == 209) {
                                // Block evaluation: instead of bailing, switch
                                // the chain loop to execute the block's code.
                                // activateBlock has set up method_, ip_, sp_
                                // for the CompiledBlock. Set up chain state
                                // and continue the loop — the normal ExitReturn
                                // handler will popFrame when the block completes.
                                jitRuntime_.noteMethodEntry(method_);
                                method = method_;
                                methObj = method.asObjectPtr();
                                uint32_t blockBC = computeCurrentBCOffset();
                                {
                                    state.sp = stackPointer_;
                                    state.receiver = receiver_;
                                    state.literals = methObj->slots() + 1;
                                    state.tempBase = framePointer_ + 1;
                                    {
                                        Oop hdr = methObj->slots()[0];
                                        int nl = hdr.isSmallInteger() ? (hdr.asSmallInteger() & 0x7FFF) : 0;
                                        state.ip = methObj->bytes() + (1 + nl) * 8;
                                    }
                                    state.method = method;
                                    state.argCount = argCount_;
                                    state.icDataPtr = nullptr;
                                    state.sendArgCount = 0;
                                    enableJ2J();
                                    // Use tryExecute (starts from beginning) not
                                    // tryResume (rejects bc offset 0).
                                    if (jitRuntime_.tryExecute(method, state)) {
                                        chargeJITBytecodes(state);
                                        materializeJ2J();
                                        jitJ2JActChains_++;
                                        jitChainPrimChains_++;
                                        if (checkCountdown_ <= 0) goto jit_loop_exit;
                                        continue;  // Continue chain loop with block
                                    }
                                }
                            }
                            jitJ2JActFalls_++;
                            jitChainPrimFalls_++;
                            // Profile which primitives push frames
                            if (primIdx < 600) jitPrimFallHisto_[primIdx]++;
                            return true;
                        }
                        // Primitive completed in place — resume JIT at bytecode after send
                        jitJ2JActChains_++;
                        jitChainPrimChains_++;
                        if (primIdx < 600) jitPrimChainHisto_[primIdx]++;
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
                        // Adaptive J2J depth: per-method limit (default 2,
                        // promotes to 8 on clean runs, demotes on bail).
                        {
                            jit::JITMethod* resumeJM = jitRuntime_.methodMap().lookup(method.rawBits());
                            int depth = (resumeJM && resumeJM->stats &&
                                         resumeJM->stats->j2jDepthLimit >= 2)
                                ? resumeJM->stats->j2jDepthLimit : 2;
                            state.j2jSaveCursor = reinterpret_cast<uint8_t*>(j2jStack);
                            state.j2jSaveLimit = reinterpret_cast<uint8_t*>(j2jStack + depth);
                        }
                        state.j2jDepth = 0;
                        state.j2jTotalCalls = 0;
                        if (!jitRuntime_.tryResume(method, bcOffset, state)) {
                            return true;
                        }
                        chargeJITBytecodes(state);
                        {
                            bool bailed = (state.j2jDepth > 0);
                            materializeJ2J();
                            adaptJ2JDepth(method, bailed);
                        }
                        if (checkCountdown_ <= 0) goto jit_loop_exit;
                        continue;
                    }
                }
            }

            // Inline chain activation removed (2026-04-11): confirmed dead code —
            // stencil-J2J path above catches all JIT-compiled targets first.
            // Diagnostic counters showed 0 entries across all AWFY benchmarks.
            {
                // Fallback: non-JIT target or unsafe prim — use activateMethod
                size_t callerDepth = frameDepth_;
                activateMethod(chainTarget, nArgs);

                if (frameDepth_ != callerDepth) {
                    // Target pushed a frame — interpreter dispatch loop handles it
                    jitJ2JActFalls_++;
                    jitChainActivateFalls_++;
                    return true;
                }

                // Target completed (JIT or trivial method handled it end-to-end).
                // Resume JIT execution at the bytecode after the send.
                jitJ2JActChains_++;
                jitChainActivateChains_++;
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
                // J2J disabled: activateMethod fallback is rare (0 chains in AWFY)
                state.j2jSaveCursor = nullptr;
                state.j2jSaveLimit = nullptr;
                state.j2jDepth = 0;
                state.j2jTotalCalls = 0;
                state.yieldCountdown = 1000;

                if (!jitRuntime_.tryResume(method, bcOffset, state)) {
                    return true;
                }
                chargeJITBytecodes(state);
                materializeJ2J();
                if (checkCountdown_ <= 0) goto jit_loop_exit;
                continue;
            }
        }
    }
    // Chain limit reached — fall through to handle unprocessed exit

jit_loop_exit:
    // If at inline depth, handle the unprocessed exit and bail to interpreter.
    // The inline frames are standard SavedFrames — the interpreter dispatch
    // loop will pop them naturally as methods return.
    if (chainCallDepth > 0) {
        if (state.exitReason == jit::ExitReturn) {
            // Callee returned — pop one frame, push return value.
            // Remaining inline frames (if any) are left for the interpreter.
            popFrame();
            push(state.returnValue);
        } else {
            // Non-return exit — sync callee's state so interpreter can continue.
            instructionPointer_ = state.ip;
            stackPointer_ = state.sp;
        }
        return true;  // Interpreter dispatch loop handles the rest
    }
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
                handleBenchComplete(state.returnValue);
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

    case jit::ExitYield:
        // Yield after chain limit / countdown expired — let interpreter run
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
