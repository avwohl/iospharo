/*
 * Interpreter.hpp - Bytecode Interpreter for Pharo VM
 *
 * This class implements the Smalltalk bytecode interpreter for Sista V1.
 *
 * EXECUTION MODEL:
 *
 *   The interpreter maintains execution state:
 *   - instructionPointer: Current bytecode position
 *   - stackPointer: Top of operand stack
 *   - framePointer: Current stack frame
 *   - method: Currently executing CompiledMethod
 *   - receiver: Current 'self'
 *
 *   Stack frames contain:
 *   - Saved frame pointer
 *   - Saved instruction pointer
 *   - Method
 *   - Receiver
 *   - Arguments
 *   - Temporaries
 *   - Operand stack
 *
 * SISTA V1 BYTECODES (256 bytecodes):
 *   See docs/SistaV1-Bytecode-Spec.md for the full spec.
 *
 *   0-15:    Push receiver variable 0-15
 *   16-31:   Push literal variable 0-15
 *   32-63:   Push literal constant 0-31
 *   64-75:   Push temp 0-11
 *   76-82:   Push special (receiver, true, false, nil, 0, 1, thisContext)
 *   83:      Dup top
 *   84-87:   Unused
 *   88-91:   Return (receiver, true, false, nil)
 *   92:      Return top
 *   93-95:   Unused
 *   96-111:  Send arithmetic selector 0-15
 *   112-127: Send special selector 16-31
 *   128-143: Send literal sel 0-15 with 0 args
 *   144-159: Send literal sel 0-15 with 1 arg
 *   160-175: Send literal sel 0-15 with 2 args
 *   176-183: Short unconditional jump
 *   184-191: Short jump if true
 *   192-199: Short jump if false
 *   200-207: Pop and store receiver variable 0-7
 *   208-215: Pop and store temp 0-7
 *   216:     Pop top
 *   217-223: Unused
 *   224+:    2-byte and 3-byte extended bytecodes (E0-FF)
 *
 * METHOD CACHE:
 *
 *   A hash table caching (selector, class) -> method lookups.
 *   Dramatically speeds up message sends.
 */

#ifndef PHARO_INTERPRETER_HPP
#define PHARO_INTERPRETER_HPP

#include "DebugSettings.hpp"
#include "DebugVars.hpp"
#include "ShadowSlots.hpp"
#include "jit/J2JSaveLayout.h"
#include "ObjectMemory.hpp"
#include "ImageLoader.hpp"
#include "../platform/EventQueue.hpp"
#include "WorldRenderer.hpp"
#if PHARO_JIT_ENABLED
#include "jit/JITRuntime.hpp"
#include "jit/sista/SistaIR.hpp"
#endif
#include <array>
#include <atomic>
#include <chrono>
#include <dlfcn.h>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <deque>
#include <vector>

// Forward declaration for FFI callback support (defined in vmCallback.h)
struct _VMCallbackContext;
typedef struct _VMCallbackContext VMCallbackContext;

// Forward declaration for friend access from j2j_call.
// Only meaningful when JIT is compiled in (iOS builds have JIT off).
#if PHARO_JIT_ENABLED
extern "C" void jit_rt_j2j_call(pharo::jit::JITState* state);
#endif

namespace pharo {

/// Thrown from primitiveCallbackReturn (and equivalent paths) when a
/// Smalltalk-side FFI callback handler has finished and we need control
/// to return to the libffi closure trampoline (`callbackClosureHandler`
/// in Primitives.cpp).  Replaces a former `siglongjmp(vmcc->trampoline)`
/// pair: with the exception, every C++ destructor between the throw and
/// the catch runs (including asmjit's `ProtectJitReadWriteScope` if any
/// is active), so MAP_JIT W^X mode is restored correctly on its own
/// instead of leaking into the post-callback execution.
///
/// libffi frames on macOS arm64 carry `__compact_unwind` so the
/// exception traverses them safely.
struct CallbackComplete {};

/// Maximum stack depth
constexpr size_t MaxStackDepth = 131072;  // Must be large enough for MaxFrameDepth frames

/// Method cache size (must be power of 2)
constexpr size_t MethodCacheSize = 16384;


// ===== PROCESS/SCHEDULER OBJECT SLOT INDICES =====

/// Process object slots
constexpr int ProcessNextLinkIndex = 0;         // nextLink (for LinkedList chain)
constexpr int ProcessSuspendedContextIndex = 1; // suspendedContext
constexpr int ProcessPriorityIndex = 2;         // priority (SmallInteger 1-80)
constexpr int ProcessMyListIndex = 3;           // myList (the list process is in)

/// ProcessScheduler slots
constexpr int SchedulerProcessListsIndex = 0;   // quiescentProcessLists
constexpr int SchedulerActiveProcessIndex = 1;  // activeProcess

/// LinkedList/Semaphore slots
constexpr int LinkedListFirstLinkIndex = 0;     // firstLink
constexpr int LinkedListLastLinkIndex = 1;      // lastLink
constexpr int SemaphoreExcessSignalsIndex = 2;  // excessSignals (Semaphore only)

/// Primitive function result
enum class PrimitiveResult {
    Success,    // Primitive completed, result on stack
    Failure,    // Primitive failed, execute method body
    Error,      // Fatal error, stop execution
};

/// Detailed execution result (for debugging/tracing)
enum class ExecuteResult {
    Active,             // Executed a bytecode
    Idle,               // No active process
    PrimitiveExecuted,  // Executed a primitive
    MessageSent,        // Sent a message
    Error,              // Execution error
};

/// Forward declaration
class Interpreter;

/// Primitive function signature
using PrimitiveFunc = PrimitiveResult (Interpreter::*)(int argCount);

/// Method cache entry
struct MethodCacheEntry {
    Oop selector;
    Oop classOop;
    Oop method;
    PrimitiveFunc primitive;  // Cached primitive (if any)
    int primitiveIndex;       // Primitive number (0 = none)
    int16_t accessorIndex;    // >=0: getter (pushRecvVar N + returnTop)
                              // -1: not a trivial method
    int16_t setterIndex;      // >=0: setter (popStoreRecvVar N + returnSelf)
    bool returnsSelf;         // true: method is just returnReceiver (yourself)
    int8_t  evenOddKind;      // 0=even, 1=odd, -1=not trivial-evenOdd
    int8_t  returnsLiteralKind; // 1=nil, 2=true, 3=false, 4=SmI 0,
                              // 5=SmI 1, 6=SmI -1, 7=SmI 2; 0=not literal
};

/// Well-known selectors (cached for performance)
struct WellKnownSelectors {
    Oop doesNotUnderstand;
    Oop mustBeBoolean;
    Oop cannotReturn;
    Oop aboutToReturn;
    Oop run;
    Oop value;
    Oop value_;        // value:
    Oop valueValue;    // value:value:
    Oop add;           // +
    Oop subtract;      // -
    Oop lessThan;      // <
    Oop greaterThan;   // >
    Oop lessEqual;     // <=
    Oop greaterEqual;  // >=
    Oop equal;         // =
    Oop notEqual;      // ~=
    Oop multiply;      // *
    Oop divide;        // /
    Oop at;            // at:
    Oop atPut;         // at:put:
    Oop size;          // size
    Oop next;          // next
    Oop nextPut;       // nextPut:
    Oop atEnd;         // atEnd
    Oop eq;            // ==
    Oop class_;        // class
    Oop new_;          // new
    Oop newSize;       // new:
};

class Interpreter {
    // Allow j2j_call to access private interpreter state directly.
    // Only needed when JIT is compiled in.
#if PHARO_JIT_ENABLED
    friend void ::jit_rt_j2j_call(::pharo::jit::JITState* state);
#endif
public:
    explicit Interpreter(ObjectMemory& memory);

    /// Initialize the interpreter with a loaded image
    bool initialize();

    /// Run the interpreter main loop
    void interpret();
    void stopVM(const char* reason);
    void dumpProcessQueues();
    void dumpTimerWedgeState();  // one-shot at first [DELAY-DEATH]; see timer-scheduler-wedge
    // Pin every JIT method live in ANY process's Smalltalk stack (incl. SUSPENDED
    // processes like the Delay timer runner), so CodeZone LRU eviction can't free
    // a method a suspended process will resume into. Called during evictLRU.
    void pinLiveJITMethodsAcrossProcesses();
    bool maybeTerminateStuckProcess();  // guarded terminateStuck_; protects Delay scheduler
    void traceExtentBytecode(uint8_t bc);  // PHARO_TRACE_EXTENT_SEL per-bytecode trace
    void dumpCurrentMethod();
    void dumpJITStats();

    /// Execute a single bytecode (for debugging)
    bool step();

    /// Execute a single bytecode with detailed result
    ExecuteResult stepDetailed();

    /// Stop the interpreter
    void stop() { running_ = false; }
    void triggerTestRunner() { pendingTestRun_.store(true, std::memory_order_release); }
    bool isRunning() const { return running_; }

    /// Start/stop the heartbeat thread (must be called from main thread)
    void startHeartbeat();
    void stopHeartbeat();

    /// Get current millisecond clock (30-bit wrapping counter since VM start)
    /// This is the time base for primitiveMillisecondClock and timer comparisons
    int64_t ioMSecs() const {
        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - vmStartTime_).count();
        return ms & 0x3FFFFFFF;  // 30 bits, wraps every ~12 days
    }

    /// Get the object memory
    ObjectMemory& memory() { return memory_; }

    // ===== GC SUPPORT =====

    /// Convert raw IP pointers to offsets before GC (methods may move)
    void prepareForGC();

    /// Convert IP offsets back to pointers after GC (methods may have
    /// moved).  fullGC=false (the per-scavenge wrap) skips the
    /// full-GC-only tail (flushMethodCache + recoverJITAfterGC's IC
    /// flush) — the ip/J2J-pool rebuilds still run.
    void afterGC(bool fullGC = true);

    /// True between prepareForGC and the end of afterGC.  The scavenge
    /// wrap checks this so a scavenge nested inside fullGC (which
    /// already converted ips to offsets) doesn't re-run prepareForGC on
    /// the offset-ized state and destroy the saved offsets.
    bool gcPrepared() const { return gcPrepared_; }

    /// Log current method and bytecode for debugging
    void logCurrentMethod(FILE* out);

    /// Visit every Oop root the interpreter holds.
    /// Visitor signature: void(Oop&) — visitor may update the Oop in-place.
    template<typename Visitor>
    void forEachRoot(Visitor&& visitor);

    /// Set/get system paths
    void setImageName(const std::string& name) { imageName_ = name; }
    void setVMPath(const std::string& path) { vmPath_ = path; }
    const std::string& imageName() const { return imageName_; }
    const std::string& vmPath() const { return vmPath_; }

    /// Set/get image arguments (passed to image via primitiveGetAttribute index 2+)
    void setImageArguments(const std::vector<std::string>& args) { imageArguments_ = args; }
    const std::vector<std::string>& imageArguments() const { return imageArguments_; }

    /// Set/get VM parameters (returned by primitiveGetAttribute at negative indices)
    /// In the standard Cog VM, these are flags like --headless passed before the image path.
    /// Index -1 returns vmParameters_[0], index -2 returns vmParameters_[1], etc.
    void setVMParameters(const std::vector<std::string>& params) { vmParameters_ = params; }
    const std::vector<std::string>& vmParameters() const { return vmParameters_; }
    bool isHeadless() const {
        for (const auto& p : vmParameters_) { if (p == "--headless") return true; }
        return false;
    }

    /// Set a callback for relinquishProcessor to call instead of usleep.
    /// This allows the platform to process its native run loop (e.g., CFRunLoop)
    /// while the VM sleeps. The callback receives microseconds to sleep.
    using RelinquishCallback = std::function<void(int microseconds)>;
    void setRelinquishCallback(RelinquishCallback cb) { relinquishCallback_ = std::move(cb); }

    /// Set/get screen dimensions
    void setScreenSize(int width, int height) { screenWidth_ = width; screenHeight_ = height; }
    void setScreenDepth(int depth) { screenDepth_ = depth; }
    int screenWidth() const { return screenWidth_; }
    int screenHeight() const { return screenHeight_; }
    Oop displayForm() const { return displayForm_; }
    void setDisplayForm(Oop form) { displayForm_ = form; }
    void initializeDisplayForm();  // Create and set up display Form
    WorldRenderer& worldRenderer() { return worldRenderer_; }
    void processInputEvents();     // Process pending input events
    void dispatchMouseEventToMorph(int x, int y, int buttons, bool isMouseDown); // Direct mouse dispatch
    void handleMenuBarClick(Oop menuBar, int x, int y, int buttons); // Handle menu bar click
    void handleWorldMenuClick(Oop world, int x, int y); // Handle world menu on right-click
    void executeMenuItemAction(Oop itemMorph); // Execute a dropdown menu item's action
    void processPendingMenuAction(); // Process pending menu item action
    Oop lookupMethodByName(Oop classObj, const char* selectorName); // Find method by name
    void processPendingWorldMenu(); // Execute queued world menu invocation
    void drawClickIndicator(int x, int y, int buttons); // Draw visible click feedback
    void syncDisplayToSurface();   // Copy Display Form to platform surface
    void ensureDisplayForm(int width, int height, int depth);  // Create Form and bind to Display global
    int screenDepth() const { return screenDepth_; }

    /// Get error object from primFailCode_ (looks up PrimErrTable, clones for OS errors)
    Oop getErrorObjectFromPrimFailCode();

    /// Get current execution state
    Oop activeContext() const;
    Oop activeMethod() const { return method_; }
    Oop receiver() const { return receiver_; }

    // ===== STACK ACCESS (for primitives) =====
    // These are inline for performance — called millions of times per second.

    /// Push a value onto the stack
    inline void push(Oop value) {
        // Detect a corrupted stackPointer_ pointing outside both the C++
        // stack array AND any known Smalltalk heap region.  Tracking A3:
        // under JIT, after many activations and GCs, stackPointer_
        // sometimes points at an address that's neither the C++ array
        // nor a live Smalltalk heap region.  The ">= end" check below
        // wouldn't catch this because the address isn't contiguous with
        // the C++ array.
        {
            uint8_t* spB = reinterpret_cast<uint8_t*>(stackPointer_);
            uint8_t* arrBase = reinterpret_cast<uint8_t*>(stack_.data());
            uint8_t* arrEnd = arrBase + MaxStackDepth * sizeof(Oop);
            bool inArr = (spB >= arrBase && spB < arrEnd);
            bool inHeap = false;
            if (memory_.isInitialized()) {
                uint8_t* os = memory_.oldSpaceStart();
                uint8_t* oe = memory_.oldSpaceEnd();
                uint8_t* ns = memory_.newSpaceStart();
                uint8_t* ne = memory_.newSpaceEnd();
                uint8_t* ps = memory_.permSpaceStart();
                uint8_t* pe = memory_.permSpaceEnd();
                inHeap = (spB >= os && spB < oe) ||
                         (spB >= ns && spB < ne) ||
                         (spB >= ps && spB < pe);
            }
            if (__builtin_expect(!inArr && !inHeap, 0)) {
                // 2026-05-06 A1: with the StackSafetyZone fix shipped in
                // d9d23903, JIT overflow now lands sp exactly at arr_end
                // (logical limit) instead of past the safety zone.  Emit
                // a clearer "Stack overflow" diagnostic for that clean
                // case rather than the original "CORRUPT" message which
                // implied actual memory corruption.
                bool cleanOverflow = (spB == arrEnd);
                void* ra0 = __builtin_return_address(0);
                // __builtin_return_address(1) is "unsafe" per clang but
                // we only use it as a diagnostic aid on a panic path —
                // a missing/garbage caller PC is fine.  Suppress the
                // -Wframe-address warning for this single call.
                #pragma GCC diagnostic push
                #pragma GCC diagnostic ignored "-Wframe-address"
                void* ra1 = __builtin_extract_return_addr(__builtin_return_address(1));
                #pragma GCC diagnostic pop
                Dl_info info0{}, info1{};
                int got0 = dladdr(ra0, &info0);
                int got1 = dladdr(ra1, &info1);
                fprintf(stderr, "[VM] push() %s stackPointer_=%p "
                        "(not in stack_[%p..%p) and not in any heap region) "
                        "fd=%zu method=#%s\n",
                        cleanOverflow ? "STACK OVERFLOW (sp at arr_end)"
                                       : "with CORRUPT",
                        (void*)spB, (void*)arrBase, (void*)arrEnd,
                        frameDepth_,
                        memory_.selectorOf(method_).c_str());
                fprintf(stderr, "[VM]   ra0=%p sym=%s+%lld\n",
                        ra0,
                        got0 && info0.dli_sname ? info0.dli_sname : "?",
                        got0 ? (long long)((uint8_t*)ra0 - (uint8_t*)info0.dli_saddr) : 0LL);
                fprintf(stderr, "[VM]   ra1=%p sym=%s+%lld\n",
                        ra1,
                        got1 && info1.dli_sname ? info1.dli_sname : "?",
                        got1 ? (long long)((uint8_t*)ra1 - (uint8_t*)info1.dli_saddr) : 0LL);
                // Decode SP to see if it looks like a SmI / nil / bool
                uint64_t spBits = (uint64_t)spB;
                fprintf(stderr, "[VM]   SP raw=0x%llx tag=%d ",
                        (unsigned long long)spBits, (int)(spBits & 7));
                if ((spBits & 7) == 1) {
                    fprintf(stderr, "looks like SmI value=%lld\n",
                            (long long)((int64_t)spBits >> 3));
                } else if (spBits == 0x300000000ULL) {
                    fprintf(stderr, "is nil sentinel\n");
                } else if (spBits == 0x300000010ULL || spBits == 0x300000020ULL) {
                    fprintf(stderr, "is true/false\n");
                } else {
                    fprintf(stderr, "(unknown encoding)\n");
                }
                fprintf(stderr, "[VM]   value being pushed: 0x%llx\n",
                        (unsigned long long)value.rawBits());
                fprintf(stderr, "[VM]   receiver_ = 0x%llx (class=%s)\n",
                        (unsigned long long)receiver_.rawBits(),
                        memory_.classNameOf(receiver_).c_str());
                fprintf(stderr, "[VM]   method_ = 0x%llx\n",
                        (unsigned long long)method_.rawBits());
                fprintf(stderr, "[VM]   instructionPointer_ = %p\n",
                        (void*)instructionPointer_);
                fprintf(stderr, "[VM]   framePointer_ = %p\n",
                        (void*)framePointer_);
                // Dump the previous SP value if available — the stencil that
                // wrote SP usually loaded then stored, so the OLD SP might be
                // just below the corrupted one.  Look at fp+1..16 area for
                // recognizable values.
                if (framePointer_) {
                    fprintf(stderr, "[VM]   FP[0..6] (low bits):\n");
                    for (int i = 0; i < 7; i++) {
                        uint64_t v = ((uint64_t*)framePointer_)[i];
                        fprintf(stderr, "[VM]     FP[%d] = 0x%llx\n", i,
                                (unsigned long long)v);
                    }
                }
                // Walk savedFrames_ to show what called this method
                fprintf(stderr, "[VM]   Saved frame chain (depth %zu):\n",
                        frameDepth_);
                for (size_t fi = 0; fi <= frameDepth_ && fi < 8; fi++) {
                    SavedFrame& sf = savedFrames_[fi];
                    fprintf(stderr, "[VM]     [%zu] method=%s rcvr=0x%llx\n",
                            fi,
                            memory_.selectorOf(sf.savedMethod).c_str(),
                            (unsigned long long)sf.savedReceiver.rawBits());
                }
                stopVM("Corrupt stackPointer_ in push()");
                return;
            }
        }
        if (__builtin_expect(stackPointer_ >= stack_.data() + MaxStackDepth, 0)) {
            fprintf(stderr, "[VM] Stack overflow! fd=%zu method=#%s rcvr=0x%llx sp_offset=%lld\n",
                    frameDepth_, memory_.selectorOf(method_).c_str(),
                    (unsigned long long)receiver_.rawBits(),
                    (long long)(stackPointer_ - stack_.data()));
            // Compute current bytecode offset for context.
            {
                ObjectHeader* mObj = method_.isObject() ? method_.asObjectPtr() : nullptr;
                const uint8_t* bcBase = nullptr;
                if (mObj) {
                    Oop hdr = mObj->slots()[0];
                    int numLits = hdr.isSmallInteger() ? (hdr.asSmallInteger() & 0x7FFF) : 0;
                    bcBase = mObj->bytes() + (1 + numLits) * 8;
                }
                long long bcOff = (bcBase && instructionPointer_)
                    ? (long long)(instructionPointer_ - bcBase) : -1;
                fprintf(stderr, "  current ip=%p bcOff=%lld bc=%02x\n",
                        (void*)instructionPointer_, bcOff,
                        (instructionPointer_ ? *instructionPointer_ : 0));
            }
            // Dump up to the most recent 40 frames. Guard against size_t underflow
            // when frameDepth_ < 40 (previous bug: "i > frameDepth_ - 20" underflows
            // and skips the loop for small fd).
            // Include each frame's saved FP so we can see which frame is leaking
            // slots (compare consecutive saved FPs to estimate per-frame slot use).
            size_t lo = (frameDepth_ > 40) ? (frameDepth_ - 40) : 0;
            Oop* priorFP = framePointer_;  // current frame's base
            for (size_t i = frameDepth_; i > lo; i--) {
                Oop* thisFP = savedFrames_[i-1].savedFP;
                long long frameSize = priorFP ? (long long)(priorFP - thisFP) : -1;
                fprintf(stderr, "  frame[%zu] method=#%s rcvr=0x%llx fp=%p size=%lld\n", i,
                        memory_.selectorOf(savedFrames_[i-1].savedMethod).c_str(),
                        (unsigned long long)savedFrames_[i-1].savedReceiver.rawBits(),
                        (void*)thisFP, frameSize);
                priorFP = thisFP;
            }
            stopVM("Stack overflow in push()");
            return;
        }
        // 2026-05-06 A1: record push origin (C++ return address gives
        // file:line equivalent via dladdr in the dump).
        if (__builtin_expect(stackOriginEnabled_, 0)) {
            recordOrigin(stackPointer_, /*kind=*/1, __builtin_return_address(0));
        }
        *stackPointer_++ = value;
    }

    /// Pop a value from the stack
    inline Oop pop() {
        if (__builtin_expect(stackPointer_ <= stackBase_, 0))
            return memory_.nil();
        return *--stackPointer_;
    }

    /// Peek at stack top without popping
    inline Oop stackTop() const {
        if (__builtin_expect(stackPointer_ <= stackBase_, 0))
            return memory_.nil();
        return *(stackPointer_ - 1);
    }

    /// Get stack value at offset from top (0 = top)
    inline Oop stackValue(size_t offset) const {
        if (__builtin_expect(stackPointer_ - offset <= stackBase_, 0))
            return memory_.nil();
        return *(stackPointer_ - 1 - offset);
    }

    /// Write stack value at offset from top (0 = top)
    inline void stackValuePut(size_t offset, Oop value) {
        *(stackPointer_ - 1 - offset) = value;
    }

    /// Pop multiple values
    inline void popN(size_t n) {
        stackPointer_ -= n;
        if (__builtin_expect(stackPointer_ < stackBase_, 0))
            stackPointer_ = stackBase_;
    }

