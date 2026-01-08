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
#include <array>
#include <cstdint>
#include <functional>

namespace pharo {

/// Maximum stack depth
constexpr size_t MaxStackDepth = 4096;

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

    /// Execute a single bytecode (for debugging)
    bool step();

    /// Stop the interpreter
    void stop() { running_ = false; }

    /// Get the object memory
    ObjectMemory& memory() { return memory_; }

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

    // ===== PRIMITIVE SUPPORT =====

    /// Set the primitive result (success)
    void primitiveSuccess(Oop result);

    /// Mark primitive as failed
    void primitiveFail();

    /// Check if primitive succeeded
    bool primitiveSucceeded() const { return !primitiveFailed_; }

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
        Oop* savedFP;
        int savedArgCount;
    };
    static constexpr size_t MaxFrameDepth = 512;
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
    Oop method_;            // Current method or CompiledBlock being executed
    Oop homeMethod_;        // Home CompiledMethod (for literal access in blocks)
    Oop receiver_;
    Oop activeContext_;  // Current Smalltalk context (for sender chain)
    int argCount_;

    // Sista V1 extension bytes (reset after each instruction)
    int extA_;  // Extension A - modifies literal/temp index
    int extB_;  // Extension B - modifies numArgs/other

    // Bytecode set detection (method header bit 31: 0=V3PlusClosures, 1=SistaV1)
    bool usesSistaV1_;

    // Execution control
    bool running_;
    bool primitiveFailed_;

    // ===== CACHES =====

    std::array<MethodCacheEntry, MethodCacheSize> methodCache_;
    WellKnownSelectors selectors_;
    std::array<PrimitiveFunc, 576> primitiveTable_;

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
    void sendLiteralZeroArgs(int literalIndex);
    void sendLiteralOneArg(int literalIndex);
    void sendLiteralTwoArgs(int literalIndex);
    void sendSelector(Oop selector, int argCount);

    // Special operations
    void duplicateTop();
    void popStack();
    void createBlock();
    void createFullBlock();

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

    /// Send mustBeBoolean
    void sendMustBeBoolean(Oop value);

    // ===== FRAME MANAGEMENT =====

    /// Create a new stack frame
    void pushFrame(Oop method, int argCount);

    /// Pop the current stack frame
    void popFrame();

    /// Get temporary variable
    Oop temporary(int index) const;

    /// Set temporary variable
    void setTemporary(int index, Oop value);

    /// Get argument
    Oop argument(int index) const;

    // ===== PRIMITIVE DISPATCH =====

    /// Initialize the primitive table
    void initializePrimitives();

    /// Execute a primitive
    PrimitiveResult executePrimitive(int primitiveIndex, int argCount);

    /// Get primitive index from method
    int primitiveIndexOf(Oop method) const;

    // ===== PRIMITIVE IMPLEMENTATIONS =====
    // (See Primitives.cpp for implementations)

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

    // Behavior
    PrimitiveResult primitivePerform(int argCount);
    PrimitiveResult primitivePerformWithArgs(int argCount);

    // Control
    PrimitiveResult primitiveBlockValue(int argCount);
    PrimitiveResult primitiveBlockValueWithArgs(int argCount);

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

    // I/O (stubs - iOS-specific implementation elsewhere)
    PrimitiveResult primitiveMousePoint(int argCount);
    PrimitiveResult primitiveMouseButtons(int argCount);
    PrimitiveResult primitiveKeyboardNext(int argCount);
    PrimitiveResult primitiveBeDisplay(int argCount);
    PrimitiveResult primitiveForceDisplayUpdate(int argCount);

    // Time
    PrimitiveResult primitiveMillisecondClock(int argCount);
    PrimitiveResult primitiveSecondsClock(int argCount);
    PrimitiveResult primitiveMicrosecondClock(int argCount);
    PrimitiveResult primitiveLocalMicrosecondClock(int argCount);
    PrimitiveResult primitiveSignalAtMilliseconds(int argCount);

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
    PrimitiveResult primitiveFloatTruncated(int argCount);    // 51
    PrimitiveResult primitiveFractionalPart(int argCount);    // 52
    PrimitiveResult primitiveExponent(int argCount);          // 53
    PrimitiveResult primitiveTimesTwoPower(int argCount);     // 54
    PrimitiveResult primitiveFloatSquareRoot(int argCount);   // 55
    PrimitiveResult primitiveFloatSin(int argCount);          // 56
    PrimitiveResult primitiveFloatArctan(int argCount);       // 57
    PrimitiveResult primitiveFloatLn(int argCount);           // 58
    PrimitiveResult primitiveFloatExp(int argCount);          // 59

    // Point creation
    PrimitiveResult primitiveMakePoint(int argCount);

    // Large integers (21-37)
    PrimitiveResult primitiveLargeIntegerAdd(int argCount);       // 21
    PrimitiveResult primitiveLargeIntegerSubtract(int argCount);  // 22
    PrimitiveResult primitiveLargeIntegerLessThan(int argCount);  // 23
    PrimitiveResult primitiveLargeIntegerGreaterThan(int argCount); // 24
    PrimitiveResult primitiveLargeIntegerLessOrEqual(int argCount); // 25
    PrimitiveResult primitiveLargeIntegerGreaterOrEqual(int argCount); // 26
    PrimitiveResult primitiveLargeIntegerEqual(int argCount);     // 27
    PrimitiveResult primitiveLargeIntegerNotEqual(int argCount);  // 28
    PrimitiveResult primitiveLargeIntegerMultiply(int argCount);  // 29
    PrimitiveResult primitiveLargeIntegerDivide(int argCount);    // 30
    PrimitiveResult primitiveLargeIntegerMod(int argCount);       // 31
    PrimitiveResult primitiveLargeIntegerDiv(int argCount);       // 32
    PrimitiveResult primitiveLargeIntegerQuo(int argCount);       // 33
    PrimitiveResult primitiveLargeIntegerBitAnd(int argCount);    // 34
    PrimitiveResult primitiveLargeIntegerBitOr(int argCount);     // 35
    PrimitiveResult primitiveLargeIntegerBitXor(int argCount);    // 36
    PrimitiveResult primitiveLargeIntegerBitShift(int argCount);  // 37

    // GC primitives
    PrimitiveResult primitiveFullGC(int argCount);

    // Utility primitives
    PrimitiveResult primitiveFlushCache(int argCount);       // 89
    PrimitiveResult primitiveBytesLeft(int argCount);        // 112
    PrimitiveResult primitiveSpecialObjectsOop(int argCount); // 129

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
    PrimitiveResult primitiveThisContext(int argCount);      // 199
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

    // Bit operation primitives
    PrimitiveResult primitiveHighBit(int argCount);          // 575
    PrimitiveResult primitiveLowBit(int argCount);           // 576

    // Word array access primitives
    PrimitiveResult primitiveIntegerAt(int argCount);        // 165
    PrimitiveResult primitiveIntegerAtPut(int argCount);     // 166

    // Class/behavior primitives
    PrimitiveResult primitiveBehaviorHash(int argCount);     // 175
    PrimitiveResult primitiveChangeClass(int argCount);      // 115

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
    PrimitiveResult primitiveGrowMemory(int argCount);       // 180
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

    // ===== STARTUP SUPPORT =====

    /// Bootstrap startup when active process has nil suspendedContext.
    /// Looks up startup entry point and creates synthetic context.
    bool bootstrapStartup();

    /// Try to reschedule to another runnable process.
    /// Returns true if a process was found and execution continues.
    bool tryReschedule();

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

    /// Add process to its priority queue
    void putToSleep(Oop process);

    /// Context switch to a different process
    void transferTo(Oop newProcess);
};

} // namespace pharo

#endif // PHARO_INTERPRETER_HPP
