/*
 * Interpreter.cpp - Bytecode Interpreter Implementation
 *
 * This implements the Sista V1 bytecode interpreter.
 */

#include "Interpreter.hpp"
#include "InterpreterProxy.h"
#include "FFI.hpp"
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
#include <atomic>
#include <dlfcn.h>

// Flag set by FFI.cpp when Emergency Debugger window is created
extern std::atomic<bool> g_emergencyDebuggerTriggered;

#if __APPLE__
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
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
// Watchdog send diagnostic: selector and receiver class name for last send
char g_watchdogSelector[64] = {0};
char g_watchdogReceiverClass[64] = {0};
char g_lastSelName[64] = {0};
volatile int g_watchdogPrimIndex = 0;
volatile int g_watchdogProcessPriority = 0;  // Current process priority (updated in step loop)
volatile sig_atomic_t g_sigsegvRecoveryEnabled = 0;

// Trace sends after SDL_PollEvent returns a mouse event (set by FFI.cpp)
std::atomic<int> g_traceAfterMouseSDLEvent{0};

namespace pharo {

// Set to false to disable all debug file logging for performance
constexpr bool ENABLE_DEBUG_LOGGING = false;  // Disabled for performance

// Global flag to trace sends after primitive 264 completes
int g_traceSendsAfterPrim264 = 0;

// Global step count and send trace file for startup debugging
int64_t g_stepCount = 0;

// Trace flag for mouse button events (defined in FFI.cpp)
extern int g_traceEventSends;

// Track the p40 (MorphicRenderLoop) process for diagnostics
static pharo::Oop g_savedP40Process = pharo::Oop::nil();

// Global flag to trace jumps after isActivePlatform returns TRUE

// REMOVED: g_debugPendingFlag (was for workaround code)

uint64_t g_stepNum = 0;  // Global step counter for hang debugging (non-static for use in Primitives.cpp)
const char* g_xferReason = "unknown";  // Reason for the most recent transferTo call


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
    , argCount_(0)
    , extA_(0)
    , extB_(0)
    , usesSistaV1_(true)  // Default to SistaV1, will be set per method
    , running_(false)
    , primitiveFailed_(false)
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

    {
        FILE* initLog = nullptr;
        if (initLog) { fprintf(initLog, "[INIT] Starting interpreter initialize...\n"); fclose(initLog); }
    }

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
            std::cerr << "[INIT] Invalidated " << invalidated
                      << " stale ExternalAddress objects\n";
        }
    }

    Oop scheduler = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);
    // DEBUG_LOG("[DEBUG] Scheduler: 0x" << std::hex << scheduler.rawBits() << std::dec;
    if (scheduler.isNil()) {
        return false;
    }

    // The scheduler association value is the ProcessScheduler
    // DEBUG: "[DEBUG] Getting process scheduler..."
    Oop processScheduler = memory_.fetchPointer(1, scheduler);  // value
    // DEBUG_LOG("[DEBUG] ProcessScheduler: 0x" << std::hex << processScheduler.rawBits() << std::dec;
    if (processScheduler.isNil()) {
        // DEBUG: "[DEBUG] ProcessScheduler is nil"
        return false;
    }

    // Dump all processes in the scheduler to understand what's running
    if constexpr (ENABLE_DEBUG_LOGGING) {
    static FILE* procLog = nullptr;
    if (procLog) {
        fprintf(procLog, "[INIT] Dumping all processes in scheduler\n");
        Oop nilObj = memory_.specialObject(SpecialObjectIndex::NilObject);

        // Get quiescentProcessLists (array of LinkedLists, one per priority)
        Oop queues = memory_.fetchPointer(0, processScheduler);
        if (queues.isObject()) {
            ObjectHeader* queuesHdr = queues.asObjectPtr();
            size_t numPriorities = queuesHdr->slotCount();
            fprintf(procLog, "[INIT] Found %zu priority levels\n", numPriorities);

            for (size_t pri = 0; pri < numPriorities; pri++) {
                Oop queue = queuesHdr->slotAt(pri);
                if (queue.rawBits() == nilObj.rawBits() || !queue.isObject()) continue;

                // LinkedList: slot 0 = firstLink, slot 1 = lastLink
                Oop proc = memory_.fetchPointer(0, queue);
                while (proc.isObject() && proc.rawBits() != nilObj.rawBits()) {
                    // Get process name if it has one (slot 3 is name in modern Pharo)
                    ObjectHeader* procHdr = proc.asObjectPtr();
                    std::string procName = "<unnamed>";
                    if (procHdr->slotCount() > 3) {
                        Oop nameOop = memory_.fetchPointer(3, proc);
                        if (nameOop.isObject() && nameOop.rawBits() != nilObj.rawBits()) {
                            ObjectHeader* nameHdr = nameOop.asObjectPtr();
                            if (nameHdr->isBytesObject() && nameHdr->byteSize() < 100) {
                                procName = std::string((char*)nameHdr->bytes(), nameHdr->byteSize());
                            }
                        }
                    }

                    // Get suspended context to find what method it's in
                    std::string methodInfo = "<no context>";
                    if (procHdr->slotCount() > 1) {
                        Oop ctx = memory_.fetchPointer(1, proc);  // suspendedContext
                        if (ctx.isObject() && ctx.rawBits() != nilObj.rawBits()) {
                            ObjectHeader* ctxHdr = ctx.asObjectPtr();
                            if (ctxHdr->slotCount() > 3) {
                                Oop method = memory_.fetchPointer(3, ctx);  // method
                                if (method.isObject()) {
                                    // Extract numLiterals from method header (slot 0)
                                    Oop headerOop = memory_.fetchPointer(0, method);
                                    if (headerOop.isSmallInteger()) {
                                        int64_t headerBits = headerOop.asSmallInteger();
                                        int numLits = headerBits & 0x7FFF;  // bits 0-14
                                        if (numLits > 0) {
                                            // Last literal (at index numLits) often contains selector
                                            Oop assoc = memory_.fetchPointer(numLits, method);
                                            if (assoc.isObject()) {
                                                ObjectHeader* assocHdr = assoc.asObjectPtr();
                                                // Could be an Association with key = selector, or AdditionalMethodState
                                                if (assocHdr->slotCount() > 0) {
                                                    Oop selector = memory_.fetchPointer(0, assoc);
                                                    if (selector.isObject()) {
                                                        ObjectHeader* selHdr = selector.asObjectPtr();
                                                        if (selHdr->isBytesObject() && selHdr->byteSize() < 100) {
                                                            methodInfo = std::string((char*)selHdr->bytes(), selHdr->byteSize());
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
                    fprintf(procLog, "[INIT] Priority %zu: Process '%s' in #%s (0x%llx)\n",
                            pri + 1, procName.c_str(), methodInfo.c_str(), (unsigned long long)proc.rawBits());

                    // Follow nextLink to next process in this queue
                    proc = memory_.fetchPointer(0, proc);
                }
            }
        }

        // Also dump the active process
        Oop active = memory_.fetchPointer(1, processScheduler);
        if (active.isObject()) {
            ObjectHeader* activeHdr = active.asObjectPtr();
            std::string activeName = "<unnamed>";
            if (activeHdr->slotCount() > 3) {
                Oop nameOop = memory_.fetchPointer(3, active);
                if (nameOop.isObject() && nameOop.rawBits() != nilObj.rawBits()) {
                    ObjectHeader* nameHdr = nameOop.asObjectPtr();
                    if (nameHdr->isBytesObject() && nameHdr->byteSize() < 100) {
                        activeName = std::string((char*)nameHdr->bytes(), nameHdr->byteSize());
                    }
                }
            }

            // Get method for active process context
            std::string activeMethod = "<no context>";
            if (activeHdr->slotCount() > 1) {
                Oop ctx = memory_.fetchPointer(1, active);
                if (ctx.isObject() && ctx.rawBits() != nilObj.rawBits()) {
                    ObjectHeader* ctxHdr = ctx.asObjectPtr();
                    if (ctxHdr->slotCount() > 3) {
                        Oop method = memory_.fetchPointer(3, ctx);
                        if (method.isObject()) {
                            // Extract numLiterals from method header (slot 0)
                            Oop headerOop = memory_.fetchPointer(0, method);
                            if (headerOop.isSmallInteger()) {
                                int64_t headerBits = headerOop.asSmallInteger();
                                int numLits = headerBits & 0x7FFF;  // bits 0-14
                                if (numLits > 0) {
                                    // Last literal (at index numLits) often contains selector
                                    Oop assoc = memory_.fetchPointer(numLits, method);
                                    if (assoc.isObject()) {
                                        ObjectHeader* assocHdr = assoc.asObjectPtr();
                                        if (assocHdr->slotCount() > 0) {
                                            Oop selector = memory_.fetchPointer(0, assoc);
                                            if (selector.isObject()) {
                                                ObjectHeader* selHdr = selector.asObjectPtr();
                                                if (selHdr->isBytesObject() && selHdr->byteSize() < 100) {
                                                    activeMethod = std::string((char*)selHdr->bytes(), selHdr->byteSize());
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
            fprintf(procLog, "[INIT] Active process: '%s' in #%s (0x%llx)\n",
                    activeName.c_str(), activeMethod.c_str(), (unsigned long long)active.rawBits());
        }
        fflush(procLog);
    }
    }

    // Get the active process
    // ProcessScheduler layout: quiescentProcessLists (slot 0), activeProcess (slot 1)
    // DEBUG: "[DEBUG] Getting active process..."
    Oop activeProcess = memory_.fetchPointer(1, processScheduler);  // activeProcess is slot 1!
    // DEBUG_LOG("[DEBUG] ActiveProcess: 0x" << std::hex << activeProcess.rawBits() << std::dec;
    if (activeProcess.isNil()) {
        // DEBUG: "[DEBUG] ActiveProcess is nil"
        return false;
    }

    // Initialize ProcessSignalingLowSpace to active process before execution.
    // This ensures the lowSpaceWatcher has a valid context if it wakes up early.
    // Pharo's lowSpaceWatcher reads ProcessSignalingLowSpace and expects a valid
    // process/context there. Without this, it gets nil and errors.
    Oop currentPSLS = memory_.specialObject(SpecialObjectIndex::ProcessSignalingLowSpace);
    if (currentPSLS.isNil() || currentPSLS.rawBits() == memory_.nil().rawBits()) {
        memory_.setSpecialObject(SpecialObjectIndex::ProcessSignalingLowSpace, activeProcess);
        FILE* initLog = nullptr;
        if (initLog) {
            fprintf(initLog, "[INIT] Set ProcessSignalingLowSpace to active process 0x%llx\n",
                    (unsigned long long)activeProcess.rawBits());
            fclose(initLog);
        }
    }

    // Note: ExternalObjectsArray is managed by Pharo's VirtualMachine class.
    // Pharo calls clearExternalObjects during startup which creates a fresh array.
    // The VM should NOT pre-initialize this array as Pharo will replace it.
    // Instead, the VM's parameter 49 set operation handles resizing when needed.

    // Debug: dump active process slots
    // DEBUG: "[DEBUG] Active Process slots:"
    if (activeProcess.isObject()) {
        ObjectHeader* procHeader = activeProcess.asObjectPtr();
        size_t slots = procHeader->slotCount();
        // std::cerr << "  ActiveProcess has " << slots << " slots, classIndex=" << procHeader->classIndex(); // DEBUG
        for (size_t i = 0; i < slots && i < 6; i++) {
            Oop slot = procHeader->slotAt(i);
            // std::cerr << "  [" << i << "]: 0x" << std::hex << slot.rawBits() << std::dec;
            if (slot.isSmallInteger()) {
                // std::cerr << " (SmallInt: " << slot.asSmallInteger() << ")";
            }
            // std::cerr; // DEBUG
        }
    }

    // Get the suspended context
    // Modern Pharo Process layout:
    //   slot 0 = nextLink (for LinkedList)
    //   slot 1 = suspendedContext
    //   slot 2 = priority
    // DEBUG: "[DEBUG] Getting suspended context..."
    Oop context = memory_.fetchPointer(1, activeProcess);  // suspendedContext is slot 1 in modern Pharo
    // DEBUG_LOG("[DEBUG] Context: 0x" << std::hex << context.rawBits() << std::dec;

    // Check for unrelocated pointer - if it points to the old image base area
    // Old images use base address 0x10000000000 (1TB)
    // Our loaded images are in a much lower address range
    uint64_t contextAddr = context.rawBits() & ~7ULL;
    if (contextAddr >= 0x10000000000ULL && contextAddr < 0x20000000000ULL) {
        // Context appears unrelocated - use bootstrap startup
        return bootstrapStartup();
    }

    // Check if context is nil (fresh image startup)
    if (context.isNil() || (context.isObject() && context.asObjectPtr()->slotCount() == 0)) {
        return bootstrapStartup();
    }

    // We have a valid context - but first analyze the sender chain
    // to understand what code we're resuming in
    // DEBUG: "[DEBUG] Analyzing sender chain..."

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

        std::string rcvrClassName = "<unknown>";
        std::string methodSelector = "<unknown>";

        // Get receiver's class name (Pharo 12 layout)
        if (receiver.isObject() && receiver.rawBits() > 0x10000) {
            Oop rcvrClass = memory_.classOf(receiver);
            if (rcvrClass.isObject()) {
                ObjectHeader* clsHdr = rcvrClass.asObjectPtr();
                size_t clsSlots = clsHdr->slotCount();
                Oop nameOop;
                if (clsSlots == 6) {
                    // Metaclass - get thisClass from slot 5, then name from slot 6
                    Oop thisClass = memory_.fetchPointer(5, rcvrClass);
                    if (thisClass.isObject() && thisClass.rawBits() > 0x10000) {
                        ObjectHeader* tcHdr = thisClass.asObjectPtr();
                        if (tcHdr->slotCount() >= 7) {
                            nameOop = memory_.fetchPointer(6, thisClass);
                        }
                    }
                } else if (clsSlots >= 7) {
                    // Regular Class - name is at slot 6
                    nameOop = memory_.fetchPointer(6, rcvrClass);
                }
                if (nameOop.isObject() && nameOop.rawBits() > 0x10000) {
                    ObjectHeader* nameHdr = nameOop.asObjectPtr();
                    if (nameHdr->isBytesObject() && nameHdr->byteSize() <= 50) {
                        rcvrClassName = std::string((char*)nameHdr->bytes(), nameHdr->byteSize());
                    }
                }
            }
        }

        // Try to get method selector (literal 1 in CompiledMethod)
        if (method.isObject() && method.rawBits() > 0x10000) {
            ObjectHeader* methodHdr = method.asObjectPtr();
            if (methodHdr->isCompiledMethod() && methodHdr->slotCount() >= 2) {
                Oop selectorOop = memory_.fetchPointer(1, method);
                if (selectorOop.isObject() && selectorOop.rawBits() > 0x10000) {
                    ObjectHeader* selHdr = selectorOop.asObjectPtr();
                    if (selHdr->isBytesObject() && selHdr->byteSize() <= 50) {
                        methodSelector = std::string((char*)selHdr->bytes(), selHdr->byteSize());
                    }
                }
            }
        }

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

    // If we detected snapshot code, we're resuming from a saved image.
    // The Pharo snapshot code checks the return value:
    //   nil = save succeeded -> quit
    //   non-nil = resuming from save -> run startup handlers
    // We need to modify the context to indicate "resuming" by ensuring the
    // snapshot primitive returns a non-nil value (true).
    std::cerr << "[INIT] inSnapshotCode=" << inSnapshotCode << " snapshotEndDepth=" << snapshotEndDepth << "\n";
    if (inSnapshotCode) {
        // Patch the snapshot context's stack top to true so Smalltalk
        // interprets this as "resuming from saved image" instead of
        // "save succeeded, now quit".
        // Context slot 2 = stackp (1-based index of top of stack)
        // Context slot 6+ = temps and stack values
        // The stack top value (return from snapshot primitive) is at
        // slot (6 + stackp - 1) where stackp is 1-based.
        ObjectHeader* ctxHdr = context.asObjectPtr();
        Oop stackpOop = memory_.fetchPointer(2, context);
        if (stackpOop.isSmallInteger()) {
            int64_t stackp = stackpOop.asSmallInteger();
            if (stackp > 0) {
                size_t stackTopSlot = 6 + static_cast<size_t>(stackp) - 1;
                if (stackTopSlot < ctxHdr->slotCount()) {
                    Oop trueObj = memory_.specialObject(SpecialObjectIndex::TrueObject);
                    Oop oldVal = memory_.fetchPointer(stackTopSlot, context);
                    memory_.storePointer(stackTopSlot, context, trueObj);
                    std::cerr << "[INIT] Patched snapshot context stack top (slot " << stackTopSlot
                              << "): 0x" << std::hex << oldVal.rawBits()
                              << " -> true (0x" << trueObj.rawBits() << ")" << std::dec << "\n";
                }
            }
        }
    }

    // Note: Display initialization is deferred to primitiveForceDisplayUpdate
    // to avoid crashes during early VM setup

    // Log context details before executing
    {
        FILE* f = nullptr;
        if (f) {
            ObjectHeader* ctxHdr = context.asObjectPtr();
            Oop pc = memory_.fetchPointer(1, context);
            Oop stackp = memory_.fetchPointer(2, context);
            Oop method = memory_.fetchPointer(3, context);
            fprintf(f, "[INIT] Resuming context: pc=0x%llx stackp=0x%llx method=0x%llx slots=%zu\n",
                    (unsigned long long)pc.rawBits(), (unsigned long long)stackp.rawBits(),
                    (unsigned long long)method.rawBits(), ctxHdr->slotCount());
            fclose(f);
        }
    }
    // Nil out the active process's suspendedContext now that we've loaded it.
    // This prevents GC from tracing stale context chains that keep objects alive.
    if (activeProcess.isObject() && !activeProcess.isNil()) {
        memory_.storePointer(1, activeProcess, memory_.nil());  // slot 1 = suspendedContext
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
            std::cerr << "[DISPLAY] Using existing Display " << existingWidth.asSmallInteger()
                      << "x" << existingHeight.asSmallInteger() << "\n";
            return;
        }
        // Existing Display has invalid dimensions - we'll need to reinitialize it
        std::cerr << "[DISPLAY] Existing Display has invalid dimensions, reinitializing...\n";
    }

    // Find Form and Bitmap classes
    Oop formClass = memory_.findGlobal("Form");
    Oop bitmapClass = memory_.findGlobal("Bitmap");

    if (formClass.isNil() || !formClass.isObject()) {
        std::cerr << "[DISPLAY] FAILED: Form class not found (nil=" << formClass.isNil() << ")\n";
        return;
    }
    if (bitmapClass.isNil() || !bitmapClass.isObject()) {
        std::cerr << "[DISPLAY] FAILED: Bitmap class not found (nil=" << bitmapClass.isNil() << ")\n";
        return;
    }

    uint32_t formClassIdx = memory_.indexOfClass(formClass);
    uint32_t bitmapClassIdx = memory_.indexOfClass(bitmapClass);

    // If class not in table, register it
    if (formClassIdx == 0) {
        formClassIdx = memory_.registerClass(formClass);
        std::cerr << "[DISPLAY] Registered Form class at index " << formClassIdx << "\n";
    }
    if (bitmapClassIdx == 0) {
        bitmapClassIdx = memory_.registerClass(bitmapClass);
        std::cerr << "[DISPLAY] Registered Bitmap class at index " << bitmapClassIdx << "\n";
    }

    if (formClassIdx == 0 || bitmapClassIdx == 0) {
        std::cerr << "[DISPLAY] FAILED: Class index 0 after registration (Form=" << formClassIdx << ", Bitmap=" << bitmapClassIdx << ")\n";
        return;
    }

    // Allocate bitmap for pixels (32-bit pixels = 1 word each for 32-bit depth)
    size_t pixelCount = static_cast<size_t>(width) * height;
    std::cerr << "[DISPLAY] Allocating Bitmap: " << pixelCount << " pixels (" << (pixelCount * 4) << " bytes)\n";
    Oop bitmapObj = memory_.allocateWords(bitmapClassIdx, pixelCount);

    if (bitmapObj.isNil()) {
        std::cerr << "[DISPLAY] FAILED: Bitmap allocation returned nil\n";
        return;
    }

    // Fill bitmap with a distinctive color to show it's our bitmap
    ObjectHeader* bitmapHdr = bitmapObj.asObjectPtr();
    uint32_t* pixels = reinterpret_cast<uint32_t*>(bitmapHdr->bytes());
    for (size_t i = 0; i < pixelCount; i++) {
        pixels[i] = 0xFF4488CC;  // Distinctive blue-gray so we know it's ours
    }

    // Allocate Form with 5 slots: bits, width, height, depth, offset
    std::cerr << "[DISPLAY] Allocating Form with 5 slots...\n";
    Oop formObj = memory_.allocateSlots(formClassIdx, 5);

    if (formObj.isNil()) {
        std::cerr << "[DISPLAY] FAILED: Form allocation returned nil\n";
        return;
    }

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
    memory_.setGlobal("Display", formObj);
    std::cerr << "[DISPLAY] SUCCESS: Created " << width << "x" << height << "x" << depth
              << " Form @0x" << std::hex << formObj.rawBits() << std::dec << "\n";
}

// ===== BITMAP FONT FOR TEXT RENDERING =====
// Simple 5x7 bitmap font for menu bar text

static const uint8_t font5x7[96][7] = {
    // ASCII 32-127 (space through tilde)
    // Each character is 5 pixels wide, 7 pixels tall
    // Space (32)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    // ! (33)
    {0x04, 0x04, 0x04, 0x04, 0x00, 0x04, 0x00},
    // " (34)
    {0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00},
    // # (35)
    {0x0A, 0x1F, 0x0A, 0x0A, 0x1F, 0x0A, 0x00},
    // $ (36)
    {0x04, 0x0F, 0x14, 0x0E, 0x05, 0x1E, 0x04},
    // % (37)
    {0x19, 0x19, 0x02, 0x04, 0x08, 0x13, 0x13},
    // & (38)
    {0x08, 0x14, 0x14, 0x08, 0x15, 0x12, 0x0D},
    // ' (39)
    {0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00},
    // ( (40)
    {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02},
    // ) (41)
    {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08},
    // * (42)
    {0x00, 0x04, 0x15, 0x0E, 0x15, 0x04, 0x00},
    // + (43)
    {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00},
    // , (44)
    {0x00, 0x00, 0x00, 0x00, 0x04, 0x04, 0x08},
    // - (45)
    {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00},
    // . (46)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00},
    // / (47)
    {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10},
    // 0 (48)
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},
    // 1 (49)
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    // 2 (50)
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
    // 3 (51)
    {0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E},
    // 4 (52)
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
    // 5 (53)
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    // 6 (54)
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},
    // 7 (55)
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    // 8 (56)
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
    // 9 (57)
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
    // : (58)
    {0x00, 0x04, 0x00, 0x00, 0x04, 0x00, 0x00},
    // ; (59)
    {0x00, 0x04, 0x00, 0x00, 0x04, 0x04, 0x08},
    // < (60)
    {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02},
    // = (61)
    {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00},
    // > (62)
    {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08},
    // ? (63)
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04},
    // @ (64)
    {0x0E, 0x11, 0x17, 0x15, 0x17, 0x10, 0x0E},
    // A (65)
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
    // B (66)
    {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},
    // C (67)
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},
    // D (68)
    {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C},
    // E (69)
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F},
    // F (70)
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},
    // G (71)
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F},
    // H (72)
    {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
    // I (73)
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},
    // J (74)
    {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C},
    // K (75)
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11},
    // L (76)
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},
    // M (77)
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11},
    // N (78)
    {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11},
    // O (79)
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    // P (80)
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},
    // Q (81)
    {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D},
    // R (82)
    {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},
    // S (83)
    {0x0E, 0x11, 0x10, 0x0E, 0x01, 0x11, 0x0E},
    // T (84)
    {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
    // U (85)
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    // V (86)
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04},
    // W (87)
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11},
    // X (88)
    {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},
    // Y (89)
    {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04},
    // Z (90)
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F},
    // [ (91)
    {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E},
    // \ (92)
    {0x10, 0x10, 0x08, 0x04, 0x02, 0x01, 0x01},
    // ] (93)
    {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E},
    // ^ (94)
    {0x04, 0x0A, 0x11, 0x00, 0x00, 0x00, 0x00},
    // _ (95)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F},
    // ` (96)
    {0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00},
    // a (97)
    {0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F},
    // b (98)
    {0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x1E},
    // c (99)
    {0x00, 0x00, 0x0E, 0x10, 0x10, 0x10, 0x0E},
    // d (100)
    {0x01, 0x01, 0x0F, 0x11, 0x11, 0x11, 0x0F},
    // e (101)
    {0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E},
    // f (102)
    {0x06, 0x08, 0x1E, 0x08, 0x08, 0x08, 0x08},
    // g (103)
    {0x00, 0x00, 0x0F, 0x11, 0x0F, 0x01, 0x0E},
    // h (104)
    {0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x11},
    // i (105)
    {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E},
    // j (106)
    {0x02, 0x00, 0x02, 0x02, 0x02, 0x12, 0x0C},
    // k (107)
    {0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12},
    // l (108)
    {0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},
    // m (109)
    {0x00, 0x00, 0x1A, 0x15, 0x15, 0x15, 0x15},
    // n (110)
    {0x00, 0x00, 0x1E, 0x11, 0x11, 0x11, 0x11},
    // o (111)
    {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E},
    // p (112)
    {0x00, 0x00, 0x1E, 0x11, 0x1E, 0x10, 0x10},
    // q (113)
    {0x00, 0x00, 0x0F, 0x11, 0x0F, 0x01, 0x01},
    // r (114)
    {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10},
    // s (115)
    {0x00, 0x00, 0x0E, 0x10, 0x0E, 0x01, 0x1E},
    // t (116)
    {0x08, 0x08, 0x1E, 0x08, 0x08, 0x09, 0x06},
    // u (117)
    {0x00, 0x00, 0x11, 0x11, 0x11, 0x11, 0x0E},
    // v (118)
    {0x00, 0x00, 0x11, 0x11, 0x11, 0x0A, 0x04},
    // w (119)
    {0x00, 0x00, 0x11, 0x15, 0x15, 0x15, 0x0A},
    // x (120)
    {0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11},
    // y (121)
    {0x00, 0x00, 0x11, 0x11, 0x0F, 0x01, 0x0E},
    // z (122)
    {0x00, 0x00, 0x1F, 0x02, 0x04, 0x08, 0x1F},
    // { (123)
    {0x02, 0x04, 0x04, 0x08, 0x04, 0x04, 0x02},
    // | (124)
    {0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
    // } (125)
    {0x08, 0x04, 0x04, 0x02, 0x04, 0x04, 0x08},
    // ~ (126)
    {0x00, 0x08, 0x15, 0x02, 0x00, 0x00, 0x00},
    // DEL (127) - blank
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
};

// Draw text using Core Graphics for proper anti-aliased font rendering
static void drawText(uint32_t* pixels, int dispWidth, int dispHeight,
                     int x, int y, const std::string& text, uint32_t color, int fontSize = 14) {
#if __APPLE__
    if (text.empty()) return;

    // Extract color components (ARGB format)
    CGFloat alpha = ((color >> 24) & 0xFF) / 255.0;
    CGFloat red = ((color >> 16) & 0xFF) / 255.0;
    CGFloat green = ((color >> 8) & 0xFF) / 255.0;
    CGFloat blue = (color & 0xFF) / 255.0;

    // Create a generous text buffer - Helvetica chars are roughly 0.6 * fontSize wide
    int textWidth = static_cast<int>(text.length()) * fontSize;  // Generous estimate
    // Font metrics: ascender can be up to 95% of fontSize for some fonts
    // Use 2x fontSize to ensure we have enough room for any font
    int textHeight = static_cast<int>(fontSize * 2.0);  // Generous room for tall glyphs

    // Clamp to display bounds
    if (x < 0 || y < 0 || x >= dispWidth || y >= dispHeight) return;
    if (x + textWidth > dispWidth) textWidth = dispWidth - x;
    if (y + textHeight > dispHeight) textHeight = dispHeight - y;

    // Create bitmap context with ARGB format matching our display buffer
    // On Apple platforms (little-endian), use ARGB with native byte order
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(
        nullptr,  // Let CG allocate memory
        textWidth, textHeight,
        8,  // bits per component
        textWidth * 4,  // bytes per row
        colorSpace,
        kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Host  // ARGB native
    );
    CGColorSpaceRelease(colorSpace);

    if (!ctx) return;

    // Clear to transparent
    CGContextClearRect(ctx, CGRectMake(0, 0, textWidth, textHeight));

    // Transform to normal text coordinates (flip Y axis)
    // CG has origin at bottom-left with Y going up
    // We want origin at top-left with Y going down
    CGContextTranslateCTM(ctx, 0, textHeight);
    CGContextScaleCTM(ctx, 1.0, -1.0);

    // Set up text rendering
    CGContextSetRGBFillColor(ctx, red, green, blue, alpha);
    CGContextSetTextDrawingMode(ctx, kCGTextFill);

    // Create font
    CTFontRef font = CTFontCreateWithName(CFSTR("Helvetica"), fontSize, nullptr);
    if (!font) {
        font = CTFontCreateWithName(CFSTR("Arial"), fontSize, nullptr);
    }
    if (!font) {
        CGContextRelease(ctx);
        return;
    }

    // Create attributed string
    CFStringRef cfText = CFStringCreateWithCString(nullptr, text.c_str(), kCFStringEncodingUTF8);
    if (!cfText) {
        CFRelease(font);
        CGContextRelease(ctx);
        return;
    }

    CFStringRef keys[] = { kCTFontAttributeName, kCTForegroundColorFromContextAttributeName };
    CFTypeRef values[] = { font, kCFBooleanTrue };
    CFDictionaryRef attrs = CFDictionaryCreate(nullptr, (const void**)keys, (const void**)values, 2,
                                                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    CFAttributedStringRef attrString = CFAttributedStringCreate(nullptr, cfText, attrs);
    CFRelease(cfText);
    CFRelease(attrs);

    if (!attrString) {
        CFRelease(font);
        CGContextRelease(ctx);
        return;
    }

    // Create line and draw
    CTLineRef line = CTLineCreateWithAttributedString(attrString);
    CFRelease(attrString);

    if (line) {
        // Position text baseline - need room for ascenders ABOVE and descenders BELOW
        // After the Y-flip transform, Y=0 is at top, Y increases downward
        // Ascender can be up to 95% of fontSize for system fonts
        CGFloat baseline = fontSize * 0.95;  // Room for tall ascenders
        CGContextSetTextPosition(ctx, 0, baseline);
        CTLineDraw(line, ctx);
        CFRelease(line);
    }

    CFRelease(font);

    // Copy rendered text to pixel buffer
    // Note: CGBitmapContext stores pixels bottom-up, but we transformed drawing to top-down
    // The pixel data is still stored bottom-up, so we need to flip Y when reading
    uint32_t* textPixels = (uint32_t*)CGBitmapContextGetData(ctx);
    if (textPixels) {
        for (int ty = 0; ty < textHeight; ty++) {
            for (int tx = 0; tx < textWidth; tx++) {
                int destX = x + tx;
                int destY = y + ty;
                if (destX >= 0 && destX < dispWidth && destY >= 0 && destY < dispHeight) {
                    // Bitmap data is stored bottom-up, flip Y to read correctly
                    int srcY = textHeight - 1 - ty;
                    uint32_t srcPixel = textPixels[srcY * textWidth + tx];
                    // ARGB format with native byte order: 0xAARRGGBB as uint32_t
                    uint8_t srcAlpha = (srcPixel >> 24) & 0xFF;
                    uint8_t srcR = (srcPixel >> 16) & 0xFF;
                    uint8_t srcG = (srcPixel >> 8) & 0xFF;
                    uint8_t srcB = srcPixel & 0xFF;
                    if (srcAlpha > 0) {
                        // Alpha blend (source is premultiplied alpha)
                        uint32_t destPixel = pixels[destY * dispWidth + destX];
                        uint8_t destR = (destPixel >> 16) & 0xFF;
                        uint8_t destG = (destPixel >> 8) & 0xFF;
                        uint8_t destB = destPixel & 0xFF;

                        // For premultiplied alpha: out = src + dest * (1 - srcAlpha)
                        uint8_t outR = srcR + (destR * (255 - srcAlpha)) / 255;
                        uint8_t outG = srcG + (destG * (255 - srcAlpha)) / 255;
                        uint8_t outB = srcB + (destB * (255 - srcAlpha)) / 255;

                        pixels[destY * dispWidth + destX] = 0xFF000000 | (outR << 16) | (outG << 8) | outB;
                    }
                }
            }
        }
    }

    CGContextRelease(ctx);
#endif
}

void Interpreter::renderWorldMorphs() {
    // Render World's morphs directly to the platform display
    // This bypasses NullWorldRenderer and draws morphs ourselves
    if (!pharo::gDisplaySurface) return;

    // Throttle to ~60fps to prevent flickering
    static auto lastRenderTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastRenderTime);
    if (elapsed.count() < 16) {  // Less than ~16ms = 60fps
        return;  // Skip this frame
    }
    lastRenderTime = now;

    static int totalMorphsDrawn = 0;
    static int renderCallCount = 0;
    renderCallCount++;

    // Debug logging to file (first 10 calls only)
    static FILE* logFile = nullptr;
    if constexpr (ENABLE_DEBUG_LOGGING) {
        if (!logFile) {
            logFile = nullptr;
        }
    }
    if (logFile && renderCallCount <= 10) {
        fprintf(logFile, "[RENDER #%d] Starting renderWorldMorphs\n", renderCallCount);
        fflush(logFile);
    }

    // Find the World global
    Oop world = memory_.findGlobal("World");
    if (world.isNil() || !world.isObject()) {
        if (logFile && renderCallCount <= 10) {
            fprintf(logFile, "[RENDER #%d] World not found or nil\n", renderCallCount);
            fflush(logFile);
        }
        return;
    }

    if (logFile && renderCallCount <= 10) {
        fprintf(logFile, "[RENDER #%d] World found at 0x%llx\n", renderCallCount, (unsigned long long)world.rawBits());
        fflush(logFile);
    }

    uint32_t* pixels = pharo::gDisplaySurface->pixels();
    int dispWidth = pharo::gDisplaySurface->width();
    int dispHeight = pharo::gDisplaySurface->height();
    Oop nilObj = memory_.nil();

    // Helper to get morph class name
    auto getMorphClassName = [this](Oop morph) -> std::string {
        Oop morphClass = memory_.classOf(morph);
        if (morphClass.isObject()) {
            ObjectHeader* clsHdr = morphClass.asObjectPtr();
            if (clsHdr->slotCount() > 6) {
                Oop clsName = memory_.fetchPointer(6, morphClass);
                if (clsName.isObject()) {
                    ObjectHeader* nameHdr = clsName.asObjectPtr();
                    if (nameHdr->isBytesObject() && nameHdr->byteSize() < 50) {
                        return std::string((char*)nameHdr->bytes(), nameHdr->byteSize());
                    }
                }
            }
        }
        return "Unknown";
    };

    // Helper to extract color from a Color object
    auto extractColor = [this, nilObj](Oop colorObj) -> uint32_t {
        if (colorObj.isNil() || !colorObj.isObject()) return 0xFFCCCCCC;

        // Color stores RGB in slot 0 (privateRGB - may be SmallInteger or Float)
        Oop rgb = memory_.fetchPointer(0, colorObj);

        if (rgb.isSmallInteger()) {
            int rgbVal = static_cast<int>(rgb.asSmallInteger());
            return 0xFF000000 | (rgbVal & 0xFFFFFF);
        }

        // Handle BoxedFloat64 - Pharo stores RGB as a float 0.0 to 16777215.0
        if (rgb.isObject()) {
            ObjectHeader* rgbHdr = rgb.asObjectPtr();
            if (rgbHdr->isBytesObject() && rgbHdr->byteSize() == 8) {
                double* floatVal = reinterpret_cast<double*>(rgbHdr->bytes());
                int rgbInt = static_cast<int>(*floatVal) & 0xFFFFFF;
                return 0xFF000000 | rgbInt;
            }
        }

        // Return a default gray color for unhandled cases
        return 0xFFCCCCCC;
    };

    // Helper to extract rectangle from a slot
    auto extractRect = [this](Oop rect, int& x1, int& y1, int& x2, int& y2) -> bool {
        if (rect.isNil() || !rect.isObject()) return false;

        // Rectangle slots: 0=origin (Point), 1=corner (Point)
        Oop origin = memory_.fetchPointer(0, rect);
        Oop corner = memory_.fetchPointer(1, rect);
        if (origin.isNil() || corner.isNil()) return false;
        if (!origin.isObject() || !corner.isObject()) return false;

        // Point slots: 0=x, 1=y
        Oop originX = memory_.fetchPointer(0, origin);
        Oop originY = memory_.fetchPointer(1, origin);
        Oop cornerX = memory_.fetchPointer(0, corner);
        Oop cornerY = memory_.fetchPointer(1, corner);

        // Extract coordinates - handle both SmallInteger and SmallFloat
        auto extractCoord = [](Oop coord) -> int {
            if (coord.isSmallInteger()) {
                return static_cast<int>(coord.asSmallInteger());
            } else if (coord.isSmallFloat()) {
                return static_cast<int>(coord.asSmallFloat());
            }
            return 0;
        };

        x1 = extractCoord(originX);
        y1 = extractCoord(originY);
        x2 = extractCoord(cornerX);
        y2 = extractCoord(cornerY);

        return true;
    };

    // Helper to extract bounds from a morph
    auto extractBounds = [this, nilObj, &extractRect, dispWidth, dispHeight, &getMorphClassName]
            (Oop morph, int& x1, int& y1, int& x2, int& y2) -> bool {
        // Try bounds (slot 0) first
        if (extractRect(memory_.fetchPointer(0, morph), x1, y1, x2, y2)) {
            // Check if bounds are valid (non-zero size)
            if (x2 - x1 > 0 && y2 - y1 > 0) return true;
        }

        // Try fullBounds (slot 3) as fallback
        if (extractRect(memory_.fetchPointer(3, morph), x1, y1, x2, y2)) {
            if (x2 - x1 > 0 && y2 - y1 > 0) return true;
        }

        // Assign fallback bounds based on morph class (for headless images)
        std::string className = getMorphClassName(morph);
        if (className == "MenubarMorph") {
            x1 = 0; y1 = 0; x2 = dispWidth; y2 = 25;
            return true;
        } else if (className == "TaskbarMorph") {
            x1 = 0; y1 = dispHeight - 40; x2 = dispWidth; y2 = dispHeight;
            return true;
        } else if (className == "SpWindow" || className == "SystemWindow") {
            x1 = 50; y1 = 50; x2 = dispWidth - 100; y2 = dispHeight - 100;
            return true;
        } else if (className.find("Grip") != std::string::npos) {
            // Window grips - skip (will be positioned relative to window)
            return false;
        }

        // For other morphs with zero bounds, skip rendering
        return false;
    };

    // Helper to extract string from a string object (ByteString, WideString, etc.)
    auto extractString = [this](Oop strObj) -> std::string {
        if (strObj.isNil() || !strObj.isObject()) return "";
        ObjectHeader* hdr = strObj.asObjectPtr();
        if (hdr->isBytesObject() && hdr->byteSize() < 100) {
            return std::string((char*)hdr->bytes(), hdr->byteSize());
        }
        return "";
    };

    // Helper to render MenubarMorph with text
    auto renderMenuBar = [&](Oop menubarMorph, int menuBarHeight) {
        // Offset below Mac title bar (title bar is ~28px, but in Retina it's ~56px)
        // Use a safe offset that works for both regular and Retina displays
        int titleBarOffset = (dispHeight > 1000) ? 56 : 28;  // Retina vs regular

        // Scale menu bar height for Retina
        int scaledMenuBarHeight = (dispHeight > 1000) ? menuBarHeight * 2 : menuBarHeight;

        // Draw menu bar background - Mac-style light gray with subtle gradient
        // Fill from y=0 to cover any gaps below the native title bar
        // Use solid gray for the area under the title bar (traffic lights)
        uint32_t titleAreaColor = 0xFFF6F6F6;  // Light gray matching top of menu bar
        for (int y = 0; y < titleBarOffset && y < dispHeight; y++) {
            for (int x = 0; x < dispWidth; x++) {
                pixels[y * dispWidth + x] = titleAreaColor;
            }
        }
        // Top color slightly lighter, bottom slightly darker for depth
        for (int y = titleBarOffset; y < titleBarOffset + scaledMenuBarHeight; y++) {
            // Subtle gradient from 0xF6 at top to 0xE8 at bottom
            int progress = y - titleBarOffset;
            int gray = 246 - (progress * 14 / scaledMenuBarHeight);  // 0xF6 to 0xE8
            uint32_t menuBarColor = 0xFF000000 | (gray << 16) | (gray << 8) | gray;
            for (int x = 0; x < dispWidth; x++) {
                if (y < dispHeight) {
                    pixels[y * dispWidth + x] = menuBarColor;
                }
            }
        }

        // Draw bottom border line (subtle shadow)
        int borderY = titleBarOffset + scaledMenuBarHeight - 1;
        if (borderY < dispHeight) {
            for (int x = 0; x < dispWidth; x++) {
                pixels[borderY * dispWidth + x] = 0xFFD0D0D0;  // Light gray border
            }
        }

        // Get submorphs (menu items)
        Oop submorphs = memory_.fetchPointer(2, menubarMorph);
        if (submorphs.isNil() || !submorphs.isObject()) {
            if (logFile && renderCallCount <= 10) {
                fprintf(logFile, "[MENUBAR] No submorphs found\n");
                fflush(logFile);
            }
            return;
        }

        ObjectHeader* subHdr = submorphs.asObjectPtr();
        size_t numItems = subHdr->slotCount();

        if (logFile && renderCallCount <= 10) {
            fprintf(logFile, "[MENUBAR] Found %zu menu items\n", numItems);
            fflush(logFile);
        }

        // Menu item text color (dark gray for readability on light background)
        uint32_t textColor = 0xFF1A1A1A;

        // Collect menu item labels and store morphs for dropdown access
        std::vector<std::string> labels;
        menuBarItemMorphs_.clear();
        for (size_t i = 0; i < numItems; i++) {
            Oop item = subHdr->slotAt(i);
            if (item.isNil() || !item.isObject()) continue;

            // Try to extract label from menu item
            // MenubarItemMorph or similar - try slot 5 (label) or look for StringMorph submorph
            std::string label;

            // Check slots 5-10 for potential label
            ObjectHeader* itemHdr = item.asObjectPtr();
            for (size_t slot = 5; slot < std::min((size_t)15, itemHdr->slotCount()); slot++) {
                Oop slotVal = memory_.fetchPointer(slot, item);
                if (slotVal.isNil() || !slotVal.isObject()) continue;

                // Check if it's a string
                ObjectHeader* slotHdr = slotVal.asObjectPtr();
                if (slotHdr->isBytesObject() && slotHdr->byteSize() > 0 && slotHdr->byteSize() < 50) {
                    std::string s((char*)slotHdr->bytes(), slotHdr->byteSize());
                    // Filter: must be readable text (printable ASCII)
                    bool valid = true;
                    for (char c : s) {
                        if (c < 32 || c > 126) { valid = false; break; }
                    }
                    if (valid && s.length() > 0 && s.length() < 20) {
                        label = s;
                        break;
                    }
                }
            }

            // If no label found in item, check its submorphs for StringMorph
            if (label.empty()) {
                Oop itemSubmorphs = memory_.fetchPointer(2, item);
                if (!itemSubmorphs.isNil() && itemSubmorphs.isObject()) {
                    ObjectHeader* isHdr = itemSubmorphs.asObjectPtr();
                    for (size_t j = 0; j < isHdr->slotCount(); j++) {
                        Oop subm = isHdr->slotAt(j);
                        if (subm.isNil() || !subm.isObject()) continue;

                        // Check if it's a StringMorph by looking for a string in slot 5+
                        ObjectHeader* submHdr = subm.asObjectPtr();
                        for (size_t slot = 5; slot < std::min((size_t)15, submHdr->slotCount()); slot++) {
                            Oop slotVal = memory_.fetchPointer(slot, subm);
                            if (slotVal.isNil() || !slotVal.isObject()) continue;

                            ObjectHeader* svHdr = slotVal.asObjectPtr();
                            if (svHdr->isBytesObject() && svHdr->byteSize() > 0 && svHdr->byteSize() < 50) {
                                std::string s((char*)svHdr->bytes(), svHdr->byteSize());
                                bool valid = true;
                                for (char c : s) {
                                    if (c < 32 || c > 126) { valid = false; break; }
                                }
                                if (valid && s.length() > 0 && s.length() < 20) {
                                    label = s;
                                    break;
                                }
                            }
                        }
                        if (!label.empty()) break;
                    }
                }
            }

            if (!label.empty()) {
                labels.push_back(label);
                menuBarItemMorphs_.push_back(item);  // Store morph for dropdown access
                if (logFile && renderCallCount <= 10) {
                    fprintf(logFile, "[MENUBAR] Item %zu label: '%s'\n", i, label.c_str());
                    fflush(logFile);
                }
            } else {
                if (logFile && renderCallCount <= 10) {
                    fprintf(logFile, "[MENUBAR] Item %zu: no label found\n", i);
                    fflush(logFile);
                }
            }
        }

        if (logFile && renderCallCount <= 10) {
            fprintf(logFile, "[MENUBAR] Total labels: %zu\n", labels.size());
            fflush(logFile);
        }

        // Draw menu item labels with proper spacing
        // Note: On Retina, we're drawing directly to a pixel buffer at 2x physical resolution
        // The scaledMenuBarHeight is already in physical pixels
        bool isRetina = dispWidth > 1500;
        // Use a large enough font that it's clearly visible in the menu bar
        int fontSize = isRetina ? 48 : 24;  // ~24pt appearance on Retina for better readability
        int textX = isRetina ? 32 : 16;  // Starting x position with padding
        // Center the x-height (lowercase letters without ascenders/descenders) vertically
        // Text buffer: baseline is at 0.95*fontSize from top, x-height center is ~0.70*fontSize from top
        int menuBarCenter = titleBarOffset + scaledMenuBarHeight / 2;
        int xHeightCenterInBuffer = static_cast<int>(fontSize * 0.70);
        int textY = menuBarCenter - xHeightCenterInBuffer;
        int itemSpacing = isRetina ? 56 : 28;  // Space between items
        int charWidth = isRetina ? 26 : 13;  // Approximate char width for larger font

        if (logFile && renderCallCount <= 3) {
            fprintf(logFile, "[MENUBAR] Drawing text at y=%d (offset=%d), scaledMenuBarHeight=%d, isRetina=%d, fontSize=%d\n",
                    textY, titleBarOffset, scaledMenuBarHeight, isRetina ? 1 : 0, fontSize);
            fflush(logFile);
        }

        // Clear and rebuild menu item bounds
        menuItemBounds_.clear();

        int itemIndex = 0;
        for (const std::string& label : labels) {
            if (logFile && renderCallCount <= 3) {
                fprintf(logFile, "[MENUBAR] drawText('%s') at x=%d y=%d fontSize=%d\n", label.c_str(), textX, textY, fontSize);
                fflush(logFile);
            }
            int itemWidth = static_cast<int>(label.length()) * charWidth;
            int itemPadding = isRetina ? 16 : 8;

            // Highlight selected menu item
            if (selectedMenuIndex_ == itemIndex) {
                // Draw highlight background (darker blue)
                uint32_t highlightColor = 0xFF3478F6;  // macOS blue selection color
                for (int hy = titleBarOffset; hy < titleBarOffset + scaledMenuBarHeight - 1; hy++) {
                    for (int hx = textX - itemPadding; hx < textX + itemWidth + itemPadding && hx < dispWidth; hx++) {
                        if (hx >= 0 && hy >= 0 && hy < dispHeight) {
                            pixels[hy * dispWidth + hx] = highlightColor;
                        }
                    }
                }
                // Use white text on blue background
                drawText(pixels, dispWidth, dispHeight, textX, textY, label, 0xFFFFFFFF, fontSize);
            } else {
                drawText(pixels, dispWidth, dispHeight, textX, textY, label, textColor, fontSize);
            }

            // Store bounds for click detection (in pixels) - include padding
            menuItemBounds_.push_back({textX - itemPadding, textX + itemWidth + itemPadding});
            textX += itemWidth + itemSpacing;
            itemIndex++;
        }

        // Store menu bar bounds for click detection
        menuBarTop_ = titleBarOffset;
        menuBarBottom_ = titleBarOffset + scaledMenuBarHeight;
        menuBarScale_ = isRetina ? 2 : 1;

        // Render dropdown menu if a menu is selected
        if (logFile) {
            fprintf(logFile, "[MENUBAR] selectedMenuIndex_=%d menuBarItemMorphs_.size()=%zu\n",
                    selectedMenuIndex_, menuBarItemMorphs_.size());
            fflush(logFile);
        }
        if (selectedMenuIndex_ >= 0 && selectedMenuIndex_ < static_cast<int>(menuBarItemMorphs_.size())) {
            Oop selectedItem = menuBarItemMorphs_[selectedMenuIndex_];

            // Find the menu associated with this menu bar item
            // In Pharo, the menu is typically in slot 6 or nearby slots
            Oop menuMorph = Oop::nil();
            ObjectHeader* itemHdr = selectedItem.asObjectPtr();

            // Look for MenuMorph in slots
            for (size_t slot = 5; slot < std::min((size_t)20, itemHdr->slotCount()); slot++) {
                Oop slotVal = memory_.fetchPointer(slot, selectedItem);
                if (slotVal.isNil() || !slotVal.isObject()) continue;

                // Check if it's a MenuMorph
                Oop slotClass = memory_.classOf(slotVal);
                if (slotClass.isObject()) {
                    ObjectHeader* classHdr = slotClass.asObjectPtr();
                    if (classHdr->slotCount() > 6) {
                        Oop className = memory_.fetchPointer(6, slotClass);
                        if (className.isObject()) {
                            ObjectHeader* nameHdr = className.asObjectPtr();
                            if (nameHdr->isBytesObject() && nameHdr->byteSize() < 50) {
                                std::string cn((char*)nameHdr->bytes(), nameHdr->byteSize());
                                if (cn.find("Menu") != std::string::npos) {
                                    menuMorph = slotVal;
                                    if (logFile) {
                                        fprintf(logFile, "[DROPDOWN] Found menu in slot %zu: %s\n", slot, cn.c_str());
                                        fflush(logFile);
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
            }

            // Collect dropdown item labels
            std::vector<std::string> dropdownLabels;
            std::vector<Oop> dropdownItemMorphs;

            if (!menuMorph.isNil() && menuMorph.isObject()) {
                // Get submorphs from the menu
                Oop menuSubmorphs = memory_.fetchPointer(2, menuMorph);
                if (!menuSubmorphs.isNil() && menuSubmorphs.isObject()) {
                    ObjectHeader* msHdr = menuSubmorphs.asObjectPtr();
                    size_t numMenuItems = msHdr->slotCount();

                    if (logFile) {
                        fprintf(logFile, "[DROPDOWN] Menu has %zu items\n", numMenuItems);
                        fflush(logFile);
                    }

                    for (size_t mi = 0; mi < numMenuItems; mi++) {
                        Oop menuItem = msHdr->slotAt(mi);
                        if (menuItem.isNil() || !menuItem.isObject()) continue;

                        // Extract label from menu item using same logic as menu bar
                        std::string itemLabel;
                        ObjectHeader* miHdr = menuItem.asObjectPtr();

                        // Check slots for string label
                        for (size_t slot = 5; slot < std::min((size_t)15, miHdr->slotCount()); slot++) {
                            Oop slotVal = memory_.fetchPointer(slot, menuItem);
                            if (slotVal.isNil() || !slotVal.isObject()) continue;

                            ObjectHeader* slotHdr = slotVal.asObjectPtr();
                            if (slotHdr->isBytesObject() && slotHdr->byteSize() > 0 && slotHdr->byteSize() < 100) {
                                std::string s((char*)slotHdr->bytes(), slotHdr->byteSize());
                                bool valid = true;
                                for (char c : s) {
                                    if (c < 32 || c > 126) { valid = false; break; }
                                }
                                if (valid && s.length() > 0) {
                                    itemLabel = s;
                                    break;
                                }
                            }
                        }

                        // Also check submorphs for label
                        if (itemLabel.empty()) {
                            Oop itemSubmorphs = memory_.fetchPointer(2, menuItem);
                            if (!itemSubmorphs.isNil() && itemSubmorphs.isObject()) {
                                ObjectHeader* isHdr = itemSubmorphs.asObjectPtr();
                                for (size_t j = 0; j < isHdr->slotCount() && itemLabel.empty(); j++) {
                                    Oop subm = isHdr->slotAt(j);
                                    if (subm.isNil() || !subm.isObject()) continue;

                                    ObjectHeader* submHdr = subm.asObjectPtr();
                                    for (size_t slot = 5; slot < std::min((size_t)15, submHdr->slotCount()); slot++) {
                                        Oop slotVal = memory_.fetchPointer(slot, subm);
                                        if (slotVal.isNil() || !slotVal.isObject()) continue;

                                        ObjectHeader* svHdr = slotVal.asObjectPtr();
                                        if (svHdr->isBytesObject() && svHdr->byteSize() > 0 && svHdr->byteSize() < 100) {
                                            std::string s((char*)svHdr->bytes(), svHdr->byteSize());
                                            bool valid = true;
                                            for (char c : s) {
                                                if (c < 32 || c > 126) { valid = false; break; }
                                            }
                                            if (valid && s.length() > 0) {
                                                itemLabel = s;
                                                break;
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        if (!itemLabel.empty()) {
                            dropdownLabels.push_back(itemLabel);
                            dropdownItemMorphs.push_back(menuItem);
                            if (logFile) {
                                fprintf(logFile, "[DROPDOWN] Item %zu: '%s'\n", mi, itemLabel.c_str());
                                fflush(logFile);
                            }
                        }
                    }
                }
            }

            // Draw dropdown if we have items
            if (logFile) {
                fprintf(logFile, "[DROPDOWN] dropdownLabels.size()=%zu\n", dropdownLabels.size());
                for (const auto& lbl : dropdownLabels) {
                    fprintf(logFile, "[DROPDOWN]   item: '%s'\n", lbl.c_str());
                }
                fflush(logFile);
            }
            if (!dropdownLabels.empty()) {
                // Calculate dropdown dimensions
                int dropdownFontSize = fontSize;
                int lineHeight = static_cast<int>(dropdownFontSize * 1.5);
                int dropdownPadding = isRetina ? 16 : 8;

                // Find max label width
                int maxLabelWidth = 0;
                for (const auto& lbl : dropdownLabels) {
                    int w = static_cast<int>(lbl.length()) * charWidth;
                    if (w > maxLabelWidth) maxLabelWidth = w;
                }

                int dropdownWidth = maxLabelWidth + dropdownPadding * 4;
                int dropdownHeight = static_cast<int>(dropdownLabels.size()) * lineHeight + dropdownPadding * 2;

                // Position dropdown below the selected menu item
                int dropdownX = (selectedMenuIndex_ < static_cast<int>(menuItemBounds_.size()))
                    ? menuItemBounds_[selectedMenuIndex_].first : textX;
                int dropdownY = titleBarOffset + scaledMenuBarHeight;

                // Draw dropdown background (white with subtle shadow)
                uint32_t bgColor = 0xFFFAFAFA;
                uint32_t borderColor = 0xFFCCCCCC;
                uint32_t shadowColor = 0x20000000;  // Semi-transparent black

                // Draw shadow (offset by 2 pixels)
                for (int sy = dropdownY + 2; sy < dropdownY + dropdownHeight + 4 && sy < dispHeight; sy++) {
                    for (int sx = dropdownX + 2; sx < dropdownX + dropdownWidth + 4 && sx < dispWidth; sx++) {
                        if (sx >= 0 && sy >= 0) {
                            pixels[sy * dispWidth + sx] = shadowColor;
                        }
                    }
                }

                // Draw background
                for (int dy = dropdownY; dy < dropdownY + dropdownHeight && dy < dispHeight; dy++) {
                    for (int dx = dropdownX; dx < dropdownX + dropdownWidth && dx < dispWidth; dx++) {
                        if (dx >= 0 && dy >= 0) {
                            pixels[dy * dispWidth + dx] = bgColor;
                        }
                    }
                }

                // Draw border
                for (int dx = dropdownX; dx < dropdownX + dropdownWidth && dx < dispWidth; dx++) {
                    if (dx >= 0) {
                        if (dropdownY >= 0 && dropdownY < dispHeight)
                            pixels[dropdownY * dispWidth + dx] = borderColor;
                        int bottomY = dropdownY + dropdownHeight - 1;
                        if (bottomY >= 0 && bottomY < dispHeight)
                            pixels[bottomY * dispWidth + dx] = borderColor;
                    }
                }
                for (int dy = dropdownY; dy < dropdownY + dropdownHeight && dy < dispHeight; dy++) {
                    if (dy >= 0) {
                        if (dropdownX >= 0 && dropdownX < dispWidth)
                            pixels[dy * dispWidth + dropdownX] = borderColor;
                        int rightX = dropdownX + dropdownWidth - 1;
                        if (rightX >= 0 && rightX < dispWidth)
                            pixels[dy * dispWidth + rightX] = borderColor;
                    }
                }

                // Draw menu item labels
                int itemY = dropdownY + dropdownPadding;
                for (size_t di = 0; di < dropdownLabels.size(); di++) {
                    drawText(pixels, dispWidth, dispHeight,
                             dropdownX + dropdownPadding * 2, itemY,
                             dropdownLabels[di], textColor, dropdownFontSize);
                    itemY += lineHeight;
                }

                // Store dropdown state for click handling
                dropdownState_.x = dropdownX;
                dropdownState_.y = dropdownY;
                dropdownState_.width = dropdownWidth;
                dropdownState_.height = dropdownHeight;
                dropdownState_.lineHeight = lineHeight;
                dropdownState_.itemMorphs = dropdownItemMorphs;
                dropdownState_.valid = true;
                dropdownState_.openTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                if (logFile) {
                    fprintf(logFile, "[DROPDOWN-SET] x=%d y=%d w=%d h=%d items=%zu VALID=TRUE openTime=%lld\n",
                            dropdownX, dropdownY, dropdownWidth, dropdownHeight,
                            dropdownItemMorphs.size(), dropdownState_.openTimeMs);
                    fflush(logFile);
                }
            } else {
                if (logFile) {
                    fprintf(logFile, "[DROPDOWN-CLEAR] dropdownLabels empty, setting valid=false\n");
                    fflush(logFile);
                }
                dropdownState_.valid = false;
            }
        } else {
            if (logFile) {
                fprintf(logFile, "[DROPDOWN-CLEAR] selectedMenuIndex_=%d menuBarItemMorphs_.size()=%zu, setting valid=false\n",
                        selectedMenuIndex_, menuBarItemMorphs_.size());
                fflush(logFile);
            }
            dropdownState_.valid = false;
        }

        totalMorphsDrawn++;
    };

    // Recursive morph rendering function
    std::function<void(Oop, int, int)> renderMorph = [&](Oop morph, int depth, int index) {
        if (morph.isNil() || !morph.isObject()) return;
        if (depth > 20) return;  // Prevent infinite recursion

        // Check for special morphs that need custom rendering
        std::string className = getMorphClassName(morph);

        // Log all morphs in first render to find "World Menu" button
        if (logFile && renderCallCount == 1 && depth <= 6) {
            std::string indent(depth * 2, ' ');
            fprintf(logFile, "[MORPH-TREE] %s[%d] %s\n", indent.c_str(), index, className.c_str());
            fflush(logFile);
        }

        // Handle MenubarMorph specially - draw with text
        if (className == "MenubarMorph") {
            renderMenuBar(morph, 44);  // 44 pixel height (88 on Retina)
            return;  // Don't recurse into menu bar submorphs
        }

        // Skip TaskbarMorph for now (just draw background)
        if (className == "TaskbarMorph") {
            // Draw simple gray taskbar
            int taskbarHeight = 40;
            int taskbarY = dispHeight - taskbarHeight;
            uint32_t taskbarColor = 0xFF2A2A2A;
            for (int y = taskbarY; y < dispHeight; y++) {
                for (int x = 0; x < dispWidth; x++) {
                    pixels[y * dispWidth + x] = taskbarColor;
                }
            }
            totalMorphsDrawn++;
            return;
        }

        totalMorphsDrawn++;

        // Get bounds
        int x1, y1, x2, y2;
        if (!extractBounds(morph, x1, y1, x2, y2)) return;

        // Get color
        Oop morphColor = memory_.fetchPointer(4, morph);
        uint32_t colorARGB = extractColor(morphColor);

        // Skip black morphs (they're likely text areas or transparent)
        // This is a temporary workaround until text rendering is implemented
        if ((colorARGB & 0xFFFFFF) == 0) return;  // Skip pure black

        // Skip if bounds are invalid or too small (but don't skip for depth > 0)
        bool hasBounds = (x2 - x1 >= 1 && y2 - y1 >= 1);

        // Clamp to display bounds
        int cx1 = std::max(0, std::min(x1, dispWidth));
        int cy1 = std::max(0, std::min(y1, dispHeight));
        int cx2 = std::max(0, std::min(x2, dispWidth));
        int cy2 = std::max(0, std::min(y2, dispHeight));

        // Draw filled rectangle if we have valid bounds
        if (hasBounds && cx2 > cx1 && cy2 > cy1) {
            for (int y = cy1; y < cy2; y++) {
                for (int x = cx1; x < cx2; x++) {
                    pixels[y * dispWidth + x] = colorARGB;
                }
            }

            // Draw a 1-pixel darker border for visibility
            uint32_t borderColor = ((colorARGB & 0xFF000000)) |
                                   (((colorARGB >> 16) & 0xFF) * 3 / 4 << 16) |
                                   (((colorARGB >> 8) & 0xFF) * 3 / 4 << 8) |
                                   ((colorARGB & 0xFF) * 3 / 4);

            // Top and bottom borders
            for (int x = cx1; x < cx2; x++) {
                if (cy1 < dispHeight) pixels[cy1 * dispWidth + x] = borderColor;
                if (cy2 - 1 >= 0 && cy2 - 1 < dispHeight) pixels[(cy2 - 1) * dispWidth + x] = borderColor;
            }
            // Left and right borders
            for (int y = cy1; y < cy2; y++) {
                if (cx1 < dispWidth) pixels[y * dispWidth + cx1] = borderColor;
                if (cx2 - 1 >= 0 && cx2 - 1 < dispWidth) pixels[y * dispWidth + cx2 - 1] = borderColor;
            }
        }

        // Recursively render submorphs
        Oop submorphs = memory_.fetchPointer(2, morph);
        if (submorphs.isNil() || !submorphs.isObject()) return;

        ObjectHeader* subHdr = submorphs.asObjectPtr();
        size_t numSubmorphs = subHdr->slotCount();

        for (size_t i = 0; i < numSubmorphs; i++) {
            Oop submorph = subHdr->slotAt(i);
            renderMorph(submorph, depth + 1, static_cast<int>(i));
        }
    };

    // Clear to World's color first
    Oop worldColor = memory_.fetchPointer(4, world);
    uint32_t worldColorARGB = extractColor(worldColor);

    if (logFile && renderCallCount <= 10) {
        fprintf(logFile, "[RENDER #%d] World color = 0x%08x, display %dx%d\n",
                renderCallCount, worldColorARGB, dispWidth, dispHeight);
        fflush(logFile);
    }

    for (int i = 0; i < dispWidth * dispHeight; i++) {
        pixels[i] = worldColorARGB;
    }

    totalMorphsDrawn = 0;

    // Get World's submorphs and render recursively
    Oop submorphs = memory_.fetchPointer(2, world);
    if (!submorphs.isNil() && submorphs.isObject()) {
        ObjectHeader* subHdr = submorphs.asObjectPtr();
        size_t numSubmorphs = subHdr->slotCount();

        if (logFile && renderCallCount <= 10) {
            fprintf(logFile, "[RENDER #%d] World has %zu submorphs\n", renderCallCount, numSubmorphs);
            fflush(logFile);
        }

        // First pass: render all submorphs EXCEPT MenubarMorph (render it last so it's on top)
        Oop menubarMorph = Oop::nil();
        for (size_t i = 0; i < numSubmorphs; i++) {
            Oop submorph = subHdr->slotAt(i);
            std::string cn = getMorphClassName(submorph);
            if (logFile && renderCallCount <= 10 && i < 5) {
                fprintf(logFile, "[RENDER #%d]   submorph[%zu] = %s\n", renderCallCount, i, cn.c_str());
                fflush(logFile);
            }
            if (cn == "MenubarMorph") {
                menubarMorph = submorph;  // Save for later, render last
            } else {
                renderMorph(submorph, 0, static_cast<int>(i));
            }
        }

        // Second pass: render MenubarMorph last so it's always on top
        if (!menubarMorph.isNil()) {
            renderMorph(menubarMorph, 0, 0);
        }
    } else {
        if (logFile && renderCallCount <= 10) {
            fprintf(logFile, "[RENDER #%d] No submorphs found\n", renderCallCount);
            fflush(logFile);
        }
    }

    if (logFile && renderCallCount <= 10) {
        fprintf(logFile, "[RENDER #%d] Done, drew %d morphs\n", renderCallCount, totalMorphsDrawn);
        fflush(logFile);
    }

    // Debug test - write to doOneCycle_debug.log from renderWorldMorphs
    if constexpr (ENABLE_DEBUG_LOGGING) {
        static FILE* testLog = nullptr;
        if (!testLog) {
            testLog = nullptr;
        }
        if (testLog) {
            fprintf(testLog, "[RENDER-END #%d] renderWorldMorphs completed\n", renderCallCount);
            fflush(testLog);
        }
    }

    // Redraw world menu if visible (placeholder menu from right-click)
    if (hasVisibleMenu_) {
        int mx = pendingMenuBounds_.x;
        int my = pendingMenuBounds_.y;
        int mw = pendingMenuBounds_.width;
        int mh = pendingMenuBounds_.height;
        int surfWidth = pharo::gDisplaySurface->width();
        int surfHeight = pharo::gDisplaySurface->height();
        int pitchPixels = static_cast<int>(pharo::gDisplaySurface->pitch() / 4);

        // Draw white background
        for (int dy = 0; dy < mh && my + dy < surfHeight; dy++) {
            for (int dx = 0; dx < mw && mx + dx < surfWidth; dx++) {
                int px = mx + dx;
                int py = my + dy;
                if (px >= 0 && py >= 0) {
                    pixels[py * pitchPixels + px] = 0xFFFFFFFF;  // White
                }
            }
        }

        // Draw gray border (2 pixels)
        uint32_t borderColor = 0xFF808080;
        for (int dx = 0; dx < mw; dx++) {
            for (int t = 0; t < 2; t++) {
                int px = mx + dx;
                int py1 = my + t;
                int py2 = my + mh - 1 - t;
                if (px >= 0 && px < surfWidth) {
                    if (py1 >= 0 && py1 < surfHeight) pixels[py1 * pitchPixels + px] = borderColor;
                    if (py2 >= 0 && py2 < surfHeight) pixels[py2 * pitchPixels + px] = borderColor;
                }
            }
        }
        for (int dy = 0; dy < mh; dy++) {
            for (int t = 0; t < 2; t++) {
                int px1 = mx + t;
                int px2 = mx + mw - 1 - t;
                int py = my + dy;
                if (py >= 0 && py < surfHeight) {
                    if (px1 >= 0 && px1 < surfWidth) pixels[py * pitchPixels + px1] = borderColor;
                    if (px2 >= 0 && px2 < surfWidth) pixels[py * pitchPixels + px2] = borderColor;
                }
            }
        }

        // Draw menu item lines (dashes to simulate text)
        uint32_t textColor = 0xFF000000;
        int itemHeight = 30;
        int textY = my + 10;
        for (int i = 0; i < 5 && textY + 20 < my + mh; i++) {
            int lineY = textY + 10;
            for (int dx = 10; dx < mw - 10; dx++) {
                int px = mx + dx;
                if (px >= 0 && px < surfWidth && lineY >= 0 && lineY < surfHeight) {
                    if ((dx % 8) < 5) {
                        pixels[lineY * pitchPixels + px] = textColor;
                    }
                }
            }
            textY += itemHeight;
        }
    }

    pharo::gDisplaySurface->update();
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

    static FILE* logFile = nullptr;
    static int callCount = 0;
    static bool initialized = false;

    if constexpr (ENABLE_DEBUG_LOGGING) {
        if (!initialized) {
            initialized = true;
            logFile = nullptr;
        }

        // Log periodically
        if (++callCount % 1000 == 1 && logFile) {
            fprintf(logFile, "[PROCESS-INPUT] call #%d passthrough=%zu queue=%s sdl2Active=%d\n",
                    callCount, passThroughEvents_.size(),
                    pharo::gEventQueue.isEmpty() ? "empty" : "has events",
                    pharo::gEventQueue.isSDL2EventPollingActive() ? 1 : 0);
            fflush(logFile);
        }
    }

    // When SDL event polling is active, let OSSDL2Driver handle events via SDL_PollEvent
    if (pharo::gEventQueue.isSDL2EventPollingActive()) return;

    // Debug: Log queue address once for comparison with push address
    static bool loggedQueueAddr = false;
    if (!loggedQueueAddr && logFile) {
        loggedQueueAddr = true;
        fprintf(logFile, "[PROCESS-INPUT] queue addr=%p empty=%d\n",
                (void*)&pharo::gEventQueue, pharo::gEventQueue.isEmpty() ? 1 : 0);
        fflush(logFile);
    }

    pharo::Event event;
    while (pharo::gEventQueue.pop(event)) {
        // Skip WindowMetrics events - internal to C++ rendering
        if (event.type == static_cast<int>(pharo::EventType::WindowMetrics)) {
            continue;
        }
        // Log all mouse events to stderr for debugging
        if (event.type == static_cast<int>(pharo::EventType::Mouse)) {
            static int mouseEventCount = 0;
            mouseEventCount++;
            if (mouseEventCount <= 20) {
                fprintf(stderr, "[DRAIN-EVT] Mouse #%d: subtype=%d at %d,%d buttons=%d semaIdx=%d\n",
                        mouseEventCount, event.arg5, event.arg1, event.arg2, event.arg3,
                        pharo::gEventQueue.getInputSemaphoreIndex());
            }
        }

        // Track mouse position for direct hand updates (backup if event system not working)
        if (event.type == static_cast<int>(pharo::EventType::Mouse)) {
            lastMouseX_ = event.arg1 * menuBarScale_;
            lastMouseY_ = event.arg2 * menuBarScale_;
            lastMouseButtons_ = event.arg3;
            lastMouseEventType_ = event.arg5;
        }

        // All events go to Pharo via passThroughEvents_ (consumed by primitive 264)
        passThroughEvents_.push_back(event);

        // Signal the input semaphore to wake up Smalltalk's event loop
        int inputSemaIdx = pharo::gEventQueue.getInputSemaphoreIndex();
        static int semaSignalCount = 0;
        if (inputSemaIdx > 0) {
            semaSignalCount++;
            if (logFile && semaSignalCount <= 10) {
                fprintf(logFile, "[SEMA] Signaling input semaphore index=%d (signal #%d)\n",
                        inputSemaIdx, semaSignalCount);
                fflush(logFile);
            }
            signalExternalSemaphore(inputSemaIdx);
        } else {
            static bool loggedNoSema = false;
            if (logFile && !loggedNoSema) {
                loggedNoSema = true;
                fprintf(logFile, "[SEMA] Input semaphore index is %d - no signaling\n", inputSemaIdx);
                fflush(logFile);
            }
        }

        // Events are now in passThroughEvents_ and the input semaphore has been signaled.
        // The Smalltalk InputEventSensor process should wake up and process events via primitive 264.
        // NO WORKAROUNDS: Do not manipulate HandMorph slots directly from C++.
        // If events aren't being processed, debug why InputEventSensor isn't running.
    }
}

// NO WORKAROUNDS: The following C++ event dispatch functions were removed:
// - dispatchMouseEventToMorph (C++ hit testing bypassed InputEventSensor)
// - handleMenuBarClick (C++ menu bar click handling)
// - handleWorldMenuClick (C++ world menu invocation)
// - executeMenuItemAction (C++ menu action execution)
// - processPendingMenuAction (C++ pending action processing)
// - processPendingWorldMenu (C++ world menu processing)
// - drawClickIndicator (C++ visual click feedback)
// - lookupMethodByName (helper for C++ method lookup)
//
// Events must be handled by Smalltalk's InputEventSensor and Morphic.
// If events aren't working, fix InputEventSensor startup - don't add C++ workarounds.

// NOTE: updateActiveHandPosition() REMOVED - was a workaround for missing InputEventSensor process
// Events must go through proper Smalltalk InputEventSensor, not C++ workarounds

// ===== DISPLAY SYNCHRONIZATION =====
// Until BitBlt primitives are fully working, bypass Display Form and
// render World morphs directly.

void Interpreter::syncDisplayToSurface() {
    if (!pharo::gDisplaySurface) return;

    // Process input events - queued for Smalltalk via primitive 264
    processInputEvents();

    // When SDL2 rendering is active, skip the Display Form copy.
    // SDL_RenderPresent copies SDL2 content to gDisplaySurface;
    // don't overwrite it with stale Display Form data.
    if (ffi_isSDLRenderingActive()) {
        return;
    }

    // displayForm_ is set during startup or by primitiveBeDisplay (prim 102).
    if (displayForm_.isNil()) {
        renderWorldMorphs();
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
        for (int y = 0; y < copyHeight; y++) {
            for (int x = 0; x < copyWidth; x++) {
                dstPixels[y * dstWidth + x] = srcPixels[y * srcWidth + x];
            }
        }
    }

    pharo::gDisplaySurface->update();
}

// ===== MAIN LOOP =====

void Interpreter::stopVM(const char* reason) {
    fprintf(stderr, "[VM-STOP step=%llu] %s\n", (unsigned long long)g_stepNum, reason);
    running_ = false;
}

void Interpreter::dumpCurrentMethod() {
    fprintf(stderr, "\n=== CURRENT METHOD (frameDepth=%zu) ===\n", frameDepth_);
    auto getSel = [&](Oop method) -> std::string {
        if (!method.isObject() || method.rawBits() < 0x10000) return "?";
        ObjectHeader* mHdr = method.asObjectPtr();
        if (!mHdr->isCompiledMethod()) return "?";
        Oop hdr = memory_.fetchPointer(0, method);
        if (!hdr.isSmallInteger()) return "?";
        size_t numLits = hdr.asSmallInteger() & 0x7FFF;
        if (numLits < 2 || numLits >= 100) return "?";
        Oop sel = memory_.fetchPointer(numLits - 1, method);
        if (!sel.isObject() || sel.rawBits() < 0x10000) return "?";
        ObjectHeader* selHdr = sel.asObjectPtr();
        if (!selHdr->isBytesObject() || selHdr->byteSize() >= 100) return "?";
        return std::string((char*)selHdr->bytes(), selHdr->byteSize());
    };
    // Current method
    fprintf(stderr, "  [current] #%s\n", getSel(method_).c_str());
    int count = 0;
    for (int f = static_cast<int>(frameDepth_); f >= 0 && count < 10; f--, count++) {
        fprintf(stderr, "  [%d] #%s\n", f, getSel(savedFrames_[f].savedMethod).c_str());
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
        // Dump active process's context chain (first 40 contexts)
        fprintf(stderr, "  Active process context chain:\n");
        Oop chainCtx = activeContext_;
        for (int ci = 0; ci < 40 && chainCtx.isObject() && chainCtx.rawBits() > 0x10000; ci++) {
            ObjectHeader* chdr = chainCtx.asObjectPtr();
            if (chdr->slotCount() < 6) break;
            Oop chainMeth = memory_.fetchPointer(3, chainCtx);
            std::string cmn = "?";
            if (chainMeth.isObject() && chainMeth.rawBits() > 0x10000) {
                Oop hdr = memory_.fetchPointer(0, chainMeth);
                if (hdr.isSmallInteger()) {
                    int nl = hdr.asSmallInteger() & 0x7FFF;
                    if (nl >= 2 && nl < 100) {
                        Oop sel = memory_.fetchPointer(nl - 1, chainMeth);
                        if (sel.isObject() && sel.rawBits() > 0x10000) {
                            ObjectHeader* sh = sel.asObjectPtr();
                            if (sh->isBytesObject() && sh->byteSize() < 50)
                                cmn = std::string((char*)sh->bytes(), sh->byteSize());
                        }
                    }
                }
            }
            Oop sender = memory_.fetchPointer(0, chainCtx);
            fprintf(stderr, "    [%d] ctx=0x%llx method=#%s\n", ci,
                    (unsigned long long)chainCtx.rawBits(), cmn.c_str());
            chainCtx = sender;
        }
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
                    if (meth.isObject() && meth.rawBits() > 0x10000) {
                        Oop hdr = memory_.fetchPointer(0, meth);
                        if (hdr.isSmallInteger()) {
                            int nl = hdr.asSmallInteger() & 0x7FFF;
                            if (nl >= 2 && nl < 100) {
                                Oop sel = memory_.fetchPointer(nl - 1, meth);
                                if (sel.isObject() && sel.rawBits() > 0x10000) {
                                    ObjectHeader* sh = sel.asObjectPtr();
                                    if (sh->isBytesObject() && sh->byteSize() < 50)
                                        mname = std::string((char*)sh->bytes(), sh->byteSize());
                                }
                            }
                        }
                    }
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
    { FILE* f = nullptr; if (f) { fprintf(f, "[INTERPRET] entered interpret() running_=%d\n", running_ ? 1 : 0); fclose(f); } }
    // Debug: Log special object addresses once at start
    if constexpr (ENABLE_DEBUG_LOGGING) {
        static bool loggedSpecialObjects = false;
        if (!loggedSpecialObjects) {
            loggedSpecialObjects = true;
            FILE* soLog = nullptr;
            if (soLog) {
                Oop trueObj = memory_.trueObject();
                Oop falseObj = memory_.falseObject();
                Oop nilObj = memory_.nil();
                fprintf(soLog, "true:  0x%llx\n", (unsigned long long)trueObj.rawBits());
                fprintf(soLog, "false: 0x%llx\n", (unsigned long long)falseObj.rawBits());
                fprintf(soLog, "nil:   0x%llx\n", (unsigned long long)nilObj.rawBits());
                fflush(soLog);
                fclose(soLog);
            }
        }
    }

    int loopCount = 0;
#if __APPLE__
    auto lastRunLoopPump = std::chrono::steady_clock::now();
#endif
    while (running_) {
        // Execute a batch of bytecodes before checking overhead
        // This dramatically reduces the per-bytecode cost of timer/event checks
        g_watchdogPhase.store(1, std::memory_order_relaxed);  // in step()
        for (int batch = 0; batch < 1000 && running_; batch++) {
            step();
        }
        g_watchdogPhase.store(0, std::memory_order_relaxed);  // between batches
        loopCount += 1000;
        g_watchdogSteps.store(loopCount, std::memory_order_relaxed);

        // Update watchdog with current process priority (every 100K steps)
        if (loopCount % 100000 == 0) {
            Oop proc = getActiveProcess();
            if (proc.isObject() && proc.rawBits() > 0x10000) {
                Oop priOop = memory_.fetchPointer(ProcessPriorityIndex, proc);
                int pri = priOop.isSmallInteger() ? priOop.asSmallInteger() : -1;
                g_watchdogProcessPriority = pri;

                // Priority histogram: sample and report every 10M steps
                static int priHist[81] = {0};  // priorities 0-80
                if (pri >= 0 && pri <= 80) priHist[pri]++;
                if (loopCount % 10000000 == 0) {
                    fprintf(stderr, "[PRI-HIST] step=%lldM: ", (long long)loopCount / 1000000);
                    for (int p = 80; p >= 1; p--) {
                        if (priHist[p] > 0) {
                            fprintf(stderr, "p%d=%d ", p, priHist[p]);
                        }
                    }
                    fprintf(stderr, "\n");
                    memset(priHist, 0, sizeof(priHist));
                }
            }
        }

        // Process any pending external semaphore signals
        if (hasPendingSignals()) {
            processPendingSignals();
        }

        // Check timer every 1000 bytecodes (~45K checks/sec at full speed)
        // to avoid calling system_clock::now() on every bytecode.
        if ((loopCount & 0x3FF) == 0) {  // every 1024 iterations
            checkTimerSemaphore();
        }

        // Process input events every 10K bytecodes
        if (loopCount % 10000 == 0) {
            processInputEvents();
        }

#if __APPLE__
        // Pump the native run loop periodically (~60fps) so Metal can
        // render and UIKit can deliver events. Check only every 10K
        // bytecodes to avoid steady_clock::now() overhead per bytecode.
        if (relinquishCallback_ && (loopCount % 10000 == 0)) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastRunLoopPump).count();
            if (elapsed >= 16) {
                lastRunLoopPump = now;
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, true);
            }
        }
#endif

        // Progress reporting every 10M steps
        if (loopCount % 10000000 == 0) {
            fprintf(stderr, "[PROGRESS] %dM steps\n", loopCount / 1000000);
            fflush(stderr);
        }

        // Periodically look for Display Form
        if (displayForm_.isNil() && (loopCount % 100000 == 0)) {
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
}

void Interpreter::checkTimerSemaphore() {
    // Periodic timer state summary (every ~5 seconds wall clock)
    // Use counter-based gating: check wall clock only every 200 calls
    // (~5 times/sec at 1024-bytecode intervals) to avoid clock overhead.
    static int checkCount = 0;
    static int usecFireTotal = 0;
    static int msFireTotal = 0;
    static auto lastSummary = std::chrono::steady_clock::now();
    checkCount++;
    bool doSummary = false;
    if ((checkCount % 200) == 0) {
        auto now_wall = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now_wall - lastSummary).count();
        if (elapsed >= 5) {
            doSummary = true;
            lastSummary = now_wall;
        }
    }
    if (doSummary) {
        // Check active process priority
        int activePri = -1;
        Oop activeProc = getActiveProcess();
        if (activeProc.isObject()) {
            Oop priOop = memory_.fetchPointer(ProcessPriorityIndex, activeProc);
            if (priOop.isSmallInteger()) activePri = (int)priOop.asSmallInteger();
        }
        // Count ready processes per priority
        int readyCounts[101] = {};
        Oop schedAssoc = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);
        if (schedAssoc.isObject()) {
            Oop processScheduler = memory_.fetchPointer(1, schedAssoc);
            if (processScheduler.isObject()) {
                Oop schedLists = memory_.fetchPointer(SchedulerProcessListsIndex, processScheduler);
                if (schedLists.isObject()) {
                    ObjectHeader* listsHdr = schedLists.asObjectPtr();
                    size_t nLists = listsHdr->slotCount();
                    for (size_t i = 0; i < nLists && i < 100; i++) {
                        Oop list = memory_.fetchPointer(i, schedLists);
                        if (!list.isObject()) continue;
                        Oop first = memory_.fetchPointer(LinkedListFirstLinkIndex, list);
                        int count = 0;
                        Oop proc = first;
                        while (proc.isObject() && !proc.isNil() && count < 50) {
                            count++;
                            proc = memory_.fetchPointer(ProcessNextLinkIndex, proc);
                        }
                        readyCounts[i] = count;
                    }
                }
            }
        }
        fprintf(stderr, "[TIMER-SUMMARY] checks=%d usecFires=%d msFires=%d "
                "usecArmed=%s msArmed=%s timerSem=%s activePri=%d step=%llu\n",
                checkCount, usecFireTotal, msFireTotal,
                nextWakeupUsec_ != INT64_MAX ? "yes" : "no",
                nextWakeupTime_ != 0 ? "yes" : "no",
                timerSemaphore_.isNil() ? "nil" : "set",
                activePri,
                (unsigned long long)g_stepNum);
        // Show non-zero ready queues
        for (int i = 0; i < 100; i++) {
            if (readyCounts[i] > 0) {
                fprintf(stderr, "  [READY] pri=%d count=%d\n", i+1, readyCounts[i]);
            }
        }
        // Show what the active process is doing
        fprintf(stderr, "  [ACTIVE] sel=%s cls=%s fd=%zu\n",
                g_lastSelName, g_watchdogReceiverClass, frameDepth_);
        // Dump blocked process info for priority 40 (MorphicRenderLoop)
        if (readyCounts[39] == 0 && activePri != 40) {
            if (g_savedP40Process.isObject() && !g_savedP40Process.isNil()) {
                Oop proc = g_savedP40Process;
                // myList (slot 3 of process) = what it's waiting on
                Oop myList = memory_.fetchPointer(3, proc);
                if (myList.isObject() && !myList.isNil()) {
                    std::string listClassName = "?";
                    Oop myListCls = memory_.classOf(myList);
                    if (myListCls.isObject() && memory_.slotCountOf(myListCls) > 6) {
                        Oop nameOop = memory_.fetchPointer(6, myListCls);
                        if (nameOop.isObject() && !nameOop.isNil()) {
                            ObjectHeader* nHdr = nameOop.asObjectPtr();
                            if (nHdr->isBytesObject() && nHdr->byteSize() < 100)
                                listClassName = std::string((char*)nHdr->bytes(), nHdr->byteSize());
                        }
                    }
                    fprintf(stderr, "  [P40-WAITING-ON] %s (0x%llx)\n",
                            listClassName.c_str(), (unsigned long long)myList.rawBits());
                }
                // Walk the context chain (up to 8 frames)
                Oop ctx = memory_.fetchPointer(1, proc); // suspendedContext
                for (int frame = 0; frame < 8 && ctx.isObject() && !ctx.isNil(); frame++) {
                    ObjectHeader* ctxHdr = ctx.asObjectPtr();
                    if (ctxHdr->slotCount() < 6) break;
                    Oop method = memory_.fetchPointer(3, ctx);
                    Oop receiver = memory_.fetchPointer(5, ctx);
                    // Get receiver class name
                    std::string rcvClass = "?";
                    if (receiver.isSmallInteger()) rcvClass = "SmallInteger";
                    else if (receiver.isNil()) rcvClass = "nil";
                    else if (receiver.isObject() && receiver.rawBits() > 0x10000) {
                        Oop cls = memory_.classOf(receiver);
                        if (cls.isObject() && memory_.slotCountOf(cls) > 6) {
                            Oop nameOop = memory_.fetchPointer(6, cls);
                            if (nameOop.isObject() && !nameOop.isNil()) {
                                ObjectHeader* nh = nameOop.asObjectPtr();
                                if (nh->isBytesObject() && nh->byteSize() < 100)
                                    rcvClass = std::string((char*)nh->bytes(), nh->byteSize());
                            }
                        }
                    }
                    // Get selector from method
                    std::string selName = "?";
                    if (method.isObject() && !method.isNil()) {
                        Oop headerOop = memory_.fetchPointer(0, method);
                        if (headerOop.isSmallInteger()) {
                            int numLits = (int)(headerOop.asSmallInteger() & 0x7FFF);
                            if (numLits > 0) {
                                // Last literal is AdditionalMethodState or selector
                                Oop lastLit = memory_.fetchPointer(numLits, method);
                                if (lastLit.isObject() && !lastLit.isNil()) {
                                    ObjectHeader* llHdr = lastLit.asObjectPtr();
                                    if (llHdr->isBytesObject() && llHdr->byteSize() < 100) {
                                        // Direct selector (Symbol)
                                        selName = std::string((char*)llHdr->bytes(), llHdr->byteSize());
                                    } else if (llHdr->slotCount() > 0) {
                                        // AdditionalMethodState: selector in slot 0
                                        Oop selOop = memory_.fetchPointer(0, lastLit);
                                        if (selOop.isObject() && !selOop.isNil()) {
                                            ObjectHeader* selHdr = selOop.asObjectPtr();
                                            if (selHdr->isBytesObject() && selHdr->byteSize() < 100)
                                                selName = std::string((char*)selHdr->bytes(), selHdr->byteSize());
                                        }
                                    }
                                }
                                // If last literal didn't give us a selector, try second-to-last
                                if (selName == "?" && numLits > 1) {
                                    Oop penLit = memory_.fetchPointer(numLits - 1, method);
                                    if (penLit.isObject() && !penLit.isNil()) {
                                        ObjectHeader* plHdr = penLit.asObjectPtr();
                                        if (plHdr->isBytesObject() && plHdr->byteSize() < 100)
                                            selName = std::string((char*)plHdr->bytes(), plHdr->byteSize());
                                    }
                                }
                            }
                        }
                    }
                    fprintf(stderr, "  [P40-FRAME#%d] %s >> %s\n", frame, rcvClass.c_str(), selName.c_str());
                    // Follow sender chain
                    ctx = memory_.fetchPointer(0, ctx); // sender
                }
            }
        }
        checkCount = 0;
        usecFireTotal = 0;
        msFireTotal = 0;
    }

    // Check microsecond timer (primitive 242 - used by DelaySemaphoreScheduler)
    if (nextWakeupUsec_ != INT64_MAX && !timerSemaphore_.isNil()) {
        static constexpr int64_t kSmalltalkEpochOffset = 2177452800LL * 1000000LL;
        auto now = std::chrono::system_clock::now();
        auto epoch = now.time_since_epoch();
        int64_t unixUsec = std::chrono::duration_cast<std::chrono::microseconds>(epoch).count();
        int64_t currentUsec = unixUsec + kSmalltalkEpochOffset;

        if (currentUsec >= nextWakeupUsec_) {
            usecFireTotal++;
            if (usecFireTotal <= 20 || usecFireTotal % 200 == 0) {
                fprintf(stderr, "[TIMER-FIRE-USEC #%d] target=%lld current=%lld delta=%lldus step=%llu\n",
                        usecFireTotal, (long long)nextWakeupUsec_, (long long)currentUsec,
                        (long long)(currentUsec - nextWakeupUsec_),
                        (unsigned long long)g_stepNum);
            }
            Oop semaphore = timerSemaphore_;
            timerSemaphore_ = Oop::nil();
            nextWakeupUsec_ = INT64_MAX;
            synchronousSignal(semaphore);
            return;
        }
    }

    // Check millisecond timer (primitive 136 - legacy)
    if (nextWakeupTime_ == 0 || timerSemaphore_.isNil()) {
        return;
    }

    int64_t currentMs = ioMSecs();
    int64_t targetMs = nextWakeupTime_;
    int64_t diff = (currentMs - targetMs) & 0x3FFFFFFF;
    bool timerElapsed = (diff > 0) && (diff < 0x20000000);

    if (timerElapsed) {
        msFireTotal++;
        if (msFireTotal <= 20 || msFireTotal % 200 == 0) {
            fprintf(stderr, "[TIMER-FIRE-MS #%d] targetMs=%lld currentMs=%lld diff=%lld step=%llu\n",
                    msFireTotal, targetMs, currentMs, diff, (unsigned long long)g_stepNum);
        }
        Oop semaphore = timerSemaphore_;
        timerSemaphore_ = Oop::nil();
        nextWakeupTime_ = 0;
        synchronousSignal(semaphore);
    }
}

void Interpreter::synchronousSignal(Oop semaphore) {
    Oop firstLink = memory_.fetchPointer(LinkedListFirstLinkIndex, semaphore);

    if (firstLink.isNil() || firstLink.rawBits() == memory_.nil().rawBits()) {
        // No processes waiting - increment excessSignals
        Oop excessOop = memory_.fetchPointer(SemaphoreExcessSignalsIndex, semaphore);
        int64_t excess = excessOop.isSmallInteger() ? excessOop.asSmallInteger() : 0;
        memory_.storePointer(SemaphoreExcessSignalsIndex, semaphore,
                            Oop::fromSmallInteger(excess + 1));
    } else {
        // Wake the first waiting process
        Oop process = removeFirstLinkOfList(semaphore);
        Oop processPriorityOop = memory_.fetchPointer(ProcessPriorityIndex, process);
        int processPriority = static_cast<int>(processPriorityOop.asSmallInteger());

        Oop activeProcess = getActiveProcess();
        Oop activePriorityOop = memory_.fetchPointer(ProcessPriorityIndex, activeProcess);
        int activePriority = static_cast<int>(activePriorityOop.asSmallInteger());

        if (processPriority > activePriority) {
            putToSleep(activeProcess);
            transferTo(process);
        } else {
            putToSleep(process);
        }
    }
}

void Interpreter::signalFinalizationIfNeeded() {
    if (memory_.pendingFinalizationSignals() > 0) {
        memory_.clearPendingFinalizationSignals();
        if (memory_.hasMourners()) {
            Oop sema = memory_.specialObject(SpecialObjectIndex::TheFinalizationSemaphore);
            if (sema.isObject() && sema.rawBits() != memory_.nil().rawBits()) {
                Oop activeProcess = getActiveProcess();
                Oop firstLink = memory_.fetchPointer(LinkedListFirstLinkIndex, sema);

                if (firstLink.isObject() && firstLink.rawBits() != memory_.nil().rawBits()) {
                    // Case 1: Finalization process is waiting on the semaphore.
                    // Remove it and force-yield to it directly.
                    Oop finProcess = removeFirstLinkOfList(sema);
                    putToSleep(activeProcess);
                    transferTo(finProcess);
                } else {
                    // Case 2: Finalization process is not on the semaphore.
                    // Signal the semaphore (for excess count), then search the
                    // ready list for a finalization-priority process and yield to it.
                    synchronousSignal(sema);
                    forceYieldForFinalization(activeProcess);
                }
            }
        }
    }
}

void Interpreter::forceYieldForFinalization(Oop activeProcess) {
    // The finalization process runs at userInterruptPriority (50).
    // Check the ready list at priority 50 for the finalization process.
    constexpr int finalizationPriority = 50;
    constexpr int finalizationIdx = finalizationPriority - 1; // 0-indexed

    Oop schedAssoc = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);
    Oop scheduler = memory_.fetchPointer(1, schedAssoc);
    Oop schedLists = memory_.fetchPointer(SchedulerProcessListsIndex, scheduler);
    ObjectHeader* listsHdr = schedLists.asObjectPtr();

    if (finalizationIdx >= (int)listsHdr->slotCount()) return;

    Oop processList = memory_.fetchPointer(finalizationIdx, schedLists);
    Oop first = memory_.fetchPointer(LinkedListFirstLinkIndex, processList);
    if (first.isObject() && first.rawBits() != memory_.nil().rawBits()) {
        // Found the finalization process on the ready list at priority 50.
        Oop finProcess = removeFirstLinkOfList(processList);
        putToSleep(activeProcess);
        transferTo(finProcess);
    } else {
        // Finalization process is not on the ready list at priority 50.
        // It may be blocked on a throttle semaphore (inside its mourning loop).
        // The finalization semaphore has been signaled (excess++), so the fin
        // process will pick up mourners when it returns to its wait loop.
        // However, this may take time if higher-priority processes keep running.
        //
        // Mark that we have pending mourners that need attention.
        // The step check will periodically try to yield to the fin process.
        pendingMournerYield_ = true;
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
                    pendingSignalIndex_.store(inputSemaIdx, std::memory_order_release);
                }
            }

            // Every ~1000ms, set force yield flag to allow lower priority processes to run
            // This simulates the preemption that would happen from primitive 230 (relinquishProcessor)
            // Using a longer interval (1 second) to allow more startup work to complete
            if (tickCount % 1000 == 0) {
                forceYield_.store(true, std::memory_order_release);
            }

            // Watchdog: report step count every 5 seconds
            if (tickCount % 5000 == 0) {
                long long steps = g_watchdogSteps.load(std::memory_order_relaxed);
                int phase = g_watchdogPhase.load(std::memory_order_relaxed);
                static long long lastSteps = -1;
                const char* phaseName[] = {"idle", "step()", "GC", "events", "syncDisplay"};
                int subphase = g_watchdogSubphase;
                uint8_t lastBC = g_watchdogLastBytecode;
                bool stuck = (steps == lastSteps);
                fprintf(stderr, "[WATCHDOG] tick=%d steps=%lldM phase=%s sub=%d bc=0x%02x pri=%d%s\n",
                        tickCount, steps / 1000000,
                        (phase >= 0 && phase <= 4) ? phaseName[phase] : "?",
                        subphase, lastBC, g_watchdogProcessPriority,
                        stuck ? " STUCK!" : "");
                if (stuck && subphase == 15) {
                    fprintf(stderr, "[WATCHDOG] STUCK in send: %s >> %s (prim=%d)\n",
                            g_watchdogReceiverClass, g_watchdogSelector, g_watchdogPrimIndex);
                }
                lastSteps = steps;
                fflush(stderr);
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
    // Store the index to be processed in the interpret loop
    // This is thread-safe due to atomic
    pendingSignalIndex_.store(index, std::memory_order_release);
}

void Interpreter::processPendingSignals() {
    int index = pendingSignalIndex_.exchange(0, std::memory_order_acquire);
    if (index <= 0) return;

    // Get the external semaphore table from special objects
    Oop semTable = memory_.specialObject(SpecialObjectIndex::ExternalObjectsArray);
    if (semTable.isNil() || !semTable.isObject()) {
        return;
    }

    // Index is 1-based, convert to 0-based array index
    size_t tableIndex = static_cast<size_t>(index - 1);
    size_t tableSize = memory_.slotCountOf(semTable);
    if (tableIndex >= tableSize) {
        return;
    }

    Oop semaphore = memory_.fetchPointer(tableIndex, semTable);
    if (semaphore.isNil() || !semaphore.isObject()) {
        return;
    }

    synchronousSignal(semaphore);
}

bool Interpreter::step() {
    if (!running_) {
        return false;
    }

    // GC safe point: between bytecodes, no C++ locals hold Oops.
    g_watchdogSubphase = 10;
    if (memory_.needsCompactGC()) {
        memory_.clearCompactGCFlag();
        int gcNum = memory_.statistics().gcCount + 1;
        fprintf(stderr, "[GC-RUN #%d] step=%llu fd=%zu method=0x%llx ip=%p\n",
                gcNum, g_stepNum, frameDepth_,
                (unsigned long long)method_.rawBits(),
                (void*)instructionPointer_);
        memory_.fullGC();
        flushMethodCache();
        fprintf(stderr, "[GC-DONE #%d] method=0x%llx ip=%p\n",
                gcNum, (unsigned long long)method_.rawBits(),
                (void*)instructionPointer_);
    }

    g_stepCount++;

    // Check timer and process pending signals periodically
    {
    static int stepCheckCounter = 0;
    stepCheckCounter++;
    if (stepCheckCounter % 100 == 0) {
        g_watchdogSubphase = 11;
        checkTimerSemaphore();
        if (hasPendingSignals()) {
            processPendingSignals();
        }
        g_watchdogSubphase = 12;
        signalFinalizationIfNeeded();

        // If mourners are pending but we couldn't yield to the finalization
        // process (it was blocked on a throttle semaphore), check if it's
        // now available on the finalization semaphore or ready list.
        if (pendingMournerYield_ && memory_.hasMourners()) {
            Oop sema = memory_.specialObject(SpecialObjectIndex::TheFinalizationSemaphore);
            if (sema.isObject() && sema.rawBits() != memory_.nil().rawBits()) {
                Oop firstLink = memory_.fetchPointer(LinkedListFirstLinkIndex, sema);
                if (firstLink.isObject() && firstLink.rawBits() != memory_.nil().rawBits()) {
                    // Finalization process is now on the semaphore - yield to it
                    pendingMournerYield_ = false;
                    Oop finProcess = removeFirstLinkOfList(sema);
                    Oop activeProcess = getActiveProcess();
                    putToSleep(activeProcess);
                    transferTo(finProcess);
                } else {
                    // Try ready list at priority 50
                    Oop schedAssoc = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);
                    Oop scheduler = memory_.fetchPointer(1, schedAssoc);
                    Oop schedLists = memory_.fetchPointer(SchedulerProcessListsIndex, scheduler);
                    constexpr int finIdx = 49; // priority 50, 0-indexed
                    if (finIdx < (int)schedLists.asObjectPtr()->slotCount()) {
                        Oop processList = memory_.fetchPointer(finIdx, schedLists);
                        Oop first = memory_.fetchPointer(LinkedListFirstLinkIndex, processList);
                        if (first.isObject() && first.rawBits() != memory_.nil().rawBits()) {
                            pendingMournerYield_ = false;
                            Oop finProcess = removeFirstLinkOfList(processList);
                            Oop activeProcess = getActiveProcess();
                            putToSleep(activeProcess);
                            transferTo(finProcess);
                        }
                    }
                }
            }
        } else if (pendingMournerYield_ && !memory_.hasMourners()) {
            pendingMournerYield_ = false;  // Mourners were processed
        }

        // Log active process priority + selector (disabled — fprintf overhead slows VM)
        // Enable only when actively debugging scheduler issues
        if (false && stepCheckCounter % 500000 == 0) {
            Oop proc = getActiveProcess();
            Oop prioOop = memory_.fetchPointer(ProcessPriorityIndex, proc);
            int prio = prioOop.isSmallInteger() ? static_cast<int>(prioOop.asSmallInteger()) : -1;
            const char* sel = g_watchdogSelector[0] ? g_watchdogSelector : "?";
            fprintf(stderr, "[PROC] steps=%lldM prio=%d sel=%s\n",
                    (long long)(g_stepNum / 1000000), prio, sel);
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

    // Track process switches (disabled for performance - getActiveProcess costs 3 fetches per step)
    if constexpr (ENABLE_DEBUG_LOGGING) {
        static FILE* procLog = nullptr;
        static Oop lastProc = Oop::nil();
        static int procSwitchCount = 0;
        Oop curProc = getActiveProcess();
        if (curProc.rawBits() != lastProc.rawBits()) {
            if (!procLog) procLog = nullptr;
            if (procLog && procSwitchCount < 200) {
                procSwitchCount++;
                int priority = -1;
                if (curProc.isObject()) {
                    Oop priOop = memory_.fetchPointer(ProcessPriorityIndex, curProc);
                    if (priOop.isSmallInteger()) priority = (int)priOop.asSmallInteger();
                }
                fprintf(procLog, "[PROC-SWITCH #%d step=%llu] 0x%llx (pri=%d) -> 0x%llx (pri=",
                        procSwitchCount, (unsigned long long)g_stepNum,
                        (unsigned long long)lastProc.rawBits(), -1,
                        (unsigned long long)curProc.rawBits());
                fprintf(procLog, "%d)\n", priority);
                fflush(procLog);
            }
            lastProc = curProc;
        }
    }

    // If the previous step slept in relinquishProcessor, report as idle
    if (relinquishSlept_) {
        relinquishSlept_ = false;
        return false;  // Signal idle to caller
    }


    static uint64_t stepCountForDriver = 0;
    stepCountForDriver++;

    // Process any pending external semaphore signals (from heartbeat/events)
    if (hasPendingSignals()) {
        processPendingSignals();
    }

    // Periodic preemption check - every 10000 bytecodes, check if we should
    // yield to a higher-priority or same-priority runnable process
    static uint64_t bytecodeCount = 0;
    bytecodeCount++;
    if (bytecodeCount % 10000 == 0) {
        checkForPreemption();
    }

    // Check if we've run past the end of bytecodes
    if (instructionPointer_ >= bytecodeEnd_) {
        returnValue(receiver_);
        return running_;
    }

    // NOTE: Do NOT reset extA_/extB_ here!
    // In Sista V1, extension bytecodes (0xE0/0xE1) set these values, then the
    // NEXT bytecode uses them. The consuming bytecodes reset them after use.
    // Resetting here would break extension byte chains.

    // Track step count (for debugging if needed)
    g_stepNum++;

    // Check for forced process yield BEFORE fetching the next bytecode.
    // CRITICAL: Must happen before fetchByte() because fetchByte() advances
    // instructionPointer_. If we yield after fetching, the saved PC will point
    // past the fetched bytecode, causing it to be SKIPPED when the process
    // is later restored — leading to expression stack corruption and DNUs.
    g_watchdogSubphase = 14;
    bool shouldYield = forceYield_.load(std::memory_order_relaxed);
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
        forceYield_.store(false, std::memory_order_relaxed);
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
            if (activePriority > 0 && activePriority <= static_cast<int>(schedLists.asObjectPtr()->slotCount())) {
                Oop processList = memory_.fetchPointer(activePriority - 1, schedLists);
                Oop first = memory_.fetchPointer(LinkedListFirstLinkIndex, processList);
                if (first.isObject() && first.rawBits() != nilObj.rawBits() &&
                    first.rawBits() != activeProcess.rawBits()) {
                    nextProcess = removeFirstLinkOfList(processList);
                }
            }
        }

        bool foundProcess = nextProcess.isObject() &&
                           nextProcess.rawBits() != nilObj.rawBits() &&
                           nextProcess.rawBits() != activeProcess.rawBits();

        if (foundProcess) {
            putToSleep(activeProcess);
            g_xferReason = "forceYield";
            transferTo(nextProcess);
        }

        if (hasPendingDriverInstall_) {
            executePendingDriverInstall();
            return running_;
        }
    }
skip_yield:

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

    // Check if this is a send bytecode (message send)
    bool isSend = (bytecode >= 0x60 && bytecode <= 0x7F) ||  // Sista send
                  (bytecode >= 0x80 && bytecode <= 0x8F) ||  // V3 send
                  (bytecode >= 0xB0 && bytecode <= 0xBF) ||  // Arithmetic sends
                  (bytecode >= 0xC0 && bytecode <= 0xCF) ||  // More sends
                  (bytecode >= 0xD0 && bytecode <= 0xDF) ||  // Send literal 0-15
                  (bytecode >= 0xEA && bytecode <= 0xED) ||  // Extended sends
                  bytecode == 0xF8 || bytecode == 0xF9;      // Super sends

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
    // Record bytecode in history buffer for debugging
    recentBytecodes_[recentBytecodeIdx_ % 256] = bytecode;
    recentBytecodeIdx_++;
    // Sista V1 bytecode dispatch (used by Pharo 10+, format 68021 with modern compiler)
    // Key differences from V3PlusClosures:
    // - 0x10-0x1F: push literal var (not temp)
    // - 0x30-0x3F: push temp 0-15
    // - 0x40-0x4B: push temp 16-27
    // - 0x4C-0x4F: push self, true, false, nil
    // - 0x50-0x51: push 0, push 1

    // ========================================================================
    // SISTA V1 BYTECODE DECODER (Pharo 10+)
    // Based on EncoderForSistaV1 specification
    // ========================================================================

    if (bytecode <= 0x0F) {
        // 0x00-0x0F: Push Receiver Variable 0-15 (same in V3 and Sista)
        pushReceiverVariable(bytecode);
    }
    else if (bytecode <= 0x1F) {
        // 0x10-0x1F: Differs by bytecode set
        if (usesSistaV1_) {
            // Sista V1: Push Literal Variable 0-15 (dereference Association)
            pushLiteralVariable(bytecode - 0x10);
        } else {
            // V3: push temp 0-15
            pushTemporary(bytecode - 0x10);
        }
    }
    else if (bytecode <= 0x3F) {
        // 0x20-0x3F: Differs by bytecode set
        if (usesSistaV1_) {
            // Sista V1: Push Literal Constant 0-31 (push literal directly)
            pushLiteralConstant(bytecode - 0x20);
        } else {
            // V3: 0x20-0x2F = push literal 0-15, 0x30-0x3F = push literal 16-31
            pushLiteralConstant(bytecode - 0x20);
        }
    }
    else if (bytecode <= 0x5F) {
        // 0x40-0x5F: Differs significantly between V3 and Sista
        if (!usesSistaV1_) {
            // V3PlusClosures: 0x40-0x5F = push literal variable 0-31
            pushLiteralVariable(bytecode - 0x40);
        } else {
            // Sista V1:
            // 0x40-0x47: Push Temp 0-7
            // 0x48-0x4B: Push Temp 8-11
            // 0x4C: Push self, 0x4D: Push true, 0x4E: Push false, 0x4F: Push nil
            // 0x50: Push 0, 0x51: Push 1, 0x52: Push thisContext, 0x53: Dup
            // 0x54-0x57: UNASSIGNED
            // 0x58: Return self, 0x59: Return true, 0x5A: Return false, 0x5B: Return nil
            // 0x5C: Return top, 0x5D: BlockReturn nil, 0x5E: BlockReturn top, 0x5F: Nop
            if (bytecode <= 0x47) {
                // 0x40-0x47: Push Temp 0-7
                pushTemporary(bytecode - 0x40);
            } else if (bytecode <= 0x4B) {
                // 0x48-0x4B: Push Temp 8-11
                pushTemporary(8 + bytecode - 0x48);
            } else if (bytecode <= 0x4F) {
                // 0x4C-0x4F: Push specials
                switch (bytecode) {
                    case 0x4C: push(receiver_); break;  // push self
                    case 0x4D: push(memory_.trueObject()); break;   // push true
                    case 0x4E: push(memory_.falseObject()); break;  // push false
                    case 0x4F: push(memory_.nil()); break;          // push nil
                }
            } else if (bytecode <= 0x53) {
                // 0x50-0x53: Special pushes
                switch (bytecode) {
                    case 0x50: push(Oop::fromSmallInteger(0)); break;  // push 0
                    case 0x51: push(Oop::fromSmallInteger(1)); break;  // push 1
                    case 0x52: {
                        // Sista V1: extB == 0 → push thisContext
                        //           extB == 1 → push thisProcess (active Process)
                        int savedExtB = extB_;
                        extB_ = 0;

                        if (savedExtB == 1) {
                            // Push thisProcess: the active Process object
                            push(getActiveProcess());
                            break;
                        }

                        // Push thisContext - must materialize inline frames first!
                        static int thisCtxCount = 0;
                        thisCtxCount++;

                        Oop contextToPush = activeContext_;

                        if (frameDepth_ > 0) {
                            size_t savedFrameDepth = frameDepth_;
                            contextToPush = materializeFrameStack();
                            activeContext_ = contextToPush;
                            currentFrameMaterializedCtx_ = memory_.nil();  // Now running from context, not frames
                            frameDepth_ = 0;

                        }

                        push(contextToPush);
                        break;
                    }
                    case 0x53: push(stackTop()); break;                // duplicate top
                }
            } else if (bytecode <= 0x57) {
                // 0x54-0x57: UNASSIGNED in Sista V1
                // These should not appear in valid Pharo code
            } else {
                // 0x58-0x5F: Returns and special operations
                switch (bytecode) {
                    case 0x58: // return self
                    case 0x59: // return true
                    case 0x5A: // return false
                    case 0x5B: // return nil
                    {
                        Oop val;
                        switch (bytecode) {
                            case 0x58: val = receiver_; break;
                            case 0x59: val = memory_.trueObject(); break;
                            case 0x5A: val = memory_.falseObject(); break;
                            default:   val = memory_.nil(); break;
                        }
                        // Push value and use returnFromMethod() which handles
                        // both regular returns and NLR with ensure: detection.
                        // Previously this had inline NLR that skipped ensure: checks.
                        push(val);
                        returnFromMethod();
                        break;
                    }
                    case 0x5C: {
                        returnFromMethod();
                        break;
                    }
                    case 0x5D: returnValue(memory_.nil()); break;          // block return nil
                    case 0x5E: {                                           // block return top
                        // Sista V1: 0x5E is extensible via Extend A
                        // extA_ = 0: return from current block (simple return)
                        // extA_ = N: return from N-th enclosing block (non-local return)
                        int enclosingLevels = extA_;
                        extA_ = 0;  // Consume extension

                        if (enclosingLevels > 0) {
                            // Non-local return - unwind enclosingLevels blocks
                            returnFromBlock();
                        } else {
                            // Simple block return - just return the value
                            Oop value = pop();
                            returnValue(value);
                        }
                        break;
                    }
                    case 0x5F: /* Nop */ break;
                }
            }
        }
    }
    else if (bytecode <= 0x6F) {
        // 0x60-0x6F: Differs between V3 and Sista
        if (!usesSistaV1_) {
            // V3: Pop and store receiver variable 0-7 / temp 0-7
            if (bytecode <= 0x67) {
                Oop value = pop();
                setReceiverInstVar(bytecode - 0x60, value);
            } else {
                Oop value = pop();
                setTemporary(bytecode - 0x68, value);
            }
        } else {
            // Sista V1: 0x60-0x6F = Send Arithmetic Message 0-15
            // (+, -, <, >, <=, >=, =, ~=, *, /, \\, @, bitShift:, //, bitAnd:, bitOr:)
            sendArithmetic(bytecode - 0x60);
        }
    }
    else if (bytecode <= 0x7F) {
        // 0x70-0x7F: Differs between V3 and Sista
        if (!usesSistaV1_) {
            // V3PlusClosures:
            // 0x70-0x77: push specials (self, true, false, nil, -1, 0, 1, 2)
            // 0x78-0x7B: return (self, true, false, nil)
            // 0x7C: return top, 0x7D: block return
            if (bytecode <= 0x77) {
                switch (bytecode) {
                    case 0x70: push(receiver_); break;
                    case 0x71: push(memory_.trueObject()); break;
                    case 0x72: push(memory_.falseObject()); break;
                    case 0x73: push(memory_.nil()); break;
                    case 0x74: push(Oop::fromSmallInteger(-1)); break;
                    case 0x75: push(Oop::fromSmallInteger(0)); break;
                    case 0x76: push(Oop::fromSmallInteger(1)); break;
                    case 0x77: push(Oop::fromSmallInteger(2)); break;
                }
            } else if (bytecode <= 0x7B) {
                switch (bytecode) {
                    case 0x78: returnValue(receiver_); break;
                    case 0x79: returnValue(memory_.trueObject()); break;
                    case 0x7A: returnValue(memory_.falseObject()); break;
                    case 0x7B: returnValue(memory_.nil()); break;
                }
            } else if (bytecode == 0x7C) {
                returnFromMethod();
            } else if (bytecode == 0x7D) {
                returnFromBlock();
            }
        } else {
            // Sista V1: 0x70-0x7F = Send Special Message 16-31
            // (at:, at:put:, size, next, nextPut:, atEnd, ==, class, ~~, value, value:, do:, new, new:, x, y)

            // Bytecodes 0x79 (value), 0x7A (value:), 0x7B (do:) are optimized:
            // If receiver is a FullBlockClosure, directly call primitiveFullClosureValue
            // to activate the block (which sets up homeFrameDepth for NLR).
            // This matches the reference VM's bytecodePrimValue/bytecodePrimValueWithArg.
            int which = bytecode - 0x70;
            bool handled = false;
            if (which == 9 || which == 10) {
                // 0x79 = value (0 args), 0x7A = value: (1 arg)
                // Optimized: directly activate FullBlockClosures via primitiveFullClosureValue
                // which calls activateBlock() and sets homeFrameDepth for NLR.
                int numArgs = which - 9;
                Oop rcvr = stackValue(numArgs);
                if (rcvr.isObject() && rcvr.rawBits() > 0x10000) {
                    ObjectHeader* rcvrHdr = rcvr.asObjectPtr();
                    if (rcvrHdr->classIndex() == 38) {  // ClassFullBlockClosureCompactIndex
                        argCount_ = numArgs;
                        primitiveFailed_ = false;
                        PrimitiveResult result = primitiveFullClosureValue(numArgs);
                        if (result == PrimitiveResult::Success) {
                            handled = true;
                        }
                    }
                }
            }
            if (!handled) {
                commonSend(which);
            }
        }
    }
    else if (bytecode <= 0x8F) {
        // Sista V1: 0x80-0x8F (128-143): Send literal selector 0-15 with 0 args
        int litIndex = bytecode & 0x0F;
        Oop selector = literal(litIndex);
        sendSelector(selector, 0);
    }
    else if (bytecode <= 0x9F) {
        // Sista V1: 0x90-0x9F (144-159): Send literal selector 0-15 with 1 arg
        int litIndex = bytecode & 0x0F;
        Oop selector = literal(litIndex);
        sendSelector(selector, 1);
    }
    else if (bytecode <= 0xAF) {
        // Sista V1: 0xA0-0xAF (160-175): Send literal selector 0-15 with 2 args
        int litIndex = bytecode & 0x0F;
        Oop selector = literal(litIndex);
        sendSelector(selector, 2);
    }
    else if (bytecode <= 0xB7) {
        // Sista V1: 0xB0-0xB7 (176-183): Short unconditional jump (1-8 bytes forward)
        int offset = (bytecode & 0x07) + 1;
        shortJump(offset);
    }
    else if (bytecode <= 0xBF) {
        // Sista V1: 0xB8-0xBF (184-191): Short conditional jump if true (1-8 bytes)
        int offset = (bytecode & 0x07) + 1;
        shortJumpIfTrue(offset);
    }
    else if (bytecode <= 0xC7) {
        // Sista V1: 0xC0-0xC7 (192-199): Short conditional jump if false (1-8 bytes)
        int offset = (bytecode & 0x07) + 1;
        shortJumpIfFalse(offset);
    }
    else if (bytecode <= 0xCF) {
        // Sista V1: 0xC8-0xCF (200-207): Pop and Store Receiver Variable 0-7
        int varIndex = bytecode & 0x07;
        Oop value = pop();
        setReceiverInstVar(varIndex, value);
    }
    else if (bytecode <= 0xD7) {
        // Sista V1: 0xD0-0xD7 (208-215): Store and pop temp 0-7
        int tempIndex = bytecode & 0x07;
        Oop value = pop();
        setTemporary(tempIndex, value);
    }
    else if (bytecode == 0xD8) {
        // Sista V1: 0xD8 (216): Pop stack (discard top of stack)
        pop();
    }
    else if (bytecode == 0xD9) {
        // Sista V1: 0xD9 (217): Unconditional trap (debugging)
        stopVM("Unconditional trap bytecode 0xD9");
    }
    else if (bytecode <= 0xDF) {
        // Sista V1: 0xDA-0xDF (218-223): Various extended operations
        // These are typically used for debugging or reserved - no-op
    }
    else if (bytecode <= 0xE7) {
        // ========================================================================
        // Sista V1: 0xE0-0xE7 (224-231): 2-byte bytecodes - Extensions and Push operations
        // ========================================================================
        if (!usesSistaV1_) {
            // V3PlusClosures: 0xE0-0xE7 = Send literal selector 0-7 with 2 args
            int litIndex = bytecode - 0xE0;
            Oop selector = literal(litIndex);
            sendSelector(selector, 2);
        } else {
            switch (bytecode) {
                case 0xE0: // 224: Extend A (unsigned) - modifies next bytecode's index
                {
                    uint8_t extByte = fetchByte();
                    extA_ = (extA_ << 8) | extByte;
                    inExtension_ = true;  // Prevent forceYield before target bytecode
                    break;
                }
                case 0xE1: // 225: Extend B (signed) - modifies next bytecode's numArgs/offset
                {
                    uint8_t extByte = fetchByte();
                    // Sign extend if high bit set
                    if (extByte >= 128) {
                        extB_ = (extB_ << 8) | extByte | 0xFFFFFF00;
                    } else {
                        extB_ = (extB_ << 8) | extByte;
                    }
                    inExtension_ = true;  // Prevent forceYield before target bytecode
                    break;
                }
                case 0xE2: // 226: Push Receiver Variable #iiiiiiii (+ extA * 256)
                {
                    uint8_t indexByte = fetchByte();
                    int fullIndex = (extA_ << 8) | indexByte;
                    extA_ = 0;
                    pushReceiverVariable(fullIndex);
                    break;
                }
                case 0xE3: // 227: Push Literal Variable #iiiiiiii (+ extA * 256)
                {
                    uint8_t indexByte = fetchByte();
                    int fullIndex = (extA_ << 8) | indexByte;
                    extA_ = 0;
                    pushLiteralVariable(fullIndex);
                    break;
                }
                case 0xE4: // 228: Push Literal Constant #iiiiiiii (+ extA * 256)
                {
                    uint8_t indexByte = fetchByte();
                    int fullIndex = (extA_ << 8) | indexByte;
                    extA_ = 0;
                    pushLiteralConstant(fullIndex);
                    break;
                }
                case 0xE5: // 229: Push Temporary Variable #iiiiiiii
                {
                    uint8_t indexByte = fetchByte();
                    pushTemporary(indexByte);
                    break;
                }
                case 0xE6: // 230: UNASSIGNED (was pushNClosureTemps)
                    fetchByte();  // Skip the argument byte
                    break;
                case 0xE7: // 231: Push Array (j=0) or Pop into Array (j=1)
                {
                    // jkkkkkkk: j=operation type, kkkkkkk=array size
                    uint8_t desc = fetchByte();
                    int arraySize = desc & 0x7F;
                    bool popIntoArray = (desc >> 7) != 0;

                    // Create array
                    Oop arrayClass = memory_.specialObject(SpecialObjectIndex::ClassArray);
                    uint32_t classIndex = memory_.indexOfClass(arrayClass);
                    Oop array = memory_.allocateSlots(classIndex, arraySize, ObjectFormat::Indexable);

                    // Trace temp vector creation
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
                        // Pop arraySize elements into new array
                        for (int i = arraySize - 1; i >= 0; i--) {
                            memory_.storePointer(i, array, pop());
                        }
                    }
                    push(array);
                    break;
                }
            }
        }
    }
    else if (bytecode <= 0xEF) {
        // ========================================================================
        // Sista V1: 0xE8-0xEF (232-239): 2-byte bytecodes - Push/Send/Jump
        // ========================================================================
        switch (bytecode) {
            case 0xE8: // 232: Push Integer #iiiiiiii (+ extB * 256, signed)
            {
                uint8_t intByte = fetchByte();
                int value = intByte + static_cast<int>(static_cast<unsigned int>(extB_) << 8);
                extB_ = 0;
                push(Oop::fromSmallInteger(value));
                break;
            }
            case 0xE9: // 233: Push Character #iiiiiiii (+ extB * 256)
            {
                uint8_t charByte = fetchByte();
                int codePoint = charByte + static_cast<int>(static_cast<unsigned int>(extB_) << 8);
                extB_ = 0;
                // Create Character object - character is stored as immediate
                push(Oop::fromCharacter(codePoint));
                break;
            }
            case 0xEA: // 234: Send Literal Selector #iiiii (+ extA*32) with jjj (+ extB*8) args
            {
                uint8_t desc = fetchByte();
                int selectorIndex = ((extA_ << 5) | (desc >> 3)) & 0xFFFF;
                int numArgs = ((extB_ << 3) | (desc & 0x07)) & 0xFF;
                extA_ = 0;
                extB_ = 0;
                Oop selector = literal(selectorIndex);
                sendSelector(selector, numArgs);
                break;
            }
            case 0xEB: // 235: Send To Superclass
            {
                uint8_t desc = fetchByte();
                int selectorIndex = ((extA_ << 5) | (desc >> 3)) & 0xFFFF;
                int effectiveExtB = extB_;
                extA_ = 0;
                extB_ = 0;
                Oop selector = literal(selectorIndex);
                Oop lookupClass;

                if (effectiveExtB >= 64) {
                    // Directed super send (used by FullBlockClosures):
                    // Stack layout (top to bottom): definingClass, argN, ..., arg1, receiver
                    // The defining class is on top of stack. Lookup starts from its SUPERCLASS.
                    // numArgs uses (extB - 64) instead of extB.
                    int numArgs = (((effectiveExtB - 64) << 3) | (desc & 0x07)) & 0xFF;
                    Oop definingClass = pop();  // Pop the defining class from top of stack
                    lookupClass = superclassOf(definingClass);

                    Oop method = lookupMethod(selector, lookupClass);
                    if (method.isNil()) {
                        sendDoesNotUnderstand(selector, numArgs);
                    } else {
                        // Check for primitive and execute it before activating the method
                        int primIdx = primitiveIndexOf(method);
                        if (primIdx > 0) {
                            argCount_ = numArgs;
                            primitiveFailed_ = false;
                            newMethod_ = method;
                            PrimitiveResult result = executePrimitive(primIdx, numArgs);
                            if (result == PrimitiveResult::Success) {
                                break;  // Primitive handled it
                            }
                        }
                        activateMethod(method, numArgs);
                    }
                } else {
                    // Normal super send: lookup from superclass of method's defining class
                    int numArgs = ((effectiveExtB << 3) | (desc & 0x07)) & 0xFF;
                    Oop methodClass = methodClassOf(method_);

                    if (methodClass.isNil() || !methodClass.isObject()) {
                        lookupClass = superclassOf(memory_.classOf(receiver_));
                    } else {
                        lookupClass = superclassOf(methodClass);
                    }
                    Oop method = lookupMethod(selector, lookupClass);

                    if (method.isNil()) {
                        sendDoesNotUnderstand(selector, numArgs);
                    } else {
                        // Check for primitive and execute it before activating the method
                        int primIdx = primitiveIndexOf(method);
                        if (primIdx > 0) {
                            argCount_ = numArgs;
                            primitiveFailed_ = false;
                            newMethod_ = method;
                            PrimitiveResult result = executePrimitive(primIdx, numArgs);
                            if (result == PrimitiveResult::Success) {
                                break;  // Primitive handled it
                            }
                        }
                        activateMethod(method, numArgs);
                    }
                }
                break;
            }
            case 0xEC: // 236: Call Mapped Inlined Primitive #iiiiiiii
            {
                uint8_t primByte = fetchByte();
                // Inlined primitives are handled specially by the JIT
                // For interpreter, we just execute the fallback code
                (void)primByte;
                break;
            }
            case 0xED: // 237: Jump #iiiiiiii (+ extB * 256, signed)
            {
                uint8_t offsetByte = fetchByte();
                // Match Cog VM: cast to unsigned before shift to avoid UB on negative extB_
                int offset = offsetByte + static_cast<int>(static_cast<unsigned int>(extB_) << 8);
                extB_ = 0;
                if (false && offset < 0 && g_stepNum >= 10000 && g_stepNum <= 25000) {
                    static FILE* bjLog = nullptr;
                    if (!bjLog) bjLog = nullptr;
                    if (bjLog) { fprintf(bjLog, "[BJ #%llu] 0xED offset=%d IP=%p\n", g_stepNum, offset, (void*)instructionPointer_); fflush(bjLog); }
                }
                instructionPointer_ += offset;
                break;
            }
            case 0xEE: // 238: Pop and Jump On True #iiiiiiii (+ extB * 256)
            {
                uint8_t offsetByte = fetchByte();
                // Match Cog VM: unsigned shift + reset both extA_ and extB_
                int offset = offsetByte + static_cast<int>(static_cast<unsigned int>(extB_) << 8);
                extA_ = 0;
                extB_ = 0;
                Oop value = pop();
                bool isT = isTrue(value);

                if constexpr (ENABLE_DEBUG_LOGGING) {
                    static int eeCount = 0;
                    static FILE* jumpLog = nullptr;
                    if (!jumpLog) jumpLog = nullptr;
                    bool isF = isFalse(value);
                    eeCount++;
                    if (jumpLog && eeCount <= 200) {
                        fprintf(jumpLog, "[JIT #%d] value=0x%llx isTrue=%d isFalse=%d offset=%d %s\n",
                                eeCount, (unsigned long long)value.rawBits(),
                                isT, isF, offset, isT ? "JUMP" : "no-jump");
                        fflush(jumpLog);
                    }
                }

                if (false && offset < 0 && g_stepNum >= 10000 && g_stepNum <= 25000) {
                    static FILE* bjLog = nullptr;
                    if (!bjLog) bjLog = nullptr;
                    if (bjLog) { fprintf(bjLog, "[BJ #%llu] 0xEE offset=%d isT=%d\n", g_stepNum, offset, isT); fflush(bjLog); }
                }
                if (isT) {
                    instructionPointer_ += offset;
                }
                break;
            }
            case 0xEF: // 239: Pop and Jump On False #iiiiiiii (+ extB * 256)
            {
                uint8_t offsetByte = fetchByte();
                // Match Cog VM: unsigned shift + reset both extA_ and extB_
                int offset = offsetByte + static_cast<int>(static_cast<unsigned int>(extB_) << 8);
                extA_ = 0;
                extB_ = 0;
                Oop value = pop();
                bool isT = isTrue(value);
                bool willJump = !isT;

                if constexpr (ENABLE_DEBUG_LOGGING) {
                    static int efCount = 0;
                    static FILE* jumpLog = nullptr;
                    if (!jumpLog) jumpLog = nullptr;
                    bool isF = isFalse(value);
                    efCount++;
                    if (jumpLog && efCount <= 200) {
                        fprintf(jumpLog, "[JIF #%d] value=0x%llx isTrue=%d isFalse=%d offset=%d %s\n",
                                efCount, (unsigned long long)value.rawBits(),
                                isT, isF, offset, willJump ? "JUMP" : "no-jump");
                        fflush(jumpLog);
                    }
                }

                if (false && offset < 0 && g_stepNum >= 10000 && g_stepNum <= 25000) {
                    static FILE* bjLog = nullptr;
                    if (!bjLog) bjLog = nullptr;
                    if (bjLog) { fprintf(bjLog, "[BJ #%llu] 0xEF offset=%d willJump=%d\n", g_stepNum, offset, willJump); fflush(bjLog); }
                }
                if (willJump) {
                    instructionPointer_ += offset;
                }
                break;
            }
        }
    }
    else if (bytecode <= 0xF7) {
        // ========================================================================
        // Sista V1: 0xF0-0xF7 (240-247): 2-byte bytecodes - Store operations
        // ========================================================================
        switch (bytecode) {
            case 0xF0: // 240: Pop and Store Receiver Variable #iiiiiiii (+ extA * 256)
            {
                uint8_t indexByte = fetchByte();
                int fullIndex = (extA_ << 8) | indexByte;
                extA_ = 0;
                Oop value = pop();
                setReceiverInstVar(fullIndex, value);
                break;
            }
            case 0xF1: // 241: Pop and Store Literal Variable #iiiiiiii (+ extA * 256)
            {
                uint8_t indexByte = fetchByte();
                int fullIndex = (extA_ << 8) | indexByte;
                extA_ = 0;
                Oop value = pop();
                Oop assoc = literal(fullIndex);
                if (assoc.isObject()) {
                    memory_.storePointer(1, assoc, value);  // Store in Association's value slot
                }
                break;
            }
            case 0xF2: // 242: Pop and Store Temporary Variable #iiiiiiii
            {
                uint8_t indexByte = fetchByte();
                Oop value = pop();
                setTemporary(indexByte, value);
                break;
            }
            case 0xF3: // 243: Store Receiver Variable #iiiiiiii (+ extA * 256) - no pop
            {
                uint8_t indexByte = fetchByte();
                int fullIndex = (extA_ << 8) | indexByte;
                extA_ = 0;
                Oop value = stackTop();
                setReceiverInstVar(fullIndex, value);
                break;
            }
            case 0xF4: // 244: Store Literal Variable #iiiiiiii (+ extA * 256) - no pop
            {
                uint8_t indexByte = fetchByte();
                int fullIndex = (extA_ << 8) | indexByte;
                extA_ = 0;
                Oop value = stackTop();
                Oop assoc = literal(fullIndex);
                if (assoc.isObject()) {
                    memory_.storePointer(1, assoc, value);
                }
                break;
            }
            case 0xF5: // 245: Store Temporary Variable #iiiiiiii - no pop
            {
                uint8_t indexByte = fetchByte();
                Oop value = stackTop();
                setTemporary(indexByte, value);
                break;
            }
            case 0xF6: // 246: UNASSIGNED
            case 0xF7: // 247: UNASSIGNED
                fetchByte();  // Skip argument byte
                break;
        }
    }
    else {
        // ========================================================================
        // Sista V1: 0xF8-0xFF (248-255): 3-byte bytecodes
        // ========================================================================
        switch (bytecode) {
            case 0xF8: // 248: Call Primitive
            {
                // iiiiiiii mssjjjjj: primitive = iiiiiiii + (jjjjj * 256)
                // m=1: inlined primitive, ss: operation set
                uint8_t primLowByte = fetchByte();
                uint8_t flagsAndHigh = fetchByte();
                int primIndex = primLowByte | ((flagsAndHigh & 0x1F) << 8);
                // Primitive is called at method activation, this bytecode is skipped
                (void)primIndex;
                break;
            }
            case 0xF9: // 249: Push FullBlockClosure
            {
                // xxxxxxxx siyyyyyy: literal index xxxxxxxx (+extA*256)
                // numCopied yyyyyy, s=receiverOnStack, i=ignoreOuterContext
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
            case 0xFA: // 250: Push Closure
            {
                // eeiiikkk jjjjjjjj: numCopied iii (+extA//16*8), numArgs kkk (+extA\16*8)
                // blockSize jjjjjjjj (+extB*256), ee=num extension bytes
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
            case 0xFB: // 251: Push Temp At kkkkkkkk In Temp Vector At jjjjjjjj
            {
                uint8_t tempIndex = fetchByte();
                uint8_t vectorIndex = fetchByte();

                // Temp vector is always a local temp (for both methods and blocks).
                Oop tempVector = temporary(vectorIndex);
                Oop value;
                if (tempVector.isObject()) {
                    value = memory_.fetchPointer(tempIndex, tempVector);
                } else {
                    value = memory_.nil();
                }
                push(value);
                break;
            }
            case 0xFC: // 252: Store Temp At kkkkkkkk In Temp Vector At jjjjjjjj (no pop)
            {
                uint8_t tempIndex = fetchByte();
                uint8_t vectorIndex = fetchByte();
                Oop value = stackTop();

                // Temp vector is always a local temp (for both methods and blocks).
                Oop tempVector = temporary(vectorIndex);
                if (tempVector.isObject()) {
                    memory_.storePointer(tempIndex, tempVector, value);
                }
                break;
            }
            case 0xFD: // 253: Pop and Store Temp At kkkkkkkk In Temp Vector At jjjjjjjj
            {
                uint8_t tempIndex = fetchByte();
                uint8_t vectorIndex = fetchByte();
                Oop value = pop();

                // Temp vector is always a local temp (for both methods and blocks).
                Oop tempVector = temporary(vectorIndex);
                if (tempVector.isObject()) {
                    memory_.storePointer(tempIndex, tempVector, value);
                }
                break;
            }
            case 0xFE: // 254: UNASSIGNED
            case 0xFF: // 255: UNASSIGNED
                fetchByte();
                fetchByte();
                break;
        }
    }
}


// ===== STACK OPERATIONS =====

void Interpreter::push(Oop value) {
    // Stack growth diagnostic: log when stack is abnormally large
    {
        size_t sd = static_cast<size_t>(stackPointer_ - stackBase_);
        if (sd > 500) {
            static FILE* stackGrowLog = nullptr;
            static int stackGrowLogCount = 0;
            if (!stackGrowLog) stackGrowLog = nullptr;
            if (stackGrowLog && stackGrowLogCount < 500) {
                stackGrowLogCount++;
                // Get method name
                std::string mname = "?";
                if (method_.isObject() && method_.rawBits() > 0x10000) {
                    Oop hdr = memory_.fetchPointer(0, method_);
                    if (hdr.isSmallInteger()) {
                        int nl = hdr.asSmallInteger() & 0x7FFF;
                        if (nl >= 2 && nl < 100) {
                            Oop sel = memory_.fetchPointer(nl - 1, method_);
                            if (sel.isObject() && sel.rawBits() > 0x10000) {
                                ObjectHeader* sh = sel.asObjectPtr();
                                if (sh->isBytesObject() && sh->byteSize() < 50)
                                    mname = std::string((char*)sh->bytes(), sh->byteSize());
                            }
                        }
                    }
                }
                fprintf(stackGrowLog, "[PUSH sd=%zu fd=%zu bc=0x%02x step=%llu] method=#%s val=0x%llx\n",
                        sd, frameDepth_, lastBytecode_, (unsigned long long)g_stepNum,
                        mname.c_str(), (unsigned long long)value.rawBits());
                if (stackGrowLogCount % 50 == 0) fflush(stackGrowLog);
            }
        }
    }
    if (stackPointer_ >= stack_.data() + MaxStackDepth) {
        static int overflowCount = 0;
        overflowCount++;
        std::cerr << "[VM-STOP] Stack overflow #" << overflowCount << " in push() at step " << g_stepNum
                  << " frameDepth=" << frameDepth_ << " method=0x" << std::hex << method_.rawBits() << std::dec << "\n";
        // Log receiver class
        if (receiver_.isObject() && receiver_.rawBits() > 0x10000 && memory_.isValidObject(receiver_)) {
            Oop cls = memory_.classOf(receiver_);
            if (cls.isObject() && memory_.isValidObject(cls)) {
                ObjectHeader* clsHdr = cls.asObjectPtr();
                if (clsHdr->slotCount() >= 7) {
                    Oop clsName = memory_.fetchPointer(6, cls);
                    if (clsName.isObject() && memory_.isValidObject(clsName)) {
                        ObjectHeader* nameHdr = clsName.asObjectPtr();
                        if (nameHdr->isBytesObject() && nameHdr->byteSize() < 100) {
                            std::cerr << "  receiver class: " << std::string((char*)nameHdr->bytes(), nameHdr->byteSize()) << "\n";
                        }
                    }
                }
            }
        } else if (receiver_.isSmallInteger()) {
            std::cerr << "  receiver: SmallInteger(" << receiver_.asSmallInteger() << ")\n";
        }
        // Log the current method selector
        if (method_.isObject() && method_.rawBits() > 0x10000 && memory_.isValidObject(method_)) {
            ObjectHeader* mHdr = method_.asObjectPtr();
            std::cerr << "  method classIdx=" << mHdr->classIndex() << " slotCount=" << mHdr->slotCount() << "\n";
            // CompiledMethod: header is slot 0, selector is at (numLiterals)
            if (mHdr->slotCount() > 0) {
                Oop hdr = memory_.fetchPointer(0, method_);
                std::cerr << "  hdr=0x" << std::hex << hdr.rawBits() << std::dec << " isSmallInt=" << hdr.isSmallInteger() << "\n";
                if (hdr.isSmallInteger()) {
                    size_t numLits = hdr.asSmallInteger() & 0x7FFF;
                    std::cerr << "  numLits=" << numLits << "\n";
                    // Try both slot numLits and numLits-1 for selector
                    for (size_t trySlot = (numLits > 0 ? numLits - 1 : 0); trySlot <= numLits && trySlot < mHdr->slotCount(); trySlot++) {
                        Oop sel = memory_.fetchPointer(trySlot, method_);
                        if (sel.isObject() && sel.rawBits() > 0x10000 && memory_.isValidObject(sel)) {
                            ObjectHeader* selHdr = sel.asObjectPtr();
                            if (selHdr->isBytesObject() && selHdr->byteSize() < 100) {
                                std::cerr << "  selector (slot " << trySlot << "): "
                                          << std::string((char*)selHdr->bytes(), selHdr->byteSize()) << "\n";
                                break;
                            }
                        }
                    }
                }
            }
        }
        stopVM("Stack overflow in push()");
        return;
    }
    *stackPointer_++ = value;
}

Oop Interpreter::pop() {
    if (stackPointer_ <= stackBase_) {
        return memory_.nil();  // Stack underflow
    }
    return *--stackPointer_;
}

Oop Interpreter::stackTop() const {
    if (stackPointer_ <= stackBase_) {
        return memory_.nil();
    }
    return *(stackPointer_ - 1);
}

Oop Interpreter::stackValue(size_t offset) const {
    if (stackPointer_ - offset <= stackBase_) {
        return memory_.nil();
    }
    return *(stackPointer_ - 1 - offset);
}

void Interpreter::stackValuePut(size_t offset, Oop value) {
    *(stackPointer_ - 1 - offset) = value;
}

void Interpreter::popN(size_t n) {
    stackPointer_ -= n;
    if (stackPointer_ < stackBase_) {
        stackPointer_ = stackBase_;
    }
}

// ===== BYTECODE IMPLEMENTATIONS =====

uint8_t Interpreter::fetchByte() {
    // Bounds check: detect when IP is past bytecodeEnd_
    if (instructionPointer_ >= bytecodeEnd_) {
        static int ipOobCount = 0;
        static FILE* ipOobLog = nullptr;
        if (!ipOobLog) ipOobLog = nullptr;
        ipOobCount++;
        if (ipOobLog && ipOobCount <= 20) {
            fprintf(ipOobLog, "[IP-OOB #%d step=%llu] IP=%p bytecodeEnd_=%p method_=0x%llx\n",
                    ipOobCount, g_stepNum, (void*)instructionPointer_, (void*)bytecodeEnd_,
                    (unsigned long long)method_.rawBits());
            // Log method info
            if (method_.isObject() && method_.rawBits() > 0x10000) {
                ObjectHeader* mh = method_.asObjectPtr();
                Oop hdr = memory_.fetchPointer(0, method_);
                if (hdr.isSmallInteger()) {
                    size_t numLits = hdr.asSmallInteger() & 0x7FFF;
                    fprintf(ipOobLog, "  method: format=%d byteSize=%zu numLits=%zu\n",
                            (int)mh->format(), mh->byteSize(), numLits);
                    // Get selector
                    if (numLits >= 2) {
                        Oop sel = memory_.fetchPointer(numLits - 1, method_);
                        if (sel.isObject() && sel.rawBits() > 0x10000) {
                            ObjectHeader* sh = sel.asObjectPtr();
                            if (sh->isBytesObject() && sh->byteSize() < 100) {
                                std::string selStr((char*)sh->bytes(), sh->byteSize());
                                fprintf(ipOobLog, "  selector: #%s\n", selStr.c_str());
                            }
                        }
                    }
                }
            }
            fflush(ipOobLog);
        }
        // Return 0x5C (returnTop) to try to recover gracefully
        return 0x5C;
    }
    return *instructionPointer_++;
}

uint16_t Interpreter::fetchTwoBytes() {
    uint8_t hi = fetchByte();
    uint8_t lo = fetchByte();
    return (hi << 8) | lo;
}

void Interpreter::pushReceiverVariable(int index) {
    // TRACE: When accessing slot 1 on FullBlockClosure (compiledBlock slot)
    // This is the critical path for understanding why compiledBlock returns wrong value
    if (index == 1 && receiver_.isObject() && receiver_.rawBits() > 0x10000) {
        Oop rcvrCls = memory_.classOf(receiver_);
        std::string rcvrClassName;
        if (rcvrCls.isObject()) {
            Oop clsName = memory_.fetchPointer(6, rcvrCls);
            if (clsName.isObject() && clsName.rawBits() > 0x10000) {
                ObjectHeader* cnHdr = clsName.asObjectPtr();
                if (cnHdr->isBytesObject() && cnHdr->byteSize() < 50) {
                    rcvrClassName = std::string((char*)cnHdr->bytes(), cnHdr->byteSize());
                }
            }
        }
        if (rcvrClassName.find("FullBlockClosure") != std::string::npos ||
            rcvrClassName.find("BlockClosure") != std::string::npos) {
            static FILE* slot1Log = nullptr;
            static int slot1Count = 0;
            slot1Count++;
            if (slot1Count <= 30) {
                if (!slot1Log) slot1Log = nullptr;
                if (slot1Log) {
                    Oop slot1Val = memory_.fetchPointer(1, receiver_);
                    std::string slot1Method = "?";
                    if (slot1Val.isObject() && slot1Val.rawBits() > 0x10000) {
                        Oop hdr = memory_.fetchPointer(0, slot1Val);
                        if (hdr.isSmallInteger()) {
                            size_t numLits = hdr.asSmallInteger() & 0x7FFF;
                            if (numLits >= 2) {
                                Oop sel = memory_.fetchPointer(numLits - 1, slot1Val);
                                if (sel.isObject() && sel.rawBits() > 0x10000) {
                                    ObjectHeader* selHdr = sel.asObjectPtr();
                                    if (selHdr->isBytesObject() && selHdr->byteSize() < 50) {
                                        slot1Method = std::string((char*)selHdr->bytes(), selHdr->byteSize());
                                    }
                                }
                            }
                        }
                    } else if (slot1Val.isSmallInteger()) {
                        slot1Method = "<SmallInteger startPC>";
                    }
                    // Also show what method we're currently executing
                    std::string currentMethod = "?";
                    if (method_.isObject() && method_.rawBits() > 0x10000) {
                        Oop hdr = memory_.fetchPointer(0, method_);
                        if (hdr.isSmallInteger()) {
                            size_t numLits = hdr.asSmallInteger() & 0x7FFF;
                            if (numLits >= 2) {
                                Oop sel = memory_.fetchPointer(numLits - 1, method_);
                                if (sel.isObject() && sel.rawBits() > 0x10000) {
                                    ObjectHeader* selHdr = sel.asObjectPtr();
                                    if (selHdr->isBytesObject() && selHdr->byteSize() < 50) {
                                        currentMethod = std::string((char*)selHdr->bytes(), selHdr->byteSize());
                                    }
                                }
                            }
                        }
                    }
                    fprintf(slot1Log, "[BLOCK-SLOT1 #%d] %s[1] = 0x%llx method=#%s (in %s)\n",
                            slot1Count, rcvrClassName.c_str(),
                            (unsigned long long)slot1Val.rawBits(),
                            slot1Method.c_str(), currentMethod.c_str());
                    // Show receiver (block) address for correlation
                    fprintf(slot1Log, "  block=0x%llx\n", (unsigned long long)receiver_.rawBits());
                    fflush(slot1Log);
                }
            }
        }
    }

    // Trace ALL instance variable reads to debug SessionManager default
    if constexpr (ENABLE_DEBUG_LOGGING) {
        static FILE* civLog = nullptr;
        static int civCount = 0;
        if (!civLog) civLog = nullptr;

        // Check if receiver is SessionManager - trace bytecodes
        if (receiver_.isObject() && receiver_.rawBits() > 0x10000) {
            Oop cls = memory_.classOf(receiver_);
            if (cls.isObject()) {
                Oop clsName = memory_.fetchPointer(6, cls);
                if (clsName.isObject() && clsName.rawBits() > 0x10000) {
                    ObjectHeader* cnHdr = clsName.asObjectPtr();
                    if (cnHdr->isBytesObject() && cnHdr->byteSize() == 14) {
                        std::string n((char*)cnHdr->bytes(), 14);
                        if (n == "SessionManager") {
                            static FILE* smLog = nullptr;
                            static int smCount = 0;
                            if (!smLog) smLog = nullptr;
                            if (smLog && smCount < 20) {
                                smCount++;
                                // Show current method
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
                                                    } else if (slHdr->format() == ObjectFormat::FixedSize && slHdr->slotCount() >= 1) {
                                                        Oop inner = memory_.fetchPointer(0, selLit);
                                                        if (inner.isObject() && inner.rawBits() > 0x10000) {
                                                            ObjectHeader* iHdr = inner.asObjectPtr();
                                                            if (iHdr->isBytesObject() && iHdr->byteSize() < 50) {
                                                                methodSel = std::string((char*)iHdr->bytes(), iHdr->byteSize());
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                                Oop val = memory_.fetchPointer(index, receiver_);
                                fprintf(smLog, "[SM-READ #%d] SessionManager >> %s reads slot[%d] = 0x%llx (ip=%zu method=0x%llx)\n",
                                        smCount, methodSel.c_str(), index,
                                        (unsigned long long)val.rawBits(),
                                        instructionPointer_ - method_.asObjectPtr()->bytes(),
                                        (unsigned long long)method_.rawBits());
                                fflush(smLog);
                            }
                        }
                    }
                }
            }
        }

        // Also log when we read nil from any object at index > 10 (likely class instvar)
        Oop result = memory_.fetchPointer(index, receiver_);
        Oop nilObj = memory_.nil();

        if (civLog && civCount < 200) {
            bool shouldLog = false;
            std::string rcvrName = "<unknown>";

            // Check if receiver is a class object
            if (receiver_.isObject() && receiver_.rawBits() > 0x10000) {
                ObjectHeader* rcvrHdr = receiver_.asObjectPtr();

                // Get class name for classes, or instance class name for others
                if (rcvrHdr->slotCount() >= 7) {
                    Oop nameOop = memory_.fetchPointer(6, receiver_);
                    if (nameOop.isObject() && nameOop.rawBits() > 0x10000) {
                        ObjectHeader* nameHdr = nameOop.asObjectPtr();
                        if (nameHdr->isBytesObject() && nameHdr->byteSize() < 50) {
                            rcvrName = std::string((char*)nameHdr->bytes(), nameHdr->byteSize());
                            shouldLog = true;
                        }
                    }
                }

                // Also get instance class name if above didn't work
                if (rcvrName == "<unknown>") {
                    Oop cls = memory_.classOf(receiver_);
                    if (cls.isObject()) {
                        Oop clsName = memory_.fetchPointer(6, cls);
                        if (clsName.isObject() && clsName.rawBits() > 0x10000) {
                            ObjectHeader* cnHdr = clsName.asObjectPtr();
                            if (cnHdr->isBytesObject() && cnHdr->byteSize() < 50) {
                                rcvrName = "[" + std::string((char*)cnHdr->bytes(), cnHdr->byteSize()) + " instance]";
                            }
                        }
                    }
                }

                // Log if: reading from class object at high index OR reading nil from any index > 10
                // OR if name contains "Session"
                bool isHighIndex = (index >= 11);
                bool isNilResult = (result.rawBits() == nilObj.rawBits());
                bool isSessionRelated = (rcvrName.find("Session") != std::string::npos);

                if (isSessionRelated || (isHighIndex && shouldLog) || civCount < 30) {
                    civCount++;
                    fprintf(civLog, "[INSTVAR #%d] %s slot[%d] = 0x%llx%s (receiver=0x%llx)\n",
                            civCount, rcvrName.c_str(), index,
                            (unsigned long long)result.rawBits(),
                            isNilResult ? " [NIL!]" : "",
                            (unsigned long long)receiver_.rawBits());
                    fflush(civLog);
                }
            }
        }
    }
    Oop result = memory_.fetchPointer(index, receiver_);
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
                // Get method selector for context
                std::string methodSel = "<unknown>";
                if (method_.isObject() && method_.rawBits() > 0x10000) {
                    ObjectHeader* mHdr = method_.asObjectPtr();
                    Oop hdr = memory_.fetchPointer(0, method_);
                    if (hdr.isSmallInteger()) {
                        size_t numLits = hdr.asSmallInteger() & 0x7FFF;
                        if (numLits >= 2) {
                            Oop sel = memory_.fetchPointer(numLits - 1, method_);
                            if (sel.isObject() && sel.rawBits() > 0x10000) {
                                ObjectHeader* selHdr = sel.asObjectPtr();
                                if (selHdr->isBytesObject() && selHdr->byteSize() < 50) {
                                    methodSel = std::string((char*)selHdr->bytes(), selHdr->byteSize());
                                }
                            }
                        }
                    }
                }
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
    // V3PlusClosures: Simple literal push, no extensions
    // The index is already the full literal index (0-31 from bytecode 0x20-0x3F,
    // or 0-63 from extended push bytecode 0x80)
    Oop val = literal(index);
    push(val);
}

void Interpreter::pushLiteralVariable(int index) {
    // V3PlusClosures: Simple literal variable push, no extensions
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
    Oop value = memory_.fetchPointer(1, assoc);  // Association>>value

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
                // Fix unrelocated sender pointer before dereferencing
                {
                    const uint64_t OLD_IMAGE_BASE = 0x10000000000ULL;
                    uint64_t sndAddr = sender.rawBits() & ~7ULL;
                    if (sndAddr >= OLD_IMAGE_BASE && sndAddr < OLD_IMAGE_BASE * 2) {
                        uint64_t offset = sndAddr - OLD_IMAGE_BASE;
                        uint64_t newAddr = reinterpret_cast<uint64_t>(memory_.oldSpaceStart()) + offset;
                        sender = memory_.oopFromPointer(reinterpret_cast<ObjectHeader*>(newAddr));
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

                    // Execute from sender, which will push the return value appropriately
                    // First, mark the returning context as dead per Cog VM semantics:
                    // nil the sender and PC so isDead returns true and sender chain is broken.
                    memory_.storePointer(0, activeContext_, memory_.nil());  // sender = nil
                    memory_.storePointer(1, activeContext_, memory_.nil());  // pc = nil → isDead

                    if (executeFromContext(sender)) {
                        // Push the return value onto the new context's stack
                        push(value);
                        // TRACE: context-based return to isValid
                        {
                            static int ctxRetTrace = 0;
                            if (ctxRetTrace < 5) {
                                bool isIsValid = false;
                                if (method_.isObject() && method_.rawBits() > 0x10000) {
                                    Oop hdr2 = memory_.fetchPointer(0, method_);
                                    if (hdr2.isSmallInteger()) {
                                        int nl2 = hdr2.asSmallInteger() & 0x7FFF;
                                        if (nl2 >= 2) {
                                            Oop sel2 = memory_.fetchPointer(nl2 - 1, method_);
                                            if (sel2.isObject() && sel2.rawBits() > 0x10000) {
                                                ObjectHeader* sh2 = sel2.asObjectPtr();
                                                if (sh2->isBytesObject() && sh2->byteSize() == 7 &&
                                                    memcmp(sh2->bytes(), "isValid", 7) == 0)
                                                    isIsValid = true;
                                            }
                                        }
                                    }
                                }
                                if (isIsValid) {
                                    ctxRetTrace++;
                                    int sd2 = static_cast<int>(stackPointer_ - stackBase_);
                                    Oop tos2 = stackTop();
                                    fprintf(stderr, "[CTX-RETURN-ISVALID] #%d TOS=0x%llx pushed=0x%llx sd=%d\n",
                                            ctxRetTrace, (unsigned long long)tos2.rawBits(),
                                            (unsigned long long)value.rawBits(), sd2);
                                    fprintf(stderr, "[CTX-RETURN-ISVALID]   stack:");
                                    for (int si = 0; si < std::min(sd2, 5); si++) {
                                        Oop sv2 = *(stackPointer_ - 1 - si);
                                        fprintf(stderr, " [%d]=0x%llx", si, (unsigned long long)sv2.rawBits());
                                    }
                                    fprintf(stderr, "\n");
                                    fflush(stderr);
                                }
                            }
                        }
                        return;
                    }
                }
            }
        }

terminate_process:
        // Mark current process as terminated by clearing its suspendedContext
        terminateCurrentProcess();

        // Check if we need to call setupEventLoop after install completed
        if (hasPendingDriverSetup_ && pendingDriverSetupMethod_.isObject()) {

            // Get the driver instance from OSWindowDriver's Current class variable
            Oop nilObj = memory_.nil();
            Oop osWindowDriverClass = memory_.findGlobal("OSWindowDriver");
            Oop driverInstance = Oop::nil();

            if (osWindowDriverClass.isObject() && osWindowDriverClass.rawBits() != nilObj.rawBits()) {
                // ClassPool is at slot 7
                Oop classPool = memory_.fetchPointer(7, osWindowDriverClass);
                if (classPool.isObject() && classPool.rawBits() != nilObj.rawBits()) {
                    ObjectHeader* poolHdr = classPool.asObjectPtr();
                    if (poolHdr->slotCount() >= 2) {
                        Oop assocArray = memory_.fetchPointer(1, classPool);
                        if (assocArray.isObject()) {
                            ObjectHeader* arrayHdr = assocArray.asObjectPtr();
                            for (size_t i = 0; i < arrayHdr->slotCount(); i++) {
                                Oop assoc = memory_.fetchPointer(i, assocArray);
                                if (assoc.isObject() && assoc.rawBits() != nilObj.rawBits()) {
                                    ObjectHeader* assocHdr = assoc.asObjectPtr();
                                    if (assocHdr->slotCount() >= 2) {
                                        Oop key = memory_.fetchPointer(0, assoc);
                                        if (key.isObject()) {
                                            ObjectHeader* keyHdr = key.asObjectPtr();
                                            if (keyHdr->isBytesObject() && keyHdr->byteSize() == 7) {
                                                std::string keyName((char*)keyHdr->bytes(), keyHdr->byteSize());
                                                if (keyName == "Current") {
                                                    driverInstance = memory_.fetchPointer(1, assoc);
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

            if (driverInstance.isObject() && driverInstance.rawBits() != nilObj.rawBits()) {
                // Schedule setupEventLoop on the driver instance
                pendingDriverInstallMethod_ = pendingDriverSetupMethod_;
                pendingDriverInstallReceiver_ = driverInstance;
                hasPendingDriverInstall_ = true;
                pendingDriverMethodNeedsArg_ = false;

                // Clear setup state
                hasPendingDriverSetup_ = false;
                pendingDriverSetupMethod_ = Oop::nil();
                pendingDriverSetupReceiver_ = Oop::nil();

                // Execute it now (like executePendingDriverInstall does)
                if (executePendingDriverInstall()) {
                    return;  // Let the method run
                }
            }

            // Clear setup state even if we couldn't call it
            hasPendingDriverSetup_ = false;
            pendingDriverSetupMethod_ = Oop::nil();
        }

        // Try to find another runnable process
        if (tryReschedule()) {
            return;
        }

        // If no other process to run, try startup entry point
        if (bootstrapStartup()) {
            return;
        }

        // No runnable processes - idle loop until one becomes available.
        // We must NOT return to the bytecode loop here because the current
        // process has been terminated and its method/IP are no longer valid.
        // Keep trying to find a runnable process - never give up in a GUI app
        {
            // Clear method_ so we can detect if signal processing activates a process
            // via synchronousSignal() → transferTo() → executeFromContext()
            method_ = Oop::nil();

            while (running_) {

                // GC safe point: no process is active, safe to compact
                if (memory_.needsCompactGC()) {
                    memory_.clearCompactGCFlag();
                    memory_.fullGC();
                    flushMethodCache();  // Compaction moves objects — stale cache
                }

                // Process pending external semaphore signals (from events, timers)
                if (hasPendingSignals()) {
                    processPendingSignals();
                }

                // Process input events - may signal semaphores that wake processes
                processInputEvents();

                // Check timer semaphore - may wake delay processes
                checkTimerSemaphore();

                // If any signal processing above called synchronousSignal() →
                // transferTo() → executeFromContext(), method_ will be set to
                // the new process's method. Return to the bytecode loop to run it.
                if (!method_.isNil() && method_.isObject()) {
                    return;
                }

                // Pump the native run loop so UIKit can deliver touch/hover
                // events and Metal can render frames.  Without this, the main
                // thread is blocked and no events arrive — creating a deadlock
                // where we wait for events that can never be delivered.
#if __APPLE__
                if (relinquishCallback_) {
                    // Pump the run loop multiple times to fully process events.
                    // A single pump may only handle hitTest; the actual touch
                    // delivery happens in a subsequent run loop iteration.
                    for (int pump = 0; pump < 10; pump++) {
                        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, true);
                    }
                    // Then sleep briefly to cap CPU usage
                    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.010, false);
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
#else
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
#endif

                // Diagnostic: detect stranded events in gEventQueue
                {
                    static int idleIterations = 0;
                    idleIterations++;
                    size_t qSize = pharo::gEventQueue.size();
                    if (qSize > 0 && (idleIterations % 100 == 0)) {
                        fprintf(stderr, "[IDLE-STRANDED] iter=%d queueSize=%zu timerSem=%s wakeup=%s\n",
                                idleIterations, qSize,
                                timerSemaphore_.isNil() ? "nil" : "set",
                                nextWakeupUsec_ == INT64_MAX ? "none" : "armed");
                    }
                    if (idleIterations % 500 == 0) {
                        fprintf(stderr, "[IDLE-STATE] iter=%d queueSize=%zu timerSem=%s wakeup=%s\n",
                                idleIterations, qSize,
                                timerSemaphore_.isNil() ? "nil" : "set",
                                nextWakeupUsec_ == INT64_MAX ? "none" : "armed");
                    }
                }

                // Try to find a runnable process
                if (tryReschedule()) {
                    return;
                }
            }
            return;
        }
    }


    // Debug: track method_ changes through popFrame
    if constexpr (ENABLE_DEBUG_LOGGING) {
        static FILE* methodChangeLog = nullptr;
        static int methodChangeCount = 0;
        if (!methodChangeLog) methodChangeLog = nullptr;

        // Get method name BEFORE popFrame
        std::string beforeMethod = "<unknown>";
        if (method_.isObject() && method_.rawBits() > 0x10000) {
            Oop mHdr = memory_.fetchPointer(0, method_);
            if (mHdr.isSmallInteger()) {
                size_t numLits = mHdr.asSmallInteger() & 0x7FFF;
                if (numLits >= 2) {
                    Oop sel = memory_.fetchPointer(numLits - 1, method_);
                    if (sel.isObject() && sel.rawBits() > 0x10000) {
                        ObjectHeader* selHdr = sel.asObjectPtr();
                        if (selHdr->isBytesObject() && selHdr->byteSize() < 50) {
                            beforeMethod = std::string((char*)selHdr->bytes(), selHdr->byteSize());
                        }
                    }
                }
            }
        }

        // Get method that WILL be restored
        std::string willBeRestoredTo = "<none>";
        if (frameDepth_ > 0) {
            const auto& frame = savedFrames_[frameDepth_ - 1];
            if (frame.savedMethod.isObject() && frame.savedMethod.rawBits() > 0x10000) {
                Oop mHdr = memory_.fetchPointer(0, frame.savedMethod);
                if (mHdr.isSmallInteger()) {
                    size_t numLits = mHdr.asSmallInteger() & 0x7FFF;
                    if (numLits >= 2) {
                        Oop sel = memory_.fetchPointer(numLits - 1, frame.savedMethod);
                        if (sel.isObject() && sel.rawBits() > 0x10000) {
                            ObjectHeader* selHdr = sel.asObjectPtr();
                            if (selHdr->isBytesObject() && selHdr->byteSize() < 50) {
                                willBeRestoredTo = std::string((char*)selHdr->bytes(), selHdr->byteSize());
                            }
                        }
                    }
                }
            }
        }

        // Pop frame and push result
        popFrame();

        // Get method name AFTER popFrame
        std::string afterMethod = "<unknown>";
        if (method_.isObject() && method_.rawBits() > 0x10000) {
            Oop mHdr = memory_.fetchPointer(0, method_);
            if (mHdr.isSmallInteger()) {
                size_t numLits = mHdr.asSmallInteger() & 0x7FFF;
                if (numLits >= 2) {
                    Oop sel = memory_.fetchPointer(numLits - 1, method_);
                    if (sel.isObject() && sel.rawBits() > 0x10000) {
                        ObjectHeader* selHdr = sel.asObjectPtr();
                        if (selHdr->isBytesObject() && selHdr->byteSize() < 50) {
                            afterMethod = std::string((char*)selHdr->bytes(), selHdr->byteSize());
                        }
                    }
                }
            }
        }

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
        popFrame();
    }

    // After popping, if execution is still running, push the result
    if (running_) {
        push(value);

        // TRACE: verify stack after isNull return to isValid
        {
            static int postReturnTrace = 0;
            if (postReturnTrace < 5) {
                // Check if restored method is isValid
                bool isIsValid = false;
                if (method_.isObject() && method_.rawBits() > 0x10000) {
                    Oop hdr = memory_.fetchPointer(0, method_);
                    if (hdr.isSmallInteger()) {
                        int nl = hdr.asSmallInteger() & 0x7FFF;
                        if (nl >= 2) {
                            Oop sel = memory_.fetchPointer(nl - 1, method_);
                            if (sel.isObject() && sel.rawBits() > 0x10000) {
                                ObjectHeader* sh = sel.asObjectPtr();
                                if (sh->isBytesObject() && sh->byteSize() == 7 &&
                                    memcmp(sh->bytes(), "isValid", 7) == 0) {
                                    isIsValid = true;
                                }
                            }
                        }
                    }
                }
                if (isIsValid) {
                    postReturnTrace++;
                    const char* tosType = "?";
                    Oop tos = stackTop();
                    if (tos.rawBits() == memory_.trueObject().rawBits()) tosType = "TRUE";
                    else if (tos.rawBits() == memory_.falseObject().rawBits()) tosType = "FALSE";
                    else if (tos.isNil()) tosType = "nil";
                    else if (tos.isSmallInteger()) tosType = "SmallInt";
                    else {
                        tosType = "OBJECT";
                    }
                    int sd = static_cast<int>(stackPointer_ - stackBase_);
                    fprintf(stderr, "[POST-RETURN-ISVALID] #%d TOS=%s(0x%llx) pushed=%s(0x%llx) sd=%d fd=%zu\n",
                            postReturnTrace, tosType, (unsigned long long)tos.rawBits(),
                            value.rawBits() == memory_.trueObject().rawBits() ? "TRUE" :
                            value.rawBits() == memory_.falseObject().rawBits() ? "FALSE" :
                            "OTHER", (unsigned long long)value.rawBits(), sd, frameDepth_);
                    // Show a few stack items
                    fprintf(stderr, "[POST-RETURN-ISVALID]   stack:");
                    for (int si = 0; si < std::min(sd, 5); si++) {
                        Oop sv = *(stackPointer_ - 1 - si);
                        fprintf(stderr, " [%d]=0x%llx", si, (unsigned long long)sv.rawBits());
                    }
                    fprintf(stderr, "\n");
                    fflush(stderr);
                }
            }
        }
    }
}

void Interpreter::returnFromMethod() {
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
                        // Home method IS in savedFrames_ — use inline NLR instead
                        size_t homeFrame = si;
                        while (frameDepth_ > homeFrame) {
                            // Check ensure: in unwind path
                            if (frameDepth_ > 1) {
                                Oop rm = savedFrames_[frameDepth_ - 1].savedMethod;
                                if (rm.isObject() && rm.rawBits() > 0x10000 &&
                                    primitiveIndexOf(rm) == 198) {
                                    popFrame();
                                    push(value);
                                    if (frameDepth_ > 0) savedFrames_[frameDepth_ - 1].homeFrameDepth = homeFrame;
                                    return;
                                }
                            }
                            popFrame();
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
                                    popFrame();
                                    // Now method_ = ensure:, IP = after valueNoContextSwitch
                                    // Push the NLR return value as the result of valueNoContextSwitch
                                    push(value);
                                    // Set homeFrameDepth so that when ensure: does ^returnValue,
                                    // the NLR continues to the original target.
                                    if (frameDepth_ > 0) {
                                        savedFrames_[frameDepth_ - 1].homeFrameDepth = homeFrame;
                                    }
                                    return;  // Let interpreter run ensure:'s continuation
                                }
                            }
                        }
                    }
                }
                popFrame();
            }
            // Now we're at homeFrame, returnValue pops this frame and returns to caller
            returnValue(value);
            return;
        }
    }

    // CONTEXT-BASED NLR: When frameDepth_ == 0 and we're in a CompiledBlock,
    // we need to use the context chain to find the home method and unwind to it.
    // This happens when exception handling (on:do:) caused context materialization.
    {
        Oop homeMethod = Oop::nil();
        bool isCompiledBlock = false;

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
                            if (!ecHeader.isSmallInteger()) { homeMethod = enclosing; break; }
                            int ecNumLits = ecHeader.asSmallInteger() & 0x7FFF;
                            if (ecNumLits < 1) { homeMethod = enclosing; break; }
                            Oop ecLastLit = memory_.fetchPointer(ecNumLits, enclosing);
                            bool isBlock = false;
                            if (ecLastLit.isObject() && ecLastLit.rawBits() > 0x10000) {
                                isBlock = ecLastLit.asObjectPtr()->isCompiledMethod();
                            }
                            if (!isBlock) { homeMethod = enclosing; break; }
                            enclosing = ecLastLit;
                            chainDepth++;
                        }
                        if (homeMethod.isNil() && enclosing.isObject() && enclosing.rawBits() > 0x10000) {
                            if (enclosing.asObjectPtr()->isCompiledMethod()) {
                                isCompiledBlock = true;
                                homeMethod = enclosing;
                            }
                        }
                    }
                }
            }
        }

        // If we're in a CompiledBlock and frameDepth_ == 0, do context-based NLR
        if (isCompiledBlock && frameDepth_ == 0 && homeMethod.isObject() && !homeMethod.isNil()) {
            // Walk up the context chain to find the context executing homeMethod
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
        // Unwind frames from current down to homeFrame + 1, checking for ensure: at each level
        while (frameDepth_ > homeFrame + 1) {
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
                                popFrame();
                                // Push the NLR return value as the result of valueNoContextSwitch
                                push(value);
                                // Set this frame's homeFrameDepth so that when ensure: does
                                // ^returnValue, the NLR continues to the original target.
                                if (frameDepth_ > 0) {
                                    savedFrames_[frameDepth_ - 1].homeFrameDepth = homeFrame;
                                }
                                return;  // Let interpreter run ensure:'s continuation
                            }
                        }
                    }
                }
            }
            popFrame();
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
    push(activeContext_);  // receiver: the context that cannot return
    push(value);           // arg: the value that was being returned
    sendSelector(selectors_.cannotReturn, 1);
}

// Handle unwind (ensure:) contexts during context-based non-local returns.
// Walks the sender chain from startCtx to homeCtx looking for contexts whose
// method has primitive 198 (the ensure:/ifCurtailed: marker). If found, sends
// aboutToReturn:through: to the current context, letting Smalltalk handle the
// unwind. Returns true if an unwind context was found.
bool Interpreter::handleContextNLRUnwind(Oop value, Oop startCtx, Oop homeCtx) {
    Oop ctx = startCtx;
    int depth = 0;

    while (ctx.isObject() && !ctx.isNil() && depth < 200) {
        if (ctx.rawBits() == homeCtx.rawBits()) break;  // Reached home, no unwind found

        Oop method = memory_.fetchPointer(3, ctx);  // slot 3 = method
        if (method.isObject() && !method.isNil()) {
            if (primitiveIndexOf(method) == 198) {
                // Found an unwind context (ensure:/ifCurtailed:).
                // Per the reference VM, send aboutToReturn:through: to the
                // current context and let Smalltalk handle the unwind.
                // Send aboutToReturn:through: to activeContext_
                // receiver = current context, arg1 = return value, arg2 = unwind context
                push(activeContext_);  // receiver
                push(value);           // arg 1: the return value
                push(ctx);             // arg 2: the first unwind context
                sendSelector(selectors_.aboutToReturn, 2);
                return true;
            }
        }
        ctx = memory_.fetchPointer(0, ctx);  // sender = slot 0
        depth++;
    }

    // Reference VM also checks if homeCtx itself has prim 198
    // (e.g., the home IS an ensure: context)
    if (ctx.isObject() && !ctx.isNil() && ctx.rawBits() == homeCtx.rawBits()) {
        Oop method = memory_.fetchPointer(3, homeCtx);
        if (method.isObject() && !method.isNil() && primitiveIndexOf(method) == 198) {
            push(activeContext_);
            push(value);
            push(homeCtx);
            sendSelector(selectors_.aboutToReturn, 2);
            return true;
        }
    }

    return false;
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

    // Debug super sends - especially for Context newForMethod:
    static int superSendDebug = 0;
    bool traceThis = false;
    std::string selectorStr;
    if (selector.isObject() && memory_.isValidPointer(selector)) {
        ObjectHeader* selHdr = selector.asObjectPtr();
        if (selHdr->isBytesObject() && selHdr->byteSize() < 50) {
            selectorStr = std::string((char*)selHdr->bytes(), selHdr->byteSize());
            if (selectorStr == "basicNew:") {
                traceThis = true;
            }
        }
    }

    if (traceThis && superSendDebug < 20) {
        superSendDebug++;
        FILE* f = fopen("/tmp/super_send_debug.txt", "a");
        if (f) {
            fprintf(f, "[SUPER #%d] selector=%s\n", superSendDebug, selectorStr.c_str());
            fprintf(f, "  methodClass=0x%llx isNil=%d\n",
                    (unsigned long long)methodClass.rawBits(), methodClass.isNil() ? 1 : 0);
            if (methodClass.isObject() && memory_.isValidPointer(methodClass)) {
                Oop mcName = memory_.fetchPointer(6, methodClass);
                if (mcName.isObject() && mcName.rawBits() > 0x10000) {
                    ObjectHeader* mcnHdr = mcName.asObjectPtr();
                    if (mcnHdr->isBytesObject() && mcnHdr->byteSize() < 80) {
                        fprintf(f, "  methodClass name=%.*s\n",
                                (int)mcnHdr->byteSize(), (char*)mcnHdr->bytes());
                    }
                }
            }
            fprintf(f, "  receiver_ class=0x%llx\n",
                    (unsigned long long)memory_.classOf(receiver_).rawBits());
            fclose(f);
        }
    }

    if (methodClass.isNil() || !methodClass.isObject()) {
        // Fallback to receiver's class superclass
        superclass = superclassOf(memory_.classOf(receiver_));
        if (traceThis && superSendDebug <= 20) {
            FILE* f = fopen("/tmp/super_send_debug.txt", "a");
            if (f) {
                fprintf(f, "  FALLBACK: using receiver's class superclass\n");
                fclose(f);
            }
        }
    } else {
        superclass = superclassOf(methodClass);
    }

    if (traceThis && superSendDebug <= 20) {
        FILE* f = fopen("/tmp/super_send_debug.txt", "a");
        if (f) {
            fprintf(f, "  superclass for lookup=0x%llx\n", (unsigned long long)superclass.rawBits());
            if (superclass.isObject() && memory_.isValidPointer(superclass)) {
                Oop scName = memory_.fetchPointer(6, superclass);
                if (scName.isObject() && scName.rawBits() > 0x10000) {
                    ObjectHeader* scnHdr = scName.asObjectPtr();
                    if (scnHdr->isBytesObject() && scnHdr->byteSize() < 80) {
                        fprintf(f, "  superclass name=%.*s\n",
                                (int)scnHdr->byteSize(), (char*)scnHdr->bytes());
                    }
                }
            }
            fclose(f);
        }
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

                // Get receiver class
                std::string rcvrClass = "SmallInt";
                if (!rcvr.isSmallInteger()) {
                    if (rcvr.isSmallFloat()) rcvrClass = "SmallFloat";
                    else if (rcvr.isCharacter()) rcvrClass = "Character";
                    else if (rcvr.isObject() && rcvr.rawBits() > 0x10000) {
                        Oop cls = memory_.classOf(rcvr);
                        if (cls.isObject()) {
                            ObjectHeader* clsHdr = cls.asObjectPtr();
                            if (clsHdr->slotCount() >= 7) {
                                Oop nm = clsHdr->slotAt(6);
                                if (nm.isObject() && nm.rawBits() > 0x10000) {
                                    ObjectHeader* nmHdr = nm.asObjectPtr();
                                    if (nmHdr->isBytesObject() && nmHdr->byteSize() < 50) {
                                        rcvrClass = std::string((char*)nmHdr->bytes(), nmHdr->byteSize());
                                    }
                                }
                            }
                        }
                    }
                }

                // Get arg class
                std::string argClass = "SmallInt";
                if (!arg.isSmallInteger()) {
                    if (arg.isSmallFloat()) argClass = "SmallFloat";
                    else if (arg.isCharacter()) argClass = "Character";
                    else if (arg.isObject() && arg.rawBits() > 0x10000) {
                        Oop cls = memory_.classOf(arg);
                        if (cls.isObject()) {
                            ObjectHeader* clsHdr = cls.asObjectPtr();
                            if (clsHdr->slotCount() >= 7) {
                                Oop nm = clsHdr->slotAt(6);
                                if (nm.isObject() && nm.rawBits() > 0x10000) {
                                    ObjectHeader* nmHdr = nm.asObjectPtr();
                                    if (nmHdr->isBytesObject() && nmHdr->byteSize() < 50) {
                                        argClass = std::string((char*)nmHdr->bytes(), nmHdr->byteSize());
                                    }
                                }
                            }
                        }
                    }
                }

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
            static FILE* arithLog = nullptr;
            static int arithCount = 0;
            if (!arithLog) arithLog = nullptr;
            if (arithLog && arithCount < 500) {
                arithCount++;
                Oop rcvr = stackValue(1);
                Oop arg = stackValue(0);
                if (rcvr.isSmallInteger() && arg.isSmallInteger()) {
                    fprintf(arithLog, "[<= #%d] %lld <= %lld fd=%d\n",
                            arithCount, rcvr.asSmallInteger(), arg.asSmallInteger(), (int)frameDepth_);
                } else {
                    fprintf(arithLog, "[<= #%d] rcvr=0x%llx arg=0x%llx (non-int) fd=%d\n",
                            arithCount, (unsigned long long)rcvr.rawBits(),
                            (unsigned long long)arg.rawBits(), (int)frameDepth_);
                }
                fflush(arithLog);
            }
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
        // Fallback: return receiver for unsupported operations
        pop();  // Pop argument, leave receiver
        return;
    }

    sendSelector(selector, argCount);
}

void Interpreter::commonSend(int which) {
    // In Sista V1, bytecodes 192-207 are "send special selector 16-31"
    // These use the special selectors array (special object index 23)
    // The array format is: [selector0, argCount0, selector1, argCount1, ...]
    // Bytecode 192 sends special selector 16, bytecode 207 sends special selector 31

    int selectorIndex = which + 16;  // Offset by 16 from the arithmetic sends

    // Get special selectors array
    Oop specialSelectors = memory_.specialObject(SpecialObjectIndex::SpecialSelectorsArray);
    if (!specialSelectors.isObject() || specialSelectors.rawBits() < 0x10000) {
        static int ssErrCount = 0;
        if (++ssErrCount <= 5) {
            std::cerr << "[SPECIAL-SEND] ERROR: Special selectors array not found (which=" << which << ")\n";
        }
        // Cannot proceed without special selectors - return receiver as no-op
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

    // Debug selector string construction removed for performance
    // (was constructing std::string on every common send)

    // Fallback for stream operations during startup to avoid DNU
    // Special selector 20 is 'nextPut:' which is commonly used in logging
    if (selectorIndex == 20 && argCount == 1) {  // nextPut:
        Oop rcvr = stackValue(1);  // Receiver is under the argument

        // If receiver doesn't look like a stream (not an object or is a special object),
        // just pop args and return the argument (stream operations return the argument)
        if (!rcvr.isObject() || rcvr.rawBits() == 0) {
            // std::cerr << "[COMMON] Fallback for nextPut: on non-stream - returning argument"; // DEBUG
            Oop arg = pop();  // Pop the argument
            pop();  // Pop the receiver
            push(arg);  // Return the argument (standard stream behavior)
            return;
        }
    }

    // Fallback for selector 21 (next) during startup
    if (selectorIndex == 21 && argCount == 0) {  // next
        Oop rcvr = stackValue(0);
        if (!rcvr.isObject() || rcvr.rawBits() == 0) {
            // std::cerr << "[COMMON] Fallback for next on non-stream - returning nil"; // DEBUG
            pop();  // Pop receiver
            push(memory_.nil());  // Return nil
            return;
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

    // Trace value: (which=10) sends with nil arguments
    if constexpr (ENABLE_DEBUG_LOGGING) {
    if (which == 10) {  // value:
        Oop arg = stackValue(0);
        Oop rcvr = stackValue(1);
        Oop nilObj = memory_.nil();
        static FILE* valueSendLog = nullptr;
        static int valueSendCount = 0;
        if (!valueSendLog) valueSendLog = nullptr;
        if (valueSendLog && valueSendCount < 200 && arg.rawBits() == nilObj.rawBits()) {
            valueSendCount++;
            std::string rcvrClass = "<unknown>";
            if (rcvr.isObject() && rcvr.rawBits() > 0x10000) {
                Oop cls = memory_.classOf(rcvr);
                if (cls.isObject()) {
                    Oop clsName = memory_.fetchPointer(6, cls);
                    if (clsName.isObject() && clsName.rawBits() > 0x10000) {
                        ObjectHeader* cnHdr = clsName.asObjectPtr();
                        if (cnHdr->isBytesObject() && cnHdr->byteSize() < 50) {
                            rcvrClass = std::string((char*)cnHdr->bytes(), cnHdr->byteSize());
                        }
                    }
                }
            }
            fprintf(valueSendLog, "[VALUE: #%d] arg=nil rcvr=%s (0x%llx) depth=%zu\n",
                    valueSendCount, rcvrClass.c_str(), (unsigned long long)rcvr.rawBits(), frameDepth_);
            // Show recent bytecodes and method context
            if (method_.isObject() && method_.rawBits() > 0x10000) {
                ObjectHeader* mHdr = method_.asObjectPtr();
                Oop hdr = memory_.fetchPointer(0, method_);
                if (hdr.isSmallInteger()) {
                    size_t numLits = hdr.asSmallInteger() & 0x7FFF;
                    size_t bcStart = (1 + numLits) * 8;
                    size_t ip = instructionPointer_ - mHdr->bytes();
                    // Get method selector
                    std::string methodSel = "<unknown>";
                    if (numLits >= 2) {
                        Oop sel = memory_.fetchPointer(numLits - 1, method_);
                        if (sel.isObject() && sel.rawBits() > 0x10000) {
                            ObjectHeader* selHdr = sel.asObjectPtr();
                            if (selHdr->isBytesObject() && selHdr->byteSize() < 50) {
                                methodSel = std::string((char*)selHdr->bytes(), selHdr->byteSize());
                            }
                        }
                    }
                    fprintf(valueSendLog, "  in method: %s ip=%zu bcStart=%zu\n", methodSel.c_str(), ip, bcStart);
                    // Show bytecodes leading up to value:
                    fprintf(valueSendLog, "  bytecodes before value:: ");
                    for (int i = -10; i <= 0; i++) {
                        if ((int)ip + i >= (int)bcStart) {
                            uint8_t bc = mHdr->bytes()[(int)ip + i];
                            fprintf(valueSendLog, "%02x ", bc);
                        }
                    }
                    fprintf(valueSendLog, "\n");
                }
            }
            // Show stack around the nil
            fprintf(valueSendLog, "  stack: ");
            for (int i = 0; i < 5; i++) {
                Oop sv = stackValue(i);
                fprintf(valueSendLog, "[%d]=0x%llx ", i, (unsigned long long)sv.rawBits());
            }
            fprintf(valueSendLog, "\n");
            // Show the BLOCK's bytecodes to understand if it has ifNotNil: check
            if (rcvr.isObject() && rcvr.rawBits() > 0x10000) {
                ObjectHeader* blockHdr = rcvr.asObjectPtr();
                // FullBlockClosure slot 1 = compiledBlock
                if (blockHdr->slotCount() > 1) {
                    Oop compiledBlock = blockHdr->slotAt(1);
                    if (compiledBlock.isObject() && compiledBlock.rawBits() > 0x10000) {
                        ObjectHeader* cbHdr = compiledBlock.asObjectPtr();
                        Oop cbMethHdr = memory_.fetchPointer(0, compiledBlock);
                        if (cbMethHdr.isSmallInteger()) {
                            size_t numLits = cbMethHdr.asSmallInteger() & 0x7FFF;
                            size_t bcStart = (1 + numLits) * 8;
                            size_t totalBytes = cbHdr->byteSize();
                            fprintf(valueSendLog, "  Block's bytecodes (%zu lits, bc start @%zu, total %zu): ",
                                    numLits, bcStart, totalBytes);
                            for (size_t i = bcStart; i < totalBytes && i < bcStart + 30; i++) {
                                fprintf(valueSendLog, "%02x ", cbHdr->bytes()[i]);
                            }
                            fprintf(valueSendLog, "\n");
                        }
                    }
                }
            }
            // Dump call stack to see full context
            fprintf(valueSendLog, "  Call stack:\n");
            for (size_t d = 0; d < frameDepth_ && d < 10; d++) {
                SavedFrame& sf = savedFrames_[frameDepth_ - 1 - d];
                std::string frameSel = "<unknown>";
                if (sf.savedMethod.isObject() && sf.savedMethod.rawBits() > 0x10000) {
                    ObjectHeader* mHdr = sf.savedMethod.asObjectPtr();
                    Oop hdr = memory_.fetchPointer(0, sf.savedMethod);
                    if (hdr.isSmallInteger()) {
                        size_t numLits = hdr.asSmallInteger() & 0x7FFF;
                        if (numLits >= 2) {
                            Oop sel = memory_.fetchPointer(numLits - 1, sf.savedMethod);
                            if (sel.isObject() && sel.rawBits() > 0x10000) {
                                ObjectHeader* selHdr = sel.asObjectPtr();
                                if (selHdr->isBytesObject() && selHdr->byteSize() < 50) {
                                    frameSel = std::string((char*)selHdr->bytes(), selHdr->byteSize());
                                }
                            }
                        }
                    }
                }
                fprintf(valueSendLog, "    [%zu] %s\n", d, frameSel.c_str());
            }
            fflush(valueSendLog);
        }
    }

    // Trace #do: (which=11) to see what collection is being iterated
    if (which == 11) {  // #do:
        Oop block = stackValue(0);
        Oop collection = stackValue(1);
        static FILE* doSendLog = nullptr;
        static int doSendCount = 0;
        if (!doSendLog) doSendLog = nullptr;
        if (doSendLog && doSendCount < 50) {
            doSendCount++;
            std::string collClass = "<unknown>";
            int collSlots = -1;
            if (collection.isObject() && collection.rawBits() > 0x10000) {
                ObjectHeader* collHdr = collection.asObjectPtr();
                collSlots = collHdr->slotCount();
                Oop cls = memory_.classOf(collection);
                if (cls.isObject()) {
                    Oop clsName = memory_.fetchPointer(6, cls);
                    if (clsName.isObject() && clsName.rawBits() > 0x10000) {
                        ObjectHeader* cnHdr = clsName.asObjectPtr();
                        if (cnHdr->isBytesObject() && cnHdr->byteSize() < 50) {
                            collClass = std::string((char*)cnHdr->bytes(), cnHdr->byteSize());
                        }
                    }
                }
            }
            // Get caller method selector
            std::string callerSel = "<unknown>";
            if (method_.isObject() && method_.rawBits() > 0x10000) {
                Oop hdr = memory_.fetchPointer(0, method_);
                if (hdr.isSmallInteger()) {
                    size_t numLits = hdr.asSmallInteger() & 0x7FFF;
                    if (numLits >= 2) {
                        Oop sel = memory_.fetchPointer(numLits - 1, method_);
                        if (sel.isObject() && sel.rawBits() > 0x10000) {
                            ObjectHeader* selHdr = sel.asObjectPtr();
                            if (selHdr->isBytesObject() && selHdr->byteSize() < 50) {
                                callerSel = std::string((char*)selHdr->bytes(), selHdr->byteSize());
                            }
                        }
                    }
                }
            }
            fprintf(doSendLog, "[DO: #%d] collection=%s (0x%llx, %d slots) caller=#%s\n",
                    doSendCount, collClass.c_str(), (unsigned long long)collection.rawBits(), collSlots, callerSel.c_str());
            // For Arrays with nil elements, show more details
            bool hasNil = false;
            if (collClass == "Array" && collection.isObject()) {
                ObjectHeader* collHdr = collection.asObjectPtr();
                Oop nilObj = memory_.nil();
                for (size_t i = 0; i < collHdr->slotCount(); i++) {
                    if (collHdr->slotAt(i).rawBits() == nilObj.rawBits()) {
                        hasNil = true;
                        break;
                    }
                }
            }
            if (hasNil) {
                ObjectHeader* collHdr = collection.asObjectPtr();
                fprintf(doSendLog, "  *** Array with NIL elements! Elements: ");
                Oop nilObj = memory_.nil();
                int nilCount = 0;
                for (size_t i = 0; i < collHdr->slotCount() && i < 15; i++) {
                    Oop elem = collHdr->slotAt(i);
                    if (elem.rawBits() == nilObj.rawBits()) {
                        fprintf(doSendLog, "[nil] ");
                        nilCount++;
                    } else if (elem.isObject()) {
                        Oop elemCls = memory_.classOf(elem);
                        if (elemCls.isObject()) {
                            Oop elemClsName = memory_.fetchPointer(6, elemCls);
                            if (elemClsName.isObject() && elemClsName.rawBits() > 0x10000) {
                                ObjectHeader* ecnHdr = elemClsName.asObjectPtr();
                                if (ecnHdr->isBytesObject() && ecnHdr->byteSize() < 30) {
                                    std::string ecn((char*)ecnHdr->bytes(), ecnHdr->byteSize());
                                    fprintf(doSendLog, "[%s] ", ecn.c_str());
                                } else {
                                    fprintf(doSendLog, "[obj] ");
                                }
                            } else {
                                fprintf(doSendLog, "[obj] ");
                            }
                        } else {
                            fprintf(doSendLog, "[obj?] ");
                        }
                    } else {
                        fprintf(doSendLog, "[0x%llx] ", (unsigned long long)elem.rawBits());
                    }
                }
                fprintf(doSendLog, " (total %d, nilCount=%d)\n", collSlots, nilCount);
                // Show call stack
                fprintf(doSendLog, "  Call stack:\n");
                for (size_t d = 0; d < frameDepth_ && d < 8; d++) {
                    SavedFrame& sf = savedFrames_[frameDepth_ - 1 - d];
                    std::string frameSel = "<unknown>";
                    std::string frameRcvr = "<unknown>";
                    if (sf.savedMethod.isObject() && sf.savedMethod.rawBits() > 0x10000) {
                        Oop fhdr = memory_.fetchPointer(0, sf.savedMethod);
                        if (fhdr.isSmallInteger()) {
                            size_t fnLits = fhdr.asSmallInteger() & 0x7FFF;
                            if (fnLits >= 2) {
                                Oop fsel = memory_.fetchPointer(fnLits - 1, sf.savedMethod);
                                if (fsel.isObject()) {
                                    ObjectHeader* fselHdr = fsel.asObjectPtr();
                                    if (fselHdr->isBytesObject() && fselHdr->byteSize() < 50) {
                                        frameSel = std::string((char*)fselHdr->bytes(), fselHdr->byteSize());
                                    }
                                }
                            }
                        }
                    }
                    if (sf.savedReceiver.isObject()) {
                        Oop frc = memory_.classOf(sf.savedReceiver);
                        if (frc.isObject()) {
                            Oop frcName = memory_.fetchPointer(6, frc);
                            if (frcName.isObject() && frcName.rawBits() > 0x10000) {
                                ObjectHeader* frnH = frcName.asObjectPtr();
                                if (frnH->isBytesObject() && frnH->byteSize() < 50) {
                                    frameRcvr = std::string((char*)frnH->bytes(), frnH->byteSize());
                                }
                            }
                        }
                    }
                    fprintf(doSendLog, "    [%zu] %s >> #%s\n", d, frameRcvr.c_str(), frameSel.c_str());
                }
            }
            fflush(doSendLog);
        }
    }
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
    // Fast path: Get selector bytes without creating std::string
    const char* selBytes = nullptr;
    size_t selLen = 0;
    if (selector.isObject() && selector.rawBits() > 0x10000) {
        ObjectHeader* selHdr = selector.asObjectPtr();
        if (selHdr->isBytesObject() && selHdr->byteSize() < 50) {
            selBytes = (const char*)selHdr->bytes();
            selLen = selHdr->byteSize();
        }
    }

    Oop rcvr = stackValue(argCount);

    // Record watchdog info periodically (every 1024 sends) for timer summary
    {
        static int sendCount = 0;
        if ((++sendCount & 0x3FF) == 0) {
            if (selBytes && selLen > 0 && selLen < 63) {
                memcpy(g_watchdogSelector, selBytes, selLen);
                g_watchdogSelector[selLen] = '\0';
                memcpy(g_lastSelName, selBytes, selLen);
                g_lastSelName[selLen] = '\0';
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

    // Trace sends after mouse button events (set by FFI.cpp)
    {
        if (g_traceEventSends > 0) {
            g_traceEventSends--;
            // Get receiver class name
            std::string rcvClassName = "?";
            if (rcvr.isSmallInteger()) rcvClassName = "SmallInteger";
            else if (rcvr.isNil()) rcvClassName = "nil";
            else if (rcvr.isObject() && rcvr.rawBits() > 0x10000) {
                Oop cls = memory_.classOf(rcvr);
                if (cls.isObject() && cls.rawBits() > 0x10000 && memory_.slotCountOf(cls) > 6) {
                    Oop nameOop = memory_.fetchPointer(6, cls);
                    if (nameOop.isObject() && nameOop.rawBits() > 0x10000) {
                        ObjectHeader* nh = nameOop.asObjectPtr();
                        if (nh->isBytesObject() && nh->byteSize() < 100)
                            rcvClassName = std::string((char*)nh->bytes(), nh->byteSize());
                    }
                }
            }
            std::string sel(selBytes, selLen);
            // Get active process priority
            int activePri = -1;
            {
                Oop ap = getActiveProcess();
                if (ap.isObject() && ap.rawBits() > 0x10000) {
                    Oop priOop = memory_.fetchPointer(ProcessPriorityIndex, ap);
                    if (priOop.isSmallInteger()) activePri = (int)priOop.asSmallInteger();
                }
            }
            fprintf(stderr, "[SEND-TRACE] #%d p%d %s >> %s (args=%d) fd=%zu\n",
                    20000 - g_traceEventSends, activePri, rcvClassName.c_str(), sel.c_str(), argCount, frameDepth_);

            // When handleEvent: is sent, also log the argument class
            if (sel == "handleEvent:" && argCount == 1) {
                Oop arg = stackValue(0);
                std::string argClassName = "?";
                if (arg.isSmallInteger()) argClassName = "SmallInteger";
                else if (arg.isNil()) argClassName = "nil";
                else if (arg.isObject() && arg.rawBits() > 0x10000) {
                    Oop argCls = memory_.classOf(arg);
                    if (argCls.isObject() && argCls.rawBits() > 0x10000 && memory_.slotCountOf(argCls) > 6) {
                        Oop argName = memory_.fetchPointer(6, argCls);
                        if (argName.isObject() && argName.rawBits() > 0x10000) {
                            ObjectHeader* anh = argName.asObjectPtr();
                            if (anh->isBytesObject() && anh->byteSize() < 100)
                                argClassName = std::string((char*)anh->bytes(), anh->byteSize());
                        }
                    }
                }
                fprintf(stderr, "[HANDLE-EVENT] arg class: %s arg=0x%llx\n",
                        argClassName.c_str(), (unsigned long long)arg.rawBits());
            }

            // When dispatchMorphicEvent: is sent, log the argument class
            if (sel == "dispatchMorphicEvent:" && argCount == 1) {
                Oop arg = stackValue(0);
                std::string argClassName = "?";
                if (arg.isSmallInteger()) argClassName = "SmallInteger";
                else if (arg.isNil()) argClassName = "nil";
                else if (arg.isObject() && arg.rawBits() > 0x10000) {
                    Oop argCls = memory_.classOf(arg);
                    if (argCls.isObject() && argCls.rawBits() > 0x10000 && memory_.slotCountOf(argCls) > 6) {
                        Oop argName = memory_.fetchPointer(6, argCls);
                        if (argName.isObject() && argName.rawBits() > 0x10000) {
                            ObjectHeader* anh = argName.asObjectPtr();
                            if (anh->isBytesObject() && anh->byteSize() < 100)
                                argClassName = std::string((char*)anh->bytes(), anh->byteSize());
                        }
                    }
                }
                fprintf(stderr, "[DISPATCH-EVENT] arg class: %s arg=0x%llx\n",
                        argClassName.c_str(), (unsigned long long)arg.rawBits());
            }
        }
    }

    if (selBytes && selLen > 0) {
        // relinquishProcessorForMicroseconds: (35 chars)
        if (selLen == 35 && argCount == 1 &&
            memcmp(selBytes, "relinquishProcessorForMicroseconds:", 35) == 0) {
            static int idleCycles = 0;
            if (++idleCycles == 10) autoLoadDriver();
        }

        // snapshot:andQuit: (17 chars) - SAVE IS DISABLED
        if (selLen == 17 && argCount == 2 &&
            memcmp(selBytes, "snapshot:andQuit:", 17) == 0) {
            Oop saveArg = stackValue(1);
            Oop quitArg = stackValue(0);
            bool shouldQuit = quitArg.rawBits() == memory_.trueObject().rawBits();
            bool shouldSave = saveArg.rawBits() == memory_.trueObject().rawBits();
            if (shouldSave) {
                std::cerr << "[VM] Save disabled - ignoring save request\n";
            }
            if (shouldQuit) {
                stopVM("snapshot:andQuit: quit requested");
                popN(argCount + 1);
                push(memory_.nil());
                return;
            }
            popN(argCount + 1);
            push(memory_.nil());
            return;
        }
    }

    // Check for completely invalid receiver (raw 0, not the actual nil object)
    if (rcvr.rawBits() == 0) {
        Oop nilObj = memory_.specialObject(SpecialObjectIndex::NilObject);
        popN(argCount + 1);
        push(nilObj);
        return;
    }

    Oop nilObj = memory_.specialObject(SpecialObjectIndex::NilObject);
    Oop rcvrClass = memory_.classOf(rcvr);

    if (rcvrClass.rawBits() == 0) {
        popN(argCount + 1);
        push(nilObj);
        return;
    }

    // Handle deprecation transform check - when deprecation messages are sent
    // to a block instead of a transform rule, return appropriate defaults.
    if (rcvr.isObject() && selBytes && selLen > 0) {
        auto selIs = [selBytes, selLen](const char* s) {
            size_t n = strlen(s);
            return selLen == n && memcmp(selBytes, s, n) == 0;
        };
        if (selIs("shouldTransform") || selIs("deprecatedMethodName") ||
            selIs("explanationString") || selIs("transformingSelector") ||
            selIs("contextOfDeprecatedMethod") || selIs("sendingMethodName") ||
            selIs("default") || selIs("transform:")) {
            Oop rcvrClassLocal = memory_.classOf(rcvr);
            std::string className;
            if (rcvrClassLocal.isObject()) {
                Oop nameOop = memory_.fetchPointer(6, rcvrClassLocal);
                if (nameOop.isObject()) {
                    ObjectHeader* nameHdr = nameOop.asObjectPtr();
                    if (nameHdr->isBytesObject() && nameHdr->byteSize() < 50) {
                        className = std::string((char*)nameHdr->bytes(), nameHdr->byteSize());
                    }
                }
            }
            if (className.find("Block") != std::string::npos ||
                className.find("Closure") != std::string::npos) {
                if (selIs("shouldTransform")) {
                    Oop falseObj = memory_.specialObject(SpecialObjectIndex::FalseObject);
                    popN(argCount + 1);
                    push(falseObj);
                    return;
                }
                if (selIs("transform:") && argCount >= 1) {
                    Oop arg = stackValue(0);
                    popN(argCount + 1);
                    push(arg);
                    return;
                }
                popN(argCount + 1);
                push(nilObj);
                return;
            }
        }
    }

    // TRACE: track isNull on non-null ExternalAddress to debug return value
    static int traceIsNull = 0;
    if (selBytes && selLen == 6 && memcmp(selBytes, "isNull", 6) == 0) {
        if (rcvr.isObject() && rcvr.rawBits() > 0x10000) {
            ObjectHeader* rh = rcvr.asObjectPtr();
            if (rh->isBytesObject() && rh->byteSize() == 8) {
                // Check if non-null (any non-zero byte)
                bool isNonNull = false;
                for (int bi = 0; bi < 8; bi++) {
                    if (rh->bytes()[bi] != 0) { isNonNull = true; break; }
                }
                if (isNonNull && traceIsNull < 5) {
                    traceIsNull++;
                    fprintf(stderr, "[TRACE-ISNULL] #%d isNull on NON-NULL EA oop=0x%llx fd=%zu bytes:",
                            traceIsNull, (unsigned long long)rcvr.rawBits(), frameDepth_);
                    for (int bi = 0; bi < 8; bi++) fprintf(stderr, " %02x", rh->bytes()[bi]);
                    fprintf(stderr, " SP=%p FP=%p stackBase=%p\n",
                            (void*)stackPointer_, (void*)framePointer_, (void*)stackBase_);
                    // Dump stack around TOS
                    int sd = static_cast<int>(stackPointer_ - stackBase_);
                    fprintf(stderr, "[TRACE-ISNULL]   stackDepth=%d stack:", sd);
                    for (int si = 0; si < std::min(sd, 5); si++) {
                        Oop sv = *(stackPointer_ - 1 - si);
                        fprintf(stderr, " [%d]=0x%llx", si, (unsigned long long)sv.rawBits());
                    }
                    fprintf(stderr, "\n");
                    fflush(stderr);
                }
            }
        }
    }

    // Check method cache
    MethodCacheEntry* cached = probeCache(selector, rcvrClass);

    if (cached && cached->method != Oop::nil()) {
        // Cache hit
        if (cached->primitiveIndex > 0) {
            g_watchdogPrimIndex = cached->primitiveIndex;
            argCount_ = argCount;
            primitiveFailed_ = false;
            newMethod_ = cached->method;
            PrimitiveResult result = executePrimitive(cached->primitiveIndex, argCount);
            if (result == PrimitiveResult::Success) {
                return;
            }
        }

        // Suppress context switch for primitive 198 (ensure:/ifCurtailed:)
        if (cached->primitiveIndex == 198) {
            suppressContextSwitch_ = true;
        }

        // Non-CompiledMethod in cache — invoke as object-as-method
        if (__builtin_expect(!cached->method.isObject() || cached->method.rawBits() < 0x10000 ||
                             !cached->method.asObjectPtr()->isCompiledMethod(), 0)) {
            invokeObjectAsMethod(cached->method, selector, argCount);
            return;
        }
        activateMethod(cached->method, argCount);
        return;
    }

    // Cache miss - look up method
    Oop method = lookupMethod(selector, rcvrClass);

    if (method.isNil()) {
        sendDoesNotUnderstand(selector, argCount);
        return;
    }

    // Non-CompiledMethod found — invoke as object-as-method (metalinks, etc.)
    if (__builtin_expect(!method.isObject() || method.rawBits() < 0x10000 ||
                         !method.asObjectPtr()->isCompiledMethod(), 0)) {
        invokeObjectAsMethod(method, selector, argCount);
        return;
    }

    // Cache the method
    cacheMethod(selector, rcvrClass, method);

    // Check for primitive
    int primIndex = primitiveIndexOf(method);

    if (primIndex > 0) {
        g_watchdogPrimIndex = primIndex;
        argCount_ = argCount;
        primitiveFailed_ = false;
        newMethod_ = method;
        PrimitiveResult result = executePrimitive(primIndex, argCount);
        if (result == PrimitiveResult::Success) {
            return;
        }
    }

    // Suppress context switch for primitive 198 (ensure:/ifCurtailed:)
    if (primIndex == 198) {
        suppressContextSwitch_ = true;
    }

    activateMethod(method, argCount);
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

    if (!methodDict.isObject()) return Oop::nil();

    ObjectHeader* mdHeader = methodDict.asObjectPtr();
    size_t mdSlotCount = mdHeader->slotCount();
    if (mdSlotCount < 3) return Oop::nil();

    // Extract selector bytes for string comparison
    std::string selectorStr;
    Oop actualSelector = selector;

    if (selector.isObject() && selector.rawBits() > 0x10000) {
        ObjectHeader* selHdr = selector.asObjectPtr();

        // Handle wrapped selectors (AdditionalMethodState etc.)
        if (selHdr->format() == ObjectFormat::FixedSize && selHdr->slotCount() >= 1) {
            Oop innerSel = memory_.fetchPointer(0, selector);
            if (innerSel.isObject() && innerSel.rawBits() > 0x10000) {
                ObjectHeader* innerHdr = innerSel.asObjectPtr();
                if (innerHdr->isBytesObject()) {
                    actualSelector = innerSel;
                    selHdr = innerHdr;
                }
            }
        }

        if (selHdr->isBytesObject() && selHdr->byteSize() <= 100) {
            selectorStr = std::string((char*)selHdr->bytes(), selHdr->byteSize());
        }
    }

    // Get values array (slot 1)
    Oop valuesArray = memory_.fetchPointer(1, methodDict);
    if (!valuesArray.isObject() || valuesArray.rawBits() < 0x10000) return Oop::nil();

    ObjectHeader* valuesHeader = valuesArray.asObjectPtr();
    size_t valuesSize = valuesHeader->slotCount();
    size_t keySlotCount = mdSlotCount - 2;

    Oop nilObj = memory_.specialObject(SpecialObjectIndex::NilObject);
    auto isNilOrEmpty = [nilObj](Oop o) -> bool {
        return o.isNil() || o.rawBits() == nilObj.rawBits() || o.rawBits() < 0x10000;
    };

    // Search all key slots (linear scan — correct but O(n))
    for (size_t i = 0; i < keySlotCount; ++i) {
        size_t mdSlotIndex = i + 2;
        Oop key = memory_.fetchPointer(mdSlotIndex, methodDict);

        if (isNilOrEmpty(key)) continue;

        // Exact identity match (Symbols are unique objects)
        if (key.rawBits() == actualSelector.rawBits() || key.rawBits() == selector.rawBits()) {
            if (i < valuesSize) {
                Oop method = memory_.fetchPointer(i, valuesArray);
                if (method.isObject() && method.rawBits() > 0x10000) {
                    // Validate method is actually a CompiledMethod
                    ObjectHeader* mHdr = method.asObjectPtr();
                    return method;  // May be non-CompiledMethod (metalink etc.) — caller handles
                }
            }
            continue;
        }

        // String comparison fallback (for selectors that may not be interned)
        if (!selectorStr.empty() && key.isObject() && key.rawBits() > 0x10000) {
            ObjectHeader* keyHdr = key.asObjectPtr();
            if (keyHdr->isBytesObject()) {
                size_t keyLen = keyHdr->byteSize();
                if (keyLen == selectorStr.size() &&
                    memcmp(keyHdr->bytes(), selectorStr.data(), keyLen) == 0) {
                    if (i < valuesSize) {
                        Oop method = memory_.fetchPointer(i, valuesArray);
                        if (method.isObject() && method.rawBits() > 0x10000) {
                            ObjectHeader* mHdr = method.asObjectPtr();
                            return method;  // May be non-CompiledMethod — caller handles
                        }
                    }
                }
            }
        }
    }

    return Oop::nil();
}

MethodCacheEntry* Interpreter::probeCache(Oop selector, Oop classOop) {
    static uint64_t cacheHits = 0;
    static uint64_t cacheMisses = 0;

    size_t hash = cacheHash(selector, classOop);
    MethodCacheEntry& entry = methodCache_[hash];

    if (entry.selector == selector && entry.classOop == classOop) {
        cacheHits++;
        if ((cacheHits & 0xFFFFFF) == 0) { // every 16M hits
            fprintf(stderr, "[CACHE-STATS] hits=%llu misses=%llu rate=%.2f%% step=%llu\n",
                    cacheHits, cacheMisses,
                    100.0 * cacheHits / (cacheHits + cacheMisses),
                    g_stepCount);
        }
        return &entry;
    }

    cacheMisses++;
    return nullptr;
}

void Interpreter::cacheMethod(Oop selector, Oop classOop, Oop method) {
    size_t hash = cacheHash(selector, classOop);
    MethodCacheEntry& entry = methodCache_[hash];

    entry.selector = selector;
    entry.classOop = classOop;
    entry.method = method;
    entry.primitiveIndex = primitiveIndexOf(method);
    entry.primitive = nullptr;  // Will be set on first call
}

size_t Interpreter::cacheHash(Oop selector, Oop classOop) const {
    // XOR the raw bits and mask to cache size
    uint64_t h = selector.rawBits() ^ classOop.rawBits();
    return static_cast<size_t>(h) & (MethodCacheSize - 1);
}

// ===== METHOD ACTIVATION =====

void Interpreter::activateMethod(Oop method, int argCount) {
    // Save current state

    if (!pushFrame(method, argCount)) {
        // Stack overflow — terminate the runaway process and switch to next.
        // This is equivalent to what happens in the Cog VM when the C stack overflows.
        terminateCurrentProcess();
        Oop next = wakeHighestPriority();
        if (next.isObject() && !next.isNil()) {
            transferTo(next);
        } else {
            stopVM("Stack overflow: no process to switch to");
        }
        return;
    }

    // Set up new method
    method_ = method;
    argCount_ = argCount;
    closure_ = memory_.nil();  // Method activations have no closure

    // Determine homeMethod_ based on whether this is a CompiledMethod or CompiledBlock
    // CompiledMethod (class index 3101): homeMethod_ = method
    // CompiledBlock (class index 3117): homeMethod_ = slot 0 (the home method)
    if (method.isObject()) {
        ObjectHeader* methodHdr = method.asObjectPtr();
        uint32_t classIdx = methodHdr->classIndex();

        if (classIdx == 3117) {
            // CompiledBlock - get home method from slot 2 (Pharo 11+ FullBlockClosure model)
            // Layout: slot 0 = header, slot 1 = selector, slot 2 = home method
            homeMethod_ = method;  // Default in case chain traversal fails

            // Try slot 2 first (home method in FullBlockClosure model)
            Oop slot2 = memory_.fetchPointer(2, method);
            if (slot2.isObject()) {
                ObjectHeader* slot2Hdr = slot2.asObjectPtr();
                if (slot2Hdr->classIndex() == 3101) {
                    homeMethod_ = slot2;
                }
            }

            // Fallback: try slot 0 chain (older formats)
            if (homeMethod_ == method) {
                Oop homeCandidate = memory_.fetchPointer(0, method);
                int maxHops = 10;
                while (homeCandidate.isObject() && maxHops-- > 0) {
                    ObjectHeader* candidateHdr = homeCandidate.asObjectPtr();
                    uint32_t candidateCls = candidateHdr->classIndex();
                    if (candidateCls == 3101) {
                        homeMethod_ = homeCandidate;
                        break;
                    } else if (candidateCls == 3117) {
                        homeCandidate = memory_.fetchPointer(0, homeCandidate);
                    } else {
                        break;
                    }
                }
            }
        } else {
            // CompiledMethod or other - homeMethod is the same as method
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
            static FILE* fcActivateLog = nullptr;
            static int fcActivateCount = 0;
            if constexpr (ENABLE_DEBUG_LOGGING) {
                if (!fcActivateLog) fcActivateLog = nullptr;
            }
            if (fcActivateLog && fcActivateCount < 10) {
                fcActivateCount++;
                fprintf(fcActivateLog, "[FC-ACTIVATE #%d] fullCheck activated\n", fcActivateCount);
                fprintf(fcActivateLog, "  receiver_ = 0x%llx\n", (unsigned long long)receiver_.rawBits());
                if (receiver_.isObject() && receiver_.rawBits() > 0x10000) {
                    ObjectHeader* rcvrHdr = receiver_.asObjectPtr();
                    fprintf(fcActivateLog, "  rawHdr = 0x%llx slots=%d classIdx=%d\n",
                            (unsigned long long)rcvrHdr->rawHeader(),
                            rcvrHdr->slotCount(), rcvrHdr->classIndex());
                    Oop cls = memory_.classOf(receiver_);
                    if (cls.isObject()) {
                        Oop clsName = memory_.fetchPointer(6, cls);
                        if (clsName.isObject() && clsName.rawBits() > 0x10000) {
                            ObjectHeader* cnHdr = clsName.asObjectPtr();
                            if (cnHdr->isBytesObject() && cnHdr->byteSize() < 50) {
                                std::string cn((char*)cnHdr->bytes(), cnHdr->byteSize());
                                fprintf(fcActivateLog, "  class = %s\n", cn.c_str());
                            }
                        }
                    }
                } else {
                    fprintf(fcActivateLog, "  receiver is nil or SmallInteger!\n");
                }
                fflush(fcActivateLog);
            }
        }
        // TRACE: After fullCheck section
    }

    // Trace method activation to debug SessionManager default (disabled for performance)
    [[maybe_unused]] static FILE* actLog = nullptr;
    [[maybe_unused]] static int actCount = 0;
    if constexpr (ENABLE_DEBUG_LOGGING) {
    // TRACE: Enter SessionManager section
    if (!actLog) actLog = nullptr;
    if (actLog && actCount < 200) {
        // TRACE: Inside actLog && actCount condition
        // Get method selector for logging
        std::string selStr = "<unknown>";
        // TRACE: Getting selStr
        if (method.isObject()) {
            ObjectHeader* mHdr = method.asObjectPtr();
            if (mHdr->slotCount() > 1) {
                Oop selector = memory_.fetchPointer(1, method);
                if (selector.isObject() && selector.rawBits() > 0x10000) {
                    ObjectHeader* selHdr = selector.asObjectPtr();
                    if (selHdr->isBytesObject() && selHdr->byteSize() < 50) {
                        selStr = std::string((char*)selHdr->bytes(), selHdr->byteSize());
                    }
                }
            }
        }
        // TRACE: Got selStr
        // Check if receiver is SessionManager
        std::string rcvrName = "<unknown>";
        // TRACE: Getting rcvrName
        if (receiver_.isObject() && receiver_.rawBits() > 0x10000) {
            ObjectHeader* rcvrHdr = receiver_.asObjectPtr();
            // TRACE: rcvrHdr info
            if (rcvrHdr->slotCount() >= 7) {
                // TRACE: Fetching slot 6
                Oop nameOop = memory_.fetchPointer(6, receiver_);
                // TRACE: Got slot 6
                // Validate nameOop is in valid heap range before dereferencing
                if (nameOop.isObject() && nameOop.rawBits() > 0x10000 &&
                    memory_.isValidObject(nameOop)) {
                    ObjectHeader* nameHdr = nameOop.asObjectPtr();
                    if (nameHdr->isBytesObject() && nameHdr->byteSize() < 50) {
                        rcvrName = std::string((char*)nameHdr->bytes(), nameHdr->byteSize());
                    }
                }
            }
            // Also get class name for non-class receivers
            if (rcvrName == "<unknown>") {
                // TRACE: Getting class name
                Oop cls = memory_.classOf(receiver_);
                if (cls.isObject()) {
                    Oop clsName = memory_.fetchPointer(6, cls);
                    if (clsName.isObject() && clsName.rawBits() > 0x10000) {
                        ObjectHeader* cnHdr = clsName.asObjectPtr();
                        if (cnHdr->isBytesObject() && cnHdr->byteSize() < 50) {
                            rcvrName = std::string((char*)cnHdr->bytes(), cnHdr->byteSize());
                        }
                    }
                }
            }
        }
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

    // NOTE: Previously had a createStartupContext() call here that created a FAKE
    // context for the caller's method with all-nil temps. This caused the ensure: DNU
    // on nil in doDrawCycleWith: because when a process switch materialized frames,
    // this fake context (with nil args/temps) ended up in the sender chain. When restored
    // via executeFromContext, temp[0] (aBlock) was nil.
    // The activeContext_ should remain as-is; it's maintained by the normal
    // materializeFrameStack/executeFromContext path.

    // Set instruction pointer to start of bytecodes
    ObjectHeader* methodObj = method_.asObjectPtr();

    Oop methodHeader = memory_.fetchPointer(0, method_);
    if (__builtin_expect(!methodHeader.isSmallInteger(), 0)) {
        // Method header must be a SmallInteger. If it's not, the method object
        // is corrupted (possibly by GC or by activating a non-method object).
        fprintf(stderr, "[FATAL] activateMethod: method header is not SmallInteger!\n"
                "  method_=0x%llx cls=%u fmt=%d slots=%zu\n"
                "  header=0x%llx tag=%d isObj=%d\n"
                "  newMethod_=0x%llx receiver_=0x%llx\n"
                "  step=%llu frameDepth=%zu\n",
                (unsigned long long)method_.rawBits(),
                methodObj->classIndex(), (int)methodObj->format(),
                methodObj->slotCount(),
                (unsigned long long)methodHeader.rawBits(),
                (int)(methodHeader.rawBits() & 7),
                methodHeader.isObject(),
                (unsigned long long)newMethod_.rawBits(),
                (unsigned long long)receiver_.rawBits(),
                (unsigned long long)g_stepNum, frameDepth_);
        // Print the first 4 slots of the method object for diagnosis
        fprintf(stderr, "  slots:");
        for (size_t i = 0; i < std::min(methodObj->slotCount(), (size_t)4); ++i) {
            Oop s = methodObj->slotAt(i);
            fprintf(stderr, " [%zu]=0x%llx", i, (unsigned long long)s.rawBits());
        }
        fprintf(stderr, "\n");
        fflush(stderr);
        abort();
    }
    int64_t headerBits = methodHeader.asSmallInteger();
    int numLiterals = headerBits & 0x7FFF;  // bits 0-14 are numLiterals

    // Detect bytecode set: sign bit (bit 63) = 0 for V3PlusClosures, 1 for SistaV1
    // In 64-bit Spur, negative header means alternate bytecode set (SistaV1)
    usesSistaV1_ = headerBits < 0;

    uint8_t* methodBytes = methodObj->bytes();
    size_t bytecodeStart = (1 + numLiterals) * 8;
    instructionPointer_ = methodBytes + bytecodeStart;

    // Skip past callPrimitive bytecode (0xF8 lowByte highByte) if present
    // In Sista V1, primitive methods start with callPrimitive which should be skipped
    // when the primitive fails and we fall through to execute bytecodes
    if (usesSistaV1_ && instructionPointer_[0] == 0xF8) {
        instructionPointer_ += 3;  // Skip 0xF8 + 2 bytes of primitive index
    }

    // Set bytecode end
    size_t totalBytes = methodObj->byteSize();
    bytecodeEnd_ = methodBytes + totalBytes;

    // DEBUG: Dump bytecodes for signal method
    static FILE* bcLog = nullptr;
    static int activationCount = 0;
    if constexpr (ENABLE_DEBUG_LOGGING) {
        if (!bcLog) bcLog = nullptr;
    }
    activationCount++;
    if (bcLog && methodObj->slotCount() > 1 && activationCount < 100) {
        std::string sel = "";
        Oop lit1 = methodObj->slotAt(1);
        if (lit1.isObject()) {
            ObjectHeader* litHdr = lit1.asObjectPtr();
            if (litHdr->isBytesObject() && litHdr->byteSize() < 50) {
                sel = std::string((char*)litHdr->bytes(), litHdr->byteSize());
            }
        }
        if (activationCount >= 10 && activationCount <= 15) {
            fprintf(bcLog, "[BC#%d] Activating '%s' sistaV1=%d rcvr=0x%llx clsIdx=%u, first 20 bytes: ",
                    activationCount, sel.c_str(), usesSistaV1_ ? 1 : 0,
                    (unsigned long long)receiver_.rawBits(),
                    receiver_.isObject() ? receiver_.asObjectPtr()->classIndex() : 0);
            for (int i = 0; i < 20 && instructionPointer_ + i < bytecodeEnd_; i++) {
                fprintf(bcLog, "%02X ", instructionPointer_[i]);
            }
            fprintf(bcLog, "\n");
            fflush(bcLog);
        }
    }

    // DEBUG_LOG("[ACTIVATE] clsIdx=" << (method.isObject() ? method.asObjectPtr()->classIndex() : 0)
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
    // DEBUG_LOG("[ACTIVATE] Method bytecodes: " << (totalBytes - bytecodeStart) << " bytes";

    // Show first few bytecodes for debugging
    // DEBUG_LOG("[ACTIVATE] First bytecodes: ";
    for (size_t i = 0; i < std::min((size_t)16, totalBytes - bytecodeStart); i++) {
        // std::cerr << std::hex << (int)methodBytes[bytecodeStart + i] << " ";
    }
    // std::cerr << std::dec; // DEBUG
}

void Interpreter::activateBlock(Oop block, int argCount) {
    // BlockClosure/FullBlockClosure layout:
    // 0: outerContext
    // 1: startPC (SmallInteger) for old BlockClosure, OR
    //    compiledBlock (Object) for FullBlockClosure
    // 2: numArgs (SmallInteger)
    // 3+: copied values

    Oop slot1 = memory_.fetchPointer(1, block);
    Oop outerContext = memory_.fetchPointer(0, block);

    Oop methodToExecute;
    uint8_t* startAddress = nullptr;
    Oop homeMethodForNLR = memory_.nil();  // The enclosing CompiledMethod for NLR home frame detection

    if (slot1.isSmallInteger()) {
        // Old-style BlockClosure: slot 1 is startPC
        int64_t startPC = slot1.asSmallInteger();
        // Get the method from outer context
        Oop outerMethod = memory_.fetchPointer(3, outerContext);
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
        Oop header = memory_.fetchPointer(0, compiledBlock);
        int64_t headerBits = header.asSmallInteger();
        int numLiterals = headerBits & 0xFFFF;
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
            Oop enclosingCode = memory_.fetchPointer(numLiterals, compiledBlock);
            // Follow the chain of enclosing blocks until we reach the home method
            int chainDepth = 0;
            while (enclosingCode.isObject() && enclosingCode.rawBits() > 0x10000 && chainDepth < 20) {
                ObjectHeader* ecHdr = enclosingCode.asObjectPtr();
                if (!ecHdr->isCompiledMethod()) break;

                // Get this code's header and last literal
                Oop ecHeader = memory_.fetchPointer(0, enclosingCode);
                if (!ecHeader.isSmallInteger()) break;
                int ecNumLits = ecHeader.asSmallInteger() & 0xFFFF;
                if (ecNumLits < 1) {
                    // No literals - this is the home method
                    homeMethodForNLR = enclosingCode;
                    break;
                }

                Oop ecLastLit = memory_.fetchPointer(ecNumLits, enclosingCode);

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
        // Stack overflow in block — terminate process and switch (same as activateMethod)
        terminateCurrentProcess();
        Oop next = wakeHighestPriority();
        if (next.isObject() && !next.isNil()) {
            transferTo(next);
        } else {
            stopVM("Stack overflow in block: no process to switch to");
        }
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
        static FILE* abLog = nullptr;
        static int abLogCount = 0;
        if (!abLog) abLog = nullptr;
        if (abLog && abLogCount++ < 200) {
            fprintf(abLog, "[AB #%d] fd=%zu homeMethodForNLR=0x%llx isObj=%d isNil=%d\n",
                    abLogCount, frameDepth_,
                    (unsigned long long)homeMethodForNLR.rawBits(),
                    homeMethodForNLR.isObject(), homeMethodForNLR.isNil());
            if (frameDepth_ > 1) {
                // Show method_ (current active method)
                fprintf(abLog, "  method_=0x%llx\n", (unsigned long long)method_.rawBits());
                // Show saved methods on stack
                for (size_t i = 0; i < frameDepth_ && i < 10; i++) {
                    fprintf(abLog, "  savedFrames_[%zu].savedMethod=0x%llx\n",
                            i, (unsigned long long)savedFrames_[i].savedMethod.rawBits());
                }
            }
            fflush(abLog);
        }
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

    // CRITICAL: Check if receiver is raw 0 - this indicates corruption
    if (receiver_.rawBits() == 0) {
        static FILE* blkRcvrZeroLog = nullptr;
        static int blkRcvrZeroCount = 0;
        if (!blkRcvrZeroLog) blkRcvrZeroLog = nullptr;
        if (blkRcvrZeroLog && blkRcvrZeroCount++ < 20) {
            fprintf(blkRcvrZeroLog, "[BLK-RCVR-ZERO #%d] In activateBlock! block=0x%llx\n",
                    blkRcvrZeroCount, (unsigned long long)block.rawBits());
            // Dump block slots
            ObjectHeader* blkHdr = block.asObjectPtr();
            fprintf(blkRcvrZeroLog, "  block slots: ");
            for (size_t i = 0; i < std::min(blkHdr->slotCount(), (size_t)10); i++) {
                fprintf(blkRcvrZeroLog, "[%zu]=0x%llx ", i, (unsigned long long)blkHdr->slotAt(i).rawBits());
            }
            fprintf(blkRcvrZeroLog, "\n");
            // Get method name from compiledBlock
            if (slot1.isObject()) {
                std::string methodName = "?";
                Oop hdr = memory_.fetchPointer(0, slot1);
                if (hdr.isSmallInteger()) {
                    int numLits = hdr.asSmallInteger() & 0x7FFF;
                    if (numLits >= 2) {
                        Oop sel = memory_.fetchPointer(numLits - 1, slot1);
                        if (sel.isObject() && sel.rawBits() > 0x10000) {
                            ObjectHeader* selHdr = sel.asObjectPtr();
                            if (selHdr->isBytesObject() && selHdr->byteSize() < 50) {
                                methodName = std::string((char*)selHdr->bytes(), selHdr->byteSize());
                            }
                        }
                    }
                }
                fprintf(blkRcvrZeroLog, "  compiledBlock method: #%s\n", methodName.c_str());
            }
            fflush(blkRcvrZeroLog);
        }
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

    // Log block activation details around the DNU step
    if (g_stepNum >= 23000 && g_stepNum <= 25000) {
        static FILE* blkActLog = nullptr;
        if (!blkActLog) blkActLog = nullptr;
        if (blkActLog) {
            fprintf(blkActLog, "[BLK-ACT step=%llu] block=0x%llx blockSlots=%zu numCopied=%d argCount=%d\n",
                    (unsigned long long)g_stepNum, (unsigned long long)block.rawBits(),
                    blockSlots, numCopied, argCount);
            fprintf(blkActLog, "  receiver_=0x%llx method_=0x%llx FP=%p SP=%p\n",
                    (unsigned long long)receiver_.rawBits(),
                    (unsigned long long)methodToExecute.rawBits(),
                    (void*)framePointer_, (void*)stackPointer_);
            // Dump block slots
            for (size_t s = 0; s < blockSlots && s < 8; s++) {
                Oop sv = memory_.fetchPointer(s, block);
                fprintf(blkActLog, "  block[%zu] = 0x%llx%s\n", s, (unsigned long long)sv.rawBits(),
                        sv.isNil() ? " (nil)" : "");
            }
            // Dump FP area
            for (int f = 0; f < 5; f++) {
                fprintf(blkActLog, "  FP[%d] = 0x%llx\n", f, (unsigned long long)(*(framePointer_ + f)).rawBits());
            }
            fflush(blkActLog);
        }
    }

    for (int i = 0; i < numCopied; i++) {
        Oop copiedValue = memory_.fetchPointer(firstCopiedSlot + i, block);
        setTemporary(argCount + i, copiedValue);

        // Log copied values when tracing is active
        if (g_traceEventSends > 0 && numCopied > 0) {
            std::string valClass = "?";
            if (copiedValue.isSmallInteger()) valClass = "SmallInteger(" + std::to_string(copiedValue.asSmallInteger()) + ")";
            else if (copiedValue.isNil()) valClass = "nil";
            else if (copiedValue.isObject() && copiedValue.rawBits() > 0x10000) {
                Oop cls = memory_.classOf(copiedValue);
                if (cls.isObject() && cls.rawBits() > 0x10000 && memory_.slotCountOf(cls) > 6) {
                    Oop nameOop = memory_.fetchPointer(6, cls);
                    if (nameOop.isObject() && nameOop.rawBits() > 0x10000) {
                        ObjectHeader* nh = nameOop.asObjectPtr();
                        if (nh->isBytesObject() && nh->byteSize() < 100)
                            valClass = std::string((char*)nh->bytes(), nh->byteSize());
                    }
                }
            }
            fprintf(stderr, "[BLK-COPIED] temp[%d] = 0x%llx class=%s (slot %d of block)\n",
                    argCount + i, (unsigned long long)copiedValue.rawBits(),
                    valClass.c_str(), firstCopiedSlot + i);
        }
    }

    instructionPointer_ = startAddress;

    // Set bytecode end based on method size
    ObjectHeader* methodHdr = methodToExecute.asObjectPtr();
    bytecodeEnd_ = methodHdr->bytes() + methodHdr->byteSize();
}

// ===== FRAME MANAGEMENT =====

bool Interpreter::pushFrame(Oop method, int argCount) {
    // Save current execution state before switching to new method
    if (frameDepth_ >= MaxFrameDepth) {
        std::cerr << "[VM-STOP] Frame depth overflow in pushFrame(): " << frameDepth_ << " >= " << MaxFrameDepth << "\n";
        stopVM("Frame depth overflow in pushFrame()");
        return false;
    }

    // Graceful stack overflow: refuse to go deeper than StackOverflowLimit.
    // The caller is responsible for cleaning up the operand stack (popping args,
    // pushing nil as a return value). This allows the Smalltalk code to unwind
    // naturally as each method returns nil to its caller.
    if (frameDepth_ >= StackOverflowLimit) {
        static int soCount = 0;
        if (++soCount <= 5) {
            std::cerr << "[STACK-OVERFLOW] frameDepth=" << frameDepth_
                      << " at limit " << StackOverflowLimit
                      << " step=" << g_stepNum << "\n";
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
    // address to the heap context's PC slot. This keeps the heap context's PC
    // current so that if Smalltalk code later modifies it (e.g. Context>>privRefresh
    // setting pc := startpc for restart), those modifications won't be overwritten
    // by a later materializeFrameStack call.
    if (frameDepth_ == 1 && activeContext_.isObject() && activeContext_.rawBits() > 0x10000 &&
        frame.savedMethod.isObject() && frame.savedMethod.rawBits() > 0x10000) {
        ObjectHeader* mObj = frame.savedMethod.asObjectPtr();
        uint8_t* mBytes = mObj->bytes();
        if (frame.savedIP >= mBytes && frame.savedIP < mBytes + mObj->byteSize()) {
            int64_t pc = static_cast<int64_t>(frame.savedIP - mBytes) + 1;
            memory_.storePointer(1, activeContext_, Oop::fromSmallInteger(pc));
        }
    }

    // Calculate number of temporaries for the new method
    Oop newMethodHeader = memory_.fetchPointer(0, method);
    if (__builtin_expect(!newMethodHeader.isSmallInteger(), 0)) {
        FILE* crashLog = fopen("/tmp/iospharo-crash.log", "w");
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
            std::string selStr = "?";
            if (sf.savedMethod.isObject() && sf.savedMethod.rawBits() > 0x10000) {
                ObjectHeader* sm = sf.savedMethod.asObjectPtr();
                if (sm->isCompiledMethod()) {
                    Oop mh = sm->slotAt(0);
                    if (mh.isSmallInteger()) {
                        size_t nl = mh.asSmallInteger() & 0x7FFF;
                        if (nl >= 2 && nl < sm->slotCount()) {
                            Oop sel = sm->slotAt(nl - 1);
                            if (sel.isObject() && sel.rawBits() > 0x10000) {
                                ObjectHeader* sh = sel.asObjectPtr();
                                if (sh->isBytesObject() && sh->byteSize() < 80) {
                                    selStr = std::string((char*)sh->bytes(), sh->byteSize());
                                }
                            }
                        }
                    }
                }
            }
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
    if (newFP < stackBase_) {
        static int fpBelowBaseCount = 0;
        if (++fpBelowBaseCount <= 10) {
            std::cerr << "[FP-BELOW-BASE #" << fpBelowBaseCount
                      << "] newFP=" << (void*)newFP << " stackBase_=" << (void*)stackBase_
                      << " SP=" << (void*)stackPointer_ << " argCount=" << argCount
                      << " fd=" << frameDepth_ << " step=" << g_stepNum
                      << "\n";
        }
    }
    framePointer_ = newFP;

    // Initialize temporaries to nil (numTemps includes args, which are already on stack)
    int numExtraTemps = numTemps - argCount;
    for (int i = 0; i < numExtraTemps; ++i) {
        push(memory_.nil());
    }

    return true;  // Successfully created frame
}

void Interpreter::popFrame() {
    // Restore previous execution state
    if (frameDepth_ == 0) {
        std::cerr << "[VM-STOP] popFrame at frameDepth 0\n";
        stopVM("popFrame at frameDepth 0");
        return;
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

    if (framePointer_ < stackBase_) {
        static int popFpBelowCount = 0;
        if (++popFpBelowCount <= 10) {
            std::cerr << "[POP-FP-BELOW #" << popFpBelowCount
                      << "] restored FP=" << (void*)framePointer_
                      << " stackBase_=" << (void*)stackBase_
                      << " fd=" << frameDepth_
                      << " step=" << g_stepNum << "\n";
        }
    }

    // If this was the last frame, we're done
    if (frameDepth_ == 0 && frame.savedIP == nullptr) {
        std::cerr << "[VM-STOP] Last frame popped in popFrame()\n";
        stopVM("Last frame popped in popFrame()");
    }
}

// ===== VARIABLE ACCESS =====

Oop Interpreter::literal(size_t index) const {
    // In Pharo 10+ with FullBlockClosure model, both CompiledMethods and CompiledBlocks
    // have their own literal frames. Each compiled object (method or block) contains
    // its own literals - blocks do NOT share literals with their home method.
    //
    // So we always use method_ (the currently executing CompiledMethod or CompiledBlock)
    // for literal access, NOT homeMethod_.
    Oop literalMethod = method_;

    // Safety check
    if (literalMethod.isNil() || !literalMethod.isObject()) {
        return memory_.specialObject(SpecialObjectIndex::NilObject);
    }

    // Get numLiterals from method header for bounds check
    // Pharo header format: bits 0-14 = numLiterals (15 bits)
    Oop methodHeader = memory_.fetchPointer(0, literalMethod);
    if (methodHeader.isSmallInteger()) {
        int64_t headerBits = methodHeader.asSmallInteger();
        size_t numLiterals = headerBits & 0x7FFF;  // bits 0-14

        if (index >= numLiterals) {
            // Out-of-bounds literal access - this is a bug!
            // Log details to help diagnose
            static int litOobCount = 0;
            static FILE* litOobLog = nullptr;
            if (!litOobLog) litOobLog = nullptr;
            litOobCount++;
            if (litOobLog && litOobCount <= 50) {
                fprintf(litOobLog, "[LIT-OOB #%d step=%llu] index=%zu >= numLiterals=%zu\n",
                        litOobCount, g_stepNum, index, numLiterals);
                // Log raw header to understand the decoding
                fprintf(litOobLog, "  headerRaw=0x%llx headerBits=%lld (0x%llx)\n",
                        (unsigned long long)methodHeader.rawBits(),
                        (long long)headerBits, (unsigned long long)headerBits);
                fprintf(litOobLog, "  method_=0x%llx\n", (unsigned long long)method_.rawBits());
                // Check if method_ is a CompiledBlock (has outerCode in penultimate literal)
                ObjectHeader* mh = literalMethod.asObjectPtr();
                uint32_t methodClassIdx = mh->classIndex();
                Oop methodClass = memory_.classAtIndex(methodClassIdx);
                std::string methodClassName = "?";
                if (methodClass.isObject()) {
                    Oop mcName = memory_.fetchPointer(6, methodClass);
                    if (mcName.isObject() && mcName.rawBits() > 0x10000) {
                        ObjectHeader* mcnh = mcName.asObjectPtr();
                        if (mcnh->isBytesObject() && mcnh->byteSize() < 50) {
                            methodClassName = std::string((char*)mcnh->bytes(), mcnh->byteSize());
                        }
                    }
                }
                fprintf(litOobLog, "  method class: %s (classIdx=%u)\n",
                        methodClassName.c_str(), methodClassIdx);
                // Show bytecodes around IP to understand what's happening
                fprintf(litOobLog, "  bytecodes around IP:\n");
                uint8_t* methodBytes = mh->bytes();
                size_t methodSize = mh->byteSize();
                size_t ipOffset = instructionPointer_ - methodBytes;
                size_t bytecodeStart = (1 + numLiterals) * 8;
                fprintf(litOobLog, "    IP offset from method start: %zu, bytecodeStart: %zu\n",
                        ipOffset, bytecodeStart);
                // Show previous 10 bytecodes
                fprintf(litOobLog, "    previous bytecodes: ");
                for (int i = 10; i > 0; i--) {
                    if (instructionPointer_ > methodBytes + i) {
                        fprintf(litOobLog, "%02X ", *(instructionPointer_ - i));
                    }
                }
                fprintf(litOobLog, "\n");
                // Dump ALL bytecodes of this method
                fprintf(litOobLog, "    ALL bytecodes (offset %zu to %zu): ", bytecodeStart, methodSize);
                for (size_t b = bytecodeStart; b < methodSize; b++) {
                    fprintf(litOobLog, "%02X ", methodBytes[b]);
                }
                fprintf(litOobLog, "\n");
                // Call stack
                fprintf(litOobLog, "  Call stack (frame depth=%zu):\n", frameDepth_);
                for (size_t i = 0; i < std::min(frameDepth_, (size_t)5); i++) {
                    if (frameDepth_ > i) {
                        const auto& sf = savedFrames_[frameDepth_ - 1 - i];
                        std::string frameSel = "<unknown>";
                        if (sf.savedMethod.isObject() && sf.savedMethod.rawBits() > 0x10000) {
                            Oop sfHdr = memory_.fetchPointer(0, sf.savedMethod);
                            if (sfHdr.isSmallInteger()) {
                                size_t sfLits = sfHdr.asSmallInteger() & 0x7FFF;
                                if (sfLits >= 2) {
                                    Oop sfSel = memory_.fetchPointer(sfLits - 1, sf.savedMethod);
                                    if (sfSel.isObject() && sfSel.rawBits() > 0x10000) {
                                        ObjectHeader* sfSelHdr = sfSel.asObjectPtr();
                                        if (sfSelHdr->isBytesObject() && sfSelHdr->byteSize() < 50) {
                                            frameSel = std::string((char*)sfSelHdr->bytes(), sfSelHdr->byteSize());
                                        }
                                    }
                                }
                            }
                        }
                        fprintf(litOobLog, "    [%zu] %s\n", i, frameSel.c_str());
                    }
                }
                fprintf(litOobLog, "  IP=%p bytecodeEnd_=%p\n", (void*)instructionPointer_, (void*)bytecodeEnd_);
                // Get method selector
                if (numLiterals >= 2) {
                    Oop sel = memory_.fetchPointer(numLiterals - 1, literalMethod);
                    if (sel.isObject() && sel.rawBits() > 0x10000) {
                        ObjectHeader* sh = sel.asObjectPtr();
                        if (sh->isBytesObject() && sh->byteSize() < 100) {
                            std::string selStr((char*)sh->bytes(), sh->byteSize());
                            fprintf(litOobLog, "  method selector: #%s\n", selStr.c_str());
                        }
                    }
                }
                // Check if IP is within method's bytes
                if (method_.isObject() && method_.rawBits() > 0x10000) {
                    ObjectHeader* mh = method_.asObjectPtr();
                    uint8_t* methodBytes = mh->bytes();
                    size_t methodSize = mh->byteSize();
                    bool ipInMethod = (instructionPointer_ >= methodBytes &&
                                      instructionPointer_ < methodBytes + methodSize);
                    fprintf(litOobLog, "  IP in method bounds: %s\n", ipInMethod ? "YES" : "NO");
                    if (!ipInMethod) {
                        fprintf(litOobLog, "  methodBytes=[%p-%p), IP=%p\n",
                                (void*)methodBytes, (void*)(methodBytes + methodSize),
                                (void*)instructionPointer_);
                    }
                }
                fflush(litOobLog);
            }
            return memory_.specialObject(SpecialObjectIndex::NilObject);
        }
    } else {
        // Method header isn't a SmallInteger - bad method
        return memory_.specialObject(SpecialObjectIndex::NilObject);
    }

    return memory_.fetchPointer(index + 1, literalMethod);
}

Oop Interpreter::temporary(int index) const {
    // In Sista bytecodes, temp indices 0..argCount-1 are the arguments,
    // and indices argCount+ are local temps/copied values.
    // Frame layout: [receiver, arg0, arg1, ..., temp0, temp1, ...]
    // So all are accessed at framePointer_[1 + index]
    return *(framePointer_ + 1 + index);
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
    // Same layout as temporary() - see comment above
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
    // Check if receiver is a byte object (String, Symbol, ByteArray, etc.)
    // Byte objects don't have pointer instance variables, so return nil
    if (receiver_.isObject()) {
        ObjectHeader* hdr = receiver_.asObjectPtr();
        if (hdr->isBytesObject() || hdr->isCompiledMethod()) {
            // std::cerr << "[WARN] Attempting to read instVar " << index
                      // << " from byte object - returning nil";
            return memory_.nil();
        }
    }
    Oop result = memory_.fetchPointer(index, receiver_);

    // Log slot 0 (superclass) accesses to debug infinite inheritsFrom loop
    static FILE* instVarLog = nullptr;
    static int instVarCount = 0;
    if constexpr (ENABLE_DEBUG_LOGGING) {
        if (!instVarLog) instVarLog = nullptr;
    }
    if (instVarLog && index == 0 && instVarCount < 100) {
        instVarCount++;
        fprintf(instVarLog, "[INSTVAR #%d] slot0 receiver=0x%llx result=0x%llx\n",
                instVarCount, (unsigned long long)receiver_.rawBits(),
                (unsigned long long)result.rawBits());
        fflush(instVarLog);
    }

    return result;
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

    // TRACE: Log OSSDL2Driver slot writes (especially inputSemaphore = slot 0)
    if (receiver_.isObject() && receiver_.rawBits() > 0x10000) {
        Oop rcls = memory_.classOf(receiver_);
        if (rcls.isObject()) {
            Oop cn = memory_.fetchPointer(6, rcls);
            if (cn.isObject() && cn.rawBits() > 0x10000) {
                ObjectHeader* cnh = cn.asObjectPtr();
                if (cnh->isBytesObject() && cnh->byteSize() == 12 &&
                    memcmp(cnh->bytes(), "OSSDL2Driver", 12) == 0) {
                    static FILE* sdlStoreLog = nullptr;
                    static int sdlStoreCount = 0;
                    if (!sdlStoreLog) sdlStoreLog = nullptr;
                    if (sdlStoreLog && sdlStoreCount++ < 50) {
                        bool isNil = (value.rawBits() == memory_.nil().rawBits());
                        fprintf(sdlStoreLog, "[SDL2-STORE #%d step=%llu] slot[%zu] := 0x%llx%s\n",
                                sdlStoreCount, (unsigned long long)g_stepNum, index,
                                (unsigned long long)value.rawBits(), isNil ? " (NIL!)" : "");
                        // Get method name
                        std::string msel = "?";
                        if (method_.isObject() && method_.rawBits() > 0x10000) {
                            Oop mh = memory_.fetchPointer(0, method_);
                            if (mh.isSmallInteger()) {
                                int nl = mh.asSmallInteger() & 0x7FFF;
                                if (nl >= 2) {
                                    Oop s = memory_.fetchPointer(nl - 1, method_);
                                    if (s.isObject() && s.rawBits() > 0x10000) {
                                        ObjectHeader* sh = s.asObjectPtr();
                                        if (sh->isBytesObject() && sh->byteSize() < 50)
                                            msel = std::string((char*)sh->bytes(), sh->byteSize());
                                    }
                                }
                            }
                        }
                        fprintf(sdlStoreLog, "  from method: #%s\n", msel.c_str());
                        fflush(sdlStoreLog);
                    }
                }
            }
        }
    }

    // TRACE: Log when Context sender (slot 0) is written during setupEventLoop
    if (index == 0 && receiver_.isObject() && receiver_.rawBits() > 0x10000) {
        Oop rcvrCls = memory_.classOf(receiver_);
        if (rcvrCls.isObject()) {
            Oop clsName = memory_.fetchPointer(6, rcvrCls);
            if (clsName.isObject() && clsName.rawBits() > 0x10000) {
                ObjectHeader* cnHdr = clsName.asObjectPtr();
                if (cnHdr->isBytesObject()) {
                    std::string name((char*)cnHdr->bytes(), cnHdr->byteSize());
                    if (name == "Context") {
                        static FILE* ctxSenderLog = nullptr;
                        static int ctxSenderCount = 0;
                        ctxSenderCount++;
                        // Only log during driver install timeframe (expanded range)
                        if (g_stepNum >= 400000 && g_stepNum <= 450000 && ctxSenderCount <= 100) {
                            if (!ctxSenderLog) ctxSenderLog = nullptr;
                            if (ctxSenderLog) {
                                Oop nilObj = memory_.nil();
                                bool senderIsNil = (value.rawBits() == nilObj.rawBits());
                                fprintf(ctxSenderLog, "[CTX-SENDER #%d step=%llu] Context 0x%llx sender := 0x%llx (nil=%d)\n",
                                        ctxSenderCount, g_stepNum,
                                        (unsigned long long)receiver_.rawBits(),
                                        (unsigned long long)value.rawBits(),
                                        senderIsNil ? 1 : 0);
                                // Show what method this context is for
                                Oop ctxMethod = memory_.fetchPointer(3, receiver_);
                                if (ctxMethod.isObject() && ctxMethod.rawBits() > 0x10000) {
                                    Oop mhdr = memory_.fetchPointer(0, ctxMethod);
                                    if (mhdr.isSmallInteger()) {
                                        int nLits = mhdr.asSmallInteger() & 0x7FFF;
                                        if (nLits >= 2 && nLits < 100) {
                                            Oop sel = memory_.fetchPointer(nLits - 1, ctxMethod);
                                            if (sel.isObject() && sel.rawBits() > 0x10000) {
                                                ObjectHeader* selH = sel.asObjectPtr();
                                                if (selH->isBytesObject() && selH->byteSize() < 100) {
                                                    fprintf(ctxSenderLog, "  context method: #%s\n",
                                                            std::string((char*)selH->bytes(), selH->byteSize()).c_str());
                                                }
                                            }
                                        }
                                    }
                                }
                                // Show what method is setting this sender
                                std::string methodSel = "<unknown>";
                                if (method_.isObject() && method_.rawBits() > 0x10000) {
                                    ObjectHeader* mHdr = method_.asObjectPtr();
                                    if (mHdr->isCompiledMethod()) {
                                        Oop mh = memory_.fetchPointer(0, method_);
                                        if (mh.isSmallInteger()) {
                                            int nLits = mh.asSmallInteger() & 0x7FFF;
                                            if (nLits >= 2 && nLits < 100) {
                                                Oop sel = memory_.fetchPointer(nLits - 1, method_);
                                                if (sel.isObject() && sel.rawBits() > 0x10000) {
                                                    ObjectHeader* selH = sel.asObjectPtr();
                                                    if (selH->isBytesObject() && selH->byteSize() < 100) {
                                                        methodSel = std::string((char*)selH->bytes(), selH->byteSize());
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                                fprintf(ctxSenderLog, "  in method: #%s\n", methodSel.c_str());
                                fflush(ctxSenderLog);
                            }
                        }
                    }
                }
            }
        }
    }

    // TRACE: Log when Context PC (slot 1) is written - key for understanding wrong PC bug
    if (index == 1 && receiver_.isObject() && receiver_.rawBits() > 0x10000) {
        Oop rcvrCls = memory_.classOf(receiver_);
        if (rcvrCls.isObject()) {
            Oop clsName = memory_.fetchPointer(6, rcvrCls);
            if (clsName.isObject() && clsName.rawBits() > 0x10000) {
                ObjectHeader* cnHdr = clsName.asObjectPtr();
                if (cnHdr->isBytesObject()) {
                    std::string name((char*)cnHdr->bytes(), cnHdr->byteSize());
                    if (name == "Context") {
                        static FILE* ctxPcLog = nullptr;
                        static int ctxPcCount = 0;
                        ctxPcCount++;
                        if (ctxPcCount <= 30) {
                            if (!ctxPcLog) ctxPcLog = nullptr;
                            if (ctxPcLog) {
                                fprintf(ctxPcLog, "[CTX-PC #%d] Context 0x%llx pc := %lld\n",
                                        ctxPcCount, (unsigned long long)receiver_.rawBits(),
                                        value.isSmallInteger() ? value.asSmallInteger() : -999);
                                // Also show what method this context is for
                                Oop ctxMethod = memory_.fetchPointer(3, receiver_);  // Might be nil if not yet set
                                if (ctxMethod.isObject() && ctxMethod.rawBits() > 0x10000) {
                                    Oop mhdr = memory_.fetchPointer(0, ctxMethod);
                                    if (mhdr.isSmallInteger()) {
                                        int nLits = mhdr.asSmallInteger() & 0x7FFF;
                                        if (nLits >= 2 && nLits < 100) {
                                            Oop sel = memory_.fetchPointer(nLits - 1, ctxMethod);
                                            if (sel.isObject() && sel.rawBits() > 0x10000) {
                                                ObjectHeader* selH = sel.asObjectPtr();
                                                if (selH->isBytesObject() && selH->byteSize() < 100) {
                                                    fprintf(ctxPcLog, "  context method: #%s\n",
                                                            std::string((char*)selH->bytes(), selH->byteSize()).c_str());
                                                }
                                            }
                                        }
                                    }
                                }
                                // Show call stack (what setter is running)
                                if (method_.isObject()) {
                                    Oop hdr = memory_.fetchPointer(0, method_);
                                    if (hdr.isSmallInteger()) {
                                        int nLits = hdr.asSmallInteger() & 0x7FFF;
                                        if (nLits >= 2) {
                                            Oop sel = memory_.fetchPointer(nLits - 1, method_);
                                            if (sel.isObject() && sel.rawBits() > 0x10000) {
                                                ObjectHeader* selH = sel.asObjectPtr();
                                                if (selH->isBytesObject() && selH->byteSize() < 100) {
                                                    fprintf(ctxPcLog, "  set by method: #%s\n",
                                                            std::string((char*)selH->bytes(), selH->byteSize()).c_str());
                                                }
                                            }
                                        }
                                    }
                                }
                                fflush(ctxPcLog);
                            }
                        }
                    }
                }
            }
        }
    }

    // TRACE: Log when Process suspendedContext (slot 1) is written
    // This is the KEY tracing point for understanding why forked processes have wrong context
    if (index == 1 && receiver_.isObject() && receiver_.rawBits() > 0x10000) {
        Oop rcvrCls = memory_.classOf(receiver_);
        if (rcvrCls.isObject()) {
            Oop clsName = memory_.fetchPointer(6, rcvrCls);
            if (clsName.isObject() && clsName.rawBits() > 0x10000) {
                ObjectHeader* cnHdr = clsName.asObjectPtr();
                if (cnHdr->isBytesObject() && cnHdr->byteSize() == 7) {
                    std::string name((char*)cnHdr->bytes(), 7);
                    if (name == "Process") {
                        static FILE* procCtxLog = nullptr;
                        static int procCtxCount = 0;
                        procCtxCount++;
                        if (procCtxCount <= 30) {
                            if (!procCtxLog) procCtxLog = nullptr;
                            if (procCtxLog) {
                                fprintf(procCtxLog, "[PROC-SLOT1 #%d] Process 0x%llx suspendedContext := 0x%llx\n",
                                        procCtxCount, (unsigned long long)receiver_.rawBits(),
                                        (unsigned long long)value.rawBits());
                                // Show context details
                                if (value.isObject() && value.rawBits() > 0x10000) {
                                    Oop ctxMethod = memory_.fetchPointer(3, value);  // Method slot
                                    std::string methName = "?";
                                    if (ctxMethod.isObject() && ctxMethod.rawBits() > 0x10000) {
                                        Oop mhdr = memory_.fetchPointer(0, ctxMethod);
                                        if (mhdr.isSmallInteger()) {
                                            int nLits = mhdr.asSmallInteger() & 0x7FFF;
                                            if (nLits >= 2 && nLits < 100) {
                                                Oop sel = memory_.fetchPointer(nLits - 1, ctxMethod);
                                                if (sel.isObject() && sel.rawBits() > 0x10000) {
                                                    ObjectHeader* selH = sel.asObjectPtr();
                                                    if (selH->isBytesObject() && selH->byteSize() < 100) {
                                                        methName = std::string((char*)selH->bytes(), selH->byteSize());
                                                    }
                                                }
                                            }
                                        }
                                    }
                                    fprintf(procCtxLog, "  context method: #%s (0x%llx)\n",
                                            methName.c_str(), (unsigned long long)ctxMethod.rawBits());
                                    Oop sender = memory_.fetchPointer(0, value);
                                    fprintf(procCtxLog, "  context sender: 0x%llx\n",
                                            (unsigned long long)sender.rawBits());
                                }
                                // Show call stack
                                fprintf(procCtxLog, "  Call stack (current method: ");
                                if (method_.isObject()) {
                                    Oop hdr = memory_.fetchPointer(0, method_);
                                    if (hdr.isSmallInteger()) {
                                        int nLits = hdr.asSmallInteger() & 0x7FFF;
                                        if (nLits >= 2) {
                                            Oop sel = memory_.fetchPointer(nLits - 1, method_);
                                            if (sel.isObject() && sel.rawBits() > 0x10000) {
                                                ObjectHeader* selH = sel.asObjectPtr();
                                                if (selH->isBytesObject() && selH->byteSize() < 100) {
                                                    fprintf(procCtxLog, "#%s",
                                                            std::string((char*)selH->bytes(), selH->byteSize()).c_str());
                                                }
                                            }
                                        }
                                    }
                                }
                                fprintf(procCtxLog, ")\n");
                                fflush(procCtxLog);
                            }
                        }
                    }
                }
            }
        }
    }

    // Check if receiver is a byte object - can't store to byte objects
    if (receiver_.isObject()) {
        ObjectHeader* hdr = receiver_.asObjectPtr();
        if (hdr->isBytesObject() || hdr->isCompiledMethod()) {
            // std::cerr << "[WARN] Attempting to write instVar " << index
                      // << " to byte object - ignored";
            return;
        }
    }

    // Trace stores to SessionManager
    static FILE* storeLog = nullptr;
    static int storeCount = 0;
    if constexpr (ENABLE_DEBUG_LOGGING) {
        if (!storeLog) storeLog = nullptr;
    }
    if (storeLog && storeCount < 100) {
        std::string rcvrName = "<unknown>";
        if (receiver_.isObject() && receiver_.rawBits() > 0x10000) {
            Oop cls = memory_.classOf(receiver_);
            if (cls.isObject()) {
                Oop clsName = memory_.fetchPointer(6, cls);
                if (clsName.isObject() && clsName.rawBits() > 0x10000) {
                    ObjectHeader* cnHdr = clsName.asObjectPtr();
                    if (cnHdr->isBytesObject() && cnHdr->byteSize() < 50) {
                        rcvrName = std::string((char*)cnHdr->bytes(), cnHdr->byteSize());
                    }
                }
            }
        }
        if (rcvrName.find("Session") != std::string::npos || storeCount < 30) {
            storeCount++;
            std::string valueName = "<unknown>";
            if (value.isObject() && value.rawBits() > 0x10000) {
                Oop vCls = memory_.classOf(value);
                if (vCls.isObject()) {
                    Oop vClsName = memory_.fetchPointer(6, vCls);
                    if (vClsName.isObject() && vClsName.rawBits() > 0x10000) {
                        ObjectHeader* vCnHdr = vClsName.asObjectPtr();
                        if (vCnHdr->isBytesObject() && vCnHdr->byteSize() < 50) {
                            valueName = std::string((char*)vCnHdr->bytes(), vCnHdr->byteSize());
                        }
                    }
                }
            } else if (value.isSmallInteger()) {
                valueName = "SmallInteger(" + std::to_string(value.asSmallInteger()) + ")";
            }
            fprintf(storeLog, "[STORE #%d] %s slot[%zu] := 0x%llx (%s) (receiver=0x%llx)\n",
                    storeCount, rcvrName.c_str(), index,
                    (unsigned long long)value.rawBits(), valueName.c_str(),
                    (unsigned long long)receiver_.rawBits());
            fflush(storeLog);
        }
    }

    memory_.storePointer(index, receiver_, value);
}

// ===== SPECIAL SENDS =====

void Interpreter::sendDoesNotUnderstand(Oop selector, int argCount) {
    static int dnuDepth = 0;
    const int MAX_DNU_DEPTH = 10;

    // General DNU tracing: log DNUs (skip startup batch, log new ones)
    static int generalDnuCount = 0;
    if (generalDnuCount < 20 || (generalDnuCount >= 20 && generalDnuCount < 500 && g_stepCount > 90000000)) {
        generalDnuCount++;
        std::string selName = "?";
        if (selector.isObject() && selector.rawBits() > 0x10000) {
            ObjectHeader* sh = selector.asObjectPtr();
            if (sh->isBytesObject() && sh->byteSize() < 100)
                selName = std::string((char*)sh->bytes(), sh->byteSize());
        }
        std::string rcvClass = "?";
        Oop rcv = stackValue(argCount);
        if (rcv.isSmallInteger()) rcvClass = "SmallInteger";
        else if (rcv.isNil()) rcvClass = "nil";
        else if (rcv.rawBits() == memory_.trueObject().rawBits()) rcvClass = "true";
        else if (rcv.rawBits() == memory_.falseObject().rawBits()) rcvClass = "false";
        else if (rcv.isObject() && rcv.rawBits() > 0x10000) {
            Oop cls = memory_.classOf(rcv);
            if (cls.isObject() && cls.rawBits() > 0x10000 && memory_.slotCountOf(cls) > 6) {
                Oop nameOop = memory_.fetchPointer(6, cls);
                if (nameOop.isObject() && nameOop.rawBits() > 0x10000) {
                    ObjectHeader* nh = nameOop.asObjectPtr();
                    if (nh->isBytesObject() && nh->byteSize() < 100)
                        rcvClass = std::string((char*)nh->bytes(), nh->byteSize());
                }
            }
        }
        // Also show the calling method
        std::string callerSel = "?";
        if (frameDepth_ > 0) {
            auto& sf = savedFrames_[frameDepth_ - 1];
            if (sf.savedMethod.isObject() && sf.savedMethod.rawBits() > 0x10000) {
                Oop hdr = memory_.fetchPointer(0, sf.savedMethod);
                if (hdr.isSmallInteger()) {
                    int nl = hdr.asSmallInteger() & 0x7FFF;
                    if (nl >= 2) {
                        Oop s = memory_.fetchPointer(nl - 1, sf.savedMethod);
                        if (s.isObject() && s.rawBits() > 0x10000) {
                            ObjectHeader* sh2 = s.asObjectPtr();
                            if (sh2->isBytesObject() && sh2->byteSize() < 100)
                                callerSel = std::string((char*)sh2->bytes(), sh2->byteSize());
                        }
                    }
                }
            }
        }
        fprintf(stderr, "[DNU #%d] %s >> #%s (args=%d) caller=%s fd=%zu\n",
                generalDnuCount, rcvClass.c_str(), selName.c_str(), argCount,
                callerSel.c_str(), frameDepth_);
        fflush(stderr);
    }

    dnuDepth++;

    // If the selector IS doesNotUnderstand:, we're in a recursive DNU cascade.
    // The standard VM terminates the process in this case — there's no way to recover
    // because the receiver's class doesn't implement doesNotUnderstand: itself.
    {
        bool isDnuSelector = false;
        if (selector.isObject() && selector.rawBits() > 0x10000) {
            ObjectHeader* sh = selector.asObjectPtr();
            if (sh->isBytesObject() && sh->byteSize() == 21 &&
                memcmp(sh->bytes(), "doesNotUnderstand:", 18) == 0) {
                isDnuSelector = true;
            }
        }
        if (isDnuSelector || selectors_.doesNotUnderstand.rawBits() == selector.rawBits()) {
            std::cerr << "[DNU-CASCADE] doesNotUnderstand: itself not found on receiver — terminating active process\n";
            dnuDepth--;
            // Standard VM behavior: terminate the active process, not the whole VM.
            // The process has an unrecoverable error (its receiver's class hierarchy
            // doesn't include doesNotUnderstand:). Other processes should continue.
            Oop nextProcess = wakeHighestPriority();
            if (nextProcess.isNil() || !nextProcess.isObject()) {
                stopVM("Recursive doesNotUnderstand: and no other runnable process");
                return;
            }
            g_xferReason = "dnuCascade";
            transferTo(nextProcess);
            return;
        }
    }

    // Depth limit — stop VM if stuck in DNU recursion
    if (dnuDepth > MAX_DNU_DEPTH) {
        std::cerr << "[DNU-DEPTH] DNU depth " << dnuDepth << " exceeded limit\n";
        dnuDepth--;
        stopVM("DNU recursion depth exceeded — infinite doesNotUnderstand: loop");
        return;
    }

    // Diagnostic: trace DNU for #not on ExternalAddress (investigating display issue)
    static int dnu_not_count = 0;
    if (dnu_not_count < 3 && selector.isObject() && selector.rawBits() > 0x10000) {
        ObjectHeader* selHdr = selector.asObjectPtr();
        if (selHdr->isBytesObject() && selHdr->byteSize() == 3 &&
            memcmp(selHdr->bytes(), "not", 3) == 0) {
            dnu_not_count++;
            Oop rcv = stackValue(argCount);  // receiver is under args
            std::string rcvClassName = "unknown";
            if (rcv.isSmallInteger()) rcvClassName = "SmallInteger(" + std::to_string(rcv.asSmallInteger()) + ")";
            else if (rcv.isCharacter()) rcvClassName = "Character";
            else if (rcv.isNil()) rcvClassName = "UndefinedObject";
            else if (rcv.isObject()) {
                Oop cls = memory_.classOf(rcv);
                if (cls.isObject() && memory_.slotCountOf(cls) > 6) {
                    Oop nameOop = memory_.fetchPointer(6, cls);
                    if (nameOop.isObject()) {
                        ObjectHeader* nh = nameOop.asObjectPtr();
                        if (nh->isBytesObject())
                            rcvClassName = std::string(reinterpret_cast<char*>(nh->bytes()), nh->byteSize());
                    }
                }
                ObjectHeader* rcvHdr = rcv.asObjectPtr();
                if (rcvHdr->isBytesObject()) {
                    size_t bs = rcvHdr->byteSize();
                    rcvClassName += " (bytes=" + std::to_string(bs) + " [";
                    for (size_t bi = 0; bi < std::min(bs, (size_t)8); bi++) {
                        char hex[4]; snprintf(hex, sizeof(hex), "%02x", rcvHdr->bytes()[bi]);
                        if (bi > 0) rcvClassName += " ";
                        rcvClassName += hex;
                    }
                    rcvClassName += "])";
                }
            }
            fprintf(stderr, "[DNU-NOT] #not sent to %s (oop=0x%llx)\n",
                    rcvClassName.c_str(), (unsigned long long)rcv.rawBits());

            // Helper lambda: extract selector string from a CompiledMethod
            // Selector is the penultimate literal (nLits-1 slot)
            auto extractSelector = [this](Oop method) -> std::string {
                if (!method.isObject() || method.rawBits() < 0x10000) return "?";
                ObjectHeader* mh = method.asObjectPtr();
                if (!mh->isCompiledMethod()) return "?cm";
                Oop methHdr = memory_.fetchPointer(0, method);
                if (!methHdr.isSmallInteger()) return "?hdr";
                int nLits = methHdr.asSmallInteger() & 0x7FFF;
                if (nLits < 2) return "?lit";
                // Penultimate literal is selector (or AdditionalMethodState)
                Oop selOop = memory_.fetchPointer(nLits - 1, method);
                if (selOop.isObject() && selOop.rawBits() > 0x10000) {
                    ObjectHeader* sh = selOop.asObjectPtr();
                    if (sh->isBytesObject() && sh->byteSize() <= 80)
                        return std::string((char*)sh->bytes(), sh->byteSize());
                    // Might be AdditionalMethodState - check slot 1 for selector
                    if (sh->slotCount() >= 2) {
                        Oop innerSel = memory_.fetchPointer(1, selOop);
                        if (innerSel.isObject() && innerSel.rawBits() > 0x10000) {
                            ObjectHeader* ish = innerSel.asObjectPtr();
                            if (ish->isBytesObject() && ish->byteSize() <= 80)
                                return std::string((char*)ish->bytes(), ish->byteSize());
                        }
                    }
                }
                return "?sel";
            };
            auto extractClass = [this](Oop method) -> std::string {
                if (!method.isObject() || method.rawBits() < 0x10000) return "?";
                Oop methHdr = memory_.fetchPointer(0, method);
                if (!methHdr.isSmallInteger()) return "?hdr";
                int nLits = methHdr.asSmallInteger() & 0x7FFF;
                if (nLits < 1) return "?lit";
                Oop lastLit = memory_.fetchPointer(nLits, method);
                if (!lastLit.isObject()) return "?ll";
                // Last literal is usually a MethodClassAssociation: key=selector, value=class
                if (memory_.slotCountOf(lastLit) >= 2) {
                    Oop cls = memory_.fetchPointer(1, lastLit);
                    if (cls.isObject() && memory_.slotCountOf(cls) > 6) {
                        Oop cn = memory_.fetchPointer(6, cls);
                        if (cn.isObject()) {
                            ObjectHeader* cnh = cn.asObjectPtr();
                            if (cnh->isBytesObject())
                                return std::string((char*)cnh->bytes(), cnh->byteSize());
                        }
                    }
                }
                return "?cls";
            };

            // Current method
            fprintf(stderr, "[DNU-NOT]   current: %s >> %s (frameDepth=%d)\n",
                    extractClass(method_).c_str(), extractSelector(method_).c_str(),
                    (int)frameDepth_);
            // Saved frames (most recent first)
            int printed = 0;
            for (int fi = (int)frameDepth_ - 1; fi >= 0 && printed < 8; fi--, printed++) {
                auto& sf = savedFrames_[fi];
                fprintf(stderr, "[DNU-NOT]   frame[%d]: %s >> %s\n",
                        fi, extractClass(sf.savedMethod).c_str(),
                        extractSelector(sf.savedMethod).c_str());
            }

            // Dump bytecodes of current method
            if (method_.isObject() && method_.rawBits() > 0x10000) {
                Oop mhdr = memory_.fetchPointer(0, method_);
                if (mhdr.isSmallInteger()) {
                    int nLits = mhdr.asSmallInteger() & 0x7FFF;
                    ObjectHeader* mobj = method_.asObjectPtr();
                    size_t totalBytes = mobj->byteSize();
                    size_t bcStart = (1 + nLits) * 8;
                    if (bcStart < totalBytes) {
                        size_t bcLen = totalBytes - bcStart;
                        uint8_t* bc = mobj->bytes() + bcStart;
                        // IP position
                        ptrdiff_t ipOffset = instructionPointer_ - bc;
                        fprintf(stderr, "[DNU-NOT]   nLits=%d bcStart=%zu bcLen=%zu ip=%td\n",
                                nLits, bcStart, bcLen, ipOffset);
                        // Dump all literals
                        for (int li = 1; li <= nLits && li <= 20; li++) {
                            Oop lit = memory_.fetchPointer(li, method_);
                            std::string litStr = "?";
                            if (lit.isSmallInteger()) {
                                litStr = "SmallInt(" + std::to_string(lit.asSmallInteger()) + ")";
                            } else if (lit.isObject() && lit.rawBits() > 0x10000) {
                                ObjectHeader* lh = lit.asObjectPtr();
                                if (lh->isBytesObject() && lh->byteSize() <= 50)
                                    litStr = "'" + std::string((char*)lh->bytes(), lh->byteSize()) + "'";
                                else
                                    litStr = "obj(cls=" + std::to_string((unsigned)lh->classIndex()) +
                                             " fmt=" + std::to_string((unsigned)lh->format()) +
                                             " slots=" + std::to_string((unsigned long)lh->slotCount()) + ")";
                            } else if (lit.rawBits() == memory_.trueObject().rawBits()) {
                                litStr = "true";
                            } else if (lit.rawBits() == memory_.falseObject().rawBits()) {
                                litStr = "false";
                            } else if (lit.isNil() || lit.rawBits() == memory_.nil().rawBits()) {
                                litStr = "nil";
                            } else {
                                litStr = "oop(0x" + std::to_string(lit.rawBits()) + ")";
                            }
                            fprintf(stderr, "[DNU-NOT]   lit[%d]: %s\n", li, litStr.c_str());
                        }
                        // Dump bytecodes (up to 40 bytes)
                        fprintf(stderr, "[DNU-NOT]   bytecodes:");
                        for (size_t i = 0; i < bcLen && i < 40; i++) {
                            if (i == (size_t)ipOffset) fprintf(stderr, " >>>");
                            fprintf(stderr, " %02x", bc[i]);
                        }
                        fprintf(stderr, "\n");
                    }
                }
            }

            // Dump stack values near TOS
            fprintf(stderr, "[DNU-NOT]   stackDepth=%d\n", (int)(stackPointer_ - stack_.data()));
            for (int si = 0; si < 5 && si <= (int)(stackPointer_ - stack_.data()); si++) {
                Oop sv = stackValue(si);
                std::string svStr = "?";
                if (sv.isSmallInteger()) svStr = "SmallInt(" + std::to_string(sv.asSmallInteger()) + ")";
                else if (sv.rawBits() == memory_.trueObject().rawBits()) svStr = "true";
                else if (sv.rawBits() == memory_.falseObject().rawBits()) svStr = "false";
                else if (sv.isNil() || sv.rawBits() == memory_.nil().rawBits()) svStr = "nil";
                else if (sv.isObject() && sv.rawBits() > 0x10000) {
                    ObjectHeader* svh = sv.asObjectPtr();
                    Oop svCls = memory_.classOf(sv);
                    std::string cn = "?";
                    if (svCls.isObject() && memory_.slotCountOf(svCls) > 6) {
                        Oop nameOop = memory_.fetchPointer(6, svCls);
                        if (nameOop.isObject()) {
                            ObjectHeader* nh = nameOop.asObjectPtr();
                            if (nh->isBytesObject() && nh->byteSize() <= 50)
                                cn = std::string((char*)nh->bytes(), nh->byteSize());
                        }
                    }
                    svStr = cn + "(fmt=" + std::to_string((unsigned)svh->format()) +
                            " cls=" + std::to_string((unsigned)svh->classIndex()) + ")";
                }
                fprintf(stderr, "[DNU-NOT]   stack[%d]: %s (0x%llx)\n",
                        si, svStr.c_str(), (unsigned long long)stackValue(si).rawBits());
            }

            fflush(stderr);
        }
    }

    // Create Message object
    Oop messageClass = memory_.specialObject(SpecialObjectIndex::ClassMessage);
    uint32_t messageClassIdx = memory_.indexOfClass(messageClass);
    if (messageClassIdx == 0)
        messageClassIdx = memory_.registerClass(messageClass);
    // Pharo's Message has 3 instance vars: selector, args, lookupClass
    // Allocate 3 slots; lookupClass (slot 2) defaults to nil
    Oop message = memory_.allocateSlots(messageClassIdx, 3, ObjectFormat::FixedSize);

    if (message.rawBits() == memory_.nil().rawBits()) {
        std::cerr << "[DNU-FATAL] Failed to allocate Message object\n";
        for (int i = 0; i < argCount + 1; i++) pop();
        dnuDepth--;
        stopVM("DNU: Failed to allocate Message object");
        return;
    }

    memory_.storePointer(0, message, selector);

    // Create arguments array
    Oop arrayClass = memory_.specialObject(SpecialObjectIndex::ClassArray);
    uint32_t arrayClassIdx = memory_.indexOfClass(arrayClass);
    if (arrayClassIdx == 0)
        arrayClassIdx = memory_.registerClass(arrayClass);
    Oop args = memory_.allocateSlots(arrayClassIdx, argCount, ObjectFormat::Indexable);

    if (args.rawBits() == memory_.nil().rawBits() && argCount > 0) {
        std::cerr << "[DNU-FATAL] Failed to allocate Array for DNU args\n";
        for (int i = 0; i < argCount + 1; i++) pop();
        dnuDepth--;
        stopVM("DNU: Failed to allocate args Array");
        return;
    }

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

    dnuDepth--;
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

    // Allocate Array for arguments
    Oop arrayClass = memory_.specialObject(SpecialObjectIndex::ClassArray);
    uint32_t arrayClassIndex = memory_.indexOfClass(arrayClass);
    Oop argsArray = memory_.allocateSlots(arrayClassIndex, argCount, ObjectFormat::Indexable);

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
    // If this causes infinite recursion, the DNU depth limit will stopVM().
    static int mbCount = 0;
    mbCount++;
    if (mbCount <= 10) {
        std::string clsName = "?";
        if (value.isSmallInteger()) {
            clsName = "SmallInteger(" + std::to_string(value.asSmallInteger()) + ")";
        } else if (value.isObject()) {
            Oop cls = memory_.classOf(value);
            if (cls.isObject() && cls.asObjectPtr()->slotCount() > 6) {
                Oop name = memory_.fetchPointer(6, cls);
                if (name.isObject() && name.asObjectPtr()->isBytesObject() && name.asObjectPtr()->byteSize() < 100)
                    clsName = std::string((char*)name.asObjectPtr()->bytes(), name.asObjectPtr()->byteSize());
            }
        }
        std::string mSel = "?";
        if (method_.isObject()) {
            Oop mh = memory_.fetchPointer(0, method_);
            if (mh.isSmallInteger()) {
                int nL = mh.asSmallInteger() & 0x7FFF;
                if (nL >= 2 && nL < 100) {
                    Oop sel = memory_.fetchPointer(nL - 1, method_);
                    if (sel.isObject() && sel.asObjectPtr()->isBytesObject() && sel.asObjectPtr()->byteSize() < 100)
                        mSel = std::string((char*)sel.asObjectPtr()->bytes(), sel.asObjectPtr()->byteSize());
                }
            }
        }
        // Check if value is a forwarding pointer (format=0 with non-zero classIndex used as forwarding address)
        bool isForwarder = false;
        if (value.isObject()) {
            ObjectHeader* hdr = value.asObjectPtr();
            isForwarder = (hdr->format() == ObjectFormat::ZeroSized && hdr->classIndex() == 0 &&
                          hdr->slotCount() == 0);  // Heuristic for freed/forwarded object
        }
        // Also dump the stack context: what receiver, what IP offset
        std::string rcvrCls = "?";
        if (receiver_.isObject()) {
            Oop rc = memory_.classOf(receiver_);
            if (rc.isObject() && rc.asObjectPtr()->slotCount() > 6) {
                Oop rn = memory_.fetchPointer(6, rc);
                if (rn.isObject() && rn.asObjectPtr()->isBytesObject() && rn.asObjectPtr()->byteSize() < 100)
                    rcvrCls = std::string((char*)rn.asObjectPtr()->bytes(), rn.asObjectPtr()->byteSize());
            }
        }
        int ipOff = -1;
        if (method_.isObject() && instructionPointer_) {
            ipOff = static_cast<int>(instructionPointer_ - method_.asObjectPtr()->bytes());
        }
        fprintf(stderr, "[MUSTBEBOOL #%d] value=0x%llx class=%s method=%s rcvr=%s ipOff=%d fd=%zu fwd=%d gcCount=%d\n",
                mbCount, (unsigned long long)value.rawBits(), clsName.c_str(), mSel.c_str(),
                rcvrCls.c_str(), ipOff, frameDepth_, isForwarder,
                memory_.statistics().gcCount);
    }
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
    Oop result = memory_.fetchPointer(0, classOop);


    return result;
}

Oop Interpreter::methodDictOf(Oop classOop) const {
    // Class layout: methodDict is slot 1
    return memory_.fetchPointer(1, classOop);
}

Oop Interpreter::methodClassOf(Oop method) const {
    // Get the class that defines this CompiledMethod by reading the last literal.
    // In Pharo, the last literal of a CompiledMethod is an Association whose value
    // is the class where the method is defined. This is critical for super sends.
    if (!method.isObject()) return memory_.nil();

    // Get numLiterals from method header
    Oop methodHeader = memory_.fetchPointer(0, method);
    if (!methodHeader.isSmallInteger()) return memory_.nil();

    int64_t headerBits = methodHeader.asSmallInteger();
    size_t numLiterals = headerBits & 0x7FFF;  // bits 0-14

    if (numLiterals < 2) return memory_.nil();

    // In Pharo, the literal layout is:
    //   slot 0: method header
    //   slot 1: literal[0]  ...  slot N: literal[N-1]
    // The LAST literal (slot numLiterals) is the class binding (Association/GlobalVariable).
    // The PENULTIMATE literal (slot numLiterals-1) is the selector (Symbol).
    // The class binding's value (slot 1) is the defining class.
    // This matches VMMaker's lastLiteralOf: which does fetchPointer(literalCount, method).
    Oop lastLiteral = memory_.fetchPointer(numLiterals, method);

    // The last literal can be:
    // 1. An Association (classBinding) - extract value (slot 1)
    // 2. Directly a class
    // 3. An AdditionalMethodState - need to unwrap

    if (!lastLiteral.isObject() || lastLiteral.isNil()) return memory_.nil();

    // Check if it's an Association by looking at its class
    Oop lastLitClass = memory_.classOf(lastLiteral);
    if (lastLitClass.isObject()) {
        Oop className = memory_.fetchPointer(6, lastLitClass);
        if (className.isObject()) {
            ObjectHeader* nameHdr = className.asObjectPtr();
            if (nameHdr->isBytesObject() && nameHdr->byteSize() < 50) {
                std::string classNameStr((char*)nameHdr->bytes(), nameHdr->byteSize());
                if (classNameStr == "Association" || classNameStr == "ClassBinding" ||
                    classNameStr == "GlobalVariable" || classNameStr == "LiteralVariable" ||
                    classNameStr == "ClassVariable" || classNameStr == "WorkspaceVariable" ||
                    classNameStr.find("Binding") != std::string::npos ||
                    classNameStr.find("Variable") != std::string::npos) {
                    // It's an Association-like binding - get value (slot 1)
                    return memory_.fetchPointer(1, lastLiteral);
                }
                if (classNameStr == "AdditionalMethodState") {
                    // AdditionalMethodState wraps the method - fallback to nil
                }
            }
        }
    }

    // Check if lastLiteral IS a class (has a methodDict at slot 1)
    ObjectHeader* litHdr = lastLiteral.asObjectPtr();
    if (litHdr->slotCount() >= 7) {
        Oop maybeName = memory_.fetchPointer(6, lastLiteral);
        if (maybeName.isObject()) {
            ObjectHeader* nameHdr = maybeName.asObjectPtr();
            if (nameHdr->isBytesObject() && nameHdr->byteSize() < 50) {
                // Looks like a class - return it
                return lastLiteral;
            }
        }
    }

    // Fallback: return nil (caller should use receiver's class as fallback)
    return memory_.nil();
}

int Interpreter::primitiveIndexOf(Oop method) const {
    if (!method.isObject()) return 0;

    Oop header = memory_.fetchPointer(0, method);
    if (!header.isSmallInteger()) return 0;

    int64_t bits = header.asSmallInteger();

    // CompiledMethod header format (after SmallInteger decoding):
    //   bits 0-14: numLiterals (15 bits)
    //   bit 15: needsLargeFrame
    //   bits 16-23: numTemps (8 bits)
    //   bits 24-27: numArgs (4 bits)
    //   bits 28-29: accessModifier
    //   bit 30: hasPrimitive flag
    //
    // The primitive number is encoded in the bytecode stream.
    // When hasPrimitive is set, bytecodes start with a callPrimitive bytecode.

    // Check hasPrimitive flag
    // In the raw SmallInteger oop, this is bit 19 (AlternateHeaderHasPrimFlag = 0x80000)
    // After asSmallInteger() decoding (>> 3), it becomes bit 16
    bool hasPrimitive = (bits >> 16) & 1;
    if (!hasPrimitive) return 0;

    ObjectHeader* methodObj = method.asObjectPtr();
    int numLiterals = bits & 0x7FFF;  // bits 0-14 are numLiterals
    uint8_t* bytecodes = methodObj->bytes() + (1 + numLiterals) * 8;

    // In Sista V1, primitive call is encoded as:
    // 248 lowByte highByte (callPrimitive)
    // The primitive number = lowByte | (highByte << 8)
    if (bytecodes[0] == 248) {
        int primIndex = bytecodes[1] | (bytecodes[2] << 8);
        return primIndex;
    }

    // hasPrimitive is set but first bytecode isn't callPrimitive
    {
        static int noCallPrimCount = 0;
        noCallPrimCount++;
        if (noCallPrimCount <= 20) {
            static FILE* ncLog = nullptr;
            if (ncLog) {
                ObjectHeader* mo = method.asObjectPtr();
                int nl = bits & 0x7FFF;
                uint8_t* bc = mo->bytes() + (1 + nl) * 8;
                fprintf(ncLog, "[NO-CALLPRIM #%d] bits=0x%llx numLits=%d bc[0]=%d bc[1]=%d bc[2]=%d\n",
                        noCallPrimCount, (long long)bits, nl, bc[0], bc[1], bc[2]);
                fflush(ncLog);
            }
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
        outerContextForBlock = materializeFrameStack();
        activeContext_ = outerContextForBlock;
        frameDepth_ = 0;  // Reset after materialization to prevent duplicate contexts
    }
    memory_.storePointer(0, block, outerContextForBlock);  // outerContext
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
    // Similar to createBlock but for full block closures
    createBlock();  // Simplified - treat same for now
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

    // The class index for instances is the index in the class table where this class is stored
    // NOT the classIndex of the class object itself (which is the metaclass index)
    // Cache the class index once found to avoid repeated searches
    static uint32_t cachedFullBlockClosureIdx = 0;
    uint32_t classIdx = cachedFullBlockClosureIdx;

    if (classIdx == 0 && blockClass.isObject()) {
        // First time: search for this class object in the class table
        for (uint32_t i = 1; i < 10000; i++) {
            Oop cls = memory_.classAtIndex(i);
            if (cls.rawBits() == blockClass.rawBits()) {
                classIdx = i;
                break;
            }
        }

        // If not found by pointer, try by name
        if (classIdx == 0) {
            for (uint32_t i = 1; i < 10000; i++) {
                Oop cls = memory_.classAtIndex(i);
                if (!cls.isNil() && cls.isObject()) {
                    Oop clsName = memory_.fetchPointer(6, cls);
                    if (clsName.isObject()) {
                        ObjectHeader* nameHdr = clsName.asObjectPtr();
                        if (nameHdr->isBytesObject() && nameHdr->byteSize() == 16) {
                            std::string name((char*)nameHdr->bytes(), 16);
                            if (name == "FullBlockClosure") {
                                classIdx = i;
                                break;
                            }
                        }
                    }
                }
            }
        }

        // Last resort fallback
        if (classIdx == 0) {
            classIdx = 38;  // BlockClosure in many images
        }

        // Cache for future calls
        cachedFullBlockClosureIdx = classIdx;
    }

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

    // Use activeContext_ as the outer context
    // Note: activateBlock now updates activeContext_ when entering blocks,
    // so blocks created inside other blocks will capture the correct context chain

    // Set outerContext field.
    // BlockClosure >> homeMethod uses outerContext to trace back to the home method
    // via the `home` chain. This is used by RecursionStopper and other code that
    // identifies the enclosing method. If outerContext is stale (points to an old
    // context from before the current inline frames were created), homeMethod will
    // return the wrong method.
    //
    // For blocks that need outerContext (ignoreOuterContext=false), we materialize
    // the frame stack to create proper Context objects. The materialized contexts
    // are only used for the closure's outerContext chain (for home/homeMethod).
    // We do NOT switch to context-based execution here because that would break
    // process suspension (activeContext_ becomes stale when inline frames are
    // pushed on top, causing suspended processes to lose their execution state).
    Oop outerContextForBlock;
    if (!ignoreOuterContext && frameDepth_ > 0) {
        outerContextForBlock = materializeFrameStack();
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
    // TRACE: Log copied values
    static FILE* copiedLog = nullptr;
    static int copiedCount = 0;
    if constexpr (ENABLE_DEBUG_LOGGING) {
        if (!copiedLog) copiedLog = nullptr;
    }

    // Log block creation params
    if (copiedLog && copiedCount < 200 && numCopied > 0) {
        fprintf(copiedLog, "[BLOCK-PARAMS] numArgs=%d numCopied=%d receiverOnStack=%d\n",
                numArgs, numCopied, receiverOnStack ? 1 : 0);
        fflush(copiedLog);
    }

    for (int i = numCopied - 1; i >= 0; --i) {
        Oop copiedValue = pop();
        memory_.storePointer(4 + i, block, copiedValue);

        bool isNilValue = (copiedValue.rawBits() == memory_.nil().rawBits());
        if (copiedLog && copiedCount < 200 && (numCopied > 0 || isNilValue)) {
            std::string valInfo = "";
            if (copiedValue.isSmallInteger()) {
                valInfo = "SmallInt " + std::to_string(copiedValue.asSmallInteger());
            } else if (copiedValue.isObject() && copiedValue.rawBits() > 0x10000) {
                Oop cls = memory_.classOf(copiedValue);
                if (cls.isObject()) {
                    Oop clsName = memory_.fetchPointer(6, cls);
                    if (clsName.isObject() && clsName.rawBits() > 0x10000) {
                        ObjectHeader* cnHdr = clsName.asObjectPtr();
                        if (cnHdr->isBytesObject() && cnHdr->byteSize() < 50) {
                            valInfo = std::string((char*)cnHdr->bytes(), cnHdr->byteSize());
                        }
                    }
                }
            } else if (isNilValue) {
                valInfo = "nil";
            } else if (copiedValue.rawBits() == memory_.trueObject().rawBits()) {
                valInfo = "true";
            } else if (copiedValue.rawBits() == memory_.falseObject().rawBits()) {
                valInfo = "false";
            }
            fprintf(copiedLog, "[COPIED #%d] block slot %d = 0x%llx %s (numCopied=%d)\n",
                    ++copiedCount, 4 + i, (unsigned long long)copiedValue.rawBits(),
                    valInfo.c_str(), numCopied);

            // If copying nil, show method context, temps, and recent bytecodes
            if (isNilValue) {
                // Show recent bytecodes (stored in a ring buffer) - 20 most recent
                fprintf(copiedLog, "  -> Recent bytecodes (before 0xF9, newest->oldest): ");
                for (int bc = 0; bc < 20 && bc < (int)recentBytecodes_.size(); bc++) {
                    uint8_t b = recentBytecodes_[(recentBytecodeIdx_ - 1 - bc + 256) % 256];
                    fprintf(copiedLog, "0x%02X ", b);
                    // Mark conditional jumps
                    if ((b >= 0xB8 && b <= 0xBF) || (b >= 0xC0 && b <= 0xC7)) {
                        fprintf(copiedLog, "<-COND ");
                    }
                }
                fprintf(copiedLog, "\n");

                std::string methodSel = "<unknown>";
                std::string rcvrClass = "<unknown>";
                if (method_.isObject()) {
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
                if (receiver_.isObject() && receiver_.rawBits() > 0x10000) {
                    Oop cls = memory_.classOf(receiver_);
                    if (cls.isObject()) {
                        Oop clsName = memory_.fetchPointer(6, cls);
                        if (clsName.isObject() && clsName.rawBits() > 0x10000) {
                            ObjectHeader* cnHdr = clsName.asObjectPtr();
                            if (cnHdr->isBytesObject() && cnHdr->byteSize() < 50) {
                                rcvrClass = std::string((char*)cnHdr->bytes(), cnHdr->byteSize());
                            }
                        }
                    }
                }
                fprintf(copiedLog, "  -> Created in %s >> #%s\n", rcvrClass.c_str(), methodSel.c_str());

                // Show current temps (0-3) to understand where nil came from
                fprintf(copiedLog, "  -> Current temps: ");
                for (int t = 0; t < 4; t++) {
                    Oop tv = temporary(t);
                    if (tv.rawBits() == memory_.nil().rawBits()) {
                        fprintf(copiedLog, "[%d]=nil ", t);
                    } else if (tv.isSmallInteger()) {
                        fprintf(copiedLog, "[%d]=%lld ", t, tv.asSmallInteger());
                    } else {
                        fprintf(copiedLog, "[%d]=0x%llx ", t, (unsigned long long)tv.rawBits());
                    }
                }
                fprintf(copiedLog, "\n");
            }
            fflush(copiedLog);
        }
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
        outerContextForBlock = materializeFrameStack();
        activeContext_ = outerContextForBlock;
        frameDepth_ = 0;  // Reset after materialization to prevent duplicate contexts
    }
    memory_.storePointer(0, block, outerContextForBlock);  // outerContext
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

void Interpreter::initializeSelectors() {
    // DEBUG: "[DEBUG] initializeSelectors: Starting..."

    // Get selectors from special objects array
    selectors_.doesNotUnderstand = memory_.specialObject(SpecialObjectIndex::SelectorDoesNotUnderstand);
    selectors_.mustBeBoolean = memory_.specialObject(SpecialObjectIndex::SelectorMustBeBoolean);
    selectors_.cannotReturn = memory_.specialObject(SpecialObjectIndex::SelectorCannotReturn);
    selectors_.aboutToReturn = memory_.specialObject(SpecialObjectIndex::SelectorAboutToReturn);

    // Debug: show what we got for doesNotUnderstand
    {
        Oop dnu = selectors_.doesNotUnderstand;
        std::cerr << "[INIT-SELECTORS] doesNotUnderstand=0x" << std::hex << dnu.rawBits() << std::dec;
        if (dnu.isObject() && dnu.rawBits() > 0x10000) {
            ObjectHeader* hdr = dnu.asObjectPtr();
            if (hdr->isBytesObject() && hdr->byteSize() < 50) {
                std::cerr << " (#" << std::string((char*)hdr->bytes(), hdr->byteSize()) << ")";
            }
        } else if (dnu.isNil() || dnu.rawBits() == 0) {
            std::cerr << " (NIL!)";
        }
        std::cerr << "\n";
    }
    // DEBUG: "[DEBUG] initializeSelectors: Got special selectors"

    // For arithmetic selectors, search SmallInteger's method dictionary
    Oop smallIntClass = memory_.specialObject(SpecialObjectIndex::ClassSmallInteger);
    if (!smallIntClass.isObject() || smallIntClass.isNil()) {
        // std::cerr << "[WARN] initializeSelectors: SmallInteger class not found"; // DEBUG
        return;
    }
    // DEBUG: "[DEBUG] initializeSelectors: Got SmallInteger class"

    // Get the actual nil object for comparison
    Oop nilObj = memory_.specialObject(SpecialObjectIndex::NilObject);

    // Helper to find a selector in a class hierarchy
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

                // MethodDictionary layout: slot 0 = tally, slot 1 = array
                size_t mdSlots = mdHeader->slotCount();
                if (mdSlots >= 2) {
                    Oop mdArray = memory_.fetchPointer(1, methodDict);
                    if (mdArray.isObject() && !isNilOrEmpty(mdArray)) {
                        ObjectHeader* arrayHeader = mdArray.asObjectPtr();
                        size_t arraySize = arrayHeader->slotCount();
                        size_t maxSearch = std::min(arraySize, (size_t)252);
                        for (size_t i = 0; i < maxSearch; i++) {
                            Oop entry = arrayHeader->slotAt(i);
                            if (isNilOrEmpty(entry) || !entry.isObject()) continue;

                            ObjectHeader* entryHdr = entry.asObjectPtr();
                            if (entryHdr->isCompiledMethod()) {
                                size_t entrySlots = entryHdr->slotCount();
                                if (entrySlots < 2) continue;
                                Oop selector = memory_.fetchPointer(1, entry);
                                if (selector.isObject() && !isNilOrEmpty(selector) && selector.rawBits() > 0x10000) {
                                    if (memory_.symbolEquals(selector, name)) {
                                        return selector;
                                    }
                                }
                            } else {
                                Oop key = memory_.fetchPointer(0, entry);
                                if (key.isObject() && !isNilOrEmpty(key) && key.rawBits() > 0x10000 && memory_.symbolEquals(key, name)) {
                                    return key;
                                }
                            }
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
    // DEBUG: "[DEBUG] initializeSelectors: Looking for +"
    selectors_.add = findSelectorInClass(smallIntClass, "+");
    // DEBUG: "[DEBUG] initializeSelectors: Looking for -"
    selectors_.subtract = findSelectorInClass(smallIntClass, "-");
    // DEBUG: "[DEBUG] initializeSelectors: Looking for <"
    selectors_.lessThan = findSelectorInClass(smallIntClass, "<");
    // DEBUG: "[DEBUG] initializeSelectors: Looking for >"
    selectors_.greaterThan = findSelectorInClass(smallIntClass, ">");
    // DEBUG: "[DEBUG] initializeSelectors: Looking for <="
    selectors_.lessEqual = findSelectorInClass(smallIntClass, "<=");
    // DEBUG: "[DEBUG] initializeSelectors: Looking for >="
    selectors_.greaterEqual = findSelectorInClass(smallIntClass, ">=");
    // DEBUG: "[DEBUG] initializeSelectors: Looking for ="
    selectors_.equal = findSelectorInClass(smallIntClass, "=");
    // DEBUG: "[DEBUG] initializeSelectors: Looking for ~="
    selectors_.notEqual = findSelectorInClass(smallIntClass, "~=");
    // DEBUG: "[DEBUG] initializeSelectors: Looking for *"
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
    // DEBUG: "[DEBUG] initializeSelectors: Done with SmallInteger selectors"

    // Skip these for now to avoid potential hangs
    selectors_.at = Oop::nil();
    selectors_.atPut = Oop::nil();
    selectors_.size = Oop::nil();
    selectors_.eq = Oop::nil();
    selectors_.class_ = Oop::nil();
    selectors_.value = Oop::nil();
    selectors_.value_ = Oop::nil();
    selectors_.valueValue = Oop::nil();

    // Log results
    // DEBUG: "[DEBUG] Arithmetic selectors found:"
    // std::cerr << "  +: 0x" << std::hex << selectors_.add.rawBits() << std::dec; // DEBUG
    // std::cerr << "  -: 0x" << std::hex << selectors_.subtract.rawBits() << std::dec; // DEBUG
    // std::cerr << "  <: 0x" << std::hex << selectors_.lessThan.rawBits() << std::dec; // DEBUG
    // std::cerr << "  >: 0x" << std::hex << selectors_.greaterThan.rawBits() << std::dec; // DEBUG
    // std::cerr << "  *: 0x" << std::hex << selectors_.multiply.rawBits() << std::dec; // DEBUG
    // std::cerr << "  /: 0x" << std::hex << selectors_.divide.rawBits() << std::dec; // DEBUG
    // std::cerr << "  =: 0x" << std::hex << selectors_.equal.rawBits() << std::dec; // DEBUG
    // std::cerr << "  at:: 0x" << std::hex << selectors_.at.rawBits() << std::dec; // DEBUG
    // std::cerr << "  ==: 0x" << std::hex << selectors_.eq.rawBits() << std::dec; // DEBUG
    // std::cerr << "  value: 0x" << std::hex << selectors_.value.rawBits() << std::dec; // DEBUG
}

// ===== PROCESS SCHEDULING =====

void Interpreter::terminateCurrentProcess() {
    static FILE* termLog = stderr;  // Always log terminations - critical diagnostic
    static int termCount = 0;
    termCount++;

    Oop nilObj = memory_.specialObject(SpecialObjectIndex::NilObject);
    Oop schedulerAssoc = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);

    if (!schedulerAssoc.isObject() || schedulerAssoc.rawBits() == nilObj.rawBits()) {
        // DEBUG: "[SCHED] No scheduler - can't terminate"
        return;
    }

    Oop scheduler = memory_.fetchPointer(1, schedulerAssoc);
    if (!scheduler.isObject()) {
        // DEBUG: "[SCHED] Invalid scheduler"
        return;
    }

    // ProcessScheduler: slot 1 = activeProcess
    Oop activeProcess = memory_.fetchPointer(1, scheduler);
    if (!activeProcess.isObject() || activeProcess.rawBits() == nilObj.rawBits()) {
        // DEBUG: "[SCHED] No active process to terminate"
        return;
    }

    // Check if already terminated (suspendedContext is nil) - prevent duplicate termination
    Oop suspendedCtx = memory_.fetchPointer(ProcessSuspendedContextIndex, activeProcess);
    if (suspendedCtx.rawBits() == nilObj.rawBits()) {
        if (termLog && termCount <= 50) {
            Oop prioOop = memory_.fetchPointer(ProcessPriorityIndex, activeProcess);
            int prio = prioOop.isSmallInteger() ? static_cast<int>(prioOop.asSmallInteger()) : -1;
            fprintf(termLog, "[TERMINATE #%d] process=0x%llx priority=%d ALREADY TERMINATED - skipping\n",
                    termCount, (unsigned long long)activeProcess.rawBits(), prio);
            fflush(termLog);
        }
        return;  // Already terminated
    }

    if (termLog && termCount <= 50) {
        // Get priority
        Oop prioOop = memory_.fetchPointer(ProcessPriorityIndex, activeProcess);
        int prio = prioOop.isSmallInteger() ? static_cast<int>(prioOop.asSmallInteger()) : -1;
        fprintf(termLog, "[TERMINATE #%d step=%lld] process=0x%llx priority=%d oldSuspendedContext=0x%llx fd=%zu\n",
                termCount, (long long)g_stepNum, (unsigned long long)activeProcess.rawBits(), prio,
                (unsigned long long)suspendedCtx.rawBits(), frameDepth_);
        // Log current method selector
        auto extractSel = [this](Oop method) -> std::string {
            if (!method.isObject() || method.rawBits() <= 0x10000) return "?";
            ObjectHeader* mh = method.asObjectPtr();
            if (!mh->isCompiledMethod()) return "?";
            Oop h = mh->slotAt(0);
            if (!h.isSmallInteger()) return "?";
            int nl = h.asSmallInteger() & 0x7FFF;
            if (nl < 2 || nl >= (int)mh->slotCount()) return "?";
            Oop sel = mh->slotAt(nl - 1);
            if (!sel.isObject() || sel.rawBits() <= 0x10000) return "?";
            ObjectHeader* sh = sel.asObjectPtr();
            if (!sh->isBytesObject() || sh->byteSize() > 80) return "?";
            return std::string((char*)sh->bytes(), sh->byteSize());
        };
        fprintf(termLog, "[TERMINATE #%d] current: %s\n", termCount, extractSel(method_).c_str());
        for (size_t fi = 0; fi < std::min(frameDepth_, (size_t)10); fi++) {
            size_t idx = frameDepth_ - 1 - fi;
            fprintf(termLog, "[TERMINATE #%d]   frame[%zu]: %s\n", termCount,
                    idx, extractSel(savedFrames_[idx].savedMethod).c_str());
        }
        fflush(termLog);
    }

    // Get the list this process belongs to and remove it properly
    Oop myList = memory_.fetchPointer(ProcessMyListIndex, activeProcess);
    if (myList.isObject() && myList.rawBits() != nilObj.rawBits()) {
        // Remove from its linked list properly
        removeProcessFromList(activeProcess, myList);
        if (termLog && termCount <= 50) {
            fprintf(termLog, "[TERMINATE #%d] Removed from list 0x%llx\n",
                    termCount, (unsigned long long)myList.rawBits());
            fflush(termLog);
        }
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
    static int wakeCallCount = 0;
    static FILE* wakeLog = nullptr;
    wakeCallCount++;

    if constexpr (ENABLE_DEBUG_LOGGING) {
        if (!wakeLog) {
            wakeLog = nullptr;
        }
    }

    Oop nilObj = memory_.nil();
    Oop schedulerAssoc = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);
    Oop scheduler = memory_.fetchPointer(1, schedulerAssoc);
    Oop schedLists = memory_.fetchPointer(SchedulerProcessListsIndex, scheduler);

    ObjectHeader* listsHeader = schedLists.asObjectPtr();
    size_t numPriorities = listsHeader->slotCount();

    if (wakeLog && wakeCallCount <= 50) {
        fprintf(wakeLog, "[WAKE #%d] Scanning %zu priority levels\n", wakeCallCount, numPriorities);
        // Dump all ready queues
        for (int p = static_cast<int>(numPriorities) - 1; p >= 0; p--) {
            Oop processList = memory_.fetchPointer(p, schedLists);
            Oop first = memory_.fetchPointer(LinkedListFirstLinkIndex, processList);
            if (!first.isNil() && first.rawBits() != nilObj.rawBits()) {
                fprintf(wakeLog, "  Priority %d: first=0x%llx\n", p + 1, (unsigned long long)first.rawBits());
            }
        }
        fflush(wakeLog);
    }

    // Search from highest to lowest priority
    for (int p = static_cast<int>(numPriorities) - 1; p >= 0; p--) {
        Oop processList = memory_.fetchPointer(p, schedLists);
        Oop first = memory_.fetchPointer(LinkedListFirstLinkIndex, processList);

        if (!first.isNil() && first.rawBits() != nilObj.rawBits()) {
            // Found a runnable process - remove and return it
            Oop result = removeFirstLinkOfList(processList);
            if (wakeLog && wakeCallCount <= 50) {
                fprintf(wakeLog, "[WAKE #%d] -> Selected process 0x%llx at priority %d\n",
                        wakeCallCount, (unsigned long long)result.rawBits(), p + 1);
                fflush(wakeLog);
            }
            return result;
        }
    }

    // No runnable process found - this should not happen in a working system
    if (wakeLog && wakeCallCount <= 50) {
        fprintf(wakeLog, "[WAKE #%d] -> No runnable process found!\n", wakeCallCount);
        fflush(wakeLog);
    }
    return nilObj;
}

Oop Interpreter::wakeLowerPriorityProcess(int currentPriority) {
    // Similar to wakeHighestPriority but only considers processes at LOWER priorities
    // This is used for force-yield to give lower priority processes CPU time
    static int wakeLowerCount = 0;
    static FILE* wakeLowerLog = nullptr;
    wakeLowerCount++;

    if constexpr (ENABLE_DEBUG_LOGGING) {
        if (!wakeLowerLog) {
            wakeLowerLog = nullptr;
        }
    }

    Oop nilObj = memory_.nil();
    Oop schedulerAssoc = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);
    Oop scheduler = memory_.fetchPointer(1, schedulerAssoc);
    Oop schedLists = memory_.fetchPointer(SchedulerProcessListsIndex, scheduler);

    ObjectHeader* listsHeader = schedLists.asObjectPtr();
    size_t numPriorities = listsHeader->slotCount();

    // Current priority is 1-based, array index is 0-based
    int maxPriorityIndex = currentPriority - 2;  // One below current priority

    if (wakeLowerLog && wakeLowerCount <= 50) {
        fprintf(wakeLowerLog, "[WAKE-LOWER #%d] Current priority %d, scanning up to index %d\n",
                wakeLowerCount, currentPriority, maxPriorityIndex);
        fflush(wakeLowerLog);
    }

    // Every 5th call, specifically try lowIOPriority (10) first to prevent starvation
    // lowIOPriority is where the event loop runs
    if (wakeLowerCount % 5 == 0 && maxPriorityIndex >= 9) {
        int lowIOIndex = 9;  // Priority 10 = index 9
        Oop processList = memory_.fetchPointer(lowIOIndex, schedLists);
        Oop first = memory_.fetchPointer(LinkedListFirstLinkIndex, processList);
        if (!first.isNil() && first.rawBits() != nilObj.rawBits()) {
            Oop result = removeFirstLinkOfList(processList);
            if (wakeLowerLog && wakeLowerCount <= 50) {
                fprintf(wakeLowerLog, "[WAKE-LOWER #%d] -> PRIORITY BOOST: Selected lowIOPriority process 0x%llx\n",
                        wakeLowerCount, (unsigned long long)result.rawBits());
                fflush(wakeLowerLog);
            }
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
            if (wakeLowerLog && wakeLowerCount <= 50) {
                fprintf(wakeLowerLog, "[WAKE-LOWER #%d] -> Selected process 0x%llx at priority %d\n",
                        wakeLowerCount, (unsigned long long)result.rawBits(), p + 1);
                fflush(wakeLowerLog);
            }
            return result;
        }
    }

    // No lower priority process found
    if (wakeLowerLog && wakeLowerCount <= 50) {
        fprintf(wakeLowerLog, "[WAKE-LOWER #%d] -> No lower priority process found\n", wakeLowerCount);
        fflush(wakeLowerLog);
    }
    return nilObj;
}

void Interpreter::putToSleep(Oop process) {
    static int sleepCallCount = 0;
    static FILE* sleepLog = nullptr;
    sleepCallCount++;

    if constexpr (ENABLE_DEBUG_LOGGING) {
        if (!sleepLog) {
            sleepLog = nullptr;
        }
    }

    Oop schedulerAssoc = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);
    Oop scheduler = memory_.fetchPointer(1, schedulerAssoc);
    Oop schedLists = memory_.fetchPointer(SchedulerProcessListsIndex, scheduler);

    // Get process priority (1-based SmallInteger)
    Oop priorityOop = memory_.fetchPointer(ProcessPriorityIndex, process);
    int priority = static_cast<int>(priorityOop.asSmallInteger());

    // Get the appropriate priority list (0-indexed in array)
    Oop processList = memory_.fetchPointer(priority - 1, schedLists);

    if (sleepLog && sleepCallCount <= 50) {
        fprintf(sleepLog, "[SLEEP #%d] Adding process 0x%llx to priority %d queue\n",
                sleepCallCount, (unsigned long long)process.rawBits(), priority);
        fflush(sleepLog);
    }

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
            if (numItems > 200) numItems = 200;  // Sanity limit
            memory_.storePointer(2, activeContext_, Oop::fromSmallInteger(numItems));

            static const int ContextFixedFields = 6;
            ObjectHeader* ctxHdr = activeContext_.asObjectPtr();
            size_t ctxSlots = ctxHdr->slotCount();

            // Sync temps: context → C++ (preserves Smalltalk modifications like tempNamed:put:)
            for (int i = 0; i < numTemps && i < numItems && (ContextFixedFields + i) < static_cast<int>(ctxSlots); i++) {
                *(framePointer_ + 1 + i) = memory_.fetchPointer(ContextFixedFields + i, activeContext_);
            }

            // Sync expression stack: C++ → context (interpreter manages the stack)
            for (int i = numTemps; i < numItems && (ContextFixedFields + i) < static_cast<int>(ctxSlots); i++) {
                Oop item = *(framePointer_ + 1 + i);
                memory_.storePointer(ContextFixedFields + i, activeContext_, item);
            }
        }
        return activeContext_;
    }

    static FILE* matLog = nullptr;

    // Build contexts from bottom to top (oldest to newest)
    // CRITICAL: frame[0] represents the same activation as activeContext_ when
    // frame[0].savedActiveContext == activeContext_. In that case, we must UPDATE
    // activeContext_ with frame[0]'s data instead of creating a duplicate context.
    // Without this, the sender chain has two contexts for the same method activation,
    // causing loops to execute extra iterations after exception handling returns.
    Oop sender = activeContext_;
    size_t startFrame = 0;

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
                // Sync temps: C++ → context. The C++ stack is authoritative for
                // saved frame 0's temps because:
                // 1. Write-through in setTemporary keeps them in sync at depth 0
                // 2. But the context can become stale when a value is modified by
                //    a path that doesn't go through setTemporary (e.g., GC-moved
                //    references, or values loaded during pushFrame that differ from
                //    the context's snapshot).
                // Expression stack: C++ → context (as before).
                size_t ctxSlots = acHdr->slotCount();
                for (int t = 0; t < numTemps && t < 32; t++) {
                    if (static_cast<size_t>(CtxFixed + t) < ctxSlots) {
                        Oop cppVal = *(frame0.savedFP + 1 + t);
                        memory_.storePointer(CtxFixed + t, activeContext_, cppVal);
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
        }
    }

    for (size_t i = startFrame; i < frameDepth_; i++) {
        auto& frame = savedFrames_[i];  // non-const: may update materializedContext

        if (matLog) {
            // Calculate PC for logging
            int logPc = 1;
            if (frame.savedMethod.isObject() && frame.savedMethod.rawBits() > 0x10000) {
                ObjectHeader* mHdr = frame.savedMethod.asObjectPtr();
                uint8_t* mBytes = mHdr->bytes();
                if (frame.savedIP >= mBytes && frame.savedIP < mBytes + mHdr->byteSize()) {
                    logPc = static_cast<int>(frame.savedIP - mBytes) + 1;
                }
            }
            fprintf(matLog, "[MATERIALIZE] Frame %zu: method=0x%llx receiver=0x%llx pc=%d\n",
                    i, (unsigned long long)frame.savedMethod.rawBits(),
                    (unsigned long long)frame.savedReceiver.rawBits(), logPc);
            fflush(matLog);
        }

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
                if (matLog) {
                    fprintf(matLog, "[MATERIALIZE] Failed to allocate context!\n");
                    fflush(matLog);
                }
                return activeContext_;  // Fall back to old behavior
            }
            // Cache this context for future materializations of the same frame
            frame.materializedContext = context;
        }

        // Calculate PC (byte offset from method start)
        uint8_t* methodBytes = methodHdr->bytes();
        int pc = 1;  // Default to start
        if (frame.savedIP >= methodBytes && frame.savedIP < methodBytes + methodHdr->byteSize()) {
            pc = static_cast<int>(frame.savedIP - methodBytes) + 1;  // 1-based
        }

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
        // Check if this is doDrawCycleWith: for debugging
        bool isDrawCycle = false;
        if (numLiterals >= 3) {
            Oop sel = memory_.fetchPointer(numLiterals - 1, frame.savedMethod);
            if (sel.isObject() && sel.rawBits() > 0x10000) {
                ObjectHeader* selH = sel.asObjectPtr();
                if (selH->isBytesObject() && selH->byteSize() == 18) {
                    std::string sn((char*)selH->bytes(), 18);
                    if (sn == "doDrawCycleWith:") isDrawCycle = true;
                }
            }
        }
        if (frame.savedFP != nullptr) {
            // Save temps
            for (int t = 0; t < numTemps && t < 32; t++) {
                Oop temp = *(frame.savedFP + 1 + t);
                if (isDrawCycle && matLog) {
                    fprintf(matLog, "[MAT-DRAW] Frame %zu temp[%d] = 0x%llx%s\n",
                            i, t, (unsigned long long)temp.rawBits(),
                            temp.rawBits() == memory_.nil().rawBits() ? " (NIL!)" : "");
                    fflush(matLog);
                }
                memory_.storePointer(6 + t, context, temp);
                savedCount++;
            }

            // TRACE: Log ensure: context materialization (prim 198)
            {
                int matPrimIdx = primitiveIndexOf(frame.savedMethod);
                if (matPrimIdx == 198) {
                    static int matEnsureCount = 0;
                    matEnsureCount++;
                }
            }

            // Save expression stack items above the temps.
            // The expression stack ends where the next frame's receiver starts.
            // For frame i, the next frame's FP (savedFrames[i+1].savedFP for i < frameDepth_-1,
            // or the current framePointer_ for the last saved frame) points to the next frame's
            // receiver. But the receiver and args of the next call were pushed BY this frame,
            // so they should NOT be saved (they're consumed by the send).
            // The next frame has numArgs_next arguments, so the receiver+args occupy
            // (numArgs_next + 1) slots. Expression items = everything between our temps
            // and those receiver+args.
            Oop* exprStart = frame.savedFP + 1 + numTemps;
            Oop* nextFrameStart;
            int nextArgCount;
            if (i + 1 < frameDepth_) {
                nextFrameStart = savedFrames_[i + 1].savedFP;
                // The next frame's argCount is savedFrames[i+1].savedArgCount?
                // No, savedArgCount stores the CALLER's argCount. We need the callee's argCount.
                // The callee's argCount is encoded in the next frame's method header.
                Oop nextMethodHdr = memory_.fetchPointer(0, savedFrames_[i + 1].savedMethod);
                nextArgCount = nextMethodHdr.isSmallInteger()
                    ? static_cast<int>((nextMethodHdr.asSmallInteger() >> 24) & 0xF) : 0;
            } else {
                // Last saved frame: the "next frame" is the current executing frame
                nextFrameStart = framePointer_;
                nextArgCount = argCount_;
            }
            // The callee's receiver + args occupy (nextArgCount + 1) slots ending at nextFrameStart
            // Expression items are from exprStart to (nextFrameStart - nextArgCount - 1)
            Oop* exprEnd = nextFrameStart - nextArgCount - 1;  // -1 for receiver
            // Actually, nextFrameStart IS the receiver position, so items from
            // nextFrameStart-nextArgCount to nextFrameStart are the args.
            // Wait: receiver is at nextFrameStart[0], args at nextFrameStart[1..argCount].
            // No — the stack grows UP: receiver is pushed first, then args.
            // So receiver is at the LOWEST address, args above.
            // nextFrameStart = receiver position = stackPointer_old - argCount - 1
            // The receiver+args start at nextFrameStart and span (argCount+1) slots.
            // Expression items are BELOW the receiver: from exprStart to nextFrameStart.
            Oop* exprEndPtr = nextFrameStart;  // expression ends before callee receiver
            if (isDrawCycle && matLog) {
                fprintf(matLog, "[MAT-DRAW] Frame %zu: savedFP=%p numTemps=%d exprStart=%p exprEnd=%p count=%ld\n",
                        i, (void*)frame.savedFP, numTemps, (void*)exprStart, (void*)exprEndPtr,
                        (long)(exprEndPtr - exprStart));
                fflush(matLog);
            }
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
        // Log what method_ actually is
        if (matLog) {
            std::string curMethod = "?";
            Oop mhdr = memory_.fetchPointer(0, method_);
            if (mhdr.isSmallInteger()) {
                int64_t hv = mhdr.asSmallInteger();
                int nLits = hv & 0x7FFF;
                if (nLits >= 2 && nLits < 100) {
                    Oop sel = memory_.fetchPointer(nLits - 1, method_);
                    if (sel.isObject()) {
                        ObjectHeader* selH = sel.asObjectPtr();
                        if (selH->isBytesObject() && selH->byteSize() < 100) {
                            curMethod = std::string((char*)selH->bytes(), selH->byteSize());
                        }
                    }
                }
            }
            fprintf(matLog, "[MATERIALIZE] Current method_ is #%s\n", curMethod.c_str());
            fflush(matLog);
        }

        ObjectHeader* methodHdr = method_.asObjectPtr();
        Oop methodHeader = memory_.fetchPointer(0, method_);
        if (methodHeader.isSmallInteger()) {
            int64_t headerValue = methodHeader.asSmallInteger();
            int numTemps = (headerValue >> 18) & 0x3F;  // Fixed: was using wrong bit offset

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
                // Use indexOfClass to get the class table index, NOT the object's own classIndex (which is the metaclass)
                uint32_t classIndex = contextClass.isObject() ? memory_.indexOfClass(contextClass) : 0;
                if (classIndex == 0) {
                    classIndex = 36;  // Fallback to typical Context class index
                }

                // Use IndexableWithFixed for contexts (format 3)
                context = memory_.allocateSlots(classIndex, contextSize, ObjectFormat::IndexableWithFixed);
                // Cache for future materializations of this frame
                currentFrameMaterializedCtx_ = context;
            }
            if (!context.isNil()) {
                uint8_t* methodBytes = methodHdr->bytes();
                int pc = 1;
                if (instructionPointer_ >= methodBytes && instructionPointer_ < methodBytes + methodHdr->byteSize()) {
                    pc = static_cast<int>(instructionPointer_ - methodBytes) + 1;
                }

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

    // Validate newProcess
    if (!newProcess.isObject()) {
        return;
    }

    ObjectHeader* newProcHdr = newProcess.asObjectPtr();
    if (newProcHdr->slotCount() < 2) {
        return;
    }

    // Log process switches with priorities
    {
        static int switchCount = 0;
        switchCount++;
        int oldPri = -1, newPri = -1;
        if (oldProcess.isObject() && oldProcess.rawBits() > 0x10000) {
            ObjectHeader* oldHdr = oldProcess.asObjectPtr();
            if (oldHdr->slotCount() > ProcessPriorityIndex) {
                Oop p = memory_.fetchPointer(ProcessPriorityIndex, oldProcess);
                oldPri = p.isSmallInteger() ? p.asSmallInteger() : -1;
            }
        }
        Oop p = memory_.fetchPointer(ProcessPriorityIndex, newProcess);
        newPri = p.isSmallInteger() ? p.asSmallInteger() : -1;
        // Track p40 process for diagnostic
        if (newPri == 40) g_savedP40Process = newProcess;
        else if (oldPri == 40) g_savedP40Process = oldProcess;
        if (switchCount <= 50 || switchCount % 5000 == 0) {
            fprintf(stderr, "[SWITCH #%d] pri %d -> %d step=%lld sel=%s cls=%s\n",
                    switchCount, oldPri, newPri, (long long)g_watchdogSteps.load(),
                    g_lastSelName, g_watchdogReceiverClass);
        }
    }

    // Save current execution state to old process's suspendedContext
    // If we have inline frames, materialize them into context objects
    Oop contextToSave = materializeFrameStack();

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

    // Resume execution from the new context
    executeFromContext(newContext);
}

bool Interpreter::tryReschedule() {
    // Debug: dump scheduler queues periodically
    static FILE* schedLog = nullptr;
    static int schedDump = 0;
    if constexpr (ENABLE_DEBUG_LOGGING) {
        if (!schedLog) schedLog = nullptr;
    }
    schedDump++;

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

    if (schedLog && schedDump == 1) {
        // Get active process priority
        Oop activePriorityOop = memory_.fetchPointer(ProcessPriorityIndex, activeProcess);
        int activePriority = activePriorityOop.isSmallInteger() ?
                             static_cast<int>(activePriorityOop.asSmallInteger()) : 0;
        fprintf(schedLog, "[SCHED] activeProcess=0x%llx priority=%d queues=0x%llx\n",
                (unsigned long long)activeProcess.rawBits(),
                activePriority,
                (unsigned long long)queues.rawBits());
        if (queues.isObject()) {
            ObjectHeader* qH = queues.asObjectPtr();
            fprintf(schedLog, "[SCHED] queues has %zu slots\n", qH->slotCount());
            // Show queues around the active priority and low priorities
            for (size_t qi = 0; qi < qH->slotCount(); qi++) {
                Oop q = qH->slotAt(qi);
                if (q.isObject() && q.rawBits() != nilObj.rawBits()) {
                    Oop first = memory_.fetchPointer(0, q);
                    if (first.rawBits() != nilObj.rawBits()) {  // Only show non-empty queues
                        fprintf(schedLog, "[SCHED] queue[%zu] firstLink=0x%llx\n",
                                qi, (unsigned long long)first.rawBits());
                    }
                }
            }
        }
        fflush(schedLog);
    }

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
        if (schedLog && schedDump <= 50) {
            Oop prioOop = memory_.fetchPointer(2, process);
            int prio = prioOop.isSmallInteger() ? static_cast<int>(prioOop.asSmallInteger()) : -1;
            fprintf(schedLog, "[RESCHED #%d] Selecting process=0x%llx priority=%d context=0x%llx from queue[%d]\n",
                    schedDump, (unsigned long long)process.rawBits(), prio,
                    (unsigned long long)context.rawBits(), i);
            fflush(schedLog);
        }

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
    static FILE* preemptLog = nullptr;
    static int preemptCount = 0;
    if constexpr (ENABLE_DEBUG_LOGGING) {
        if (!preemptLog) preemptLog = nullptr;
    }
    preemptCount++;

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
    if (preemptLog && (preemptCount % 1000 == 1)) {
        fprintf(preemptLog, "[PREEMPT #%d] activePriority=%d checking queues...\n",
                preemptCount, activePriority);
        // Show what's in the queues
        for (size_t qi = 0; qi < numQueues; qi++) {
            Oop q = queuesHeader->slotAt(qi);
            if (q.isObject() && q.rawBits() != nilObj.rawBits()) {
                Oop first = memory_.fetchPointer(0, q);
                if (first.rawBits() != nilObj.rawBits()) {
                    fprintf(preemptLog, "[PREEMPT]   queue[%zu] has process 0x%llx\n",
                            qi, (unsigned long long)first.rawBits());
                }
            }
        }
        fflush(preemptLog);
    }

    // Check for runnable processes at SAME or higher priority (round-robin).
    // Priorities are 1-based, queue indices are 0-based: queue[p-1] = priority p.
    size_t startIdx = (activePriority > 0) ? static_cast<size_t>(activePriority - 1) : 0;
    for (size_t i = startIdx; i < numQueues; i++) {
        Oop queue = queuesHeader->slotAt(i);
        if (!queue.isObject() || queue.rawBits() == nilObj.rawBits()) continue;

        Oop firstProcess = memory_.fetchPointer(0, queue);
        if (!firstProcess.isObject() || firstProcess.rawBits() == nilObj.rawBits()) continue;

        // Skip if it's the current process
        if (firstProcess.rawBits() == activeProcess.rawBits()) continue;

        // Found a runnable process - preempt!
        if (preemptLog && preemptCount <= 100) {
            fprintf(preemptLog, "[PREEMPT #%d] Switching from priority %d to process at priority %zu\n",
                    preemptCount, activePriority, i);
            fflush(preemptLog);
        }

        // Remove process from ready queue using the proper helper
        // (removeFirstLinkOfList clears both nextLink and myList)
        removeFirstLinkOfList(queue);

        // Put current process back on ready queue
        putToSleep(activeProcess);

        // Switch to new process
        g_xferReason = "checkPreemption";
        transferTo(firstProcess);
        return;
    }
}

// ===== STARTUP SUPPORT =====

void Interpreter::installOSiOSDriver() {
    // Try to install OSiOSDriver to start the event loop
    // This is called from step() after the image has had time to initialize

    static FILE* driverLog = nullptr;
    if (driverLog) {
        fprintf(driverLog, "[DRIVER] installOSiOSDriver called (step 100K)\n");
        fflush(driverLog);
    }

    // Test if dlsym can find SDL2 functions at runtime
    if (driverLog) {
        void* sdlInit = dlsym(RTLD_DEFAULT, "SDL_Init");
        void* sdlPoll = dlsym(RTLD_DEFAULT, "SDL_PollEvent");
        void* sdlQuit = dlsym(RTLD_DEFAULT, "SDL_Quit");
        fprintf(driverLog, "[DRIVER] dlsym SDL_Init=%p SDL_PollEvent=%p SDL_Quit=%p\n",
                sdlInit, sdlPoll, sdlQuit);
        if (sdlInit) {
            fprintf(driverLog, "[DRIVER] SUCCESS: SDL2 functions are accessible via dlsym!\n");
        } else {
            fprintf(driverLog, "[DRIVER] FAILED: SDL2 functions NOT found via dlsym\n");
        }
        fflush(driverLog);
    }

    Oop nilObj = memory_.specialObject(SpecialObjectIndex::NilObject);
    Oop osDriverClass = memory_.findGlobal("OSiOSDriver");

    if (driverLog) {
        fprintf(driverLog, "[DRIVER] nilObj: 0x%llx\n", (unsigned long long)nilObj.rawBits());
        fprintf(driverLog, "[DRIVER] OSiOSDriver class: 0x%llx isNil=%d isObj=%d sameAsNil=%d\n",
                (unsigned long long)osDriverClass.rawBits(), osDriverClass.isNil() ? 1 : 0,
                osDriverClass.isObject() ? 1 : 0,
                osDriverClass.rawBits() == nilObj.rawBits() ? 1 : 0);
        fflush(driverLog);
    }

    // Check if OSiOSDriver was actually found (compare against nilObj, not just isNil())
    if (osDriverClass.rawBits() == nilObj.rawBits()) {
        if (driverLog) {
            fprintf(driverLog, "[DRIVER] OSiOSDriver not found in image - trying InputEventSensor\n");
            fflush(driverLog);
        }

        // Try InputEventSensor instead
        Oop sensor = memory_.findGlobal("Sensor");
        if (driverLog) {
            fprintf(driverLog, "[DRIVER] Sensor global: 0x%llx sameAsNil=%d\n",
                    (unsigned long long)sensor.rawBits(),
                    sensor.rawBits() == nilObj.rawBits() ? 1 : 0);
            fflush(driverLog);
        }
        if (sensor.isObject() && sensor.rawBits() != nilObj.rawBits()) {
            Oop sensorClass = memory_.classOf(sensor);
            if (driverLog) {
                Oop className = memory_.fetchPointer(6, sensorClass);
                std::string name = "<unknown>";
                if (className.isObject()) {
                    ObjectHeader* hdr = className.asObjectPtr();
                    if (hdr->isBytesObject() && hdr->byteSize() < 50) {
                        name = std::string((char*)hdr->bytes(), hdr->byteSize());
                    }
                }
                fprintf(driverLog, "[DRIVER] Sensor class: %s (0x%llx)\n", name.c_str(),
                        (unsigned long long)sensorClass.rawBits());
                fflush(driverLog);
            }
            if (sensorClass.isObject() && sensorClass.rawBits() != nilObj.rawBits()) {
                fprintf(driverLog, "[DRIVER] InputEventSensor found - checking if event loop is running\n");
                fflush(driverLog);

                // The event loop should already be running from session startup.
                // Let's verify by checking if primitive 264 is being called.
                // If events aren't being consumed, we might need to start the loop.
            }
        } else {
            if (driverLog) {
                fprintf(driverLog, "[DRIVER] Sensor global not found\n");

                // Check if InputEventSensor class exists
                Oop ieClass = memory_.findGlobal("InputEventSensor");
                fprintf(driverLog, "[DRIVER] InputEventSensor class: 0x%llx sameAsNil=%d\n",
                        (unsigned long long)ieClass.rawBits(),
                        ieClass.rawBits() == nilObj.rawBits() ? 1 : 0);

                // Check OSWindow infrastructure
                Oop osWindowDriverClass = memory_.findGlobal("OSWindowDriver");
                fprintf(driverLog, "[DRIVER] OSWindowDriver class: 0x%llx sameAsNil=%d\n",
                        (unsigned long long)osWindowDriverClass.rawBits(),
                        osWindowDriverClass.rawBits() == nilObj.rawBits() ? 1 : 0);

                Oop osWindow = memory_.findGlobal("OSWindow");
                fprintf(driverLog, "[DRIVER] OSWindow class: 0x%llx sameAsNil=%d\n",
                        (unsigned long long)osWindow.rawBits(),
                        osWindow.rawBits() == nilObj.rawBits() ? 1 : 0);

                // If OSWindowDriver exists, try to get the current driver and check its class
                if (osWindowDriverClass.isObject() && osWindowDriverClass.rawBits() != nilObj.rawBits()) {
                    // OSWindowDriver has a 'Current' class variable at slot 12 (after standard class slots)
                    // Class layout: superclass[0], methodDict[1], format[2], layout[3],
                    // instanceVariables[4], organization[5], name[6], classPool[7], sharedPools[8],
                    // environment[9], category[10], then class inst vars start at 11+
                    ObjectHeader* driverHdr = osWindowDriverClass.asObjectPtr();
                    fprintf(driverLog, "[DRIVER] OSWindowDriver class has %zu slots\n", driverHdr->slotCount());

                    // The Current class variable is in classPool (slot 7)
                    Oop classPool = memory_.fetchPointer(7, osWindowDriverClass);
                    fprintf(driverLog, "[DRIVER] OSWindowDriver classPool: 0x%llx isObj=%d\n",
                            (unsigned long long)classPool.rawBits(), classPool.isObject() ? 1 : 0);

                    // Check if there's a Current key in the classPool (which is a Dictionary)
                    // Dictionary layout: slot[0]=tally, slot[1]=array of associations
                    if (classPool.isObject() && classPool.rawBits() != nilObj.rawBits()) {
                        ObjectHeader* poolHdr = classPool.asObjectPtr();
                        fprintf(driverLog, "[DRIVER] ClassPool has %zu slots, format=%d\n", poolHdr->slotCount(), poolHdr->format());

                        // Get the associations array from slot 1
                        if (poolHdr->slotCount() >= 2) {
                            Oop assocArray = memory_.fetchPointer(1, classPool);
                            if (assocArray.isObject() && assocArray.rawBits() != nilObj.rawBits()) {
                                ObjectHeader* arrayHdr = assocArray.asObjectPtr();
                                fprintf(driverLog, "[DRIVER] ClassPool associations array has %zu slots\n", arrayHdr->slotCount());

                                // Look for "Current" in the associations
                                for (size_t i = 0; i < arrayHdr->slotCount(); i++) {
                                    Oop assoc = memory_.fetchPointer(i, assocArray);
                                    if (assoc.isObject() && assoc.rawBits() != nilObj.rawBits()) {
                                        ObjectHeader* assocHdr = assoc.asObjectPtr();
                                        if (assocHdr->slotCount() >= 2) {
                                            Oop key = memory_.fetchPointer(0, assoc);
                                            Oop val = memory_.fetchPointer(1, assoc);
                                            std::string keyName = "<notBytes>";
                                            if (key.isObject()) {
                                                ObjectHeader* keyHdr = key.asObjectPtr();
                                                if (keyHdr->isBytesObject() && keyHdr->byteSize() < 50) {
                                                    keyName = std::string((char*)keyHdr->bytes(), keyHdr->byteSize());
                                                }
                                            }
                                            fprintf(driverLog, "[DRIVER]   assoc[%zu]: key='%s' val=0x%llx",
                                                    i, keyName.c_str(), (unsigned long long)val.rawBits());
                                            if (val.rawBits() == nilObj.rawBits()) {
                                                fprintf(driverLog, " (nil)");
                                            } else if (val.isObject()) {
                                                Oop valClass = memory_.classOf(val);
                                                std::string valClassName = "<unknown>";
                                                if (valClass.isObject()) {
                                                    // Get class name
                                                    Oop nameOop = memory_.fetchPointer(6, valClass);
                                                    if (nameOop.isObject()) {
                                                        ObjectHeader* nameHdr = nameOop.asObjectPtr();
                                                        if (nameHdr->isBytesObject() && nameHdr->byteSize() < 100) {
                                                            valClassName = std::string((char*)nameHdr->bytes(), nameHdr->byteSize());
                                                        }
                                                    }
                                                }
                                                fprintf(driverLog, " (class=%s)", valClassName.c_str());
                                                // If this is a class (metaclass), get the actual class name
                                                if (valClassName.find("class") != std::string::npos || valClassName == "Class" || valClassName == "<unknown>") {
                                                    // Try to get name slot directly from val (if it's a class)
                                                    ObjectHeader* valHdr = val.asObjectPtr();
                                                    if (valHdr->slotCount() >= 7) {
                                                        Oop valName = memory_.fetchPointer(6, val);
                                                        if (valName.isObject()) {
                                                            ObjectHeader* vnHdr = valName.asObjectPtr();
                                                            if (vnHdr->isBytesObject() && vnHdr->byteSize() < 100) {
                                                                std::string name((char*)vnHdr->bytes(), vnHdr->byteSize());
                                                                fprintf(driverLog, " actualName='%s'", name.c_str());
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                            fprintf(driverLog, "\n");
                                        }
                                    }
                                }
                            }
                        }
                    }
                    fflush(driverLog);

                    // Check for World global
                    Oop world = memory_.findGlobal("World");
                    fprintf(driverLog, "[DRIVER] World global: 0x%llx sameAsNil=%d\n",
                            (unsigned long long)world.rawBits(),
                            world.rawBits() == nilObj.rawBits() ? 1 : 0);
                    if (world.isObject() && world.rawBits() != nilObj.rawBits()) {
                        Oop worldClass = memory_.classOf(world);
                        std::string className = "<unknown>";
                        if (worldClass.isObject()) {
                            Oop nameOop = memory_.fetchPointer(6, worldClass);
                            if (nameOop.isObject()) {
                                ObjectHeader* nameHdr = nameOop.asObjectPtr();
                                if (nameHdr->isBytesObject() && nameHdr->byteSize() < 100) {
                                    className = std::string((char*)nameHdr->bytes(), nameHdr->byteSize());
                                }
                            }
                        }
                        fprintf(driverLog, "[DRIVER] World class: %s\n", className.c_str());
                    }

                    // Check for InputEventFetcher
                    Oop eventFetcher = memory_.findGlobal("InputEventFetcher");
                    fprintf(driverLog, "[DRIVER] InputEventFetcher class: 0x%llx sameAsNil=%d\n",
                            (unsigned long long)eventFetcher.rawBits(),
                            eventFetcher.rawBits() == nilObj.rawBits() ? 1 : 0);

                    // Look for the Hand morph through World
                    // WorldMorph has: submorphs, owner, bounds, fullBounds, color, extension, eventHandler, hands
                    // hands is typically at slot 7 or similar
                    if (world.isObject() && world.rawBits() != nilObj.rawBits()) {
                        ObjectHeader* worldHdr = world.asObjectPtr();
                        fprintf(driverLog, "[DRIVER] World has %zu slots\n", worldHdr->slotCount());
                        // Find hands - search for an OrderedCollection or Array containing HandMorph
                        for (size_t i = 0; i < worldHdr->slotCount() && i < 20; i++) {
                            Oop slot = memory_.fetchPointer(i, world);
                            if (slot.isObject() && slot.rawBits() != nilObj.rawBits()) {
                                Oop slotClass = memory_.classOf(slot);
                                std::string className = "<unknown>";
                                if (slotClass.isObject()) {
                                    Oop nameOop = memory_.fetchPointer(6, slotClass);
                                    if (nameOop.isObject()) {
                                        ObjectHeader* nameHdr = nameOop.asObjectPtr();
                                        if (nameHdr->isBytesObject() && nameHdr->byteSize() < 100) {
                                            className = std::string((char*)nameHdr->bytes(), nameHdr->byteSize());
                                        }
                                    }
                                }
                                ObjectHeader* slotHdr = slot.asObjectPtr();
                                fprintf(driverLog, "[DRIVER] World slot[%zu]: class=%s slots=%zu\n",
                                        i, className.c_str(), slotHdr->slotCount());
                                // If it's WorldState, examine its slots
                                if (className == "WorldState") {
                                    fprintf(driverLog, "[DRIVER] Found WorldState - examining slots:\n");
                                    for (size_t j = 0; j < slotHdr->slotCount() && j < 15; j++) {
                                        Oop wsSlot = memory_.fetchPointer(j, slot);
                                        std::string wsClassName = "nil";
                                        size_t wsSlotCount = 0;
                                        if (wsSlot.isObject() && wsSlot.rawBits() != nilObj.rawBits()) {
                                            Oop wsSlotClass = memory_.classOf(wsSlot);
                                            if (wsSlotClass.isObject()) {
                                                Oop wsNameOop = memory_.fetchPointer(6, wsSlotClass);
                                                if (wsNameOop.isObject()) {
                                                    ObjectHeader* wsNameHdr = wsNameOop.asObjectPtr();
                                                    if (wsNameHdr->isBytesObject() && wsNameHdr->byteSize() < 100) {
                                                        wsClassName = std::string((char*)wsNameHdr->bytes(), wsNameHdr->byteSize());
                                                    }
                                                }
                                            }
                                            wsSlotCount = wsSlot.asObjectPtr()->slotCount();
                                        } else if (wsSlot.isSmallInteger()) {
                                            wsClassName = "SmallInt";
                                        }
                                        fprintf(driverLog, "[DRIVER]   WorldState slot[%zu]: class=%s slots=%zu\n",
                                                j, wsClassName.c_str(), wsSlotCount);
                                        // Look for Array or OrderedCollection containing HandMorph
                                        if (wsClassName == "Array" && wsSlotCount > 0) {
                                            for (size_t k = 0; k < wsSlotCount && k < 3; k++) {
                                                Oop elem = memory_.fetchPointer(k, wsSlot);
                                                if (elem.isObject() && elem.rawBits() != nilObj.rawBits()) {
                                                    Oop elemClass = memory_.classOf(elem);
                                                    std::string elemClassName = "<unknown>";
                                                    if (elemClass.isObject()) {
                                                        Oop elemName = memory_.fetchPointer(6, elemClass);
                                                        if (elemName.isObject()) {
                                                            ObjectHeader* enHdr = elemName.asObjectPtr();
                                                            if (enHdr->isBytesObject() && enHdr->byteSize() < 100) {
                                                                elemClassName = std::string((char*)enHdr->bytes(), enHdr->byteSize());
                                                            }
                                                        }
                                                    }
                                                    fprintf(driverLog, "[DRIVER]     Array[%zu]: class=%s\n", k, elemClassName.c_str());
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // Check for MorphicUIManager
                    Oop uiManager = memory_.findGlobal("MorphicUIManager");
                    fprintf(driverLog, "[DRIVER] MorphicUIManager class: 0x%llx sameAsNil=%d\n",
                            (unsigned long long)uiManager.rawBits(),
                            uiManager.rawBits() == nilObj.rawBits() ? 1 : 0);

                    // Check for event-related classes
                    const char* eventClasses[] = {
                        "MouseEvent", "MouseButtonEvent", "MouseMoveEvent",
                        "MouseButtonPressedEvent", "MouseButtonReleasedEvent",
                        "KeyboardEvent", "MorphicEvent", "UserInputEvent",
                        "OSMouseButtonPressEvent", "OSMouseMoveEvent",
                        nullptr
                    };
                    for (int i = 0; eventClasses[i]; i++) {
                        Oop evtClass = memory_.findGlobal(eventClasses[i]);
                        if (evtClass.isObject() && evtClass.rawBits() != nilObj.rawBits()) {
                            fprintf(driverLog, "[DRIVER] Found event class: %s at 0x%llx\n",
                                    eventClasses[i], (unsigned long long)evtClass.rawBits());
                        }
                    }

                    // Check for driver-related classes
                    const char* driverClasses[] = {
                        "OSNullDriver", "OSNullWindowDriver", "OSHeadlessDriver",
                        "NullWindowDriver", "HeadlessDriver",
                        "OSHeadlessWorldRenderer", "OSNullWorldRenderer",
                        "OSWindowMorphicEventHandler",
                        "OSAbstractRenderer", "OSWorldRenderer",
                        nullptr
                    };
                    for (int i = 0; driverClasses[i]; i++) {
                        Oop driverClass = memory_.findGlobal(driverClasses[i]);
                        if (driverClass.isObject() && driverClass.rawBits() != nilObj.rawBits()) {
                            fprintf(driverLog, "[DRIVER] Found driver class: %s at 0x%llx\n",
                                    driverClasses[i], (unsigned long long)driverClass.rawBits());
                        }
                    }

                    // Check for UIManager (the singleton, not the class)
                    Oop uiManagerSingleton = memory_.findGlobal("UIManager");
                    fprintf(driverLog, "[DRIVER] UIManager global: 0x%llx sameAsNil=%d\n",
                            (unsigned long long)uiManagerSingleton.rawBits(),
                            uiManagerSingleton.rawBits() == nilObj.rawBits() ? 1 : 0);

                    // Check process list to see what's running
                    // Processor activeProcess gives us the current process
                    Oop scheduler = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);
                    if (scheduler.isObject()) {
                        Oop processScheduler = memory_.fetchPointer(1, scheduler);  // value
                        if (processScheduler.isObject()) {
                            // ProcessScheduler>>activeProcess is inst var 0
                            Oop activeProcess = memory_.fetchPointer(0, processScheduler);
                            fprintf(driverLog, "[DRIVER] Active process: 0x%llx\n", (unsigned long long)activeProcess.rawBits());
                            if (activeProcess.isObject() && activeProcess.rawBits() != nilObj.rawBits()) {
                                // Process has: suspendedContext, priority, myList, name, env
                                // name is at slot 3
                                ObjectHeader* procHdr = activeProcess.asObjectPtr();
                                if (procHdr->slotCount() >= 4) {
                                    Oop procName = memory_.fetchPointer(3, activeProcess);
                                    if (procName.isObject()) {
                                        ObjectHeader* nameHdr = procName.asObjectPtr();
                                        if (nameHdr->isBytesObject() && nameHdr->byteSize() < 100) {
                                            std::string name((char*)nameHdr->bytes(), nameHdr->byteSize());
                                            fprintf(driverLog, "[DRIVER] Active process name: '%s'\n", name.c_str());
                                        }
                                    }
                                }
                            }
                            // List all processes in all priority queues
                            // quiescentProcessLists is inst var 1
                            Oop processLists = memory_.fetchPointer(1, processScheduler);
                            if (processLists.isObject()) {
                                ObjectHeader* listsHdr = processLists.asObjectPtr();
                                fprintf(driverLog, "[DRIVER] Process priority levels: %zu\n", listsHdr->slotCount());
                            }
                        }
                    }
                    fflush(driverLog);

                    // Try to install OSSDL2Driver as Current (we have SDL2 stubs that will handle events)
                    // Get DriverClass which should be OSSDL2Driver
                    Oop sdl2DriverClass = memory_.findGlobal("OSSDL2Driver");
                    if (sdl2DriverClass.rawBits() == nilObj.rawBits()) {
                        fprintf(driverLog, "[DRIVER] OSSDL2Driver not found, falling back to OSNullWindowDriver\n");
                        sdl2DriverClass = memory_.findGlobal("OSNullWindowDriver");
                    }

                    if (sdl2DriverClass.isObject() && sdl2DriverClass.rawBits() != nilObj.rawBits()) {
                        fprintf(driverLog, "[DRIVER] Attempting to install OSSDL2Driver as Current\n");
                        fflush(driverLog);

                        // Check the class format to see how many instance vars it has
                        Oop formatOop = memory_.fetchPointer(2, sdl2DriverClass);  // format at slot 2
                        fprintf(driverLog, "[DRIVER] OSSDL2Driver format: 0x%llx\n",
                                (unsigned long long)formatOop.rawBits());

                        // Get instance spec to determine number of instance variables
                        int64_t format = 0;
                        if (formatOop.isSmallInteger()) {
                            format = formatOop.asSmallInteger();
                            fprintf(driverLog, "[DRIVER] Format value: %lld\n", format);
                        }

                        // Find the Current association in OSWindowDriver's classPool
                        if (osWindowDriverClass.isObject()) {
                            Oop classPool = memory_.fetchPointer(7, osWindowDriverClass);
                            if (classPool.isObject()) {
                                ObjectHeader* poolHdr = classPool.asObjectPtr();
                                if (poolHdr->slotCount() >= 2) {
                                    Oop assocArray = memory_.fetchPointer(1, classPool);
                                    if (assocArray.isObject()) {
                                        ObjectHeader* arrayHdr = assocArray.asObjectPtr();
                                        // Find Current association and set its value
                                        for (size_t i = 0; i < arrayHdr->slotCount(); i++) {
                                            Oop assoc = memory_.fetchPointer(i, assocArray);
                                            if (assoc.isObject() && assoc.rawBits() != nilObj.rawBits()) {
                                                ObjectHeader* assocHdr = assoc.asObjectPtr();
                                                if (assocHdr->slotCount() >= 2) {
                                                    Oop key = memory_.fetchPointer(0, assoc);
                                                    if (key.isObject()) {
                                                        ObjectHeader* keyHdr = key.asObjectPtr();
                                                        if (keyHdr->isBytesObject() && keyHdr->byteSize() == 7) {
                                                            std::string keyName((char*)keyHdr->bytes(), keyHdr->byteSize());
                                                            if (keyName == "Current") {
                                                                // Create an instance of OSSDL2Driver (we have SDL2 stubs that will handle events)
                                                                // Need to find out how many inst vars it has

                                                                // First, let me try creating a minimal instance
                                                                // OSWindowDriver has inst vars, let's check format
                                                                ObjectHeader* ndcHdr = sdl2DriverClass.asObjectPtr();
                                                                fprintf(driverLog, "[DRIVER] OSSDL2Driver class has %zu slots\n", ndcHdr->slotCount());

                                                                // Get class index from identity hash (this is how Pharo stores class refs)
                                                                uint32_t classIdx = ndcHdr->identityHash();
                                                                fprintf(driverLog, "[DRIVER] OSSDL2Driver class index: %u\n", classIdx);

                                                                // Get instance format and size from the class
                                                                // format is at slot 2, contains encoding of inst var count
                                                                Oop formatOop2 = memory_.fetchPointer(2, sdl2DriverClass);
                                                                size_t instVarCount = 0;
                                                                if (formatOop2.isSmallInteger()) {
                                                                    int64_t fmt = formatOop2.asSmallInteger();
                                                                    fprintf(driverLog, "[DRIVER] Format raw value: %lld\n", fmt);
                                                                    // Extract inst var count from format (bits 0-15)
                                                                    instVarCount = fmt & 0xFFFF;
                                                                    fprintf(driverLog, "[DRIVER] Extracted instVarCount: %zu\n", instVarCount);
                                                                }

                                                                // Allocate the instance using allocateSlots
                                                                Oop driverInstance = memory_.allocateSlots(classIdx, instVarCount, ObjectFormat::FixedSize);
                                                                if (driverInstance.isObject() && driverInstance.rawBits() != nilObj.rawBits()) {
                                                                    // Set the Current value
                                                                    memory_.storePointer(1, assoc, driverInstance);
                                                                    fprintf(driverLog, "[DRIVER] Successfully set OSSDL2Driver instance as Current: 0x%llx\n",
                                                                            (unsigned long long)driverInstance.rawBits());

                                                                    // Verify it was set
                                                                    Oop verifyVal = memory_.fetchPointer(1, assoc);
                                                                    fprintf(driverLog, "[DRIVER] Verified Current value: 0x%llx\n",
                                                                            (unsigned long long)verifyVal.rawBits());

                                                                    // CRITICAL: Start the event loop by calling ensureEventLoop on the driver
                                                                    // Inline method lookup since lookupMethodInClass is in another function
                                                                    auto findMethodInClass = [&](Oop classObj, const char* selectorName, bool dumpAll = false) -> Oop {
                                                                        if (!classObj.isObject()) return Oop::nil();
                                                                        Oop methodDict = memory_.fetchPointer(1, classObj);
                                                                        if (!methodDict.isObject()) return Oop::nil();
                                                                        ObjectHeader* mdHeader = methodDict.asObjectPtr();
                                                                        size_t mdSlots = mdHeader->slotCount();
                                                                        if (dumpAll) {
                                                                            fprintf(driverLog, "[DRIVER] MethodDict slots=%zu format=%u\n", mdSlots, mdHeader->format());
                                                                        }
                                                                        for (size_t mi = 2; mi < mdSlots; mi++) {
                                                                            Oop key = mdHeader->slotAt(mi);
                                                                            if (!key.isObject() || key.isNil()) continue;
                                                                            ObjectHeader* keyHdr = key.asObjectPtr();
                                                                            if (!keyHdr->isBytesObject()) {
                                                                                if (dumpAll) {
                                                                                    fprintf(driverLog, "[DRIVER]   slot[%zu]: not bytes (fmt=%u)\n", mi, keyHdr->format());
                                                                                }
                                                                                continue;
                                                                            }
                                                                            size_t keyLen = keyHdr->byteSize();
                                                                            if (dumpAll && keyLen < 50) {
                                                                                std::string methodName((char*)keyHdr->bytes(), keyLen);
                                                                                fprintf(driverLog, "[DRIVER]   slot[%zu]: '%s'\n", mi, methodName.c_str());
                                                                            }
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

                                                                    // First try instance method ensureEventLoop
                                                                    fprintf(driverLog, "[DRIVER] Looking for ensureEventLoop in OSSDL2Driver, dumping methods:\n");
                                                                    Oop ensureMethod = findMethodInClass(sdl2DriverClass, "ensureEventLoop", true);
                                                                    if (ensureMethod.isObject() && ensureMethod.rawBits() != nilObj.rawBits()) {
                                                                        fprintf(driverLog, "[DRIVER] Found instance ensureEventLoop method, queuing for execution\n");
                                                                        pendingDriverInstallMethod_ = ensureMethod;
                                                                        pendingDriverInstallReceiver_ = driverInstance;
                                                                        hasPendingDriverInstall_ = true;
                                                                        pendingDriverMethodNeedsArg_ = false;
                                                                    } else {
                                                                        fprintf(driverLog, "[DRIVER] ensureEventLoop NOT found, trying setupEventLoop\n");
                                                                        // Try setupEventLoop (older Pharo images don't have ensureEventLoop)
                                                                        Oop setupMethod = findMethodInClass(sdl2DriverClass, "setupEventLoop");
                                                                        if (setupMethod.isObject() && setupMethod.rawBits() != nilObj.rawBits()) {
                                                                            fprintf(driverLog, "[DRIVER] Found setupEventLoop, queuing for execution\n");

                                                                            // Dump method details to understand what it does
                                                                            ObjectHeader* methHdr = setupMethod.asObjectPtr();
                                                                            Oop methHeader = memory_.fetchPointer(0, setupMethod);
                                                                            if (methHeader.isSmallInteger()) {
                                                                                int64_t hBits = methHeader.asSmallInteger();
                                                                                int numLits = hBits & 0x7FFF;
                                                                                int numTemps = (hBits >> 18) & 0x3F;
                                                                                int numArgs = (hBits >> 24) & 0xF;
                                                                                int primIdx = (hBits >> 28) & 0x3FF;
                                                                                fprintf(driverLog, "[DRIVER] setupEventLoop method: lits=%d temps=%d args=%d prim=%d\n",
                                                                                        numLits, numTemps, numArgs, primIdx);

                                                                                // Dump literals (selectors being called)
                                                                                for (int li = 1; li <= numLits && li <= 10; li++) {
                                                                                    Oop lit = memory_.fetchPointer(li, setupMethod);
                                                                                    if (lit.isObject() && lit.rawBits() > 0x10000) {
                                                                                        ObjectHeader* litH = lit.asObjectPtr();
                                                                                        if (litH->isBytesObject() && litH->byteSize() < 100) {
                                                                                            std::string s((char*)litH->bytes(), litH->byteSize());
                                                                                            fprintf(driverLog, "[DRIVER]   lit[%d]='%s'\n", li, s.c_str());
                                                                                        } else {
                                                                                            fprintf(driverLog, "[DRIVER]   lit[%d] fmt=%d cls=%u slots=%zu\n",
                                                                                                    li, (int)litH->format(), litH->classIndex(), litH->slotCount());
                                                                                        }
                                                                                    }
                                                                                }

                                                                                // Dump first 20 bytecodes
                                                                                size_t bcStart = (1 + numLits) * 8;
                                                                                uint8_t* bytes = methHdr->bytes();
                                                                                size_t totalBytes = methHdr->byteSize();
                                                                                fprintf(driverLog, "[DRIVER]   bytecodes (offset %zu):", bcStart);
                                                                                for (size_t bi = bcStart; bi < totalBytes && bi < bcStart + 20; bi++) {
                                                                                    fprintf(driverLog, " %02x", bytes[bi]);
                                                                                }
                                                                                fprintf(driverLog, "\n");
                                                                            }
                                                                            fflush(driverLog);

                                                                            pendingDriverInstallMethod_ = setupMethod;
                                                                            pendingDriverInstallReceiver_ = driverInstance;
                                                                            hasPendingDriverInstall_ = true;
                                                                            pendingDriverMethodNeedsArg_ = false;
                                                                        } else {
                                                                            fprintf(driverLog, "[DRIVER] setupEventLoop NOT found either, trying startUp:\n");
                                                                            // Try CLASS-SIDE startUp: - this is in the metaclass!
                                                                            // The metaclass is classOf(sdl2DriverClass)
                                                                            Oop metaclass = memory_.classOf(sdl2DriverClass);
                                                                            fprintf(driverLog, "[DRIVER] OSSDL2Driver metaclass: 0x%llx\n", (unsigned long long)metaclass.rawBits());
                                                                            Oop classStartUpMethod = findMethodInClass(metaclass, "startUp:");
                                                                            if (classStartUpMethod.isObject() && classStartUpMethod.rawBits() != nilObj.rawBits()) {
                                                                                fprintf(driverLog, "[DRIVER] Found CLASS-SIDE startUp: in metaclass, calling on class\n");
                                                                                pendingDriverInstallMethod_ = classStartUpMethod;
                                                                                pendingDriverInstallReceiver_ = sdl2DriverClass;  // Call on CLASS, not instance
                                                                                hasPendingDriverInstall_ = true;
                                                                                pendingDriverMethodNeedsArg_ = true;  // startUp: needs true as argument
                                                                            } else {
                                                                                fprintf(driverLog, "[DRIVER] CLASS-SIDE startUp: not found in metaclass\n");
                                                                                // Fall back to instance-side startUp:
                                                                                Oop instanceStartUpMethod = findMethodInClass(sdl2DriverClass, "startUp:");
                                                                                if (instanceStartUpMethod.isObject() && instanceStartUpMethod.rawBits() != nilObj.rawBits()) {
                                                                                    fprintf(driverLog, "[DRIVER] Found instance-side startUp: as fallback\n");
                                                                                    pendingDriverInstallMethod_ = instanceStartUpMethod;
                                                                                    pendingDriverInstallReceiver_ = driverInstance;
                                                                                    hasPendingDriverInstall_ = true;
                                                                                    pendingDriverMethodNeedsArg_ = true;
                                                                                } else {
                                                                                    fprintf(driverLog, "[DRIVER] No startUp: method found anywhere!\n");
                                                                                }
                                                                            }
                                                                        }
                                                                    }

                                                                    // Now let's examine the HandMorph structure
                                                                    // HandMorph was found at WorldState slot 7
                                                                    if (world.isObject() && world.rawBits() != nilObj.rawBits()) {
                                                                        Oop worldState = memory_.fetchPointer(9, world);  // slot 9 is WorldState
                                                                        if (worldState.isObject() && worldState.rawBits() != nilObj.rawBits()) {
                                                                            Oop handMorph = memory_.fetchPointer(7, worldState);  // slot 7 is HandMorph
                                                                            if (handMorph.isObject() && handMorph.rawBits() != nilObj.rawBits()) {
                                                                                ObjectHeader* handHdr = handMorph.asObjectPtr();
                                                                                fprintf(driverLog, "[DRIVER] HandMorph has %zu slots\n", handHdr->slotCount());

                                                                                // Examine HandMorph slots to understand structure
                                                                                for (size_t j = 0; j < handHdr->slotCount() && j < 30; j++) {
                                                                                    Oop slot = memory_.fetchPointer(j, handMorph);
                                                                                    std::string slotClass = "nil/int";
                                                                                    size_t slotSlots = 0;
                                                                                    if (slot.isSmallInteger()) {
                                                                                        slotClass = "SmallInt";
                                                                                    } else if (slot.isObject() && slot.rawBits() != nilObj.rawBits()) {
                                                                                        Oop sc = memory_.classOf(slot);
                                                                                        if (sc.isObject()) {
                                                                                            Oop scName = memory_.fetchPointer(6, sc);
                                                                                            if (scName.isObject()) {
                                                                                                ObjectHeader* scnHdr = scName.asObjectPtr();
                                                                                                if (scnHdr->isBytesObject() && scnHdr->byteSize() < 100) {
                                                                                                    slotClass = std::string((char*)scnHdr->bytes(), scnHdr->byteSize());
                                                                                                }
                                                                                            }
                                                                                        }
                                                                                        slotSlots = slot.asObjectPtr()->slotCount();
                                                                                    }
                                                                                    fprintf(driverLog, "[DRIVER] HandMorph slot[%zu]: class=%s slots=%zu\n",
                                                                                            j, slotClass.c_str(), slotSlots);
                                                                                }

                                                                                // Examine the WaitfreeQueue at slot 25
                                                                                Oop eventQueue = memory_.fetchPointer(25, handMorph);
                                                                                fprintf(driverLog, "[DRIVER] HandMorph eventQueue (slot 25): 0x%llx\n",
                                                                                        (unsigned long long)eventQueue.rawBits());
                                                                                if (eventQueue.isObject() && eventQueue.rawBits() != nilObj.rawBits()) {
                                                                                    ObjectHeader* eqHdr = eventQueue.asObjectPtr();
                                                                                    fprintf(driverLog, "[DRIVER] WaitfreeQueue has %zu slots, classIdx=%u\n",
                                                                                            eqHdr->slotCount(), eqHdr->classIndex());

                                                                                    // Examine the AtomicQueueItem structures
                                                                                    for (size_t k = 0; k < eqHdr->slotCount() && k < 5; k++) {
                                                                                        Oop eqs = memory_.fetchPointer(k, eventQueue);
                                                                                        fprintf(driverLog, "[DRIVER]   WaitfreeQueue slot[%zu]: 0x%llx\n",
                                                                                                k, (unsigned long long)eqs.rawBits());
                                                                                        if (eqs.isObject() && eqs.rawBits() != nilObj.rawBits()) {
                                                                                            ObjectHeader* eqsHdr = eqs.asObjectPtr();
                                                                                            fprintf(driverLog, "[DRIVER]     AtomicQueueItem has %zu slots, classIdx=%u\n",
                                                                                                    eqsHdr->slotCount(), eqsHdr->classIndex());
                                                                                            // AtomicQueueItem typically has: value, next
                                                                                            for (size_t m = 0; m < eqsHdr->slotCount() && m < 5; m++) {
                                                                                                Oop aqis = memory_.fetchPointer(m, eqs);
                                                                                                std::string aqisInfo = "";
                                                                                                if (aqis.rawBits() == nilObj.rawBits()) {
                                                                                                    aqisInfo = "nil";
                                                                                                } else if (aqis.isObject()) {
                                                                                                    Oop sc = memory_.classOf(aqis);
                                                                                                    if (sc.isObject()) {
                                                                                                        Oop scName = memory_.fetchPointer(6, sc);
                                                                                                        if (scName.isObject()) {
                                                                                                            ObjectHeader* scnHdr = scName.asObjectPtr();
                                                                                                            if (scnHdr->isBytesObject() && scnHdr->byteSize() < 100) {
                                                                                                                aqisInfo = std::string((char*)scnHdr->bytes(), scnHdr->byteSize());
                                                                                                            }
                                                                                                        }
                                                                                                    }
                                                                                                }
                                                                                                fprintf(driverLog, "[DRIVER]       AQI slot[%zu]: 0x%llx (%s)\n",
                                                                                                        m, (unsigned long long)aqis.rawBits(), aqisInfo.c_str());
                                                                                            }
                                                                                        }
                                                                                    }

                                                                                    // NOTE: Event injection workaround REMOVED - was storing handEventQueue_
                                                                                    // Events should go through proper Smalltalk InputEventSensor process
                                                                                }

                                                                                // NOTE: Event injection workaround REMOVED - was storing eventInjectionHand_
                                                                                fprintf(driverLog, "[DRIVER] HandMorph found at 0x%llx (not storing - workaround removed)\n",
                                                                                        (unsigned long long)handMorph.rawBits());

                                                                                // Examine the existing MouseEvent in slot 12 to understand structure
                                                                                Oop lastMouseEvent = memory_.fetchPointer(12, handMorph);
                                                                                fprintf(driverLog, "[DRIVER] lastMouseEvent (slot 12): 0x%llx\n",
                                                                                        (unsigned long long)lastMouseEvent.rawBits());
                                                                                if (lastMouseEvent.isObject() && lastMouseEvent.rawBits() != nilObj.rawBits()) {
                                                                                    ObjectHeader* meHdr = lastMouseEvent.asObjectPtr();
                                                                                    fprintf(driverLog, "[DRIVER] MouseEvent has %zu slots, format=%d, classIdx=%u\n",
                                                                                            meHdr->slotCount(), meHdr->format(), meHdr->classIndex());

                                                                                    // Dump all slots of the MouseEvent
                                                                                    for (size_t l = 0; l < meHdr->slotCount() && l < 15; l++) {
                                                                                        Oop mes = memory_.fetchPointer(l, lastMouseEvent);
                                                                                        std::string mesInfo = "";
                                                                                        if (mes.isSmallInteger()) {
                                                                                            mesInfo = "SmallInt=" + std::to_string(mes.asSmallInteger());
                                                                                        } else if (mes.rawBits() == nilObj.rawBits()) {
                                                                                            mesInfo = "nil";
                                                                                        } else if (mes.isObject()) {
                                                                                            ObjectHeader* mesHdr = mes.asObjectPtr();
                                                                                            // Check if it's a symbol (ByteSymbol)
                                                                                            if (mesHdr->isBytesObject() && mesHdr->byteSize() < 50) {
                                                                                                mesInfo = "Symbol=#" + std::string((char*)mesHdr->bytes(), mesHdr->byteSize());
                                                                                            } else {
                                                                                                Oop sc = memory_.classOf(mes);
                                                                                                if (sc.isObject()) {
                                                                                                    Oop scName = memory_.fetchPointer(6, sc);
                                                                                                    if (scName.isObject()) {
                                                                                                        ObjectHeader* scnHdr = scName.asObjectPtr();
                                                                                                        if (scnHdr->isBytesObject() && scnHdr->byteSize() < 100) {
                                                                                                            mesInfo = std::string((char*)scnHdr->bytes(), scnHdr->byteSize());
                                                                                                        }
                                                                                                    }
                                                                                                }
                                                                                            }
                                                                                        }
                                                                                        fprintf(driverLog, "[DRIVER]   MouseEvent slot[%zu]: 0x%llx (%s)\n",
                                                                                                l, (unsigned long long)mes.rawBits(), mesInfo.c_str());
                                                                                    }

                                                                                    // NOTE: Event injection workaround REMOVED - was storing existingMouseEvent_ and mouseEventClassIndex_
                                                                                    fprintf(driverLog, "[DRIVER] Found MouseEvent classIdx=%u (not storing - workaround removed)\n",
                                                                                            meHdr->classIndex());
                                                                                }
                                                                            }
                                                                        }
                                                                    }
                                                                } else {
                                                                    fprintf(driverLog, "[DRIVER] Failed to instantiate OSSDL2Driver\n");
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
                        fflush(driverLog);
                    }

                    // NOTE: Input semaphore index should be set by primitive 153/265 when
                    // InputEventSensor registers. Don't use a fallback - if no semaphore is
                    // registered, events won't be signaled but that's better than signaling
                    // the wrong semaphore (like TFCallbackQueue's).
                    if (pharo::gEventQueue.getInputSemaphoreIndex() == 0) {
                        fprintf(driverLog, "[DRIVER] No input semaphore registered yet (index=0)\n");
                        fprintf(driverLog, "[DRIVER] InputEventSensor should register via primitive 153/265\n");
                    } else {
                        fprintf(driverLog, "[DRIVER] Input semaphore index already set to %d\n",
                                pharo::gEventQueue.getInputSemaphoreIndex());
                    }
                }

                fflush(driverLog);
            }
        }
        return;
    }

    if (!osDriverClass.isObject()) {
        if (driverLog) {
            fprintf(driverLog, "[DRIVER] OSiOSDriver class not found\n");
            fflush(driverLog);
        }
        return;
    }

    // Get the metaclass (class of the class) for class-side method lookup
    Oop metaclass = memory_.classOf(osDriverClass);
    if (driverLog) {
        fprintf(driverLog, "[DRIVER] Metaclass: 0x%llx isObj=%d\n",
                (unsigned long long)metaclass.rawBits(), metaclass.isObject() ? 1 : 0);
        fflush(driverLog);
    }

    if (metaclass.isNil() || !metaclass.isObject()) {
        if (driverLog) {
            fprintf(driverLog, "[DRIVER] Metaclass not found\n");
            fflush(driverLog);
        }
        return;
    }

    // Debug: Get metaclass name
    if (driverLog) {
        // Class has name at slot 6
        Oop nameOop = memory_.fetchPointer(6, osDriverClass);
        if (nameOop.isObject()) {
            ObjectHeader* nameHdr = nameOop.asObjectPtr();
            if (nameHdr->isBytesObject() && nameHdr->byteSize() < 100) {
                std::string name((char*)nameHdr->bytes(), nameHdr->byteSize());
                fprintf(driverLog, "[DRIVER] Class name: '%s'\n", name.c_str());
            }
        }
        fflush(driverLog);
    }

    // Look up the "install" method in the metaclass's methodDict
    Oop methodDict = memory_.fetchPointer(1, metaclass);
    if (driverLog) {
        fprintf(driverLog, "[DRIVER] Metaclass methodDict: 0x%llx isObj=%d\n",
                (unsigned long long)methodDict.rawBits(), methodDict.isObject() ? 1 : 0);
        if (methodDict.isObject()) {
            ObjectHeader* mdHdr = methodDict.asObjectPtr();
            fprintf(driverLog, "[DRIVER] MethodDict slots: %zu\n", mdHdr->slotCount());
        }
        fflush(driverLog);
    }
    if (!methodDict.isObject()) {
        if (driverLog) {
            fprintf(driverLog, "[DRIVER] Metaclass has no methodDict\n");
            fflush(driverLog);
        }
        return;
    }

    ObjectHeader* mdHeader = methodDict.asObjectPtr();
    size_t mdSlots = mdHeader->slotCount();
    Oop installMethod = Oop::nil();

    // Debug: dump first 10 selectors
    if (driverLog) {
        fprintf(driverLog, "[DRIVER] Scanning selectors in methodDict (slots 2-%zu):\n", mdSlots);
        int selectorCount = 0;
        for (size_t i = 2; i < mdSlots && selectorCount < 15; i++) {
            Oop key = mdHeader->slotAt(i);
            if (!key.isObject() || key.rawBits() == nilObj.rawBits()) continue;
            ObjectHeader* keyHdr = key.asObjectPtr();
            if (!keyHdr->isBytesObject()) continue;
            size_t keyLen = keyHdr->byteSize();
            if (keyLen > 0 && keyLen < 50) {
                std::string sel((char*)keyHdr->bytes(), keyLen);
                fprintf(driverLog, "[DRIVER]   slot[%zu] = '%s' (len=%zu)\n", i, sel.c_str(), keyLen);
                selectorCount++;
            }
        }
        fflush(driverLog);
    }

    for (size_t i = 2; i < mdSlots; i++) {
        Oop key = mdHeader->slotAt(i);
        if (!key.isObject() || key.rawBits() == nilObj.rawBits()) continue;

        ObjectHeader* keyHdr = key.asObjectPtr();
        if (!keyHdr->isBytesObject()) continue;

        size_t keyLen = keyHdr->byteSize();
        const char* keyBytes = (const char*)keyHdr->bytes();

        if (keyLen == 7 && memcmp(keyBytes, "install", 7) == 0) {
            // Found "install" selector - get the method from values array
            Oop values = memory_.fetchPointer(1, methodDict);
            if (values.isObject()) {
                ObjectHeader* valHdr = values.asObjectPtr();
                size_t valueIdx = i - 2;
                if (valueIdx < valHdr->slotCount()) {
                    installMethod = valHdr->slotAt(valueIdx);
                    if (driverLog) {
                        fprintf(driverLog, "[DRIVER] Found 'install' method at idx %zu\n", valueIdx);
                        fflush(driverLog);
                    }
                }
            }
            break;
        }
    }

    if (installMethod.isNil() || !installMethod.isObject()) {
        if (driverLog) {
            fprintf(driverLog, "[DRIVER] 'install' method not found\n");
            fflush(driverLog);
        }
        return;
    }

    // Create a process to run OSiOSDriver install
    // We can't interrupt the current process, so we schedule it for later
    if (driverLog) {
        fprintf(driverLog, "[DRIVER] Found install method 0x%llx - scheduling execution\n",
                (unsigned long long)installMethod.rawBits());
        fflush(driverLog);
    }

    // Store the method for deferred execution in the run loop
    // We'll execute it when the current process yields
    pendingDriverInstallMethod_ = installMethod;
    pendingDriverInstallReceiver_ = osDriverClass;
    hasPendingDriverInstall_ = true;

    if (driverLog) {
        fprintf(driverLog, "[DRIVER] Scheduled OSiOSDriver install for deferred execution\n");
        fflush(driverLog);
    }

    // Also find setupEventLoop method to call AFTER install completes
    // This is an INSTANCE method in OSSDL2Driver (superclass of OSiOSDriver)
    // We look it up now but will call it on the Current instance after install runs
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

    // Look for setupEventLoop - FIRST in OSiOSDriver (uses primitive 264),
    // then fall back to OSSDL2Driver (uses FFI which may fail)
    Oop setupMethod = Oop::nil();

    // First try OSiOSDriver's hierarchy (priority: uses primitive 264 instead of FFI)
    {
        Oop currentClass = osDriverClass;
        int depth = 0;
        while (currentClass.isObject() && !currentClass.isNil() && depth < 10) {
            std::string className = "<unknown>";
            Oop nameOop = memory_.fetchPointer(6, currentClass);
            if (nameOop.isObject()) {
                ObjectHeader* nameHdr = nameOop.asObjectPtr();
                if (nameHdr->isBytesObject() && nameHdr->byteSize() < 100) {
                    className = std::string((char*)nameHdr->bytes(), nameHdr->byteSize());
                }
            }
            if (driverLog) {
                fprintf(driverLog, "[DRIVER] Looking for setupEventLoop in %s (depth %d)\n",
                        className.c_str(), depth);
                // Dump instance methods at depth 0 to see what's available
                if (depth == 0) {
                    Oop methodDict = memory_.fetchPointer(1, currentClass);
                    if (methodDict.isObject()) {
                        ObjectHeader* mdHdr = methodDict.asObjectPtr();
                        fprintf(driverLog, "[DRIVER] Instance methodDict has %zu slots:\n", mdHdr->slotCount());
                        for (size_t mi = 2; mi < mdHdr->slotCount() && mi < 50; mi++) {
                            Oop key = mdHdr->slotAt(mi);
                            if (key.isObject() && !key.isNil()) {
                                ObjectHeader* keyHdr = key.asObjectPtr();
                                if (keyHdr->isBytesObject() && keyHdr->byteSize() < 50) {
                                    std::string name((char*)keyHdr->bytes(), keyHdr->byteSize());
                                    fprintf(driverLog, "[DRIVER]   [%zu] '%s'\n", mi, name.c_str());
                                }
                            }
                        }
                    }
                }
                fflush(driverLog);
            }

            setupMethod = findMethodInClass(currentClass, "setupEventLoop");
            if (setupMethod.isObject() && !setupMethod.isNil()) {
                if (driverLog) {
                    fprintf(driverLog, "[DRIVER] FOUND setupEventLoop in %s!\n", className.c_str());
                    fflush(driverLog);
                }
                break;
            }

            currentClass = memory_.fetchPointer(0, currentClass);
            depth++;
        }
    }

    // If not found in OSiOSDriver hierarchy, check if we should use OSSDL2Driver fallback
    // NOTE: For OSiOSDriver, the install method calls ensureEventLoop which triggers our
    // DNU fallback that enables native event polling. We DON'T want OSSDL2Driver's
    // FFI-based event loop in that case.
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
        // setupEventLoop not found in OSiOSDriver
        // DO NOT use OSSDL2Driver's FFI-based setupEventLoop - FFI type resolution is broken
        // Instead, enable VM-based event processing which directly injects events
        if (driverLog) {
            fprintf(driverLog, "[DRIVER] No setupEventLoop found - enabling VM native event processing\n");
            fprintf(driverLog, "[DRIVER] Events will be handled via VM's processInputEvents and signalInputSemaphore\n");
            fflush(driverLog);
        }
        // Keep SDL2EventPollingActive=false so processInputEvents handles events
        // Enable direct InputEventSensor signaling mode
        enableDirectInputSignaling_ = true;
    }

    if (setupMethod.isObject() && !setupMethod.isNil()) {
        pendingDriverSetupMethod_ = setupMethod;
        // NOTE: receiver will be set to Current value after install completes
        pendingDriverSetupReceiver_ = Oop::nil();  // Placeholder - will be filled in later
        hasPendingDriverSetup_ = true;
        if (driverLog) {
            fprintf(driverLog, "[DRIVER] Scheduled setupEventLoop for execution after install\n");
            fflush(driverLog);
        }
    } else if (!enableDirectInputSignaling_) {
        // Only warn if we didn't enable native event processing
        if (driverLog) {
            fprintf(driverLog, "[DRIVER] WARNING: setupEventLoop not found - event loop won't start!\n");
            fflush(driverLog);
        }
    }
    // If enableDirectInputSignaling_ is true, VM will handle events natively
}

bool Interpreter::executePendingDriverInstall() {
    if (!hasPendingDriverInstall_) {
        return false;
    }

    hasPendingDriverInstall_ = false;  // Clear flag before executing

    static FILE* driverLog = nullptr;
    if (driverLog) {
        fprintf(driverLog, "[DRIVER] executePendingDriverInstall: Creating context for install\n");
        fflush(driverLog);
    }

    if (pendingDriverInstallMethod_.isNil() || !pendingDriverInstallMethod_.isObject()) {
        if (driverLog) {
            fprintf(driverLog, "[DRIVER] executePendingDriverInstall: Invalid method\n");
            fflush(driverLog);
        }
        return false;
    }

    // Create a context for OSiOSDriver install and execute it
    Oop context;
    if (pendingDriverMethodNeedsArg_) {
        // startUp: needs a boolean argument (resuming = true)
        context = memory_.createStartupContextWithArg(pendingDriverInstallMethod_, pendingDriverInstallReceiver_, memory_.trueObject());
        if (driverLog) {
            fprintf(driverLog, "[DRIVER] executePendingDriverInstall: Created context with arg=true for startUp:\n");
            fflush(driverLog);
        }
    } else {
        context = memory_.createStartupContext(pendingDriverInstallMethod_, pendingDriverInstallReceiver_);
    }
    pendingDriverMethodNeedsArg_ = false;  // Reset flag

    if (context.isNil()) {
        if (driverLog) {
            fprintf(driverLog, "[DRIVER] executePendingDriverInstall: Failed to create context\n");
            fflush(driverLog);
        }
        return false;
    }

    if (driverLog) {
        fprintf(driverLog, "[DRIVER] executePendingDriverInstall: Setting up install method context\n");
        fflush(driverLog);
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

    if (driverLog) {
        fprintf(driverLog, "[DRIVER] executePendingDriverInstall: Context setup %s\n",
                result ? "succeeded - method will run in subsequent steps" : "failed");
        fflush(driverLog);
    }

    // Clear the pending install (method is now set up to run)
    pendingDriverInstallMethod_ = Oop::nil();
    pendingDriverInstallReceiver_ = Oop::nil();

    return result;
}

bool Interpreter::autoLoadDriver() {
    // Auto-load disabled - OSiOSDriver must be loaded from Pharo image side
    // (Pending action dispatch mechanism has been removed)
    return false;
}

bool Interpreter::bootstrapStartup() {
    static int bootstrapCallCount = 0;
    static bool imageBooted = false;  // true once we've ever found a runnable process
    bootstrapCallCount++;
    if (bootstrapCallCount <= 5) {
        std::cerr << "[STARTUP] bootstrapStartup called (call #" << bootstrapCallCount << ")\n";
    }

    // In Spur, nil is an actual object at heap start, not 0
    Oop nilObj = memory_.specialObject(SpecialObjectIndex::NilObject);
    // DEBUG_LOG("[DEBUG] nil object = 0x" << std::hex << nilObj.rawBits() << std::dec;

    // Approach 1: Look for any ready-to-run process in the scheduler's queues
    // DEBUG: "[DEBUG] Checking scheduler process queues for runnable processes..."
    Oop schedulerAssoc = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);
    if (schedulerAssoc.rawBits() != nilObj.rawBits() && schedulerAssoc.isObject()) {
        Oop scheduler = memory_.fetchPointer(1, schedulerAssoc);
        if (scheduler.isObject()) {
            // ProcessScheduler slot 0 = quiescentProcessLists (array of LinkedLists)
            Oop queues = memory_.fetchPointer(0, scheduler);
            if (queues.isObject()) {
                ObjectHeader* queuesHeader = queues.asObjectPtr();
                size_t numQueues = queuesHeader->slotCount();
                std::cerr << "[STARTUP] Found " << numQueues << " priority queues\n";

                // Search from highest to lowest priority for a runnable process
                for (int i = static_cast<int>(numQueues) - 1; i >= 0; i--) {
                    Oop queue = queuesHeader->slotAt(i);
                    if (queue.rawBits() == nilObj.rawBits() || !queue.isObject()) continue;

                    ObjectHeader* queueHeader = queue.asObjectPtr();
                    std::cerr << "[STARTUP] Queue at priority " << (i + 1) << ": cls=" << queueHeader->classIndex()
                              << " slots=" << queueHeader->slotCount() << "\n";

                    // LinkedList layout: slot 0 = firstLink, slot 1 = lastLink
                    Oop firstProcess = memory_.fetchPointer(0, queue);
                    std::cerr << "[STARTUP] firstProcess: 0x" << std::hex << firstProcess.rawBits()
                              << std::dec << " isNil=" << (firstProcess.rawBits() == nilObj.rawBits())
                              << " isObj=" << firstProcess.isObject() << "\n";
                    if (firstProcess.rawBits() == nilObj.rawBits() || !firstProcess.isObject()) continue;

                    ObjectHeader* procHeader = firstProcess.asObjectPtr();
                    // DEBUG_LOG("[DEBUG] Process at priority " << (i + 1) << ": cls=" << procHeader->classIndex()
                              // << " slots=" << procHeader->slotCount();

                    // Dump first 5 slots of process
                    for (size_t j = 0; j < std::min(procHeader->slotCount(), (size_t)5); j++) {
                        Oop slot = procHeader->slotAt(j);
                        // DEBUG_LOG("[DEBUG]   proc slot[" << j << "] = 0x" << std::hex << slot.rawBits() << std::dec;
                        // if (slot.rawBits() == nilObj.rawBits()) std::cerr << " (NIL)";
                        // else if (slot.isSmallInteger()) std::cerr << " (SmallInt: " << slot.asSmallInteger() << ")";
                        // else if (slot.isObject()) {
                        //     ObjectHeader* h = slot.asObjectPtr();
                        //     std::cerr << " (obj: slots=" << h->slotCount() << " cls=" << h->classIndex() << ")";
                        // }
                        // std::cerr; // DEBUG
                    }

                    // Check if this process has a valid context
                    // Modern Pharo Process layout:
                    //   slot 0 = nextLink (for LinkedList)
                    //   slot 1 = suspendedContext
                    //   slot 2 = priority
                    Oop context = memory_.fetchPointer(1, firstProcess);  // suspendedContext is at slot 1
                    std::cerr << "[STARTUP] Process at priority " << (i + 1)
                              << ": suspendedContext=0x" << std::hex << context.rawBits() << std::dec << "\n";
                    if (context.rawBits() != nilObj.rawBits() && context.isObject()) {
                        ObjectHeader* ctxHeader = context.asObjectPtr();
                        std::cerr << "[STARTUP] suspendedContext: cls=" << ctxHeader->classIndex()
                                  << " slots=" << ctxHeader->slotCount()
                                  << " fmt=" << (int)ctxHeader->format() << "\n";

                        // Only try to execute if it looks like a Context (not a Process)
                        // Context format is usually 3 (indexable with fixed), Process format is 1
                        if (ctxHeader->format() == ObjectFormat::IndexableWithFixed) {
                            std::cerr << "[STARTUP] Found valid context, resuming execution!\n";
                            imageBooted = true;
                            return executeFromContext(context);
                        } else {
                            std::cerr << "[STARTUP] suspendedContext doesn't look like a Context (format="
                                      << (int)ctxHeader->format() << ")\n";
                        }
                    }
                }
            }
        }
    }

    // Approach 2: Try to resume from where the image was saved
    // The saved active process might have a context embedded deeper
    std::cerr << "[STARTUP] Approach 2: Checking active process...\n";
    Oop schedulerAssoc2 = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);
    if (schedulerAssoc2.isObject()) {
        Oop scheduler = memory_.fetchPointer(1, schedulerAssoc2);  // Get scheduler
        if (scheduler.isObject()) {
            Oop activeProcess = memory_.fetchPointer(1, scheduler);  // Get activeProcess
            if (activeProcess.isObject()) {
                ObjectHeader* procHeader = activeProcess.asObjectPtr();
                std::cerr << "[STARTUP] Active process: 0x" << std::hex << activeProcess.rawBits()
                          << std::dec << " slots=" << procHeader->slotCount()
                          << " cls=" << procHeader->classIndex() << "\n";

                // Check suspendedContext (slot 1) of active process
                if (procHeader->slotCount() > 1) {
                    Oop suspendedCtx = procHeader->slotAt(1);
                    std::cerr << "[STARTUP] Active process suspendedContext: 0x"
                              << std::hex << suspendedCtx.rawBits() << std::dec;
                    if (suspendedCtx.isNil()) {
                        std::cerr << " (nil)\n";
                    } else if (suspendedCtx.isObject()) {
                        ObjectHeader* ctxHdr = suspendedCtx.asObjectPtr();
                        std::cerr << " cls=" << ctxHdr->classIndex()
                                  << " fmt=" << (int)ctxHdr->format() << "\n";
                        // Try to resume from this context if it looks valid
                        if (ctxHdr->format() == ObjectFormat::IndexableWithFixed) {
                            std::cerr << "[STARTUP] Resuming from active process context!\n";
                            imageBooted = true;
                            return executeFromContext(suspendedCtx);
                        }
                    } else {
                        std::cerr << " (not object)\n";
                    }
                }
            }
        }
    }

    // Once the image has booted, don't retry Approach 3 startup methods.
    // The caller's idle loop handles the "nothing to run" case properly.
    if (imageBooted) {
        return false;
    }

    // Approach 3: Try to find and call a startup method directly
    // Use static tracking to prevent infinite loops
    static int startupAttempt = 0;
    static bool startupSucceeded = false;
    static bool displayInitialized = false;

    startupAttempt++;
    // Log every startup attempt to verify the code is being reached
    if constexpr (ENABLE_DEBUG_LOGGING) {
        fprintf(stderr, "[BOOTSTRAP] Attempt #%d\n", startupAttempt);
        static FILE* startupLog = nullptr;
        if (startupLog) {
            fprintf(startupLog, "[BOOTSTRAP] Attempt #%d\n", startupAttempt);
            fflush(startupLog);
        }
    }

    // Initialize the platform display ONCE with a test pattern
    // Skip Smalltalk Form creation - just write directly to platform buffer
    if (!displayInitialized && pharo::gDisplaySurface) {
        displayInitialized = true;

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
                        // Allocate form with 5 slots
                        Oop formObj = memory_.allocateSlots(formClassIdx, 5);

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
    if (startupAttempt > 100) {
        return false;
    }

    // DEBUG: "[DEBUG] bootstrapStartup: Trying to find startup globals..."

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
                        // DEBUG_LOG("[DEBUG] lookupMethodInClass: Found " << selectorName
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
    if (startupAttempt == 1) {
        // SmalltalkImage is the class; we need to find "Smalltalk" which is the instance
        Oop smalltalk = memory_.findGlobal("Smalltalk");
        Oop smalltalkImageClass = memory_.findGlobal("SmalltalkImage");

        // Debug logging
        static FILE* startupDebugLog = nullptr;
        if constexpr (ENABLE_DEBUG_LOGGING) {
            if (!startupDebugLog) startupDebugLog = nullptr;
        }
        Oop nilObj = memory_.specialObject(SpecialObjectIndex::NilObject);
        if (startupDebugLog) {
            fprintf(startupDebugLog, "[STARTUP-1] nil object: 0x%llx\n", (unsigned long long)nilObj.rawBits());
            fprintf(startupDebugLog, "[STARTUP-1] Smalltalk global: 0x%llx isNil=%d isObj=%d\n",
                    (unsigned long long)smalltalk.rawBits(), smalltalk.isNil() ? 1 : 0, smalltalk.isObject() ? 1 : 0);
            fprintf(startupDebugLog, "[STARTUP-1] SmalltalkImage class: 0x%llx isNil=%d isObj=%d\n",
                    (unsigned long long)smalltalkImageClass.rawBits(), smalltalkImageClass.isNil() ? 1 : 0, smalltalkImageClass.isObject() ? 1 : 0);

            // Check SessionManager - why is it nil in our VM but not in others?
            Oop sessionMgrClass = memory_.findGlobal("SessionManager");
            fprintf(startupDebugLog, "[STARTUP-1] SessionManager class: 0x%llx isNil=%d isObj=%d\n",
                    (unsigned long long)sessionMgrClass.rawBits(), sessionMgrClass.isNil() ? 1 : 0, sessionMgrClass.isObject() ? 1 : 0);

            // SessionManager class has a 'default' class variable that holds the singleton
            // In Pharo, class instance variables are stored after the class's own slots
            // The metaclass (SessionManager class) has the 'default' inst var
            if (sessionMgrClass.isObject()) {
                ObjectHeader* smHdr = sessionMgrClass.asObjectPtr();
                fprintf(startupDebugLog, "[STARTUP-1] SessionManager classIdx=%u slots=%zu\n",
                        smHdr->classIndex(), smHdr->slotCount());

                // Get the metaclass (class of SessionManager)
                Oop metaclass = memory_.classOf(sessionMgrClass);
                fprintf(startupDebugLog, "[STARTUP-1] SessionManager's metaclass: 0x%llx\n",
                        (unsigned long long)metaclass.rawBits());

                // The 'default' class instance variable is stored in the class object itself
                // Class layout: superclass, methodDict, format, layout, instanceVariables, organization,
                //               subclasses, name, environment, classVars, pool, <class inst vars start here>
                // Class inst vars are after slot 11 in modern Pharo
                fprintf(startupDebugLog, "[STARTUP-1] SessionManager slots (looking for 'default'):\n");
                for (size_t i = 0; i < std::min(smHdr->slotCount(), (size_t)15); i++) {
                    Oop slot = memory_.fetchPointer(i, sessionMgrClass);
                    const char* slotName = "";
                    if (i == 0) slotName = " (superclass)";
                    else if (i == 1) slotName = " (methodDict)";
                    else if (i == 6) slotName = " (name)";
                    else if (i >= 12) slotName = " <-- class inst var?";
                    fprintf(startupDebugLog, "[STARTUP-1]   slot[%zu] = 0x%llx%s%s\n",
                            i, (unsigned long long)slot.rawBits(),
                            slot.rawBits() == nilObj.rawBits() ? " (NIL)" : "",
                            slotName);
                }
            }
            fflush(startupDebugLog);
        }

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
            if (startupDebugLog) {
                fprintf(startupDebugLog, "[STARTUP-1] recordStartupStamp method: 0x%llx isNil=%d\n",
                        (unsigned long long)method.rawBits(), method.isNil() ? 1 : 0);
                fflush(startupDebugLog);
            }
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
    if (startupAttempt == 2) {
        // Find Smalltalk instance and SmalltalkImage class
        Oop smalltalk = memory_.findGlobal("Smalltalk");
        Oop smalltalkImageClass = memory_.findGlobal("SmalltalkImage");

        static FILE* startupDebugLog = nullptr;
        if constexpr (ENABLE_DEBUG_LOGGING) {
            if (!startupDebugLog) startupDebugLog = nullptr;
            if (startupDebugLog) {
                fprintf(startupDebugLog, "[STARTUP-2] Looking for restartMethods\n");
                fflush(startupDebugLog);
            }
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
                        // DEBUG: "[DEBUG] Started restartMethods execution"
                        return true;
                    }
                }
            } else {
                // DEBUG: "[DEBUG] Method restartMethods not found in SmalltalkImage"
            }
        }
    }

    // Attempt 3: Ensure UIManager is initialized (spawns UI process)
    // This is needed because UIManager may not be registered as a startup handler
    // in some Pharo images, preventing the Morphic event loop from starting.
    if (startupAttempt == 3) {
        static bool uiManagerStarted = false;
        static FILE* uiLog = nullptr;
        if constexpr (ENABLE_DEBUG_LOGGING) {
            if (!uiLog) uiLog = nullptr;
        }

        if (!uiManagerStarted) {
            uiManagerStarted = true;

            if (uiLog) {
                fprintf(uiLog, "[UI] Attempting to initialize UIManager\n");
                fflush(uiLog);
            }

            // Find UIManager class
            Oop uiManagerClass = memory_.findGlobal("UIManager");
            Oop nilObj = memory_.specialObject(SpecialObjectIndex::NilObject);

            if (uiLog) {
                fprintf(uiLog, "[UI] UIManager class: 0x%llx sameAsNil=%d\n",
                        (unsigned long long)uiManagerClass.rawBits(),
                        uiManagerClass.rawBits() == nilObj.rawBits() ? 1 : 0);
                fflush(uiLog);
            }

            if (uiManagerClass.isObject() && uiManagerClass.rawBits() != nilObj.rawBits()) {
                // Get the metaclass (UIManager class) for class-side method lookup
                Oop metaclass = memory_.classOf(uiManagerClass);

                if (uiLog) {
                    fprintf(uiLog, "[UI] UIManager metaclass: 0x%llx\n",
                            (unsigned long long)metaclass.rawBits());
                    fflush(uiLog);
                }

                // Look up startUp: method on the class side
                Oop method = lookupMethodInClass(metaclass, "startUp:");

                if (uiLog) {
                    fprintf(uiLog, "[UI] UIManager class>>startUp: method: 0x%llx isNil=%d\n",
                            (unsigned long long)method.rawBits(), method.isNil() ? 1 : 0);
                    fflush(uiLog);
                }

                if (!method.isNil() && method.isObject()) {
                    // Create context for UIManager class>>startUp: true
                    // The receiver is UIManager class, argument is true
                    Oop context = memory_.createStartupContextWithArg(method, uiManagerClass, memory_.trueObject());

                    if (!context.isNil()) {
                        stackPointer_ = stackBase_;
                        frameDepth_ = 0;
                        if (uiLog) {
                            fprintf(uiLog, "[UI] Executing UIManager class>>startUp: true\n");
                            fflush(uiLog);
                        }
                        if (executeFromContext(context)) {
                            if (uiLog) {
                                fprintf(uiLog, "[UI] UIManager startUp: returned successfully\n");
                                fflush(uiLog);
                            }
                            return true;
                        }
                    } else if (uiLog) {
                        fprintf(uiLog, "[UI] Failed to create context for UIManager startUp:\n");
                        fflush(uiLog);
                    }
                } else if (uiLog) {
                    fprintf(uiLog, "[UI] startUp: method not found on UIManager class\n");
                    fflush(uiLog);
                }
            }
        }
    }

    // Fourth try and beyond: Keep calling World>>doOneCycle for UI loop
    if (startupAttempt >= 4 && startupAttempt <= 100) {
        // One-time: Try to start InputEventSensor's event loop
        static bool sensorStartAttempted = false;
        static FILE* sensorLog = nullptr;
        if constexpr (ENABLE_DEBUG_LOGGING) {
            if (!sensorLog) {
                sensorLog = nullptr;
            }
        }
        if (!sensorStartAttempted) {
            sensorStartAttempted = true;

            if (sensorLog) {
                fprintf(sensorLog, "[SENSOR] Starting InputEventSensor lookup (attempt #%d)\n", startupAttempt);
                fflush(sensorLog);
            }

            // Find the Sensor global (InputEventSensor instance)
            Oop sensor = memory_.findGlobal("Sensor");
            if (sensorLog) {
                fprintf(sensorLog, "[SENSOR] Sensor global: 0x%llx isNil=%d isObj=%d\n",
                        (unsigned long long)sensor.rawBits(), sensor.isNil() ? 1 : 0, sensor.isObject() ? 1 : 0);
                fflush(sensorLog);
            }
            if (!sensor.isNil() && sensor.isObject()) {
                Oop sensorClass = memory_.classOf(sensor);
                if (sensorLog) {
                    fprintf(sensorLog, "[SENSOR] Sensor class: 0x%llx isNil=%d isObj=%d\n",
                            (unsigned long long)sensorClass.rawBits(), sensorClass.isNil() ? 1 : 0, sensorClass.isObject() ? 1 : 0);
                    fflush(sensorLog);
                }
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
                        if (sensorLog) {
                            fprintf(sensorLog, "[SENSOR] Trying '%s': 0x%llx isNil=%d\n",
                                    methodNames[i], (unsigned long long)startUpMethod.rawBits(), startUpMethod.isNil() ? 1 : 0);
                            fflush(sensorLog);
                        }
                        if (!startUpMethod.isNil() && startUpMethod.isObject()) {
                            foundMethodName = methodNames[i];
                            break;
                        }
                    }
                    if (!startUpMethod.isNil() && startUpMethod.isObject()) {
                        if (sensorLog) {
                            fprintf(sensorLog, "[SENSOR] Found method: %s\n", foundMethodName);
                            fflush(sensorLog);
                        }
                        if (sensorLog) {
                            fprintf(sensorLog, "[SENSOR] Attempting to call Sensor>>startUp\n");
                            fflush(sensorLog);
                        }
                        Oop context = memory_.createStartupContext(startUpMethod, sensor);
                        if (!context.isNil()) {
                            // Execute startUp in the current context
                            // Push it onto the context stack for execution
                            stackPointer_ = stackBase_;
                            frameDepth_ = 0;
                            if (executeFromContext(context)) {
                                if (sensorLog) {
                                    fprintf(sensorLog, "[SENSOR] Started Sensor>>startUp execution\n");
                                    fflush(sensorLog);
                                }
                                return true;  // Let startUp complete before doing doOneCycle
                            }
                        }
                    } else {
                        if (sensorLog) {
                            fprintf(sensorLog, "[SENSOR] No event loop method found in Sensor class\n");
                            fflush(sensorLog);
                        }
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

    // Fourth try: Try Object >> yourself just to prove basic execution works
    if (startupAttempt == 4) {
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

    // If we get here, we've exhausted our startup options
    // Note: This is normal for headless images - the startup methods executed
    // successfully in earlier attempts, but the Smalltalk code returned because
    // there's no GUI event loop to run.
    // DEBUG_LOG("[DEBUG] bootstrapStartup: No more startup methods to try (attempt #"
              // << startupAttempt << ")";
    // DEBUG: "[DEBUG] This is normal for headless images - startup code ran and returned."
    return false;
}

Oop Interpreter::findSelector(const char* name) {
    // Find a selector symbol by searching through method dictionaries
    // Modern Pharo MethodDictionary stores keys INLINE at slot 2+
    // DEBUG: "[DEBUG] findSelector: Looking for '" << name << "'"

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
        // DEBUG_LOG("[DEBUG] findSelector: SmalltalkImage = 0x" << std::hex << smalltalkImage.rawBits()
                  // << " classIdx=" << std::dec << siHdr->classIndex()
                  // << " slots=" << siHdr->slotCount();

        // SmalltalkImage is a class, so search its metaclass (class of the class)
        Oop metaclass = memory_.classOf(smalltalkImage);
        // DEBUG_LOG("[DEBUG] findSelector: SmalltalkImage metaclass = 0x" << std::hex
                  // << metaclass.rawBits() << std::dec;

        // If classOf returns nil, try directly accessing the classIndex
        if (metaclass.isNil() || metaclass.rawBits() == 0) {
            // DEBUG: "[DEBUG] findSelector: classOf returned nil, trying direct class table access..."
            metaclass = memory_.classAtIndex(siHdr->classIndex());
            // DEBUG_LOG("[DEBUG] findSelector: Direct classAtIndex(" << siHdr->classIndex()
                      // << ") = 0x" << std::hex << metaclass.rawBits() << std::dec;

            // Still nil? Try searching the method dictionary of SmalltalkImage directly
            // SmalltalkImage is a class, slot 1 = methodDict for instance methods
            // For class methods, we'd need the metaclass, but since that's not available,
            // let's search the class's own method dictionary for selectors
            if (metaclass.isNil() || metaclass.rawBits() == 0) {
                // DEBUG: "[DEBUG] findSelector: Trying SmalltalkImage's own methodDict..."
                Oop methodDict = memory_.fetchPointer(1, smalltalkImage);
                if (methodDict.isObject()) {
                    ObjectHeader* mdHeader = methodDict.asObjectPtr();
                    // DEBUG_LOG("[DEBUG] findSelector: SmalltalkImage methodDict has "
                              // << mdHeader->slotCount() << " slots, cls="
                              // << mdHeader->classIndex() << " fmt=" << (int)mdHeader->format();

                    // Debug: list ALL selectors looking for startup-related ones
                    static bool selectorsDumped = false;
                    if (!selectorsDumped) {
                        selectorsDumped = true;
                        // DEBUG: "[DEBUG] ALL selectors in SmalltalkImage methodDict:"
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
                                        // DEBUG: "[DEBUG]   slot[" << i << "]: '" << keyStr << "'"
                                    }
                                }
                            }
                        }
                    }

                    // Search for selector in method dict (keys at slot 2+)
                    // DEBUG_LOG("[DEBUG] findSelector: Searching for '" << name << "' in "
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
                                    // DEBUG_LOG("[DEBUG] findSelector: Found '" << name
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
                // DEBUG_LOG("[DEBUG] findSelector: depth=" << depth << " methodDict=0x" << std::hex
                          // << methodDict.rawBits() << std::dec;
                if (methodDict.isObject()) {
                    ObjectHeader* mdHeader = methodDict.asObjectPtr();
                    size_t mdSlots = mdHeader->slotCount();
                    // DEBUG_LOG("[DEBUG] findSelector: methodDict has " << mdSlots << " slots, cls="
                              // << mdHeader->classIndex();

                    // Debug: show first few selectors found
                    static bool debugPrinted = false;
                    if (!debugPrinted && depth == 0) {
                        debugPrinted = true;
                        // DEBUG: "[DEBUG] findSelector: First 10 selectors in SmalltalkImage metaclass MD:"
                        int count = 0;
                        for (size_t i = 2; i < mdSlots && count < 10; i++) {
                            Oop key = mdHeader->slotAt(i);
                            if (key.isObject() && !key.isNil()) {
                                ObjectHeader* keyHdr = key.asObjectPtr();
                                if (keyHdr->isBytesObject() && keyHdr->byteSize() <= 50) {
                                    std::string keyStr((char*)keyHdr->bytes(), keyHdr->byteSize());
                                    // DEBUG: "[DEBUG]   slot[" << i << "]: '" << keyStr << "'"
                                    count++;
                                }
                            }
                        }
                    }

                    // Keys are stored inline from slot 2 onwards
                    for (size_t i = 2; i < mdSlots; i++) {
                        Oop key = mdHeader->slotAt(i);
                        if (key.isObject() && !key.isNil()) {
                            if (memory_.symbolEquals(key, name)) {
                                // DEBUG: "[DEBUG] findSelector: Found '" << name << "' in SmalltalkImage metaclass!"
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
                            // DEBUG: "[DEBUG] findSelector: Found '" << name << "' in SmalltalkImage class!"
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
                            // DEBUG: "[DEBUG] findSelector: Found '" << name << "' in class " << ci << "!"
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

    // DEBUG: "[DEBUG] findSelector: '" << name << "' not found"
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

    // Fix unrelocated context pointer if needed
    {
        const uint64_t OLD_IMAGE_BASE = 0x10000000000ULL;
        uint64_t ctxAddr = context.rawBits() & ~7ULL;
        if (ctxAddr >= OLD_IMAGE_BASE && ctxAddr < OLD_IMAGE_BASE * 2) {
            uint64_t offset = ctxAddr - OLD_IMAGE_BASE;
            uint64_t newAddr = reinterpret_cast<uint64_t>(memory_.oldSpaceStart()) + offset;
            context = memory_.oopFromPointer(reinterpret_cast<ObjectHeader*>(newAddr));
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

    // Check for and fix unrelocated pointers (old image base 0x10000000000+)
    // The old Spur 64-bit image base is 0x10000000000 (1TB)
    const uint64_t OLD_IMAGE_BASE = 0x10000000000ULL;
    uint64_t newBase = reinterpret_cast<uint64_t>(memory_.oldSpaceStart());

    uint64_t methodAddr = method_.rawBits() & ~7ULL;
    uint64_t receiverAddr = receiver_.rawBits() & ~7ULL;

    bool methodUnrelocated = (methodAddr >= OLD_IMAGE_BASE && methodAddr < OLD_IMAGE_BASE * 2);
    bool receiverUnrelocated = (receiverAddr >= OLD_IMAGE_BASE && receiverAddr < OLD_IMAGE_BASE * 2);

    if (methodUnrelocated || receiverUnrelocated) {
        // Fix method pointer if needed
        if (methodUnrelocated) {
            uint64_t offset = methodAddr - OLD_IMAGE_BASE;
            uint64_t newAddr = newBase + offset;
            ObjectHeader* newMethodPtr = reinterpret_cast<ObjectHeader*>(newAddr);
            method_ = memory_.oopFromPointer(newMethodPtr);

            // Also fix the context slot
            ObjectHeader* ctxHdr = context.asObjectPtr();
            ctxHdr->slotAtPut(3, method_);
        }

        // Fix receiver pointer if needed
        if (receiverUnrelocated) {
            uint64_t offset = receiverAddr - OLD_IMAGE_BASE;
            uint64_t newAddr = newBase + offset;
            ObjectHeader* newRcvrPtr = reinterpret_cast<ObjectHeader*>(newAddr);
            receiver_ = memory_.oopFromPointer(newRcvrPtr);

            // Also fix the context slot
            ObjectHeader* ctxHdr = context.asObjectPtr();
            ctxHdr->slotAtPut(5, receiver_);
        }
    }

    // Fix sender slot if unrelocated
    Oop sender = memory_.fetchPointer(0, context);
    uint64_t senderAddr = sender.rawBits() & ~7ULL;
    if (senderAddr >= OLD_IMAGE_BASE && senderAddr < OLD_IMAGE_BASE * 2) {
        uint64_t offset = senderAddr - OLD_IMAGE_BASE;
        uint64_t newAddr = newBase + offset;
        ObjectHeader* newSenderPtr = reinterpret_cast<ObjectHeader*>(newAddr);
        sender = memory_.oopFromPointer(newSenderPtr);
        ObjectHeader* ctxHdr = context.asObjectPtr();
        ctxHdr->slotAtPut(0, sender);
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

        if (methodClsIdx == 3101) {
            // CompiledMethod - this is the home method
            homeMethod_ = method_;
        } else if (methodClsIdx == 3117) {
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
                    if (slot2Hdr->classIndex() == 3101) {
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
                    if (candidateCls == 3101) {
                        homeMethod_ = homeCandidate;
                        break;
                    } else if (candidateCls == 3117) {
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

                        if (outerMethodCls == 3101) {
                            // Found home CompiledMethod
                            homeMethod_ = outerMethod;
                            // DEBUG: "[DEBUG] executeFromContext: Found homeMethod via closure chain"
                            break;
                        } else if (outerMethodCls == 3117) {
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

    // DEBUG_LOG("[DEBUG] executeFromContext: context=0x" << std::hex << context.rawBits()
              // << " method=0x" << method_.rawBits()
              // << " receiver=0x" << receiver_.rawBits() << std::dec;

    // If method is a CompiledBlock, we need to check if the context has a closure
    // In modern Pharo, BlockContext/FullBlockClosure contexts may need special handling
    if (method_.isObject() && memory_.isValidPointer(method_)) {
        ObjectHeader* methodHdr = method_.asObjectPtr();
        if (methodHdr->classIndex() == 3117) {
            Oop closure = memory_.fetchPointer(4, context);  // closureOrNil
            if (closure.isObject() && memory_.isValidPointer(closure)) {
                ObjectHeader* closureHdr = closure.asObjectPtr();
                // DEBUG_LOG("[DEBUG] Closure at slot 4: cls=" << closureHdr->classIndex()
                          // << " slots=" << closureHdr->slotCount();
            }
        }
    }

    if (method_.isNil() || !method_.isObject() || !memory_.isValidPointer(method_)) {
        return false;
    }

    // Get method header to calculate bytecode start
    ObjectHeader* methodObj = method_.asObjectPtr();
    // DEBUG_LOG("[DEBUG] executeFromContext: Method has " << methodObj->slotCount() << " slots, cls=" << methodObj->classIndex() << ", fmt=" << (int)methodObj->format();

    // Check if method has a primitive or if we're in snapshotPrimitive method
    int primIdx = primitiveIndexOf(method_);
    bool isSnapshotResume = false;

    // Track if this is our first snapshot resume (for logging)
    static bool firstSnapshotResume = true;

    // Log snapshot resume detection
    static FILE* snapLog = nullptr;
    if constexpr (ENABLE_DEBUG_LOGGING) {
        if (!snapLog) snapLog = nullptr;
        if (snapLog && firstSnapshotResume) {
            fprintf(snapLog, "[SNAP] Checking for snapshot resume, primIdx=%d\n", primIdx);
            fflush(snapLog);
        }
    }

    // Check for snapshot primitive (131) by primitive number
    if (primIdx == 131) {
        isSnapshotResume = true;
        if (snapLog) {
            fprintf(snapLog, "[SNAP] Detected by primitive 131\n");
            fflush(snapLog);
        }
    }

    // Also check by method selector name (for methods that call primitives differently)
    if (!isSnapshotResume && receiver_.isObject()) {
        // Get receiver's class name
        Oop rcvrClass = memory_.classOf(receiver_);
        if (rcvrClass.isObject()) {
            ObjectHeader* clsHdr = rcvrClass.asObjectPtr();
            if (clsHdr->slotCount() > 6) {
                Oop nameOop = memory_.fetchPointer(6, rcvrClass);
                if (nameOop.isObject() && nameOop.rawBits() > 0x10000) {
                    ObjectHeader* nameHdr = nameOop.asObjectPtr();
                    if (nameHdr->isBytesObject() && nameHdr->byteSize() <= 100) {
                        std::string className((char*)nameHdr->bytes(), nameHdr->byteSize());
                        if (snapLog && firstSnapshotResume) {
                            fprintf(snapLog, "[SNAP] Receiver class name: '%s'\n", className.c_str());
                            fflush(snapLog);
                        }
                        if (className == "SnapshotOperation" || className == "SessionManager") {
                            // We're resuming in snapshot-related context
                            // SnapshotOperation is used in Pharo <= 12
                            // SessionManager is used in Pharo 13+
                            isSnapshotResume = true;
                            if (snapLog) {
                                fprintf(snapLog, "[SNAP] Detected snapshot class: %s!\n", className.c_str());
                                fflush(snapLog);
                            }
                        }
                    }
                }
            }
        }
    }

    // Mark that we've detected snapshot resume (used to suppress further logging)
    if (isSnapshotResume) {
        firstSnapshotResume = false;
        if (snapLog) {
            fprintf(snapLog, "[SNAP] Will set TOS to TRUE for resume\n");
            fflush(snapLog);
        }
    } else if (firstSnapshotResume && snapLog) {
        fprintf(snapLog, "[SNAP] Not a snapshot resume\n");
        fflush(snapLog);
        firstSnapshotResume = false;  // Only log once
    }

    Oop methodHeader = memory_.fetchPointer(0, method_);
    // DEBUG_LOG("[DEBUG] executeFromContext: Method slot 0 = 0x" << std::hex << methodHeader.rawBits() << std::dec;
    // if (methodHeader.isSmallInteger()) std::cerr << " (SmallInt: " << methodHeader.asSmallInteger() << ")";
    // else if (methodHeader.isObject()) {
    //     ObjectHeader* h = methodHeader.asObjectPtr();
    //     std::cerr << " (obj: " << h->slotCount() << " slots, cls=" << h->classIndex() << ")";
    // }
    // std::cerr; // DEBUG

    if (!methodHeader.isSmallInteger()) {
        // ERROR_LOG("[ERROR] executeFromContext: Invalid method header (not SmallInteger)";
        return false;
    }

    int64_t headerBits = methodHeader.asSmallInteger();
    int numLiterals = headerBits & 0x7FFF;  // bits 0-14 are numLiterals
    int numTemps = (headerBits >> 18) & 0x3F;

    // Detect bytecode set: sign bit (bit 63) = 0 for V3PlusClosures, 1 for SistaV1
    // In 64-bit Spur, negative header means alternate bytecode set (SistaV1)
    usesSistaV1_ = headerBits < 0;

    uint8_t* methodBytes = methodObj->bytes();
    size_t bytecodeStart = (1 + numLiterals) * 8;

    // Calculate bytecode end - CompiledMethod format encodes unused bytes
    size_t totalBytes = methodObj->byteSize();
    bytecodeEnd_ = methodBytes + totalBytes;

    // Get the saved PC from the context
    // In Pharo, PC is 1-based byte offset from start of method bytes (absolute, includes header+literals)
    // Initial PC = (1 + numLiterals) * 8 + 1 = bytecodeStart + 1
    Oop savedPC = memory_.fetchPointer(1, context);

    // Log context restoration details
    static FILE* ctxRestoreLog = nullptr;
    static int ctxRestoreCount = 0;
    if constexpr (ENABLE_DEBUG_LOGGING) {
        if (!ctxRestoreLog) ctxRestoreLog = nullptr;
    }
    ctxRestoreCount++;
    if (ctxRestoreLog && ctxRestoreCount <= 50) {
        // Get active process
        Oop activeProc = getActiveProcess();
        Oop prioOop = memory_.fetchPointer(2, activeProc);
        int prio = prioOop.isSmallInteger() ? static_cast<int>(prioOop.asSmallInteger()) : -1;

        // Get method selector
        std::string selName = "?";
        if (numLiterals >= 2 && numLiterals < 100) {
            Oop sel = memory_.fetchPointer(numLiterals - 1, method_);
            if (sel.isObject() && sel.rawBits() > 0x10000) {
                ObjectHeader* selH = sel.asObjectPtr();
                if (selH->isBytesObject() && selH->byteSize() < 50) {
                    selName = std::string((char*)selH->bytes(), selH->byteSize());
                }
            }
        }
        // Get sender context
        Oop sender = memory_.fetchPointer(0, context);
        // Get receiver class name
        std::string rcvrClsName = "?";
        if (receiver_.isObject() && receiver_.rawBits() > 0x10000) {
            Oop rcvrCls = memory_.classOf(receiver_);
            if (rcvrCls.isObject()) {
                Oop clsName = memory_.fetchPointer(6, rcvrCls);
                if (clsName.isObject() && clsName.rawBits() > 0x10000) {
                    ObjectHeader* cnH = clsName.asObjectPtr();
                    if (cnH->isBytesObject() && cnH->byteSize() < 50) {
                        rcvrClsName = std::string((char*)cnH->bytes(), cnH->byteSize());
                    }
                }
            }
        }
        fprintf(ctxRestoreLog, "[RESTORE #%d] proc=0x%llx(p%d) context=0x%llx method=#%s rcvr=%s sender=0x%llx pc=%lld bcStart=%zu\n",
                ctxRestoreCount, (unsigned long long)activeProc.rawBits(), prio,
                (unsigned long long)context.rawBits(), selName.c_str(), rcvrClsName.c_str(),
                (unsigned long long)sender.rawBits(),
                savedPC.isSmallInteger() ? savedPC.asSmallInteger() : -1,
                bytecodeStart);

        // Dump first few bytecodes at PC position for debugging
        int64_t pcVal = savedPC.isSmallInteger() ? savedPC.asSmallInteger() : 0;
        if (pcVal > 0 && pcVal <= static_cast<int64_t>(totalBytes)) {
            size_t startOff = static_cast<size_t>(pcVal - 1);
            fprintf(ctxRestoreLog, "  bytecodes at PC %lld:", pcVal);
            for (size_t j = startOff; j < std::min(startOff + 20, totalBytes); j++) {
                fprintf(ctxRestoreLog, " %02x", methodBytes[j]);
            }
            fprintf(ctxRestoreLog, "\n");
        }

        // Show the method object's class to distinguish CompiledMethod vs CompiledBlock
        Oop methodClass = memory_.classOf(method_);
        std::string methodClassName = "?";
        if (methodClass.isObject()) {
            Oop mcn = memory_.fetchPointer(6, methodClass);
            if (mcn.isObject() && mcn.rawBits() > 0x10000) {
                ObjectHeader* mcnH = mcn.asObjectPtr();
                if (mcnH->isBytesObject() && mcnH->byteSize() < 50) {
                    methodClassName = std::string((char*)mcnH->bytes(), mcnH->byteSize());
                }
            }
        }
        fprintf(ctxRestoreLog, "  method class: %s, numLiterals=%d\n", methodClassName.c_str(), numLiterals);

        fflush(ctxRestoreLog);
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
                if (usesSistaV1_ && instructionPointer_[0] == 0xF8) {
                    instructionPointer_ += 3;
                }
            }
        } else {
            instructionPointer_ = methodBytes + bytecodeStart;
            // Skip past callPrimitive if at start
            if (usesSistaV1_ && instructionPointer_[0] == 0xF8) {
                instructionPointer_ += 3;
            }
        }
    } else {
        instructionPointer_ = methodBytes + bytecodeStart;
        // Skip past callPrimitive if at start
        if (usesSistaV1_ && instructionPointer_[0] == 0xF8) {
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
    // DEBUG: "[DEBUG] executeFromContext: savedStackp=" << stackp << " numTemps=" << numTemps

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

    // If resuming from snapshot, we need to configure the return value correctly.
    // Pharo SnapshotOperation checks:
    //   result not ifTrue: [ self quitPrimitive ]
    //
    // So if result = true:  true not = false -> ifTrue skips -> continues to startup
    //    if result = false: false not = true -> ifTrue runs -> QUIT!
    //
    // We want to SKIP quit, so set TOS to TRUE.
    //
    // Also clear SnapshotOperation's 'isQuit' slot to prevent quit:
    //   slot 0: save (Boolean)
    //   slot 2: isQuit (Boolean) - if true, quitPrimitive is called after resume
    if (isSnapshotResume) {
        static FILE* snapStackLog = nullptr;
        if constexpr (ENABLE_DEBUG_LOGGING) {
            if (!snapStackLog) snapStackLog = nullptr;
        }
        if (snapStackLog) {
            // Log the context's stack/temp area
            fprintf(snapStackLog, "[SNAP-STACK] stackp=%d Context slots 6+:\n", stackp);
            ObjectHeader* ctxHdr = context.asObjectPtr();
            for (int i = 0; i < std::min(stackp + 2, 10); i++) {
                Oop item = ctxHdr->slotAt(ContextFixedFields + i);
                bool isTrueObj = (item.rawBits() == memory_.trueObject().rawBits());
                bool isFalseObj = (item.rawBits() == memory_.falseObject().rawBits());
                fprintf(snapStackLog, "  slot[%d]: 0x%llx %s%s\n", 6 + i,
                        (unsigned long long)item.rawBits(),
                        isTrueObj ? "(TRUE)" : "",
                        isFalseObj ? "(FALSE)" : "");
            }
            fflush(snapStackLog);
        }

        // Set TOS to true ONLY for the actual snapshot primitive (131) context.
        // This allows the image to proceed with startup instead of quitting.
        // Do NOT set TOS for other snapshot-related contexts (like SessionManager methods)
        // as that would corrupt return values.
        if (primIdx == 131) {
            if (stackPointer_ > stackBase_) {
                *(stackPointer_ - 1) = memory_.trueObject();
            } else {
                push(memory_.trueObject());
            }
        }

        // For SessionManager (Pharo 13+), the 'quit' parameter might be stored
        // differently than SnapshotOperation. Try to find and clear it.
        // In snapshot:andQuit:, the second argument (quit) might be at slot 7
        // (since slot 6 = first arg/temp 'save', slot 7 = second arg 'quit')
        ObjectHeader* ctxHdrForQuit = context.asObjectPtr();
        if (ctxHdrForQuit->slotCount() > 7) {
            Oop quitSlot = ctxHdrForQuit->slotAt(7);  // 'quit' parameter
            if (quitSlot.rawBits() == memory_.trueObject().rawBits()) {
                ctxHdrForQuit->slotAtPut(7, memory_.falseObject());
                if (snapStackLog) {
                    fprintf(snapStackLog, "[SNAP-STACK] Cleared quit parameter at slot 7\n");
                    fflush(snapStackLog);
                }
            }
        }

        // Also clear isQuit slot in SnapshotOperation (for older Pharo) to be safe
        if (receiver_.isObject()) {
            ObjectHeader* rcvr = receiver_.asObjectPtr();
            if (rcvr->slotCount() >= 3) {
                Oop slot2 = rcvr->slotAt(2);
                if (slot2 == memory_.trueObject()) {
                    rcvr->slotAtPut(2, memory_.falseObject());
                }
            }
        }
    }

    // Only initialize selectors once (not on every executeFromContext call)
    static bool selectorsInitialized = false;
    if (!selectorsInitialized) {
        initializeSelectors();
        selectorsInitialized = true;
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

    // Display primitives (called with empty module name)
    registerNamedPrimitive("", "primitiveForceDisplayUpdate", &Interpreter::primitiveForceDisplayUpdate);
    registerNamedPrimitive("", "primitiveShowDisplayRect", &Interpreter::primitiveShowDisplayRect);

    // BitBlt plugin
    registerNamedPrimitive("BitBltPlugin", "primitiveCopyBits", &Interpreter::primitiveCopyBits);
    registerNamedPrimitive("BitBltPlugin", "primitiveDrawLoop", &Interpreter::primitiveDrawLoop);

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

    // FileAttributesPlugin
    registerNamedPrimitive("FileAttributesPlugin", "primitiveFileMasks", &Interpreter::primitiveFileMasks);
    registerNamedPrimitive("FileAttributesPlugin", "primitiveFileAttribute", &Interpreter::primitiveFileAttribute);
    registerNamedPrimitive("FileAttributesPlugin", "primitiveFileExists", &Interpreter::primitiveFileExists);
    registerNamedPrimitive("FileAttributesPlugin", "primitiveOpendir", &Interpreter::primitiveOpendir);
    registerNamedPrimitive("FileAttributesPlugin", "primitiveReaddir", &Interpreter::primitiveReaddir);
    registerNamedPrimitive("FileAttributesPlugin", "primitiveClosedir", &Interpreter::primitiveClosedir);
    registerNamedPrimitive("FileAttributesPlugin", "primitiveRewinddir", &Interpreter::primitiveRewinddir);

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

    // LocalePlugin primitives
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

    // SDL2 input semaphore - enables SDL2 event polling
    // The image calls this to register a semaphore for SDL2 event notification
    registerNamedPrimitive("", "primitiveSetVMSDL2Input:", &Interpreter::primitiveSetVMSDL2Input);
    registerNamedPrimitive("SDL_Event", "primitiveSetVMSDL2Input:", &Interpreter::primitiveSetVMSDL2Input);

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
    // Named primitives have high numbers (typically >= 32768)
    // They are looked up by name from method literals - not yet implemented
    // For now, fail gracefully so the method body executes
    if (primitiveIndex >= 32768) {
        // Named primitive - would need to look up by name in method literals
        // For now, just fail and let the method body execute
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

        if (primitiveIndex >= 264) {
            // Return instance variable at index (primitiveIndex - 264)
            if (!receiver.isObject()) {
                return PrimitiveResult::Failure;
            }
            size_t instVarIndex = static_cast<size_t>(primitiveIndex - 264);
            size_t slotCount = memory_.slotCountOf(receiver);
            if (instVarIndex >= slotCount) {
                return PrimitiveResult::Failure;
            }
            Oop value = memory_.fetchPointer(instVarIndex, receiver);
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

    // Regular primitives (0-255): dispatch through primitive table
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
    static FILE* unimplPrimLog = nullptr;
    static int unimplCount = 0;
    if (!unimplPrimLog) unimplPrimLog = nullptr;
    if (unimplPrimLog && unimplCount < 200) {
        unimplCount++;
        fprintf(unimplPrimLog, "[UNIMPL-PRIM #%d step=%llu] primitive=%d argCount=%d\n",
                unimplCount, (unsigned long long)g_stepNum, primitiveIndex, argCount);
        fflush(unimplPrimLog);
    }
    return PrimitiveResult::Failure;
}

Oop Interpreter::activeContext() const {
    // Would return actual context object
    // For stack-based execution, we'd need to materialize one
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
}

void Interpreter::logCurrentMethod(FILE* out) {
    if (!method_.isObject() || method_.rawBits() <= 0x10000) {
        fprintf(out, "[INTERP-STATE] method_=0x%llx (not valid)\n",
                (unsigned long long)method_.rawBits());
        return;
    }
    // Extract selector from method (last literal before association)
    Oop methodHeader = memory_.fetchPointer(0, method_);
    std::string selName = "?";
    if (methodHeader.isSmallInteger()) {
        int numLiterals = methodHeader.asSmallInteger() & 0x7FFF;
        if (numLiterals >= 2) {
            Oop sel = memory_.fetchPointer(numLiterals - 1, method_);
            if (sel.isObject() && sel.rawBits() > 0x10000) {
                ObjectHeader* selH = sel.asObjectPtr();
                if (selH->isBytesObject() && selH->byteSize() < 100) {
                    selName = std::string((char*)selH->bytes(), selH->byteSize());
                }
            }
        }
    }
    // Get receiver class name
    std::string rcvrClassName = "?";
    if (receiver_.isSmallInteger()) rcvrClassName = "SmallInteger";
    else if (receiver_.isNil()) rcvrClassName = "nil";
    else if (receiver_.isObject() && receiver_.rawBits() > 0x10000) {
        Oop cls = memory_.classOf(receiver_);
        if (cls.isObject() && cls.rawBits() > 0x10000 && memory_.slotCountOf(cls) > 6) {
            Oop nameOop = memory_.fetchPointer(6, cls);
            if (nameOop.isObject() && nameOop.rawBits() > 0x10000) {
                ObjectHeader* nh = nameOop.asObjectPtr();
                if (nh->isBytesObject() && nh->byteSize() < 80) {
                    rcvrClassName = std::string((char*)nh->bytes(), nh->byteSize());
                }
            }
        }
    }
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

} // namespace pharo