    /// Number of arguments in current activation
    int argumentCount() const { return argCount_; }

    // ===== METHOD ACCESS (for primitives) =====

    /// Get a literal from the current method
    Oop literal(size_t index) const;

    /// Get the receiver's instance variable
    Oop receiverInstVar(size_t index) const;

    /// Set the receiver's instance variable
    void setReceiverInstVar(size_t index, Oop value);

    // ===== EXTERNAL SEMAPHORE SIGNALING =====

    /// Signal an external semaphore by index (for I/O events, timers, etc.)
    /// Called from outside the interpreter (e.g., event handlers)
    void signalExternalSemaphore(int index);

    /// Check if there are pending external semaphores to signal
    bool hasPendingSignals() const { return pendingSignalHead_.load(std::memory_order_acquire) != pendingSignalTail_.load(std::memory_order_acquire); }

    /// Process any pending external semaphore signals (called during interpret loop)
    void processPendingSignals();

    /// Check timer semaphore and signal if time has elapsed
    void checkTimerSemaphore();

    /// Signal a semaphore synchronously (wake first waiter or increment excess).
    /// If the woken process has higher priority, preempts the active process.
    void synchronousSignal(Oop semaphore);

    /// Walk all Process instances and signal the Semaphore of any process
    /// blocked in SessionManager>>snapshot:andQuit:.  Called at resume time
    /// and periodically from handleForceYield to break a fork/wait deadlock
    /// that our headless eval path can trigger.  See docs/fixed_priority_workarounds.md.
    void unblockStuckSnapshotCallers();

    /// Signal the finalization semaphore if any mourners were queued during GC.
    void signalFinalizationIfNeeded();
    /// Variant for primitiveWait: non-preempting (puts waiter on ready queue).
    void signalFinalizationIfNeededDeferred();

    /// Check if an object can be made immutable (per Cog VM's canBeImmutable:).
    /// Returns false for contexts, ephemerons, weak objects, semaphores,
    /// the processor scheduler, process lists, linked lists, and processes.
    bool canBeImmutable(Oop oop);

    // ===== PRIMITIVE SUPPORT =====

    /// Set the primitive result (success)
    void primitiveSuccess(Oop result);

    /// Mark primitive as failed
    void primitiveFail();

    /// Check if primitive succeeded
    bool primitiveSucceeded() const { return !primitiveFailed_; }

    // External plugin primitive support (for B2DPlugin, etc.)
    using ExternalPrimFunc = int (*)(void);
    void registerNamedPrimitive(const std::string& module, const std::string& name, ExternalPrimFunc func);
    PrimitiveResult callExternalPrimitive(ExternalPrimFunc fn);

private:
    ObjectMemory& memory_;

    // ===== EXECUTION STATE =====

    // Saved frame info for returns
    struct SavedFrame {
        uint8_t* savedIP;
        uint8_t* savedBytecodeEnd;
        Oop savedMethod;
        Oop savedHomeMethod;  // Home method for literal access
        Oop savedReceiver;
        Oop savedClosure;     // FullBlockClosure for block frames, nil for method frames
        Oop savedActiveContext;  // Active context at time of call (for proper return chain)
        Oop materializedContext;  // Cached context from materializeFrameStack (nil if not yet materialized)
        Oop* savedFP;
        // jit-may20b Step 7 (recursive-safe materialize): when this SavedFrame
        // was pushed from a J2JSave by the chain-loop materialize-bail
        // fallback, materializedRetSlot points at the caller's "receiver slot"
        // (= J2JSave.sp - (nArgs+1)*8) where the eventual return value must
        // land.  Normal pushFrame paths leave this NULL.  returnValue (after
        // popFrame) writes the value to *materializedRetSlot when non-null,
        // sets stackPointer_ to materializedRetSlot+1, and skips the standard
        // push.  Deferred A6 iter N+30i root cause; bug presented as fib(N)
        // returning fib(N-2) for ~20% of runs with the inline-J2J gate off.
        Oop* materializedRetSlot;
        int savedArgCount;
        size_t homeFrameDepth;  // For non-local block returns: the frame to return to (SIZE_MAX = not a block)
        // GC: IP offsets (set by prepareForGC, read by afterGC)
        ptrdiff_t savedIPOffset;
        ptrdiff_t savedBytecodeEndOffset;
    };
    static constexpr size_t MaxFrameDepth = 65536;
    static constexpr size_t StackOverflowLimit = 4096;  // Graceful overflow limit — catch infinite recursion fast
    // Extra frame budget granted WHILE signaling a stack-overflow error, so the
    // image's exception machinery + ensure:/ifCurtailed: unwinds can run above
    // the soft limit (4096+8192=12288, still well under MaxFrameDepth). Without
    // this the #error: send re-hits the guard and we'd hard-kill again, leaking
    // any held critical: mutex -> timer-scheduler-wedge (blocker #2).
    static constexpr size_t StackOverflowSignalHeadroom = 8192;

    // J2J save frame — stencil-to-stencil call state saved during J2J execution.
    // Used by the ASM trampoline and chain loop J2J path.
    //
    // Previously held literals/argCount/bcStart as separate fields; those are
    // all derivable from `jitMethod` on return, so we dropped them. Saves 3
    // stores per send, shrinks from 72 to 56 bytes. See
    // docs/jit-j2j-reduction-plan.md for the derivation math.
    //
    // PUBLIC so jit_t2_send (JITRuntime.cpp) can push entries for A1
    // chain-loop continuation.  Only meaningful when JIT is compiled in.
public:
#if PHARO_JIT_ENABLED
#if PHARO_J2J_SAVE_V2
    // V2 packed save (J2JSaveLayout.h): the resumeAddr slot carries
    //   bits 0-47:  the resume code address
    //   bits 48-59: bcOffset of the resume point (post-send), GC-stable
    //   bits 60-63: the send's nArgs
    // jitMethod derives from the address via codeZone.findMethodByPC;
    // the resume continuation pops args + writes the x1 retval with
    // static offsets.
    struct J2JSave {
        Oop* sp;                  // 0
        Oop receiver;             // 8
        Oop* tempBase;            // 16
        uint64_t resumeAddr;      // 24  packed: addr | bcOff<<48 | nArgs<<60
        static constexpr uint64_t kAddrMask = 0x0000FFFFFFFFFFFFULL;
        uint8_t* addr() const {
            return reinterpret_cast<uint8_t*>(resumeAddr & kAddrMask);
        }
        uint32_t bcOff() const { return (resumeAddr >> 48) & 0xFFF; }
        uint32_t nArgs() const { return resumeAddr >> 60; }
    };
    static_assert(sizeof(J2JSave) == 32, "V2 packed save is 32 bytes");
#else
    struct J2JSave {
        Oop* sp;                  // 0
        Oop receiver;             // 8
        Oop* tempBase;            // 16  (pair with ip)
        uint8_t* ip;               // 24
        jit::JITMethod* jitMethod;// 32  (pair with resumeAddr)
        uint8_t* resumeAddr;      // 40  Precomputed JIT code to resume at
        int sendArgCount;         // 48
        int ipOffset;             // 52  prepareForGC stash: `ip` as an
                                  //     offset into the pushing method's
                                  //     bytecodes (-1 = not derivable).
                                  //     afterGC rebuilds the raw ip from
                                  //     it — GC moves CompiledMethods and
                                  //     forEachRoot only updates the
                                  //     receiver Oop, not raw ip.  Never
                                  //     written by emitted code (the push
                                  //     stores only bytes 0-51).
    };
    static_assert(sizeof(J2JSave) == 56, "J2JSave should be 56 bytes after reduction");
#endif
    // Tie the struct to the shared asm layout header (J2JSaveLayout.h)
    // — the .S used to carry a hand-synced duplicate of these offsets.
    static_assert(JSV_SP == offsetof(J2JSave, sp), "JSV_SP");
    static_assert(JSV_RECEIVER == offsetof(J2JSave, receiver), "JSV_RECEIVER");
    static_assert(JSV_TEMPBASE == offsetof(J2JSave, tempBase), "JSV_TEMPBASE");
#if !PHARO_J2J_SAVE_V2
    static_assert(JSV_IP == offsetof(J2JSave, ip), "JSV_IP");
    static_assert(JSV_JITMETHOD == offsetof(J2JSave, jitMethod), "JSV_JITMETHOD");
    static_assert(JSV_SENDARGCOUNT == offsetof(J2JSave, sendArgCount), "JSV_SENDARGCOUNT");
#endif
    static_assert(JSV_RESUMEADDR == offsetof(J2JSave, resumeAddr), "JSV_RESUMEADDR");
    static_assert(JSV_SIZE == sizeof(J2JSave), "JSV_SIZE");
#endif
private:
    // Max J2J save depth per tryJITActivation entry.  At J2JSlotPerEntry
    // depth, the JIT-emitted inline-J2J push bails to ExitSendCached and
    // the C++ materialize creates SavedFrames_ for the inline-J2J saves.
    // After materialize, the outer switch activates the cached method.
    static constexpr int J2JSlotPerEntry = 32;  // max J2J depth per tryJITActivation
    static constexpr int MaxJ2JPoolSize = 1024; // shared pool across recursive entries
    // Primitive error codes (matching PrimErrTable indices in the image).
    // Index is 1-based and matches Pharo's SpecialObjectsArray slot 52.
    //   1: nil
    //   2: #'bad receiver'
    //   3: #'bad argument'
    //   4: #'bad index'
    //   5: #'bad number of arguments'
    //   6: #'inappropriate operation'
    //   7: #'unsupported operation'
    //   8: #'no modification'
    //   9: #'insufficient object memory'  <-- OutOfMemory trigger
    //  10: #'insufficient C memory'
    //  ... (see Pharo SmalltalkImage>>initializePrimitiveErrors)
    static constexpr int PrimErrNoMemory_       = 9;  // Insufficient object memory
    static constexpr int PrimErrNoModification_ = 8;  // Attempt to modify immutable object
    static constexpr int PrimErrOSError         = 21; // Index in PrimErrTable for OS errors
    std::array<SavedFrame, MaxFrameDepth> savedFrames_;
    size_t frameDepth_;
    // jit-may24 Step 2: counter of SavedFrames currently on the stack
    // with materializedRetSlot != nullptr.  Used by canJITActivate's
    // fast-skip check.  Incremented at each materialize-bail push
    // (see Interpreter.cpp ~line 20861, 22367, etc.); decremented in
    // popFrame() when the popped frame had materializedRetSlot set.
    // popFrame is the only path that removes frames, so this stays
    // accurate across all return paths (returnValue + NLR + ensure).
    size_t materializedFrameCount_ = 0;

    // jit-may25 Step A — interp-side IC table for Sista hint extraction
    // when methods aren't JIT-compiled.  Populated by recordInterpSend()
    // from interp's send dispatch.  Read by extractInlineHintsForMethod()
    // as fallback when JITMethod doesn't exist.  Keyed by caller method
    // oop rawBits.  See docs/sista-ic-promotion-plan.md.
    struct InterpHintEntry {
        uint16_t bcOff;
        uint32_t classKey;     // receiver class index (low 22 bits) | (0x80000000 if immediate)
        uint64_t targetMethod;
        uint32_t hits;
    };
    std::unordered_map<uint64_t, std::vector<InterpHintEntry>> interpHints_;

    // Shared J2J save pool — heap-allocated once, carved into per-entry slices
    // by tryJITActivation via j2jPoolCursor_ to avoid per-call stack allocation.
#if PHARO_JIT_ENABLED
    std::array<J2JSave, MaxJ2JPoolSize> j2jPool_;
    int j2jPoolCursor_ = 0;
public:
    // Session H side-stack for BV inline closure restore.  Lives in
    // Interpreter so forEachRoot can walk it during GC; the existing
    // thread_local in JITRuntime.cpp wasn't GC-walked, so closure Oops
    // saved across allocating sends inside the BV-inlined block went
    // stale after compaction.  Capacity matches the j2j save pool depth
    // (256) since BV inline pushes one entry per active save.
    std::array<Oop, 256> bvClosureSaveStack_;
    int bvClosureSaveDepth_ = 0;
    bool bvIsBvSaveAtJ2jDepth_[256] = {false};
private:
#endif

    // Stack (single stack for all frames)
    static constexpr size_t StackSafetyZone = 256;
    std::array<Oop, MaxStackDepth + StackSafetyZone> stack_;
    Oop* stackPointer_;
    Oop* stackBase_;

    // 2026-05-06 A1: per-slot push-origin tracking.  TWO parallel arrays:
    // - stackOrigins_: 64-bit packed (method_hash:32, kind:16, bcOff:16).
    // - stackPushReturnAddr_: 64-bit return address from push()'s caller,
    //   resolvable via dladdr to a C++ symbol+offset.  This is the
    //   "source file/line" equivalent for stack pushes.
    std::vector<uint64_t> stackOrigins_;
    std::vector<void*> stackPushReturnAddr_;
    bool stackOriginEnabled_ = false;
    uint64_t makeOrigin(uint8_t kind) const {
        if (!stackOriginEnabled_) return 0;
        uint64_t methHash = method_.rawBits() >> 4;
        ObjectHeader* mObj = method_.isObject() ? method_.asObjectPtr() : nullptr;
        uint16_t bcOff = 0;
        if (mObj && instructionPointer_) {
            Oop hdr = mObj->slots()[0];
            int numLits = hdr.isSmallInteger() ? (hdr.asSmallInteger() & 0x7FFF) : 0;
            const uint8_t* bcBase = mObj->bytes() + (1 + numLits) * 8;
            ptrdiff_t off = instructionPointer_ - bcBase;
            if (off >= 0 && off < 0xFFFF) bcOff = (uint16_t)off;
        }
        return ((methHash & 0xFFFFFFFFull) << 32)
             | ((uint64_t)kind << 16)
             | bcOff;
    }
    void recordOrigin(Oop* slot, uint8_t kind, void* ra) {
        if (!stackOriginEnabled_) return;
        size_t idx = slot - stack_.data();
        if (idx < stackOrigins_.size()) {
            stackOrigins_[idx] = makeOrigin(kind);
            stackPushReturnAddr_[idx] = ra;
        }
    }

    // Current frame
    Oop* framePointer_;
    uint8_t* instructionPointer_;
    uint8_t* bytecodeEnd_;  // End of bytecodes in current method
    uint8_t lastBytecode_ = 0;  // Last bytecode dispatched (for stack overflow diagnosis)
    // GC: IP offsets for current frame (set by prepareForGC, read by afterGC)
    ptrdiff_t ipOffset_ = 0;
    ptrdiff_t bytecodeEndOffset_ = 0;
    bool gcPrepared_ = false;  // see gcPrepared()

    // GC: the live JITState currently driving JIT execution, or nullptr
    // when no JIT code is on the stack. forEachRoot scans its Oop fields
    // so GC compaction correctly updates state.receiver / state.method /
    // state.cachedTarget / state.returnValue. prepareForGC stashes
    // state.ip as an offset into state.method's bytecodes so afterGC can
    // restore the absolute pointer against the (possibly moved) method.
    // Only the innermost frame is tracked — tryJITActivation/tryResume
    // sync state back to interpreter fields on return, so outer states
    // aren't live once an inner one takes over. Push on entry, restore
    // previous on exit (supports recursion defensively).
#if PHARO_JIT_ENABLED
    jit::JITState* currentJITState_ = nullptr;
    ptrdiff_t jitStateIpOffset_ = 0;
#endif
    Oop method_;            // Current method or CompiledBlock being executed
    Oop newMethod_;         // Method about to be activated (for primitive 117 to read literals)
    Oop homeMethod_;        // Home CompiledMethod (for literal access in blocks)
    Oop receiver_;
    Oop closure_;        // Current FullBlockClosure if executing a block, nil for methods
    Oop activeContext_;  // Current Smalltalk context (for sender chain)
    Oop currentFrameMaterializedCtx_;  // Cached context for current frame (reused across materialize calls)

    // Pending NLR through ensure: — tracks NLR continuation when ensure: runs cleanup.
    // Context-based path: nlrTargetCtx_ = home context, set by handleContextNLRUnwind.
    // Inline path safety net: nlrHomeMethod_ = home method, set by inline ensure: handler.
    // When ensure: returns, returnValue() at fd=0 checks these to continue the NLR.
    Oop nlrTargetCtx_;    // Home context for pending NLR (nil when no NLR pending)
    Oop nlrEnsureCtx_;    // The ensure: context being resumed (nil when no NLR pending)
    Oop nlrHomeMethod_;   // Home method for inline NLR safety net (nil when not active)
    Oop nlrValue_;        // Saved NLR value for safety net
    // Per-process saved NLR state: when a process switch occurs mid-NLR,
    // the outgoing process's NLR state is saved here and restored when it resumes.
    // Without this, process switches during NLR through ensure: would lose the NLR
    // (e.g., Symbol>>intern: returns Symbol class instead of the interned symbol).
    static constexpr int MAX_SAVED_NLR = 8;
    struct SavedNlrState {
        Oop process;       // The process that owns this NLR state
        Oop targetCtx;     // nlrTargetCtx_
        Oop ensureCtx;     // nlrEnsureCtx_
        Oop homeMethod;    // nlrHomeMethod_
        Oop value;         // nlrValue_
    };
    SavedNlrState savedNlrStates_[MAX_SAVED_NLR];
    int savedNlrCount_ = 0;

    Oop lastCannotReturnCtx_;      // Guard against cannotReturn: infinite loop (GC root)
    Oop lastCannotReturnProcess_;  // Process that triggered cannotReturn: (GC root)
    int cannotReturnCount_;        // Counter for cannotReturn: events per process
    uint64_t cannotReturnDeadline_; // Step deadline for cannotReturn: handling (0 = none)
    int argCount_;

    // Sista V1 extension bytes (reset after each instruction)
    int extA_;  // Extension A - modifies literal/temp index
    int extB_;  // Extension B - modifies numArgs/other

    // Bytecode set detection (method header bit 31: 0=V3PlusClosures, 1=SistaV1)
    bool usesSistaV1_;

    // Execution control
    bool running_;
    bool primitiveFailed_;
    int primFailCode_ = 0;  // Primitive failure error code (stored in error: temp on failure)
    int64_t osErrorCode_ = 0;  // OS error code for PrimErrOSError (e.g., errno)
    // DNS resolver state (async)
    uint8_t resolverResult_[16] = {};      // DNS lookup result (IPv4=4 bytes, IPv6=16 bytes)
    int resolverResultSize_ = 0;           // 4 for IPv4, 16 for IPv6
    bool resolverResultValid_ = false;
    int resolverSemaIndex_ = 0;            // Semaphore to signal when lookup completes
    std::atomic<int> resolverStatus_{0};   // 0=Uninit, 1=Ready, 2=Busy, 3=Error
    bool suppressContextSwitch_ = false;  // Suppress forceYield after prim 198 (ensure:) activation

    // Sampling profiler state (primitiveProfileSemaphore/Start/Sample/Primitive).
    // When profileInterval_ > 0, every method activation decrements the counter;
    // at 0 we snapshot the active process + current primitive index, reset to
    // interval, and signal profileSemaphore_ if set.
    Oop profileSemaphore_ = Oop::nil();   // Semaphore to signal on each sample
    Oop profileSample_    = Oop::nil();   // Last active process at sample time
    Oop profilePrimitive_ = Oop::nil();   // Last primitive method at sample time
    int64_t profileInterval_ = 0;         // Counter reload; 0 disables profiling
    int64_t profileCounter_  = 0;         // Current countdown

    int checkCountdown_ = 1024;           // Periodic check countdown (shared with JIT for scheduling)
    bool inExtension_ = false;  // True after extension byte (0xE0/0xE1), prevents forceYield from splitting extension+target
    static constexpr bool dispatchTraceLeakOn_ = false;
    // SP-corruption / per-bc sp-watermark diagnostics: investigation aids
    // for A1/A4 (resolved at root cause).  When PHARO_HOT_PATH_DIAG is
    // off, these are constexpr false — the 17+ `if (traceSpCorrupt_)`
    // checks throughout Interpreter.cpp dead-code-eliminate.  Re-enable
    // for similar future investigations with -DPHARO_HOT_PATH_DIAG=1.
#if PHARO_HOT_PATH_DIAG
    bool traceSpCorrupt_ = false;
    bool tracePerBcSp_ = false;
#else
    static constexpr bool traceSpCorrupt_ = false;
    static constexpr bool tracePerBcSp_ = false;
#endif
    bool finalizationCheckAfterGC_ = false;  // One-shot: signal finalization on next step after GC
    size_t finalizationSignalCount_ = 0;     // Diagnostic: total signalFinalizationIfNeeded firings
    size_t finalizationPendingTotal_ = 0;    // Diagnostic: sum of pending mourners across firings
    std::chrono::steady_clock::time_point lastFinalizationSignalTime_;  // Deferred #3: timestamp of last finalization signal, for pop-latency
    int lastPrimitiveIndex_ = 0;  // For stepDetailed() tracking

    // System paths and arguments
    std::string imageName_;
    std::string vmPath_;
    SpurImageHeader originalImageHeader_{};
public:
    void setOriginalImageHeader(const SpurImageHeader& h) { originalImageHeader_ = h; }
    const SpurImageHeader& originalImageHeader() const { return originalImageHeader_; }
#if PHARO_JIT_ENABLED
    // Used by JITCompiler eviction: live J2J entries are 0..j2jPoolLiveCount().
    const J2JSave* j2jPoolBase() const { return j2jPool_.data(); }
    int j2jPoolLiveCount() const { return j2jPoolCursor_; }
    // Set/get closure_ from JIT helpers (BV inline prep needs to sync
    // closure_ to the BV-inlined block so closure-relative ops like
    // PushTempAtInVec resolve the right captured tempVec).
    Oop currentClosure() const { return closure_; }
    void setCurrentClosure(Oop c) { closure_ = c; }
#endif
private:
    std::vector<std::string> imageArguments_;  // Command-line args for the image (index 2+)
    std::vector<std::string> vmParameters_;    // VM flags like --headless (negative indices)

    // Screen dimensions (configurable, defaults for headless)
    int screenWidth_ = 1024;
    int screenHeight_ = 768;
    int screenDepth_ = 32;

    // Display Form (the Smalltalk Form that represents the screen)
    Oop displayForm_ = Oop::nil();
    bool displayFormReady_ = false;  // Set true on first primitiveForceDisplayUpdate


    // Pending world menu invocation (queued to avoid reentrant execution)
    int pendingWorldMenuX_ = -1;
    int pendingWorldMenuY_ = -1;
    Oop pendingWorldMenuMethod_ = Oop::nil();
    Oop pendingWorldMenuReceiver_ = Oop::nil();
    const char* pendingWorldMenuMethodName_ = nullptr;

    // Pending click info for Pharo
    int pendingClickX_ = 0;
    int pendingClickY_ = 0;
    int pendingClickButtons_ = 0;
    int pendingClickType_ = 0;
    bool hasPendingClick_ = false;

    // World renderer (menu bar, morphs, dropdowns — extracted to WorldRenderer.cpp)
    WorldRenderer worldRenderer_;

    // Pending menu item action (from dropdown click)
    Oop pendingMenuActionMorph_ = Oop::nil();  // Actually the target object
    Oop pendingMenuActionMethod_ = Oop::nil();
    Oop pendingMenuActionArgs_ = Oop::nil();
    std::string pendingMenuActionSelector_;

