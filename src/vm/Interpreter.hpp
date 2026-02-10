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
 *
 *   0-15:    Push receiver variable 0-15
 *   16-31:   Push temporary 0-15
 *   32-63:   Push literal constant 0-31
 *   64-95:   Push literal variable 0-31
 *   96-103:  Pop and store receiver variable 0-7
 *   104-111: Pop and store temporary 0-7
 *   112-119: Push special (receiver, true, false, nil, -1, 0, 1, 2)
 *   120-127: Return (receiver, true, false, nil, top, block)
 *   128-143: Extended push (2 bytes)
 *   144-159: Extended store (2 bytes)
 *   160-167: Pop and store into receiver variable (extended)
 *   168-175: Pop and store into temporary (extended)
 *   176-191: Arithmetic send (+, -, <, >, <=, >=, =, ~=, *, /, \\, @, etc.)
 *   192-207: Common sends (at:, at:put:, size, next, nextPut:, etc.)
 *   208-223: Send literal selector 0-15 with 0 args
 *   224-239: Send literal selector 0-15 with 1 arg
 *   240-255: Extended sends and jumps (2-3 bytes)
 *
 * METHOD CACHE:
 *
 *   A hash table caching (selector, class) -> method lookups.
 *   Dramatically speeds up message sends.
 */

#ifndef PHARO_INTERPRETER_HPP
#define PHARO_INTERPRETER_HPP

#include "ObjectMemory.hpp"
#include "../platform/EventQueue.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <thread>
#include <vector>

namespace pharo {

/// Maximum stack depth
constexpr size_t MaxStackDepth = 131072;  // Must be large enough for MaxFrameDepth frames

/// Method cache size (must be power of 2)
constexpr size_t MethodCacheSize = 2048;

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
public:
    explicit Interpreter(ObjectMemory& memory);

    /// Initialize the interpreter with a loaded image
    bool initialize();

    /// Run the interpreter main loop
    void interpret();
    void stopVM(const char* reason);
    void dumpProcessQueues();
    void dumpCurrentMethod();

    /// Execute a single bytecode (for debugging)
    bool step();

    /// Execute a single bytecode with detailed result
    ExecuteResult stepDetailed();

    /// Stop the interpreter
    void stop() { running_ = false; }
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

    /// Convert IP offsets back to pointers after GC (methods may have moved)
    void afterGC();

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

    /// Set/get screen dimensions
    void setScreenSize(int width, int height) { screenWidth_ = width; screenHeight_ = height; }
    void setScreenDepth(int depth) { screenDepth_ = depth; }
    int screenWidth() const { return screenWidth_; }
    int screenHeight() const { return screenHeight_; }
    Oop displayForm() const { return displayForm_; }
    void setDisplayForm(Oop form) { displayForm_ = form; }
    void initializeDisplayForm();  // Create and set up display Form
    void renderWorldMorphs();      // Direct rendering of World's morphs
    void processInputEvents();     // Process pending input events
    void dispatchMouseEventToMorph(int x, int y, int buttons, bool isMouseDown); // Direct mouse dispatch
    void handleMenuBarClick(Oop menuBar, int x, int y, int buttons); // Handle menu bar click
    void handleWorldMenuClick(Oop world, int x, int y); // Handle world menu on right-click
    void executeMenuItemAction(Oop itemMorph); // Execute a dropdown menu item's action
    void processPendingMenuAction(); // Process pending menu item action
    Oop lookupMethodByName(Oop classObj, const char* selectorName); // Find method by name
    void processPendingWorldMenu(); // Execute queued world menu invocation
    void drawClickIndicator(int x, int y, int buttons); // Draw visible click feedback
    // NOTE: updateActiveHandPosition() REMOVED - was a workaround for missing InputEventSensor process
    void syncDisplayToSurface();   // Copy Display Form to platform surface
    void ensureDisplayForm(int width, int height, int depth);  // Create Form and bind to Display global
    int screenDepth() const { return screenDepth_; }

