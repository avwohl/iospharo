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
#include <cstdio>
#include <functional>
#include <map>

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

    /// Set/get system paths
    void setImageName(const std::string& name) { imageName_ = name; }
    void setVMPath(const std::string& path) { vmPath_ = path; }
    const std::string& imageName() const { return imageName_; }
    const std::string& vmPath() const { return vmPath_; }

    /// Set/get screen dimensions
    void setScreenSize(int width, int height) { screenWidth_ = width; screenHeight_ = height; }
    void setScreenDepth(int depth) { screenDepth_ = depth; }
    int screenWidth() const { return screenWidth_; }
    int screenHeight() const { return screenHeight_; }
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

    // System paths
    std::string imageName_;
    std::string vmPath_;

    // Screen dimensions (configurable, defaults for headless)
    int screenWidth_ = 1024;
    int screenHeight_ = 768;
    int screenDepth_ = 32;

    // Clipboard (simple in-memory storage for headless mode)
    std::string clipboardText_;

    // File handles (maps Smalltalk file IDs to FILE pointers)
    std::map<int, FILE*> openFiles_;
    int nextFileId_ = 1;

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
    PrimitiveResult primitiveClosureValueUnwind(int argCount);   // 208
    PrimitiveResult primitiveClosureValueNoUnwind(int argCount); // 209

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
    PrimitiveResult primitiveSnapshotEmbedded(int argCount);     // 109

    // I/O (stubs - iOS-specific implementation elsewhere)
    PrimitiveResult primitiveMousePoint(int argCount);
    PrimitiveResult primitiveMouseButtons(int argCount);
    PrimitiveResult primitiveKeyboardNext(int argCount);
    PrimitiveResult primitiveScreenSize(int argCount);           // 106
    PrimitiveResult primitiveScreenDepth(int argCount);          // 108
    PrimitiveResult primitiveBeep(int argCount);                 // 140
    PrimitiveResult primitiveClipboardText(int argCount);        // 141
    PrimitiveResult primitiveForceDisplayUpdate(int argCount);

    // System primitives (152-155)
    PrimitiveResult primitiveSetFullScreen(int argCount);          // 152
    PrimitiveResult primitiveInputSemaphore(int argCount);         // 153
    PrimitiveResult primitiveInputWord(int argCount);              // 154
    PrimitiveResult primitiveCompareString(int argCount);          // 155

    // FFI/External primitives (116-118, 147)
    PrimitiveResult primitiveFlushExternalPrimitives(int argCount); // 116
    PrimitiveResult primitiveCalloutToFFI(int argCount);           // 117
    PrimitiveResult primitiveDLLCall(int argCount);                // 118
    PrimitiveResult primitiveExternalCall(int argCount);           // 147

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
    PrimitiveResult primitiveGrowMemoryByAtLeast(int argCount);    // 219
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

    // Process/become primitives (197-198)
    PrimitiveResult primitiveArrayBecomeOneWay(int argCount);      // 197
    PrimitiveResult primitiveArrayBecomeOneWayCopyHash(int argCount); // 198

    // Context primitive (203)
    PrimitiveResult primitiveValueUninterruptably(int argCount);   // 203

    // Process/system primitives (172, 179)
    PrimitiveResult primitiveSetGCSemaphore(int argCount);         // 172
    PrimitiveResult primitiveRelinquishProcessor(int argCount);    // 179

    // Time
    PrimitiveResult primitiveMillisecondClock(int argCount);
    PrimitiveResult primitiveSecondsClock(int argCount);
    PrimitiveResult primitiveMicrosecondClock(int argCount);
    PrimitiveResult primitiveLocalMicrosecondClock(int argCount);
    PrimitiveResult primitiveSignalAtMilliseconds(int argCount);

    // Time primitives (242-252)
    PrimitiveResult primitiveUTCMicrosecondClock(int argCount);    // 242
    PrimitiveResult primitiveLocalTimezone(int argCount);          // 243
    PrimitiveResult primitiveTimezoneOffset(int argCount);         // 244
    PrimitiveResult primitiveDaylightSavingTimeOffset(int argCount); // 245
    PrimitiveResult primitiveVMOffsetToUTC(int argCount);          // 246
    PrimitiveResult primitivePosixMicrosecondClockWithOffset(int argCount); // 247
    PrimitiveResult primitiveSystemTimezone(int argCount);         // 248
    PrimitiveResult primitiveHighResClock(int argCount);           // 249
    PrimitiveResult primitiveUTCDateAndTime(int argCount);         // 250
    PrimitiveResult primitiveLocalDateAndTime(int argCount);       // 251
    PrimitiveResult primitiveNanosecondClock(int argCount);        // 252

    // Misc primitives (222-230)
    PrimitiveResult primitiveClosureValueNoContextSwitch2(int argCount); // 222
    PrimitiveResult primitiveClosureValueWithArgsNoContextSwitch(int argCount); // 223
    PrimitiveResult primitiveSetIdentityHash(int argCount);        // 224
    PrimitiveResult primitiveLoadInstVar(int argCount);            // 225
    PrimitiveResult primitiveStringCompare(int argCount);          // 226
    PrimitiveResult primitiveStringReplace(int argCount);          // 227
    PrimitiveResult primitiveScreenScale(int argCount);            // 228
    PrimitiveResult primitiveStringHash2(int argCount);            // 229
    PrimitiveResult primitiveShrinkMemory(int argCount);           // 230

    // Misc primitives (232-239) - 231 uses existing primitiveForceDisplayUpdate
    PrimitiveResult primitiveFormPrint(int argCount);              // 232
    PrimitiveResult primitiveSetDisplayMode(int argCount);         // 233
    PrimitiveResult primitiveBitmapDecompress(int argCount);       // 234
    PrimitiveResult primitiveStringCompareWith(int argCount);      // 235
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

    // Context inspection primitives
    PrimitiveResult primitiveContextAt(int argCount);            // 211
    PrimitiveResult primitiveContextAtPut(int argCount);         // 212

    // Cache flushing primitives
    PrimitiveResult primitiveFlushCacheByMethod(int argCount);   // 119
    PrimitiveResult primitiveFlushCacheBySelector(int argCount); // 120

    // Perform in superclass primitive
    PrimitiveResult primitivePerformInSuperclass(int argCount);  // 100

    // Closure value variant
    PrimitiveResult primitiveClosureValueNoContextSwitch(int argCount); // 204

    // Class structure primitives
    PrimitiveResult primitiveInstSize(int argCount);             // 254
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

    // String hash primitive
    PrimitiveResult primitiveStringHash(int argCount);           // 146

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