    // Pending OSiOSDriver install (scheduled for deferred execution)
    Oop pendingDriverInstallMethod_ = Oop::nil();
    Oop pendingDriverInstallReceiver_ = Oop::nil();
    bool hasPendingDriverInstall_ = false;
    bool pendingDriverMethodNeedsArg_ = false;  // True if method takes an argument (e.g., startUp:)

    // Second phase method (setupEventLoop after install)
    Oop pendingDriverSetupMethod_ = Oop::nil();
    Oop pendingDriverSetupReceiver_ = Oop::nil();
    bool hasPendingDriverSetup_ = false;
    bool enableDirectInputSignaling_ = false;  // True when VM should signal input semaphore directly
    bool relinquishSlept_ = false;       // Set by primitiveRelinquishProcessor when it sleeps
    RelinquishCallback relinquishCallback_;  // Platform callback for sleep (CFRunLoop etc.)

    // Debug: visual click indicator
    int debugClickX_ = -1;
    int debugClickY_ = -1;
    int debugClickFrame_ = 0;  // Frame counter for fade-out

    // Pass-through events (events not handled by processInputEvents, passed to Pharo)
    std::deque<pharo::Event> passThroughEvents_;

    // External semaphore signaling (for I/O events)
    // Ring buffer: stores up to 64 pending signal indices, lock-free producer/consumer
    static constexpr int kPendingSignalCapacity = 64;
    std::array<std::atomic<int>, 64> pendingSignals_{};
    std::atomic<int> pendingSignalHead_{0};  // producer writes here (mod capacity)
    std::atomic<int> pendingSignalTail_{0};  // consumer reads here (mod capacity)

    // Force yield flag - set by heartbeat to preempt long-running processes
    // Cascade-#3 forensics: how the current activation was last
    // (re)entered.  1=savedResumeEntry JIT_CALL, 2=inline tryResume,
    // 3=JITRuntime tryResume, 4=fastResume, 5=yield re-entry,
    // 6=normal activation.  Read by the MUSTBOOL trap dump.
    int lastResumeKind_ = 0;

    std::atomic<bool> forceYield_{false};

    // Watchdog: terminate process stuck for too long (VM safety feature)
    std::atomic<int> stuckTicks_{0};       // Consecutive stuck ticks from watchdog thread
    std::atomic<bool> terminateStuck_{false}; // Flag: main thread should terminate current process

    // Test runner trigger (set from monitor thread, checked by main loop)
    std::atomic<bool> pendingTestRun_{false};

    // Timer/delay semaphore (for Delay class)
    Oop timerSemaphore_ = Oop::nil();
    int64_t nextWakeupTime_ = 0;  // 0 means no timer set (in ioMSecs units)
    int64_t nextWakeupUsec_ = INT64_MAX;  // UTC microsecond wakeup (for primitive 242)

    // Deferred timer signal for headless mode
    bool timerSignalDeferred_ = false;  // true when headless mode defers the initial timer signal

    // Delay scheduler death detector and recovery
    Oop lastKnownTimerSemaphore_ = Oop::nil();  // saved for recovery when scheduler dies
    std::chrono::steady_clock::time_point lastTimerSignalTime_{};
    bool timerWasArmed_ = false;  // true after primitive 136/242 arms the timer
    bool schedulerDeathLogged_ = false;  // only log once
    int schedulerRecoveryAttempts_ = 0;  // count recovery attempts

    // GC-safe temporary storage for Oops that need to survive allocation.
    // Used in transferTo() to protect newProcess across materializeFrameStack().
    Oop gcTempOop_ = Oop::nil();

    // Low space threshold for GC (bytes) - signals TheLowSpaceSemaphore when free < threshold
    size_t lowSpaceThreshold_ = 0;

    // VM start time for ioMSecs() - millisecond clock base
    std::chrono::steady_clock::time_point vmStartTime_ = std::chrono::steady_clock::now();

    // Heartbeat thread
    std::atomic<bool> heartbeatRunning_{false};
    std::atomic<bool> pendingDisplaySync_{false};
    std::thread heartbeatThread_;

    // GC verification: saved bytecode at IP before GC to detect corruption
    uint8_t gcVerifyBytecodeAtIP_ = 0xFF;
    uint64_t gcVerifyMethodOop_ = 0;

    // ===== STEP / PERIODIC CHECK STATE =====
    // (Members, not static locals, so delete+new resets them for VM relaunch)
    int stepCheckCounter_ = 0;
    bool deferredPeriodicCheck_ = false;
    // B-1: set true while driving step() inside jitSistaCallSend so
    // step()'s periodic check skips process-switch-triggering work
    // (timer, signals, preemption).  Always false when JIT is
    // disabled.  Lives here (not in the JIT-only block) so the
    // periodic-check code can read it without #if PHARO_JIT_ENABLED
    // guards everywhere.
    bool inSyncSend_ = false;
    // True while handleStackOverflow is driving a Smalltalk #error: signal so the
    // image can unwind (ensure:/ifCurtailed:) instead of a C++ hard-kill. Grants
    // StackOverflowSignalHeadroom extra frames; cleared on process switch
    // (executeFromContext) and lazily once frameDepth_ falls back below the soft
    // limit. See timer-scheduler-wedge memory / blocker #2.
    bool inStackOverflowSignal_ = false;
    Oop trackedProcess_ = Oop::nil();
    std::chrono::steady_clock::time_point trackStartTime_{};
    int64_t cumulativeMs_ = 0;
    std::chrono::steady_clock::time_point lastResumeTime_{};
    bool startupGracePeriod_ = true;
    uint64_t stepCountForDriver_ = 0;
    uint64_t bytecodeCount_ = 0;
    int sendCount_ = 0;
    int dnuDepth_ = 0;
    int wakeLowerCount_ = 0;
    long long lastHeartbeatSteps_ = -1;

    // ===== BENCHMARK MODE =====
    bool benchMode_ = false; // Set when PHARO_BENCH is active
    int traceAtAllPut_ = 0;  // >0 = trace bytecodes inside atAllPut: (counts calls)
    int benchRunCount_ = 0;  // -1=warmup, 0-4=timed runs, 5=done
    std::chrono::high_resolution_clock::time_point benchStartTime_;
    // GC counter snapshots captured at setupBenchContext start, used to
    // compute per-run deltas for the [BENCH ... run N] line.  Without
    // these, the printed gcCount/gcTime is cumulative across all runs
    // and the user has to subtract previous-run's numbers manually.
    size_t benchStartGcCount_ = 0;
    size_t benchStartGcTime_ = 0;
    size_t benchStartScavCount_ = 0;
    size_t benchStartScavTime_ = 0;
    size_t benchStartFullCount_ = 0;
    size_t benchStartFullTime_ = 0;
    size_t benchStartSweepCount_ = 0;
    size_t benchStartSweepTime_ = 0;
    size_t benchStartBytesAllocated_ = 0;
    Oop findMethod(const char* className, const char* selector);
    Oop findMethodInHierarchy(Oop cls, const char* selector);  // walks superclass chain
    Oop allocateInstance(const char* className);  // allocate zero-slot instance
    Oop findBenchFibMethod();  // Re-lookups Integer>>benchFib (GC-safe)
    void handleBenchComplete(Oop returnValue = Oop::nil());  // Benchmark run completion handler
    // Multi-benchmark support
    struct BenchSpec {
        std::string name;
        const char* className;
        const char* selector;
        int64_t arg;  // SmallInteger argument to method
        int runs;     // number of timed runs
        bool instanceReceiver = false;  // true: allocate instance of className as receiver, pass arg separately
    };
    std::vector<BenchSpec> benchSpecs_;
    int benchSpecIdx_ = 0;
    void startBench(const BenchSpec& spec);
    void setupBenchContext();

    // ===== BOOTSTRAP STARTUP STATE =====
    bool imageBooted_ = false;
    int startupAttempt_ = 0;
    bool startupSucceeded_ = false;
    bool displayInitialized_ = false;
    bool uiManagerStarted_ = false;
    bool sensorStartAttempted_ = false;
    bool selectorsInitialized_ = false;

    // ===== STDIO / FFI INIT GATES =====
    bool stdioInitialized_ = false;
    int stdinId_ = 0, stdoutId_ = 1, stderrId_ = 2;
    bool ffiInitialized_ = false;

    // ===== DIAGNOSTIC COUNTERS =====
    int preemptCount_ = 0;
    int bitbltFailCount_ = 0;

    // Clipboard (simple in-memory storage for headless mode)
    std::string clipboardText_;

    // File handles (maps Smalltalk file IDs to FILE pointers)
    std::map<int, FILE*> openFiles_;
    int nextFileId_ = 3;  // 0,1,2 reserved for stdin/stdout/stderr

    // ===== CLASS INDEX CACHE =====
    // These are looked up dynamically from the class table at init time.
    // Do NOT hardcode values — they vary between images.
    uint32_t compiledMethodClassIndex_ = 0;
    uint32_t compiledBlockClassIndex_ = 0;
    uint32_t fullBlockClosureClassIndex_ = 0;
    uint32_t orderedCollectionClassIndex_ = 0;
    uint32_t writeStreamClassIndex_ = 0;
    uint32_t lookupClassIndexByName(const char* name);
    void initializeClassIndexCache();
public:
    uint32_t compiledBlockClassIndex() const { return compiledBlockClassIndex_; }
    uint32_t fullBlockClosureClassIndex() const { return fullBlockClosureClassIndex_; }
    uint32_t orderedCollectionClassIndex() const { return orderedCollectionClassIndex_; }
    // Synthetic primitive: inline OrderedCollection>>add: as if it
    // were a primitive method.  Implements addLast: behavior (no
    // resize) directly, falling back to PrimitiveFailure for the
    // resize case (caller activates the full method).
    PrimitiveResult primitiveOCAdd(int argCount);
    // Synthetic primitive: inline WriteStream>>nextPut:.
    // WriteStream layout (Pharo 13):
    //   slot 0: collection
    //   slot 1: position
    //   slot 2: readLimit
    //   slot 3: writeLimit
    // Algorithm:
    //   if position < writeLimit: position++; collection[position] := arg; ^arg
    //   else: PrimitiveFailure (caller activates the full method for grow)
    PrimitiveResult primitiveWSNextPut(int argCount);
    /// Blocker #4 test: sync C++ interpreter globals from a live JITState (def
    /// in Interpreter.cpp where jit::JITState is fully visible).
#if PHARO_JIT_ENABLED
    void syncGlobalsFromJITState(jit::JITState& s);
#endif
private:

    // ===== CACHES =====

    std::array<MethodCacheEntry, MethodCacheSize> methodCache_;
    WellKnownSelectors selectors_;
    std::array<PrimitiveFunc, 700> primitiveTable_;  // Must be >= 661 for generated_primitives.inc

    // Named primitive registry - maps "moduleName:primitiveName" to function
    std::map<std::string, PrimitiveFunc> namedPrimitives_;

    // External plugin primitive registry (free C functions, not member functions)
    std::map<std::string, ExternalPrimFunc> externalPrimitives_;

    // Bytecode history for debugging
    std::array<uint8_t, 256> recentBytecodes_;
    size_t recentBytecodeIdx_ = 0;

    /// Clear the method cache (used when methods are modified or GC runs)
    void flushMethodCache() {
        for (auto& entry : methodCache_) {
            entry.selector = Oop::nil();
            entry.classOop = Oop::nil();
            entry.method = Oop::nil();
            entry.accessorIndex = -1;
            entry.setterIndex = -1;
            entry.returnsSelf = false;
        }
    }

    /// Flush JIT inline caches and mega cache.
    /// Must be called after GC (compaction moves objects, invalidating cached Oops),
    /// after become:, and after method changes.
    void flushJITCaches() {
#if PHARO_JIT_ENABLED
        if (jitRuntime_.isInitialized()) {
            jitRuntime_.flushCaches();
        }
#endif
    }

    /// Full JIT recovery after GC compaction: flush caches, rebuild MethodMap
    /// from updated JITMethod headers, update special Oops, clear count map.
    void recoverJITAfterGC() {
#if PHARO_JIT_ENABLED
        if (jitRuntime_.isInitialized()) {
            jitRuntime_.recoverAfterGC(memory_);
        }
        recoverSistaAfterGC();
#endif
    }
#if PHARO_JIT_ENABLED
    // Clears Sista's method→fn cache — raw oop keys go stale across
    // Spur compaction.  Defined in Interpreter.cpp so it can see the
    // file-scope Sista runtime pointer.
    void recoverSistaAfterGC();

#endif

public:
#if PHARO_JIT_ENABLED
    // jit-may22b Step 1: rekey Sista's cache during the compact's
    // pointer-update phase, while forwarders are still valid.
    // Caller passes a follow callback: `uint64_t newBits = follow(oldBits)`
    // (0 = drop entry).  Public because ObjectMemory calls it from
    // updatePointersAfterCompact.  Uses std::function to avoid pulling
    // SistaRuntime.hpp into ObjectMemory's translation unit.
    void rekeySistaCacheViaForwarders(
        const std::function<uint64_t(uint64_t)>& follow);
#endif

    // ===== JIT COMPILER =====
#if PHARO_JIT_ENABLED
    jit::JITRuntime jitRuntime_;
    bool jitInitialized_ = false;

    // Try to execute via JIT. Returns true if JIT handled the method
    // (caller should NOT proceed with interpreter execution).
    bool tryJITActivation(Oop method, int argCount);

    // Populate a SavedFrame from a J2JSave at materialize-bail time.
    // Returns false if both save.jitMethod and fallbackJM are null
    // (caller must bail).  See Interpreter.cpp for full semantics.
    bool materializeJ2JSaveIntoFrame(
        SavedFrame& frame, J2JSave& save, jit::JITMethod* fallbackJM,
        const char* siteTag);

    // Inline fast-reject for tryJITActivation (todo.md §2.9).
    // Returns true if the method has a chance of being JIT-compiled;
    // false means definitely not — caller should skip the
    // tryJITActivation call entirely, avoiding its prologue/epilogue.
    //
    // The full check is `jitRuntime_.methodMap().lookup(...)` +
    // executable check; we inline both here.  The call site pattern is:
    //   if (canJITActivate(method) && tryJITActivation(method, argCount))
    //       return;
    // Short-circuit `&&` avoids the call entirely on the miss path.
    inline bool canJITActivate(Oop method) {
#if PHARO_JIT_ENABLED
        if (!jitInitialized_) return false;
        if (!method.isObject() || method.rawBits() < 0x10000) return false;
        jit::JITMethod* jm = jitRuntime_.methodMap().lookup(method.rawBits());
        if (!jm || !jm->isExecutable()) return false;
        // jit-may24 Step 2: don't re-enter JIT while a materialize-bail
        // unwind is in progress.  Two-stage check: counter fast-path
        // (1 load) bails out when no mat frames exist; full 4-frame
        // scan verifies (mirrors original scan exactly — keeps the
        // safety of the proven-correct scan).  When counter is
        // accurate (default), no scan happens.  When counter
        // mismatches (some path missed an increment/decrement), the
        // scan catches the actual matRetSlot frame regardless.
        // PHARO_T1_ALLOW_NESTED_JIT_BAIL=1 opts out (legacy buggy
        // behavior, for bisection).
        if (__builtin_expect(materializedFrameCount_ > 0
                && !g_debug.t1AllowNestedJitBail, 0)) {
            size_t scan = (frameDepth_ < 4) ? frameDepth_ : 4;
            for (size_t i = 0; i < scan; i++) {
                if (savedFrames_[frameDepth_ - 1 - i].materializedRetSlot
                        != nullptr) {
                    return false;
                }
            }
        }
        return true;
#else
        (void)method;
        return false;
#endif
    }

    // On-stack replacement: at interpreter backward jumps, check if the
    // current method is JIT-compiled and transfer execution to JIT.
    void tryOSRAtBackwardJump();

    // Per-bytecode Sista hook (item #8 in jit-multiweek-work.md).
    // Called at backward-jump targets (loop headers).  Looks up the
    // per-(method, bcOffset) Sista cache; on hit, dispatches into
    // the lowered loop body.  On miss, bumps a counter and on
    // threshold queues a Sista compile of the loop body.
    void tryPerBcSistaAtBackwardJump();
#endif  // PHARO_JIT_ENABLED — finalization helpers below run regardless

    // Cog-spec interrupt check fired at backward branches.  Matches
    // cointerp-cpp.c:12236-12260 (backwardJumpCountByte mechanism).
    // Delivers pending finalization signals at a bytecode-safe point.
    // Under PHARO_FINALIZE_DEFERRED, drains the mourn queue natively
    // (bypassing P50/P51 FinalizationProcess) for deterministic timing.
    // Defined regardless of JIT — bytecode dispatch in Interpreter.cpp
    // calls this on every backward branch.
    void backwardBranchInterruptCheck();

    // Drain mournQueue_ synchronously in C++ by reimplementing
    // WeakKeyAssociation>>mourn + Dictionary>>removeKey:ifAbsent:
    // + fixCollisionsFrom: semantics.  Called from
    // backwardBranchInterruptCheck.  Avoids the P50/P51 scheduling
    // race in Pharo's FinalizationProcess.
    void drainMournQueueNatively();
    uint32_t weakKeyAssociationClassIndex_ = 0;
    uint32_t weakArrayClassIndex_ = 0;  // Used to drop WeakArray mourners (see drainMournQueueNatively)
    uint32_t weakValueAssociationClassIndex_ = 0;  // Processed natively like WKA (same Association layout)
#if PHARO_JIT_ENABLED  // re-open: tryJITResumeInCaller and below are JIT-only

    // After a send returns, try to re-enter JIT execution in the caller.
    // Called from returnValue() after push(result).
    void tryJITResumeInCaller();

    // Compute the current bytecode offset (IP - method bytecode start).
    // Returns UINT32_MAX if it can't be computed.
    uint32_t computeCurrentBCOffset();

    // Patch the inline cache after the interpreter resolves a send.
    // Called from sendSelector() after method lookup succeeds.
    void patchJITICAfterSend(Oop resolvedMethod, Oop receiver, Oop selector);
    // Re-resolve a cached IC method against the receiver's live class; returns
    // the fresh method on mismatch (stale-IC dispatch guard, blocker #4 T1).
    Oop validateICTarget(const char* tag, Oop cached, Oop* sp, int sendArgCount);

    // Phase 4 monomorphic inlining: read T1's IC table for this method
    // and return its monomorphic entries as Sista InlineHints.  Empty
    // if the method has no JITMethod or no monomorphic IC sites.
    std::vector<sista::InlineHint>
    extractInlineHintsForMethod(Oop method);

    // Initialize JIT on first use (lazy init after image is loaded)
    void initializeJIT();

    // Pending IC patch data (set during ExitSend when IC was empty)
    uint64_t* pendingICPatch_ = nullptr;
    int pendingICSendArgCount_ = 0;
    // Caller method that owns pendingICPatch_'s slot.  Captured at
    // set time so patchJITICAfterSend can call
    // sistaRuntime->invalidateIfHintless(method) once its IC patches.
    Oop pendingICOwnerMethod_;

    // Re-entrancy guard for JIT resume chaining
    bool inJITResume_ = false;

    // B-1: depth of nested jitSistaCallSend frames.  When > 0, an outer
    // helper is driving step(), and nested sends (recursive Sista
    // callees) MUST bail to the interpreter rather than re-enter the
    // helper — otherwise fib-like recursive workloads blow the C
    // stack.  jit_rt_sista_call_send checks this and deopts (returns
    // 0) when depth would exceed the cap.
    int sistaHelperDepth_ = 0;

    // (inSyncSend_ moved outside the JIT block — see below; it's a
    // plain bool referenced from non-JIT-guarded periodic-check code.)

    // IC statistics
    size_t jitICHits_ = 0;        // ExitSendCached exits (IC hit, skip lookup)
    size_t jitICMisses_ = 0;      // ExitSend exits from sendMono (IC miss or empty)
    size_t jitICPatches_ = 0;     // Successful IC data patches
    size_t jitICStale_ = 0;       // Stale IC invalidations
    // Miss breakdown (why ExitSend fired):
    size_t jitICMissNoSelBits_ = 0;    // icData[18] was 0 — megacache path skipped
    size_t jitICMissCold_ = 0;         // IC had empty slots — first miss at this site
    size_t jitICMissPolymorphic_ = 0;  // IC had entries but none matched — true polymorphism
    size_t jitICMissNoICData_ = 0;     // state.icDataPtr was null (shouldn't happen for T1)
    size_t jitJ2JChains_ = 0;     // JIT-to-JIT chain hits (resume path)
    size_t jitJ2JFallbacks_ = 0;  // JIT-to-JIT fallbacks (resume path)
    size_t jitJ2JActChains_ = 0;  // JIT-to-JIT chain hits (activation path)
    size_t jitJ2JActFalls_ = 0;   // JIT-to-JIT fallbacks (activation path)
    size_t jitJ2JDirectPatches_ = 0;  // IC entries with J2J direct call bit set
    size_t jitJ2JStencilCalls_ = 0;   // Stencil-internal J2J pushFrame calls
    size_t jitJ2JStencilReturns_ = 0; // Stencil-internal J2J popFrame (successful return)
    size_t jitActivations_ = 0;       // tryJITActivation calls
    size_t jitActivationHits_ = 0;    // tryJITActivation entered JIT code

    // --- Chain loop path diagnostics ---
    size_t jitChainPrimChains_ = 0;      // Primitives that chained (no frame push)
    size_t jitChainPrimFalls_ = 0;       // Primitives that pushed frame → bail
    size_t jitPrimFallHisto_[600] = {};  // Per-primitive fall histogram (temp profiling)
    size_t jitPrimChainHisto_[600] = {};  // Per-primitive chain histogram (temp profiling)
    size_t jitMaterializeCount_ = 0;    // materializeJ2J with j2jDepth > 0 (temp diag)
    size_t jitMaterializeTotalDepth_ = 0; // cumulative j2jDepth materialized
    size_t jitChainActivateFalls_ = 0;   // activateMethod pushed frame → bail
    size_t jitChainActivateChains_ = 0;  // activateMethod completed in-place → chain
    // Stencil-J2J non-return exit breakdown
    size_t jitStencilFallSendCached_ = 0;
    size_t jitStencilFallSend_ = 0;
    size_t jitStencilFallJ2JCall_ = 0;
    size_t jitStencilFallOther_ = 0;
    size_t jitYieldCount_ = 0;  // ExitYield hits (backward-jump yield check)
    size_t jitOSREntries_ = 0;  // On-stack replacement entries from backward jumps
    int osrCountdown_ = 0;      // OSR sampling: only check every N backward jumps

    // Matches Cog's `backwardJumpCountByte` — interrupt check fires
    // when this hits 0 (every ~60 backward jumps).  Only consulted
    // when PHARO_FINALIZE_DEFERRED is set.  Used by
    // backwardBranchInterruptCheck regardless of JIT.
#endif  // PHARO_JIT_ENABLED — countdown is JIT-independent
    int backwardBranchCountdown_ = 0;
    static constexpr int kBackwardBranchCheckReload = 60;
#if PHARO_JIT_ENABLED  // re-open: remaining JIT-only fields below