    /// Get current execution state
    Oop activeContext() const;
    Oop activeMethod() const { return method_; }
    Oop receiver() const { return receiver_; }

    // ===== STACK ACCESS (for primitives) =====

    /// Push a value onto the stack
    void push(Oop value);

    /// Pop a value from the stack
    Oop pop();

    /// Peek at stack top without popping
    Oop stackTop() const;

    /// Get stack value at offset from top (0 = top)
    Oop stackValue(size_t offset) const;

    /// Pop multiple values
    void popN(size_t n);

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
    bool hasPendingSignals() const { return pendingSignalIndex_ > 0; }

    /// Process any pending external semaphore signals (called during interpret loop)
    void processPendingSignals();

    /// Check timer semaphore and signal if time has elapsed
    void checkTimerSemaphore();

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
        int savedArgCount;
        size_t homeFrameDepth;  // For non-local block returns: the frame to return to (SIZE_MAX = not a block)
        // GC: IP offsets (set by prepareForGC, read by afterGC)
        ptrdiff_t savedIPOffset;
        ptrdiff_t savedBytecodeEndOffset;
    };
    static constexpr size_t MaxFrameDepth = 65536;
    static constexpr size_t StackOverflowLimit = 35000;  // Graceful overflow limit (MaxFrameDepth is hard array bound)
    std::array<SavedFrame, MaxFrameDepth> savedFrames_;
    size_t frameDepth_;

    // Stack (single stack for all frames)
    std::array<Oop, MaxStackDepth> stack_;
    Oop* stackPointer_;
    Oop* stackBase_;

    // Current frame
    Oop* framePointer_;
    uint8_t* instructionPointer_;
    uint8_t* bytecodeEnd_;  // End of bytecodes in current method
    uint8_t lastBytecode_ = 0;  // Last bytecode dispatched (for stack overflow diagnosis)
    // GC: IP offsets for current frame (set by prepareForGC, read by afterGC)
    ptrdiff_t ipOffset_ = 0;
    ptrdiff_t bytecodeEndOffset_ = 0;
    Oop method_;            // Current method or CompiledBlock being executed
    Oop newMethod_;         // Method about to be activated (for primitive 117 to read literals)
    Oop homeMethod_;        // Home CompiledMethod (for literal access in blocks)
    Oop receiver_;
    Oop closure_;        // Current FullBlockClosure if executing a block, nil for methods
    Oop activeContext_;  // Current Smalltalk context (for sender chain)
    Oop currentFrameMaterializedCtx_;  // Cached context for current frame (reused across materialize calls)
    int argCount_;

    // Sista V1 extension bytes (reset after each instruction)
    int extA_;  // Extension A - modifies literal/temp index
    int extB_;  // Extension B - modifies numArgs/other

    // Bytecode set detection (method header bit 31: 0=V3PlusClosures, 1=SistaV1)
    bool usesSistaV1_;

    // Execution control
    bool running_;
    bool primitiveFailed_;
    bool suppressContextSwitch_ = false;  // Suppress forceYield after prim 198 (ensure:) activation
    bool inExtension_ = false;  // True after extension byte (0xE0/0xE1), prevents forceYield from splitting extension+target
    int lastPrimitiveIndex_ = 0;  // For stepDetailed() tracking

    // System paths and arguments
    std::string imageName_;
    std::string vmPath_;
    std::vector<std::string> imageArguments_;  // Command-line args for the image (index 2+)
    std::vector<std::string> vmParameters_;    // VM flags like --headless (negative indices)

    // Screen dimensions (configurable, defaults for headless)
    int screenWidth_ = 1024;
    int screenHeight_ = 768;
    int screenDepth_ = 32;

    // Display Form (the Smalltalk Form that represents the screen)
    Oop displayForm_ = Oop::nil();

    // Mouse tracking for event handling
    int lastMouseX_ = 0;
    int lastMouseY_ = 0;
    int lastMouseButtons_ = 0;
    int lastMouseEventType_ = 0;  // 0=move, 1=down, 2=up

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

    // Menu interaction state (for direct menu handling)
    int selectedMenuIndex_ = -1;  // -1 = no menu selected
    int prevSelectedMenuIndex_ = -1;  // Previous frame's menu state for dirty tracking
    int64_t lastMenuClickTime_ = 0;  // Debounce duplicate clicks
    int lastMenuClickX_ = -1000;     // Last click X coordinate
    int lastMenuClickY_ = -1000;     // Last click Y coordinate
    std::vector<std::pair<int, int>> menuItemBounds_;  // Stored menu item X bounds (start, end)
    std::vector<Oop> menuBarItemMorphs_;  // The actual menu bar item morphs for dropdown access
    int menuBarTop_ = 28;    // Top of menu bar in pixels (default for non-Retina)
    int menuBarBottom_ = 72; // Bottom of menu bar in pixels (default for non-Retina)
    int menuBarScale_ = 1;   // Scale factor (2 for Retina)

    // Dropdown menu state for click handling
    struct DropdownState {
        int x = 0, y = 0, width = 0, height = 0;  // Dropdown bounds
        int lineHeight = 0;
        std::vector<Oop> itemMorphs;  // The actual menu item morphs for action invocation
        bool valid = false;
        int64_t openTimeMs = 0;  // When dropdown became valid (for debouncing)
    };
    DropdownState dropdownState_;

    // World menu state (for right-click context menu)
    struct WorldMenuBounds {
        int x = 0, y = 0, width = 0, height = 0;
    };
    WorldMenuBounds pendingMenuBounds_;
    bool hasVisibleMenu_ = false;

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

    // NOTE: Event injection workaround member variables REMOVED
    // Events must go through proper Smalltalk InputEventSensor process, not C++ workarounds

    // Debug: visual click indicator
    int debugClickX_ = -1;
    int debugClickY_ = -1;
    int debugClickFrame_ = 0;  // Frame counter for fade-out

    // Dirty rectangle tracking for efficient redraws
    struct DirtyRect {
        int x1, y1, x2, y2;
        bool valid = false;
    };
    DirtyRect dirtyMenuDropdown_;  // Track dropdown area changes

    // Pass-through events (events not handled by processInputEvents, passed to Pharo)
    std::vector<pharo::Event> passThroughEvents_;

    // External semaphore signaling (for I/O events)
    // Simple approach: store one pending signal index, process in interpret loop
    std::atomic<int> pendingSignalIndex_{0};

    // Force yield flag - set by heartbeat to preempt long-running processes
    std::atomic<bool> forceYield_{false};

    // Timer/delay semaphore (for Delay class)
    Oop timerSemaphore_ = Oop::nil();
    int64_t nextWakeupTime_ = 0;  // 0 means no timer set (in ioMSecs units)
    int64_t nextWakeupUsec_ = INT64_MAX;  // UTC microsecond wakeup (for primitive 242)

    // Low space threshold for GC (bytes) - signals TheLowSpaceSemaphore when free < threshold
    size_t lowSpaceThreshold_ = 0;

    // VM start time for ioMSecs() - millisecond clock base
    std::chrono::steady_clock::time_point vmStartTime_ = std::chrono::steady_clock::now();

    // Heartbeat thread
    std::atomic<bool> heartbeatRunning_{false};
    std::atomic<bool> pendingDisplaySync_{false};
    std::thread heartbeatThread_;

    // Clipboard (simple in-memory storage for headless mode)
    std::string clipboardText_;

    // File handles (maps Smalltalk file IDs to FILE pointers)
    std::map<int, FILE*> openFiles_;
    int nextFileId_ = 1;

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

    /// Clear the method cache (used when methods are modified)
    void flushMethodCache() {
        for (auto& entry : methodCache_) {
            entry.selector = Oop::nil();
            entry.classOop = Oop::nil();
            entry.method = Oop::nil();
        }
    }

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

    /// Send doesNotUnderstand:
    void sendDoesNotUnderstand(Oop selector, int argCount);

    /// Invoke a non-CompiledMethod object as a method (sends #run:with:in:)
    void invokeObjectAsMethod(Oop nonMethod, Oop selector, int argCount);

    /// Send mustBeBoolean
    void sendMustBeBoolean(Oop value);

    // ===== FRAME MANAGEMENT =====

    /// Create a new stack frame
    bool pushFrame(Oop method, int argCount);  // Returns false if recursion detected

    /// Pop the current stack frame
    void popFrame();

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

    /// Get primitive index from method
    int primitiveIndexOf(Oop method) const;

    // ===== PRIMITIVE IMPLEMENTATIONS =====
    // (See Primitives.cpp for implementations)

    // Stub primitives (always fail or succeed with no-op)
    PrimitiveResult primitiveFailure(int argCount);
    PrimitiveResult primitiveNoop(int argCount);
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

    // Time/Timezone primitives (242-246)
    PrimitiveResult primitiveSignalAtUTCMicroseconds(int argCount);  // 242
    PrimitiveResult primitiveUpdateTimezone(int argCount);           // 243
    PrimitiveResult primitiveUtcAndTimezoneOffset(int argCount);     // 244
    PrimitiveResult primitiveCoarseUTCMicrosecondClock(int argCount); // 245
    PrimitiveResult primitiveCoarseLocalMicrosecondClock(int argCount); // 246

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

    // SurfacePlugin named primitives (for OSSDL2ExternalForm)
    PrimitiveResult primitiveCreateManualSurface(int argCount);
    PrimitiveResult primitiveDestroyManualSurface(int argCount);
    PrimitiveResult primitiveSetManualSurfacePointer(int argCount);

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
    PrimitiveResult primitiveBecomeForward(int argCount);    // 128
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
    PrimitiveResult primitiveGetenv(int argCount);                    // Named: get environment variable

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

    /// Auto-load OSiOSDriver.st by evaluating Smalltalk code
    /// Called once at startup to enable the event system
    bool autoLoadDriver();

    /// Try to reschedule to another runnable process.
    /// Returns true if a process was found and execution continues.
    bool tryReschedule();

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
    uint8_t fetchByte();

    /// Fetch next 2 bytes as big-endian uint16
    uint16_t fetchTwoBytes();

    /// Check if value is true/false
    bool isTrue(Oop value) const;
    bool isFalse(Oop value) const;

    /// Get the superclass of a class
    Oop superclassOf(Oop classOop) const;

    /// Get the class where a CompiledMethod is defined (from last literal)
    /// This is critical for super sends which must lookup from method's defining class
    Oop methodClassOf(Oop method) const;

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

    /// Add process to its priority queue
    void putToSleep(Oop process);

    /// Materialize inline frame stack into context objects
    Oop materializeFrameStack();

    /// Context switch to a different process
    void transferTo(Oop newProcess);
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

    // VM state Oops
    visitor(displayForm_);
    visitor(timerSemaphore_);
    visitor(pendingWorldMenuMethod_);
    visitor(pendingWorldMenuReceiver_);
    visitor(pendingMenuActionMorph_);
    visitor(pendingMenuActionMethod_);
    visitor(pendingMenuActionArgs_);
    visitor(pendingDriverInstallMethod_);
    visitor(pendingDriverInstallReceiver_);
    visitor(pendingDriverSetupMethod_);
    visitor(pendingDriverSetupReceiver_);

    // Menu bar item morphs
    for (auto& morph : menuBarItemMorphs_) {
        visitor(morph);
    }

    // Dropdown item morphs
    for (auto& morph : dropdownState_.itemMorphs) {
        visitor(morph);
    }

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
}

} // namespace pharo

#endif // PHARO_INTERPRETER_HPP