    // Last JIT return tracker — for diagnosing wrong-value DNUs.
    // Updated on every ExitReturn from JIT. Checked in DNU handler.
    struct {
        uint64_t methodBits = 0;  // CompiledMethod oop of callee that returned
        uint64_t returnBits = 0;  // Return value raw bits
        size_t   frameDepth = 0;  // Frame depth at return time
        bool     wasResume = false; // true = tryJITResumeInCaller, false = tryJITActivation
    } lastJitReturn_;

    // J2J ban set: methods whose J2J calls always bail out (callee modifies
    // state before hitting a send, causing double-execution on re-entry).
    // Key = method oop raw bits. Checked in patchJITICAfterSend to avoid
    // setting the J2J entry bit for these methods.
    std::unordered_set<uint64_t> j2jBannedMethods_;
public:
    bool isJ2JBanned(uint64_t methodBits) const {
        return j2jBannedMethods_.count(methodBits) > 0;
    }
    size_t jitICHits() const { return jitICHits_; }
    size_t jitICMisses() const { return jitICMisses_; }
    size_t jitICPatches() const { return jitICPatches_; }
    size_t jitICStale() const { return jitICStale_; }
    size_t jitJ2JChains() const { return jitJ2JChains_; }
    size_t jitJ2JFallbacks() const { return jitJ2JFallbacks_; }
    size_t jitJ2JActChains() const { return jitJ2JActChains_; }
    size_t jitJ2JActFalls() const { return jitJ2JActFalls_; }
    size_t jitJ2JDirectPatches() const { return jitJ2JDirectPatches_; }
    size_t jitJ2JStencilCalls() const { return jitJ2JStencilCalls_; }
    size_t jitJ2JStencilReturns() const { return jitJ2JStencilReturns_; }
    void incJ2JStencilCalls() { jitJ2JStencilCalls_++; }
    void incJ2JStencilReturns() { jitJ2JStencilReturns_++; }
    size_t jitActivations() const { return jitActivations_; }
    size_t jitActivationHits() const { return jitActivationHits_; }
    void trackJ2JEntry(jit::JITState* state);
    void trackJ2JReturn(jit::JITState* state);

    /// J2J direct call: lightweight frame push/pop for GC root scanning.
    /// Called from jit_rt_push_frame / jit_rt_pop_frame during stencil execution.
    inline void pushFrameForJIT(jit::JITState* state) {
        // 2026-05-07 A1 hunt: log pushFrameForJIT for pollEvent:
        const bool a1TraceFJ = g_debug.a1Trace;
        if (__builtin_expect(a1TraceFJ, 0)) {
            Oop targetMethodCheck = Oop::fromRawBits(state->cachedTarget.rawBits());
            if (targetMethodCheck.isObject()
                && memory_.selectorOf(targetMethodCheck) == "pollEvent:") {
                static size_t cnt = 0;
                cnt++;
                if (cnt <= 30 || cnt % 5000 == 1) {
                    fprintf(stderr,
                        "[A1-PUSHFJ] #%zu pushFrameForJIT(#pollEvent:) "
                        "sp_in=%lld fp_in=%lld nArgs=%d fd=%zu\n",
                        cnt,
                        (long long)((Oop*)state->sp - stack_.data()),
                        (long long)(framePointer_ - stack_.data()),
                        state->sendArgCount, frameDepth_);
                }
            }
        }
        // Lightweight frame push for J2J direct calls.
        // Syncs interpreter state from JITState, pushes a frame, then sets up
        // callee state in both interpreter and JITState.
        //
        // Optimized: during J2J chains, closure_, activeContext_, and
        // currentFrameMaterializedCtx_ are always nil (set by previous
        // pushFrameForJIT, never changed by JIT code). We load nil once
        // and store it directly, avoiding 3 interpreter field reads.

        // 2026-05-06 A1 — sp-delta check at pushFrameForJIT.  Records
        // entry sp and validates exit sp = entry + (totalTemps - nArgs).
        Oop* spAtEntry = state->sp;
        Oop targetMethod = Oop::fromRawBits(state->cachedTarget.rawBits());
        int nArgs = state->sendArgCount;

        // 2026-05-06 A1 trace: detect SP-tag corruption in JIT state at
        // pushFrameForJIT entry — `state->sp` here is the JIT-side SP
        // that the stencil left behind.  If a stencil wrote a SmI-encoded
        // value (e.g., a primitive return) into state->sp by mistake,
        // pushFrameForJIT propagates it into stackPointer_ and then into
        // framePointer_ via fp = sp - (nArgs+1).
        if (__builtin_expect(traceSpCorrupt_, 0)) {
            uint64_t spB = (uint64_t)state->sp;
            if ((spB & 7) == 1) {
                static int n = 0;
                if (n++ < 5) {
                    fprintf(stderr,
                        "[SP-CORRUPT-pushFrameForJIT] state->sp=0x%llx "
                        "(SmI %lld) callee=#%s caller=#%s nArgs=%d fd=%zu "
                        "state->ip=%p state->method=0x%llx\n",
                        (unsigned long long)spB,
                        (long long)((int64_t)spB >> 3),
                        memory_.selectorOf(targetMethod).c_str(),
                        memory_.selectorOf(method_).c_str(),
                        nArgs, frameDepth_,
                        (void*)state->ip,
                        (unsigned long long)state->method.rawBits());
                }
            }
        }

        // Sync interpreter state from JITState (the stencil has been modifying sp)
        stackPointer_ = state->sp;
        instructionPointer_ = state->ip;

        // Overflow check
        if (__builtin_expect(frameDepth_ >= StackOverflowLimit, 0)) {
            state->exitReason = jit::ExitStackOverflow;
            return;
        }

        // Load nil once for all nil stores
        Oop nil = memory_.nil();

        // Save current frame — GC-critical Oop fields + IP/bytecodeEnd
        SavedFrame& frame = savedFrames_[frameDepth_++];
        frame.savedIP = instructionPointer_;
        frame.savedBytecodeEnd = bytecodeEnd_;
        frame.savedMethod = method_;
        frame.savedHomeMethod = homeMethod_;
        frame.savedReceiver = receiver_;
        // closure_, activeContext_, materializedContext_ are always nil during J2J
        frame.savedClosure = nil;
        frame.savedActiveContext = nil;
        frame.materializedContext = nil;
        frame.savedFP = framePointer_;
        // Step 7: not a materialize-bail frame; standard popFrame applies.
        // Without this, stale materializedRetSlot from a prior occupant of
        // this slot can leak into returnValue and corrupt fib results.
        frame.materializedRetSlot = nullptr;
        frame.savedArgCount = argCount_;
        frame.homeFrameDepth = SIZE_MAX;

        // Set up callee state in interpreter
        method_ = targetMethod;
        homeMethod_ = targetMethod;
        Oop calleeRecv = stackPointer_[-(nArgs + 1)];
        receiver_ = calleeRecv;
        argCount_ = nArgs;
        // closure_ and activeContext_ are already nil — skip writing

        // Frame pointer: base of the frame (receiver slot)
        Oop* fp = stackPointer_ - (nArgs + 1);
        framePointer_ = fp;

        // Derive JITMethod* from entry address
        jit::JITMethod* jm = reinterpret_cast<jit::JITMethod*>(
            reinterpret_cast<uint8_t*>(state->jitMethod) - sizeof(jit::JITMethod));

        // Compute bytecodeStart using cached methodHeader from JITMethod
        ObjectHeader* methObj = targetMethod.asObjectPtr();
        int numLits = static_cast<int>(jm->methodHeader & 0x7FFF);
        uint8_t* bytecodeStart = methObj->bytes() + (1 + numLits) * 8;
        instructionPointer_ = bytecodeStart;

        // Allocate temps (skipped for most methods where nArgs == tempCount)
        int totalTemps = jm->tempCount;
        if (__builtin_expect(nArgs < totalTemps, 0)) {
            for (int i = nArgs; i < totalTemps; i++) {
                if (__builtin_expect(stackOriginEnabled_, 0)) {
                    recordOrigin(stackPointer_, /*kind=*/2, __builtin_return_address(0));
                }
                *stackPointer_ = nil;
                stackPointer_++;
            }
        }

        // Update JITState for callee
        state->receiver = calleeRecv;
        state->literals = methObj->slots() + 1;
        state->tempBase = fp + 1;
        state->ip = bytecodeStart;
        state->method = targetMethod;
        state->argCount = nArgs;
        state->sp = stackPointer_;
        state->exitReason = jit::ExitNone;
        state->jitMethod = jm;

        // 2026-05-06 A1 — verify sp-delta is exactly (totalTemps - nArgs).
        // PHARO_TRACE_PUSHFRAME_DELTA=1 enables.
        if (__builtin_expect(traceSpCorrupt_, 0)) {
            ptrdiff_t actualDelta = state->sp - spAtEntry;
            ptrdiff_t expectedDelta = (ptrdiff_t)(totalTemps - nArgs);
            if (actualDelta != expectedDelta) {
                static int n = 0;
                if (n++ < 8) {
                    fprintf(stderr,
                        "[PUSHFRAME-DELTA] sel=#%s nArgs=%d totalTemps=%d "
                        "actual=%+lld expected=%+lld\n",
                        memory_.selectorOf(targetMethod).c_str(),
                        nArgs, totalTemps,
                        (long long)actualDelta, (long long)expectedDelta);
                }
            }
        }
    }
    void popFrameForJIT(jit::JITState* state);

    /// Minimal frame pop for J2J fast path (ExitReturn).
    /// Only decrements frameDepth and restores GC-critical interpreter fields.
    /// Caller is responsible for restoring JITState fields from C locals.
    // 2026-05-06 A1 diagnostic helper: restores stackPointer_ from a JIT
    // exit's state.sp.  Logs the call site (return address) when the
    // restored SP has SmI tag bits — proves WHICH JIT exit is leaking
    // a SmI-encoded value into stackPointer_.  Gated on
    // PHARO_TRACE_SP_CORRUPT=1 (zero overhead in production).
    inline void restoreSpFromState(jit::JITState& state, int siteId) {
        stackPointer_ = state.sp;
        if (__builtin_expect(traceSpCorrupt_, 0)) {
            uint64_t spB = (uint64_t)stackPointer_;
            if ((spB & 7) == 1) {
                static int n = 0;
                if (n++ < 8) {
                    void* ra = __builtin_return_address(0);
                    Dl_info info{};
                    int got = dladdr(ra, &info);
                    fprintf(stderr,
                        "[SP-CORRUPT-restore] siteId=%d sp=0x%llx "
                        "(SmI %lld) caller=%s+%lld method=#%s fd=%zu "
                        "state.sp=%p\n",
                        siteId, (unsigned long long)spB,
                        (long long)((int64_t)spB >> 3),
                        got && info.dli_sname ? info.dli_sname : "?",
                        got ? (long long)((uint8_t*)ra - (uint8_t*)info.dli_saddr) : 0LL,
                        memory_.selectorOf(method_).c_str(),
                        frameDepth_, (void*)state.sp);
                }
            }
        }
    }

    inline void j2jPopFrame(Oop callerMethod, Oop callerRecv) {
        --frameDepth_;
        SavedFrame& frame = savedFrames_[frameDepth_];
        method_ = callerMethod;
        homeMethod_ = callerMethod;
        receiver_ = callerRecv;
        framePointer_ = frame.savedFP;
        argCount_ = frame.savedArgCount;
        bytecodeEnd_ = frame.savedBytecodeEnd;
    }

    /// Upgrade an IC entry to J2J direct call if the target is now JIT-compiled.
    /// Called from ExitSendCached handlers when an IC hit goes through C++.
    /// callerMethod is the JIT method whose IC is being filled (for debug only).
    void upgradeICToJ2J(uint64_t* icData, Oop cachedMethod, int sendArgCount,
                        Oop callerMethod = Oop::fromRawBits(0));

    /// Method lookup for T2 inline send helper.
    /// Probes method cache first, falls back to full hierarchy lookup.
    inline Oop lookupMethodForSend(Oop selector, Oop rcvrClass) {
        MethodCacheEntry* ce = probeCache(selector, rcvrClass);
        if (ce) return ce->method;
        Oop resolved = lookupMethod(selector, rcvrClass);
        if (!resolved.isNil()) cacheMethod(selector, rcvrClass, resolved);
        return resolved;
    }

    /// Push a SavedFrame for a T2 caller whose inline send callee bailed.
    /// Called from jit_t2_send when the callee exits with non-ExitReturn.
    inline void pushSavedFrameForT2(Oop method, Oop recv, Oop* tempBase,
                                     int argCount, uint8_t* ip) {
        if (frameDepth_ >= StackOverflowLimit) return;
        Oop nil = memory_.nil();
        ObjectHeader* methObj = method.asObjectPtr();
        SavedFrame& frame = savedFrames_[frameDepth_++];
        frame.savedIP = ip;
        frame.savedBytecodeEnd = methObj->bytes() + methObj->byteSize();
        frame.savedMethod = method;
        frame.savedHomeMethod = method;
        frame.savedReceiver = recv;
        frame.savedClosure = nil;
        frame.savedActiveContext = nil;
        frame.materializedContext = nil;
        frame.savedFP = tempBase - 1;
        frame.materializedRetSlot = nullptr;  // Step 7: no callers — clear for safety
        frame.savedArgCount = argCount;
        frame.homeFrameDepth = SIZE_MAX;
    }

    uint32_t compiledMethodClassIdx() const { return compiledMethodClassIndex_; }

    // Native JIT back edges poll this flag (heartbeat preemption);

    // the address is baked into emitted code at compile time

    // (the Interpreter object is a session-stable singleton).

    void* forceYieldAddr() { return &forceYield_; }


    jit::JITRuntime& jitRuntime() { return jitRuntime_; }

    /// Sista runtime helper: monomorphic Array basicSize.
    /// Returns the size as a SmallInteger Oop on success, or 0 if
    /// the receiver isn't an indexable object (which means the
    /// JIT-side guard was wrong — caller should treat 0 as deopt
    /// signal).  Doesn't allocate; cannot trigger GC.
    uint64_t jitSistaBasicSize(jit::JITState* state, uint64_t rcvBits);

    /// Sista runtime helper: monomorphic Array basicAt:.
    /// Returns the element as an Oop on success, or 0 if the index
    /// is out of bounds or the receiver isn't an Array (deopt).
    /// Doesn't allocate; safe to call without state sync.
    uint64_t jitSistaBasicAt(jit::JITState* state, uint64_t rcvBits,
                              uint64_t idxBits);

    /// Sista runtime helper for basicAt:put: (prim 61).  Returns the
    /// stored value (val) on success, 0 on guard miss / OOB / non-SmI
    /// index / immutable receiver / non-SmI byte value.  Mirrors
    /// jitSistaBasicAt for the at:put: side; lets the lifter emit
    /// kPrimAtPut for any `array at: i put: v` SpecialSend site.
    uint64_t jitSistaBasicAtPut(jit::JITState* state, uint64_t rcvBits,
                                 uint64_t idxBits, uint64_t valBits);

    /// Sista runtime helper: synchronously invoke a send and return
    /// its result.  This is what makes Sista compiled code able to
    /// continue past sends instead of bailing.
    ///
    /// JIT'd code has pushed receiver + nArgs args onto the interpreter
    /// stack at state->sp.  This helper:
    ///   1. Syncs interp stackPointer_ from state->sp.
    ///   2. Calls sendSelector(sel, nArgs) which activates the method.
    ///   3. Drives step() in a loop until frameDepth returns to the
    ///      saved value (the called method has returned).
    ///   4. Pops the result from the interp stack and returns it.
    ///   5. Restores caller JIT state (sp, ip, method).
    ///
    /// NLR detection: if step() pops frameDepth_ BELOW the saved
    /// value, a non-local return jumped past our frame.  Returns
    /// the sentinel `kSistaSendNLR` (= raw bits 0); JIT-side caller
    /// must check and bail to interpreter at the source bcOffset.
    /// (TODO: NLR / exception / GC / process-switch handling per
    ///  the design notes in docs/sista-plan-2026-04-27.md.)
    ///
    /// Returns the raw Oop bits of the send's result on success,
    /// 0 on NLR / abnormal exit (caller must deopt).
    uint64_t jitSistaCallSend(jit::JITState* state,
                                uint64_t selBits,
                                uint64_t nArgs);

    /// Sista runtime helper: create a FullBlockClosure from JIT'd code.
    ///
    /// JIT'd code has pushed `numCopied` (and possibly an extra
    /// receiver) operands onto the interpreter stack, then calls this
    /// helper.  The helper synchronizes our stackPointer_ to state->sp
    /// so that createFullBlockWithLiteral pops from the right place,
    /// invokes it, then pops the resulting block back off (the JIT'd
    /// caller takes the block via the return value, not the stack).
    ///
    /// state->sp is updated to the post-consume top.  Returns the new
    /// FullBlockClosure as raw oop bits.
    uint64_t jitSistaCreateFullBlock(jit::JITState* state,
                                       int litIndex,
                                       int numCopied,
                                       int flags);

    /// Allocate a fresh Array of the given size (initialized to nil).
    /// Mirrors the bytecode-level PushNewArray (0xE7) j=0 path.  No
    /// stack effect — the JIT-compiled caller takes the new Array as
    /// the helper's return value.  Returns 0 on alloc failure.
    uint64_t jitSistaAllocArray(jit::JITState* state, uint64_t size);

    /// JIT helper for kStoreInstVar: write `val` into `recv`'s instVar at
    /// `ivarIdx`.  Returns 1 on success, 0 if the store was refused
    /// (non-object / immutable / bytes / OOB).  See implementation
    /// comment for safety guarantees.
    uint64_t jitStoreInstVar(Oop recv, uint64_t ivarIdx, Oop val);

    /// jit-may20b Step 10: inline-prim 18 (basicNew:) helper for
    /// asmjit-T1's IC HIT path.  state.sp points PAST [rcvr=class,
    /// size].  On success: sets state.sp to post-op (rcvr slot now
    /// holds the new oop, args popped) and returns 1.  On failure:
    /// returns 0 (caller bails to chain-loop dispatch).
    uint64_t jitBasicNewWithArg(jit::JITState* state);
    /// jit-may23 T4: basicNew (0-arg) helper.  Same shape but for
    /// `Class basicNew` — receiver=class, no size arg.
    uint64_t jitBasicNew(jit::JITState* state);

    /// jit-may22b Step 2: asmjit-T1 IC-HIT → Sista dispatch.
    /// Allocates fresh JITState for the callee, sets it up, calls
    /// the Sista fn, propagates return value.  Returns 1 on success
    /// (caller's sp updated with result), 0 on bail.
    uint64_t jitT1SistaDispatch(jit::JITState* callerState,
                                  uint64_t fnPtr, uint64_t methodBits,
                                  uint64_t nArgs);

    /// jit-may22a B1: Sista self-recursive send helper.  Bypasses
    /// jit_rt_sista_call_send's sendSelector + step() machinery and
    /// directly invokes the cached Sista fn for state.method on
    /// itself.  Caller pushed [rcvr, args...] onto state.sp.  On
    /// success: writes the result at the receiver slot (state.sp
    /// adjusted to past the result), returns non-zero.  On failure
    /// (save pool full, no cached fn, or callee deopted): returns
    /// zero so the caller falls back to the standard helper path.
    uint64_t jitSistaSelfRecCall(jit::JITState* state, uint64_t nArgs,
                                   uint64_t bcOffset);

    /// Sista deopt-with-resume helper for kCountedLoopArrayDoAccum.
    /// Called from compiled code when an inlined SmI-fast-path iter
    /// hits a non-SmI element.  Continues the iteration in C++ from
    /// `startIdx`..size, applying the recognized arith op (0=+, 1=-,
    /// 2=*) with full coercion via Smalltalk-side sends for non-SmI
    /// elements.  On full success, writes the final accumulator into
    /// vec[slotByteOff/8] and returns the final accBits (always
    /// non-zero for a valid Oop).  On failure (NLR, depth overflow,
    /// non-fast-path receiver format), returns 0 and leaves vec[slot]
    /// untouched so the caller's conservative deopt to PushFullBlock
    /// can re-run the original do: from scratch with vec[slot] = s_0.
    uint64_t jitSistaCompleteArrayDoAccum(jit::JITState* state,
                                            uint64_t rcvBits,
                                            uint64_t vecBits,
                                            uint64_t slotByteOff,
                                            uint64_t startIdx,
                                            uint64_t accBits,
                                            uint64_t arithCode);

    /// Sista deopt-with-resume helper for kCountedLoopInjectInto.
    /// Called when a canonical-shape inject:into: splice's per-iter
    /// SmI tag check misses on a mixed-type Array.  Continues the
    /// iteration in C++ from `startIdx`..size, threading the
    /// accumulator through the recognized arith op (0=+, 1=-, 2=*).
    /// SmI/SmI iters use overflow-checked native arith; non-SmI or
    /// overflow dispatches via jitSistaCallSend (full image-side
    /// coercion).  Returns the final accumulator on success (always
    /// non-zero for a valid Oop), 0 on failure (NLR / depth overflow
    /// / bad receiver format).  Caller's conservative deopt path
    /// rebuilds [rcv, init] with original init, so a re-run is
    /// correct (just no faster than today).
    uint64_t jitSistaCompleteArrayInjectInto(jit::JITState* state,
                                               uint64_t rcvBits,
                                               uint64_t startIdx,
                                               uint64_t accBits,
                                               uint64_t arithCode);

    /// Sista deopt-with-resume helper for kCountedLoopArrayCollect with
    /// canonical-shape blocks `[:e | e OP const]`.  Compiled code has
    /// allocated `result` and populated result[0..startIdx-2]; helper
    /// finishes result[startIdx-1..size-1] = arr[startIdx..size] OP const.
    /// Returns resultBits on full success, 0 on failure.
    uint64_t jitSistaCompleteArrayCollect(jit::JITState* state,
                                            uint64_t rcvBits,
                                            uint64_t resultBits,
                                            uint64_t startIdx,
                                            uint64_t constBits,
                                            uint64_t arithCode);

private:
#endif

public:
    /// Get the class where a CompiledMethod is defined (from last literal).
    /// Available regardless of JIT — used by super-send paths.
    Oop methodClassOf(Oop method) const;

    /// Return "ClassName" for a compiled method oop, using methodClassOf +
    /// ObjectMemory::nameOfClass.  Returns "?" when the method's class
    /// binding can't be resolved.  Diagnostic use only (B5/DNU traces).
    std::string classNameOfMethod(Oop method) const;

private:

    // ===== BYTECODE DISPATCH =====

    /// Main bytecode dispatch
    void dispatchBytecode(uint8_t bytecode);

    // Single-byte bytecodes
    void pushReceiverVariable(int index);
    void pushTemporary(int index);
    void pushLiteralConstant(int index);
    void pushLiteralVariable(int index);
    void storeReceiverVariable(int index);
    void storeTemporary(int index);
    void pushSpecial(int which);
    void returnValue(Oop value);
    void returnFromMethod();
    void returnFromBlock();
    bool handleContextNLRUnwind(Oop value, Oop startCtx, Oop homeCtx);

    // Extended bytecodes (2-3 bytes)
    void extendedPush();
    void extendedStore();
    void extendedSend();
    void extendedSuperSend();

    // Jumps
    void shortJump(int offset);
    void shortJumpIfTrue(int offset);
    void shortJumpIfFalse(int offset);
    void longJump();
    void longJumpIfTrue();
    void longJumpIfFalse();

    // Sends
    void arithmeticSend(int which);
    void commonSend(int which);
    void sendArithmetic(int which);
    void sendSpecial(int which);
    void sendLiteralZeroArgs(int literalIndex);
    void sendLiteralOneArg(int literalIndex);
    void sendLiteralTwoArgs(int literalIndex);
    void sendSelector(Oop selector, int argCount);

    // Special operations
    void duplicateTop();
    void popStack();
    void createBlock();
    void createFullBlock();
    void createFullBlockWithLiteral(int litIndex, int numCopied, bool receiverOnStack, bool ignoreOuterContext);
    void createBlockWithArgs(int numArgs, int numCopied, int blockSize);

    // ===== MESSAGE SENDING =====

    /// Look up a method in the class hierarchy
    Oop lookupMethod(Oop selector, Oop classOop);

    /// Check method cache
    MethodCacheEntry* probeCache(Oop selector, Oop classOop);

    /// Add entry to method cache
    void cacheMethod(Oop selector, Oop classOop, Oop method);

    /// Activate a method (create new frame)
    void activateMethod(Oop method, int argCount);

    /// Activate a block closure
    void activateBlock(Oop block, int argCount);

    /// Handle stack overflow by terminating the current process
    void handleStackOverflow(int argCount);

    /// Terminate current process and switch to next runnable one (safety net)
    void terminateAndSwitchProcess();

    /// Send doesNotUnderstand:
    void sendDoesNotUnderstand(Oop selector, int argCount);

    /// Invoke a non-CompiledMethod object as a method (sends #run:with:in:)
    void invokeObjectAsMethod(Oop nonMethod, Oop selector, int argCount);

    /// Send mustBeBoolean
    void sendMustBeBoolean(Oop value);

    // ===== FRAME MANAGEMENT =====

    /// Create a new stack frame
    bool pushFrame(Oop method, int argCount);  // Returns false if recursion detected

    /// Pop the current stack frame. Returns false if this was the last frame
    /// (process completed) and the caller should NOT push a return value.
    bool popFrame();
    // NLR-marker validation (cascade-#3 residual): see Interpreter.cpp.
    Oop nlrHomeCMOf(Oop code);
    size_t validateNLRHomeFrame(size_t homeFrame, Oop expectedHome);

    /// Get temporary variable
    Oop temporary(int index) const;

    /// Check if currently executing a CompiledBlock (vs CompiledMethod)
    bool isExecutingBlock() const;

    /// Get temporary from outer context (for remote temp access in blocks)
    Oop outerTemporary(int index) const;

    /// Set temporary in outer context (for remote temp store in blocks)
    void setOuterTemporary(int index, Oop value);

    /// Set temporary variable
    void setTemporary(int index, Oop value);

    /// Get argument
    Oop argument(int index) const;

    // ===== PRIMITIVE DISPATCH =====

    /// Initialize the primitive table
    void initializePrimitives();

    /// Initialize named primitives (module:name -> function mapping)
    void initializeNamedPrimitives();

    /// Register a named primitive (member function)
    void registerNamedPrimitive(const std::string& module, const std::string& name, PrimitiveFunc func);

    /// Execute a primitive
    PrimitiveResult executePrimitive(int primitiveIndex, int argCount);

public:
    /// Get primitive index from method (public so jit_rt_fill_ic can
    /// decode callee primitives during cold-IC mega-cache fill).
    int primitiveIndexOf(Oop method) const;
private:

    // ===== PRIMITIVE IMPLEMENTATIONS =====
    // (See Primitives.cpp for implementations)

    // Stub primitives (always fail or succeed with no-op)
    PrimitiveResult primitiveFailure(int argCount);
    PrimitiveResult primitiveNoop(int argCount);
    PrimitiveResult primitiveDebugPrint(int argCount);
    PrimitiveResult primitiveLowSpaceSemaphore(int argCount);
    PrimitiveResult primitiveDeferDisplayUpdates(int argCount);
    PrimitiveResult primitiveArrayBecome(int argCount);
    PrimitiveResult primitiveIncrementalGC(int argCount);
    PrimitiveResult primitiveSetInterruptKey(int argCount);
    PrimitiveResult primitiveClone(int argCount);
    PrimitiveResult primitiveDoPrimitiveWithArgs(int argCount);
    PrimitiveResult primitiveStringCompareWith(int argCount);
    PrimitiveResult primitiveFetchNextMourner(int argCount);
    PrimitiveResult primitiveExitCriticalSection(int argCount);
    PrimitiveResult primitiveEnterCriticalSection(int argCount);
    PrimitiveResult primitiveTestAndSetOwnershipOfCriticalSection(int argCount);
    PrimitiveResult primitiveExecuteMethodArgsArray(int argCount);
    PrimitiveResult primitiveExecuteMethod(int argCount);
    PrimitiveResult primitiveFloatArrayAt(int argCount);
    PrimitiveResult primitiveFloatArrayAtPut(int argCount);

    // Arithmetic
    PrimitiveResult primitiveAdd(int argCount);
    PrimitiveResult primitiveSubtract(int argCount);
    PrimitiveResult primitiveMultiply(int argCount);
    PrimitiveResult primitiveDivide(int argCount);
    PrimitiveResult primitiveMod(int argCount);
    PrimitiveResult primitiveDiv(int argCount);
    PrimitiveResult primitiveQuo(int argCount);
    PrimitiveResult primitiveBitAnd(int argCount);
    PrimitiveResult primitiveBitOr(int argCount);
    PrimitiveResult primitiveBitXor(int argCount);
    PrimitiveResult primitiveBitShift(int argCount);

    // Comparison
    PrimitiveResult primitiveLessThan(int argCount);
    PrimitiveResult primitiveGreaterThan(int argCount);
    PrimitiveResult primitiveLessOrEqual(int argCount);
    PrimitiveResult primitiveGreaterOrEqual(int argCount);
    PrimitiveResult primitiveEqual(int argCount);
    PrimitiveResult primitiveNotEqual(int argCount);

    // Object access
    PrimitiveResult primitiveAt(int argCount);
    PrimitiveResult primitiveAtPut(int argCount);
    PrimitiveResult primitiveSize(int argCount);
    PrimitiveResult primitiveInstVarAt(int argCount);
    PrimitiveResult primitiveInstVarAtPut(int argCount);
    PrimitiveResult primitiveBasicAt(int argCount);
    PrimitiveResult primitiveBasicAtPut(int argCount);
    PrimitiveResult primitiveBasicSize(int argCount);
    PrimitiveResult primitiveObjectAt(int argCount);       // 68 - CompiledMethod literal access
    PrimitiveResult primitiveObjectAtPut(int argCount);    // 69 - CompiledMethod literal access

    // Object creation
    PrimitiveResult primitiveNew(int argCount);
    PrimitiveResult primitiveNewWithArg(int argCount);
    PrimitiveResult primitiveShallowCopy(int argCount);

    // Identity and class
    PrimitiveResult primitiveIdentityHash(int argCount);
    PrimitiveResult primitiveClass(int argCount);
    PrimitiveResult primitiveIdentical(int argCount);
    PrimitiveResult primitiveNotIdentical(int argCount);

    // Character conversion
    PrimitiveResult primitiveAsCharacter(int argCount);
    PrimitiveResult primitiveAsInteger(int argCount);

    // Stream primitives (65-67)
    PrimitiveResult primitiveNext(int argCount);                 // 65
    PrimitiveResult primitiveNextPut(int argCount);              // 66
    PrimitiveResult primitiveAtEnd(int argCount);                // 67

    // Behavior
    PrimitiveResult primitivePerform(int argCount);
    PrimitiveResult primitivePerformWithArgs(int argCount);

    // Control
    PrimitiveResult primitiveBlockValue(int argCount);
    PrimitiveResult primitiveBlockValueWithArgs(int argCount);
    PrimitiveResult primitiveBlockCopy(int argCount);            // 80
    PrimitiveResult primitiveValue(int argCount);                // 81
    PrimitiveResult primitiveValueWithArgs(int argCount);        // 82
    PrimitiveResult primitiveClosureCopyWithCopiedValues(int argCount); // 200
    PrimitiveResult primitiveFullClosureValue(int argCount);     // 207
    PrimitiveResult primitiveFullClosureValueWithArgs(int argCount); // 208
    PrimitiveResult primitiveFullClosureValueNoContextSwitch(int argCount); // 209

    // Process/Scheduler
    PrimitiveResult primitiveSuspend(int argCount);
    PrimitiveResult primitiveResume(int argCount);
    PrimitiveResult primitiveSignal(int argCount);
    PrimitiveResult primitiveWait(int argCount);

    // System
    PrimitiveResult primitiveQuit(int argCount);
    PrimitiveResult primitiveExitToDebugger(int argCount);
    PrimitiveResult primitiveVMParameter(int argCount);
    PrimitiveResult primitiveSnapshot(int argCount);
    PrimitiveResult primitiveImageName(int argCount);            // 121
    PrimitiveResult primitiveVMPath(int argCount);               // 142

    // File I/O primitives (90-99)
    PrimitiveResult primitiveFileAtEnd(int argCount);              // 90
    PrimitiveResult primitiveFileClose(int argCount);              // 91
    PrimitiveResult primitiveFileGetPosition(int argCount);        // 92
    PrimitiveResult primitiveFileOpen(int argCount);               // 93
    PrimitiveResult primitiveFileRead(int argCount);               // 94
    PrimitiveResult primitiveFileSetPosition(int argCount);        // 95
    PrimitiveResult primitiveFileDelete(int argCount);             // 96
    PrimitiveResult primitiveFileSize(int argCount);               // 97
    PrimitiveResult primitiveFileWrite(int argCount);              // 98
    PrimitiveResult primitiveFileRename(int argCount);             // 99

    // Directory primitives (122-124, 126-127)
    PrimitiveResult primitiveDirectoryCreate(int argCount);        // 122
    PrimitiveResult primitiveDirectoryDelimitor(int argCount);     // 123
    PrimitiveResult primitiveDirectoryLookup(int argCount);        // 124
    PrimitiveResult primitiveDirectoryDelete(int argCount);        // 126
    PrimitiveResult primitiveDirectoryGetMacTypeAndCreator(int argCount); // 127
    PrimitiveResult primitiveGetCurrentWorkingDirectory(int argCount);  // named primitive

    // Additional file primitives (161-164)
    PrimitiveResult primitiveFileStdioHandles(int argCount);       // 161
    PrimitiveResult primitiveFileDescriptorType(int argCount);     // 162
    PrimitiveResult primitiveFileFlush(int argCount);              // 163
    PrimitiveResult primitiveFileTruncate(int argCount);           // 164

    // Display primitives (101-104, 107, 109)
    PrimitiveResult primitiveBeCursor(int argCount);             // 101
    PrimitiveResult primitiveBeDisplay(int argCount);            // 102
    PrimitiveResult primitiveScanCharacters(int argCount);       // 103
    PrimitiveResult primitiveDrawLoop(int argCount);             // 104
    PrimitiveResult primitiveShowDisplayRect(int argCount);      // 107
    void showDisplayBits(Oop destForm, int left, int top, int right, int bottom);
    PrimitiveResult primitiveSnapshotEmbedded(int argCount);     // 109

    // I/O (stubs - iOS-specific implementation elsewhere)
    PrimitiveResult primitiveMousePoint(int argCount);
    PrimitiveResult primitiveMouseButtons(int argCount);
    PrimitiveResult primitiveKeyboardNext(int argCount);
    PrimitiveResult primitiveScreenSize(int argCount);           // 106
    PrimitiveResult primitiveScreenDepth(int argCount);          // 108
    PrimitiveResult primitiveIsVMDisplayUsingSDL2(int argCount); // SDL2 detection for OSSDL2Driver
    PrimitiveResult primitiveSetVMSDL2Input(int argCount);       // Set SDL2 input semaphore
    PrimitiveResult primitiveBeep(int argCount);                 // 140
    PrimitiveResult primitiveClipboardText(int argCount);        // 141
    PrimitiveResult primitiveForceDisplayUpdate(int argCount);

    // System primitives (152-155)
    PrimitiveResult primitiveSetFullScreen(int argCount);          // 152
    PrimitiveResult primitiveInputSemaphore(int argCount);         // 153
    PrimitiveResult primitiveInputWord(int argCount);              // 154
    PrimitiveResult primitiveCompareString(int argCount);          // 155

    // FFI/External primitives (116-118, 147, 570-573)
    PrimitiveResult primitiveFlushExternalPrimitives(int argCount); // 116 (also 570)
    PrimitiveResult primitiveCalloutToFFI(int argCount);           // 117
    PrimitiveResult primitiveDLLCall(int argCount);                // 118
    PrimitiveResult primitiveExternalCall(int argCount);           // 147
    PrimitiveResult primitiveUnloadModule(int argCount);           // 571
    PrimitiveResult primitiveListBuiltinModule(int argCount);      // 572
    PrimitiveResult primitiveListExternalModule(int argCount);     // 573
    PrimitiveResult primitiveFloat64ArrayAdd(int argCount);        // 574

    // Socket primitive (133)
    PrimitiveResult primitiveSocket(int argCount);                 // 133

    // Image segment primitives (213-216)
    PrimitiveResult primitiveStoreImageSegment(int argCount);      // 213
    PrimitiveResult primitiveLoadImageSegment(int argCount);       // 214
    PrimitiveResult primitiveArraySwap(int argCount);              // 215
    PrimitiveResult primitiveFindRoots(int argCount);              // 216

    // Object/memory primitives (217-221)
    PrimitiveResult primitiveVMFunctionality(int argCount);        // 217
    PrimitiveResult primitiveIdentityHash32(int argCount);         // 218
    PrimitiveResult primitiveGrowMemoryByAtLeastByAtLeast(int argCount);    // 219
    PrimitiveResult primitiveImageFormatVersion(int argCount);     // 220
    PrimitiveResult primitiveClosureValueWithArgs(int argCount);   // 221

    // System primitives (528-530)
    PrimitiveResult primitiveGetExtraWordAt(int argCount);         // 528
    PrimitiveResult primitiveSetExtraWordAt(int argCount);         // 529
    PrimitiveResult primitiveImmediateAsInteger(int argCount);     // 530

    // String/encoding primitives (531-534)
    PrimitiveResult primitiveStringEncode(int argCount);           // 531
    PrimitiveResult primitiveStringDecode(int argCount);           // 532
    PrimitiveResult primitiveCharacterAsciiValue(int argCount);    // 533
    PrimitiveResult primitiveAllObjectsInMemory(int argCount);     // 534

    // Reflection primitives (535-538)
    PrimitiveResult primitiveObjectSlotAt(int argCount);           // 535
    PrimitiveResult primitiveObjectSlotAtPut(int argCount);        // 536
    PrimitiveResult primitiveObjectNumSlots(int argCount);         // 537
    PrimitiveResult primitiveObjectFormat(int argCount);           // 538

    // Advanced object primitives (539-550)
    PrimitiveResult primitiveObjectClass(int argCount);            // 539
    PrimitiveResult primitiveObjectClassIndex(int argCount);       // 540
    PrimitiveResult primitiveObjectIsPinned(int argCount);         // 541
    PrimitiveResult primitiveObjectSetPinned(int argCount);        // 542
    PrimitiveResult primitiveObjectIsReadOnly(int argCount);       // 543
    PrimitiveResult primitiveObjectSetReadOnly(int argCount);      // 544
    PrimitiveResult primitiveObjectBytesSize(int argCount);        // 545
    PrimitiveResult primitiveObjectWordsSize(int argCount);        // 546
    PrimitiveResult primitiveObjectPointersSize(int argCount);     // 547
    PrimitiveResult primitiveObjectHeader(int argCount);           // 548
    PrimitiveResult primitiveObjectHeaderPut(int argCount);        // 549
    PrimitiveResult primitiveIdentityHashSmallInteger(int argCount); // 550

    // Method and class primitives (551-560)
    PrimitiveResult primitiveCompiledMethodNumLiterals(int argCount); // 551
    PrimitiveResult primitiveCompiledMethodLiteralAt(int argCount);   // 552
    PrimitiveResult primitiveCompiledMethodLiteralAtPut(int argCount); // 553
    PrimitiveResult primitiveCompiledMethodBytecodeAt(int argCount);  // 554
    PrimitiveResult primitiveCompiledMethodBytecodeAtPut(int argCount); // 555
    PrimitiveResult primitiveCompiledMethodNumArgs(int argCount);     // 556
    PrimitiveResult primitiveCompiledMethodNumTemps(int argCount);    // 557
    PrimitiveResult primitiveCompiledMethodFrameSize(int argCount);   // 558
    PrimitiveResult primitiveCompiledMethodPrimitive(int argCount);   // 559
    PrimitiveResult primitiveCompiledMethodSelector(int argCount);    // 560

    // System and debug primitives (561-570)
    PrimitiveResult primitiveVMHeapStatistics(int argCount);       // 561
    PrimitiveResult primitiveVMGCStatistics(int argCount);         // 562
    PrimitiveResult primitiveVMStackDepth(int argCount);           // 563
    PrimitiveResult primitiveVMBytecodeCount(int argCount);        // 564
    PrimitiveResult primitiveVMSendCount(int argCount);            // 565
    PrimitiveResult primitiveVMPrimitiveCount(int argCount);       // 566
    PrimitiveResult primitiveVMContextSwitchCount(int argCount);   // 567
    PrimitiveResult primitiveVMUptime(int argCount);               // 568
    PrimitiveResult primitiveVMCPUTime(int argCount);              // 569
    PrimitiveResult primitiveVMIdleTime(int argCount);             // 570

    // Additional bit primitives (571-574)
    PrimitiveResult primitiveBitCount(int argCount);               // 571
    PrimitiveResult primitiveBitReverse(int argCount);             // 572
    PrimitiveResult primitiveByteSwap32(int argCount);             // 573
    PrimitiveResult primitiveByteSwap64(int argCount);             // 574

    // Platform primitives (500-513)
    PrimitiveResult primitiveGetEnvironment(int argCount);         // 500
    PrimitiveResult primitiveSetEnvironment(int argCount);         // 501
    PrimitiveResult primitiveGetCurrentDirectory(int argCount);    // 502
    PrimitiveResult primitiveSetCurrentDirectory(int argCount);    // 503
    PrimitiveResult primitiveGetPlatformName(int argCount);        // 504
    PrimitiveResult primitiveGetOSVersion(int argCount);           // 505
    PrimitiveResult primitiveGetProcessorCount(int argCount);      // 506
    PrimitiveResult primitiveGetPhysicalMemory(int argCount);      // 507
    PrimitiveResult primitiveGetHostName(int argCount);            // 508
    PrimitiveResult primitiveGetUserName(int argCount);            // 509
    PrimitiveResult primitiveGetHomeDirectory(int argCount);       // 510
    PrimitiveResult primitiveGetTempDirectory(int argCount);       // 511
    PrimitiveResult primitiveGetVMVersion(int argCount);           // 512
    PrimitiveResult primitiveGetSystemLocale(int argCount);        // 513

    // String primitives (157-158)
    PrimitiveResult primitiveCompareStringCollated(int argCount);  // 157
    PrimitiveResult primitiveCompareStringNoCase(int argCount);    // 158

    // Process/become primitives (197-198, 248-249)
    PrimitiveResult primitiveArrayBecomeOneWay(int argCount);           // 197
    PrimitiveResult primitiveArrayBecomeOneWayCopyHash(int argCount);   // 198 (also 249)
    PrimitiveResult primitiveArrayBecomeOneWayNoCopyHash(int argCount); // 248

    // Context primitive (203)
    PrimitiveResult primitiveValueUninterruptably(int argCount);   // 203

    // Process/system primitives (172, 179, 230, 231)
    PrimitiveResult primitiveSetGCSemaphore(int argCount);         // 172
    PrimitiveResult primitiveRelinquishProcessor(int argCount);    // 230
    PrimitiveResult primitiveFormat(int argCount);                 // 231

    // Time
    PrimitiveResult primitiveMillisecondClock(int argCount);
    PrimitiveResult primitiveSecondsClock(int argCount);
    PrimitiveResult primitiveMicrosecondClock(int argCount);
    PrimitiveResult primitiveLocalMicrosecondClock(int argCount);
    PrimitiveResult primitiveHighResClock(int argCount);
    PrimitiveResult primitiveProcessCpuMicroseconds(int argCount);
    PrimitiveResult primitiveUtcWithOffset(int argCount);
    PrimitiveResult primitiveSignalAtMilliseconds(int argCount);

    // LargeIntegers plugin named primitives
    PrimitiveResult primDigitMultiplyNegative(int argCount);
    PrimitiveResult primDigitAddLargeIntegers(int argCount);
    PrimitiveResult primNormalizePositive(int argCount);
    PrimitiveResult primNormalizeNegative(int argCount);
    PrimitiveResult primDigitDivNegative(int argCount);
    PrimitiveResult primDigitSubtractLargeIntegers(int argCount);
    PrimitiveResult primDigitCompare(int argCount);
    PrimitiveResult primAnyBitFromTo(int argCount);
    PrimitiveResult primMontgomeryDigitLength(int argCount);
    PrimitiveResult primMontgomeryTimesModulo(int argCount);

    // Time/Timezone primitives (242-246)
    PrimitiveResult primitiveSignalAtUTCMicroseconds(int argCount);  // 242
    PrimitiveResult primitiveUpdateTimezone(int argCount);           // 243
    PrimitiveResult primitiveUtcAndTimezoneOffset(int argCount);     // 244
    PrimitiveResult primitiveCoarseUTCMicrosecondClock(int argCount); // 245
    PrimitiveResult primitiveCoarseLocalMicrosecondClock(int argCount); // 246
    PrimitiveResult primitiveProcessCPUTimeMicroseconds(int argCount); // 247

    // VM Profiling primitives (250-253)
    PrimitiveResult primitiveClearVMProfile(int argCount);           // 250
    PrimitiveResult primitiveControlVMProfiling(int argCount);       // 251
    // primitiveVMProfileSamplesInto is declared below (260) and reused for 252
    PrimitiveResult primitiveCollectCogCodeConstituents(int argCount); // 253 (Cog-specific)

    // Misc primitives (222-230)
    PrimitiveResult primitiveClosureValueNoContextSwitch2(int argCount); // 222
    PrimitiveResult primitiveClosureValueWithArgsNoContextSwitch(int argCount); // 223
    PrimitiveResult primitiveSetOrHasIdentityHash(int argCount);        // 224
    PrimitiveResult primitiveLoadInstVar(int argCount);            // 225
    PrimitiveResult primitiveStringCompare(int argCount);          // 226
    PrimitiveResult primitiveStringReplace(int argCount);          // 227
    PrimitiveResult primitiveScreenScale(int argCount);            // 228
    PrimitiveResult primitiveStringHash2(int argCount);            // 229
    PrimitiveResult primitiveShrinkMemory(int argCount);           // 230

    // Misc primitives (232-239) - 231 uses existing primitiveForceDisplayUpdate
    PrimitiveResult primitiveFormPrint(int argCount);              // 232
    PrimitiveResult primitiveSetDisplayMode(int argCount);         // 233
    PrimitiveResult primitiveTestDisplayDepth(int argCount);       // 91 (also 91)
    PrimitiveResult primitiveBitmapDecompress(int argCount);       // 234
    // primitiveStringCompareWith is declared above (158)
    PrimitiveResult primitiveSampledSoundConvert(int argCount);    // 236
    PrimitiveResult primitiveSerialPortOp(int argCount);           // 237
    PrimitiveResult primitivePluginCallback(int argCount);         // 238
    PrimitiveResult primitiveLongRunningPrimitive(int argCount);   // 239

    // Profiling primitives (260-263)
    PrimitiveResult primitiveVMProfileSamplesInto(int argCount);   // 260
    PrimitiveResult primitiveVMProfileInfoInto(int argCount);      // 261
    PrimitiveResult primitiveVMProfileStart(int argCount);         // 262
    PrimitiveResult primitiveVMProfileStop(int argCount);          // 263

    // Event/input primitives (264-269)
    PrimitiveResult primitiveGetNextEvent(int argCount);           // 264
    PrimitiveResult primitiveInputSemaphore2(int argCount);        // 265
    PrimitiveResult primitiveEventProcessingControl(int argCount); // 266
    PrimitiveResult primitiveSampledSound(int argCount);           // 267
    PrimitiveResult primitiveMixedSound(int argCount);             // 268
    PrimitiveResult primitiveControlOSProcess(int argCount);       // 269

    // SurfacePlugin named primitives (for OSSDL2ExternalForm and Athens/Cairo)
    PrimitiveResult primitiveCreateManualSurface(int argCount);
    PrimitiveResult primitiveDestroyManualSurface(int argCount);
    PrimitiveResult primitiveSetManualSurfacePointer(int argCount);
    PrimitiveResult primitiveRegisterSurface(int argCount);
    PrimitiveResult primitiveUnregisterSurface(int argCount);

    // BitBlt primitives (290-299)
    PrimitiveResult primitiveCopyBits(int argCount);               // 290
    // primitiveDrawLoop (291) - uses existing declaration at 104
    PrimitiveResult primitiveCompressToByteArray(int argCount);    // 292
    PrimitiveResult primitiveDecompressFromByteArray(int argCount); // 293
    PrimitiveResult primitiveFindFirstInString(int argCount);      // 294
    PrimitiveResult primitiveTranslateStringWithTable(int argCount); // 295
    PrimitiveResult primitiveFindSubstring(int argCount);          // 296
    PrimitiveResult primitivePixelValueAt(int argCount);           // 297
    PrimitiveResult primitivePixelValueAtPut(int argCount);        // 298
    PrimitiveResult primitiveWarpBits(int argCount);               // 299

    // Sound primitives (300-329)
    PrimitiveResult primitiveSoundStart(int argCount);             // 300
    PrimitiveResult primitiveSoundStartWithSemaphore(int argCount); // 301
    PrimitiveResult primitiveSoundStop(int argCount);              // 302
    PrimitiveResult primitiveSoundAvailableSpace(int argCount);    // 303
    PrimitiveResult primitiveSoundPlaySamples(int argCount);       // 304
    PrimitiveResult primitiveSoundPlaySilence(int argCount);       // 305
    PrimitiveResult primitiveSoundGetVolume(int argCount);         // 306
    PrimitiveResult primitiveSoundSetVolume(int argCount);         // 307
    PrimitiveResult primitiveSoundSetStereoBalance(int argCount);  // 308
    PrimitiveResult primitiveSoundGetSampleRate(int argCount);     // 309
    PrimitiveResult primitiveSoundSetSampleRate(int argCount);     // 310
    PrimitiveResult primitiveSoundRecordStart(int argCount);       // 311
    PrimitiveResult primitiveSoundRecordStop(int argCount);        // 312
    PrimitiveResult primitiveSoundRecordSamplesInto(int argCount); // 313
    PrimitiveResult primitiveSoundGetRecordLevel(int argCount);    // 314
    PrimitiveResult primitiveSoundSetRecordLevel(int argCount);    // 315
    PrimitiveResult primitiveSoundRecordSamplesAvailable(int argCount); // 316
    PrimitiveResult primitiveSoundCodecStatus(int argCount);       // 317
    PrimitiveResult primitiveSoundMixerStart(int argCount);        // 318
    PrimitiveResult primitiveSoundMixerStop(int argCount);         // 319
    PrimitiveResult primitiveSoundMixerPlayChannel(int argCount);  // 320
    PrimitiveResult primitiveSoundMixerSetVolume(int argCount);    // 321
    PrimitiveResult primitiveSoundMixerSetPan(int argCount);       // 322
    PrimitiveResult primitiveSoundMixerStopChannel(int argCount);  // 323
    PrimitiveResult primitiveSoundMixerChannelDone(int argCount);  // 324
    PrimitiveResult primitiveSoundMixerChannelPosition(int argCount); // 325
    PrimitiveResult primitiveSoundInsertSamples(int argCount);     // 326
    PrimitiveResult primitiveSoundStartBuffered(int argCount);     // 327
    PrimitiveResult primitiveSoundEnableAEC(int argCount);         // 328
    PrimitiveResult primitiveSoundSupportsAEC(int argCount);       // 329

    // MIDI primitives (330-349)
    PrimitiveResult primitiveMIDIGetPortCount(int argCount);       // 330
    PrimitiveResult primitiveMIDIGetPortName(int argCount);        // 331
    PrimitiveResult primitiveMIDIOpenPort(int argCount);           // 332
    PrimitiveResult primitiveMIDIClosePort(int argCount);          // 333
    PrimitiveResult primitiveMIDIRead(int argCount);               // 334
    PrimitiveResult primitiveMIDIWrite(int argCount);              // 335
    PrimitiveResult primitiveMIDIGetClock(int argCount);           // 336
    PrimitiveResult primitiveMIDISetClock(int argCount);           // 337
    PrimitiveResult primitiveMIDIParameterGet(int argCount);       // 338
    PrimitiveResult primitiveMIDIParameterSet(int argCount);       // 339
    PrimitiveResult primitiveMIDIDriverVersion(int argCount);      // 340
    PrimitiveResult primitiveMIDIPortType(int argCount);           // 341
    PrimitiveResult primitiveMIDIDeviceID(int argCount);           // 342
    PrimitiveResult primitiveMIDIFlushPort(int argCount);          // 343
    PrimitiveResult primitiveMIDISendNoteOn(int argCount);         // 344
    PrimitiveResult primitiveMIDISendNoteOff(int argCount);        // 345
    PrimitiveResult primitiveMIDISendController(int argCount);     // 346
    PrimitiveResult primitiveMIDISendProgramChange(int argCount);  // 347
    PrimitiveResult primitiveMIDISendPitchBend(int argCount);      // 348
    PrimitiveResult primitiveMIDISendSysEx(int argCount);          // 349

    // Serial port primitives (270-279)
    PrimitiveResult primitiveSerialPortCount(int argCount);         // 270
    PrimitiveResult primitiveSerialPortName(int argCount);          // 271
    PrimitiveResult primitiveSerialPortOpen(int argCount);          // 272
    PrimitiveResult primitiveSerialPortClose(int argCount);         // 273
    PrimitiveResult primitiveSerialPortRead(int argCount);          // 274
    PrimitiveResult primitiveSerialPortWrite(int argCount);         // 275
    PrimitiveResult primitiveSerialPortSetParams(int argCount);     // 276
    PrimitiveResult primitiveSerialPortGetParams(int argCount);     // 277
    PrimitiveResult primitiveSerialPortDataAvailable(int argCount); // 278
    PrimitiveResult primitiveSerialPortFlush(int argCount);         // 279

    // Joystick primitives (280-289)
    PrimitiveResult primitiveJoystickCount(int argCount);           // 280
    PrimitiveResult primitiveJoystickName(int argCount);            // 281
    PrimitiveResult primitiveJoystickOpen(int argCount);            // 282
    PrimitiveResult primitiveJoystickClose(int argCount);           // 283
    PrimitiveResult primitiveJoystickRead(int argCount);            // 284
    PrimitiveResult primitiveJoystickButtonCount(int argCount);     // 285
    PrimitiveResult primitiveJoystickAxisCount(int argCount);       // 286
    PrimitiveResult primitiveJoystickButtonState(int argCount);     // 287
    PrimitiveResult primitiveJoystickAxisValue(int argCount);       // 288
    PrimitiveResult primitiveJoystickHatValue(int argCount);        // 289

    // Socket primitives (350-359)
    PrimitiveResult primitiveSocketCreate(int argCount);            // 350
    PrimitiveResult primitiveSocketDestroy(int argCount);           // 351
    PrimitiveResult primitiveSocketConnect(int argCount);           // 352
    PrimitiveResult primitiveSocketListen(int argCount);            // 353
    PrimitiveResult primitiveSocketAccept(int argCount);            // 354
    PrimitiveResult primitiveSocketSend(int argCount);              // 355
    PrimitiveResult primitiveSocketReceive(int argCount);           // 356
    PrimitiveResult primitiveSocketStatus(int argCount);            // 357
    PrimitiveResult primitiveSocketError(int argCount);             // 358
    PrimitiveResult primitiveSocketLocalAddress(int argCount);      // 359

    // Clipboard/drag-drop primitives (360-369)
    // primitiveClipboardText is at 141, reused for 360
    PrimitiveResult primitiveClipboardTextStore(int argCount);      // 361
    PrimitiveResult primitiveClipboardHasText(int argCount);        // 362
    PrimitiveResult primitiveClipboardClear(int argCount);          // 363
    PrimitiveResult primitiveDragDropFileCount(int argCount);       // 364
    PrimitiveResult primitiveDragDropFileName(int argCount);        // 365
    PrimitiveResult primitiveDragDropRequestFile(int argCount);     // 366
    PrimitiveResult primitiveDragDropCancel(int argCount);          // 367
    PrimitiveResult primitiveClipboardFormats(int argCount);        // 368
    PrimitiveResult primitiveClipboardDataForFormat(int argCount);  // 369

    // Misc plugin primitives (370-379)
    PrimitiveResult primitiveUUIDGenerate(int argCount);            // 370
    PrimitiveResult primitiveUUIDParse(int argCount);               // 371
    PrimitiveResult primitiveUUIDToString(int argCount);            // 372
    PrimitiveResult primitiveSSLCreate(int argCount);               // 373
    PrimitiveResult primitiveSSLDestroy(int argCount);              // 374
    PrimitiveResult primitiveSSLConnect(int argCount);              // 375
    PrimitiveResult primitiveSSLAccept(int argCount);               // 376
    PrimitiveResult primitiveSSLSend(int argCount);                 // 377
    PrimitiveResult primitiveSSLReceive(int argCount);              // 378
    PrimitiveResult primitiveSSLStatus(int argCount);               // 379

    // SSL extended primitives (380-389)
    PrimitiveResult primitiveSSLSetCertificate(int argCount);       // 380
    PrimitiveResult primitiveSSLSetPrivateKey(int argCount);        // 381
    PrimitiveResult primitiveSSLGetPeerCertificate(int argCount);   // 382
    PrimitiveResult primitiveSSLGetCertificateName(int argCount);   // 383
    PrimitiveResult primitiveSSLSetVerifyMode(int argCount);        // 384
    PrimitiveResult primitiveSSLGetVerifyResult(int argCount);      // 385
    PrimitiveResult primitiveSSLSetSNI(int argCount);               // 386
    PrimitiveResult primitiveSSLGetVersion(int argCount);           // 387
    PrimitiveResult primitiveSSLGetCipher(int argCount);            // 388
    PrimitiveResult primitiveSSLClose(int argCount);                // 389

    // Locale primitives (390-399)
    PrimitiveResult primitiveLocaleLanguage(int argCount);          // 390
    PrimitiveResult primitiveLocaleCountry(int argCount);           // 391
    PrimitiveResult primitiveLocaleCurrencySymbol(int argCount);    // 392
    PrimitiveResult primitiveLocaleDecimalSeparator(int argCount);  // 393
    PrimitiveResult primitiveLocaleThousandsSeparator(int argCount);// 394
    PrimitiveResult primitiveLocaleDateFormat(int argCount);        // 395
    PrimitiveResult primitiveLocaleTimeFormat(int argCount);        // 396
    PrimitiveResult primitiveLocaleTimezone(int argCount);          // 397
    PrimitiveResult primitiveLocaleTimezoneOffset(int argCount);    // 398
    PrimitiveResult primitiveLocaleDaylightSaving(int argCount);    // 399

    // CoreMotionPlugin named primitives
    PrimitiveResult primitiveMotionData(int argCount);
    PrimitiveResult primitiveMotionAvailable(int argCount);
    PrimitiveResult primitiveMotionStart(int argCount);
    PrimitiveResult primitiveMotionStop(int argCount);

    // SecurityPlugin named primitives
    PrimitiveResult primitiveCanWriteImage(int argCount);
    PrimitiveResult primitiveDisableImageWrite(int argCount);
    PrimitiveResult primitiveGetSecureUserDirectory(int argCount);
    PrimitiveResult primitiveGetUntrustedUserDirectory(int argCount);

    // Image/graphics primitives (400-409)
    PrimitiveResult primitiveImageReadHeader(int argCount);         // 400
    PrimitiveResult primitiveImageReadPixels(int argCount);         // 401
    PrimitiveResult primitiveImageWritePNG(int argCount);           // 402
    PrimitiveResult primitiveImageWriteJPEG(int argCount);          // 403
    PrimitiveResult primitiveImageScale(int argCount);              // 404
    PrimitiveResult primitiveImageRotate(int argCount);             // 405
    PrimitiveResult primitiveImageComposite(int argCount);          // 406
    PrimitiveResult primitiveImageColorConvert(int argCount);       // 407
    PrimitiveResult primitiveImageFilter(int argCount);             // 408
    PrimitiveResult primitiveImageGetMetadata(int argCount);        // 409

    // System info primitives (410-419)
    PrimitiveResult primitiveSystemBatteryLevel(int argCount);      // 410
    PrimitiveResult primitiveSystemBatteryState(int argCount);      // 411
    PrimitiveResult primitiveSystemScreenBrightness(int argCount);  // 412
    PrimitiveResult primitiveSystemSetScreenBrightness(int argCount);// 413
    PrimitiveResult primitiveSystemDeviceModel(int argCount);       // 414
    PrimitiveResult primitiveSystemDeviceUUID(int argCount);        // 415
    PrimitiveResult primitiveSystemAppVersion(int argCount);        // 416
    PrimitiveResult primitiveSystemAppBuild(int argCount);          // 417
    PrimitiveResult primitiveSystemAvailableMemory(int argCount);   // 418
    PrimitiveResult primitiveSystemDiskSpace(int argCount);         // 419

    // Hardware/sensor primitives (420-429)
    PrimitiveResult primitiveAccelerometerStart(int argCount);      // 420
    PrimitiveResult primitiveAccelerometerStop(int argCount);       // 421
    PrimitiveResult primitiveAccelerometerRead(int argCount);       // 422
    PrimitiveResult primitiveGyroscopeStart(int argCount);          // 423
    PrimitiveResult primitiveGyroscopeStop(int argCount);           // 424
    PrimitiveResult primitiveGyroscopeRead(int argCount);           // 425
    PrimitiveResult primitiveMagnetometerStart(int argCount);       // 426
    PrimitiveResult primitiveMagnetometerStop(int argCount);        // 427
    PrimitiveResult primitiveMagnetometerRead(int argCount);        // 428
    PrimitiveResult primitiveDeviceMotionRead(int argCount);        // 429

    // Location primitives (430-439)
    PrimitiveResult primitiveLocationStart(int argCount);           // 430
    PrimitiveResult primitiveLocationStop(int argCount);            // 431
    PrimitiveResult primitiveLocationRead(int argCount);            // 432
    PrimitiveResult primitiveLocationAccuracy(int argCount);        // 433
    PrimitiveResult primitiveLocationDistance(int argCount);        // 434
    PrimitiveResult primitiveHeadingStart(int argCount);            // 435
    PrimitiveResult primitiveHeadingStop(int argCount);             // 436
    PrimitiveResult primitiveHeadingRead(int argCount);             // 437
    PrimitiveResult primitiveGeocode(int argCount);                 // 438
    PrimitiveResult primitiveReverseGeocode(int argCount);          // 439

    // Camera primitives (440-449)
    PrimitiveResult primitiveCameraCount(int argCount);             // 440
    PrimitiveResult primitiveCameraOpen(int argCount);              // 441
    PrimitiveResult primitiveCameraClose(int argCount);             // 442
    PrimitiveResult primitiveCameraCapture(int argCount);           // 443
    PrimitiveResult primitiveCameraStartPreview(int argCount);      // 444
    PrimitiveResult primitiveCameraStopPreview(int argCount);       // 445
    PrimitiveResult primitiveCameraGetFrame(int argCount);          // 446
    PrimitiveResult primitiveCameraSetFlash(int argCount);          // 447
    PrimitiveResult primitiveCameraSetFocus(int argCount);          // 448
    PrimitiveResult primitiveCameraSetExposure(int argCount);       // 449

    // Notification primitives (450-459)
    PrimitiveResult primitiveNotificationSchedule(int argCount);    // 450
    PrimitiveResult primitiveNotificationCancel(int argCount);      // 451
    PrimitiveResult primitiveNotificationCancelAll(int argCount);   // 452
    PrimitiveResult primitiveNotificationGetPending(int argCount);  // 453
    PrimitiveResult primitiveNotificationRequestPermission(int argCount); // 454
    PrimitiveResult primitiveNotificationGetPermission(int argCount);     // 455
    PrimitiveResult primitiveNotificationSetBadge(int argCount);    // 456
    PrimitiveResult primitiveNotificationGetBadge(int argCount);    // 457
    PrimitiveResult primitiveNotificationRegisterPush(int argCount);// 458
    PrimitiveResult primitiveNotificationGetToken(int argCount);    // 459

    // In-app purchase primitives (460-469)
    PrimitiveResult primitiveIAPCanMakePayments(int argCount);      // 460
    PrimitiveResult primitiveIAPRequestProducts(int argCount);      // 461
    PrimitiveResult primitiveIAPGetProducts(int argCount);          // 462
    PrimitiveResult primitiveIAPPurchase(int argCount);             // 463
    PrimitiveResult primitiveIAPRestore(int argCount);              // 464
    PrimitiveResult primitiveIAPGetTransactions(int argCount);      // 465
    PrimitiveResult primitiveIAPFinishTransaction(int argCount);    // 466
    PrimitiveResult primitiveIAPGetReceipt(int argCount);           // 467
    PrimitiveResult primitiveIAPRefreshReceipt(int argCount);       // 468
    PrimitiveResult primitiveIAPGetSubscriptionStatus(int argCount);// 469

    // Sharing/social primitives (470-479)
    PrimitiveResult primitiveShareText(int argCount);               // 470
    PrimitiveResult primitiveShareImage(int argCount);              // 471
    PrimitiveResult primitiveShareURL(int argCount);                // 472
    PrimitiveResult primitiveShareFile(int argCount);               // 473
    PrimitiveResult primitiveOpenURL(int argCount);                 // 474
    PrimitiveResult primitiveCanOpenURL(int argCount);              // 475
    PrimitiveResult primitiveMailCompose(int argCount);             // 476
    PrimitiveResult primitiveMessageCompose(int argCount);          // 477
    PrimitiveResult primitiveSocialPost(int argCount);              // 478
    PrimitiveResult primitivePrint(int argCount);                   // 479

    // Keychain/security primitives (480-489)
    PrimitiveResult primitiveKeychainSet(int argCount);             // 480
    PrimitiveResult primitiveKeychainGet(int argCount);             // 481
    PrimitiveResult primitiveKeychainDelete(int argCount);          // 482
    PrimitiveResult primitiveKeychainHas(int argCount);             // 483
    PrimitiveResult primitiveBiometricAvailable(int argCount);      // 484
    PrimitiveResult primitiveBiometricAuthenticate(int argCount);   // 485
    PrimitiveResult primitiveCryptoRandomBytes(int argCount);       // 486
    PrimitiveResult primitiveCryptoHash(int argCount);              // 487
    PrimitiveResult primitiveCryptoHMAC(int argCount);              // 488
    PrimitiveResult primitiveCryptoEncrypt(int argCount);           // 489

    // Misc platform primitives (490-499)
    PrimitiveResult primitiveHapticFeedback(int argCount);          // 490
    PrimitiveResult primitiveVibrate(int argCount);                 // 491
    PrimitiveResult primitiveFlashlight(int argCount);              // 492
    PrimitiveResult primitiveIdleTimerDisable(int argCount);        // 493
    PrimitiveResult primitiveStatusBarHide(int argCount);           // 494
    PrimitiveResult primitiveStatusBarStyle(int argCount);          // 495
    PrimitiveResult primitiveOrientationLock(int argCount);         // 496
    PrimitiveResult primitiveOrientationGet(int argCount);          // 497
    PrimitiveResult primitiveAppReview(int argCount);               // 498
    PrimitiveResult primitiveAppSettings(int argCount);             // 499

    // String/Array
    PrimitiveResult primitiveStringAt(int argCount);
    PrimitiveResult primitiveStringAtPut(int argCount);
    PrimitiveResult primitiveReplaceFromTo(int argCount);

    // Float primitives (40-59)
    PrimitiveResult primitiveAsFloat(int argCount);           // 40
    PrimitiveResult primitiveFloatAdd(int argCount);          // 41
    PrimitiveResult primitiveFloatSubtract(int argCount);     // 42
    PrimitiveResult primitiveFloatLessThan(int argCount);     // 43
    PrimitiveResult primitiveFloatGreaterThan(int argCount);  // 44
    PrimitiveResult primitiveFloatLessOrEqual(int argCount);  // 45
    PrimitiveResult primitiveFloatGreaterOrEqual(int argCount); // 46
    PrimitiveResult primitiveFloatEqual(int argCount);        // 47
    PrimitiveResult primitiveFloatNotEqual(int argCount);     // 48
    PrimitiveResult primitiveFloatMultiply(int argCount);     // 49
    PrimitiveResult primitiveFloatDivide(int argCount);       // 50
    PrimitiveResult primitiveTruncated(int argCount);    // 51
    PrimitiveResult primitiveFractionalPart(int argCount);    // 52
    PrimitiveResult primitiveExponent(int argCount);          // 53
    PrimitiveResult primitiveTimesTwoPower(int argCount);     // 54
    PrimitiveResult primitiveSquareRoot(int argCount);   // 55
    PrimitiveResult primitiveSine(int argCount);          // 56
    PrimitiveResult primitiveArctan(int argCount);       // 57
    PrimitiveResult primitiveLogN(int argCount);           // 58
    PrimitiveResult primitiveExp(int argCount);          // 59

    // Point creation
    PrimitiveResult primitiveMakePoint(int argCount);

    // Large integers (20-37)
    PrimitiveResult primitiveRemLargeIntegers(int argCount);       // 20 - rem: (sign of dividend)
    PrimitiveResult primitiveAddLargeIntegers(int argCount);       // 21
    PrimitiveResult primitiveSubtractLargeIntegers(int argCount);  // 22
    PrimitiveResult primitiveLessThanLargeIntegers(int argCount);  // 23
    PrimitiveResult primitiveGreaterThanLargeIntegers(int argCount); // 24
    PrimitiveResult primitiveLessOrEqualLargeIntegers(int argCount); // 25
    PrimitiveResult primitiveGreaterOrEqualLargeIntegers(int argCount); // 26
    PrimitiveResult primitiveEqualLargeIntegers(int argCount);     // 27
    PrimitiveResult primitiveNotEqualLargeIntegers(int argCount);  // 28
    PrimitiveResult primitiveMultiplyLargeIntegers(int argCount);  // 29
    PrimitiveResult primitiveDivideLargeIntegers(int argCount);    // 30
    PrimitiveResult primitiveModLargeIntegers(int argCount);       // 31
    PrimitiveResult primitiveDivLargeIntegers(int argCount);       // 32
    PrimitiveResult primitiveQuoLargeIntegers(int argCount);       // 33
    PrimitiveResult primitiveBitAndLargeIntegers(int argCount);    // 34
    PrimitiveResult primitiveBitOrLargeIntegers(int argCount);     // 35
    PrimitiveResult primitiveBitXorLargeIntegers(int argCount);    // 36
    PrimitiveResult primitiveBitShiftLargeIntegers(int argCount);  // 37

    // GC primitives
    PrimitiveResult primitiveFullGC(int argCount);

    // Utility primitives
    PrimitiveResult primitiveFlushCache(int argCount);       // 89
    PrimitiveResult primitiveBytesLeft(int argCount);        // 112
    PrimitiveResult primitiveSpecialObjectsOop(int argCount); // 129

    // Permanent space primitives (stubs - no perm space implementation)
    PrimitiveResult primitiveMoveToPermSpace(int argCount);           // 90
    PrimitiveResult primitiveMoveToPermSpaceInBulk(int argCount);     // 91
    PrimitiveResult primitiveIsInPermSpace(int argCount);             // 92
    PrimitiveResult primitiveMoveToPermSpaceAllOldObjects(int argCount); // 93

    // Object enumeration primitives
    PrimitiveResult primitiveSomeInstance(int argCount);     // 77
    PrimitiveResult primitiveNextInstance(int argCount);     // 78

    // Array/memory primitives
    PrimitiveResult primitiveConstantFill(int argCount);     // 145
    PrimitiveResult primitiveCompareBytes(int argCount);     // 156
    PrimitiveResult primitiveHashMultiply(int argCount);     // 159

    // Process primitives
    PrimitiveResult primitiveYield(int argCount);            // 167

    // Context primitives
    PrimitiveResult primitiveExceptionMarker(int argCount);  // 199 (exception handler marker, always fails)
    PrimitiveResult primitiveClosureNumArgs(int argCount);   // 206

    // Slot access primitives
    PrimitiveResult primitiveSlotAt(int argCount);           // 173
    PrimitiveResult primitiveSlotAtPut(int argCount);        // 174

    // Object enumeration primitives
    PrimitiveResult primitiveAllInstances(int argCount);     // 177
    PrimitiveResult primitiveAllObjects(int argCount);       // 178

    // Object reference primitives
    PrimitiveResult primitiveObjectPointsTo(int argCount);   // 132

    // Become primitives
    PrimitiveResult primitiveBecome(int argCount);           // 72
    void scanStackReplace(Oop oldOop, Oop newOop);          // Helper for become

    // Bit operation primitives
    PrimitiveResult primitiveHighBit(int argCount);          // 575
    PrimitiveResult primitiveLowBit(int argCount);           // 576

    // Word array access primitives
    PrimitiveResult primitiveIntegerAt(int argCount);        // 165
    PrimitiveResult primitiveIntegerAtPut(int argCount);     // 166

    // Class/behavior primitives
    PrimitiveResult primitiveBehaviorHash(int argCount);     // 175
    PrimitiveResult primitiveChangeClass(int argCount);      // 115
    PrimitiveResult changeClassOf(Oop rcvr, Oop newClass);   // shared helper

    // 16-bit array access primitives
    PrimitiveResult primitiveShortAt(int argCount);          // 143
    PrimitiveResult primitiveShortAtPut(int argCount);       // 144

    // Raw object iteration primitives
    PrimitiveResult primitiveSomeObject(int argCount);       // 138
    PrimitiveResult primitiveNextObject(int argCount);       // 139

    // VM attribute primitive
    PrimitiveResult primitiveGetAttribute(int argCount);     // 149

    // Immutability primitives
    PrimitiveResult primitiveGetImmutability(int argCount);  // 150
    PrimitiveResult primitiveSetImmutability(int argCount);  // 151

    // Object copy primitive
    PrimitiveResult primitiveCopyObject(int argCount);       // 168

    // Compiled method creation primitive
    PrimitiveResult primitiveNewMethod(int argCount);        // 79

    // Instance adoption primitive
    PrimitiveResult primitiveAdoptInstance(int argCount);    // 160

    // Object pinning primitives
    PrimitiveResult primitiveIsPinned(int argCount);         // 183
    PrimitiveResult primitivePin(int argCount);              // 184
    PrimitiveResult primitiveUnpin(int argCount);            // 185

    // Memory management primitives
    PrimitiveResult primitiveMaxIdentityHash(int argCount);  // 176
    PrimitiveResult primitiveGrowMemoryByAtLeast(int argCount);       // 180
    PrimitiveResult primitiveSignalAtBytesLeft(int argCount); // 125

    // Interrupt semaphore primitive
    PrimitiveResult primitiveInterruptSemaphore(int argCount); // 134

    // Context termination primitive
    PrimitiveResult primitiveTerminateTo(int argCount);      // 196

    // Float bit access primitives
    PrimitiveResult primitiveFloatAt(int argCount);          // 38
    PrimitiveResult primitiveFloatAtPut(int argCount);       // 39

    // LargeInteger digit access primitives
    PrimitiveResult primitiveDigitAt(int argCount);          // 19
    PrimitiveResult primitiveDigitAtPut(int argCount);       // 20

    // Exception handler primitives
    PrimitiveResult primitiveMarkHandlerMethod(int argCount);    // 186
    PrimitiveResult primitiveMarkUnwindMethod(int argCount);     // 187
    PrimitiveResult primitiveFindHandlerContext(int argCount);   // 188
    PrimitiveResult primitiveFindNextUnwindContext(int argCount); // 189

    // Context inspection primitives
    PrimitiveResult primitiveContextAt(int argCount);            // 211
    PrimitiveResult primitiveContextAtPut(int argCount);         // 212

    // Context/VM Introspection primitives (213-218)
    PrimitiveResult primitiveContextXray(int argCount);          // 213
    PrimitiveResult primitiveVoidVMState(int argCount);          // 214
    PrimitiveResult primitiveVoidVMStateForMethod(int argCount); // 215
    PrimitiveResult primitiveMethodXray(int argCount);           // 216 (Cog-specific, fails)
    PrimitiveResult primitiveMethodProfilingData(int argCount);  // 217 (Cog-specific, fails)
    PrimitiveResult primitiveDoNamedPrimitiveWithArgs(int argCount); // 218

    // Cache flushing primitives
    PrimitiveResult primitiveFlushCacheByMethod(int argCount);   // 119
    PrimitiveResult primitiveFlushCacheBySelector(int argCount); // 120

    // Perform in superclass primitive
    PrimitiveResult primitivePerformInSuperclass(int argCount);  // 100

    // Closure value variant
    PrimitiveResult primitiveClosureValueNoContextSwitch(int argCount); // 204

    // Class structure primitives
    PrimitiveResult primitiveInstSize(int argCount);             // 254
    // primitiveSizeInBytesOfInstance declared below (181, also 255)
    PrimitiveResult primitiveSuperclass(int argCount);           // 253

    // Context size primitive
    PrimitiveResult primitiveContextSize(int argCount);          // 210

    // Object size primitives (181-182)
    PrimitiveResult primitiveSizeInBytesOfInstance(int argCount); // 181
    PrimitiveResult primitiveSizeInBytes(int argCount);           // 182

    // Context manipulation primitives (190-195)
    PrimitiveResult primitiveSetSender(int argCount);            // 190 - Context>>privSender:
    PrimitiveResult primitiveSetInstructionPointer(int argCount); // 191 - Context>>pc:
    PrimitiveResult primitiveSetStackPointer(int argCount);      // 192 - Context>>stackp:
    PrimitiveResult primitiveSetMethod(int argCount);            // 193 - Context>>method:
    PrimitiveResult primitiveSetReceiver(int argCount);          // 194 - Context>>receiver:
    PrimitiveResult primitiveSetClosureOrNil(int argCount);       // 195 - Context>>closureOrNil:

    // Quick return primitives (256-258)
    PrimitiveResult primitiveQuickReturnSelf(int argCount);      // 256
    PrimitiveResult primitiveQuickReturnTrue(int argCount);      // 257
    PrimitiveResult primitiveQuickReturnFalse(int argCount);     // 258
    PrimitiveResult primitiveQuickReturnNil(int argCount);       // 259

    // Object format query primitives
    PrimitiveResult primitiveIsBytes(int argCount);              // 15 (part of)
    PrimitiveResult primitiveIsWords(int argCount);              // 15 (part of)
    PrimitiveResult primitiveIsPointers(int argCount);           // 15 (part of)

    // String hash primitives
    PrimitiveResult primitiveStringHash(int argCount);           // not in standard table
    PrimitiveResult primitiveStringHashInitialHash(int argCount); // 146 - stringHash:initialHash:
    PrimitiveResult primitiveIndexOfAscii(int argCount);         // MiscPrimitivePlugin - indexOfAscii:inString:startingAt:

    // Class name primitive
    PrimitiveResult primitiveClassName(int argCount);            // 514

    // FFI and system primitives (515-527)
    PrimitiveResult primitiveVMInformation(int argCount);        // 515
    PrimitiveResult primitiveImageBaseAddress(int argCount);     // 516
    PrimitiveResult primitiveHighestAvailableAddress(int argCount); // 517
    PrimitiveResult primitiveIsContextPostMortem(int argCount);  // 518
    PrimitiveResult primitiveSandboxedArgs(int argCount);        // 519
    PrimitiveResult primitiveDebugHalt(int argCount);            // 520
    PrimitiveResult primitiveFlushExternalPrimitiveOf(int argCount); // 521
    PrimitiveResult primitivePrepareStackForNonLocalReturn(int argCount); // 522
    PrimitiveResult primitiveContextInstructionPointer(int argCount); // 523
    PrimitiveResult primitiveExternalObjectAccess(int argCount); // 524
    PrimitiveResult primitiveByteArrayToInt32(int argCount);     // 525
    PrimitiveResult primitiveInt32ToByteArray(int argCount);     // 526
    PrimitiveResult primitivePointerAddress(int argCount);       // 527

    // Old Space / Pinned Allocation Primitives (596-599)
    PrimitiveResult primitiveNewOldSpace(int argCount);          // 596
    PrimitiveResult primitiveNewWithArgOldSpace(int argCount);   // 597
    PrimitiveResult primitiveNewPinned(int argCount);            // 598
    PrimitiveResult primitiveNewWithArgPinned(int argCount);     // 599

    // FFI Byte Access Primitives (600-659)
    // Load from bytes (ByteArray, String, etc.)
    PrimitiveResult primitiveLoadBoolean8FromBytes(int argCount);    // 600
    PrimitiveResult primitiveLoadUInt8FromBytes(int argCount);       // 601
    PrimitiveResult primitiveLoadInt8FromBytes(int argCount);        // 602
    PrimitiveResult primitiveLoadUInt16FromBytes(int argCount);      // 603
    PrimitiveResult primitiveLoadInt16FromBytes(int argCount);       // 604
    PrimitiveResult primitiveLoadUInt32FromBytes(int argCount);      // 605
    PrimitiveResult primitiveLoadInt32FromBytes(int argCount);       // 606
    PrimitiveResult primitiveLoadUInt64FromBytes(int argCount);      // 607
    PrimitiveResult primitiveLoadInt64FromBytes(int argCount);       // 608
    PrimitiveResult primitiveLoadPointerFromBytes(int argCount);     // 609
    PrimitiveResult primitiveLoadChar8FromBytes(int argCount);       // 610
    PrimitiveResult primitiveLoadChar16FromBytes(int argCount);      // 611
    PrimitiveResult primitiveLoadChar32FromBytes(int argCount);      // 612
    PrimitiveResult primitiveLoadFloat32FromBytes(int argCount);     // 613
    PrimitiveResult primitiveLoadFloat64FromBytes(int argCount);     // 614

    // Store into bytes
    PrimitiveResult primitiveStoreBoolean8IntoBytes(int argCount);   // 615
    PrimitiveResult primitiveStoreUInt8IntoBytes(int argCount);      // 616
    PrimitiveResult primitiveStoreInt8IntoBytes(int argCount);       // 617
    PrimitiveResult primitiveStoreUInt16IntoBytes(int argCount);     // 618
    PrimitiveResult primitiveStoreInt16IntoBytes(int argCount);      // 619
    PrimitiveResult primitiveStoreUInt32IntoBytes(int argCount);     // 620
    PrimitiveResult primitiveStoreInt32IntoBytes(int argCount);      // 621
    PrimitiveResult primitiveStoreUInt64IntoBytes(int argCount);     // 622
    PrimitiveResult primitiveStoreInt64IntoBytes(int argCount);      // 623
    PrimitiveResult primitiveStorePointerIntoBytes(int argCount);    // 624
    PrimitiveResult primitiveStoreChar8IntoBytes(int argCount);      // 625
    PrimitiveResult primitiveStoreChar16IntoBytes(int argCount);     // 626
    PrimitiveResult primitiveStoreChar32IntoBytes(int argCount);     // 627
    PrimitiveResult primitiveStoreFloat32IntoBytes(int argCount);    // 628
    PrimitiveResult primitiveStoreFloat64IntoBytes(int argCount);    // 629

    // Load from ExternalAddress
    PrimitiveResult primitiveLoadBoolean8FromExternalAddress(int argCount);  // 630
    PrimitiveResult primitiveLoadUInt8FromExternalAddress(int argCount);     // 631
    PrimitiveResult primitiveLoadInt8FromExternalAddress(int argCount);      // 632
    PrimitiveResult primitiveLoadUInt16FromExternalAddress(int argCount);    // 633
    PrimitiveResult primitiveLoadInt16FromExternalAddress(int argCount);     // 634
    PrimitiveResult primitiveLoadUInt32FromExternalAddress(int argCount);    // 635
    PrimitiveResult primitiveLoadInt32FromExternalAddress(int argCount);     // 636
    PrimitiveResult primitiveLoadUInt64FromExternalAddress(int argCount);    // 637
    PrimitiveResult primitiveLoadInt64FromExternalAddress(int argCount);     // 638
    PrimitiveResult primitiveLoadPointerFromExternalAddress(int argCount);   // 639
    PrimitiveResult primitiveLoadChar8FromExternalAddress(int argCount);     // 640
    PrimitiveResult primitiveLoadChar16FromExternalAddress(int argCount);    // 641
    PrimitiveResult primitiveLoadChar32FromExternalAddress(int argCount);    // 642
    PrimitiveResult primitiveLoadFloat32FromExternalAddress(int argCount);   // 643
    PrimitiveResult primitiveLoadFloat64FromExternalAddress(int argCount);   // 644

    // Store into ExternalAddress
    PrimitiveResult primitiveStoreBoolean8IntoExternalAddress(int argCount);  // 645
    PrimitiveResult primitiveStoreUInt8IntoExternalAddress(int argCount);     // 646
    PrimitiveResult primitiveStoreInt8IntoExternalAddress(int argCount);      // 647
    PrimitiveResult primitiveStoreUInt16IntoExternalAddress(int argCount);    // 648
    PrimitiveResult primitiveStoreInt16IntoExternalAddress(int argCount);     // 649
    PrimitiveResult primitiveStoreUInt32IntoExternalAddress(int argCount);    // 650
    PrimitiveResult primitiveStoreInt32IntoExternalAddress(int argCount);     // 651
    PrimitiveResult primitiveStoreUInt64IntoExternalAddress(int argCount);    // 652
    PrimitiveResult primitiveStoreInt64IntoExternalAddress(int argCount);     // 653
    PrimitiveResult primitiveStorePointerIntoExternalAddress(int argCount);   // 654
    PrimitiveResult primitiveStoreChar8IntoExternalAddress(int argCount);     // 655
    PrimitiveResult primitiveStoreChar16IntoExternalAddress(int argCount);    // 656
    PrimitiveResult primitiveStoreChar32IntoExternalAddress(int argCount);    // 657
    PrimitiveResult primitiveStoreFloat32IntoExternalAddress(int argCount);   // 658
    PrimitiveResult primitiveStoreFloat64IntoExternalAddress(int argCount);   // 659

    // FFI Module/Symbol Loading Primitives (named primitives via primitive 117)
    PrimitiveResult primitiveLoadSymbolFromModule(int argCount);  // Named: load symbol address
    PrimitiveResult primitiveLoadModule(int argCount);            // Named: load module handle

    // FFI Memory Access Primitives (required by TFFIBackend)
    PrimitiveResult primitiveFFIAllocate(int argCount);           // Named: allocate external memory
    PrimitiveResult primitiveFFIFree(int argCount);               // Named: free external memory
    PrimitiveResult primitiveFFIIntegerAt(int argCount);          // Named: read integer at offset
    PrimitiveResult primitiveFFIIntegerAtPut(int argCount);       // Named: write integer at offset
    PrimitiveResult primitiveGetAddressOfOOP(int argCount);       // Named: get address of oop

    // ByteArray data access primitives (600-629)
    // Read/write typed data within byte-format objects
    PrimitiveResult primitiveBytesBoolean8Read(int argCount);     // 600
    PrimitiveResult primitiveBytesUint8Read(int argCount);        // 601
    PrimitiveResult primitiveBytesInt8Read(int argCount);         // 602
    PrimitiveResult primitiveBytesUint16Read(int argCount);       // 603
    PrimitiveResult primitiveBytesInt16Read(int argCount);        // 604
    PrimitiveResult primitiveBytesUint32Read(int argCount);       // 605
    PrimitiveResult primitiveBytesInt32Read(int argCount);        // 606
    PrimitiveResult primitiveBytesUint64Read(int argCount);       // 607
    PrimitiveResult primitiveBytesInt64Read(int argCount);        // 608
    PrimitiveResult primitiveBytesPointerRead(int argCount);      // 609
    PrimitiveResult primitiveBytesChar8Read(int argCount);        // 610
    PrimitiveResult primitiveBytesChar16Read(int argCount);       // 611
    PrimitiveResult primitiveBytesChar32Read(int argCount);       // 612
    PrimitiveResult primitiveFloat32Read(int argCount);           // 613
    PrimitiveResult primitiveFloat64Read(int argCount);           // 614
    PrimitiveResult primitiveBytesBoolean8Write(int argCount);    // 615
    PrimitiveResult primitiveBytesUint8Write(int argCount);       // 616
    PrimitiveResult primitiveBytesInt8Write(int argCount);        // 617
    PrimitiveResult primitiveBytesUint16Write(int argCount);      // 618
    PrimitiveResult primitiveBytesInt16Write(int argCount);       // 619
    PrimitiveResult primitiveBytesUint32Write(int argCount);      // 620
    PrimitiveResult primitiveBytesInt32Write(int argCount);       // 621
    PrimitiveResult primitiveBytesUint64Write(int argCount);      // 622
    PrimitiveResult primitiveBytesInt64Write(int argCount);       // 623
    PrimitiveResult primitiveBytesPointerWrite(int argCount);     // 624
    PrimitiveResult primitiveBytesChar8Write(int argCount);       // 625
    PrimitiveResult primitiveBytesChar16Write(int argCount);      // 626
    PrimitiveResult primitiveBytesChar32Write(int argCount);      // 627
    PrimitiveResult primitiveFloat32Write(int argCount);          // 628
    PrimitiveResult primitiveFloat64Write(int argCount);          // 629

    // ExternalAddress read primitives (numbered 631-639)
    // Read from external memory pointed to by ExternalAddress
    PrimitiveResult primitiveExternalUint8Read(int argCount);     // 631: uint8AtOffset:
    PrimitiveResult primitiveExternalUint16Read(int argCount);    // 633: uint16AtOffset:
    PrimitiveResult primitiveExternalUint32Read(int argCount);    // 635: uint32AtOffset:
    PrimitiveResult primitiveExternalInt32Read(int argCount);     // 636: int32AtOffset:
    PrimitiveResult primitiveExternalPointerRead(int argCount);   // 639: pointerAtOffset:

    // Write to external memory pointed to by ExternalAddress
    PrimitiveResult primitiveExternalUint8Write(int argCount);    // 646: uint8AtOffset:put:
    PrimitiveResult primitiveExternalUint16Write(int argCount);   // 648: uint16AtOffset:put:
    PrimitiveResult primitiveExternalUint32Write(int argCount);   // 650: uint32AtOffset:put:
    PrimitiveResult primitiveExternalInt32Write(int argCount);    // 651: int32AtOffset:put:
    PrimitiveResult primitiveExternalUint64Write(int argCount);   // 652: uint64AtOffset:put:
    PrimitiveResult primitiveExternalPointerWrite(int argCount);  // 654: pointerAtOffset:put:

    // ThreadedFFI (TFFI) Primitives - used by TFFIBackend in Pharo 13+
    PrimitiveResult primitiveFillBasicType(int argCount);            // Named: fill ffi_type* from typeCode
    PrimitiveResult primitiveTypeByteSize(int argCount);             // Named: return ffi_type->size
    PrimitiveResult primitiveDefineFunction(int argCount);           // Named: create ffi_cif via ffi_prep_cif
    PrimitiveResult primitiveFreeDefinition(int argCount);           // Named: free ffi_cif
    PrimitiveResult primitiveDefineVariadicFunction(int argCount);   // Named: create variadic ffi_cif
    PrimitiveResult primitiveGetSameThreadRunnerAddress(int argCount); // Named: return runner address
    PrimitiveResult primitiveSameThreadCallout(int argCount);        // Named: ffi_call via same-thread runner
    PrimitiveResult primitiveCopyFromTo(int argCount);               // Named: memcpy between addr/bytearray
    PrimitiveResult primitiveInitializeStructType(int argCount);     // Named: build ffi_type for struct
    PrimitiveResult primitiveFreeStruct(int argCount);               // Named: free struct ffi_type
    PrimitiveResult primitiveStructByteSize(int argCount);           // Named: struct ffi_type->size
    PrimitiveResult primitiveInitilizeCallbacks(int argCount);       // Named: init callback support (sic)
    PrimitiveResult primitiveReadNextCallback(int argCount);         // Named: read pending callback
    PrimitiveResult primitiveRegisterCallback(int argCount);         // Named: register FFI callback
    PrimitiveResult primitiveUnregisterCallback(int argCount);       // Named: unregister FFI callback
    PrimitiveResult primitiveCallbackReturn(int argCount);           // Named: callback return

    // Sampling profiler — named primitives used by AndreasSystemProfiler.
    PrimitiveResult primitiveProfileSemaphore(int argCount);         // Named: install sem
    PrimitiveResult primitiveProfileStart(int argCount);             // Named: start/stop
    PrimitiveResult primitiveProfileSample(int argCount);            // Named: last process
    PrimitiveResult primitiveProfilePrimitive(int argCount);         // Named: last primitive

    // TFFI helpers (private)
    void* tffi_readAddress(Oop externalAddress);
    void  tffi_writeAddress(Oop externalAddress, void* value);
    void* tffi_getHandler(Oop obj);
    void  tffi_setHandler(Oop obj, void* value);
    Oop   tffi_newExternalAddress(void* ptr);
    void* tffi_getAddressFromExternalAddressOrByteArray(Oop obj);

    // VM info named primitives
    PrimitiveResult primitiveInterpreterSourceVersion(int argCount);  // Named: interpreter source version
    PrimitiveResult primitiveFileMasks(int argCount);                 // Named: FileAttributesPlugin file masks
    PrimitiveResult primitiveFileAttribute(int argCount);             // Named: FileAttributesPlugin file attribute
    PrimitiveResult primitiveFileExists(int argCount);                // Named: FileAttributesPlugin file exists
    PrimitiveResult primitiveOpendir(int argCount);                   // Named: FileAttributesPlugin opendir
    PrimitiveResult primitiveReaddir(int argCount);                   // Named: FileAttributesPlugin readdir
    PrimitiveResult primitiveClosedir(int argCount);                  // Named: FileAttributesPlugin closedir
    PrimitiveResult primitiveRewinddir(int argCount);                 // Named: FileAttributesPlugin rewinddir
    PrimitiveResult primitiveChangeMode(int argCount);               // Named: FileAttributesPlugin changeMode
    PrimitiveResult primitiveChangeOwner(int argCount);              // Named: FileAttributesPlugin changeOwner
    PrimitiveResult primitiveSymlinkChangeOwner(int argCount);       // Named: FileAttributesPlugin symlinkChangeOwner
    PrimitiveResult primitiveFileAttributes(int argCount);           // Named: FileAttributesPlugin file attributes (batch)
    PrimitiveResult primitivePlatToStPath(int argCount);             // Named: FileAttributesPlugin platToStPath
    PrimitiveResult primitiveStToPlatPath(int argCount);             // Named: FileAttributesPlugin stToPlatPath
    PrimitiveResult primitivePathMax(int argCount);                  // Named: FileAttributesPlugin pathMax
    PrimitiveResult primitiveGetenv(int argCount);                    // Named: get environment variable

    // SocketPlugin stubs
    PrimitiveResult primitiveInitializeNetwork(int argCount);         // Named: SocketPlugin
    PrimitiveResult primitiveResolverStatus(int argCount);            // Named: SocketPlugin
    PrimitiveResult primitiveResolverLocalAddress(int argCount);      // Named: SocketPlugin
    PrimitiveResult primitiveResolverStartNameLookup(int argCount);   // Named: SocketPlugin
    PrimitiveResult primitiveResolverNameLookupResult(int argCount);  // Named: SocketPlugin
    PrimitiveResult primitiveResolverAbortLookup(int argCount);       // Named: SocketPlugin

    // UUIDPlugin
    PrimitiveResult primitiveMakeUUID(int argCount);                  // Named: UUIDPlugin

    // SmallFloat primitives (541-559)
    // These are optimized versions of Float primitives for SmallFloat immediates
    // Our Float primitives already handle SmallFloat via extractFloat(), so these delegate
    PrimitiveResult primitiveSmallFloatAdd(int argCount);          // 541
    PrimitiveResult primitiveSmallFloatSubtract(int argCount);     // 542
    PrimitiveResult primitiveSmallFloatLessThan(int argCount);     // 543
    PrimitiveResult primitiveSmallFloatGreaterThan(int argCount);  // 544
    PrimitiveResult primitiveSmallFloatLessOrEqual(int argCount);  // 545
    PrimitiveResult primitiveSmallFloatGreaterOrEqual(int argCount); // 546
    PrimitiveResult primitiveSmallFloatEqual(int argCount);        // 547
    PrimitiveResult primitiveSmallFloatNotEqual(int argCount);     // 548
    PrimitiveResult primitiveSmallFloatMultiply(int argCount);     // 549
    PrimitiveResult primitiveSmallFloatDivide(int argCount);       // 550
    PrimitiveResult primitiveSmallFloatTruncated(int argCount);    // 551
    PrimitiveResult primitiveSmallFloatFractionalPart(int argCount); // 552
    PrimitiveResult primitiveSmallFloatExponent(int argCount);     // 553
    PrimitiveResult primitiveSmallFloatTimesTwoPower(int argCount); // 554
    PrimitiveResult primitiveSmallFloatSquareRoot(int argCount);   // 555
    PrimitiveResult primitiveSmallFloatSine(int argCount);         // 556
    PrimitiveResult primitiveSmallFloatArctan(int argCount);       // 557
    PrimitiveResult primitiveSmallFloatLogN(int argCount);         // 558
    PrimitiveResult primitiveSmallFloatExp(int argCount);          // 559

    // ===== STARTUP SUPPORT =====

    /// Bootstrap startup when active process has nil suspendedContext.
    /// Looks up startup entry point and creates synthetic context.
    bool bootstrapStartup();

    /// Install OSiOSDriver to start the event loop.
    /// Called from step() after the image has had time to initialize.
    void installOSiOSDriver();

    /// Execute pending driver install if scheduled.
    /// Returns true if the install was executed.
    bool executePendingDriverInstall();


    /// Try to reschedule to another runnable process.
    /// Returns true if a process was found and execution continues.
    bool tryReschedule();

    /// Handle forced yield from heartbeat thread (process switching)
    void handleForceYield();

    /// Periodic preemption check - allow other processes to run
    void checkForPreemption();

    /// Mark the current process as terminated (clear suspendedContext)
    void terminateCurrentProcess();

    /// Find a selector symbol by name in global dictionaries.
    /// Returns nil if not found.
    Oop findSelector(const char* name);

    /// Set up interpreter execution state from a Context object.
    bool executeFromContext(Oop context);

    // ===== HELPER METHODS =====

    /// Fetch next bytecode
    inline uint8_t fetchByte() {
        if (__builtin_expect(instructionPointer_ >= bytecodeEnd_, 0))
            return 0x5C;  // returnTop — graceful recovery
        return *instructionPointer_++;
    }

    /// Fetch next 2 bytes as big-endian uint16
    inline uint16_t fetchTwoBytes() {
        uint8_t hi = fetchByte();
        uint8_t lo = fetchByte();
        return (hi << 8) | lo;
    }

    /// Check if value is true/false
    bool isTrue(Oop value) const;
    bool isFalse(Oop value) const;

    /// Get the superclass of a class
    Oop superclassOf(Oop classOop) const;

    /// Get the method dictionary of a class
    Oop methodDictOf(Oop classOop) const;

    /// Look up selector in method dictionary
    Oop lookupInMethodDict(Oop methodDict, Oop selector) const;

    /// Hash for method cache
    size_t cacheHash(Oop selector, Oop classOop) const;

    /// Initialize well-known selectors
    void initializeSelectors();

    // ===== PROCESS SCHEDULING HELPERS =====

    /// Get current active process from scheduler
    Oop getActiveProcess();

    /// Set the active process in scheduler
    void setActiveProcess(Oop process);

    /// Add process to end of a LinkedList
    void addLastLinkToList(Oop process, Oop list);

    /// Remove and return first process from a LinkedList
    Oop removeFirstLinkOfList(Oop list);

    /// Remove specific process from a LinkedList
    bool removeProcessFromList(Oop process, Oop list);

    /// Find and return highest priority runnable process
    Oop wakeHighestPriority();

    /// Find a runnable process at lower priority than the given priority
    Oop wakeLowerPriorityProcess(int currentPriority);

    /// Get process priority safely; returns -1 if corrupted
    int safeProcessPriority(Oop process);

    /// Add process to its priority queue
    void putToSleep(Oop process);

    /// Materialize inline frame stack into context objects
    Oop materializeFrameStack();

    /// Context switch to a different process
    void transferTo(Oop newProcess);

public:
    // ===== FFI CALLBACK SUPPORT =====
    // sigsetjmp/siglongjmp mechanism for C-to-Smalltalk callbacks

    /// Enter interpreter from a C callback (called from callbackClosureHandler)
    void enterInterpreterFromCallback(VMCallbackContext* vmcc);

private:
    /// Re-entry point for callbacks (sigsetjmp target in interpret())
    sigjmp_buf reenterInterpreter_;

    /// Stack of active callback contexts (for nested callbacks)
    static constexpr int MaxCallbackDepth = 16;
    VMCallbackContext* callbackContextStack_[MaxCallbackDepth] = {};
    // Parallel stack: the handler process we transferred to in step 5 of
    // enterInterpreterFromCallback. Used at callback-return time to decide
    // whether to re-queue the current active process: if it's still this
    // handler (handler didn't wait on the sem during cooldown), putToSleep
    // it; if it's something else (handler already transferred away via
    // sem-wait), don't touch the new active.
    Oop callbackHandlerStack_[MaxCallbackDepth] = {};
    int callbackDepth_ = 0;

    /// Deferred callback return: set by primitiveCallbackReturn, consumed by
    /// the nested interpret loop in enterInterpreterFromCallback.
    /// primitiveCallbackReturn returns true to Smalltalk so the image can
    /// release mutexes and clean up. The nested loop then detects this,
    /// restores the original process, and siglongjmps back to C.
    VMCallbackContext* pendingCallbackReturn_ = nullptr;

    /// Signal a semaphore directly by external index (synchronous, not via atomic)
    void signalSemaphoreDirectly(int externalIndex);
};

// ===== TEMPLATE IMPLEMENTATIONS =====

template<typename Visitor>
void Interpreter::forEachRoot(Visitor&& visitor) {
    // Current frame Oops
    visitor(method_);
    visitor(newMethod_);
    visitor(homeMethod_);
    visitor(receiver_);
    visitor(closure_);
    visitor(activeContext_);
    visitor(currentFrameMaterializedCtx_);

    // Pending NLR state (must survive GC during ensure: cleanup)
    visitor(nlrTargetCtx_);
    visitor(nlrEnsureCtx_);
    visitor(nlrHomeMethod_);
    visitor(nlrValue_);
    visitor(lastCannotReturnCtx_);
    visitor(lastCannotReturnProcess_);

    // Per-process saved NLR states (all entries, including process Oops)
    for (int i = 0; i < savedNlrCount_; ++i) {
        visitor(savedNlrStates_[i].process);
        visitor(savedNlrStates_[i].targetCtx);
        visitor(savedNlrStates_[i].ensureCtx);
        visitor(savedNlrStates_[i].homeMethod);
        visitor(savedNlrStates_[i].value);
    }

    // VM state Oops
    visitor(displayForm_);
    visitor(timerSemaphore_);
    visitor(lastKnownTimerSemaphore_);
    visitor(gcTempOop_);
    visitor(pendingWorldMenuMethod_);
    visitor(pendingWorldMenuReceiver_);
    visitor(pendingMenuActionMorph_);
    visitor(pendingMenuActionMethod_);
    visitor(pendingMenuActionArgs_);
    visitor(pendingDriverInstallMethod_);
    visitor(pendingDriverInstallReceiver_);
    visitor(pendingDriverSetupMethod_);
    visitor(pendingDriverSetupReceiver_);

    // Profiler state (may hold Process/Semaphore/Method Oops while
    // primitiveProfileStart is active).
    visitor(profileSemaphore_);
    visitor(profileSample_);
    visitor(profilePrimitive_);

    // Stuck-process detector caches the current active Process by Oop.
    // Without this visit, after scavenge the raw-bits compare in
    // periodic_checks walks a stale young address.
    visitor(trackedProcess_);

    // Callback handler stack — Oops of waiters/handlers during FFI
    // callbacks back into the VM.
    for (int i = 0; i < MaxCallbackDepth; ++i) {
        visitor(callbackHandlerStack_[i]);
    }

    // World renderer roots (menu bar items, dropdown items)
    worldRenderer_.forEachOopRoot(visitor);

    // Operand stack (only the live portion)
    // stackPointer_ points one past the last live value (post-increment push),
    // so use < not <= to avoid scanning the dead slot at stackPointer_.
    for (Oop* p = stackBase_; p < stackPointer_; ++p) {
        visitor(*p);
    }

    // Saved frames
    for (size_t i = 0; i < frameDepth_; ++i) {
        SavedFrame& frame = savedFrames_[i];
        visitor(frame.savedMethod);
        visitor(frame.savedHomeMethod);
        visitor(frame.savedReceiver);
        visitor(frame.savedClosure);
        visitor(frame.savedActiveContext);
        visitor(frame.materializedContext);
    }

    // Method cache
    for (auto& entry : methodCache_) {
        visitor(entry.selector);
        visitor(entry.classOop);
        visitor(entry.method);
    }


    // JIT code zone: compiledMethodOop + selectorOop in each JITMethod header,
    // AND method Oops + selector Oops in inline cache entries.
    // These must be GC roots so (a) referenced objects aren't collected
    // and (b) Oops are updated in-place when compaction moves objects.
    // This keeps IC entries valid across GC, avoiding a full IC flush.
#if PHARO_JIT_ENABLED
    if (jitRuntime_.isInitialized()) {
        // Ensure the whole code zone is writable before updating Oop fields.
        // updatePointersAfterCompact rewrites method header and IC Oops in
        // place; if the zone is left executable after a JIT call path that
        // didn't flip it back, the store here crashes with SIGSEGV inside
        // fullGC. See docs/deferred.md A1.
        jit::makeWritable(jitRuntime_.codeZone().rawStart(),
                          jitRuntime_.codeZone().totalBytes());
        jit::JITMethod* m = jitRuntime_.codeZone().firstMethod();
        while (m) {
            if (m->compiledMethodOop != 0) {
                Oop& methodOop = *reinterpret_cast<Oop*>(&m->compiledMethodOop);
                visitor(methodOop);
                // Refresh the bcStart cache from the (possibly moved) oop.
                // The xmethod inline-J2J emit loads state.ip from
                // JITMethod::bcStartCache (AsmjitT1 callee-setup + push
                // paths); the cache was set once at compile time, so a
                // scavenge/compaction that moves the CompiledMethod (e.g.
                // a fresh eden DoIt) left it pointing at the OLD address
                // — materialized frames then resumed interpreting garbage
                // bytes.  Recompute is idempotent (mark phases leave the
                // oop unchanged) and the zone is writable here (above).
                m->bcStartCache = m->compiledMethodOop
                    + (2 + (m->methodHeader & 0x7FFF)) * 8;
            }
            if (m->selectorOop != 0) {
                Oop& selOop = *reinterpret_cast<Oop*>(&m->selectorOop);
                visitor(selOop);
            }
            // Visit IC entries. Layout from JITMethod.hpp IC_BYTES_PER_SITE:
            //   [key, method, extra] * IC_ENTRIES_PER_SITE + selectorBits
            //   key = classIndex (stable, not an Oop)
            //   method = CompiledMethod Oop (needs GC update)
            //   extra = flags + slot/address (not an Oop)
            //   selectorBits = Symbol Oop (needs GC update)
            // `numICEntries` is the SITE count (yes, misnamed), sized as
            // numSites * IC_BYTES_PER_SITE in the code zone.
            if (m->numICEntries > 0 && m->icBuffer) {
                // 2026-05-03: IC zone moved out of MAP_JIT to heap-side
                // `m->icBuffer` (calloc'd by CodeZone::allocate).  The
                // previous code computed icStart as
                // `m->codeStart() + m->codeSize - numICEntries*IC_BYTES_PER_SITE`
                // — that formula matched the OLD in-codezone IC layout
                // but now reads stray bytes from the machine-code tail,
                // not the IC.  Worse, GC writes (visitor updates oop
                // bits in place) corrupted the selBitsArray that lives
                // at the end of the JIT method's payload, producing
                // bogus sendSel reads at chain-loop ExitSend dispatch
                // (the f138 #linkTo:-instead-of-#push bug, root-caused
                // 2026-05-13).
                uint8_t* icStart = reinterpret_cast<uint8_t*>(m->icBuffer);
                for (uint32_t i = 0; i < m->numICEntries; i++) {
                    uint64_t* slots = reinterpret_cast<uint64_t*>(
                        icStart + i * jit::IC_BYTES_PER_SITE);
                    for (uint32_t e = 0; e < jit::IC_ENTRIES_PER_SITE; e++) {
                        uint64_t& methodBits = slots[e * 3 + 1];
                        if (methodBits != 0) {
                            Oop& mOop = *reinterpret_cast<Oop*>(&methodBits);
                            visitor(mOop);
                        }
                    }
                    uint64_t& selBits = slots[jit::IC_SELBITS_SLOT];
                    if (selBits != 0) {
                        Oop& sOop = *reinterpret_cast<Oop*>(&selBits);
                        visitor(sOop);
                    }
                }
                // Task #41: side-channel selBits array.  Each u64 is a
                // selector Oop that needs GC update so the megacache miss
                // path sees live pointers after compaction.
                uint64_t* sba = m->selBitsArray();
                if (sba) {
                    for (uint32_t i = 0; i < m->numICEntries; i++) {
                        if (sba[i] != 0) {
                            Oop& sOop = *reinterpret_cast<Oop*>(&sba[i]);
                            visitor(sOop);
                        }
                    }
                }
            }
            m = m->nextInZone;
        }

        // Count map: keys are CompiledMethod Oop bits (needs GC update).
        // Preserving counts across GC lets methods accumulate toward the
        // compile threshold instead of resetting to zero every GC.
        for (size_t i = 0; i < jit::CountMapSize; i++) {
            auto& entry = jitRuntime_.countMapEntry(i);
            if (entry.key != 0) {
                Oop& keyOop = *reinterpret_cast<Oop*>(&entry.key);
                visitor(keyOop);
            }
        }

        // Initial-compile-failed negative cache: same treatment as the
        // count map (keys are CompiledMethod/Block oop bits; updated here,
        // rehashed in recoverAfterGC).  Persisting entries across GC is
        // what stops the per-GC asmjit-pipeline retry thrash on
        // un-compilable methods (1.4M+ failed attempts per SUnit batch
        // before 2026-06-10).
        for (size_t i = 0; i < jit::JITRuntime::FailedMapSize; i++) {
            uint64_t& key = jitRuntime_.initialCompileFailedKey(i);
            if (key != 0) {
                Oop& keyOop = *reinterpret_cast<Oop*>(&key);
                visitor(keyOop);
            }
        }

        // Shadow-slot detector (PHARO_SHADOW_SLOTS): entry obj keys and
        // tracked values are oops — GC must update them or every moved
        // object would false-positive on its next verified read.
        // Positions rehash in afterGC.
        if (GET_DEBUG_BOOL(PHARO_SHADOW_SLOTS)) {
            for (size_t i = 0; i < kShadowSize; i++) {
                ShadowEntry& e = g_shadowSlots[i];
                if (e.obj == 0) continue;
                visitor(*reinterpret_cast<Oop*>(&e.obj));
                // Values can be immediates — visitor implementations
                // skip non-objects themselves.
                visitor(*reinterpret_cast<Oop*>(&e.value));
            }
        }

        // Tier 2 map: keys are CompiledMethod Oop bits (needs GC update).
        for (size_t i = 0; i < jit::JITRuntime::Tier2MapSize; i++) {
            auto& entry = jitRuntime_.tier2Entry(i);
            if (entry.key != 0) {
                Oop& keyOop = *reinterpret_cast<Oop*>(&entry.key);
                visitor(keyOop);
            }
        }
    }

    // Live JITState: its Oop fields aren't reachable any other way while
    // JIT code is on the C stack. Without this, GC compaction would move
    // state.receiver/method/cachedTarget out from under the JIT, leaving
    // stale pointers that crash on next dereference.
    if (currentJITState_) {
        visitor(currentJITState_->receiver);
        visitor(currentJITState_->method);
        visitor(currentJITState_->returnValue);
        visitor(currentJITState_->cachedTarget);
        // trueOop/falseOop are visited via specialObjects_ already.

        // J2J pool: pending save structs hold caller-side Oops (receiver)
        // that the GC must update.  Without this scan, xmethod-pushed
        // saves accumulate stale receiver refs across GC; pop later
        // restores a stale oop, leading to nil-receiver DNUs.  Root-
        // caused 2026-05-19 (deferred A6 iter N+15): the sharp 32K
        // xmethod corruption threshold corresponded to the cumulative
        // GC frequency × bench's xmethod fire rate.
        for (int i = 0; i < j2jPoolCursor_; i++) {
            visitor(j2jPool_[i].receiver);
        }

        // Session H: BV-inline closure side-stack must be walked so
        // saved closure Oops survive a GC that fires inside the
        // BV-inlined block's body (e.g., on an allocating send).
        for (int i = 0; i < bvClosureSaveDepth_; i++) {
            visitor(bvClosureSaveStack_[i]);
        }

        // jit-may22a B1 Sub-step 5: Sista inline-self save pool also
        // holds caller-side receivers across recursive calls.  Live
        // save-stack lives in stack-local memory of tryJITActivation
        // — reachable via currentJITState_'s sistaSaveCursor.
        if (currentJITState_
                && currentJITState_->sistaSaveCursor
                && currentJITState_->sistaSaveLimit) {
            // The pool extends from base to sistaSaveCursor (one past
            // last live entry).  We don't have the base pointer
            // directly, but sistaSaveLimit - 256*56 gives it.
            jit::SistaSave* poolEnd = reinterpret_cast<jit::SistaSave*>(
                currentJITState_->sistaSaveLimit);
            jit::SistaSave* poolBase = poolEnd - jit::MaxSistaSavePoolSize;
            jit::SistaSave* poolTop = reinterpret_cast<jit::SistaSave*>(
                currentJITState_->sistaSaveCursor);
            for (jit::SistaSave* s = poolBase; s < poolTop; s++) {
                visitor(s->receiver);
            }
        }
        // sp/tempBase/literals are recomputed in afterGC from updated method.
    }
#endif

    // Well-known selectors
    visitor(selectors_.doesNotUnderstand);
    visitor(selectors_.mustBeBoolean);
    visitor(selectors_.cannotReturn);
    visitor(selectors_.aboutToReturn);
    visitor(selectors_.run);
    visitor(selectors_.value);
    visitor(selectors_.value_);
    visitor(selectors_.valueValue);
    visitor(selectors_.add);
    visitor(selectors_.subtract);
    visitor(selectors_.lessThan);
    visitor(selectors_.greaterThan);
    visitor(selectors_.lessEqual);
    visitor(selectors_.greaterEqual);
    visitor(selectors_.equal);
    visitor(selectors_.notEqual);
    visitor(selectors_.multiply);
    visitor(selectors_.divide);
    visitor(selectors_.at);
    visitor(selectors_.atPut);
    visitor(selectors_.size);
    visitor(selectors_.next);
    visitor(selectors_.nextPut);
    visitor(selectors_.atEnd);
    visitor(selectors_.eq);
    visitor(selectors_.class_);
    visitor(selectors_.new_);
    visitor(selectors_.newSize);

#if PHARO_JIT_ENABLED
    // Restore MAP_JIT to executable for this thread.  Apple Silicon
    // `pthread_jit_write_protect_np` is a per-thread toggle that affects
    // every MAP_JIT page; leaving it writable here means JIT entry after
    // GC faults with SIGBUS (fault_addr == PC in code zone).  The earlier
    // `makeWritable` in this function must be balanced.
    if (jitRuntime_.isInitialized()) {
        jit::makeExecutable(jitRuntime_.codeZone().rawStart(),
                            jitRuntime_.codeZone().totalBytes());
    }
#endif
}

} // namespace pharo

#endif // PHARO_INTERPRETER_HPP
