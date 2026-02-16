/*
 * Primitives.cpp - Essential Primitive Implementations
 *
 * This file implements the ~30 essential primitives needed for bootstrap.
 * Other primitives can fail and fall back to Smalltalk code.
 */

#include "Interpreter.hpp"
#include "ImageLoader.hpp"
#include "FFI.hpp"
#include "../platform/DisplaySurface.hpp"
#include "../platform/EventQueue.hpp"
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <dlfcn.h>
#include <dirent.h>
#if __APPLE__
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <CoreFoundation/CoreFoundation.h>
// CoreFoundation pulls in objc headers which define nil as a macro
#undef nil
#endif
#include <thread>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <vector>
#include <set>
#include <unordered_map>

// Forward declaration for SDL rendering active check (defined in FFI.cpp)
extern "C" bool ffi_isSDLRenderingActive();

// Global symbol name map for TFFI call logging
static std::unordered_map<uintptr_t, std::string> g_symbolNames;
#include <ffi.h>

// ===== SurfacePlugin: Manual Surface Management =====
// Manual surfaces allow external code (e.g., SDL2 texture locking) to provide
// pixel buffers that BitBlt can read from/write to. The surface ID is stored
// as a SmallInteger in the Form's bits field.
struct ManualSurface {
    bool active;
    int width;
    int height;
    int rowPitch;  // bytes per row
    int depth;
    bool isMSB;
    void* bits;    // external pixel pointer (set via setPointer)
};

static constexpr int kMaxSurfaces = 64;
static ManualSurface g_surfaces[kMaxSurfaces] = {};
static int g_nextSurfaceID = 1;  // 0 is reserved/invalid

// Look up a surface by handle. Returns nullptr if invalid.
static ManualSurface* lookupSurface(int handle) {
    if (handle < 1 || handle >= kMaxSurfaces) return nullptr;
    if (!g_surfaces[handle].active) return nullptr;
    return &g_surfaces[handle];
}

namespace pharo {

// Forward declarations for LargeInteger arithmetic helpers
static bool extractInteger(ObjectMemory& memory, Oop oop, std::vector<uint8_t>& magnitude, bool& isNegative);
static std::vector<uint8_t> addMagnitudes(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b);
static std::vector<uint8_t> subtractMagnitudes(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b);
static int compareMagnitudes(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b);
static Oop makeLargeInteger(ObjectMemory& memory, const std::vector<uint8_t>& magnitude, bool isNegative);
static bool isLargeInteger(ObjectMemory& memory, Oop oop, bool& isNegative);
static std::vector<uint8_t> extractMagnitude(ObjectMemory& memory, Oop largeInt);
static int compareIntegers(ObjectMemory& memory, Oop a, Oop b);

// External step counter from Interpreter.cpp for debugging
extern uint64_t g_stepNum;

extern const char* g_xferReason;

// External variable from Interpreter.cpp for tracing sends after prim 264
extern int g_traceSendsAfterPrim264;

// Forward declaration for large integer helper (defined later with other large int primitives)
static bool trySigned64BitValueOf(ObjectMemory& memory, Oop oop, int64_t& value);
static std::vector<uint8_t> magnitudeLeftShift(const std::vector<uint8_t>& mag, int64_t shift);

// Helper: Convert uint64_t to SmallInteger or LargePositiveInteger.
// Returns nil on allocation failure.
static Oop uint64ToOop(ObjectMemory& memory, uint64_t value) {
    if (value <= static_cast<uint64_t>(Oop::smallIntegerMax())) {
        return Oop::fromSmallInteger(static_cast<int64_t>(value));
    }
    // Create LargePositiveInteger from little-endian bytes
    std::vector<uint8_t> mag;
    uint64_t tmp = value;
    while (tmp > 0) {
        mag.push_back(static_cast<uint8_t>(tmp & 0xFF));
        tmp >>= 8;
    }
    if (mag.empty()) mag.push_back(0);
    return makeLargeInteger(memory, mag, false);
}

// Helper: Convert int64_t to SmallInteger or LargeInteger (positive or negative).
// Returns nil on allocation failure.
static Oop int64ToOop(ObjectMemory& memory, int64_t value) {
    if (Oop::canBeSmallInteger(value)) {
        return Oop::fromSmallInteger(value);
    }
    bool neg = value < 0;
    uint64_t abs = neg ? static_cast<uint64_t>(-value) : static_cast<uint64_t>(value);
    std::vector<uint8_t> mag;
    while (abs > 0) {
        mag.push_back(static_cast<uint8_t>(abs & 0xFF));
        abs >>= 8;
    }
    if (mag.empty()) mag.push_back(0);
    return makeLargeInteger(memory, mag, neg);
}

// ===== PRIMITIVE FAILURE STUB =====
// This is used for primitives that should always fail (fall back to Smalltalk)
PrimitiveResult Interpreter::primitiveFailure(int argCount) {
    (void)argCount;
    return PrimitiveResult::Failure;
}

// ===== STUB PRIMITIVES (unimplemented, always fail) =====
PrimitiveResult Interpreter::primitiveNoop(int argCount) {
    (void)argCount;
    return PrimitiveResult::Success;  // noop succeeds by doing nothing
}

PrimitiveResult Interpreter::primitiveLowSpaceSemaphore(int argCount) {
    // Primitive 124: Register the low space semaphore
    // Args: receiver (ignored), arg = semaphore (or nil to clear)
    // Stores semaphore in special objects array at TheLowSpaceSemaphore (index 17)

    if (argCount != 1) {
        return PrimitiveResult::Failure;
    }

    Oop arg = stackValue(0);

    // Validate: must be nil or a Semaphore
    if (!arg.isNil()) {
        if (!arg.isObject()) {
            return PrimitiveResult::Failure;
        }
        // Check that it's a Semaphore (class index should match ClassSemaphore)
        Oop semaphoreClass = memory_.specialObject(SpecialObjectIndex::ClassSemaphore);
        if (memory_.classOf(arg) != semaphoreClass) {
            return PrimitiveResult::Failure;
        }
    }

    // Store in special objects array
    memory_.setSpecialObject(SpecialObjectIndex::TheLowSpaceSemaphore, arg);

    // Return receiver
    Oop receiver = stackValue(1);
    primitiveSuccess(receiver);
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveDeferDisplayUpdates(int argCount) {
    (void)argCount;
    return PrimitiveResult::Success;  // ignore display update deferral
}

PrimitiveResult Interpreter::primitiveArrayBecome(int argCount) {
    // Primitive 128: Two-way become - swaps object contents in place
    // receiver elementsExchange: anotherArray
    // Per official Spur VM: twoWay: true, copyHash: false
    // The correct Spur approach is to swap object BODIES in place at the same
    // addresses, so all existing references automatically see the other object.
    if (argCount != 1) {
        return PrimitiveResult::Failure;
    }

    Oop toArrayOop = stackValue(0);
    Oop fromArrayOop = stackValue(1);

    if (!fromArrayOop.isObject() || !toArrayOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* fromHdr = fromArrayOop.asObjectPtr();
    ObjectHeader* toHdr = toArrayOop.asObjectPtr();

    // Must be indexable pointer objects (Arrays)
    if (fromHdr->format() != ObjectFormat::Indexable ||
        toHdr->format() != ObjectFormat::Indexable) {
        return PrimitiveResult::Failure;
    }

    size_t fromSize = fromHdr->slotCount();
    size_t toSize = toHdr->slotCount();

    // Arrays must be the same size
    if (fromSize != toSize) {
        return PrimitiveResult::Failure;
    }

    for (size_t i = 0; i < fromSize; i++) {
        Oop obj1 = memory_.fetchPointer(i, fromArrayOop);
        Oop obj2 = memory_.fetchPointer(i, toArrayOop);

        if (!obj1.isObject() || !obj2.isObject()) {
            return PrimitiveResult::Failure;  // Can't become immediates
        }
        if (obj1.rawBits() == obj2.rawBits()) {
            continue;  // Same object, nothing to swap
        }

        ObjectHeader* hdr1 = obj1.asObjectPtr();
        ObjectHeader* hdr2 = obj2.asObjectPtr();

        // Both objects must be same size for in-place swap
        size_t size1 = hdr1->slotCount();
        size_t size2 = hdr2->slotCount();
        if (size1 != size2) {
            return PrimitiveResult::Failure;  // Different sizes not yet supported
        }

        // In-place become: swap the entire object contents (header + slots)
        // Note: no pinned check needed — objects stay at their addresses
        // This preserves all references automatically.
        // Swap header
        uint64_t tempHeader = hdr1->rawHeader();
        hdr1->setRawHeader(hdr2->rawHeader());
        hdr2->setRawHeader(tempHeader);

        // Swap all slot data
        Oop* slots1 = hdr1->slots();
        Oop* slots2 = hdr2->slots();
        for (size_t s = 0; s < size1; s++) {
            Oop temp = slots1[s];
            slots1[s] = slots2[s];
            slots2[s] = temp;
        }
    }

    // Flush method cache (critical after become that may affect classes)
    flushMethodCache();

    popN(1);  // Pop argument, leave receiver
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveIncrementalGC(int argCount) {
    // Primitive 131: Perform an incremental garbage collection
    // Returns the number of bytes of free space after collection
    (void)argCount;

    memory_.incrementalGC();
    flushMethodCache();  // Compaction moves objects — stale cache entries cause DNU

    size_t freeBytes = memory_.freeOldSpaceBytes();

    // Push result BEFORE signaling finalization (same reason as primitiveFullGC)
    if (Oop::canBeSmallInteger(static_cast<int64_t>(freeBytes))) {
        primitiveSuccess(Oop::fromSmallInteger(static_cast<int64_t>(freeBytes)));
    } else {
        primitiveSuccess(Oop::fromSmallInteger(Oop::smallIntegerMax()));
    }

    signalFinalizationIfNeeded();

    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveSetInterruptKey(int argCount) {
    (void)argCount;
    // Set interrupt key - acknowledge but don't do anything
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveClone(int argCount) {
    // Primitive 148: Return a shallow copy of the receiver
    // Per official VM: immediates return themselves, objects get copied
    Oop rcvr = stackValue(0);

    // Handle immediates - they are their own copies
    if (!rcvr.isObject()) {
        primitiveSuccess(rcvr);
        return PrimitiveResult::Success;
    }

    // Follow forwarding pointers (created by become:)
    rcvr = memory_.followForwarded(rcvr);
    stackValuePut(0, rcvr);

    // Delegate to shallowCopy
    return primitiveShallowCopy(argCount);
}

// Primitive 118: Execute a primitive with arguments from an array
// Stack: receiver, primitiveIndex, argumentArray (top)
PrimitiveResult Interpreter::primitiveDoPrimitiveWithArgs(int argCount) {
    if (argCount < 2 || argCount > 3) {
        return PrimitiveResult::Failure;
    }

    // Get arguments from stack
    Oop argsArrayOop = stackValue(0);  // argument array (top)
    Oop primIndexOop = stackValue(1);  // primitive index

    // Validate primitive index
    if (!primIndexOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }
    int64_t primIndex = primIndexOop.asSmallInteger();
    if (primIndex < 0 || primIndex >= static_cast<int64_t>(primitiveTable_.size())) {
        return PrimitiveResult::Failure;
    }

    // Handle quick primitives (256-519) inline.
    // These are not in the primitive table — they're bytecode shortcuts that
    // return self, constants, or instance variables. The image's Context>>step
    // simulation calls tryPrimitive:withArgs: (primitive 118) to execute them.
    if (primIndex >= 256 && primIndex <= 519) {
        // Validate argument array first
        if (!argsArrayOop.isObject()) {
            return PrimitiveResult::Failure;
        }

        Oop receiver = stackValue(2);  // receiver is below primIndex and argsArray on stack

        Oop result;
        if (primIndex == 256) {
            result = receiver;  // return self
        } else if (primIndex == 257) {
            result = memory_.trueObject();
        } else if (primIndex == 258) {
            result = memory_.falseObject();
        } else if (primIndex == 259) {
            result = memory_.nil();
        } else if (primIndex == 260) {
            result = Oop::fromSmallInteger(-1);
        } else if (primIndex == 261) {
            result = Oop::fromSmallInteger(0);
        } else if (primIndex == 262) {
            result = Oop::fromSmallInteger(1);
        } else if (primIndex == 263) {
            result = Oop::fromSmallInteger(2);
        } else {
            // 264+ = return instVar at (primIndex - 264)
            size_t instVarIndex = static_cast<size_t>(primIndex - 264);
            if (!receiver.isObject()) {
                return PrimitiveResult::Failure;
            }
            ObjectHeader* rcvrHdr = receiver.asObjectPtr();
            if (instVarIndex >= rcvrHdr->slotCount()) {
                return PrimitiveResult::Failure;
            }
            result = memory_.fetchPointer(instVarIndex, receiver);
        }

        // Pop args (argsArray, primIndex) and replace receiver with result
        popN(2);
        pop();  // pop receiver
        push(result);
        return PrimitiveResult::Success;
    }

    // Get the primitive function
    PrimitiveFunc primFunc = primitiveTable_[static_cast<size_t>(primIndex)];
    if (primFunc == nullptr) {
        return PrimitiveResult::Failure;
    }

    // Validate argument array
    if (!argsArrayOop.isObject()) {
        return PrimitiveResult::Failure;
    }
    ObjectHeader* argsHdr = argsArrayOop.asObjectPtr();
    if (!argsHdr->isPointersObject()) {
        return PrimitiveResult::Failure;
    }
    size_t numArgs = argsHdr->slotCount();

    // Pop the doPrimitive arguments (argsArray, primIndex), leave receiver
    popN(2);

    // receiver is now at stackTop(), save it
    Oop receiver = stackTop();
    pop();  // Remove receiver temporarily

    // Push the new arguments from the array
    push(receiver);  // Push receiver back as first "argument"
    for (size_t i = 0; i < numArgs; ++i) {
        push(argsHdr->slotAt(i));
    }

    // Execute the primitive with the new argument count
    // Note: argCount_ needs to be set for the called primitive
    int oldArgCount = argCount_;
    argCount_ = static_cast<int>(numArgs);
    PrimitiveResult result = (this->*primFunc)(static_cast<int>(numArgs));
    argCount_ = oldArgCount;

    // If primitive failed, restore stack state
    if (result == PrimitiveResult::Failure) {
        // Pop the pushed arguments
        popN(numArgs + 1);  // +1 for receiver
        // Push back original arguments
        push(receiver);
        push(primIndexOop);
        push(argsArrayOop);
    }

    return result;
}

// Note: primitiveScanCharacters, primitiveStringReplace, primitiveSetOrHasIdentityHash,
// primitiveImmediateAsInteger are defined later in this file with full implementations

// primitiveStringCompareWith - implemented below at primitive 158

PrimitiveResult Interpreter::primitiveFetchNextMourner(int argCount) {
    (void)argCount;
    if (!memory_.hasMourners()) {
        primitiveSuccess(memory_.nil());
        return PrimitiveResult::Success;
    }
    Oop mourner = memory_.popMourner();
    primitiveSuccess(mourner);
    return PrimitiveResult::Success;
}

// Primitive 185: Exit critical section
// Release ownership and potentially resume a waiting process
PrimitiveResult Interpreter::primitiveExitCriticalSection(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop criticalSection = stackTop();  // receiver
    if (!criticalSection.isObject()) return PrimitiveResult::Failure;

    // CriticalSections are laid out like Semaphores
    // Slot 2 (ExcessSignalsIndex) stores the owning process
    constexpr int OwnerIndex = SemaphoreExcessSignalsIndex;

    // Check if the wait list is empty
    Oop firstLink = memory_.fetchPointer(LinkedListFirstLinkIndex, criticalSection);
    Oop nilObj = memory_.nil();

    if (firstLink == nilObj) {
        // No waiting processes - just clear owner
        memory_.storePointer(OwnerIndex, criticalSection, nilObj);
        // Return self (don't change stack, just success)
        return PrimitiveResult::Success;
    } else {
        // There are waiting processes - transfer ownership to first waiter
        Oop newOwner = removeFirstLinkOfList(criticalSection);

        // Set new owner
        memory_.storePointer(OwnerIndex, criticalSection, newOwner);

        // Resume the new owner process
        // This is similar to primitiveResume
        Oop activeProcess = getActiveProcess();

        // Get priority of new owner
        Oop newPriorityOop = memory_.fetchPointer(ProcessPriorityIndex, newOwner);
        if (!newPriorityOop.isSmallInteger()) {
            return PrimitiveResult::Failure;
        }
        int64_t newPriority = newPriorityOop.asSmallInteger();

        // Get priority of active process
        Oop activePriorityOop = memory_.fetchPointer(ProcessPriorityIndex, activeProcess);
        if (!activePriorityOop.isSmallInteger()) {
            return PrimitiveResult::Failure;
        }
        int64_t activePriority = activePriorityOop.asSmallInteger();

        // Put new owner to sleep (add to its priority queue)
        putToSleep(newOwner);

        // If new owner has higher or equal priority, switch to it
        if (newPriority >= activePriority) {
            // Put active process to sleep and switch
            putToSleep(activeProcess);
            g_xferReason = "critSectEnter";
            transferTo(wakeHighestPriority());
        }

        return PrimitiveResult::Success;
    }
}

// Primitive 186: Enter critical section
// If not owned, set owner and return false.
// If owned by current process, return true (reentrant).
// If owned by another, suspend and wait.
PrimitiveResult Interpreter::primitiveEnterCriticalSection(int argCount) {
    Oop criticalSection;
    Oop activeProc;

    // Support both 0-arg (use active process) and 1-arg (explicit process) forms
    if (argCount > 1) return PrimitiveResult::Failure;

    if (argCount > 0) {
        criticalSection = stackValue(1);  // receiver
        activeProc = stackTop();          // explicit process argument
    } else {
        criticalSection = stackTop();     // receiver
        activeProc = getActiveProcess();
    }

    if (!criticalSection.isObject()) return PrimitiveResult::Failure;

    constexpr int OwnerIndex = SemaphoreExcessSignalsIndex;
    Oop nilObj = memory_.nil();

    Oop owningProcess = memory_.fetchPointer(OwnerIndex, criticalSection);

    if (owningProcess == nilObj) {
        // Not owned - claim it and return false
        memory_.storePointer(OwnerIndex, criticalSection, activeProc);
        primitiveSuccess(memory_.falseObject());
        return PrimitiveResult::Success;
    }

    if (owningProcess.rawBits() == activeProc.rawBits()) {
        // Already owned by us - return true (reentrant)
        primitiveSuccess(memory_.trueObject());
        return PrimitiveResult::Success;
    }

    // Owned by another process - must wait
    // First, set up to return false when we're eventually resumed
    // Pop args, push false as the result for when we resume
    popN(argCount + 1);
    push(memory_.falseObject());

    // Add current process to the critical section's wait queue
    addLastLinkToList(activeProc, criticalSection);

    // Switch to highest priority runnable process
    g_xferReason = "critSectWait";
    transferTo(wakeHighestPriority());

    return PrimitiveResult::Success;
}

// Primitive 187: Test and set ownership of critical section (non-blocking)
// If not owned, set owner and return false.
// If owned by current process, return true.
// If owned by another process, return nil (don't block).
PrimitiveResult Interpreter::primitiveTestAndSetOwnershipOfCriticalSection(int argCount) {
    Oop criticalSection;
    Oop activeProc;

    if (argCount > 1) return PrimitiveResult::Failure;

    if (argCount > 0) {
        criticalSection = stackValue(1);  // receiver
        activeProc = stackTop();          // explicit process argument
    } else {
        criticalSection = stackTop();     // receiver
        activeProc = getActiveProcess();
    }

    if (!criticalSection.isObject()) return PrimitiveResult::Failure;

    constexpr int OwnerIndex = SemaphoreExcessSignalsIndex;
    Oop nilObj = memory_.nil();

    Oop owningProcess = memory_.fetchPointer(OwnerIndex, criticalSection);

    if (owningProcess == nilObj) {
        // Not owned - claim it and return false
        memory_.storePointer(OwnerIndex, criticalSection, activeProc);
        primitiveSuccess(memory_.falseObject());
        return PrimitiveResult::Success;
    }

    if (owningProcess.rawBits() == activeProc.rawBits()) {
        // Already owned by us - return true
        primitiveSuccess(memory_.trueObject());
        return PrimitiveResult::Success;
    }

    // Owned by another process - return nil (non-blocking)
    primitiveSuccess(nilObj);
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveExecuteMethodArgsArray(int argCount) {
    // Primitive 188: receiver withArgs: argsArray executeMethod: aMethod
    // 2-arg form: receiver withArgs: argsArray executeMethod: method
    //   Stack: receiver | argsArray | method (top)    argCount=2
    // 3-arg form (mirror): CM receiver: rcvr withArguments: argsArray executeMethod: method
    //   Stack: CM | rcvr | argsArray | method (top)   argCount=3

    if (argCount < 2) {
        return PrimitiveResult::Failure;
    }

    // Get method from stack top
    Oop method = stackValue(0);
    // Get args array
    Oop argsArray = stackValue(1);

    // Validate method is a CompiledMethod
    if (!method.isObject()) {
        return PrimitiveResult::Failure;
    }
    ObjectHeader* methodHdr = method.asObjectPtr();
    if (!methodHdr->isCompiledMethod()) {
        return PrimitiveResult::Failure;
    }

    // Validate argsArray is an Array (format == 2, Indexable)
    if (!argsArray.isObject()) {
        return PrimitiveResult::Failure;
    }
    ObjectHeader* argsHdr = argsArray.asObjectPtr();
    if (argsHdr->format() != ObjectFormat::Indexable) {
        return PrimitiveResult::Failure;
    }

    // Get argument count from method header
    Oop methodHeader = memory_.fetchPointer(0, method);
    if (!methodHeader.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }
    int64_t headerBits = methodHeader.asSmallInteger();
    int methodNumArgs = (headerBits >> 24) & 0x0F;

    // Verify array size matches expected argument count
    size_t arraySize = memory_.slotCountOf(argsArray);
    if (static_cast<size_t>(methodNumArgs) != arraySize) {
        return PrimitiveResult::Failure;
    }

    // Mirror form: argCount > 2 means desired receiver is at stackValue(2)
    // Replace the message receiver slot with the desired receiver
    if (argCount > 2) {
        if (argCount > 4) {
            return PrimitiveResult::Failure;
        }
        Oop desiredReceiver = stackValue(2);
        // Write desired receiver into the message receiver position
        *(stackPointer_ - 1 - argCount) = desiredReceiver;
    }

    // Pop the current arguments but leave receiver
    popN(static_cast<size_t>(argCount));

    // Push arguments from the array onto the stack
    for (int i = 0; i < methodNumArgs; i++) {
        Oop arg = memory_.fetchPointer(static_cast<size_t>(i), argsArray);
        push(arg);
    }

    // Check if the method has a primitive — execute it before activating
    int primIndex = primitiveIndexOf(method);
    if (primIndex > 0) {
        Oop savedMethod = method_;
        method_ = method;
        auto result = executePrimitive(primIndex, methodNumArgs);
        if (result == PrimitiveResult::Success) {
            method_ = savedMethod;
            return PrimitiveResult::Success;
        }
        method_ = savedMethod;
        // Primitive failed — fall through to activate the method normally
    }

    // Activate the method with the new arguments
    activateMethod(method, methodNumArgs);

    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveExecuteMethod(int argCount) {
    (void)argCount;
    return PrimitiveResult::Failure;  // method execution - let Smalltalk handle
}

// Note: primitiveFindHandlerContext, primitiveContextAt, primitiveContextAtPut,
// primitiveSetFullScreen are defined later in this file with full implementations

// Note: primitiveClosureValue, primitiveClosureValueWithArgs, primitiveFullClosureValue*
// are defined later in this file with full implementations

PrimitiveResult Interpreter::primitiveFloatArrayAt(int argCount) {
    (void)argCount;
    return PrimitiveResult::Failure;  // float array access - let Smalltalk handle
}

PrimitiveResult Interpreter::primitiveFloatArrayAtPut(int argCount) {
    (void)argCount;
    return PrimitiveResult::Failure;  // float array access - let Smalltalk handle
}

// ===== ARITHMETIC PRIMITIVES (1-17) =====

PrimitiveResult Interpreter::primitiveAdd(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    if (rcvr.isSmallInteger() && arg.isSmallInteger()) {
        int64_t a = rcvr.asSmallInteger();
        int64_t b = arg.asSmallInteger();
        int64_t result = a + b;

        // Check for overflow
        if (Oop::canBeSmallInteger(result)) {
            primitiveSuccess(Oop::fromSmallInteger(result));
            return PrimitiveResult::Success;
        }

        // Overflow: produce LargeInteger
        bool resultNeg = (result < 0);
        uint64_t absResult = resultNeg ? static_cast<uint64_t>(-result) : static_cast<uint64_t>(result);
        uint8_t mag[8];
        int magLen = 0;
        uint64_t val = absResult;
        while (val > 0 && magLen < 8) {
            mag[magLen++] = (uint8_t)(val & 0xFF);
            val >>= 8;
        }
        if (magLen == 0) { mag[0] = 0; magLen = 1; }

        Oop intClass = resultNeg
            ? memory_.specialObject(SpecialObjectIndex::ClassLargeNegativeInteger)
            : memory_.specialObject(SpecialObjectIndex::ClassLargePositiveInteger);
        uint32_t classIndex = memory_.indexOfClass(intClass);
        if (classIndex == 0) classIndex = memory_.registerClass(intClass);
        Oop largeInt = memory_.allocateBytes(classIndex, magLen);
        if (largeInt.isNil() || largeInt.rawBits() == memory_.nil().rawBits())
            return PrimitiveResult::Failure;
        for (int i = 0; i < magLen; i++)
            memory_.storeByte(i, largeInt, mag[i]);
        popN(2);
        push(largeInt);
        return PrimitiveResult::Success;
    }

    // Handle SmallInteger + LargeInteger (and vice versa)
    std::vector<uint8_t> aMag, bMag;
    bool aNeg, bNeg;
    if (extractInteger(memory_, rcvr, aMag, aNeg) &&
        extractInteger(memory_, arg, bMag, bNeg)) {
        std::vector<uint8_t> resultMag;
        bool resultNeg;
        if (aNeg == bNeg) {
            resultMag = addMagnitudes(aMag, bMag);
            resultNeg = aNeg;
        } else {
            // Different signs: subtract the smaller magnitude from larger
            int cmp = compareMagnitudes(aMag, bMag);
            if (cmp >= 0) {
                resultMag = subtractMagnitudes(aMag, bMag);
                resultNeg = aNeg;
            } else {
                resultMag = subtractMagnitudes(bMag, aMag);
                resultNeg = bNeg;
            }
        }
        Oop result = makeLargeInteger(memory_, resultMag, resultNeg);
        if (!result.isNil()) {
            popN(2);
            push(result);
            return PrimitiveResult::Success;
        }
    }

    return PrimitiveResult::Failure;  // Fall back to Smalltalk
}

PrimitiveResult Interpreter::primitiveSubtract(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    if (rcvr.isSmallInteger() && arg.isSmallInteger()) {
        int64_t a = rcvr.asSmallInteger();
        int64_t b = arg.asSmallInteger();
        int64_t result = a - b;

        if (Oop::canBeSmallInteger(result)) {
            primitiveSuccess(Oop::fromSmallInteger(result));
            return PrimitiveResult::Success;
        }

        // Overflow: produce LargeInteger
        bool resultNeg = (result < 0);
        uint64_t absResult = resultNeg ? static_cast<uint64_t>(-result) : static_cast<uint64_t>(result);
        uint8_t mag[8];
        int magLen = 0;
        uint64_t val = absResult;
        while (val > 0 && magLen < 8) {
            mag[magLen++] = (uint8_t)(val & 0xFF);
            val >>= 8;
        }
        if (magLen == 0) { mag[0] = 0; magLen = 1; }

        Oop intClass = resultNeg
            ? memory_.specialObject(SpecialObjectIndex::ClassLargeNegativeInteger)
            : memory_.specialObject(SpecialObjectIndex::ClassLargePositiveInteger);
        uint32_t classIndex = memory_.indexOfClass(intClass);
        if (classIndex == 0) classIndex = memory_.registerClass(intClass);
        Oop largeInt = memory_.allocateBytes(classIndex, magLen);
        if (largeInt.isNil() || largeInt.rawBits() == memory_.nil().rawBits())
            return PrimitiveResult::Failure;
        for (int i = 0; i < magLen; i++)
            memory_.storeByte(i, largeInt, mag[i]);
        popN(2);
        push(largeInt);
        return PrimitiveResult::Success;
    }

    // Handle mixed SmallInteger/LargeInteger subtraction
    std::vector<uint8_t> aMag, bMag;
    bool aNeg, bNeg;
    if (extractInteger(memory_, rcvr, aMag, aNeg) &&
        extractInteger(memory_, arg, bMag, bNeg)) {
        // a - b = a + (-b)
        bool bNegFlipped = !bNeg;
        std::vector<uint8_t> resultMag;
        bool resultNeg;
        if (aNeg == bNegFlipped) {
            resultMag = addMagnitudes(aMag, bMag);
            resultNeg = aNeg;
        } else {
            int cmp = compareMagnitudes(aMag, bMag);
            if (cmp >= 0) {
                resultMag = subtractMagnitudes(aMag, bMag);
                resultNeg = aNeg;
            } else {
                resultMag = subtractMagnitudes(bMag, aMag);
                resultNeg = bNegFlipped;
            }
        }
        Oop result = makeLargeInteger(memory_, resultMag, resultNeg);
        if (!result.isNil()) {
            popN(2);
            push(result);
            return PrimitiveResult::Success;
        }
    }

    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveMultiply(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    if (rcvr.isSmallInteger() && arg.isSmallInteger()) {
        int64_t a = rcvr.asSmallInteger();
        int64_t b = arg.asSmallInteger();

        // Per official VM: 4-case overflow check based on sign combinations
        bool overflow = false;
        int64_t maxSmall = Oop::smallIntegerMax();
        int64_t minSmall = Oop::smallIntegerMin();

        if (a > 0) {
            if (b > 0) {
                overflow = a > (maxSmall / b);
            } else if (b < 0) {
                overflow = b < (minSmall / a);
            }
            // b == 0: no overflow (result is 0)
        } else if (a < 0) {
            if (b > 0) {
                overflow = a < (minSmall / b);
            } else if (b < 0) {
                overflow = b < (maxSmall / a);
            }
            // b == 0: no overflow (result is 0)
        }
        // a == 0: no overflow (result is 0)

        if (overflow) {
            // Produce a LargeInteger result using __int128 for full precision
            __int128 wideA = a;
            __int128 wideB = b;
            __int128 wideResult = wideA * wideB;
            bool resultNeg = (wideResult < 0);
            if (resultNeg) wideResult = -wideResult;

            // Convert to byte magnitude (little-endian)
            uint8_t mag[16];
            int magLen = 0;
            __int128 val = wideResult;
            while (val > 0 && magLen < 16) {
                mag[magLen++] = (uint8_t)(val & 0xFF);
                val >>= 8;
            }
            if (magLen == 0) { mag[0] = 0; magLen = 1; }

            // Allocate LargeInteger
            Oop intClass = resultNeg
                ? memory_.specialObject(SpecialObjectIndex::ClassLargeNegativeInteger)
                : memory_.specialObject(SpecialObjectIndex::ClassLargePositiveInteger);
            uint32_t classIndex = memory_.indexOfClass(intClass);
            if (classIndex == 0) classIndex = memory_.registerClass(intClass);
            Oop largeInt = memory_.allocateBytes(classIndex, magLen);
            if (largeInt.isNil() || largeInt.rawBits() == memory_.nil().rawBits()) {
                return PrimitiveResult::Failure;
            }
            for (int i = 0; i < magLen; i++) {
                memory_.storeByte(i, largeInt, mag[i]);
            }

            popN(2);
            push(largeInt);
            return PrimitiveResult::Success;
        }

        int64_t result = a * b;
        if (Oop::canBeSmallInteger(result)) {
            primitiveSuccess(Oop::fromSmallInteger(result));
            return PrimitiveResult::Success;
        }
        // Overflow but both SmallInt — produce LargeInteger
        // (This case: overflow detected but fell through above — shouldn't happen
        //  but handle it as failure for safety)
    }

    // Mixed SmallInt/LargeInt multiplication using trySigned64BitValueOf
    int64_t a, b;
    if (trySigned64BitValueOf(memory_, rcvr, a) &&
        trySigned64BitValueOf(memory_, arg, b)) {
        __int128 wideResult = (__int128)a * (__int128)b;
        bool resultNeg = (wideResult < 0);
        if (resultNeg) wideResult = -wideResult;

        // Try SmallInteger first
        if (!resultNeg && wideResult <= Oop::smallIntegerMax()) {
            primitiveSuccess(Oop::fromSmallInteger((int64_t)wideResult));
            return PrimitiveResult::Success;
        }
        if (resultNeg && (-((__int128)1) * wideResult) >= Oop::smallIntegerMin()) {
            primitiveSuccess(Oop::fromSmallInteger((int64_t)(-wideResult)));
            return PrimitiveResult::Success;
        }

        // Create LargeInteger
        uint8_t mag[16];
        int magLen = 0;
        __int128 val = wideResult;
        while (val > 0 && magLen < 16) {
            mag[magLen++] = (uint8_t)(val & 0xFF);
            val >>= 8;
        }
        if (magLen == 0) { mag[0] = 0; magLen = 1; }

        std::vector<uint8_t> bytes(mag, mag + magLen);
        Oop largeResult = makeLargeInteger(memory_, bytes, resultNeg);
        if (largeResult.rawBits() == 0) return PrimitiveResult::Failure;
        primitiveSuccess(largeResult);
        return PrimitiveResult::Success;
    }

    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveDivide(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    if (rcvr.isSmallInteger() && arg.isSmallInteger()) {
        int64_t a = rcvr.asSmallInteger();
        int64_t b = arg.asSmallInteger();

        if (b == 0) {
            return PrimitiveResult::Failure;  // Division by zero
        }

        // Check for exact division
        if (a % b != 0) {
            return PrimitiveResult::Failure;  // Not exact, use Fraction
        }

        int64_t result = a / b;
        primitiveSuccess(Oop::fromSmallInteger(result));
        return PrimitiveResult::Success;
    }

    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveMod(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    int64_t a, b;
    if (trySigned64BitValueOf(memory_, rcvr, a) &&
        trySigned64BitValueOf(memory_, arg, b)) {
        if (b == 0) {
            return PrimitiveResult::Failure;
        }

        // Smalltalk mod (\\) returns result with same sign as divisor
        int64_t rem = a % b;
        // Adjust sign: C remainder has sign of dividend, Smalltalk mod has sign of divisor
        if (rem != 0 && ((a < 0) != (b < 0))) {
            rem += b;
        }
        if (rem >= Oop::smallIntegerMin() && rem <= Oop::smallIntegerMax()) {
            primitiveSuccess(Oop::fromSmallInteger(rem));
            return PrimitiveResult::Success;
        }
        // Result doesn't fit in SmallInteger - create LargeInteger
        bool neg = rem < 0;
        uint64_t mag = neg ? (uint64_t)(-(rem + 1)) + 1 : (uint64_t)rem;
        std::vector<uint8_t> bytes;
        while (mag > 0) { bytes.push_back(mag & 0xFF); mag >>= 8; }
        if (bytes.empty()) bytes.push_back(0);
        Oop result = makeLargeInteger(memory_, bytes, neg);
        if (result.rawBits() == 0) return PrimitiveResult::Failure;
        primitiveSuccess(result);
        return PrimitiveResult::Success;
    }

    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveDiv(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    if (rcvr.isSmallInteger() && arg.isSmallInteger()) {
        int64_t a = rcvr.asSmallInteger();
        int64_t b = arg.asSmallInteger();

        if (b == 0) {
            return PrimitiveResult::Failure;
        }

        // Floored division (//) using integer arithmetic
        int64_t q = a / b;
        int64_t rem = a % b;
        if (rem != 0 && ((a < 0) != (b < 0))) {
            q -= 1;
        }
        primitiveSuccess(Oop::fromSmallInteger(q));
        return PrimitiveResult::Success;
    }

    // Handle mixed SmallInteger/LargeInteger division
    int64_t aVal, bVal;
    if (trySigned64BitValueOf(memory_, rcvr, aVal) &&
        trySigned64BitValueOf(memory_, arg, bVal)) {
        if (bVal == 0) return PrimitiveResult::Failure;
        int64_t q = aVal / bVal;
        int64_t rem = aVal % bVal;
        if (rem != 0 && ((aVal < 0) != (bVal < 0))) {
            q -= 1;
        }
        if (Oop::canBeSmallInteger(q)) {
            popN(2);
            push(Oop::fromSmallInteger(q));
            return PrimitiveResult::Success;
        }
        // Result doesn't fit SmallInt — make LargeInteger
        bool resultNeg = q < 0;
        uint64_t absQ = resultNeg ? static_cast<uint64_t>(-q) : static_cast<uint64_t>(q);
        std::vector<uint8_t> mag;
        if (absQ == 0) { mag.push_back(0); }
        else { while (absQ > 0) { mag.push_back(absQ & 0xFF); absQ >>= 8; } }
        Oop result = makeLargeInteger(memory_, mag, resultNeg);
        if (!result.isNil()) {
            popN(2);
            push(result);
            return PrimitiveResult::Success;
        }
    }

    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveQuo(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    int64_t a, b;
    if (trySigned64BitValueOf(memory_, rcvr, a) &&
        trySigned64BitValueOf(memory_, arg, b)) {
        if (b == 0) {
            return PrimitiveResult::Failure;
        }

        // Truncated division (quo:)
        int64_t result = a / b;
        if (result >= Oop::smallIntegerMin() && result <= Oop::smallIntegerMax()) {
            primitiveSuccess(Oop::fromSmallInteger(result));
            return PrimitiveResult::Success;
        }
        // Result doesn't fit in SmallInteger
        bool neg = result < 0;
        uint64_t mag = neg ? (uint64_t)(-(result + 1)) + 1 : (uint64_t)result;
        std::vector<uint8_t> bytes;
        while (mag > 0) { bytes.push_back(mag & 0xFF); mag >>= 8; }
        if (bytes.empty()) bytes.push_back(0);
        Oop largeResult = makeLargeInteger(memory_, bytes, neg);
        if (largeResult.rawBits() == 0) return PrimitiveResult::Failure;
        primitiveSuccess(largeResult);
        return PrimitiveResult::Success;
    }

    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveBitAnd(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    int64_t a, b;
    if (trySigned64BitValueOf(memory_, rcvr, a) &&
        trySigned64BitValueOf(memory_, arg, b)) {
        int64_t result = a & b;
        if (Oop::canBeSmallInteger(result)) {
            primitiveSuccess(Oop::fromSmallInteger(result));
            return PrimitiveResult::Success;
        }
        bool neg = result < 0;
        uint64_t mag = neg ? (uint64_t)(-(result + 1)) + 1 : (uint64_t)result;
        std::vector<uint8_t> bytes;
        while (mag > 0) { bytes.push_back(mag & 0xFF); mag >>= 8; }
        if (bytes.empty()) bytes.push_back(0);
        Oop largeResult = makeLargeInteger(memory_, bytes, neg);
        if (largeResult.rawBits() == 0) return PrimitiveResult::Failure;
        primitiveSuccess(largeResult);
        return PrimitiveResult::Success;
    }

    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveBitOr(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    int64_t a, b;
    if (trySigned64BitValueOf(memory_, rcvr, a) &&
        trySigned64BitValueOf(memory_, arg, b)) {
        int64_t result = a | b;
        if (Oop::canBeSmallInteger(result)) {
            primitiveSuccess(Oop::fromSmallInteger(result));
            return PrimitiveResult::Success;
        }
        bool neg = result < 0;
        uint64_t mag = neg ? (uint64_t)(-(result + 1)) + 1 : (uint64_t)result;
        std::vector<uint8_t> bytes;
        while (mag > 0) { bytes.push_back(mag & 0xFF); mag >>= 8; }
        if (bytes.empty()) bytes.push_back(0);
        Oop largeResult = makeLargeInteger(memory_, bytes, neg);
        if (largeResult.rawBits() == 0) return PrimitiveResult::Failure;
        primitiveSuccess(largeResult);
        return PrimitiveResult::Success;
    }

    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveBitXor(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    int64_t a, b;
    if (trySigned64BitValueOf(memory_, rcvr, a) &&
        trySigned64BitValueOf(memory_, arg, b)) {
        int64_t result = a ^ b;
        if (Oop::canBeSmallInteger(result)) {
            primitiveSuccess(Oop::fromSmallInteger(result));
            return PrimitiveResult::Success;
        }
        bool neg = result < 0;
        uint64_t mag = neg ? (uint64_t)(-(result + 1)) + 1 : (uint64_t)result;
        std::vector<uint8_t> bytes;
        while (mag > 0) { bytes.push_back(mag & 0xFF); mag >>= 8; }
        if (bytes.empty()) bytes.push_back(0);
        Oop largeResult = makeLargeInteger(memory_, bytes, neg);
        if (largeResult.rawBits() == 0) return PrimitiveResult::Failure;
        primitiveSuccess(largeResult);
        return PrimitiveResult::Success;
    }

    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveBitShift(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    if (!arg.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t shift = arg.asSmallInteger();

    int64_t value;
    if (!trySigned64BitValueOf(memory_, rcvr, value))
        return PrimitiveResult::Failure;

    if (shift >= 0) {
        // Left shift — handle overflow by creating LargeInteger
        if (value == 0) {
            // 0 << anything = 0
            primitiveSuccess(Oop::fromSmallInteger(0));
            return PrimitiveResult::Success;
        }

        // For shifts within __int128 range, use it for exact computation
        if (shift < 127) {
            __int128 wide = (__int128)value << shift;
            if (wide >= Oop::smallIntegerMin() && wide <= Oop::smallIntegerMax()) {
                primitiveSuccess(Oop::fromSmallInteger((int64_t)wide));
                return PrimitiveResult::Success;
            }
            // Fits in int64 but not SmallInteger
            if (wide >= INT64_MIN && wide <= INT64_MAX) {
                int64_t result = (int64_t)wide;
                bool neg = result < 0;
                uint64_t mag = neg ? (uint64_t)(-(result + 1)) + 1 : (uint64_t)result;
                std::vector<uint8_t> bytes;
                while (mag > 0) { bytes.push_back(mag & 0xFF); mag >>= 8; }
                if (bytes.empty()) bytes.push_back(0);
                Oop largeResult = makeLargeInteger(memory_, bytes, neg);
                if (largeResult.rawBits() == 0) return PrimitiveResult::Failure;
                primitiveSuccess(largeResult);
                return PrimitiveResult::Success;
            }
        }

        // Result exceeds int64 range — use magnitude-based shift
        // This handles the case where Smalltalk fallback would incorrectly
        // create a LargePositiveInteger from a negative SmallInteger
        bool neg = value < 0;
        uint64_t mag = neg ? (uint64_t)(-(value + 1)) + 1 : (uint64_t)value;
        std::vector<uint8_t> magBytes;
        while (mag > 0) { magBytes.push_back(mag & 0xFF); mag >>= 8; }
        if (magBytes.empty()) magBytes.push_back(0);

        std::vector<uint8_t> shiftedMag = magnitudeLeftShift(magBytes, shift);
        if (shiftedMag.empty()) return PrimitiveResult::Failure;  // overflow

        Oop largeResult = makeLargeInteger(memory_, shiftedMag, neg);
        if (largeResult.rawBits() == 0) return PrimitiveResult::Failure;
        primitiveSuccess(largeResult);
        return PrimitiveResult::Success;
    } else {
        // Right shift
        int64_t result;
        if (shift <= -64) {
            result = (value < 0) ? -1 : 0;
        } else {
            result = value >> (-shift);
        }
        if (Oop::canBeSmallInteger(result)) {
            primitiveSuccess(Oop::fromSmallInteger(result));
            return PrimitiveResult::Success;
        }
    }

    return PrimitiveResult::Failure;
}

// ===== POINT PRIMITIVE =====

PrimitiveResult Interpreter::primitiveMakePoint(int argCount) {
    // Primitive 18: Number @ aNumber - create a Point
    static int callCount = 0;
    static FILE* pointLog = nullptr;
    callCount++;

    if (!pointLog) {
        pointLog = nullptr;
    }

    Oop yArg = stackValue(0);
    Oop xRcvr = stackValue(1);

    if (pointLog && callCount <= 20) {
        fprintf(pointLog, "[POINT %d] primitiveMakePoint: x=0x%llx y=0x%llx\n",
                callCount, (unsigned long long)xRcvr.rawBits(), (unsigned long long)yArg.rawBits());
        fprintf(pointLog, "[POINT %d]   xRcvr.isSmallInt=%d yArg.isSmallInt=%d\n",
                callCount, xRcvr.isSmallInteger() ? 1 : 0, yArg.isSmallInteger() ? 1 : 0);
        fflush(pointLog);
    }

    // Both arguments should be numbers (SmallInteger or Float)
    // For simplicity, we accept SmallIntegers, SmallFloats, or boxed Floats
    if (!xRcvr.isSmallInteger() && !xRcvr.isSmallFloat() &&
        !(xRcvr.isObject() && memory_.classOf(xRcvr) == memory_.specialObject(SpecialObjectIndex::ClassFloat))) {
        if (pointLog && callCount <= 20) {
            fprintf(pointLog, "[POINT %d]   FAIL: xRcvr not a number\n", callCount);
            fflush(pointLog);
        }
        return PrimitiveResult::Failure;
    }
    if (!yArg.isSmallInteger() && !yArg.isSmallFloat() &&
        !(yArg.isObject() && memory_.classOf(yArg) == memory_.specialObject(SpecialObjectIndex::ClassFloat))) {
        if (pointLog && callCount <= 20) {
            fprintf(pointLog, "[POINT %d]   FAIL: yArg not a number\n", callCount);
            fflush(pointLog);
        }
        return PrimitiveResult::Failure;
    }

    // Get Point class
    Oop pointClass = memory_.specialObject(SpecialObjectIndex::ClassPoint);
    uint32_t classIndex = memory_.indexOfClass(pointClass);

    if (pointLog && callCount <= 20) {
        fprintf(pointLog, "[POINT %d]   pointClass=0x%llx classIndex=%u\n",
                callCount, (unsigned long long)pointClass.rawBits(), classIndex);
        fflush(pointLog);
    }

    // Allocate Point with 2 slots (x, y)
    Oop point = memory_.allocateSlots(classIndex, 2);
    if (point.isNil()) {
        if (pointLog && callCount <= 20) {
            fprintf(pointLog, "[POINT %d]   FAIL: allocation failed\n", callCount);
            fflush(pointLog);
        }
        return PrimitiveResult::Failure;
    }

    // Store x and y
    memory_.storePointer(0, point, xRcvr);  // x
    memory_.storePointer(1, point, yArg);   // y

    if (pointLog && callCount <= 20) {
        fprintf(pointLog, "[POINT %d]   SUCCESS: point=0x%llx\n", callCount, (unsigned long long)point.rawBits());
        fflush(pointLog);
    }

    primitiveSuccess(point);
    return PrimitiveResult::Success;
}

// ===== COMPARISON PRIMITIVES =====

PrimitiveResult Interpreter::primitiveLessThan(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    // Fast path: both are SmallIntegers
    if (rcvr.isSmallInteger() && arg.isSmallInteger()) {
        bool result = rcvr.asSmallInteger() < arg.asSmallInteger();
        primitiveSuccess(result ? memory_.trueObject() : memory_.falseObject());
        return PrimitiveResult::Success;
    }

    // Try to extract 64-bit signed values (handles LargeIntegers)
    int64_t rcvrVal, argVal;
    if (trySigned64BitValueOf(memory_, rcvr, rcvrVal) &&
        trySigned64BitValueOf(memory_, arg, argVal)) {
        bool result = (rcvrVal < argVal);
        primitiveSuccess(result ? memory_.trueObject() : memory_.falseObject());
        return PrimitiveResult::Success;
    }

    // Fallback: handle LargeIntegers of arbitrary size
    {
        std::vector<uint8_t> aMag, bMag;
        bool aNeg, bNeg;
        if (extractInteger(memory_, rcvr, aMag, aNeg) &&
            extractInteger(memory_, arg, bMag, bNeg)) {
            int cmp = compareIntegers(memory_, rcvr, arg);
            primitiveSuccess(cmp < 0 ? memory_.trueObject() : memory_.falseObject());
            return PrimitiveResult::Success;
        }
    }

    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveGreaterThan(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    // Fast path: both are SmallIntegers
    if (rcvr.isSmallInteger() && arg.isSmallInteger()) {
        bool result = rcvr.asSmallInteger() > arg.asSmallInteger();
        primitiveSuccess(result ? memory_.trueObject() : memory_.falseObject());
        return PrimitiveResult::Success;
    }

    // Try to extract 64-bit signed values (handles LargeIntegers)
    int64_t rcvrVal, argVal;
    if (trySigned64BitValueOf(memory_, rcvr, rcvrVal) &&
        trySigned64BitValueOf(memory_, arg, argVal)) {
        bool result = (rcvrVal > argVal);
        primitiveSuccess(result ? memory_.trueObject() : memory_.falseObject());
        return PrimitiveResult::Success;
    }

    // Fallback: handle LargeIntegers of arbitrary size
    {
        std::vector<uint8_t> aMag, bMag;
        bool aNeg, bNeg;
        if (extractInteger(memory_, rcvr, aMag, aNeg) &&
            extractInteger(memory_, arg, bMag, bNeg)) {
            int cmp = compareIntegers(memory_, rcvr, arg);
            primitiveSuccess(cmp > 0 ? memory_.trueObject() : memory_.falseObject());
            return PrimitiveResult::Success;
        }
    }

    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveLessOrEqual(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    // Fast path: both are SmallIntegers
    if (rcvr.isSmallInteger() && arg.isSmallInteger()) {
        int64_t rcvrVal = rcvr.asSmallInteger();
        int64_t argVal = arg.asSmallInteger();
        bool result = rcvrVal <= argVal;
        primitiveSuccess(result ? memory_.trueObject() : memory_.falseObject());
        return PrimitiveResult::Success;
    }

    // Try to extract 64-bit signed values (handles LargeIntegers)
    int64_t rcvrVal, argVal;
    if (trySigned64BitValueOf(memory_, rcvr, rcvrVal) &&
        trySigned64BitValueOf(memory_, arg, argVal)) {
        bool result = (rcvrVal <= argVal);
        primitiveSuccess(result ? memory_.trueObject() : memory_.falseObject());
        return PrimitiveResult::Success;
    }

    // Fallback: handle LargeIntegers of arbitrary size
    {
        std::vector<uint8_t> aMag, bMag;
        bool aNeg, bNeg;
        if (extractInteger(memory_, rcvr, aMag, aNeg) &&
            extractInteger(memory_, arg, bMag, bNeg)) {
            int cmp = compareIntegers(memory_, rcvr, arg);
            primitiveSuccess(cmp <= 0 ? memory_.trueObject() : memory_.falseObject());
            return PrimitiveResult::Success;
        }
    }

    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveGreaterOrEqual(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    // Fast path: both are SmallIntegers
    if (rcvr.isSmallInteger() && arg.isSmallInteger()) {
        bool result = rcvr.asSmallInteger() >= arg.asSmallInteger();
        primitiveSuccess(result ? memory_.trueObject() : memory_.falseObject());
        return PrimitiveResult::Success;
    }

    // Try to extract 64-bit signed values (handles LargeIntegers)
    int64_t rcvrVal, argVal;
    if (trySigned64BitValueOf(memory_, rcvr, rcvrVal) &&
        trySigned64BitValueOf(memory_, arg, argVal)) {
        bool result = (rcvrVal >= argVal);
        primitiveSuccess(result ? memory_.trueObject() : memory_.falseObject());
        return PrimitiveResult::Success;
    }

    // Fallback: handle LargeIntegers of arbitrary size
    {
        std::vector<uint8_t> aMag, bMag;
        bool aNeg, bNeg;
        if (extractInteger(memory_, rcvr, aMag, aNeg) &&
            extractInteger(memory_, arg, bMag, bNeg)) {
            int cmp = compareIntegers(memory_, rcvr, arg);
            primitiveSuccess(cmp >= 0 ? memory_.trueObject() : memory_.falseObject());
            return PrimitiveResult::Success;
        }
    }

    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveEqual(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    // Fast path: both are SmallIntegers (most common case)
    if (rcvr.isSmallInteger() && arg.isSmallInteger()) {
        bool result = (rcvr.rawBits() == arg.rawBits());
        primitiveSuccess(result ? memory_.trueObject() : memory_.falseObject());
        return PrimitiveResult::Success;
    }

    // Try to extract 64-bit signed values (handles LargeIntegers that fit in 64 bits)
    int64_t rcvrVal, argVal;
    if (trySigned64BitValueOf(memory_, rcvr, rcvrVal) &&
        trySigned64BitValueOf(memory_, arg, argVal)) {
        bool result = (rcvrVal == argVal);
        primitiveSuccess(result ? memory_.trueObject() : memory_.falseObject());
        return PrimitiveResult::Success;
    }

    // Fallback: handle LargeIntegers of arbitrary size
    {
        std::vector<uint8_t> aMag, bMag;
        bool aNeg, bNeg;
        if (extractInteger(memory_, rcvr, aMag, aNeg) &&
            extractInteger(memory_, arg, bMag, bNeg)) {
            int cmp = compareIntegers(memory_, rcvr, arg);
            primitiveSuccess(cmp == 0 ? memory_.trueObject() : memory_.falseObject());
            return PrimitiveResult::Success;
        }
    }

    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveNotEqual(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    // Fast path: both are SmallIntegers
    if (rcvr.isSmallInteger() && arg.isSmallInteger()) {
        bool result = (rcvr.rawBits() != arg.rawBits());
        primitiveSuccess(result ? memory_.trueObject() : memory_.falseObject());
        return PrimitiveResult::Success;
    }

    // Try to extract 64-bit signed values (handles LargeIntegers)
    int64_t rcvrVal, argVal;
    if (trySigned64BitValueOf(memory_, rcvr, rcvrVal) &&
        trySigned64BitValueOf(memory_, arg, argVal)) {
        bool result = (rcvrVal != argVal);
        primitiveSuccess(result ? memory_.trueObject() : memory_.falseObject());
        return PrimitiveResult::Success;
    }

    // Fallback: handle LargeIntegers of arbitrary size
    {
        std::vector<uint8_t> aMag, bMag;
        bool aNeg, bNeg;
        if (extractInteger(memory_, rcvr, aMag, aNeg) &&
            extractInteger(memory_, arg, bMag, bNeg)) {
            int cmp = compareIntegers(memory_, rcvr, arg);
            primitiveSuccess(cmp != 0 ? memory_.trueObject() : memory_.falseObject());
            return PrimitiveResult::Success;
        }
    }

    return PrimitiveResult::Failure;
}

// ===== OBJECT ACCESS PRIMITIVES (60-75) =====

PrimitiveResult Interpreter::primitiveAt(int argCount) {
    Oop index = stackValue(0);
    Oop rcvr = stackValue(1);

    // Special handling for nil receiver - return nil instead of failing
    // This prevents error cascades during FFI startup when struct refs are nil
    if (rcvr.isNil() || rcvr.rawBits() == memory_.nil().rawBits()) {
        static int nilAtCount = 0;
        nilAtCount++;
        if (nilAtCount <= 10) {
            std::cerr << "[PRIM-AT-NIL #" << nilAtCount << "] at: on nil - returning nil\n";
        }
        primitiveSuccess(memory_.nil());
        return PrimitiveResult::Success;
    }

    // Handle SmallFloat64 immediates: basicAt: returns 32-bit word halves
    if (rcvr.isSmallFloat() && index.isSmallInteger()) {
        int64_t idx = index.asSmallInteger();
        if (idx < 1 || idx > 2) {
            return PrimitiveResult::Failure;
        }
        double dval = rcvr.asSmallFloat();
        uint64_t bits;
        std::memcpy(&bits, &dval, sizeof(double));
        uint32_t word;
        if (idx == 1) {
            word = static_cast<uint32_t>(bits >> 32);  // high word
        } else {
            word = static_cast<uint32_t>(bits & 0xFFFFFFFF);  // low word
        }
        primitiveSuccess(Oop::fromSmallInteger(static_cast<int64_t>(word)));
        return PrimitiveResult::Success;
    }

    // Official VM behavior: fail if index is not SmallInteger (PrimErrBadArgument)
    // or if receiver is not an object (PrimErrInappropriate)
    if (!index.isSmallInteger() || !rcvr.isObject()) {
        return PrimitiveResult::Failure;
    }

    int64_t idx = index.asSmallInteger();
    if (idx < 1) {
        return PrimitiveResult::Failure;  // 1-based indexing
    }
    // Validate receiver pointer is within heap bounds (old, new, or perm space)
    {
        uint8_t* ptr = reinterpret_cast<uint8_t*>(rcvr.rawBits());
        if (!memory_.isOldObject(ptr) && !memory_.isYoungObject(ptr) && !memory_.isPermObject(ptr)) {
            return PrimitiveResult::Failure;
        }
    }

    ObjectHeader* header = rcvr.asObjectPtr();
    size_t arrayIndex = static_cast<size_t>(idx - 1);

    // Sanity check header before accessing object data
    ObjectFormat fmt = header->format();
    size_t slots = header->slotCount();
    // Note: objEnd check removed - was using incorrect memory layout assumptions
    // The receiver pointer has already been validated to be in a valid heap space above
    if (fmt == ObjectFormat::Indexable64) {
        // 64-bit word array (DoubleWordArray): each slot is one 64-bit element
        size_t numElements = header->slotCount();
        if (arrayIndex >= numElements) {
            return PrimitiveResult::Failure;
        }
        uint64_t bits = memory_.fetchWord64(arrayIndex, rcvr);
        // Return as SmallInteger if it fits, otherwise as LargePositiveInteger
        if (bits <= static_cast<uint64_t>(Oop::smallIntegerMax())) {
            primitiveSuccess(Oop::fromSmallInteger(static_cast<int64_t>(bits)));
        } else {
            // Create LargePositiveInteger from 64-bit value (little-endian bytes)
            std::vector<uint8_t> mag;
            uint64_t tmp = bits;
            while (tmp > 0) {
                mag.push_back(static_cast<uint8_t>(tmp & 0xFF));
                tmp >>= 8;
            }
            if (mag.empty()) mag.push_back(0);
            Oop result = makeLargeInteger(memory_, mag, false);
            if (result.isNil()) return PrimitiveResult::Failure;
            primitiveSuccess(result);
        }
        return PrimitiveResult::Success;
    }

    // Handle byte-format BoxedFloat64 (formats 10-15 with 8 bytes)
    // These should also return 32-bit word halves like format 9
    uint8_t fmtVal = static_cast<uint8_t>(fmt);
    if (fmtVal >= 10 && fmtVal <= 15 && header->byteSize() == 8) {
        // Check if this is a BoxedFloat64
        Oop floatClass = memory_.specialObject(SpecialObjectIndex::ClassFloat);
        Oop objClass = memory_.classOf(rcvr);
        if (objClass.rawBits() == floatClass.rawBits()) {
            if (arrayIndex >= 2) {
                return PrimitiveResult::Failure;
            }
            uint64_t bits;
            std::memcpy(&bits, header->bytes(), 8);
            uint32_t word;
            if (arrayIndex == 0) {
                word = static_cast<uint32_t>(bits >> 32);  // high word
            } else {
                word = static_cast<uint32_t>(bits & 0xFFFFFFFF);  // low word
            }
            primitiveSuccess(Oop::fromSmallInteger(static_cast<int64_t>(word)));
            return PrimitiveResult::Success;
        }
    }

    // Handle 32-bit word objects (format 10-11): WideString, WordArray
    if (fmtVal >= 10 && fmtVal <= 11) {
        size_t numElements = header->slotCount() * 2 - (fmtVal - 10);
        if (arrayIndex >= numElements) {
            return PrimitiveResult::Failure;
        }
        uint32_t word;
        std::memcpy(&word, header->bytes() + arrayIndex * 4, 4);
        primitiveSuccess(Oop::fromSmallInteger(static_cast<int64_t>(word)));
        return PrimitiveResult::Success;
    }

    // Handle 16-bit word objects (format 12-15): DoubleByteArray
    if (fmtVal >= 12 && fmtVal <= 15) {
        size_t numElements = header->slotCount() * 4 - (fmtVal - 12);
        if (arrayIndex >= numElements) {
            return PrimitiveResult::Failure;
        }
        uint16_t word;
        std::memcpy(&word, header->bytes() + arrayIndex * 2, 2);
        primitiveSuccess(Oop::fromSmallInteger(static_cast<int64_t>(word)));
        return PrimitiveResult::Success;
    }

    if (header->isBytesObject()) {
        if (arrayIndex >= header->byteSize()) {
            return PrimitiveResult::Failure;
        }
        uint8_t byte = header->byteAt(arrayIndex);
        // For generic at:, return SmallInteger (byte value)
        // ByteString should use primitive 63 (stringAt:) which returns Character
        primitiveSuccess(Oop::fromSmallInteger(byte));
        return PrimitiveResult::Success;
    } else if (header->isPointersObject()) {
        // Per official VM: at: accesses the INDEXABLE part, skipping fixed fields
        // For format 2 (Indexable): no fixed fields, access directly
        // For format 3 (IndexableWithFixed): add fixedFields to index
        ObjectFormat fmt = header->format();
        // Per official VM: format 0 (ZeroSized) and 1 (FixedSize) are NOT indexable.
        // primitive at:/at:put: must fail for these - use instVarAt: instead.
        if (fmt == ObjectFormat::ZeroSized || fmt == ObjectFormat::FixedSize) {
            return PrimitiveResult::Failure;
        }
        size_t fixedFields = 0;
        // Formats 3, 4, 5 all can have named (fixed) instVars before indexable slots:
        //   3 = IndexableWithFixed (e.g. Context)
        //   4 = Weak (weak variable, may have fixed fields e.g. WeakAnnouncementSubscription)
        //   5 = WeakWithFixed/Ephemeron
        if (fmt == ObjectFormat::IndexableWithFixed ||
            fmt == ObjectFormat::Weak ||
            fmt == ObjectFormat::WeakWithFixed) {
            Oop objClass = memory_.classOf(rcvr);
            if (objClass.isObject()) {
                Oop instSpec = memory_.fetchPointer(2, objClass);
                if (instSpec.isSmallInteger()) {
                    fixedFields = instSpec.asSmallInteger() & 0xFFFF;
                }
            }
        }
        size_t slotCount = header->slotCount();
        if (fixedFields > slotCount) {
            return PrimitiveResult::Failure;
        }
        size_t indexableSize = slotCount - fixedFields;
        if (arrayIndex >= indexableSize) {
            return PrimitiveResult::Failure;
        }
        size_t actualSlot = fixedFields + arrayIndex;
        Oop result = header->slotAt(actualSlot);
        primitiveSuccess(result);
        return PrimitiveResult::Success;
    } else if (header->isCompiledMethod()) {
        // CompiledMethods are hybrid objects: slots (header + literals) then bytecodes
        // at: accesses raw bytes directly using 1-based index
        // Smalltalk calculates initialPC = (numLiterals+1)*wordSize+1 to find bytecodes
        // objectAt: (primitive 68) accesses the literal frame instead

        size_t totalBytes = header->byteSize();

        // arrayIndex is (idx - 1), so byteIndex is 0-based
        if (arrayIndex >= totalBytes) {
            return PrimitiveResult::Failure;
        }

        uint8_t byte = header->bytes()[arrayIndex];
        primitiveSuccess(Oop::fromSmallInteger(byte));
        return PrimitiveResult::Success;
    }

    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveAtPut(int argCount) {
    Oop value = stackValue(0);
    Oop index = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!index.isSmallInteger() || !rcvr.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Validate receiver pointer is within heap bounds
    if (!memory_.isValidObject(rcvr)) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* header = rcvr.asObjectPtr();
    if (header->isImmutable()) {
        return PrimitiveResult::Failure;
    }

    int64_t idx = index.asSmallInteger();
    if (idx < 1) {
        return PrimitiveResult::Failure;
    }

    size_t arrayIndex = static_cast<size_t>(idx - 1);

    if (header->isBytesObject()) {
        if (!value.isSmallInteger()) {
            return PrimitiveResult::Failure;
        }
        int64_t byteValue = value.asSmallInteger();
        if (byteValue < 0 || byteValue > 255) {
            return PrimitiveResult::Failure;
        }
        if (arrayIndex >= header->byteSize()) {
            return PrimitiveResult::Failure;
        }
        header->byteAtPut(arrayIndex, static_cast<uint8_t>(byteValue));
        primitiveSuccess(value);
        return PrimitiveResult::Success;
    } else if (header->isPointersObject()) {
        // Per official VM: at:put: accesses the INDEXABLE part, skipping fixed fields
        // Formats 3, 4, 5 can have named (fixed) instVars before indexable slots
        ObjectFormat fmt = header->format();
        // Per official VM: format 0 (ZeroSized) and 1 (FixedSize) are NOT indexable.
        // primitive at:/at:put: must fail for these - use instVarAt: instead.
        if (fmt == ObjectFormat::ZeroSized || fmt == ObjectFormat::FixedSize) {
            return PrimitiveResult::Failure;
        }
        size_t fixedFields = 0;
        if (fmt == ObjectFormat::IndexableWithFixed ||
            fmt == ObjectFormat::Weak ||
            fmt == ObjectFormat::WeakWithFixed) {
            Oop objClass = memory_.classOf(rcvr);
            if (objClass.isObject()) {
                Oop instSpec = memory_.fetchPointer(2, objClass);
                if (instSpec.isSmallInteger()) {
                    fixedFields = instSpec.asSmallInteger() & 0xFFFF;
                }
            }
        }
        size_t slotCount = header->slotCount();
        if (fixedFields > slotCount) {
            return PrimitiveResult::Failure;
        }
        size_t indexableSize = slotCount - fixedFields;
        if (arrayIndex >= indexableSize) {
            return PrimitiveResult::Failure;
        }
        size_t actualSlot = fixedFields + arrayIndex;
        header->slotAtPut(actualSlot, value);
        primitiveSuccess(value);
        return PrimitiveResult::Success;
    } else if (header->isCompiledMethod()) {
        // CompiledMethods: at:put: modifies bytecodes, not literals
        // Use objectAt:put: (primitive 69) for literal modification
        if (!value.isSmallInteger()) {
            return PrimitiveResult::Failure;
        }
        int64_t byteValue = value.asSmallInteger();
        if (byteValue < 0 || byteValue > 255) {
            return PrimitiveResult::Failure;
        }

        // CompiledMethod at:put: accesses raw bytes directly
        size_t totalBytes = header->byteSize();

        if (arrayIndex >= totalBytes) {
            return PrimitiveResult::Failure;
        }

        header->bytes()[arrayIndex] = static_cast<uint8_t>(byteValue);
        primitiveSuccess(value);
        return PrimitiveResult::Success;
    }

    // Handle word-indexed objects (formats 9-15)
    uint8_t fmtVal = static_cast<uint8_t>(header->format());

    // 64-bit word objects (format 9): DoubleWordArray
    if (fmtVal == 9) {
        size_t numElements = header->slotCount();
        if (arrayIndex >= numElements) {
            return PrimitiveResult::Failure;
        }
        uint64_t word;
        if (value.isSmallInteger()) {
            int64_t wordVal = value.asSmallInteger();
            if (wordVal < 0) return PrimitiveResult::Failure;
            word = static_cast<uint64_t>(wordVal);
        } else if (value.isObject()) {
            // Must be LargePositiveInteger
            bool isNeg = false;
            if (!isLargeInteger(memory_, value, isNeg) || isNeg) {
                return PrimitiveResult::Failure;
            }
            std::vector<uint8_t> mag = extractMagnitude(memory_, value);
            if (mag.size() > 8) return PrimitiveResult::Failure;
            word = 0;
            for (size_t i = 0; i < mag.size(); i++) {
                word |= static_cast<uint64_t>(mag[i]) << (i * 8);
            }
        } else {
            return PrimitiveResult::Failure;
        }
        memory_.storeWord64(arrayIndex, rcvr, word);
        primitiveSuccess(value);
        return PrimitiveResult::Success;
    }

    // 32-bit word objects (format 10-11): WideString, WordArray
    if (fmtVal >= 10 && fmtVal <= 11) {
        if (!value.isSmallInteger()) {
            return PrimitiveResult::Failure;
        }
        int64_t wordValue = value.asSmallInteger();
        if (wordValue < 0 || wordValue > 0xFFFFFFFF) {
            return PrimitiveResult::Failure;
        }
        size_t numElements = header->slotCount() * 2 - (fmtVal - 10);
        if (arrayIndex >= numElements) {
            return PrimitiveResult::Failure;
        }
        uint32_t word = static_cast<uint32_t>(wordValue);
        std::memcpy(header->bytes() + arrayIndex * 4, &word, 4);
        primitiveSuccess(value);
        return PrimitiveResult::Success;
    }

    // 16-bit word objects (format 12-15): DoubleByteArray
    if (fmtVal >= 12 && fmtVal <= 15) {
        if (!value.isSmallInteger()) {
            return PrimitiveResult::Failure;
        }
        int64_t wordValue = value.asSmallInteger();
        if (wordValue < 0 || wordValue > 0xFFFF) {
            return PrimitiveResult::Failure;
        }
        size_t numElements = header->slotCount() * 4 - (fmtVal - 12);
        if (arrayIndex >= numElements) {
            return PrimitiveResult::Failure;
        }
        uint16_t word = static_cast<uint16_t>(wordValue);
        std::memcpy(header->bytes() + arrayIndex * 2, &word, 2);
        primitiveSuccess(value);
        return PrimitiveResult::Success;
    }

    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveSize(int argCount) {
    // Primitive 62: Return the indexable size of an object
    // Per official VM: fails for non-indexable objects (format < 2)
    // Returns: indexable size = totalSlots - fixedFields
    Oop rcvr = stackValue(argCount);

    if (!rcvr.isObject()) {
        return PrimitiveResult::Failure;  // Immediates fail
    }

    ObjectHeader* header = rcvr.asObjectPtr();
    ObjectFormat fmt = header->format();
    size_t totalSlots = header->slotCount();
    size_t size;

    // Per official VM: fail for non-indexable objects (format 0-1)
    if (fmt == ObjectFormat::ZeroSized || fmt == ObjectFormat::FixedSize) {
        return PrimitiveResult::Failure;
    }

    if (header->isBytesObject()) {
        // Byte objects: return byte count
        size = header->byteSize();
    } else if (header->isCompiledMethod()) {
        // CompiledMethods: return byte size (literal frame + bytecodes)
        size = header->byteSize();
    } else if (fmt == ObjectFormat::IndexableWithFixed) {
        // Objects with both fixed and indexable fields (e.g., Context)
        // Get fixed field count from class instance specification
        Oop objClass = memory_.classOf(rcvr);
        size_t fixedFields = 0;
        if (objClass.isObject()) {
            Oop instSpec = memory_.fetchPointer(2, objClass);
            if (instSpec.isSmallInteger()) {
                fixedFields = instSpec.asSmallInteger() & 0xFFFF;
            }
        }
        size = (totalSlots > fixedFields) ? totalSlots - fixedFields : 0;
    } else if (fmt == ObjectFormat::Indexable) {
        // Pure indexable objects (Array): all slots are indexable
        size = totalSlots;
    } else if (fmt == ObjectFormat::Weak || fmt == ObjectFormat::WeakWithFixed) {
        // Weak objects - similar to indexable
        Oop objClass = memory_.classOf(rcvr);
        size_t fixedFields = 0;
        if (objClass.isObject()) {
            Oop instSpec = memory_.fetchPointer(2, objClass);
            if (instSpec.isSmallInteger()) {
                fixedFields = instSpec.asSmallInteger() & 0xFFFF;
            }
        }
        size = (totalSlots > fixedFields) ? totalSlots - fixedFields : 0;
    } else if (fmt == ObjectFormat::Indexable64) {
        // 64-bit word array: each 64-bit slot = 1 indexable element
        size = totalSlots;
    } else if (fmt >= ObjectFormat::Indexable32 && fmt <= ObjectFormat::Indexable32Odd) {
        // 32-bit word arrays
        size = totalSlots * 2 - (fmt == ObjectFormat::Indexable32Odd ? 1 : 0);
    } else if (fmt >= ObjectFormat::Indexable16 && fmt <= ObjectFormat::Indexable16_3) {
        // 16-bit word arrays
        size = totalSlots * 4 - (static_cast<int>(fmt) - static_cast<int>(ObjectFormat::Indexable16));
    } else {
        // Default: return slot count
        size = totalSlots;
    }

    if (!Oop::canBeSmallInteger(static_cast<int64_t>(size))) {
        return PrimitiveResult::Failure;
    }

    popN(argCount + 1);
    push(Oop::fromSmallInteger(static_cast<int64_t>(size)));
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveInstVarAt(int argCount) {
    Oop index = stackValue(0);
    Oop rcvr = stackValue(1);

    if (!index.isSmallInteger() || !rcvr.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Follow forwarding pointers (created by become:)
    rcvr = memory_.followForwarded(rcvr);

    int64_t idx = index.asSmallInteger();
    if (idx < 1) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* header = rcvr.asObjectPtr();
    auto fmt = header->format();
    size_t totalLength = header->slotCount();

    // Calculate fixed fields based on format (per official Pharo VM)
    size_t fixedFields;
    if (fmt == ObjectFormat::Indexable || static_cast<int>(fmt) >= 9) {
        // Format 2 (pure indexable like Array) or 64-bit indexable: no fixed fields
        fixedFields = 0;
    } else if (static_cast<int>(fmt) < 2) {
        // Format 0-1 (zero-sized or fixed-only): all slots are fixed
        fixedFields = totalLength;
    } else {
        // Formats 3-8 (indexable with fixed fields): look up from class
        Oop objClass = memory_.classOf(rcvr);
        if (objClass.isObject()) {
            Oop formatObj = memory_.fetchPointer(2, objClass);  // InstanceSpecificationIndex
            if (formatObj.isSmallInteger()) {
                fixedFields = formatObj.asSmallInteger() & 0xFFFF;  // Low 16 bits
            } else {
                fixedFields = 0;
            }
        } else {
            fixedFields = 0;
        }
    }

    // Validate index is within fixed fields range (1-based)
    if (static_cast<size_t>(idx) > fixedFields) {
        return PrimitiveResult::Failure;
    }

    size_t instVarIndex = static_cast<size_t>(idx - 1);
    primitiveSuccess(header->slotAt(instVarIndex));
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveInstVarAtPut(int argCount) {
    Oop value = stackValue(0);
    Oop index = stackValue(1);
    Oop rcvr = stackValue(2);

    // TRACE: Log ALL instVarAtPut calls with Process receiver
    static FILE* allInstVarLog = nullptr;
    static int allInstVarCount = 0;
    if (!allInstVarLog) allInstVarLog = nullptr;
    if (allInstVarLog && rcvr.isObject() && rcvr.rawBits() > 0x10000) {
        Oop cls = memory_.classOf(rcvr);
        if (cls.isObject()) {
            Oop clsName = memory_.fetchPointer(6, cls);
            if (clsName.isObject() && clsName.rawBits() > 0x10000) {
                ObjectHeader* cnHdr = clsName.asObjectPtr();
                if (cnHdr->isBytesObject() && cnHdr->byteSize() < 50) {
                    std::string name((char*)cnHdr->bytes(), cnHdr->byteSize());
                    if (name == "Process" && allInstVarCount < 30) {
                        allInstVarCount++;
                        fprintf(allInstVarLog, "[INSTVAR #%d] Process 0x%llx instVarAt:%lld put:0x%llx\n",
                                allInstVarCount, (unsigned long long)rcvr.rawBits(),
                                index.isSmallInteger() ? index.asSmallInteger() : -1,
                                (unsigned long long)value.rawBits());
                        fflush(allInstVarLog);
                    }
                }
            }
        }
    }

    // TRACE: Log when Process suspendedContext (instVar 2) is written
    if (index.isSmallInteger() && index.asSmallInteger() == 2 && rcvr.isObject()) {
        Oop cls = memory_.classOf(rcvr);
        if (cls.isObject()) {
            Oop clsName = memory_.fetchPointer(6, cls);
            if (clsName.isObject() && clsName.rawBits() > 0x10000) {
                ObjectHeader* cnHdr = clsName.asObjectPtr();
                if (cnHdr->isBytesObject() && cnHdr->byteSize() == 7) {
                    std::string name((char*)cnHdr->bytes(), 7);
                    if (name == "Process") {
                        static FILE* procCtxLog = nullptr;
                        static int procCtxCount = 0;
                        procCtxCount++;
                        if (procCtxCount <= 20) {
                            if (!procCtxLog) procCtxLog = nullptr;
                            if (procCtxLog) {
                                fprintf(procCtxLog, "[PROC-INSTVAR #%d] Process 0x%llx suspendedContext := 0x%llx\n",
                                        procCtxCount, (unsigned long long)rcvr.rawBits(),
                                        (unsigned long long)value.rawBits());
                                // Show what method the context will run
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
                                    fprintf(procCtxLog, "  context method: #%s\n", methName.c_str());
                                    // Also show context's sender
                                    Oop sender = memory_.fetchPointer(0, value);
                                    fprintf(procCtxLog, "  context sender: 0x%llx (isNil=%d)\n",
                                            (unsigned long long)sender.rawBits(),
                                            sender.rawBits() == memory_.nil().rawBits() ? 1 : 0);
                                } else {
                                    fprintf(procCtxLog, "  value is nil or invalid\n");
                                }
                                fflush(procCtxLog);
                            }
                        }
                    }
                }
            }
        }
    }

    if (!index.isSmallInteger() || !rcvr.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Follow forwarding pointers (created by become:)
    rcvr = memory_.followForwarded(rcvr);
    ObjectHeader* header = rcvr.asObjectPtr();

    if (header->isImmutable()) {
        return PrimitiveResult::Failure;
    }

    int64_t idx = index.asSmallInteger();
    if (idx < 1) {
        return PrimitiveResult::Failure;
    }

    auto fmt = header->format();
    size_t totalLength = header->slotCount();

    // Calculate fixed fields based on format (per official Pharo VM)
    size_t fixedFields;
    if (fmt == ObjectFormat::Indexable || static_cast<int>(fmt) >= 9) {
        // Format 2 (pure indexable like Array) or 64-bit indexable: no fixed fields
        fixedFields = 0;
    } else if (static_cast<int>(fmt) < 2) {
        // Format 0-1 (zero-sized or fixed-only): all slots are fixed
        fixedFields = totalLength;
    } else {
        // Formats 3-8 (indexable with fixed fields): look up from class
        Oop objClass = memory_.classOf(rcvr);
        if (objClass.isObject()) {
            Oop formatObj = memory_.fetchPointer(2, objClass);  // InstanceSpecificationIndex
            if (formatObj.isSmallInteger()) {
                fixedFields = formatObj.asSmallInteger() & 0xFFFF;  // Low 16 bits
            } else {
                fixedFields = 0;
            }
        } else {
            fixedFields = 0;
        }
    }

    // Validate index is within fixed fields range (1-based)
    if (static_cast<size_t>(idx) > fixedFields) {
        return PrimitiveResult::Failure;
    }

    size_t instVarIndex = static_cast<size_t>(idx - 1);
    header->slotAtPut(instVarIndex, value);
    primitiveSuccess(value);
    return PrimitiveResult::Success;
}

// Primitive 68: primitiveObjectAt
// Access a literal in a CompiledMethod by 1-based index.
// receiver index primitiveObjectAt -> literal
// Index 1 returns slot 0 (the header), index 2 returns slot 1 (first literal), etc.
PrimitiveResult Interpreter::primitiveObjectAt(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackValue(0);
    Oop rcvr = stackValue(1);

    if (!indexOop.isSmallInteger() || !rcvr.isObject()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* header = rcvr.asObjectPtr();

    // This primitive is defined for CompiledMethods only
    if (!header->isCompiledMethod()) {
        return PrimitiveResult::Failure;
    }

    int64_t index = indexOop.asSmallInteger();
    if (index < 1) {
        return PrimitiveResult::Failure;
    }

    // Get literal count from method header (slot 0)
    Oop methodHeaderOop = header->slotAt(0);
    if (!methodHeaderOop.isSmallInteger()) {
        // In Cog VMs, slot 0 can be a CogMethod pointer when JIT-compiled.
        // For saved images, this should always be a SmallInteger. Log failures.
        static int objAtFailCount = 0;
        if (objAtFailCount++ < 5) {
            std::cerr << "[PRIM68-FAIL] objectAt: slot 0 of CompiledMethod 0x" << std::hex
                      << rcvr.rawBits() << " is not SmallInteger: raw=0x"
                      << methodHeaderOop.rawBits() << " isObj=" << methodHeaderOop.isObject()
                      << " tag=" << (methodHeaderOop.rawBits() & 7) << std::dec << "\n";
        }
        return PrimitiveResult::Failure;
    }
    int64_t headerBits = methodHeaderOop.asSmallInteger();
    size_t numLiterals = headerBits & 0x7FFF;

    // LiteralStart is 1 (header at slot 0, literals start at slot 1)
    // Valid indices are 1 to (numLiterals + 1)
    // Index 1 = header (slot 0), index 2 = literal 0 (slot 1), etc.
    size_t maxIndex = numLiterals + 1;  // +1 for header

    if (static_cast<size_t>(index) > maxIndex) {
        return PrimitiveResult::Failure;
    }

    // Fetch slot at (index - 1)
    Oop result = header->slotAt(static_cast<size_t>(index - 1));
    popN(2);
    push(result);
    return PrimitiveResult::Success;
}

// Primitive 69: primitiveObjectAtPut
// Store a literal in a CompiledMethod by 1-based index.
// receiver index value primitiveObjectAtPut -> value
PrimitiveResult Interpreter::primitiveObjectAtPut(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop value = stackValue(0);
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!indexOop.isSmallInteger() || !rcvr.isObject()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* header = rcvr.asObjectPtr();

    // This primitive is defined for CompiledMethods only
    if (!header->isCompiledMethod()) {
        return PrimitiveResult::Failure;
    }

    // Check immutability
    if (header->isImmutable()) {
        return PrimitiveResult::Failure;
    }

    int64_t index = indexOop.asSmallInteger();
    if (index < 1) {
        return PrimitiveResult::Failure;
    }

    // Get literal count from current method header (slot 0)
    Oop currentHeaderOop = header->slotAt(0);
    if (!currentHeaderOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t currentHeaderBits = currentHeaderOop.asSmallInteger();
    size_t currentLiteralCount = currentHeaderBits & 0x7FFF;
    size_t maxIndex = currentLiteralCount + 1;

    // Per official VM: when storing at index 1 (method header):
    // 1. Value must be a SmallInteger
    // 2. Literal count in new header must match current header's literal count
    // This prevents corruption of the method's literal frame
    if (index == 1) {
        if (!value.isSmallInteger()) {
            return PrimitiveResult::Failure;
        }
        int64_t newHeaderBits = value.asSmallInteger();
        size_t newLiteralCount = newHeaderBits & 0x7FFF;
        if (newLiteralCount != currentLiteralCount) {
            return PrimitiveResult::Failure;
        }
    }

    if (static_cast<size_t>(index) > maxIndex) {
        return PrimitiveResult::Failure;
    }

    // Store value at (index - 1)
    header->slotAtPut(static_cast<size_t>(index - 1), value);
    popN(3);
    push(value);
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveBasicAt(int argCount) {
    return primitiveAt(argCount);  // Same behavior
}

PrimitiveResult Interpreter::primitiveBasicAtPut(int argCount) {
    return primitiveAtPut(argCount);  // Same behavior
}

PrimitiveResult Interpreter::primitiveBasicSize(int argCount) {
    return primitiveSize(argCount);  // Same behavior
}

// ===== OBJECT CREATION PRIMITIVES (70-71) =====

PrimitiveResult Interpreter::primitiveNew(int argCount) {
    Oop rcvr = stackValue(0);  // Class

    if (!rcvr.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Get instance spec from class
    // Slot 2 is the format/instSize (instance specification)
    Oop formatObj = memory_.fetchPointer(2, rcvr);
    if (!formatObj.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t instSpec = formatObj.asSmallInteger();
    size_t instSize = instSpec & 0xFFFF;  // Low 16 bits are instance size

    // Bits 16-20 encode the object format: 0=zero-sized, 1=fixed, 2+=variable
    // For variable-size classes, basicNew creates an instance with 0 indexed slots
    // (only the fixed fields). The official VM allows this.
    int instFormat = (instSpec >> 16) & 0x1F;

    uint32_t classIndex = memory_.indexOfClass(rcvr);
    if (classIndex == 0) {
        classIndex = memory_.registerClass(rcvr);
    }

    // TRACE: Log OSSDL2Driver creation
    {
        Oop clsName = memory_.fetchPointer(6, rcvr);
        if (clsName.isObject() && clsName.rawBits() > 0x10000) {
            ObjectHeader* cnH = clsName.asObjectPtr();
            if (cnH->isBytesObject() && cnH->byteSize() == 12 &&
                memcmp(cnH->bytes(), "OSSDL2Driver", 12) == 0) {
                static FILE* sdlNewLog = nullptr;
                if (sdlNewLog) {
                    fprintf(sdlNewLog, "[SDL2-NEW] Creating OSSDL2Driver instance! classIdx=%u instSize=%zu step=%llu\n",
                            classIndex, instSize, (unsigned long long)g_stepNum);
                    fflush(sdlNewLog);
                }
            }
        }
    }

    // Choose correct object format based on instSpec
    ObjectFormat objFormat;
    switch (instFormat) {
        case 0: objFormat = ObjectFormat::ZeroSized; break;
        case 1: objFormat = ObjectFormat::FixedSize; break;
        case 2: objFormat = ObjectFormat::Indexable; break;
        case 3: objFormat = ObjectFormat::IndexableWithFixed; break;
        case 4: objFormat = ObjectFormat::Weak; break;
        case 5: objFormat = ObjectFormat::WeakWithFixed; break;
        default: objFormat = ObjectFormat::FixedSize; break;
    }

    Oop newObj = memory_.allocateSlots(classIndex, instSize, objFormat);

    if (newObj.isNil() || newObj.rawBits() == memory_.nil().rawBits()) {
        return PrimitiveResult::Failure;  // Out of memory
    }

    primitiveSuccess(newObj);
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveNewWithArg(int argCount) {
    // Handle two variants:
    // argCount=1: basicNew: size   -> stackValue(0)=size, stackValue(1)=class
    // argCount=2: basicNew: size header: header -> stackValue(0)=header, stackValue(1)=size, stackValue(2)=class
    Oop headerOop = Oop::nil();
    Oop sizeOop;
    Oop rcvr;

    if (argCount == 2) {
        // basicNew:header: for CompiledMethod
        headerOop = stackValue(0);
        sizeOop = stackValue(1);
        rcvr = stackValue(2);  // Class
    } else {
        sizeOop = stackValue(0);
        rcvr = stackValue(1);  // Class
    }


    // Debug: trace all calls during startup
    static int newArgCallCount = 0;
    newArgCallCount++;
    auto logNewArgFail = [&](const char* reason) {
        if (newArgCallCount <= 10) {
            FILE* f = nullptr;
            if (f) {
                std::string className = "?";
                if (rcvr.isObject() && rcvr.rawBits() > 0x10000) {
                    ObjectHeader* ch = rcvr.asObjectPtr();
                    if (ch->slotCount() >= 7) {
                        Oop nm = memory_.fetchPointer(6, rcvr);
                        if (nm.isObject() && nm.rawBits() > 0x10000) {
                            ObjectHeader* nh = nm.asObjectPtr();
                            if (nh->isBytesObject() && nh->byteSize() < 50)
                                className = std::string((char*)nh->bytes(), nh->byteSize());
                        }
                    }
                }
                // Get class name of size arg
                std::string sizeClassName = "?";
                if (sizeOop.isNil()) { sizeClassName = "nil"; }
                else if (sizeOop.isSmallInteger()) { sizeClassName = "SmallInt(" + std::to_string(sizeOop.asSmallInteger()) + ")"; }
                else if (sizeOop.isObject() && sizeOop.rawBits() > 0x10000) {
                    Oop scls = memory_.classOf(sizeOop);
                    if (scls.isObject() && scls.rawBits() > 0x10000) {
                        Oop snm = memory_.fetchPointer(6, scls);
                        if (snm.isObject() && snm.rawBits() > 0x10000) {
                            ObjectHeader* snH = snm.asObjectPtr();
                            if (snH->isBytesObject() && snH->byteSize() < 80)
                                sizeClassName = std::string((char*)snH->bytes(), snH->byteSize());
                        }
                    }
                }
                fprintf(f, "[basicNew: #%d] FAIL %s class=%s sizeArg=%s(0x%llx) sizeIsSmallInt=%d sizeIsObj=%d argCount=%d step=%llu\n",
                        newArgCallCount, reason, className.c_str(), sizeClassName.c_str(),
                        (unsigned long long)sizeOop.rawBits(), sizeOop.isSmallInteger() ? 1 : 0,
                        sizeOop.isObject() ? 1 : 0, argCount, (unsigned long long)g_stepNum);
                // Dump current method selector
                {
                    std::string curSel = "?";
                    if (method_.isObject() && memory_.isValidPointer(method_)) {
                        Oop hdrOop = memory_.fetchPointer(0, method_);
                        if (hdrOop.isSmallInteger()) {
                            int64_t hbits = hdrOop.asSmallInteger();
                            int nLit = hbits & 0x7FFF;
                            // Try penultimate literal (selector in some layouts)
                            for (int li = nLit; li >= std::max(1, nLit-1); li--) {
                                Oop lit = memory_.fetchPointer(li, method_);
                                if (lit.isObject() && memory_.isValidPointer(lit)) {
                                    ObjectHeader* lh = lit.asObjectPtr();
                                    if (lh->isBytesObject() && lh->byteSize() < 100 && lh->byteSize() > 0) {
                                        curSel = std::string((char*)lh->bytes(), lh->byteSize());
                                        break;
                                    }
                                    // Association: slot 0 = key (Symbol), slot 1 = value
                                    if (lh->slotCount() >= 2) {
                                        Oop key = memory_.fetchPointer(0, lit);
                                        if (key.isObject() && memory_.isValidPointer(key)) {
                                            ObjectHeader* kh = key.asObjectPtr();
                                            if (kh->isBytesObject() && kh->byteSize() < 100) {
                                                curSel = std::string((char*)kh->bytes(), kh->byteSize());
                                                break;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    fprintf(f, "[basicNew: #%d] current method selector: #%s frameDepth=%zu\n",
                            newArgCallCount, curSel.c_str(), frameDepth_);
                }
                // Dump call stack from savedFrames
                fprintf(f, "[basicNew: #%d] Call stack:\n", newArgCallCount);
                for (size_t fi = 0; fi < frameDepth_ && fi < 25; fi++) {
                    size_t idx = frameDepth_ - 1 - fi;
                    const auto& sf = savedFrames_[idx];
                    std::string frameSel = "?";
                    if (sf.savedMethod.isObject() && sf.savedMethod.rawBits() > 0x10000) {
                        Oop sfHdr = memory_.fetchPointer(0, sf.savedMethod);
                        if (sfHdr.isSmallInteger()) {
                            int64_t hv = sfHdr.asSmallInteger();
                            int nl = hv & 0x7FFF;
                            if (nl >= 2 && nl < 100) {
                                Oop sel = memory_.fetchPointer(nl - 1, sf.savedMethod);
                                if (sel.isObject() && sel.rawBits() > 0x10000) {
                                    ObjectHeader* sh = sel.asObjectPtr();
                                    if (sh->isBytesObject() && sh->byteSize() < 80) {
                                        frameSel = std::string((char*)sh->bytes(), sh->byteSize());
                                    }
                                    // Check if it's an Association (slot 0 = key)
                                    else if (sh->slotCount() >= 2) {
                                        Oop key = memory_.fetchPointer(0, sel);
                                        if (key.isObject() && key.rawBits() > 0x10000) {
                                            ObjectHeader* kh = key.asObjectPtr();
                                            if (kh->isBytesObject() && kh->byteSize() < 80) {
                                                frameSel = std::string((char*)kh->bytes(), kh->byteSize());
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    fprintf(f, "  [%zu] #%s\n", fi, frameSel.c_str());
                }
                fclose(f);
            }
        }
    };

    if (!rcvr.isObject()) {
        logNewArgFail("rcvr-not-object");
        return PrimitiveResult::Failure;
    }

    // Handle size argument - can be SmallInteger or LargePositiveInteger
    int64_t indexableSize = 0;
    if (sizeOop.isSmallInteger()) {
        indexableSize = sizeOop.asSmallInteger();
    } else if (sizeOop.isObject() && sizeOop.rawBits() > 0x10000) {
        // Extract value from LargePositiveInteger (little-endian bytes)
        ObjectHeader* hdr = sizeOop.asObjectPtr();
        if (hdr->isBytesObject() && hdr->byteSize() <= 8) {
            uint8_t* bytes = (uint8_t*)hdr->bytes();
            size_t byteLen = hdr->byteSize();
            for (size_t i = 0; i < byteLen; i++) {
                indexableSize |= ((int64_t)bytes[i]) << (i * 8);
            }
        } else {
            // Log details about the non-integer argument object
            if (newArgCallCount <= 10) {
                FILE* f = nullptr;
                if (f) {
                    fprintf(f, "[basicNew:] arg byteSize=%zu isBytesObj=%d fmt=%d classIdx=%u slots=%zu\n",
                            hdr->byteSize(), hdr->isBytesObject() ? 1 : 0,
                            (int)hdr->format(), hdr->classIndex(), hdr->slotCount());
                    // Resolve class name of the argument
                    Oop argClass = memory_.classOf(sizeOop);
                    if (argClass.isObject() && argClass.rawBits() > 0x10000) {
                        ObjectHeader* acHdr = argClass.asObjectPtr();
                        if (acHdr->slotCount() >= 7) {
                            Oop acName = memory_.fetchPointer(6, argClass);
                            if (acName.isObject() && acName.rawBits() > 0x10000) {
                                ObjectHeader* anHdr = acName.asObjectPtr();
                                if (anHdr->isBytesObject() && anHdr->byteSize() < 100) {
                                    fprintf(f, "[basicNew:] arg class name: %.*s\n",
                                            (int)anHdr->byteSize(), (char*)anHdr->bytes());
                                }
                            }
                        }
                    }
                    // Dump slot values
                    for (size_t s = 0; s < std::min(hdr->slotCount(), (size_t)4); s++) {
                        Oop slotVal = memory_.fetchPointer(s, sizeOop);
                        fprintf(f, "[basicNew:] arg slot[%zu] = 0x%llx isSmallInt=%d",
                                s, (unsigned long long)slotVal.rawBits(), slotVal.isSmallInteger() ? 1 : 0);
                        if (slotVal.isSmallInteger()) {
                            fprintf(f, " value=%lld", (long long)slotVal.asSmallInteger());
                        } else if (slotVal.isObject() && slotVal.rawBits() > 0x10000) {
                            // Try to get class name of slot value
                            Oop slotClass = memory_.classOf(slotVal);
                            if (slotClass.isObject() && slotClass.rawBits() > 0x10000) {
                                ObjectHeader* scHdr = slotClass.asObjectPtr();
                                if (scHdr->slotCount() >= 7) {
                                    Oop scName = memory_.fetchPointer(6, slotClass);
                                    if (scName.isObject() && scName.rawBits() > 0x10000) {
                                        ObjectHeader* snHdr = scName.asObjectPtr();
                                        if (snHdr->isBytesObject() && snHdr->byteSize() < 100) {
                                            fprintf(f, " class=%.*s", (int)snHdr->byteSize(), (char*)snHdr->bytes());
                                        }
                                    }
                                }
                            }
                        }
                        fprintf(f, "\n");
                    }
                    // Dump stack context: show several stack values around the failure
                    fprintf(f, "[basicNew:] stack dump (argCount=%d frameDepth=%zu):\n", argCount, frameDepth_);
                    for (int sv = 0; sv < 6; sv++) {
                        Oop val = stackValue(sv);
                        fprintf(f, "  stackValue(%d) = 0x%llx isSmallInt=%d isObj=%d\n",
                                sv, (unsigned long long)val.rawBits(),
                                val.isSmallInteger() ? 1 : 0, val.isObject() ? 1 : 0);
                    }
                    fclose(f);
                }
            }
            logNewArgFail("non-integer-arg");
            return PrimitiveResult::Failure;
        }
    } else {
        logNewArgFail("size-not-int-or-obj");
        return PrimitiveResult::Failure;
    }
    if (indexableSize < 0) {
        logNewArgFail("size-negative");
        return PrimitiveResult::Failure;
    }

    // Get class format to determine if bytes or pointers
    Oop formatObj = memory_.fetchPointer(2, rcvr);
    if (!formatObj.isSmallInteger()) {
        logNewArgFail("format-not-smallint");
        return PrimitiveResult::Failure;
    }

    int64_t format = formatObj.asSmallInteger();
    size_t fixedSize = format & 0xFFFF;
    int instSpec = (format >> 16) & 0x1F;  // Instance specification (0-31)

    // Per official VM: validate this is a variable-sized class (format >= 2)
    // Format 0-1 are fixed-size, format 2+ are variable (Array, String, etc.)
    if (instSpec < 2) {
        logNewArgFail("not-variable");
        return PrimitiveResult::Failure;
    }

    bool isBytes = instSpec >= 16;
    bool isWords32 = (instSpec >= 10 && instSpec <= 11);
    bool isWords16 = (instSpec >= 12 && instSpec <= 15);
    bool isWords64 = (instSpec == 9);

    uint32_t classIndex = memory_.indexOfClass(rcvr);

    Oop newObj;

    // CompiledMethod (instSpec 24) is special: it has pointer slots for literals
    // followed by bytecode bytes. The header encodes numLiterals.
    // basicNew: size header: header allocates (1+numLiterals)*8 + size bytes
    if (instSpec == 24 && !headerOop.isNil() && headerOop.isSmallInteger()) {
        int64_t headerBits = headerOop.asSmallInteger();
        int numLiterals = headerBits & 0x7FFF;  // bits 0-14
        size_t bytecodeSize = static_cast<size_t>(indexableSize);

        // Total slots = 1 (header slot) + numLiterals
        // Total bytes = slots*8 + bytecodeSize
        size_t numSlots = 1 + numLiterals;
        size_t totalBytes = numSlots * 8 + bytecodeSize;

        // CompiledMethod uses format 24 (Indexable bytes with 0 odd)
        // But we store as CompiledMethod format which includes both slots and bytes
        newObj = memory_.allocateCompiledMethod(classIndex, numSlots, bytecodeSize);

        if (!newObj.isNil()) {
            // Store the header in slot 0
            memory_.storePointer(0, newObj, headerOop);
        }
    } else if (isBytes) {
        newObj = memory_.allocateBytes(classIndex, static_cast<size_t>(indexableSize));
    } else if (isWords32) {
        // 32-bit word objects (WideString, WordArray) - instSpec 10-11
        size_t numElements = static_cast<size_t>(indexableSize);
        size_t byteCount = numElements * 4;
        // Allocate as bytes then fix the format
        newObj = memory_.allocateBytes(classIndex, byteCount);
        if (!newObj.isNil()) {
            ObjectHeader* hdr = newObj.asObjectPtr();
            // Set correct 32-bit format: 10 (even count) or 11 (odd count)
            size_t padding32 = (hdr->slotCount() * 2) - numElements;
            ObjectFormat fmt = static_cast<ObjectFormat>(
                static_cast<int>(ObjectFormat::Indexable32) + padding32);
            hdr->setFormat(fmt);
        }
    } else if (isWords16) {
        // 16-bit word objects (DoubleByteArray) - instSpec 12-15
        size_t numElements = static_cast<size_t>(indexableSize);
        size_t byteCount = numElements * 2;
        newObj = memory_.allocateBytes(classIndex, byteCount);
        if (!newObj.isNil()) {
            ObjectHeader* hdr = newObj.asObjectPtr();
            size_t padding16 = (hdr->slotCount() * 4) - numElements;
            ObjectFormat fmt = static_cast<ObjectFormat>(
                static_cast<int>(ObjectFormat::Indexable16) + padding16);
            hdr->setFormat(fmt);
        }
    } else if (isWords64) {
        // 64-bit word objects - instSpec 9
        size_t numElements = static_cast<size_t>(indexableSize);
        size_t byteCount = numElements * 8;
        newObj = memory_.allocateBytes(classIndex, byteCount);
        if (!newObj.isNil()) {
            ObjectHeader* hdr = newObj.asObjectPtr();
            hdr->setFormat(ObjectFormat::Indexable64);
        }
    } else {
        size_t totalSlots = fixedSize + static_cast<size_t>(indexableSize);
        // Choose correct format based on instSpec:
        // instSpec 2 = variable pointers only (Array)
        // instSpec 3 = variable pointers with fixed fields (Context, CompiledMethod)
        // instSpec 4 = weak variable pointers only (WeakArray)
        // instSpec 5 = weak variable pointers with fixed fields (WeakAnnouncementSubscription)
        ObjectFormat objFormat;
        switch (instSpec) {
            case 3: objFormat = ObjectFormat::IndexableWithFixed; break;
            case 4: objFormat = ObjectFormat::Weak; break;
            case 5: objFormat = ObjectFormat::WeakWithFixed; break;
            default: objFormat = ObjectFormat::Indexable; break;
        }
        newObj = memory_.allocateSlots(classIndex, totalSlots, objFormat);
    }

    if (newObj.isNil()) {
        logNewArgFail("alloc-returned-nil");
        return PrimitiveResult::Failure;
    }

    primitiveSuccess(newObj);
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveShallowCopy(int argCount) {
    Oop rcvr = stackValue(0);
    Oop copy = memory_.shallowCopy(rcvr);

    if (copy.isNil() && !rcvr.isNil()) {
        return PrimitiveResult::Failure;
    }

    primitiveSuccess(copy);
    return PrimitiveResult::Success;
}

// ===== IDENTITY PRIMITIVES =====

PrimitiveResult Interpreter::primitiveIdentityHash(int argCount) {
    Oop rcvr = stackValue(argCount);  // Receiver is under arguments

    uint32_t hash;
    if (rcvr.isSmallInteger()) {
        // SmallInteger identity hash: use the value itself (masked to positive range)
        hash = static_cast<uint32_t>(rcvr.asSmallInteger()) & 0x3FFFFF;
    } else if (rcvr.isCharacter()) {
        // Character identity hash: use the character value
        hash = static_cast<uint32_t>(rcvr.asCharacter()) & 0x3FFFFF;
    } else if (rcvr.isSmallFloat()) {
        // SmallFloat identity hash: hash the raw bits
        uint64_t bits = rcvr.rawBits();
        hash = static_cast<uint32_t>((bits >> 32) ^ bits) & 0x3FFFFF;
    } else if (rcvr.isObject()) {
        // Follow forwarding pointers (created by become:)
        rcvr = memory_.followForwarded(rcvr);
        // identityHashOf handles lazy hash generation and caching
        hash = memory_.identityHashOf(rcvr);
    } else {
        return PrimitiveResult::Failure;
    }

    popN(argCount + 1);
    push(Oop::fromSmallInteger(hash));
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveClass(int argCount) {
    // Primitive 111 returns the class of the top-of-stack value.
    // For Object>>class (argCount=0): stackValue(0) = receiver
    // For Context>>objectClass: (argCount=1): stackValue(0) = the argument
    // The standard VM always operates on stackTop, not the receiver.
    Oop target = stackValue(0);

    // Follow forwarding pointers (created by become:)
    target = memory_.followForwarded(target);

    Oop classOop = memory_.classOf(target);
    popN(argCount + 1);
    push(classOop);
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveIdentical(int argCount) {
    Oop arg = stackValue(0);   // otherObject
    Oop rcvr = stackValue(1);  // thisObject

    // Follow forwarding pointers (created by become:)
    arg = memory_.followForwarded(arg);
    rcvr = memory_.followForwarded(rcvr);

    bool result = (rcvr == arg);

    pop();
    pop();
    push(result ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveNotIdentical(int argCount) {
    Oop arg = stackValue(0);   // otherObject
    Oop rcvr = stackValue(1);  // thisObject

    // Follow forwarding pointers (created by become:)
    arg = memory_.followForwarded(arg);
    rcvr = memory_.followForwarded(rcvr);

    bool result = (rcvr != arg);
    pop();
    pop();
    push(result ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveAsCharacter(int argCount) {
    // Primitive 170: Integer >> asCharacter - convert integer to Character
    Oop rcvr = stackValue(0);

    if (!rcvr.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t value = rcvr.asSmallInteger();

    // Official VM allows character codes 0 to (2^30 - 1)
    // This is larger than Unicode (0x10FFFF) to support extended uses
    constexpr int64_t MaxCharacterCode = (1LL << 30) - 1;  // 0x3FFFFFFF
    if (value < 0 || value > MaxCharacterCode) {
        return PrimitiveResult::Failure;
    }

    primitiveSuccess(Oop::fromCharacter(static_cast<uint32_t>(value)));
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveAsInteger(int argCount) {
    // Primitive 171: Character >> asInteger - convert Character to integer
    Oop rcvr = stackValue(0);

    if (!rcvr.isCharacter()) {
        return PrimitiveResult::Failure;
    }

    uint32_t codePoint = rcvr.asCharacter();
    primitiveSuccess(Oop::fromSmallInteger(codePoint));
    return PrimitiveResult::Success;
}

// ===== STREAM PRIMITIVES (65-67) =====
// Stream object layout (ReadStream, WriteStream, etc.):
// 0: array (the underlying collection)
// 1: position (current index, 0-based internally)
// 2: readLimit (for ReadStream) or writeLimit

static constexpr size_t StreamArrayIndex = 0;
static constexpr size_t StreamPositionIndex = 1;
static constexpr size_t StreamLimitIndex = 2;

// Primitive 65: Stream>>next - get next element and advance position
PrimitiveResult Interpreter::primitiveNext(int argCount) {
    Oop stream = stackTop();

    if (!stream.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Get stream fields
    Oop array = memory_.fetchPointer(StreamArrayIndex, stream);
    Oop positionOop = memory_.fetchPointer(StreamPositionIndex, stream);
    Oop limitOop = memory_.fetchPointer(StreamLimitIndex, stream);

    if (!positionOop.isSmallInteger() || !limitOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t position = positionOop.asSmallInteger();
    int64_t limit = limitOop.asSmallInteger();

    // Check if at end
    if (position >= limit) {
        return PrimitiveResult::Failure;  // At end, let Smalltalk handle it
    }

    if (!array.isObject()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* arrayHeader = array.asObjectPtr();
    ObjectFormat format = arrayHeader->format();

    Oop result;

    // Handle different array types
    if (format >= ObjectFormat::Indexable8 && format <= ObjectFormat::Indexable8_7) {
        // Byte array or String - return Character
        size_t byteCount = memory_.byteSizeOf(array);
        if (static_cast<size_t>(position) >= byteCount) {
            return PrimitiveResult::Failure;
        }
        uint8_t byte = memory_.fetchByte(static_cast<size_t>(position), array);
        result = Oop::fromCharacter(byte);
    } else if (format <= ObjectFormat::Weak) {
        // Pointer array - return element
        size_t slotCount = arrayHeader->slotCount();
        if (static_cast<size_t>(position) >= slotCount) {
            return PrimitiveResult::Failure;
        }
        result = memory_.fetchPointer(static_cast<size_t>(position), array);
    } else {
        // Other formats not supported
        return PrimitiveResult::Failure;
    }

    // Advance position
    memory_.storePointer(StreamPositionIndex, stream,
                         Oop::fromSmallInteger(position + 1));

    pop();
    push(result);
    return PrimitiveResult::Success;
}

// Primitive 66: Stream>>nextPut: - store element and advance position
PrimitiveResult Interpreter::primitiveNextPut(int argCount) {
    if (argCount != 1) {
        return PrimitiveResult::Failure;
    }

    Oop value = stackValue(0);
    Oop stream = stackValue(1);

    if (!stream.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Get stream fields
    Oop array = memory_.fetchPointer(StreamArrayIndex, stream);
    Oop positionOop = memory_.fetchPointer(StreamPositionIndex, stream);
    Oop limitOop = memory_.fetchPointer(StreamLimitIndex, stream);

    if (!positionOop.isSmallInteger() || !limitOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t position = positionOop.asSmallInteger();

    if (!array.isObject()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* arrayHeader = array.asObjectPtr();
    ObjectFormat format = arrayHeader->format();

    // Handle different array types
    if (format >= ObjectFormat::Indexable8 && format <= ObjectFormat::Indexable8_7) {
        // Byte array or String - store byte from Character or SmallInteger
        size_t byteCount = memory_.byteSizeOf(array);
        if (static_cast<size_t>(position) >= byteCount) {
            return PrimitiveResult::Failure;
        }

        uint8_t byte;
        if (value.isCharacter()) {
            byte = static_cast<uint8_t>(value.asCharacter());
        } else if (value.isSmallInteger()) {
            int64_t intVal = value.asSmallInteger();
            if (intVal < 0 || intVal > 255) {
                return PrimitiveResult::Failure;
            }
            byte = static_cast<uint8_t>(intVal);
        } else {
            return PrimitiveResult::Failure;
        }

        memory_.storeByte(static_cast<size_t>(position), array, byte);
    } else if (format <= ObjectFormat::Weak) {
        // Pointer array - store element
        size_t slotCount = arrayHeader->slotCount();
        if (static_cast<size_t>(position) >= slotCount) {
            return PrimitiveResult::Failure;
        }
        memory_.storePointer(static_cast<size_t>(position), array, value);
    } else {
        // Other formats not supported
        return PrimitiveResult::Failure;
    }

    // Advance position
    int64_t newPosition = position + 1;
    memory_.storePointer(StreamPositionIndex, stream,
                         Oop::fromSmallInteger(newPosition));

    // Update limit if we're past it (for WriteStream)
    int64_t limit = limitOop.asSmallInteger();
    if (newPosition > limit) {
        memory_.storePointer(StreamLimitIndex, stream,
                             Oop::fromSmallInteger(newPosition));
    }

    // Return the value that was stored
    popN(2);
    push(value);
    return PrimitiveResult::Success;
}

// Primitive 67: Stream>>atEnd - check if stream is at end
PrimitiveResult Interpreter::primitiveAtEnd(int argCount) {
    Oop stream = stackTop();

    if (!stream.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Get stream fields
    Oop positionOop = memory_.fetchPointer(StreamPositionIndex, stream);
    Oop limitOop = memory_.fetchPointer(StreamLimitIndex, stream);

    if (!positionOop.isSmallInteger() || !limitOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t position = positionOop.asSmallInteger();
    int64_t limit = limitOop.asSmallInteger();

    bool atEnd = (position >= limit);

    pop();
    push(atEnd ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

// ===== BEHAVIOR PRIMITIVES =====

PrimitiveResult Interpreter::primitivePerform(int argCount) {
    // perform: selector
    // perform:with: selector arg1
    // perform:with:with: selector arg1 arg2
    // etc.
    //
    // Stack layout: receiver, selector, arg1, arg2, ... (argN at top)
    // For argCount=1: receiver, selector  (selector at top)
    // For argCount=2: receiver, selector, arg1  (selector at stackValue(1))
    // For argCount=3: receiver, selector, arg1, arg2  (selector at stackValue(2))
    // General: selector at stackValue(argCount - 1)

    if (argCount < 1) return PrimitiveResult::Failure;

    // Selector is BEFORE the additional arguments
    Oop selector = stackValue(argCount - 1);
    int additionalArgs = argCount - 1;

    // Validate selector is a Symbol
    if (!selector.isObject() || selector.rawBits() < 0x10000) {
        return PrimitiveResult::Failure;
    }

    // Remove selector from the middle of the stack, keeping args
    // Stack before: receiver, selector, arg1, ..., argN
    // Stack after:  receiver, arg1, ..., argN
    //
    // Stack memory layout (stackPointer_ points one past top):
    // For argCount=2: stackPointer_[-1]=arg1, stackPointer_[-2]=selector, stackPointer_[-3]=receiver
    // After pop: stackPointer_[-1]=arg1, stackPointer_[-2]=receiver
    //
    // CRITICAL: Must iterate from deepest argument to top to avoid overwriting!
    // Otherwise with multiple args, we overwrite args before copying them.

    // Shift additional arguments down to cover the selector slot
    // Iterate from bottom arg (deepest) to top arg to preserve order
    for (int i = additionalArgs - 1; i >= 0; i--) {
        // Copy arg at position -(i+1) to position -(i+2)
        stackPointer_[-(i + 2)] = stackPointer_[-(i + 1)];
    }
    popN(1);  // Pop the duplicate top slot

    // Send the message with the additional args
    sendSelector(selector, additionalArgs);
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitivePerformWithArgs(int argCount) {
    // perform:withArguments:
    // Stack: receiver, selector, arguments array

    Oop argsArray = stackValue(0);
    Oop selector = stackValue(1);

    // Debug: detect bad selectors
    if (!selector.isObject() || selector.rawBits() < 0x10000) {
        static int badPerformArgsCount = 0;
        if (badPerformArgsCount++ < 5) {
            std::cerr << "[PERFORM-ARGS-DEBUG] Bad selector in primitivePerformWithArgs!"
                      << " selector=0x" << std::hex << selector.rawBits() << std::dec
                      << " isSmallFloat=" << selector.isSmallFloat()
                      << " receiver=0x" << std::hex << stackValue(2).rawBits() << std::dec
                      << "\n";
            // Show method context
            if (method_.isObject() && method_.rawBits() > 0x10000) {
                Oop mHdr = memory_.fetchPointer(0, method_);
                if (mHdr.isSmallInteger()) {
                    int numLits = mHdr.asSmallInteger() & 0x7FFF;
                    if (numLits >= 2) {
                        Oop sel = memory_.fetchPointer(numLits - 1, method_);
                        if (sel.isObject() && sel.rawBits() > 0x10000) {
                            ObjectHeader* selHdr = sel.asObjectPtr();
                            if (selHdr->isBytesObject() && selHdr->byteSize() < 50) {
                                std::cerr << "  in method: #" << std::string((char*)selHdr->bytes(), selHdr->byteSize()) << "\n";
                            }
                        }
                    }
                }
            }
        }
        return PrimitiveResult::Failure;
    }

    if (!argsArray.isObject()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* argsHeader = argsArray.asObjectPtr();

    // Follow forwarding pointers (created by become:)
    argsArray = memory_.followForwarded(argsArray);
    argsHeader = argsArray.asObjectPtr();

    // Validate argsArray is an indexable pointer object (Array)
    ObjectFormat fmt = argsHeader->format();
    if (fmt != ObjectFormat::Indexable) {
        // Not a proper array - could be fixed-size object, byte array, etc.
        return PrimitiveResult::Failure;
    }

    size_t numArgs = argsHeader->slotCount();

    // Pop selector and args array, push args from array
    popN(2);

    for (size_t i = 0; i < numArgs; ++i) {
        push(argsHeader->slotAt(i));
    }

    sendSelector(selector, static_cast<int>(numArgs));
    return PrimitiveResult::Success;
}

// ===== BLOCK PRIMITIVES =====

PrimitiveResult Interpreter::primitiveBlockValue(int argCount) {
    Oop block = stackValue(argCount);

    // Validate block is an object
    if (!block.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Follow forwarding pointers (created by become:)
    block = memory_.followForwarded(block);
    stackValuePut(argCount, block);

    ObjectHeader* blockHdr = block.asObjectPtr();

    // Verify arg count matches block's numArgs
    Oop numArgsObj = memory_.fetchPointer(2, block);
    if (!numArgsObj.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int blockArgCount = static_cast<int>(numArgsObj.asSmallInteger());
    if (blockArgCount != argCount) {
        return PrimitiveResult::Failure;
    }

    // Per official VM: validate slot 1 (compiledBlock for FullBlockClosure)
    // For old-style BlockClosure, slot 1 is SmallInteger (startPC) - OK
    // For FullBlockClosure, slot 1 must be a CompiledMethod
    Oop slot1 = memory_.fetchPointer(1, block);
    if (slot1.isObject()) {
        ObjectHeader* slot1Hdr = slot1.asObjectPtr();
        if (!slot1Hdr->isCompiledMethod()) {
            return PrimitiveResult::Failure;  // Not a valid compiledBlock
        }
    } else if (!slot1.isSmallInteger()) {
        return PrimitiveResult::Failure;  // Neither startPC nor compiledBlock
    }

    activateBlock(block, argCount);
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveBlockValueWithArgs(int argCount) {
    // valueWithArguments: anArray
    Oop argsArray = stackValue(0);
    Oop block = stackValue(1);

    if (!argsArray.isObject()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* argsHeader = argsArray.asObjectPtr();
    size_t numArgs = argsHeader->slotCount();

    // Check block's numArgs
    Oop numArgsObj = memory_.fetchPointer(2, block);
    if (!numArgsObj.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    if (static_cast<size_t>(numArgsObj.asSmallInteger()) != numArgs) {
        return PrimitiveResult::Failure;
    }

    // Pop array, push args from array
    pop();
    for (size_t i = 0; i < numArgs; ++i) {
        push(argsHeader->slotAt(i));
    }

    activateBlock(block, static_cast<int>(numArgs));
    return PrimitiveResult::Success;
}

// Primitive 80: BlockClosure copy
// Creates a BlockClosure from the current context
// Stack: context (receiver), numArgs, startpc
// In modern Pharo this is often replaced by full block closures created by the compiler
PrimitiveResult Interpreter::primitiveBlockCopy(int argCount) {
    // Classic block copy: context blockCopy: numArgs startPc: startPc
    // But the exact calling convention can vary

    if (argCount < 1) {
        return PrimitiveResult::Failure;
    }

    // Get the BlockClosure class
    Oop blockClass = memory_.specialObject(SpecialObjectIndex::ClassBlockClosure);
    if (blockClass.isNil()) {
        return PrimitiveResult::Failure;
    }

    // Get class index for allocation
    uint32_t classIndex = memory_.indexOfClass(blockClass);
    if (classIndex == 0) {
        return PrimitiveResult::Failure;
    }

    // BlockClosure layout:
    // 0: outerContext
    // 1: startpc (or compiledBlock in full closures)
    // 2: numArgs
    // Additional slots for copied values if needed

    Oop outerContext;
    Oop startPc;
    Oop numArgsOop;

    if (argCount == 2) {
        // blockCopy: numArgs startPc: startPc (receiver is context)
        startPc = stackValue(0);
        numArgsOop = stackValue(1);
        outerContext = stackValue(2);  // receiver
    } else if (argCount == 1) {
        // Simplified: just numArgs, startPc derived from method
        numArgsOop = stackValue(0);
        outerContext = stackValue(1);  // receiver
        startPc = Oop::fromSmallInteger(0);  // Will need to be set properly
    } else {
        return PrimitiveResult::Failure;
    }

    if (!numArgsOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    // Allocate a BlockClosure with 3 slots (outerContext, startpc, numArgs)
    Oop closure = memory_.allocateSlots(classIndex, 3, ObjectFormat::FixedSize);
    if (closure.isNil()) {
        return PrimitiveResult::Failure;
    }

    // Initialize the closure
    memory_.storePointer(0, closure, outerContext);  // outerContext
    memory_.storePointer(1, closure, startPc);       // startpc
    memory_.storePointer(2, closure, numArgsOop);    // numArgs

    // Pop arguments and receiver, push result
    popN(argCount + 1);
    push(closure);
    return PrimitiveResult::Success;
}

// Primitive 81: value (BlockClosure>>value with 0 args)
// This is essentially the same as primitiveBlockValue but specifically for 0 args
PrimitiveResult Interpreter::primitiveValue(int argCount) {
    Oop block = stackValue(argCount);

    if (!block.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Verify block expects the right number of arguments
    Oop numArgsObj = memory_.fetchPointer(2, block);
    if (!numArgsObj.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int blockArgCount = static_cast<int>(numArgsObj.asSmallInteger());
    if (blockArgCount != argCount) {
        return PrimitiveResult::Failure;
    }

    activateBlock(block, argCount);
    return PrimitiveResult::Success;
}

// Primitive 82: valueWithArguments: (BlockClosure>>valueWithArguments:)
// Evaluates the block with arguments from an array
PrimitiveResult Interpreter::primitiveValueWithArgs(int argCount) {
    // This is the same as primitiveBlockValueWithArgs
    return primitiveBlockValueWithArgs(argCount);
}

// Primitive 200: Create a closure with copied values
// This is used by the compiler to create closures that capture variables
// Stack: outerContext, numArgs, compiledBlock/startPc, copiedValue1, copiedValue2, ...
PrimitiveResult Interpreter::primitiveClosureCopyWithCopiedValues(int argCount) {
    // In Spur/Cog, this creates a FullBlockClosure or BlockClosure with copied values
    // argCount includes: numCopied values + 3 (outerContext, numArgs, compiledBlock)

    if (argCount < 3) {
        return PrimitiveResult::Failure;
    }

    size_t numCopied = static_cast<size_t>(argCount - 3);

    // Get the BlockClosure or FullBlockClosure class
    Oop blockClass = memory_.specialObject(SpecialObjectIndex::ClassBlockClosure);
    if (blockClass.isNil()) {
        // Try FullBlockClosure
        blockClass = memory_.specialObject(SpecialObjectIndex::ClassFullBlockClosure);
    }
    if (blockClass.isNil()) {
        return PrimitiveResult::Failure;
    }

    uint32_t classIndex = memory_.indexOfClass(blockClass);
    if (classIndex == 0) {
        return PrimitiveResult::Failure;
    }

    // Stack layout (top to bottom):
    // copiedN, ..., copied1, compiledBlock, numArgs, outerContext
    Oop compiledBlock = stackValue(static_cast<size_t>(numCopied));
    Oop numArgsOop = stackValue(static_cast<size_t>(numCopied + 1));
    Oop outerContext = stackValue(static_cast<size_t>(numCopied + 2));

    if (!numArgsOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    // BlockClosure layout:
    // 0: outerContext
    // 1: startpc/compiledBlock
    // 2: numArgs
    // 3+: copied values

    size_t totalSlots = 3 + numCopied;
    Oop closure = memory_.allocateSlots(classIndex, totalSlots, ObjectFormat::FixedSize);
    if (closure.isNil()) {
        return PrimitiveResult::Failure;
    }

    // Initialize the closure
    memory_.storePointer(0, closure, outerContext);
    memory_.storePointer(1, closure, compiledBlock);
    memory_.storePointer(2, closure, numArgsOop);

    // Copy the captured values (in reverse order from stack)
    for (size_t i = 0; i < numCopied; ++i) {
        Oop copiedValue = stackValue(numCopied - 1 - i);
        memory_.storePointer(3 + i, closure, copiedValue);
    }

    // Pop all arguments and push result
    popN(static_cast<size_t>(argCount));
    push(closure);
    return PrimitiveResult::Success;
}

// Primitive 207: Full closure value (for closures with many arguments)
// This handles FullBlockClosures which may have more complex activation
PrimitiveResult Interpreter::primitiveFullClosureValue(int argCount) {
    Oop closure = stackValue(static_cast<size_t>(argCount));

    if (!closure.isObject()) {
        static int f1 = 0; if (f1++ < 5) std::cerr << "[P207-FAIL] not object\n";
        return PrimitiveResult::Failure;
    }

    // Get numArgs from the closure (slot 2)
    Oop numArgsOop = memory_.fetchPointer(2, closure);
    if (!numArgsOop.isSmallInteger()) {
        static int f2 = 0; if (f2++ < 5) std::cerr << "[P207-FAIL] numArgs not SmallInt: 0x" << std::hex << numArgsOop.rawBits() << std::dec << "\n";
        return PrimitiveResult::Failure;
    }

    int closureNumArgs = static_cast<int>(numArgsOop.asSmallInteger());
    if (closureNumArgs != argCount) {
        static int f3 = 0; if (f3++ < 5) std::cerr << "[P207-FAIL] argMismatch closureArgs=" << closureNumArgs << " argCount=" << argCount << "\n";
        return PrimitiveResult::Failure;
    }

    // TRACE: Log block activation with args - focus on nil args
    static FILE* blockArgLog = nullptr;
    static int blockArgCount = 0;
    static int nilArgCount = 0;
    if (!blockArgLog) blockArgLog = nullptr;

    // Check if any arg is nil
    bool hasNilArg = false;
    for (int i = 0; i < argCount; i++) {
        Oop arg = stackValue(static_cast<size_t>(i));
        // Check if arg is nil (nil is typically at old space base or has specific format)
        if (arg.isObject() && arg.asObjectPtr()->format() == ObjectFormat::ZeroSized
            && arg.asObjectPtr()->slotCount() == 0) {
            hasNilArg = true;
            break;
        }
    }

    if (blockArgLog && (blockArgCount < 100 || (hasNilArg && nilArgCount < 50))) {
        blockArgCount++;
        if (hasNilArg) nilArgCount++;

        fprintf(blockArgLog, "[BLOCK-ARG #%d] primitiveFullClosureValue argCount=%d%s\n",
                blockArgCount, argCount, hasNilArg ? " [HAS NIL ARG!]" : "");
        for (int i = 0; i < argCount; i++) {
            Oop arg = stackValue(static_cast<size_t>(i));
            std::string argInfo = "";
            if (arg.isObject() && arg.rawBits() > 0x10000) {
                ObjectHeader* hdr = arg.asObjectPtr();
                if (hdr->format() == ObjectFormat::ZeroSized && hdr->slotCount() == 0) {
                    argInfo = " [NIL]";
                } else {
                    Oop cls = memory_.classOf(arg);
                    if (cls.isObject()) {
                        Oop clsName = memory_.fetchPointer(6, cls);
                        if (clsName.isObject() && clsName.rawBits() > 0x10000) {
                            ObjectHeader* cnHdr = clsName.asObjectPtr();
                            if (cnHdr->isBytesObject() && cnHdr->byteSize() < 50) {
                                argInfo = " [" + std::string((char*)cnHdr->bytes(), cnHdr->byteSize()) + "]";
                            }
                        }
                    }
                }
            } else if (arg.isSmallInteger()) {
                argInfo = " [SmallInt " + std::to_string(arg.asSmallInteger()) + "]";
            }
            fprintf(blockArgLog, "  arg[%d] = 0x%llx%s\n",
                    i, (unsigned long long)arg.rawBits(), argInfo.c_str());
        }

        // Show closure info and calling context
        if (hasNilArg) {
            fprintf(blockArgLog, "  closure = 0x%llx\n", (unsigned long long)closure.rawBits());

            // Show receiver_ class (the class of the object whose method we're in)
            std::string rcvrClassName = "?";
            if (receiver_.isObject() && receiver_.rawBits() > 0x10000) {
                Oop cls = memory_.classOf(receiver_);
                if (cls.isObject()) {
                    Oop clsName = memory_.fetchPointer(6, cls);
                    if (clsName.isObject() && clsName.rawBits() > 0x10000) {
                        ObjectHeader* cnHdr = clsName.asObjectPtr();
                        if (cnHdr->isBytesObject() && cnHdr->byteSize() < 50) {
                            rcvrClassName = std::string((char*)cnHdr->bytes(), cnHdr->byteSize());
                        }
                    }
                }
            }
            fprintf(blockArgLog, "  receiver_ class: %s\n", rcvrClassName.c_str());

            // Show current method selector
            if (method_.isObject()) {
                Oop hdr = memory_.fetchPointer(0, method_);
                if (hdr.isSmallInteger()) {
                    int numLits = hdr.asSmallInteger() & 0x7FFF;
                    if (numLits >= 2) {
                        Oop sel = memory_.fetchPointer(numLits - 1, method_);
                        if (sel.isObject() && sel.rawBits() > 0x10000) {
                            ObjectHeader* selHdr = sel.asObjectPtr();
                            if (selHdr->isBytesObject() && selHdr->byteSize() < 50) {
                                fprintf(blockArgLog, "  in method: %s >> #%s\n", rcvrClassName.c_str(),
                                        std::string((char*)selHdr->bytes(), selHdr->byteSize()).c_str());
                            }
                        }
                    }
                }
            }
        }
        fflush(blockArgLog);
    }

    // Activate the closure - same as regular block activation
    activateBlock(closure, argCount);
    return PrimitiveResult::Success;
}
// Primitive 209: Full closure value without context switch
// Same as primitiveFullClosureValue but won't check for interrupts/process switches
// Used for critical sections where process switching must be avoided
PrimitiveResult Interpreter::primitiveFullClosureValueNoContextSwitch(int argCount) {
    Oop closure = stackValue(static_cast<size_t>(argCount));

    if (!closure.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Verify argument count
    Oop numArgsOop = memory_.fetchPointer(2, closure);
    if (!numArgsOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int closureNumArgs = static_cast<int>(numArgsOop.asSmallInteger());
    if (closureNumArgs != argCount) {
        return PrimitiveResult::Failure;
    }

    activateBlock(closure, argCount);
    return PrimitiveResult::Success;
}

// Primitive 208: Full closure value with arguments array
// BlockClosure>>valueWithArguments: anArray
// Takes an array of arguments and passes them to the closure
PrimitiveResult Interpreter::primitiveFullClosureValueWithArgs(int argCount) {
    if (argCount != 1) {
        return PrimitiveResult::Failure;
    }

    Oop argsArray = stackValue(0);   // The arguments array
    Oop closure = stackValue(1);     // The closure (receiver)

    if (!closure.isObject() || !argsArray.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Get closure's expected numArgs
    Oop numArgsOop = memory_.fetchPointer(2, closure);
    if (!numArgsOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }
    int closureNumArgs = static_cast<int>(numArgsOop.asSmallInteger());

    // Get the array size
    ObjectHeader* arrayHeader = argsArray.asObjectPtr();
    ObjectFormat fmt = arrayHeader->format();

    // Verify it's a pointer array (format 2 = Indexable)
    if (fmt != ObjectFormat::Indexable) {
        return PrimitiveResult::Failure;
    }

    size_t arraySize = arrayHeader->slotCount();
    if (static_cast<int>(arraySize) != closureNumArgs) {
        return PrimitiveResult::Failure;
    }

    // Pop the argument and closure from stack
    popN(2);

    // Push closure back (will be popped by activateBlock)
    push(closure);

    // Push arguments from array onto stack (in order)
    for (size_t i = 0; i < arraySize; i++) {
        Oop arg = memory_.fetchPointer(i, argsArray);
        push(arg);
    }

    // Now activate the closure with the pushed arguments
    activateBlock(closure, closureNumArgs);
    return PrimitiveResult::Success;
}

// ===== PROCESS PRIMITIVES =====

PrimitiveResult Interpreter::primitiveSuspend(int argCount) {
    // Primitive 88: Process>>suspend
    // Suspend the receiver process. If it's the active process, switch to next.
    // Returns the list the process was on (or nil if it was running).
    Oop process = stackTop();  // Receiver is the process to suspend

    if (!process.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Follow forwarding pointers (created by become:)
    process = memory_.followForwarded(process);
    stackValuePut(0, process);

    Oop activeProcess = getActiveProcess();
    Oop nilObj = memory_.nil();

    if (process.rawBits() == activeProcess.rawBits()) {
        // Suspending ourselves - return nil (we weren't on any list, we were running)
        pop();  // Remove receiver
        push(nilObj);  // Push result (nil)

        // Find next runnable process and switch to it
        Oop nextProcess = wakeHighestPriority();
        if (nextProcess.isNil() || nextProcess.rawBits() == nilObj.rawBits()) {
            // No other process to run - this shouldn't happen
            return PrimitiveResult::Failure;
        }
        g_xferReason = "primSuspend";
        transferTo(nextProcess);
        return PrimitiveResult::Success;
    }

    // Suspending another process - it must be on some list
    Oop myList = memory_.fetchPointer(ProcessMyListIndex, process);

    // Follow forwarding pointers on myList
    if (myList.isObject()) {
        myList = memory_.followForwarded(myList);
    }

    if (myList.isNil() || myList.rawBits() == nilObj.rawBits()) {
        // Process not on any list - can't suspend (already suspended?)
        return PrimitiveResult::Failure;
    }

    // Remove from its current list
    removeProcessFromList(process, myList);

    // Per official VM: set myList to nil after removal
    memory_.storePointer(ProcessMyListIndex, process, nilObj);

    // Return the list it was on
    pop();  // Remove receiver
    push(myList);  // Push result (the list)

    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveResume(int argCount) {
    // Primitive 87: Process>>resume
    // Resume a suspended process. Add to scheduler queue.
    // If it has higher priority than current, preempt.

    Oop process = stackTop();  // Receiver is the process to resume

    if (!process.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Follow forwarding pointers (created by become:)
    process = memory_.followForwarded(process);
    stackValuePut(0, process);

    // Verify process has a valid suspended context
    Oop context = memory_.fetchPointer(ProcessSuspendedContextIndex, process);
    Oop nilObj = memory_.nil();

    if (context.isNil() || context.rawBits() == nilObj.rawBits() || !context.isObject()) {
        return PrimitiveResult::Failure;  // Can't resume without a valid context
    }

    // Follow forwarding on context too
    context = memory_.followForwarded(context);

    // Context objects have format 3 (IndexableWithFixed)
    ObjectHeader* ctxHdr = context.asObjectPtr();
    if (ctxHdr->format() != ObjectFormat::IndexableWithFixed) {
        return PrimitiveResult::Failure;  // Not a valid Context
    }

    // Get priorities to check for preemption
    int processPriority = safeProcessPriority(process);
    if (processPriority < 0) return PrimitiveResult::Failure;

    Oop activeProcess = getActiveProcess();
    int activePriority = safeProcessPriority(activeProcess);
    if (activePriority < 0) return PrimitiveResult::Failure;

    if (processPriority > activePriority) {
        // Resumed process has higher priority - preempt current process
        // Put current process to sleep
        putToSleep(activeProcess);
        // Switch to resumed process (don't put it to sleep, just run it)
        g_xferReason = "primResume";
        transferTo(process);
    } else {
        // Same or lower priority - just add to ready queue
        putToSleep(process);
    }

    // Return the process (receiver stays on stack)
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveSignal(int argCount) {
    // Primitive 85: Semaphore>>signal
    Oop semaphore = stackTop();

    if (!semaphore.isObject()) {
        return PrimitiveResult::Failure;
    }

    // If this is the lowSpaceSemaphore, set ProcessSignalingLowSpace
    Oop lowSpaceSem = memory_.specialObject(SpecialObjectIndex::TheLowSpaceSemaphore);
    if (semaphore.rawBits() == lowSpaceSem.rawBits() && !lowSpaceSem.isNil()) {
        memory_.setSpecialObject(SpecialObjectIndex::ProcessSignalingLowSpace, getActiveProcess());
    }

    synchronousSignal(semaphore);
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveWait(int argCount) {
    // Primitive 86: Semaphore>>wait
    // Wait on a semaphore. If excessSignals > 0, decrement and return.
    // Otherwise suspend current process on the semaphore's wait list.

    if (stackPointer_ < stackBase_ + argCount + 1) {
        return PrimitiveResult::Failure;
    }

    Oop semaphore = stackValue(argCount);  // Receiver is under args

    if (!semaphore.isObject()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* semHdr = semaphore.asObjectPtr();
    if (!semHdr || semHdr->slotCount() < 3) {
        return PrimitiveResult::Failure;
    }

    // Check excessSignals (slot 2 of Semaphore)
    Oop excessOop = memory_.fetchPointer(SemaphoreExcessSignalsIndex, semaphore);

    if (excessOop.isSmallInteger()) {
        int64_t excess = excessOop.asSmallInteger();
        if (excess > 0) {
            // Semaphore is signaled - decrement and return immediately
            Oop lowSpaceSem = memory_.specialObject(SpecialObjectIndex::TheLowSpaceSemaphore);
            if (semaphore.rawBits() == lowSpaceSem.rawBits() && !lowSpaceSem.isNil()) {
                Oop currentProcess = getActiveProcess();
                Oop procSignalingLowSpace = memory_.specialObject(SpecialObjectIndex::ProcessSignalingLowSpace);
                if (procSignalingLowSpace.isNil() || procSignalingLowSpace.rawBits() == memory_.nil().rawBits()) {
                    memory_.setSpecialObject(SpecialObjectIndex::ProcessSignalingLowSpace, currentProcess);
                }
            }

            memory_.storePointer(SemaphoreExcessSignalsIndex, semaphore,
                                Oop::fromSmallInteger(excess - 1));
            return PrimitiveResult::Success;
        }
    } else {
        return PrimitiveResult::Failure;
    }

    // No signal available - must wait
    Oop activeProcess = getActiveProcess();
    Oop activePriorityOop = memory_.fetchPointer(ProcessPriorityIndex, activeProcess);
    int activePriority = activePriorityOop.isSmallInteger() ?
                         static_cast<int>(activePriorityOop.asSmallInteger()) : -1;

    // Add current process to semaphore wait list and switch to next runnable
    addLastLinkToList(activeProcess, semaphore);

    // Find next runnable process and switch to it
    Oop nextProcess = wakeHighestPriority();
    if (nextProcess.isNil()) {
        // No runnable process - this shouldn't happen in a working system
        return PrimitiveResult::Failure;
    }

    g_xferReason = "primWait";
    transferTo(nextProcess);
    return PrimitiveResult::Success;
}

// ===== SYSTEM PRIMITIVES =====

PrimitiveResult Interpreter::primitiveQuit(int argCount) {
    // Smalltalk quitPrimitive / Smalltalk exit: exitCode
    // The standard Cog VM always exits on this primitive.
    // The image's SnapshotOperation checks isImageStarting and only calls
    // quitPrimitive when actually intended to quit (not during startup resume).

    int exitCode = 0;
    if (argCount > 0) {
        Oop arg = stackTop();
        if (arg.isSmallInteger()) {
            exitCode = static_cast<int>(arg.asSmallInteger());
        }
    }

    std::cerr << "[VM] primitiveQuit: exit code " << exitCode << "\n";

    // On error exit, dump the context sender chain for debugging
    if (exitCode != 0) {
        std::cerr << "[VM] Context chain at exit:\n";
        Oop ctx = activeContext_;
        for (int i = 0; i < 30 && ctx.isObject() && !ctx.isNil(); i++) {
            Oop method = memory_.fetchPointer(3, ctx);
            std::string selStr = "<unknown>";
            if (method.isObject() && !method.isNil()) {
                ObjectHeader* mhdr = method.asObjectPtr();
                if (mhdr->isCompiledMethod() && mhdr->slotCount() >= 2) {
                    Oop sel = memory_.fetchPointer(1, method);
                    if (sel.isObject() && !sel.isNil()) {
                        ObjectHeader* shdr = sel.asObjectPtr();
                        if (shdr->isBytesObject() && shdr->byteSize() <= 100) {
                            selStr = std::string((char*)shdr->bytes(), shdr->byteSize());
                        }
                    }
                }
            }
            std::cerr << "  [" << i << "] #" << selStr << "\n";
            ctx = memory_.fetchPointer(0, ctx); // sender
        }
    }

    running_ = false;
    std::exit(exitCode);

    return PrimitiveResult::Success;  // Never reached
}

PrimitiveResult Interpreter::primitiveExitToDebugger(int argCount) {
    // Primitive 114: Enter debugger / halt VM

    // On debug builds, trigger a breakpoint
#if defined(__APPLE__) && defined(__arm64__)
    __builtin_debugtrap();
#elif defined(__x86_64__) || defined(_M_X64)
    __builtin_trap();
#else
    std::abort();
#endif

    // Not reached
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveVMParameter(int argCount) {
    // Primitive 254: Access VM parameters
    // With 0 args: return array of all parameters
    // With 1 arg (index): return parameter at index
    // With 2 args (index, value): set parameter and return old value

    const int paramsArraySize = 86;

    // Helper to get a parameter value
    auto getParameter = [this](int index) -> Oop {
        switch (index) {
            case 1:  // Total heap size (old space)
                return Oop::fromSmallInteger(
                    memory_.oldSpaceEnd() - memory_.oldSpaceStart());
            case 2:  // Free space in young generation
                return Oop::fromSmallInteger(0);  // Simplified
            case 3:  // Total memory size
                return Oop::fromSmallInteger(
                    memory_.oldSpaceEnd() - memory_.oldSpaceStart());
            case 7:  // Full GC count
                return Oop::fromSmallInteger(memory_.statistics().gcCount);
            case 8:  // Full GC time (ms)
                return Oop::fromSmallInteger(memory_.statistics().totalGCTime);
            case 9:  // Scavenge count
                return Oop::fromSmallInteger(0);
            case 10: // Scavenge time (ms)
                return Oop::fromSmallInteger(0);
            case 11: // Tenures count
                return Oop::fromSmallInteger(0);
            case 40: // Bytes per word
                return Oop::fromSmallInteger(8);  // 64-bit
            case 41: // Image format version
                return Oop::fromSmallInteger(68021);  // Spur 64-bit
            case 42: // Number of stack pages
                return Oop::fromSmallInteger(1);
            case 44: // Eden size
                return Oop::fromSmallInteger(22003584);
            case 46: // Cog code size
                return Oop::fromSmallInteger(0);  // No JIT
            case 48: // VM flags
                return Oop::fromSmallInteger(0);
            case 49: { // Max external semaphores table size
                static int param49Count = 0;
                param49Count++;
                Oop semTable = memory_.specialObject(SpecialObjectIndex::ExternalObjectsArray);
                size_t size = 0;
                if (!semTable.isNil() && semTable.isObject()) {
                    size = memory_.slotCountOf(semTable);
                }
                // Auto-resize if table is empty or missing - Pharo expects at least some slots
                if (size == 0) {
                    constexpr size_t AUTO_EXT_OBJ_SIZE = 256;
                    Oop arrayClass = memory_.specialObject(SpecialObjectIndex::ClassArray);
                    uint32_t classIndex = memory_.indexOfClass(arrayClass);
                    Oop newTable = memory_.allocateSlots(classIndex, AUTO_EXT_OBJ_SIZE, ObjectFormat::Indexable);
                    if (!newTable.isNil()) {
                        for (size_t i = 0; i < AUTO_EXT_OBJ_SIZE; i++) {
                            memory_.storePointer(i, newTable, memory_.nil());
                        }
                        memory_.setSpecialObject(SpecialObjectIndex::ExternalObjectsArray, newTable);
                        size = AUTO_EXT_OBJ_SIZE;
                        FILE* logFile = nullptr;
                        if (logFile) {
                            fprintf(logFile, "[PARAM49 #%d step=%llu] AUTO-CREATED %zu slots\n",
                                    param49Count, (unsigned long long)g_stepNum, AUTO_EXT_OBJ_SIZE);
                            fclose(logFile);
                        }
                    }
                }
                if (param49Count <= 20) {
                    FILE* logFile = nullptr;
                    if (logFile) {
                        fprintf(logFile, "[PARAM49 #%d step=%llu] semTable=0x%llx size=%zu\n",
                                param49Count, (unsigned long long)g_stepNum,
                                (unsigned long long)semTable.rawBits(), size);
                        fclose(logFile);
                    }
                }
                return Oop::fromSmallInteger(size);
            }
            case 65: // VM features (immutability support, etc.)
                return Oop::fromSmallInteger(2);  // Immutability supported
            default:
                return Oop::fromSmallInteger(0);
        }
    };

    if (argCount == 0) {
        // Return array of all parameters
        Oop arrayClass = memory_.specialObject(SpecialObjectIndex::ClassArray);
        uint32_t classIndex = memory_.indexOfClass(arrayClass);
        Oop result = memory_.allocateSlots(classIndex, paramsArraySize, ObjectFormat::Indexable);
        if (result.isNil()) {
            return PrimitiveResult::Failure;
        }

        for (int i = 0; i < paramsArraySize; i++) {
            Oop value = getParameter(i + 1);  // 1-based
            memory_.storePointer(i, result, value);
        }

        primitiveSuccess(result);
        return PrimitiveResult::Success;
    }

    if (argCount == 1) {
        // Get single parameter
        Oop indexOop = stackValue(0);
        if (!indexOop.isSmallInteger()) {
            return PrimitiveResult::Failure;
        }

        int64_t index = indexOop.asSmallInteger();
        if (index < 1 || index > paramsArraySize) {
            return PrimitiveResult::Failure;
        }

        Oop result = getParameter(static_cast<int>(index));
        primitiveSuccess(result);
        return PrimitiveResult::Success;
    }

    if (argCount == 2) {
        // Set parameter
        Oop indexOop = stackValue(1);
        Oop newValueOop = stackValue(0);
        if (!indexOop.isSmallInteger()) {
            return PrimitiveResult::Failure;
        }

        int64_t index = indexOop.asSmallInteger();
        if (index < 1 || index > paramsArraySize) {
            return PrimitiveResult::Failure;
        }

        Oop oldValue = getParameter(static_cast<int>(index));

        if (index == 49 && newValueOop.isSmallInteger()) {
            // Resize external semaphore table
            int64_t newSize = newValueOop.asSmallInteger();
            static int resize49Count = 0;
            resize49Count++;
            FILE* logFile = nullptr;
            if (logFile) {
                fprintf(logFile, "[RESIZE49 #%d step=%llu] requested newSize=%lld\n",
                        resize49Count, (unsigned long long)g_stepNum, (long long)newSize);
                fclose(logFile);
            }
            if (newSize < 0 || newSize > 65535) {
                logFile = nullptr;
                if (logFile) { fprintf(logFile, "[RESIZE49 #%d] FAIL: size out of range\n", resize49Count); fclose(logFile); }
                return PrimitiveResult::Failure;
            }
            Oop oldTable = memory_.specialObject(SpecialObjectIndex::ExternalObjectsArray);
            size_t oldSize = 0;
            if (!oldTable.isNil() && oldTable.isObject()) {
                oldSize = memory_.slotCountOf(oldTable);
            }
            logFile = nullptr;
            if (logFile) {
                fprintf(logFile, "[RESIZE49 #%d] oldTable=0x%llx oldSize=%zu\n",
                        resize49Count, (unsigned long long)oldTable.rawBits(), oldSize);
                fclose(logFile);
            }
            if ((size_t)newSize > oldSize) {
                // Allocate new larger array
                Oop arrayClass = memory_.specialObject(SpecialObjectIndex::ClassArray);
                uint32_t classIndex = memory_.indexOfClass(arrayClass);
                Oop newTable = memory_.allocateSlots(classIndex, (size_t)newSize, ObjectFormat::Indexable);
                if (newTable.isNil()) {
                    logFile = nullptr;
                    if (logFile) { fprintf(logFile, "[RESIZE49 #%d] FAIL: allocation failed\n", resize49Count); fclose(logFile); }
                    return PrimitiveResult::Failure;
                }
                // Initialize with nil
                for (size_t i = 0; i < (size_t)newSize; i++) {
                    memory_.storePointer(i, newTable, memory_.nil());
                }
                // Copy old entries
                if (!oldTable.isNil() && oldTable.isObject()) {
                    for (size_t i = 0; i < oldSize; i++) {
                        Oop entry = memory_.fetchPointer(i, oldTable);
                        memory_.storePointer(i, newTable, entry);
                    }
                }
                // Replace in special objects array
                memory_.setSpecialObject(SpecialObjectIndex::ExternalObjectsArray, newTable);
            } else {
            }
        }

        primitiveSuccess(oldValue);
        return PrimitiveResult::Success;
    }

    return PrimitiveResult::Failure;
}

// Helper: Extract string from ByteString Oop
static std::string extractString(ObjectMemory& memory, Oop stringOop) {
    if (!stringOop.isObject()) return "";

    ObjectHeader* header = stringOop.asObjectPtr();
    ObjectFormat format = header->format();

    // Must be a ByteString (format 16-23)
    if (format < ObjectFormat::Indexable8 || format > ObjectFormat::Indexable8_7) {
        return "";
    }

    size_t len = header->byteSize();
    std::string result(len, '\0');
    for (size_t i = 0; i < len; i++) {
        result[i] = static_cast<char>(header->byteAt(i));
    }
    return result;
}

// Helper: Translate runtime Oop to image format (for saving)
static uint64_t oopToImageFormat(Oop oop, uint8_t* runtimeBase, uint64_t imageBase) {
    if (oop.isImmediate() || oop.isNil()) {
        // Immediates and nil don't need translation
        return oop.rawBits();
    }

    // Get the object address and translate to image space
    ObjectHeader* ptr = oop.asObjectPtr();
    uint64_t runtimeAddr = reinterpret_cast<uint64_t>(ptr);
    uint64_t baseAddr = reinterpret_cast<uint64_t>(runtimeBase);

    // Calculate offset from runtime base and add to image base
    uint64_t offset = runtimeAddr - baseAddr;
    return imageBase + offset;
}

PrimitiveResult Interpreter::primitiveSnapshot(int argCount) {
    // Image saving is disabled, but we must return true to indicate
    // "resuming from saved image" — this is how the active process
    // was suspended when the image was saved. Returning true triggers
    // SnapshotOperation to set isImageStarting=true, which calls
    // SessionManager>>installNewSession to initialize currentSession.
    // Failing the primitive breaks the entire startup sequence.
    { FILE* f = nullptr; if (f) { fprintf(f, "[VM] primitiveSnapshot CALLED! argCount=%d\n", argCount); fclose(f); } }
    if (argCount > 0) {
        popN(argCount);  // pop arguments
    }
    // Pop receiver, push true (isImageStarting = true)
    stackTop() = memory_.specialObject(SpecialObjectIndex::TrueObject);
    return PrimitiveResult::Success;

    // === ORIGINAL IMPLEMENTATION DISABLED ===
    // The code below is kept for reference but never executes due to early return above
#if 0
    // primitiveSnapshot / primitiveSnapshotEmbedded
    // Argument: optional filename (String)
    // Returns: true if save succeeded, false if resuming from load

    std::string filename;

    if (argCount >= 1) {
        Oop arg = stackValue(0);
        filename = extractString(memory_, arg);
    }

    if (filename.empty()) {
        // No filename provided - fail
        return PrimitiveResult::Failure;
    }

    // Open output file
    std::ofstream file(filename, std::ios::binary);
    if (!file) {
        return PrimitiveResult::Failure;
    }

    // Standard Spur 64-bit image base address
    const uint64_t imageBase = 0x10000000000ULL;

    // Calculate heap size (old space used)
    uint8_t* heapStart = memory_.oldSpaceStart();
    uint8_t* heapEnd = memory_.oldSpaceEnd();
    size_t heapSize = heapEnd - heapStart;

    // Build image header
    SpurImageHeader header = {};
    header.imageFormat = 68021;  // Spur 64-bit
    header.headerSize = 128;     // Standard header size
    header.imageBytes = heapSize;
    header.startOfMemory = imageBase;

    // Translate special objects array pointer
    header.specialObjectsOop = oopToImageFormat(
        memory_.specialObjectsArray(), heapStart, imageBase);

    header.lastHash = 0;  // Will be updated on next allocation
    header.screenSize = 0;
    header.imageHeaderFlags = 0x2;  // Preemption yields
    header.extraVMMemory = 0;
    header.numStackPages = 0;
    header.cogCodeSize = 0;
    header.edenBytes = 22003584;  // Default eden size
    header.maxExtSemTabSize = 0;
    header.unused1 = 0;
    header.firstSegmentBytes = heapSize;
    header.freeOldSpaceInImage = 0;

    // Write header (128 bytes, padded)
    file.write(reinterpret_cast<const char*>(&header.imageFormat), 4);
    file.write(reinterpret_cast<const char*>(&header.headerSize), 4);
    file.write(reinterpret_cast<const char*>(&header.imageBytes), 8);
    file.write(reinterpret_cast<const char*>(&header.startOfMemory), 8);
    file.write(reinterpret_cast<const char*>(&header.specialObjectsOop), 8);
    file.write(reinterpret_cast<const char*>(&header.lastHash), 8);
    file.write(reinterpret_cast<const char*>(&header.screenSize), 8);
    file.write(reinterpret_cast<const char*>(&header.imageHeaderFlags), 8);
    file.write(reinterpret_cast<const char*>(&header.extraVMMemory), 4);
    file.write(reinterpret_cast<const char*>(&header.numStackPages), 2);
    file.write(reinterpret_cast<const char*>(&header.cogCodeSize), 2);
    file.write(reinterpret_cast<const char*>(&header.edenBytes), 4);
    file.write(reinterpret_cast<const char*>(&header.maxExtSemTabSize), 2);
    file.write(reinterpret_cast<const char*>(&header.unused1), 2);
    file.write(reinterpret_cast<const char*>(&header.firstSegmentBytes), 8);
    file.write(reinterpret_cast<const char*>(&header.freeOldSpaceInImage), 8);

    // Pad to 128 bytes
    size_t written = 4 + 4 + 8 + 8 + 8 + 8 + 8 + 8 + 4 + 2 + 2 + 4 + 2 + 2 + 8 + 8;
    std::vector<char> padding(128 - written, 0);
    file.write(padding.data(), padding.size());

    // Now write the heap data with pointer translation
    // We need to iterate through all objects and translate pointers

    // Make a copy of heap for translation
    std::vector<uint8_t> heapCopy(heapStart, heapEnd);

    // Iterate through objects and translate pointers
    uint8_t* pos = heapCopy.data();
    uint8_t* end = pos + heapSize;

    while (pos < end) {
        ObjectHeader* obj = reinterpret_cast<ObjectHeader*>(pos);
        uint64_t rawHeader = obj->rawHeader();

        // Check for overflow header (numSlots == 255)
        size_t numSlots = (rawHeader >> 56) & 0xFF;
        size_t actualSlots = numSlots;

        if (numSlots == 255) {
            // Overflow: previous 8 bytes contain actual slot count
            if (pos >= heapCopy.data() + 8) {
                uint64_t* overflowSlot = reinterpret_cast<uint64_t*>(pos - 8);
                actualSlots = *overflowSlot;
            }
        }

        // Get format to determine if this is a pointer object
        uint8_t format = (rawHeader >> 24) & 0x1F;

        // Pointer objects: format 0-5
        if (format <= 5 && actualSlots > 0) {
            // Translate each slot
            Oop* slots = reinterpret_cast<Oop*>(pos + 8);
            for (size_t i = 0; i < actualSlots; i++) {
                Oop oldOop = slots[i];
                uint64_t translated = oopToImageFormat(oldOop, heapStart, imageBase);
                *reinterpret_cast<uint64_t*>(&slots[i]) = translated;
            }
        }

        // Move to next object
        size_t objectSize = 8 + actualSlots * 8;  // Header + slots
        if (objectSize < 16) objectSize = 16;     // Minimum object size

        // Align to 8 bytes
        objectSize = (objectSize + 7) & ~7ULL;

        pos += objectSize;
    }

    // Write the translated heap
    file.write(reinterpret_cast<const char*>(heapCopy.data()), heapSize);

    file.close();

    if (!file) {
        return PrimitiveResult::Failure;
    }

    // Return true to indicate successful save
    primitiveSuccess(memory_.trueObject());
    return PrimitiveResult::Success;
#endif  // Disabled save implementation
}

// ===== I/O PRIMITIVES (stubs - iOS implementation elsewhere) =====

PrimitiveResult Interpreter::primitiveMousePoint(int argCount) {
    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveMouseButtons(int argCount) {
    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveKeyboardNext(int argCount) {
    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveBeDisplay(int argCount) {
    static int callCount = 0;
    callCount++;
    if (callCount <= 10) {
        fprintf(stderr, "[PRIM102] beDisplay #%d argCount=%d\n", callCount, argCount);
    }
    if (argCount != 0) return PrimitiveResult::Failure;

    // Get the receiver (a Form object)
    Oop form = stackTop();
    if (!form.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Store as the display form
    setDisplayForm(form);
    if (callCount <= 10) {
        Oop w = memory_.fetchPointer(1, form);
        Oop h = memory_.fetchPointer(2, form);
        Oop bits = memory_.fetchPointer(0, form);
        uint32_t classIdx = form.asObjectPtr()->classIndex();
        fprintf(stderr, "[PRIM102] setDisplayForm=%llx classIdx=%u width=%lld height=%lld bits=%llx\n",
                (unsigned long long)form.rawBits(), classIdx,
                w.isSmallInteger() ? (long long)w.asSmallInteger() : -1LL,
                h.isSmallInteger() ? (long long)h.asSmallInteger() : -1LL,
                (unsigned long long)bits.rawBits());
    }

    // Extract form dimensions to update screen size
    // Form slots: 0=bits, 1=width, 2=height, 3=depth
    Oop widthOop = memory_.fetchPointer(1, form);
    Oop heightOop = memory_.fetchPointer(2, form);
    Oop depthOop = memory_.fetchPointer(3, form);

    if (widthOop.isSmallInteger() && heightOop.isSmallInteger() && depthOop.isSmallInteger()) {
        int width = static_cast<int>(widthOop.asSmallInteger());
        int height = static_cast<int>(heightOop.asSmallInteger());
        int depth = static_cast<int>(depthOop.asSmallInteger());

        if (width > 0 && height > 0 && depth > 0) {
            setScreenSize(width, height);
            setScreenDepth(depth);
        }
    }

    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveForceDisplayUpdate(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // If no display surface, nothing to do
    if (!pharo::gDisplaySurface) {
        return PrimitiveResult::Success;
    }

    // When SDL2 rendering is active, skip the pixel copy but still discover
    // the Display global (some code may depend on displayForm_ being set).
    bool skipPixelCopy = ffi_isSDLRenderingActive();

    // Auto-discover Display global if displayForm_ not set
    if (displayForm_.isNil()) {
        Oop display = memory_.findGlobal("Display");
        if (!display.isNil() && display.isObject()) {
            displayForm_ = display;
        }
    }

    uint32_t* dstPixels = pharo::gDisplaySurface->pixels();
    int dstWidth = pharo::gDisplaySurface->width();
    int dstHeight = pharo::gDisplaySurface->height();

    // Copy display form bits to the platform display surface
    // Form slots: 0=bits, 1=width, 2=height, 3=depth
    if (!skipPixelCopy && !displayForm_.isNil() && displayForm_.isObject()) {
        // Get the Form's bits (slot 0)
        Oop bits = memory_.fetchPointer(0, displayForm_);
        if (!bits.isNil() && bits.isObject()) {
            ObjectHeader* bitsHdr = bits.asObjectPtr();
            uint32_t* srcPixels = reinterpret_cast<uint32_t*>(bitsHdr->bytes());

            // Get actual dimensions from the Form
            Oop widthOop = memory_.fetchPointer(1, displayForm_);
            Oop heightOop = memory_.fetchPointer(2, displayForm_);
            Oop depthOop = memory_.fetchPointer(3, displayForm_);

            int srcWidth = widthOop.isSmallInteger() ? widthOop.asSmallInteger() : screenWidth_;
            int srcHeight = heightOop.isSmallInteger() ? heightOop.asSmallInteger() : screenHeight_;
            int srcDepth = depthOop.isSmallInteger() ? depthOop.asSmallInteger() : 32;

            // Copy pixels (handle size mismatch)
            int copyWidth = std::min(srcWidth, dstWidth);
            int copyHeight = std::min(srcHeight, dstHeight);

            if (srcDepth == 32) {
                for (int y = 0; y < copyHeight; y++) {
                    memcpy(dstPixels + y * dstWidth, srcPixels + y * srcWidth, copyWidth * sizeof(uint32_t));
                }
            } else if (srcDepth == 16) {
                uint16_t* src16 = reinterpret_cast<uint16_t*>(srcPixels);
                for (int y = 0; y < copyHeight; y++) {
                    for (int x = 0; x < copyWidth; x++) {
                        uint16_t pixel = src16[y * srcWidth + x];
                        uint8_t r = ((pixel >> 11) & 0x1F) << 3;
                        uint8_t g = ((pixel >> 5) & 0x3F) << 2;
                        uint8_t b = (pixel & 0x1F) << 3;
                        dstPixels[y * dstWidth + x] = (255 << 24) | (r << 16) | (g << 8) | b;
                    }
                }
            } else {
                for (int y = 0; y < copyHeight; y++) {
                    memcpy(dstPixels + y * dstWidth, srcPixels + y * srcWidth, copyWidth * sizeof(uint32_t));
                }
            }

            // Notify platform of update
            pharo::gDisplaySurface->update();
            return PrimitiveResult::Success;
        }
    }

    // No Form set - show a test pattern to verify display pipeline works
    static bool patternShown = false;
    if (!patternShown) {
        patternShown = true;

        // Fill with a gradient pattern
        for (int y = 0; y < dstHeight; y++) {
            for (int x = 0; x < dstWidth; x++) {
                // Create a blue-to-white gradient pattern
                uint8_t r = static_cast<uint8_t>(128 + (x * 127 / dstWidth));
                uint8_t g = static_cast<uint8_t>(128 + (y * 127 / dstHeight));
                uint8_t b = 255;
                dstPixels[y * dstWidth + x] = (255 << 24) | (r << 16) | (g << 8) | b;  // ARGB
            }
        }
        pharo::gDisplaySurface->update();
    }

    return PrimitiveResult::Success;
}

// ===== SYSTEM PATH PRIMITIVES =====
// Note: primitiveCopyBits (BitBlt, primitive 96) is defined later in this file

// Helper function to create a String object from a C++ string
static Oop createStringObject(ObjectMemory& memory, const std::string& str) {
    // Get the ByteString class
    Oop stringClass = memory.specialObject(SpecialObjectIndex::ClassByteString);
    if (stringClass.isNil()) {
        return Oop::nil();
    }

    uint32_t classIndex = memory.indexOfClass(stringClass);
    if (classIndex == 0) {
        return Oop::nil();
    }

    // Allocate a byte object for the string
    Oop stringObj = memory.allocateBytes(classIndex, str.size());
    if (stringObj.isNil()) {
        return Oop::nil();
    }

    // Copy the string contents
    for (size_t i = 0; i < str.size(); ++i) {
        memory.storeByte(i, stringObj, static_cast<uint8_t>(str[i]));
    }

    return stringObj;
}

// Primitive 121: Get or set the image file name
// With no argument: returns the image name as a String
// With argument: sets the image name (returns receiver)
PrimitiveResult Interpreter::primitiveImageName(int argCount) {
    if (argCount == 0) {
        // Get image name
        Oop result = createStringObject(memory_, imageName_);
        if (result.isNil()) {
            return PrimitiveResult::Failure;
        }

        pop();  // Pop receiver
        push(result);
        return PrimitiveResult::Success;
    } else if (argCount == 1) {
        // Set image name
        Oop nameOop = stackValue(0);

        if (!nameOop.isObject()) {
            return PrimitiveResult::Failure;
        }

        // Extract string from the argument
        ObjectHeader* header = nameOop.asObjectPtr();
        ObjectFormat format = header->format();

        if (format < ObjectFormat::Indexable8 || format > ObjectFormat::Indexable8_7) {
            return PrimitiveResult::Failure;  // Not a byte object
        }

        size_t len = memory_.byteSizeOf(nameOop);
        std::string newName;
        newName.reserve(len);

        for (size_t i = 0; i < len; ++i) {
            newName.push_back(static_cast<char>(memory_.fetchByte(i, nameOop)));
        }

        imageName_ = newName;

        // Return receiver (pop argument, leave receiver)
        pop();
        return PrimitiveResult::Success;
    }

    return PrimitiveResult::Failure;
}

// Primitive 142: Get the VM executable directory path
// Returns the directory containing the VM as a String (with trailing separator)
PrimitiveResult Interpreter::primitiveVMPath(int argCount) {
    std::string dir = vmPath_;
    // Extract directory from full path
    size_t lastSlash = dir.rfind('/');
    if (lastSlash != std::string::npos) {
        dir = dir.substr(0, lastSlash + 1);  // Include trailing /
    }
    Oop result = createStringObject(memory_, dir);
    if (result.isNil()) {
        return PrimitiveResult::Failure;
    }

    pop();  // Pop receiver
    push(result);
    return PrimitiveResult::Success;
}

// ===== SCREEN PRIMITIVES =====

// Primitive 106: Get the screen size as a Point
// Returns Point with x = width, y = height
PrimitiveResult Interpreter::primitiveScreenSize(int argCount) {
    static int callCount = 0;
    callCount++;
    static FILE* log = nullptr;
    if (log && callCount <= 20) {
        fprintf(log, "[PRIM106] primitiveScreenSize called #%d -> %dx%d\n",
                callCount, screenWidth_, screenHeight_);
        fflush(log);
    }

    // Create a Point object with screen dimensions
    // Point is stored as: x @ y where x and y are SmallIntegers

    // Get the Point class
    Oop pointClass = memory_.specialObject(SpecialObjectIndex::ClassPoint);
    if (pointClass.isNil()) {
        return PrimitiveResult::Failure;
    }

    uint32_t classIndex = memory_.indexOfClass(pointClass);
    if (classIndex == 0) {
        return PrimitiveResult::Failure;
    }

    // Allocate a Point with 2 slots (x, y)
    Oop point = memory_.allocateSlots(classIndex, 2, ObjectFormat::FixedSize);
    if (point.isNil()) {
        return PrimitiveResult::Failure;
    }

    // Store width and height
    memory_.storePointer(0, point, Oop::fromSmallInteger(screenWidth_));
    memory_.storePointer(1, point, Oop::fromSmallInteger(screenHeight_));

    pop();  // Pop receiver
    push(point);
    return PrimitiveResult::Success;
}

// Primitive 108: Get the screen color depth
// Returns the number of bits per pixel
PrimitiveResult Interpreter::primitiveScreenDepth(int argCount) {
    pop();  // Pop receiver
    push(Oop::fromSmallInteger(screenDepth_));
    return PrimitiveResult::Success;
}

// Named primitive: isVMDisplayUsingSDL2
// Returns true to indicate that SDL2 display subsystem is being used.
// CRITICAL: OSSDL2Driver checks this to decide whether to start its event loop.
// Without this returning true, the driver won't poll for SDL events.
PrimitiveResult Interpreter::primitiveIsVMDisplayUsingSDL2(int argCount) {
    // Return true - we're providing SDL2 stubs that will handle events
    pop();  // Pop receiver
    push(memory_.trueObject());
    return PrimitiveResult::Success;
}

// Named primitive: primitiveSetVMSDL2Input
// receiver primitiveSetVMSDL2Input: aSemaphoreIndex -> receiver
// Sets the semaphore index to signal when SDL2 events are available.
// This enables SDL2 event handling by the image's OSSDL2Driver.
PrimitiveResult Interpreter::primitiveSetVMSDL2Input(int argCount) {
    static FILE* sdlLog = nullptr;
    static int callCount = 0;
    callCount++;

    if (!sdlLog) {
        sdlLog = nullptr;
    }

    if (sdlLog && callCount <= 20) {
        fprintf(sdlLog, "[SDL2-INPUT] primitiveSetVMSDL2Input called #%d, argCount=%d\n",
                callCount, argCount);
        fflush(sdlLog);
    }

    // Expect 1 argument: the semaphore index
    if (argCount != 1) {
        if (sdlLog) {
            fprintf(sdlLog, "[SDL2-INPUT] Wrong argCount=%d (expected 1), failing\n", argCount);
            fflush(sdlLog);
        }
        return PrimitiveResult::Failure;
    }

    Oop semIndexOop = stackTop();

    if (!semIndexOop.isSmallInteger()) {
        if (sdlLog) {
            fprintf(sdlLog, "[SDL2-INPUT] Semaphore index is not SmallInteger, failing\n");
            fflush(sdlLog);
        }
        return PrimitiveResult::Failure;
    }

    int64_t semIndex = semIndexOop.asSmallInteger();

    if (sdlLog) {
        fprintf(sdlLog, "[SDL2-INPUT] Setting SDL2 input semaphore index to %lld\n", (long long)semIndex);
        fflush(sdlLog);
    }

    // Store the semaphore index for SDL2 event signaling
    gEventQueue.setSDL2InputSemaphoreIndex(static_cast<int>(semIndex));

    // Note: do NOT set isSDL2EventPollingActive here. That flag should only
    // be set when stub_SDL_PollEvent is actually called. Setting it here
    // blocks the Display Form → gDisplaySurface copy in syncDisplayToSurface(),
    // but if OSSDL2Driver never reaches its event loop (e.g., SDL_Init fails
    // or FFI type resolution fails), no SDL2 rendering happens and the display
    // goes blank.

    if (sdlLog) {
        fprintf(sdlLog, "[SDL2-INPUT] SDL2 event polling NOW ACTIVE\n");
        fflush(sdlLog);
    }

    // Pop argument and return receiver
    pop();  // Pop semaphore index argument
    // Receiver stays on stack
    return PrimitiveResult::Success;
}

// Primitive 140: Beep
// Produces a system beep sound (no-op in headless mode)
PrimitiveResult Interpreter::primitiveBeep(int argCount) {
    // In headless mode, this is a no-op
    // On platforms with audio, this could trigger a system sound
    // For now, just succeed silently
    return PrimitiveResult::Success;
}

// Primitive 141: Get or set clipboard text
// With no argument: returns clipboard contents as a String
// With argument: sets clipboard contents (returns receiver)
PrimitiveResult Interpreter::primitiveClipboardText(int argCount) {
    if (argCount == 0) {
        // Get clipboard text
        Oop result = createStringObject(memory_, clipboardText_);
        if (result.isNil() && !clipboardText_.empty()) {
            return PrimitiveResult::Failure;
        }

        // If clipboard is empty, return empty string
        if (result.isNil()) {
            // Try to create an empty string
            Oop stringClass = memory_.specialObject(SpecialObjectIndex::ClassByteString);
            if (stringClass.isNil()) {
                return PrimitiveResult::Failure;
            }
            uint32_t classIndex = memory_.indexOfClass(stringClass);
            if (classIndex == 0) {
                return PrimitiveResult::Failure;
            }
            result = memory_.allocateBytes(classIndex, 0);
            if (result.isNil()) {
                return PrimitiveResult::Failure;
            }
        }

        pop();  // Pop receiver
        push(result);
        return PrimitiveResult::Success;
    } else if (argCount == 1) {
        // Set clipboard text
        Oop textOop = stackValue(0);

        if (!textOop.isObject()) {
            // If nil, clear clipboard
            if (textOop.isNil()) {
                clipboardText_.clear();
                pop();  // Pop argument, leave receiver
                return PrimitiveResult::Success;
            }
            return PrimitiveResult::Failure;
        }

        // Extract string from the argument
        ObjectHeader* header = textOop.asObjectPtr();
        ObjectFormat format = header->format();

        if (format < ObjectFormat::Indexable8 || format > ObjectFormat::Indexable8_7) {
            return PrimitiveResult::Failure;  // Not a byte object
        }

        size_t len = memory_.byteSizeOf(textOop);
        std::string newText;
        newText.reserve(len);

        for (size_t i = 0; i < len; ++i) {
            newText.push_back(static_cast<char>(memory_.fetchByte(i, textOop)));
        }

        clipboardText_ = newText;

        // Return receiver (pop argument, leave receiver)
        pop();
        return PrimitiveResult::Success;
    }

    return PrimitiveResult::Failure;
}

// ===== TIME PRIMITIVES =====

PrimitiveResult Interpreter::primitiveMillisecondClock(int argCount) {
    // Primitive 135: Return milliseconds since VM start
    // Official VM returns ioMSecs() masked to 30 bits for wrapping behavior
    // Timer calculations depend on this 30-bit wrapping semantics
    constexpr int64_t MillisecondClockMask = 0x3FFFFFFF;  // 30-bit mask
    int64_t maskedMs = ioMSecs() & MillisecondClockMask;
    primitiveSuccess(Oop::fromSmallInteger(maskedMs));
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveSecondsClock(int argCount) {
    // Primitive 137: Return seconds from OS/platform epoch
    // The image-side code handles any epoch conversion (e.g., to Smalltalk epoch)
    // Official VM just returns ioSecondsNow() via positive32BitIntegerFor:
    auto now = std::chrono::system_clock::now();
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();

    // positive32BitIntegerFor semantics: return as unsigned 32-bit value
    uint32_t secs32 = static_cast<uint32_t>(seconds & 0xFFFFFFFF);
    primitiveSuccess(Oop::fromSmallInteger(static_cast<int64_t>(secs32)));
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveMicrosecondClock(int argCount) {
    // Primitive 240: Return UTC microseconds since Smalltalk epoch (Jan 1, 1901)
    // Per official VM: ioUTCMicrosecondsNow() returns Smalltalk epoch microseconds

    auto now = std::chrono::system_clock::now();
    auto unixUs = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();

    // Convert Unix epoch (1970) to Smalltalk epoch (1901)
    // Difference: 2177452800 seconds = 2177452800000000 microseconds
    constexpr int64_t unixToSmalltalkOffsetUs = 2177452800LL * 1000000LL;
    int64_t smalltalkUs = unixUs + unixToSmalltalkOffsetUs;

    // Return as SmallInteger (fits in 61-bit signed for current times)
    if (Oop::canBeSmallInteger(smalltalkUs)) {
        primitiveSuccess(Oop::fromSmallInteger(smalltalkUs));
        return PrimitiveResult::Success;
    }

    // Create LargePositiveInteger for large timestamp
    Oop result = int64ToOop(memory_, smalltalkUs);
    if (result.isNil()) return PrimitiveResult::Failure;
    primitiveSuccess(result);
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveLocalMicrosecondClock(int argCount) {
    // Primitive 241: Same as 240 but for local time
    return primitiveMicrosecondClock(argCount);
}

PrimitiveResult Interpreter::primitiveHighResClock(int argCount) {
    // Named primitive: returns high-resolution clock value (nanoseconds)
    // Used by Time class>>primNanoClock for timestamps
    auto now = std::chrono::high_resolution_clock::now();
    int64_t nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();

    if (Oop::canBeSmallInteger(nanos)) {
        primitiveSuccess(Oop::fromSmallInteger(nanos));
        return PrimitiveResult::Success;
    }

    // Too large for SmallInteger — create LargePositiveInteger
    Oop result = int64ToOop(memory_, nanos);
    if (result.isNil()) return PrimitiveResult::Failure;
    primitiveSuccess(result);
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveSignalAtMilliseconds(int argCount) {
    // Primitive 136: Schedule semaphore signal at given milliseconds
    // Called as: sema signalAtMilliseconds: msecs
    // Args: semaphore (receiver at stackValue(1)), milliseconds (arg at stackValue(0))
    // The milliseconds value comes from Smalltalk as `ioMSecs + delayMs`,
    // which is a 30-bit wrapping value. Timer comparison uses wrap-around handling.

    if (argCount != 1) {
        return PrimitiveResult::Failure;
    }

    Oop msOop = stackValue(0);      // milliseconds (SmallInteger, in ioMSecs units)
    Oop semaphore = stackValue(1);  // receiver (the semaphore)

    if (!msOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t targetMs = msOop.asSmallInteger();

    // If target time is 0 or semaphore is nil, cancel the timer
    if (targetMs == 0 || semaphore.isNil()) {
        timerSemaphore_ = Oop::nil();
        nextWakeupTime_ = 0;
        primitiveSuccess(semaphore);  // Return receiver
        return PrimitiveResult::Success;
    }

    // Detect conflict: usec timer also armed?
    if (nextWakeupUsec_ != INT64_MAX) {
        fprintf(stderr, "[TIMER-CONFLICT] prim136 arming while usec timer also armed! usecTarget=%lld\n",
                (long long)nextWakeupUsec_);
    }

    // Store the timer info (in ioMSecs units, 30-bit wrapping)
    timerSemaphore_ = semaphore;
    nextWakeupTime_ = targetMs & 0x3FFFFFFF;  // Ensure 30-bit

    static int timerMs136Count = 0;
    timerMs136Count++;
    if (timerMs136Count <= 20 || timerMs136Count % 200 == 0) {
        fprintf(stderr, "[TIMER-SET136 #%d] targetMs=%lld sema=0x%llx step=%llu\n",
                timerMs136Count, (long long)targetMs, (unsigned long long)semaphore.rawBits(),
                (unsigned long long)g_stepNum);
    }

    primitiveSuccess(semaphore);  // Return receiver
    return PrimitiveResult::Success;
}

// ===== STRING PRIMITIVES =====

PrimitiveResult Interpreter::primitiveStringAt(int argCount) {
    // String>>at: - returns Character at index
    Oop index = stackValue(0);
    Oop rcvr = stackValue(1);

    if (!index.isSmallInteger() || !rcvr.isObject()) {
        return PrimitiveResult::Failure;
    }

    int64_t idx = index.asSmallInteger();
    if (idx < 1) {
        return PrimitiveResult::Failure;  // 1-based indexing
    }

    ObjectHeader* header = rcvr.asObjectPtr();
    size_t arrayIndex = static_cast<size_t>(idx - 1);
    ObjectFormat format = header->format();

    // ByteString: format 16-23 (Indexable8 through Indexable8_7)
    if (format >= ObjectFormat::Indexable8 && format <= ObjectFormat::Indexable8_7) {
        if (arrayIndex >= header->byteSize()) {
            return PrimitiveResult::Failure;
        }
        uint8_t byte = header->byteAt(arrayIndex);
        // Return as Character immediate
        primitiveSuccess(Oop::fromCharacter(byte));
        return PrimitiveResult::Success;
    }
    // WideString: format 10-11 (Indexable32, Indexable32Odd) - 32-bit characters
    else if (format == ObjectFormat::Indexable32 || format == ObjectFormat::Indexable32Odd) {
        size_t numChars = header->byteSize() / 4;
        if (arrayIndex >= numChars) {
            return PrimitiveResult::Failure;
        }
        // Read 4 bytes as little-endian 32-bit value
        // Note: Use raw bytes() instead of byteAt() since byteAt() asserts isBytesObject()
        // which is false for Indexable32 format
        size_t byteOffset = arrayIndex * 4;
        const uint8_t* data = header->bytes();
        uint32_t codePoint = data[byteOffset] |
                            (data[byteOffset + 1] << 8) |
                            (data[byteOffset + 2] << 16) |
                            (data[byteOffset + 3] << 24);
        primitiveSuccess(Oop::fromCharacter(codePoint));
        return PrimitiveResult::Success;
    }

    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveStringAtPut(int argCount) {
    // String>>at:put: - stores Character at index
    Oop value = stackValue(0);
    Oop index = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!index.isSmallInteger() || !rcvr.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Value must be a Character
    if (!value.isCharacter()) {
        return PrimitiveResult::Failure;
    }

    int64_t idx = index.asSmallInteger();
    if (idx < 1) {
        return PrimitiveResult::Failure;  // 1-based indexing
    }

    ObjectHeader* header = rcvr.asObjectPtr();

    // Check immutability
    if (header->isImmutable()) {
        return PrimitiveResult::Failure;
    }

    size_t arrayIndex = static_cast<size_t>(idx - 1);
    ObjectFormat format = header->format();
    uint32_t codePoint = value.asCharacter();

    // ByteString: format 16-23 (Indexable8 through Indexable8_7)
    if (format >= ObjectFormat::Indexable8 && format <= ObjectFormat::Indexable8_7) {
        if (arrayIndex >= header->byteSize()) {
            return PrimitiveResult::Failure;
        }
        // Check that character fits in a byte
        if (codePoint > 255) {
            return PrimitiveResult::Failure;
        }
        header->byteAtPut(arrayIndex, static_cast<uint8_t>(codePoint));
        primitiveSuccess(value);  // Return the character
        return PrimitiveResult::Success;
    }
    // WideString: format 10-11 (Indexable32, Indexable32Odd) - 32-bit characters
    else if (format == ObjectFormat::Indexable32 || format == ObjectFormat::Indexable32Odd) {
        size_t numChars = header->byteSize() / 4;
        if (arrayIndex >= numChars) {
            return PrimitiveResult::Failure;
        }
        // Store 4 bytes as little-endian 32-bit value
        // Use raw bytes() instead of byteAtPut() since byteAtPut() asserts
        // isBytesObject() which is false for Indexable32 format
        size_t byteOffset = arrayIndex * 4;
        uint8_t* data = const_cast<uint8_t*>(header->bytes());
        data[byteOffset] = codePoint & 0xFF;
        data[byteOffset + 1] = (codePoint >> 8) & 0xFF;
        data[byteOffset + 2] = (codePoint >> 16) & 0xFF;
        data[byteOffset + 3] = (codePoint >> 24) & 0xFF;
        primitiveSuccess(value);  // Return the character
        return PrimitiveResult::Success;
    }

    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveReplaceFromTo(int argCount) {
    // replaceFrom:to:with:startingAt:
    // rcvr[start..stop] := replacement[repStart..]

    static int replaceCallCount = 0;
    replaceCallCount++;
    {
        static FILE* replCallLog = nullptr;
        if (!replCallLog) replCallLog = nullptr;
        if (replCallLog && replaceCallCount <= 50) {
            fprintf(replCallLog, "[REPLACE-CALL #%d step=%llu]\n", replaceCallCount, g_stepNum);
            fflush(replCallLog);
        }
    }

    Oop repStart = stackValue(0);
    Oop replacement = stackValue(1);
    Oop stop = stackValue(2);
    Oop start = stackValue(3);
    Oop rcvr = stackValue(4);

    if (!start.isSmallInteger() || !stop.isSmallInteger() ||
        !repStart.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    if (!rcvr.isObject() || !replacement.isObject()) {
        return PrimitiveResult::Failure;
    }

    int64_t startIdx = start.asSmallInteger();
    int64_t stopIdx = stop.asSmallInteger();
    int64_t repStartIdx = repStart.asSmallInteger();

    if (startIdx < 1 || repStartIdx < 1) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* rcvrHeader = rcvr.asObjectPtr();
    ObjectHeader* replHeader = replacement.asObjectPtr();

    if (rcvrHeader->isImmutable()) {
        static FILE* replLog = nullptr;
        static int immFailCount = 0;
        if (!replLog) replLog = nullptr;
        if (replLog && immFailCount++ < 50) {
            fprintf(replLog, "[REPLACE-IMMUTABLE #%d step=%llu] rcvr=0x%llx fmt=%d classIdx=%u byteSize=%zu\n",
                    immFailCount, g_stepNum, (unsigned long long)rcvr.rawBits(),
                    (int)rcvrHeader->format(), rcvrHeader->classIndex(), rcvrHeader->byteSize());
            fprintf(replLog, "  start=%lld stop=%lld repStart=%lld\n",
                    (long long)startIdx, (long long)stopIdx, (long long)repStartIdx);
            // Get class name
            Oop cls = memory_.classOf(rcvr);
            if (cls.isObject()) {
                Oop cn = memory_.fetchPointer(6, cls);
                if (cn.isObject() && cn.rawBits() > 0x10000) {
                    ObjectHeader* cnH = cn.asObjectPtr();
                    if (cnH->isBytesObject() && cnH->byteSize() < 100)
                        fprintf(replLog, "  rcvr class: %.*s\n", (int)cnH->byteSize(), (char*)cnH->bytes());
                }
            }
            // Show rcvr content (first 50 bytes)
            if (rcvrHeader->isBytesObject() && rcvrHeader->byteSize() <= 200) {
                size_t len = std::min(rcvrHeader->byteSize(), (size_t)50);
                fprintf(replLog, "  rcvr bytes: '%.*s'\n", (int)len, (char*)rcvrHeader->bytes());
            }
            fflush(replLog);
        }
        return PrimitiveResult::Failure;
    }

    int64_t count = stopIdx - startIdx + 1;
    if (count < 0) {
        primitiveSuccess(rcvr);  // Empty range
        return PrimitiveResult::Success;
    }

    // Handle byte objects
    if (rcvrHeader->isBytesObject() && replHeader->isBytesObject()) {
        size_t rcvrSize = rcvrHeader->byteSize();
        size_t replSize = replHeader->byteSize();
        if (static_cast<size_t>(stopIdx) > rcvrSize ||
            static_cast<size_t>(repStartIdx + count - 1) > replSize) {
            static FILE* bndLog = nullptr;
            static int bndCount = 0;
            if (!bndLog) bndLog = nullptr;
            if (bndLog && bndCount++ < 50) {
                fprintf(bndLog, "[REPLACE-BOUNDS #%d step=%llu] rcvrSize=%zu replSize=%zu start=%lld stop=%lld repStart=%lld count=%lld\n",
                        bndCount, g_stepNum, rcvrSize, replSize, (long long)startIdx, (long long)stopIdx, (long long)repStartIdx, (long long)count);
                // Class names
                Oop cls = memory_.classOf(rcvr);
                if (cls.isObject()) {
                    Oop cn = memory_.fetchPointer(6, cls);
                    if (cn.isObject() && cn.rawBits() > 0x10000) {
                        ObjectHeader* cnH = cn.asObjectPtr();
                        if (cnH->isBytesObject() && cnH->byteSize() < 100)
                            fprintf(bndLog, "  rcvr class: %.*s\n", (int)cnH->byteSize(), (char*)cnH->bytes());
                    }
                }
                if (rcvrHeader->byteSize() <= 200) {
                    size_t len = std::min(rcvrHeader->byteSize(), (size_t)80);
                    fprintf(bndLog, "  rcvr bytes: '%.*s'\n", (int)len, (char*)rcvrHeader->bytes());
                }
                if (replHeader->byteSize() <= 200) {
                    size_t len = std::min(replHeader->byteSize(), (size_t)80);
                    fprintf(bndLog, "  repl bytes: '%.*s'\n", (int)len, (char*)replHeader->bytes());
                }
                fflush(bndLog);
            }
            return PrimitiveResult::Failure;
        }

        for (int64_t i = 0; i < count; ++i) {
            uint8_t byte = replHeader->byteAt(repStartIdx - 1 + i);
            rcvrHeader->byteAtPut(startIdx - 1 + i, byte);
        }

        primitiveSuccess(rcvr);
        return PrimitiveResult::Success;
    }

    // Handle 32-bit word objects (WideString, WordArray - format 10-11)
    {
        auto rcvrFmt = rcvrHeader->format();
        auto replFmt = replHeader->format();
        bool rcvrIs32 = (rcvrFmt == ObjectFormat::Indexable32 || rcvrFmt == ObjectFormat::Indexable32Odd);
        bool replIs32 = (replFmt == ObjectFormat::Indexable32 || replFmt == ObjectFormat::Indexable32Odd);

        if (rcvrIs32 && replIs32) {
            size_t rcvrWords = rcvrHeader->byteSize() / 4;
            size_t replWords = replHeader->byteSize() / 4;

            if (static_cast<size_t>(stopIdx) > rcvrWords ||
                static_cast<size_t>(repStartIdx + count - 1) > replWords) {
                return PrimitiveResult::Failure;
            }

            uint8_t* rcvrBytes = const_cast<uint8_t*>(rcvrHeader->bytes());
            const uint8_t* replBytes = replHeader->bytes();
            size_t srcOffset = static_cast<size_t>(repStartIdx - 1) * 4;
            size_t dstOffset = static_cast<size_t>(startIdx - 1) * 4;
            memmove(rcvrBytes + dstOffset, replBytes + srcOffset, static_cast<size_t>(count) * 4);

            primitiveSuccess(rcvr);
            return PrimitiveResult::Success;
        }

        // Handle 16-bit word objects (format 12-15)
        bool rcvrIs16 = (rcvrFmt >= ObjectFormat::Indexable16 && rcvrFmt <= ObjectFormat::Indexable16_3);
        bool replIs16 = (replFmt >= ObjectFormat::Indexable16 && replFmt <= ObjectFormat::Indexable16_3);

        if (rcvrIs16 && replIs16) {
            size_t rcvrShorts = rcvrHeader->byteSize() / 2;
            size_t replShorts = replHeader->byteSize() / 2;

            if (static_cast<size_t>(stopIdx) > rcvrShorts ||
                static_cast<size_t>(repStartIdx + count - 1) > replShorts) {
                return PrimitiveResult::Failure;
            }

            uint8_t* rcvrBytes = const_cast<uint8_t*>(rcvrHeader->bytes());
            const uint8_t* replBytes = replHeader->bytes();
            size_t srcOffset = static_cast<size_t>(repStartIdx - 1) * 2;
            size_t dstOffset = static_cast<size_t>(startIdx - 1) * 2;
            memmove(rcvrBytes + dstOffset, replBytes + srcOffset, static_cast<size_t>(count) * 2);

            primitiveSuccess(rcvr);
            return PrimitiveResult::Success;
        }

        // Handle 64-bit word objects (format 9)
        if (rcvrFmt == ObjectFormat::Indexable64 && replFmt == ObjectFormat::Indexable64) {
            size_t rcvrQuads = rcvrHeader->byteSize() / 8;
            size_t replQuads = replHeader->byteSize() / 8;

            if (static_cast<size_t>(stopIdx) > rcvrQuads ||
                static_cast<size_t>(repStartIdx + count - 1) > replQuads) {
                return PrimitiveResult::Failure;
            }

            uint8_t* rcvrBytes = const_cast<uint8_t*>(rcvrHeader->bytes());
            const uint8_t* replBytes = replHeader->bytes();
            size_t srcOffset = static_cast<size_t>(repStartIdx - 1) * 8;
            size_t dstOffset = static_cast<size_t>(startIdx - 1) * 8;
            memmove(rcvrBytes + dstOffset, replBytes + srcOffset, static_cast<size_t>(count) * 8);

            primitiveSuccess(rcvr);
            return PrimitiveResult::Success;
        }
    }

    // Log format mismatch failures
    if (!rcvrHeader->isPointersObject() || !replHeader->isPointersObject()) {
        // If we got here, bytes path didn't match - log why
        static FILE* replFmtLog = nullptr;
        static int fmtFailCount = 0;
        if (!replFmtLog) replFmtLog = nullptr;
        if (replFmtLog && fmtFailCount++ < 50) {
            fprintf(replFmtLog, "[REPLACE-FMT #%d step=%llu] rcvr: fmt=%d isByte=%d isPtr=%d cls=%u sz=%zu | repl: fmt=%d isByte=%d isPtr=%d cls=%u sz=%zu\n",
                    fmtFailCount, g_stepNum,
                    (int)rcvrHeader->format(), rcvrHeader->isBytesObject(), rcvrHeader->isPointersObject(), rcvrHeader->classIndex(), rcvrHeader->byteSize(),
                    (int)replHeader->format(), replHeader->isBytesObject(), replHeader->isPointersObject(), replHeader->classIndex(), replHeader->byteSize());
            fprintf(replFmtLog, "  start=%lld stop=%lld repStart=%lld\n",
                    (long long)startIdx, (long long)stopIdx, (long long)repStartIdx);
            // Class names
            auto logClassName = [&](const char* label, Oop obj) {
                Oop cls = memory_.classOf(obj);
                if (cls.isObject()) {
                    Oop cn = memory_.fetchPointer(6, cls);
                    if (cn.isObject() && cn.rawBits() > 0x10000) {
                        ObjectHeader* cnH = cn.asObjectPtr();
                        if (cnH->isBytesObject() && cnH->byteSize() < 100)
                            fprintf(replFmtLog, "  %s class: %.*s\n", label, (int)cnH->byteSize(), (char*)cnH->bytes());
                    }
                }
            };
            logClassName("rcvr", rcvr);
            logClassName("repl", replacement);
            fflush(replFmtLog);
        }
    }

    // Handle pointer objects
    // Both must be pointer objects, and we must account for fixed fields
    if (rcvrHeader->isPointersObject() && replHeader->isPointersObject()) {
        ObjectFormat rcvrFmt = rcvrHeader->format();
        ObjectFormat replFmt = replHeader->format();

        // Get fixed field counts
        size_t rcvrFixed = 0, replFixed = 0;

        if (rcvrFmt == ObjectFormat::IndexableWithFixed || rcvrFmt == ObjectFormat::WeakWithFixed) {
            Oop rcvrClass = memory_.classOf(rcvr);
            if (rcvrClass.isObject()) {
                Oop instSpec = memory_.fetchPointer(2, rcvrClass);
                if (instSpec.isSmallInteger()) rcvrFixed = instSpec.asSmallInteger() & 0xFFFF;
            }
        } else if (rcvrFmt == ObjectFormat::FixedSize || rcvrFmt == ObjectFormat::ZeroSized) {
            // Non-indexable receiver: primitive should fail
            return PrimitiveResult::Failure;
        }

        if (replFmt == ObjectFormat::IndexableWithFixed || replFmt == ObjectFormat::WeakWithFixed) {
            Oop replClass = memory_.classOf(replacement);
            if (replClass.isObject()) {
                Oop instSpec = memory_.fetchPointer(2, replClass);
                if (instSpec.isSmallInteger()) replFixed = instSpec.asSmallInteger() & 0xFFFF;
            }
        } else if (replFmt == ObjectFormat::FixedSize || replFmt == ObjectFormat::ZeroSized) {
            // Non-indexable replacement: primitive should fail
            return PrimitiveResult::Failure;
        }

        size_t rcvrSize = rcvrHeader->slotCount();
        size_t replSize = replHeader->slotCount();
        size_t rcvrIndexable = rcvrSize > rcvrFixed ? rcvrSize - rcvrFixed : 0;
        size_t replIndexable = replSize > replFixed ? replSize - replFixed : 0;

        if (static_cast<size_t>(stopIdx) > rcvrIndexable ||
            static_cast<size_t>(repStartIdx + count - 1) > replIndexable) {
            return PrimitiveResult::Failure;
        }

        for (int64_t i = 0; i < count; ++i) {
            Oop value = replHeader->slotAt(replFixed + repStartIdx - 1 + i);
            rcvrHeader->slotAtPut(rcvrFixed + startIdx - 1 + i, value);
        }

        primitiveSuccess(rcvr);
        return PrimitiveResult::Success;
    }

    return PrimitiveResult::Failure;
}

// ===== FLOAT PRIMITIVES =====

// Helper: Extract double from SmallInteger, SmallFloat, or boxed Float
static bool extractFloat(ObjectMemory& memory, Oop oop, double& result) {
    // Handle SmallIntegers - convert to double (for Float >= 0, Float + 1, etc.)
    if (oop.isSmallInteger()) {
        result = static_cast<double>(oop.asSmallInteger());
        return true;
    }
    if (oop.isSmallFloat()) {
        result = oop.asSmallFloat();
        return true;
    }
    if (oop.isObject()) {
        ObjectHeader* header = oop.asObjectPtr();
        auto fmt = header->format();
        if (fmt == ObjectFormat::Indexable64) {
            uint64_t bits = memory.fetchWord64(0, oop);
            std::memcpy(&result, &bits, sizeof(double));
            return true;
        }
        // BoxedFloat64 might use byte format (10-15) with 8 bytes of data
        uint8_t fmtVal = static_cast<uint8_t>(fmt);
        if (fmtVal >= 10 && fmtVal <= 15) {
            size_t byteSize = header->byteSize();
            if (byteSize == 8) {
                std::memcpy(&result, header->bytes(), 8);
                return true;
            }
        }
    }
    return false;
}

// Helper: Create Float result (tries SmallFloat first, then allocates boxed Float)
static Oop makeFloat(ObjectMemory& memory, double value) {
    // Try to encode as SmallFloat
    Oop result;
    if (Oop::tryFromSmallFloat(value, result)) {
        return result;
    }

    // Allocate boxed Float with format 10 (Indexable32)
    // BoxedFloat64 has instSpec 10 (32-bit word indexable), NOT 9 (64-bit).
    // With format 10, 1 slot = 2 x 32-bit words, so size = 2 (matching standard VM).
    Oop floatClass = memory.specialObject(SpecialObjectIndex::ClassFloat);
    uint32_t classIndex = memory.indexOfClass(floatClass);
    Oop floatObj = memory.allocateSlots(classIndex, 1, ObjectFormat::Indexable32);

    if (!floatObj.isNil()) {
        uint64_t bits;
        std::memcpy(&bits, &value, sizeof(double));
        memory.storeWord64(0, floatObj, bits);
    }
    return floatObj;
}

// Primitive 40: Convert integer to Float
PrimitiveResult Interpreter::primitiveAsFloat(int argCount) {
    Oop rcvr = stackTop();

    double value;
    if (rcvr.isSmallInteger()) {
        value = static_cast<double>(rcvr.asSmallInteger());
    } else if (rcvr.isObject()) {
        // Check if LargeInteger
        Oop largePositiveClass = memory_.specialObject(SpecialObjectIndex::ClassLargePositiveInteger);
        Oop largeNegativeClass = memory_.specialObject(SpecialObjectIndex::ClassLargeNegativeInteger);
        Oop objClass = memory_.classOf(rcvr);

        bool isNegative;
        if (objClass.rawBits() == largePositiveClass.rawBits()) {
            isNegative = false;
        } else if (objClass.rawBits() == largeNegativeClass.rawBits()) {
            isNegative = true;
        } else {
            return PrimitiveResult::Failure;
        }

        // Convert magnitude bytes (little-endian) to double
        size_t byteSize = memory_.byteSizeOf(rcvr);
        value = 0.0;
        double multiplier = 1.0;
        for (size_t i = 0; i < byteSize; i++) {
            uint8_t byte = memory_.fetchByte(i, rcvr);
            value += static_cast<double>(byte) * multiplier;
            multiplier *= 256.0;
        }

        if (isNegative) {
            value = -value;
        }
    } else {
        return PrimitiveResult::Failure;
    }

    Oop resultOop = makeFloat(memory_, value);
    if (resultOop.isNil()) return PrimitiveResult::Failure;

    pop();
    push(resultOop);
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveFloatAdd(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    double a, b;
    if (!extractFloat(memory_, rcvr, a) || !extractFloat(memory_, arg, b)) {
        return PrimitiveResult::Failure;
    }

    double result = a + b;
    Oop resultOop = makeFloat(memory_, result);
    if (resultOop.isNil()) return PrimitiveResult::Failure;

    popN(2);
    push(resultOop);
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveFloatSubtract(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    double a, b;
    if (!extractFloat(memory_, rcvr, a) || !extractFloat(memory_, arg, b)) {
        return PrimitiveResult::Failure;
    }

    double result = a - b;
    Oop resultOop = makeFloat(memory_, result);
    if (resultOop.isNil()) return PrimitiveResult::Failure;

    popN(2);
    push(resultOop);
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveFloatMultiply(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    double a, b;
    if (!extractFloat(memory_, rcvr, a) || !extractFloat(memory_, arg, b)) {
        return PrimitiveResult::Failure;
    }

    double result = a * b;
    Oop resultOop = makeFloat(memory_, result);
    if (resultOop.isNil()) return PrimitiveResult::Failure;

    popN(2);
    push(resultOop);
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveFloatDivide(int argCount) {
    // Primitive 50: Float division
    // Per official Pharo VM: fails on division by zero (image handles error)
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    double a, b;
    if (!extractFloat(memory_, rcvr, a) || !extractFloat(memory_, arg, b)) {
        return PrimitiveResult::Failure;
    }

    // Division by zero fails the primitive (official Pharo behavior)
    // The image-side Float>>/ handles this by raising ZeroDivide error
    if (b == 0.0) {
        return PrimitiveResult::Failure;
    }

    double result = a / b;
    Oop resultOop = makeFloat(memory_, result);
    if (resultOop.isNil()) return PrimitiveResult::Failure;

    popN(2);
    push(resultOop);
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveFloatLessThan(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    double a, b;
    if (!extractFloat(memory_, rcvr, a) || !extractFloat(memory_, arg, b)) {
        return PrimitiveResult::Failure;
    }

    popN(2);
    push(a < b ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveFloatGreaterThan(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    double a, b;
    if (!extractFloat(memory_, rcvr, a) || !extractFloat(memory_, arg, b)) {
        return PrimitiveResult::Failure;
    }

    popN(2);
    push(a > b ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveFloatLessOrEqual(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    double a, b;
    if (!extractFloat(memory_, rcvr, a) || !extractFloat(memory_, arg, b)) {
        return PrimitiveResult::Failure;
    }

    popN(2);
    push(a <= b ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveFloatGreaterOrEqual(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    double a, b;
    if (!extractFloat(memory_, rcvr, a) || !extractFloat(memory_, arg, b)) {
        return PrimitiveResult::Failure;
    }

    popN(2);
    push(a >= b ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveFloatEqual(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    double a, b;
    if (!extractFloat(memory_, rcvr, a) || !extractFloat(memory_, arg, b)) {
        return PrimitiveResult::Failure;
    }

    popN(2);
    push(a == b ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveFloatNotEqual(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    double a, b;
    if (!extractFloat(memory_, rcvr, a) || !extractFloat(memory_, arg, b)) {
        return PrimitiveResult::Failure;
    }

    popN(2);
    push(a != b ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveTruncated(int argCount) {
    Oop rcvr = stackTop();

    double value;
    if (!extractFloat(memory_, rcvr, value)) {
        return PrimitiveResult::Failure;
    }

    // Truncate toward zero
    double truncated = std::trunc(value);

    // Try to create SmallInteger from truncated value
    int64_t intValue = static_cast<int64_t>(truncated);
    Oop resultOop;
    if (Oop::tryFromSmallInteger(intValue, resultOop)) {
        pop();
        push(resultOop);
        return PrimitiveResult::Success;
    }

    // Result too large for SmallInteger - fail (needs LargeInteger)
    return PrimitiveResult::Failure;
}

// Primitive 52: Return the fractional part of a Float
PrimitiveResult Interpreter::primitiveFractionalPart(int argCount) {
    Oop rcvr = stackTop();

    double value;
    if (!extractFloat(memory_, rcvr, value)) {
        return PrimitiveResult::Failure;
    }

    double intPart;
    double fracPart = std::modf(value, &intPart);

    Oop resultOop = makeFloat(memory_, fracPart);
    if (resultOop.isNil()) return PrimitiveResult::Failure;

    pop();
    push(resultOop);
    return PrimitiveResult::Success;
}

// Primitive 53: Return the exponent of a Float (as used in IEEE representation)
PrimitiveResult Interpreter::primitiveExponent(int argCount) {
    Oop rcvr = stackTop();

    double value;
    if (!extractFloat(memory_, rcvr, value)) {
        return PrimitiveResult::Failure;
    }

    if (value == 0.0) {
        // Exponent of zero is conventionally 0 or undefined
        pop();
        push(Oop::fromSmallInteger(0));
        return PrimitiveResult::Success;
    }

    // Get the exponent using frexp (returns value in range [0.5, 1) and exponent)
    int exponent;
    std::frexp(value, &exponent);
    // Adjust because frexp returns mantissa in [0.5, 1), Smalltalk expects [1, 2)
    exponent--;

    pop();
    push(Oop::fromSmallInteger(exponent));
    return PrimitiveResult::Success;
}

// Primitive 54: Multiply a Float by a power of 2 (receiver * 2^arg)
PrimitiveResult Interpreter::primitiveTimesTwoPower(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    if (!arg.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }
    int64_t power = arg.asSmallInteger();

    double value;
    if (!extractFloat(memory_, rcvr, value)) {
        return PrimitiveResult::Failure;
    }

    // Clamp exponent to safe range for ldexp (int argument)
    // IEEE 754 double exponent range is roughly -1074 to +1023
    // Extreme values just result in 0 or infinity, but clamp to avoid int overflow
    int clampedPower;
    if (power > 10000) {
        clampedPower = 10000;  // Will produce infinity for any non-zero value
    } else if (power < -10000) {
        clampedPower = -10000;  // Will produce zero for any finite value
    } else {
        clampedPower = static_cast<int>(power);
    }

    // Use ldexp to multiply by 2^power efficiently
    double result = std::ldexp(value, clampedPower);

    Oop resultOop = makeFloat(memory_, result);
    if (resultOop.isNil()) return PrimitiveResult::Failure;

    popN(2);
    push(resultOop);
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveSquareRoot(int argCount) {
    Oop rcvr = stackTop();

    double value;
    if (!extractFloat(memory_, rcvr, value)) {
        return PrimitiveResult::Failure;
    }

    if (value < 0.0) {
        return PrimitiveResult::Failure;  // Negative number
    }

    double result = std::sqrt(value);
    Oop resultOop = makeFloat(memory_, result);
    if (resultOop.isNil()) return PrimitiveResult::Failure;

    pop();
    push(resultOop);
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveSine(int argCount) {
    Oop rcvr = stackTop();

    double value;
    if (!extractFloat(memory_, rcvr, value)) {
        return PrimitiveResult::Failure;
    }

    double result = std::sin(value);
    Oop resultOop = makeFloat(memory_, result);
    if (resultOop.isNil()) return PrimitiveResult::Failure;

    pop();
    push(resultOop);
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveArctan(int argCount) {
    Oop rcvr = stackTop();

    double value;
    if (!extractFloat(memory_, rcvr, value)) {
        return PrimitiveResult::Failure;
    }

    double result = std::atan(value);
    Oop resultOop = makeFloat(memory_, result);
    if (resultOop.isNil()) return PrimitiveResult::Failure;

    pop();
    push(resultOop);
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveExp(int argCount) {
    Oop rcvr = stackTop();

    double value;
    if (!extractFloat(memory_, rcvr, value)) {
        return PrimitiveResult::Failure;
    }

    double result = std::exp(value);
    Oop resultOop = makeFloat(memory_, result);
    if (resultOop.isNil()) return PrimitiveResult::Failure;

    pop();
    push(resultOop);
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveLogN(int argCount) {
    // Primitive 58: Natural logarithm
    // Per official Pharo VM: allows log(0) -> -Infinity, log(negative) -> NaN
    // (no explicit check, just uses IEEE 754 behavior)
    Oop rcvr = stackTop();

    double value;
    if (!extractFloat(memory_, rcvr, value)) {
        return PrimitiveResult::Failure;
    }

    // Official VM doesn't check for non-positive - allows IEEE 754 special values
    // log(0) = -Infinity, log(negative) = NaN
    double result = std::log(value);
    Oop resultOop = makeFloat(memory_, result);
    if (resultOop.isNil()) return PrimitiveResult::Failure;

    pop();
    push(resultOop);
    return PrimitiveResult::Success;
}

// ===== LARGE INTEGER PRIMITIVES =====

// Helper: Try to extract a signed 64-bit integer from an Oop
// Returns true if successful, storing the result in 'value'
// Works for SmallIntegers and LargeIntegers that fit in 64 bits
static bool trySigned64BitValueOf(ObjectMemory& memory, Oop oop, int64_t& value) {
    if (oop.isSmallInteger()) {
        value = oop.asSmallInteger();
        return true;
    }

    if (!oop.isObject()) return false;

    Oop largePositiveClass = memory.specialObject(SpecialObjectIndex::ClassLargePositiveInteger);
    Oop largeNegativeClass = memory.specialObject(SpecialObjectIndex::ClassLargeNegativeInteger);
    Oop objClass = memory.classOf(oop);

    bool isNegative = false;
    if (objClass.rawBits() == largePositiveClass.rawBits()) {
        isNegative = false;
    } else if (objClass.rawBits() == largeNegativeClass.rawBits()) {
        isNegative = true;
    } else {
        return false;  // Not a LargeInteger
    }

    // Extract bytes (little-endian)
    size_t byteSize = memory.byteSizeOf(oop);
    if (byteSize > 8) return false;  // Too large for 64 bits

    uint64_t mag = 0;
    for (size_t i = 0; i < byteSize; i++) {
        mag |= static_cast<uint64_t>(memory.fetchByte(i, oop)) << (i * 8);
    }

    // Check if it fits in signed 64 bits
    if (isNegative) {
        // For negative, magnitude must be <= 2^63 (to represent -2^63 to -1)
        if (mag > 0x8000000000000000ULL) return false;
        value = -static_cast<int64_t>(mag);
    } else {
        // For positive, magnitude must be < 2^63
        if (mag >= 0x8000000000000000ULL) return false;
        value = static_cast<int64_t>(mag);
    }
    return true;
}

// Helper: Extract unsigned 64-bit value from SmallInteger or LargePositiveInteger
static bool tryUnsigned64BitValueOf(ObjectMemory& memory, Oop oop, uint64_t& value) {
    if (oop.isSmallInteger()) {
        int64_t sv = oop.asSmallInteger();
        if (sv < 0) return false;
        value = static_cast<uint64_t>(sv);
        return true;
    }

    if (!oop.isObject()) return false;

    Oop largePositiveClass = memory.specialObject(SpecialObjectIndex::ClassLargePositiveInteger);
    Oop objClass = memory.classOf(oop);
    if (objClass.rawBits() != largePositiveClass.rawBits()) return false;

    size_t byteSize = memory.byteSizeOf(oop);
    if (byteSize > 8) return false;

    value = 0;
    for (size_t i = 0; i < byteSize; i++) {
        value |= static_cast<uint64_t>(memory.fetchByte(i, oop)) << (i * 8);
    }
    return true;
}

// Helper: Check if Oop is a LargeInteger (positive or negative)
static bool isLargeInteger(ObjectMemory& memory, Oop oop, bool& isNegative) {
    if (!oop.isObject()) return false;

    Oop largePositiveClass = memory.specialObject(SpecialObjectIndex::ClassLargePositiveInteger);
    Oop largeNegativeClass = memory.specialObject(SpecialObjectIndex::ClassLargeNegativeInteger);
    Oop objClass = memory.classOf(oop);

    if (objClass.rawBits() == largePositiveClass.rawBits()) {
        isNegative = false;
        return true;
    }
    if (objClass.rawBits() == largeNegativeClass.rawBits()) {
        isNegative = true;
        return true;
    }
    return false;
}

// Helper: Extract magnitude bytes into a vector (little-endian)
static std::vector<uint8_t> extractMagnitude(ObjectMemory& memory, Oop largeInt) {
    size_t byteSize = memory.byteSizeOf(largeInt);
    std::vector<uint8_t> result(byteSize);
    for (size_t i = 0; i < byteSize; i++) {
        result[i] = memory.fetchByte(i, largeInt);
    }
    // Remove trailing zeros (normalize)
    while (result.size() > 1 && result.back() == 0) {
        result.pop_back();
    }
    return result;
}

// Helper: Compare magnitudes (returns -1, 0, or 1)
static int compareMagnitudes(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.size() != b.size()) {
        return a.size() < b.size() ? -1 : 1;
    }
    for (size_t i = a.size(); i > 0; i--) {
        if (a[i-1] != b[i-1]) {
            return a[i-1] < b[i-1] ? -1 : 1;
        }
    }
    return 0;
}

// Helper: Add two magnitudes
static std::vector<uint8_t> addMagnitudes(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    size_t maxLen = std::max(a.size(), b.size());
    std::vector<uint8_t> result(maxLen + 1, 0);

    uint16_t carry = 0;
    for (size_t i = 0; i < maxLen || carry; i++) {
        uint16_t sum = carry;
        if (i < a.size()) sum += a[i];
        if (i < b.size()) sum += b[i];
        if (i < result.size()) {
            result[i] = static_cast<uint8_t>(sum & 0xFF);
        } else {
            result.push_back(static_cast<uint8_t>(sum & 0xFF));
        }
        carry = sum >> 8;
    }

    // Remove trailing zeros
    while (result.size() > 1 && result.back() == 0) {
        result.pop_back();
    }
    return result;
}

// Helper: Subtract magnitudes (assumes a >= b)
static std::vector<uint8_t> subtractMagnitudes(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    std::vector<uint8_t> result(a.size(), 0);

    int16_t borrow = 0;
    for (size_t i = 0; i < a.size(); i++) {
        int16_t diff = static_cast<int16_t>(a[i]) - borrow;
        if (i < b.size()) diff -= b[i];
        if (diff < 0) {
            diff += 256;
            borrow = 1;
        } else {
            borrow = 0;
        }
        result[i] = static_cast<uint8_t>(diff);
    }

    // Remove trailing zeros
    while (result.size() > 1 && result.back() == 0) {
        result.pop_back();
    }
    return result;
}

// Helper: Schoolbook multiplication for small operands
static std::vector<uint8_t> schoolbookMultiply(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    std::vector<uint8_t> result(a.size() + b.size() + 1, 0);

    for (size_t i = 0; i < a.size(); i++) {
        uint32_t carry = 0;
        for (size_t j = 0; j < b.size() || carry; j++) {
            size_t pos = i + j;
            if (pos >= result.size()) break;
            uint32_t prod = result[pos] + carry;
            if (j < b.size()) {
                prod += static_cast<uint32_t>(a[i]) * b[j];
            }
            result[pos] = static_cast<uint8_t>(prod & 0xFF);
            carry = prod >> 8;
        }
    }

    while (result.size() > 1 && result.back() == 0) {
        result.pop_back();
    }
    return result;
}

// Helper: Add two magnitudes (for Karatsuba)
static std::vector<uint8_t> addMag(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    size_t maxLen = std::max(a.size(), b.size());
    std::vector<uint8_t> result(maxLen + 1, 0);
    uint32_t carry = 0;
    for (size_t i = 0; i < maxLen || carry; i++) {
        if (i >= result.size()) break;
        uint32_t sum = carry;
        if (i < a.size()) sum += a[i];
        if (i < b.size()) sum += b[i];
        result[i] = static_cast<uint8_t>(sum & 0xFF);
        carry = sum >> 8;
    }
    while (result.size() > 1 && result.back() == 0) result.pop_back();
    return result;
}

// Helper: Subtract magnitudes (a >= b required, for Karatsuba)
static std::vector<uint8_t> subMag(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    std::vector<uint8_t> result(a.size(), 0);
    int32_t borrow = 0;
    for (size_t i = 0; i < a.size(); i++) {
        int32_t diff = static_cast<int32_t>(a[i]) - borrow;
        if (i < b.size()) diff -= b[i];
        if (diff < 0) { diff += 256; borrow = 1; }
        else { borrow = 0; }
        result[i] = static_cast<uint8_t>(diff);
    }
    while (result.size() > 1 && result.back() == 0) result.pop_back();
    return result;
}

// Karatsuba multiplication: O(n^1.585) instead of O(n^2)
// Threshold: use schoolbook for small operands
static constexpr size_t KARATSUBA_THRESHOLD = 32;

static std::vector<uint8_t> multiplyMagnitudes(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.empty() || b.empty() || (a.size() == 1 && a[0] == 0) || (b.size() == 1 && b[0] == 0)) {
        return std::vector<uint8_t>(1, 0);
    }

    // Use schoolbook for small operands
    if (a.size() < KARATSUBA_THRESHOLD || b.size() < KARATSUBA_THRESHOLD) {
        return schoolbookMultiply(a, b);
    }

    // Karatsuba: split each number at midpoint
    // a = a1 * B^m + a0,  b = b1 * B^m + b0  where B=256, m=half
    size_t m = std::min(a.size(), b.size()) / 2;

    // Split a into a0 (low m bytes) and a1 (high bytes)
    std::vector<uint8_t> a0(a.begin(), a.begin() + m);
    std::vector<uint8_t> a1(a.begin() + m, a.end());
    // Split b into b0 (low m bytes) and b1 (high bytes)
    std::vector<uint8_t> b0(b.begin(), b.begin() + m);
    std::vector<uint8_t> b1(b.begin() + m, b.end());

    // Trim trailing zeros from halves
    while (a0.size() > 1 && a0.back() == 0) a0.pop_back();
    while (a1.size() > 1 && a1.back() == 0) a1.pop_back();
    while (b0.size() > 1 && b0.back() == 0) b0.pop_back();
    while (b1.size() > 1 && b1.back() == 0) b1.pop_back();

    // Three recursive multiplications (instead of four)
    std::vector<uint8_t> z0 = multiplyMagnitudes(a0, b0);           // a0 * b0
    std::vector<uint8_t> z2 = multiplyMagnitudes(a1, b1);           // a1 * b1
    std::vector<uint8_t> a0a1 = addMag(a0, a1);                    // a0 + a1
    std::vector<uint8_t> b0b1 = addMag(b0, b1);                    // b0 + b1
    std::vector<uint8_t> z1full = multiplyMagnitudes(a0a1, b0b1);   // (a0+a1)*(b0+b1)

    // z1 = z1full - z2 - z0
    std::vector<uint8_t> z1 = subMag(subMag(z1full, z2), z0);

    // Result = z2 * B^(2m) + z1 * B^m + z0
    // Shift z2 left by 2m bytes, z1 left by m bytes
    std::vector<uint8_t> result = z0;
    result.resize(std::max(result.size(), m + z1.size()), 0);
    result.resize(std::max(result.size(), 2 * m + z2.size()), 0);

    // Add z1 << m
    uint32_t carry = 0;
    for (size_t i = 0; i < z1.size() || carry; i++) {
        size_t pos = m + i;
        if (pos >= result.size()) result.push_back(0);
        uint32_t sum = result[pos] + carry;
        if (i < z1.size()) sum += z1[i];
        result[pos] = static_cast<uint8_t>(sum & 0xFF);
        carry = sum >> 8;
    }

    // Add z2 << 2m
    carry = 0;
    for (size_t i = 0; i < z2.size() || carry; i++) {
        size_t pos = 2 * m + i;
        if (pos >= result.size()) result.push_back(0);
        uint32_t sum = result[pos] + carry;
        if (i < z2.size()) sum += z2[i];
        result[pos] = static_cast<uint8_t>(sum & 0xFF);
        carry = sum >> 8;
    }

    while (result.size() > 1 && result.back() == 0) result.pop_back();
    return result;
}

// Helper: Divide magnitudes, returns quotient and remainder
static void divideMagnitudes(const std::vector<uint8_t>& dividend, const std::vector<uint8_t>& divisor,
                             std::vector<uint8_t>& quotient, std::vector<uint8_t>& remainder) {
    // Handle division by zero
    if (divisor.empty() || (divisor.size() == 1 && divisor[0] == 0)) {
        quotient = std::vector<uint8_t>(1, 0);
        remainder = std::vector<uint8_t>(1, 0);
        return;
    }

    // If dividend < divisor, quotient = 0, remainder = dividend
    if (compareMagnitudes(dividend, divisor) < 0) {
        quotient = std::vector<uint8_t>(1, 0);
        remainder = dividend;
        return;
    }

    // Fast path: single-byte divisor (covers dividing by 10, printing, etc.)
    if (divisor.size() == 1) {
        uint8_t d = divisor[0];
        quotient.resize(dividend.size(), 0);
        uint16_t rem = 0;
        for (int i = static_cast<int>(dividend.size()) - 1; i >= 0; i--) {
            rem = (rem << 8) | dividend[i];
            quotient[i] = static_cast<uint8_t>(rem / d);
            rem = rem % d;
        }
        remainder = { static_cast<uint8_t>(rem) };
        while (quotient.size() > 1 && quotient.back() == 0) quotient.pop_back();
        return;
    }

    // Multi-byte divisor: long division with trial quotient estimation
    remainder.clear();
    quotient.resize(dividend.size(), 0);

    for (int i = static_cast<int>(dividend.size()) - 1; i >= 0; i--) {
        // Shift remainder left by 8 bits and add next byte
        remainder.insert(remainder.begin(), dividend[i]);

        // Remove leading zeros from remainder for comparison
        while (remainder.size() > 1 && remainder.back() == 0) {
            remainder.pop_back();
        }

        // Estimate quotient digit using top bytes
        uint8_t q = 0;
        if (compareMagnitudes(remainder, divisor) >= 0) {
            if (remainder.size() > divisor.size()) {
                // remainder has more bytes — estimate from top 2 bytes / top byte
                uint16_t top = (static_cast<uint16_t>(remainder.back()) << 8);
                if (remainder.size() >= 2) top |= remainder[remainder.size() - 2];
                q = static_cast<uint8_t>(std::min<uint16_t>(255, top / divisor.back()));
            } else {
                // Same size — estimate from top bytes
                q = remainder.back() / divisor.back();
            }

            // Multiply divisor by estimated q and check
            if (q > 0) {
                // Compute divisor * q
                std::vector<uint8_t> product(divisor.size() + 1, 0);
                uint16_t carry = 0;
                for (size_t j = 0; j < divisor.size(); j++) {
                    uint16_t v = static_cast<uint16_t>(divisor[j]) * q + carry;
                    product[j] = v & 0xFF;
                    carry = v >> 8;
                }
                product[divisor.size()] = static_cast<uint8_t>(carry);
                while (product.size() > 1 && product.back() == 0) product.pop_back();

                // Adjust q down if over-estimated
                while (compareMagnitudes(product, remainder) > 0 && q > 0) {
                    q--;
                    product = subtractMagnitudes(product, divisor);
                }

                if (q > 0) {
                    remainder = subtractMagnitudes(remainder, product);
                }
            }

            // Handle any remaining (q was under-estimated by at most 1-2)
            while (compareMagnitudes(remainder, divisor) >= 0) {
                remainder = subtractMagnitudes(remainder, divisor);
                q++;
            }
        }
        quotient[i] = q;
    }

    // Remove trailing zeros from quotient
    while (quotient.size() > 1 && quotient.back() == 0) {
        quotient.pop_back();
    }
}

// Helper: Check if magnitude fits in SmallInteger and convert
static bool tryConvertToSmallInteger(const std::vector<uint8_t>& magnitude, bool isNegative, Oop& result) {
    // SmallInteger is 61 bits, so max 8 bytes but must check range
    if (magnitude.size() > 8) return false;

    uint64_t value = 0;
    for (size_t i = magnitude.size(); i > 0; i--) {
        value = (value << 8) | magnitude[i - 1];
    }

    // Check if fits in SmallInteger range (61 bits signed)
    const int64_t maxPositive = (1LL << 60) - 1;
    const int64_t minNegative = -(1LL << 60);

    if (isNegative) {
        if (value > static_cast<uint64_t>(-minNegative)) return false;
        int64_t signedValue = -static_cast<int64_t>(value);
        return Oop::tryFromSmallInteger(signedValue, result);
    } else {
        if (value > static_cast<uint64_t>(maxPositive)) return false;
        return Oop::tryFromSmallInteger(static_cast<int64_t>(value), result);
    }
}

// Helper: Create LargeInteger from magnitude and sign
static Oop makeLargeInteger(ObjectMemory& memory, const std::vector<uint8_t>& magnitude, bool isNegative) {
    // First try to convert to SmallInteger
    Oop result;
    if (tryConvertToSmallInteger(magnitude, isNegative, result)) {
        return result;
    }

    // Allocate LargeInteger
    Oop intClass = isNegative
        ? memory.specialObject(SpecialObjectIndex::ClassLargeNegativeInteger)
        : memory.specialObject(SpecialObjectIndex::ClassLargePositiveInteger);
    uint32_t classIndex = memory.indexOfClass(intClass);
    if (classIndex == 0) classIndex = memory.registerClass(intClass);

    Oop largeInt = memory.allocateBytes(classIndex, magnitude.size());
    if (largeInt.isNil()) return largeInt;

    // Store magnitude bytes
    for (size_t i = 0; i < magnitude.size(); i++) {
        memory.storeByte(i, largeInt, magnitude[i]);
    }

    return largeInt;
}

// Helper: Extract integer value (SmallInteger or LargeInteger)
static bool extractInteger(ObjectMemory& memory, Oop oop, std::vector<uint8_t>& magnitude, bool& isNegative) {
    if (oop.isSmallInteger()) {
        int64_t value = oop.asSmallInteger();
        isNegative = value < 0;
        uint64_t absValue = isNegative ? static_cast<uint64_t>(-value) : static_cast<uint64_t>(value);

        magnitude.clear();
        if (absValue == 0) {
            magnitude.push_back(0);
        } else {
            while (absValue > 0) {
                magnitude.push_back(static_cast<uint8_t>(absValue & 0xFF));
                absValue >>= 8;
            }
        }
        return true;
    }

    if (isLargeInteger(memory, oop, isNegative)) {
        magnitude = extractMagnitude(memory, oop);
        return true;
    }

    return false;
}

PrimitiveResult Interpreter::primitiveAddLargeIntegers(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    std::vector<uint8_t> aMag, bMag;
    bool aNeg, bNeg;

    if (!extractInteger(memory_, rcvr, aMag, aNeg) ||
        !extractInteger(memory_, arg, bMag, bNeg)) {
        return PrimitiveResult::Failure;
    }

    std::vector<uint8_t> resultMag;
    bool resultNeg;

    if (aNeg == bNeg) {
        // Same sign: add magnitudes, keep sign
        resultMag = addMagnitudes(aMag, bMag);
        resultNeg = aNeg;
    } else {
        // Different signs: subtract smaller from larger
        int cmp = compareMagnitudes(aMag, bMag);
        if (cmp >= 0) {
            resultMag = subtractMagnitudes(aMag, bMag);
            resultNeg = aNeg;
        } else {
            resultMag = subtractMagnitudes(bMag, aMag);
            resultNeg = bNeg;
        }
    }

    // Handle zero (always positive)
    if (resultMag.size() == 1 && resultMag[0] == 0) {
        resultNeg = false;
    }

    Oop result = makeLargeInteger(memory_, resultMag, resultNeg);
    if (result.isNil()) return PrimitiveResult::Failure;

    popN(2);
    push(result);
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveSubtractLargeIntegers(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    std::vector<uint8_t> aMag, bMag;
    bool aNeg, bNeg;

    if (!extractInteger(memory_, rcvr, aMag, aNeg) ||
        !extractInteger(memory_, arg, bMag, bNeg)) {
        return PrimitiveResult::Failure;
    }

    // a - b = a + (-b)
    bNeg = !bNeg;

    std::vector<uint8_t> resultMag;
    bool resultNeg;

    if (aNeg == bNeg) {
        // Same sign: add magnitudes, keep sign
        resultMag = addMagnitudes(aMag, bMag);
        resultNeg = aNeg;
    } else {
        // Different signs: subtract smaller from larger
        int cmp = compareMagnitudes(aMag, bMag);
        if (cmp >= 0) {
            resultMag = subtractMagnitudes(aMag, bMag);
            resultNeg = aNeg;
        } else {
            resultMag = subtractMagnitudes(bMag, aMag);
            resultNeg = bNeg;
        }
    }

    // Handle zero (always positive)
    if (resultMag.size() == 1 && resultMag[0] == 0) {
        resultNeg = false;
    }

    Oop result = makeLargeInteger(memory_, resultMag, resultNeg);
    if (result.isNil()) return PrimitiveResult::Failure;

    popN(2);
    push(result);
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveMultiplyLargeIntegers(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    std::vector<uint8_t> aMag, bMag;
    bool aNeg, bNeg;

    if (!extractInteger(memory_, rcvr, aMag, aNeg) ||
        !extractInteger(memory_, arg, bMag, bNeg)) {
        return PrimitiveResult::Failure;
    }

    std::vector<uint8_t> resultMag = multiplyMagnitudes(aMag, bMag);
    bool resultNeg = (aNeg != bNeg);

    // Handle zero (always positive)
    if (resultMag.size() == 1 && resultMag[0] == 0) {
        resultNeg = false;
    }

    Oop result = makeLargeInteger(memory_, resultMag, resultNeg);
    if (result.isNil()) return PrimitiveResult::Failure;

    popN(2);
    push(result);
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveDivideLargeIntegers(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    std::vector<uint8_t> aMag, bMag;
    bool aNeg, bNeg;

    if (!extractInteger(memory_, rcvr, aMag, aNeg) ||
        !extractInteger(memory_, arg, bMag, bNeg)) {
        return PrimitiveResult::Failure;
    }

    // Check for division by zero
    if (bMag.size() == 1 && bMag[0] == 0) {
        return PrimitiveResult::Failure;
    }

    std::vector<uint8_t> quotient, remainder;
    divideMagnitudes(aMag, bMag, quotient, remainder);

    // Check for exact division (remainder must be zero)
    if (!(remainder.size() == 1 && remainder[0] == 0)) {
        return PrimitiveResult::Failure;  // Not exact division
    }

    bool resultNeg = (aNeg != bNeg);

    // Handle zero (always positive)
    if (quotient.size() == 1 && quotient[0] == 0) {
        resultNeg = false;
    }

    Oop result = makeLargeInteger(memory_, quotient, resultNeg);
    if (result.isNil()) return PrimitiveResult::Failure;

    popN(2);
    push(result);
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveModLargeIntegers(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    std::vector<uint8_t> aMag, bMag;
    bool aNeg, bNeg;

    if (!extractInteger(memory_, rcvr, aMag, aNeg) ||
        !extractInteger(memory_, arg, bMag, bNeg)) {
        return PrimitiveResult::Failure;
    }

    // Check for division by zero
    if (bMag.size() == 1 && bMag[0] == 0) {
        return PrimitiveResult::Failure;
    }

    std::vector<uint8_t> quotient, remainder;
    divideMagnitudes(aMag, bMag, quotient, remainder);

    // Smalltalk mod (\\) returns result with same sign as divisor
    // C-style division gives remainder with sign of dividend
    // When signs differ and remainder is non-zero, adjust: remainder = b - remainder
    bool remZero = (remainder.size() == 1 && remainder[0] == 0);

    if (!remZero && (aNeg != bNeg)) {
        // Adjust remainder: result = divisor - remainder
        // Subtract remainder from divisor magnitude
        std::vector<uint8_t> adjusted;
        int borrow = 0;
        size_t maxLen = std::max(bMag.size(), remainder.size());
        adjusted.resize(maxLen);

        for (size_t i = 0; i < maxLen; i++) {
            int bVal = (i < bMag.size()) ? bMag[i] : 0;
            int rVal = (i < remainder.size()) ? remainder[i] : 0;
            int diff = bVal - rVal - borrow;
            if (diff < 0) {
                diff += 256;
                borrow = 1;
            } else {
                borrow = 0;
            }
            adjusted[i] = static_cast<uint8_t>(diff);
        }

        // Remove leading zeros
        while (adjusted.size() > 1 && adjusted.back() == 0) {
            adjusted.pop_back();
        }
        remainder = adjusted;
    }

    // Result has sign of divisor (unless zero)
    remZero = (remainder.size() == 1 && remainder[0] == 0);
    bool resultNeg = bNeg && !remZero;

    Oop result = makeLargeInteger(memory_, remainder, resultNeg);
    if (result.isNil()) return PrimitiveResult::Failure;

    popN(2);
    push(result);
    return PrimitiveResult::Success;
}

// Primitive 20: LargeInteger rem: - C-style remainder (sign of dividend)
// Unlike mod (\\) which has sign of divisor, rem: has sign of dividend
PrimitiveResult Interpreter::primitiveRemLargeIntegers(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    std::vector<uint8_t> aMag, bMag;
    bool aNeg, bNeg;

    if (!extractInteger(memory_, rcvr, aMag, aNeg) ||
        !extractInteger(memory_, arg, bMag, bNeg)) {
        return PrimitiveResult::Failure;
    }

    // Check for division by zero
    if (bMag.size() == 1 && bMag[0] == 0) {
        return PrimitiveResult::Failure;
    }

    std::vector<uint8_t> quotient, remainder;
    divideMagnitudes(aMag, bMag, quotient, remainder);

    // Result has sign of dividend (unlike mod which has sign of divisor)
    bool remZero = (remainder.size() == 1 && remainder[0] == 0);
    bool resultNeg = aNeg && !remZero;  // Sign of dividend

    Oop resultOop;
    if (tryConvertToSmallInteger(remainder, resultNeg, resultOop)) {
        primitiveSuccess(resultOop);
        return PrimitiveResult::Success;
    }

    resultOop = makeLargeInteger(memory_, remainder, resultNeg);
    if (resultOop.isNil()) return PrimitiveResult::Failure;

    primitiveSuccess(resultOop);
    return PrimitiveResult::Success;
}

// Helper: Compare two integers (SmallInteger or LargeInteger)
// Returns -1, 0, or 1
static int compareIntegers(ObjectMemory& memory, Oop a, Oop b) {
    std::vector<uint8_t> aMag, bMag;
    bool aNeg, bNeg;

    if (!extractInteger(memory, a, aMag, aNeg) ||
        !extractInteger(memory, b, bMag, bNeg)) {
        return 0;  // Error case
    }

    // Handle zero specially
    bool aIsZero = (aMag.size() == 1 && aMag[0] == 0);
    bool bIsZero = (bMag.size() == 1 && bMag[0] == 0);

    if (aIsZero && bIsZero) return 0;
    if (aIsZero) return bNeg ? 1 : -1;
    if (bIsZero) return aNeg ? -1 : 1;

    // Different signs
    if (aNeg && !bNeg) return -1;
    if (!aNeg && bNeg) return 1;

    // Same sign - compare magnitudes
    int magCmp = compareMagnitudes(aMag, bMag);

    // If both negative, reverse the comparison
    if (aNeg) magCmp = -magCmp;

    return magCmp;
}

PrimitiveResult Interpreter::primitiveLessThanLargeIntegers(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    std::vector<uint8_t> aMag, bMag;
    bool aNeg, bNeg;

    if (!extractInteger(memory_, rcvr, aMag, aNeg) ||
        !extractInteger(memory_, arg, bMag, bNeg)) {
        return PrimitiveResult::Failure;
    }

    int cmp = compareIntegers(memory_, rcvr, arg);
    primitiveSuccess(cmp < 0 ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveGreaterThanLargeIntegers(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    std::vector<uint8_t> aMag, bMag;
    bool aNeg, bNeg;

    if (!extractInteger(memory_, rcvr, aMag, aNeg) ||
        !extractInteger(memory_, arg, bMag, bNeg)) {
        return PrimitiveResult::Failure;
    }

    int cmp = compareIntegers(memory_, rcvr, arg);
    primitiveSuccess(cmp > 0 ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveLessOrEqualLargeIntegers(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    std::vector<uint8_t> aMag, bMag;
    bool aNeg, bNeg;

    if (!extractInteger(memory_, rcvr, aMag, aNeg) ||
        !extractInteger(memory_, arg, bMag, bNeg)) {
        return PrimitiveResult::Failure;
    }

    int cmp = compareIntegers(memory_, rcvr, arg);
    primitiveSuccess(cmp <= 0 ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveGreaterOrEqualLargeIntegers(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    std::vector<uint8_t> aMag, bMag;
    bool aNeg, bNeg;

    if (!extractInteger(memory_, rcvr, aMag, aNeg) ||
        !extractInteger(memory_, arg, bMag, bNeg)) {
        return PrimitiveResult::Failure;
    }

    int cmp = compareIntegers(memory_, rcvr, arg);
    primitiveSuccess(cmp >= 0 ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveEqualLargeIntegers(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    std::vector<uint8_t> aMag, bMag;
    bool aNeg, bNeg;

    if (!extractInteger(memory_, rcvr, aMag, aNeg) ||
        !extractInteger(memory_, arg, bMag, bNeg)) {
        return PrimitiveResult::Failure;
    }

    int cmp = compareIntegers(memory_, rcvr, arg);
    primitiveSuccess(cmp == 0 ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveNotEqualLargeIntegers(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    std::vector<uint8_t> aMag, bMag;
    bool aNeg, bNeg;

    if (!extractInteger(memory_, rcvr, aMag, aNeg) ||
        !extractInteger(memory_, arg, bMag, bNeg)) {
        return PrimitiveResult::Failure;
    }

    int cmp = compareIntegers(memory_, rcvr, arg);
    primitiveSuccess(cmp != 0 ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

// Primitive 32: Integer division (truncates toward negative infinity)
PrimitiveResult Interpreter::primitiveDivLargeIntegers(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    std::vector<uint8_t> aMag, bMag;
    bool aNeg, bNeg;

    if (!extractInteger(memory_, rcvr, aMag, aNeg) ||
        !extractInteger(memory_, arg, bMag, bNeg)) {
        return PrimitiveResult::Failure;
    }

    // Check for division by zero
    bool bIsZero = bMag.empty() || (bMag.size() == 1 && bMag[0] == 0);
    if (bIsZero) {
        return PrimitiveResult::Failure;
    }

    // Perform unsigned division
    std::vector<uint8_t> quotient, remainder;
    divideMagnitudes(aMag, bMag, quotient, remainder);

    // Result sign: negative if signs differ
    bool resultNeg = (aNeg != bNeg);

    // For div (floor division), if signs differ and there's a remainder, adjust
    bool hasRemainder = !remainder.empty() && !(remainder.size() == 1 && remainder[0] == 0);
    if (resultNeg && hasRemainder) {
        // Add 1 to magnitude for floor division
        uint16_t carry = 1;
        for (size_t i = 0; i < quotient.size() && carry; i++) {
            uint16_t sum = quotient[i] + carry;
            quotient[i] = sum & 0xFF;
            carry = sum >> 8;
        }
        if (carry) quotient.push_back(1);
    }

    // Handle zero result
    if (quotient.empty() || (quotient.size() == 1 && quotient[0] == 0)) {
        resultNeg = false;
    }

    Oop resultOop;
    if (tryConvertToSmallInteger(quotient, resultNeg, resultOop)) {
        primitiveSuccess(resultOop);
        return PrimitiveResult::Success;
    }

    resultOop = makeLargeInteger(memory_, quotient, resultNeg);
    if (resultOop.isNil()) return PrimitiveResult::Failure;

    primitiveSuccess(resultOop);
    return PrimitiveResult::Success;
}

// Primitive 33: Integer quotient (truncates toward zero)
PrimitiveResult Interpreter::primitiveQuoLargeIntegers(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    std::vector<uint8_t> aMag, bMag;
    bool aNeg, bNeg;

    if (!extractInteger(memory_, rcvr, aMag, aNeg) ||
        !extractInteger(memory_, arg, bMag, bNeg)) {
        return PrimitiveResult::Failure;
    }

    // Check for division by zero
    bool bIsZero = bMag.empty() || (bMag.size() == 1 && bMag[0] == 0);
    if (bIsZero) {
        return PrimitiveResult::Failure;
    }

    // Perform unsigned division
    std::vector<uint8_t> quotient, remainder;
    divideMagnitudes(aMag, bMag, quotient, remainder);

    // Result sign: negative if signs differ (quo truncates toward zero)
    bool resultNeg = (aNeg != bNeg);

    // Handle zero result
    if (quotient.empty() || (quotient.size() == 1 && quotient[0] == 0)) {
        resultNeg = false;
    }

    Oop resultOop;
    if (tryConvertToSmallInteger(quotient, resultNeg, resultOop)) {
        primitiveSuccess(resultOop);
        return PrimitiveResult::Success;
    }

    resultOop = makeLargeInteger(memory_, quotient, resultNeg);
    if (resultOop.isNil()) return PrimitiveResult::Failure;

    primitiveSuccess(resultOop);
    return PrimitiveResult::Success;
}

// Helper for bitwise operations on magnitudes (treats as unsigned)
static std::vector<uint8_t> bitwiseAnd(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    size_t minSize = std::min(a.size(), b.size());
    std::vector<uint8_t> result(minSize);
    for (size_t i = 0; i < minSize; i++) {
        result[i] = a[i] & b[i];
    }
    // Trim leading zeros
    while (result.size() > 1 && result.back() == 0) result.pop_back();
    return result;
}

static std::vector<uint8_t> bitwiseOr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    size_t maxSize = std::max(a.size(), b.size());
    std::vector<uint8_t> result(maxSize, 0);
    for (size_t i = 0; i < a.size(); i++) result[i] |= a[i];
    for (size_t i = 0; i < b.size(); i++) result[i] |= b[i];
    // Trim leading zeros
    while (result.size() > 1 && result.back() == 0) result.pop_back();
    return result;
}

static std::vector<uint8_t> bitwiseXor(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    size_t maxSize = std::max(a.size(), b.size());
    std::vector<uint8_t> result(maxSize, 0);
    for (size_t i = 0; i < a.size(); i++) result[i] = a[i];
    for (size_t i = 0; i < b.size(); i++) result[i] ^= b[i];
    // Trim leading zeros
    while (result.size() > 1 && result.back() == 0) result.pop_back();
    return result;
}

// Helper: Convert magnitude + sign to two's complement byte representation
// Positive: magnitude with a zero sign-extension byte
// Negative: ~(magnitude - 1) with 0xFF sign-extension byte
static std::vector<uint8_t> toTwosComplement(const std::vector<uint8_t>& magnitude, bool isNegative) {
    if (magnitude.size() == 1 && magnitude[0] == 0) {
        return {0};  // Zero is zero regardless of sign
    }
    if (!isNegative) {
        auto result = magnitude;
        result.push_back(0);  // sign extension
        return result;
    }
    // Negative: Two's complement of n = ~(n - 1)
    auto temp = magnitude;
    // Subtract 1
    for (size_t i = 0; i < temp.size(); i++) {
        if (temp[i] > 0) { temp[i]--; break; }
        temp[i] = 0xFF;  // borrow
    }
    // Invert all bits
    for (size_t i = 0; i < temp.size(); i++) temp[i] = ~temp[i];
    temp.push_back(0xFF);  // sign extension
    return temp;
}

// Helper: Convert two's complement bytes back to magnitude + sign
static void fromTwosComplement(const std::vector<uint8_t>& tc, std::vector<uint8_t>& magnitude, bool& isNegative) {
    if (tc.empty() || (tc.back() & 0x80) == 0) {
        // Positive
        isNegative = false;
        magnitude = tc.empty() ? std::vector<uint8_t>{0} : tc;
        while (magnitude.size() > 1 && magnitude.back() == 0) magnitude.pop_back();
        return;
    }
    // Negative: magnitude = ~tc + 1
    isNegative = true;
    magnitude = tc;
    for (size_t i = 0; i < magnitude.size(); i++) magnitude[i] = ~magnitude[i];
    uint16_t carry = 1;
    for (size_t i = 0; i < magnitude.size() && carry; i++) {
        uint16_t sum = magnitude[i] + carry;
        magnitude[i] = sum & 0xFF;
        carry = sum >> 8;
    }
    if (carry) magnitude.push_back(1);
    while (magnitude.size() > 1 && magnitude.back() == 0) magnitude.pop_back();
}

// Primitive 34: Bitwise AND (with two's complement for negative integers)
PrimitiveResult Interpreter::primitiveBitAndLargeIntegers(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    std::vector<uint8_t> aMag, bMag;
    bool aNeg, bNeg;

    if (!extractInteger(memory_, rcvr, aMag, aNeg) ||
        !extractInteger(memory_, arg, bMag, bNeg)) {
        return PrimitiveResult::Failure;
    }

    std::vector<uint8_t> resultMag;
    bool resultNeg;

    if (!aNeg && !bNeg) {
        // Fast path: both positive
        resultMag = bitwiseAnd(aMag, bMag);
        resultNeg = false;
    } else {
        // Two's complement path for negative operands
        auto aTc = toTwosComplement(aMag, aNeg);
        auto bTc = toTwosComplement(bMag, bNeg);
        size_t maxSize = std::max(aTc.size(), bTc.size());
        uint8_t aExt = aNeg ? 0xFF : 0x00;
        uint8_t bExt = bNeg ? 0xFF : 0x00;
        while (aTc.size() < maxSize) aTc.push_back(aExt);
        while (bTc.size() < maxSize) bTc.push_back(bExt);
        std::vector<uint8_t> result(maxSize);
        for (size_t i = 0; i < maxSize; i++) result[i] = aTc[i] & bTc[i];
        fromTwosComplement(result, resultMag, resultNeg);
    }

    Oop resultOop;
    if (tryConvertToSmallInteger(resultMag, resultNeg, resultOop)) {
        primitiveSuccess(resultOop);
        return PrimitiveResult::Success;
    }

    resultOop = makeLargeInteger(memory_, resultMag, resultNeg);
    if (resultOop.isNil()) return PrimitiveResult::Failure;

    primitiveSuccess(resultOop);
    return PrimitiveResult::Success;
}

// Primitive 35: Bitwise OR (with two's complement for negative integers)
PrimitiveResult Interpreter::primitiveBitOrLargeIntegers(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    std::vector<uint8_t> aMag, bMag;
    bool aNeg, bNeg;

    if (!extractInteger(memory_, rcvr, aMag, aNeg) ||
        !extractInteger(memory_, arg, bMag, bNeg)) {
        return PrimitiveResult::Failure;
    }

    std::vector<uint8_t> resultMag;
    bool resultNeg;

    if (!aNeg && !bNeg) {
        resultMag = bitwiseOr(aMag, bMag);
        resultNeg = false;
    } else {
        auto aTc = toTwosComplement(aMag, aNeg);
        auto bTc = toTwosComplement(bMag, bNeg);
        size_t maxSize = std::max(aTc.size(), bTc.size());
        uint8_t aExt = aNeg ? 0xFF : 0x00;
        uint8_t bExt = bNeg ? 0xFF : 0x00;
        while (aTc.size() < maxSize) aTc.push_back(aExt);
        while (bTc.size() < maxSize) bTc.push_back(bExt);
        std::vector<uint8_t> result(maxSize);
        for (size_t i = 0; i < maxSize; i++) result[i] = aTc[i] | bTc[i];
        fromTwosComplement(result, resultMag, resultNeg);
    }

    Oop resultOop;
    if (tryConvertToSmallInteger(resultMag, resultNeg, resultOop)) {
        primitiveSuccess(resultOop);
        return PrimitiveResult::Success;
    }

    resultOop = makeLargeInteger(memory_, resultMag, resultNeg);
    if (resultOop.isNil()) return PrimitiveResult::Failure;

    primitiveSuccess(resultOop);
    return PrimitiveResult::Success;
}

// Primitive 36: Bitwise XOR (with two's complement for negative integers)
PrimitiveResult Interpreter::primitiveBitXorLargeIntegers(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    std::vector<uint8_t> aMag, bMag;
    bool aNeg, bNeg;

    if (!extractInteger(memory_, rcvr, aMag, aNeg) ||
        !extractInteger(memory_, arg, bMag, bNeg)) {
        return PrimitiveResult::Failure;
    }

    std::vector<uint8_t> resultMag;
    bool resultNeg;

    if (!aNeg && !bNeg) {
        resultMag = bitwiseXor(aMag, bMag);
        resultNeg = false;
    } else {
        auto aTc = toTwosComplement(aMag, aNeg);
        auto bTc = toTwosComplement(bMag, bNeg);
        size_t maxSize = std::max(aTc.size(), bTc.size());
        uint8_t aExt = aNeg ? 0xFF : 0x00;
        uint8_t bExt = bNeg ? 0xFF : 0x00;
        while (aTc.size() < maxSize) aTc.push_back(aExt);
        while (bTc.size() < maxSize) bTc.push_back(bExt);
        std::vector<uint8_t> result(maxSize);
        for (size_t i = 0; i < maxSize; i++) result[i] = aTc[i] ^ bTc[i];
        fromTwosComplement(result, resultMag, resultNeg);
    }

    Oop resultOop;
    if (tryConvertToSmallInteger(resultMag, resultNeg, resultOop)) {
        primitiveSuccess(resultOop);
        return PrimitiveResult::Success;
    }

    resultOop = makeLargeInteger(memory_, resultMag, resultNeg);
    if (resultOop.isNil()) return PrimitiveResult::Failure;

    primitiveSuccess(resultOop);
    return PrimitiveResult::Success;
}

// Helper: Right-shift a magnitude vector by k bits (logical shift)
static std::vector<uint8_t> magnitudeRightShift(const std::vector<uint8_t>& mag, int64_t shift) {
    size_t byteShift = shift / 8;
    int bitShift = shift % 8;

    if (byteShift >= mag.size()) return {0};

    std::vector<uint8_t> result(mag.size() - byteShift, 0);
    for (size_t i = byteShift; i < mag.size(); i++) {
        result[i - byteShift] = mag[i];
    }
    if (bitShift > 0) {
        uint8_t carry = 0;
        for (size_t i = result.size(); i > 0; i--) {
            uint8_t byte = result[i-1];
            result[i-1] = (byte >> bitShift) | (carry << (8 - bitShift));
            carry = byte & ((1 << bitShift) - 1);
        }
    }
    while (result.size() > 1 && result.back() == 0) result.pop_back();
    return result;
}

// Helper: Left-shift a magnitude vector by k bits
static std::vector<uint8_t> magnitudeLeftShift(const std::vector<uint8_t>& mag, int64_t shift) {
    size_t byteShift = shift / 8;
    int bitShift = shift % 8;

    size_t resultSize = mag.size() + byteShift + 1;
    if (resultSize > 128 * 1024 * 1024) return {};  // 128MB max
    std::vector<uint8_t> result(resultSize, 0);

    for (size_t i = 0; i < mag.size(); i++) {
        result[i + byteShift] = mag[i];
    }
    if (bitShift > 0) {
        uint8_t carry = 0;
        for (size_t i = byteShift; i < result.size(); i++) {
            uint16_t val = (static_cast<uint16_t>(result[i]) << bitShift) | carry;
            result[i] = val & 0xFF;
            carry = val >> 8;
        }
    }
    while (result.size() > 1 && result.back() == 0) result.pop_back();
    return result;
}

// Helper: Subtract 1 from magnitude (assumes magnitude > 0)
static std::vector<uint8_t> magnitudeSubtractOne(const std::vector<uint8_t>& mag) {
    auto result = mag;
    for (size_t i = 0; i < result.size(); i++) {
        if (result[i] > 0) { result[i]--; break; }
        result[i] = 0xFF;
    }
    while (result.size() > 1 && result.back() == 0) result.pop_back();
    return result;
}

// Helper: Add 1 to magnitude
static std::vector<uint8_t> magnitudeAddOne(const std::vector<uint8_t>& mag) {
    auto result = mag;
    uint16_t carry = 1;
    for (size_t i = 0; i < result.size() && carry; i++) {
        uint16_t sum = result[i] + carry;
        result[i] = sum & 0xFF;
        carry = sum >> 8;
    }
    if (carry) result.push_back(1);
    return result;
}

// Primitive 37: Bit shift (positive = left, negative = right)
// Supports negative receivers via arithmetic shift semantics
PrimitiveResult Interpreter::primitiveBitShiftLargeIntegers(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    if (!arg.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }
    int64_t shift = arg.asSmallInteger();

    std::vector<uint8_t> aMag;
    bool aNeg;

    if (!extractInteger(memory_, rcvr, aMag, aNeg)) {
        return PrimitiveResult::Failure;
    }

    std::vector<uint8_t> resultMag;
    bool resultNeg;

    if (!aNeg) {
        // Positive receiver: simple magnitude shift
        if (shift >= 0) {
            resultMag = magnitudeLeftShift(aMag, shift);
            if (resultMag.empty()) return PrimitiveResult::Failure;  // overflow
        } else {
            resultMag = magnitudeRightShift(aMag, -shift);
        }
        resultNeg = false;
    } else {
        // Negative receiver: arithmetic shift semantics
        // Left shift: (-n) << k = -(n << k)
        // Right shift: (-n) >> k = -((n-1) >> k + 1)
        if (shift >= 0) {
            resultMag = magnitudeLeftShift(aMag, shift);
            if (resultMag.empty()) return PrimitiveResult::Failure;
            resultNeg = true;
        } else {
            // Arithmetic right shift of negative
            auto m = magnitudeSubtractOne(aMag);
            m = magnitudeRightShift(m, -shift);
            resultMag = magnitudeAddOne(m);
            resultNeg = true;
        }
        // Check for -0 → 0
        if (resultMag.size() == 1 && resultMag[0] == 0) resultNeg = false;
    }

    Oop resultOop;
    if (tryConvertToSmallInteger(resultMag, resultNeg, resultOop)) {
        primitiveSuccess(resultOop);
        return PrimitiveResult::Success;
    }

    resultOop = makeLargeInteger(memory_, resultMag, resultNeg);
    if (resultOop.isNil()) return PrimitiveResult::Failure;

    primitiveSuccess(resultOop);
    return PrimitiveResult::Success;
}

// ===== LARGE INTEGERS PLUGIN NAMED PRIMITIVES =====

// primDigitMultiplyNegative: receiver * arg, with explicit neg flag
// receiver: Integer, args: (Integer, Boolean)
PrimitiveResult Interpreter::primDigitMultiplyNegative(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop negFlag = stackValue(0);  // Boolean
    Oop arg = stackValue(1);      // Integer
    Oop rcvr = stackValue(2);     // receiver Integer

    std::vector<uint8_t> aMag, bMag;
    bool aNeg, bNeg;

    if (!extractInteger(memory_, rcvr, aMag, aNeg) ||
        !extractInteger(memory_, arg, bMag, bNeg)) {
        return PrimitiveResult::Failure;
    }

    std::vector<uint8_t> resultMag = multiplyMagnitudes(aMag, bMag);
    bool resultNeg = (negFlag.rawBits() == memory_.trueObject().rawBits());

    if (resultMag.size() == 1 && resultMag[0] == 0) resultNeg = false;

    Oop result = makeLargeInteger(memory_, resultMag, resultNeg);
    if (result.isNil()) return PrimitiveResult::Failure;

    popN(3);
    push(result);
    return PrimitiveResult::Success;
}

// primDigitAdd: receiver digitAdd: arg
// receiver: Integer, args: (Integer)
PrimitiveResult Interpreter::primDigitAddLargeIntegers(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    std::vector<uint8_t> aMag, bMag;
    bool aNeg, bNeg;

    if (!extractInteger(memory_, rcvr, aMag, aNeg) ||
        !extractInteger(memory_, arg, bMag, bNeg)) {
        return PrimitiveResult::Failure;
    }

    // Both should have same sign (caller ensures this)
    std::vector<uint8_t> resultMag = addMagnitudes(aMag, bMag);
    Oop result = makeLargeInteger(memory_, resultMag, aNeg);
    if (result.isNil()) return PrimitiveResult::Failure;

    popN(2);
    push(result);
    return PrimitiveResult::Success;
}

// primNormalizePositive: strip leading zeros, convert to SmallInt if fits
// receiver: LargePositiveInteger, no args
PrimitiveResult Interpreter::primNormalizePositive(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop rcvr = stackTop();

    std::vector<uint8_t> mag;
    bool isNeg;
    if (!extractInteger(memory_, rcvr, mag, isNeg)) {
        return PrimitiveResult::Failure;
    }

    // Try to convert to SmallInteger
    Oop result = makeLargeInteger(memory_, mag, false);
    if (result.isNil()) return PrimitiveResult::Failure;

    pop();
    push(result);
    return PrimitiveResult::Success;
}

// primNormalizeNegative: strip leading zeros, convert to SmallInt if fits
// receiver: LargeNegativeInteger, no args
PrimitiveResult Interpreter::primNormalizeNegative(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop rcvr = stackTop();

    std::vector<uint8_t> mag;
    bool isNeg;
    if (!extractInteger(memory_, rcvr, mag, isNeg)) {
        return PrimitiveResult::Failure;
    }

    // Try to convert to SmallInteger (negative)
    Oop result = makeLargeInteger(memory_, mag, true);
    if (result.isNil()) return PrimitiveResult::Failure;

    pop();
    push(result);
    return PrimitiveResult::Success;
}

// primDigitDivNegative: receiver digitDiv: arg neg: ng
// Returns Array of (quotient, remainder)
PrimitiveResult Interpreter::primDigitDivNegative(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop negFlag = stackValue(0);  // Boolean (neg flag for quotient)
    Oop arg = stackValue(1);      // divisor Integer
    Oop rcvr = stackValue(2);     // dividend Integer (self)

    std::vector<uint8_t> dividendMag, divisorMag;
    bool dividendNeg, divisorNeg;

    if (!extractInteger(memory_, rcvr, dividendMag, dividendNeg) ||
        !extractInteger(memory_, arg, divisorMag, divisorNeg)) {
        return PrimitiveResult::Failure;
    }

    // Check for division by zero
    if (divisorMag.size() == 1 && divisorMag[0] == 0) {
        return PrimitiveResult::Failure;
    }

    // Use general-purpose divideMagnitudes (no size limit)
    std::vector<uint8_t> quotientMag, remainderMag;
    divideMagnitudes(dividendMag, divisorMag, quotientMag, remainderMag);

    // Determine quotient sign from the neg flag
    bool quotientNeg = (negFlag.rawBits() == memory_.trueObject().rawBits());
    // Don't negate zero
    if (quotientMag.size() == 1 && quotientMag[0] == 0) quotientNeg = false;

    // GC safety: each makeLargeInteger/allocateSlots may trigger GC, invalidating
    // previously allocated Oops on the C++ stack. Push intermediates onto the
    // Smalltalk stack so GC can update them.

    Oop quotient = makeLargeInteger(memory_, quotientMag, quotientNeg);
    if (quotient.isNil()) return PrimitiveResult::Failure;
    // Push quotient onto stack so GC can find it
    push(quotient);

    // Remainder keeps the sign of the dividend (per Smalltalk semantics of digitDiv)
    bool remainderNeg = dividendNeg && !(remainderMag.size() == 1 && remainderMag[0] == 0);
    Oop remainder = makeLargeInteger(memory_, remainderMag, remainderNeg);
    if (remainder.isNil()) { pop(); return PrimitiveResult::Failure; }
    // Push remainder onto stack so GC can find it
    push(remainder);

    // Allocate a 2-element Array
    Oop arrayClass = memory_.specialObject(SpecialObjectIndex::ClassArray);
    uint32_t arrayClassIdx = memory_.indexOfClass(arrayClass);
    if (arrayClassIdx == 0) arrayClassIdx = memory_.registerClass(arrayClass);
    Oop resultArray = memory_.allocateSlots(arrayClassIdx, 2, ObjectFormat::Indexable);
    if (resultArray.isNil()) { popN(2); return PrimitiveResult::Failure; }

    // Retrieve GC-safe values from stack
    remainder = stackValue(0);
    quotient = stackValue(1);
    popN(2);  // pop the temp quotient and remainder

    memory_.storePointer(0, resultArray, quotient);
    memory_.storePointer(1, resultArray, remainder);

    popN(3);  // pop receiver + 2 args
    push(resultArray);
    return PrimitiveResult::Success;
}

// primDigitSubtract: receiver digitSubtract: arg
// Used by Integer>>- for same-sign subtraction
PrimitiveResult Interpreter::primDigitSubtractLargeIntegers(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    std::vector<uint8_t> aMag, bMag;
    bool aNeg, bNeg;

    if (!extractInteger(memory_, rcvr, aMag, aNeg) ||
        !extractInteger(memory_, arg, bMag, bNeg)) {
        return PrimitiveResult::Failure;
    }

    // Determine result based on magnitude comparison
    std::vector<uint8_t> resultMag;
    bool resultNeg;
    int cmp = compareMagnitudes(aMag, bMag);
    if (cmp >= 0) {
        resultMag = subtractMagnitudes(aMag, bMag);
        resultNeg = aNeg;
    } else {
        resultMag = subtractMagnitudes(bMag, aMag);
        resultNeg = !aNeg;  // Flip sign when |b| > |a|
    }

    Oop result = makeLargeInteger(memory_, resultMag, resultNeg);
    if (result.isNil()) return PrimitiveResult::Failure;

    popN(2);
    push(result);
    return PrimitiveResult::Success;
}

// primDigitCompare: compare magnitudes of two integers
// Returns SmallInteger: -1 if receiver < arg, 0 if equal, 1 if receiver > arg
PrimitiveResult Interpreter::primDigitCompare(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    std::vector<uint8_t> aMag, bMag;
    bool aNeg, bNeg;

    if (!extractInteger(memory_, rcvr, aMag, aNeg) ||
        !extractInteger(memory_, arg, bMag, bNeg)) {
        return PrimitiveResult::Failure;
    }

    int cmp = compareMagnitudes(aMag, bMag);

    popN(2);
    push(Oop::fromSmallInteger(cmp));
    return PrimitiveResult::Success;
}

// ===== GC PRIMITIVES =====

PrimitiveResult Interpreter::primitiveFullGC(int argCount) {
    // Primitive 130: Perform a full garbage collection
    // Returns the number of bytes of free space after collection

    // Trigger a full garbage collection
    memory_.fullGC();
    flushMethodCache();  // Compaction moves objects — stale cache entries cause DNU

    // Get free space after GC
    size_t freeBytes = memory_.freeOldSpaceBytes();

    // Push result before signaling finalization.
    if (Oop::canBeSmallInteger(static_cast<int64_t>(freeBytes))) {
        primitiveSuccess(Oop::fromSmallInteger(static_cast<int64_t>(freeBytes)));
    } else {
        primitiveSuccess(Oop::fromSmallInteger(Oop::smallIntegerMax()));
    }

    signalFinalizationIfNeeded();

    return PrimitiveResult::Success;
}

// ===== UTILITY PRIMITIVES =====

// Primitive 89: Flush the method cache
PrimitiveResult Interpreter::primitiveFlushCache(int argCount) {
    // Clear the method cache to force re-lookups
    // This is called after methods are modified or removed
    flushMethodCache();
    primitiveSuccess(stackTop());  // Return receiver
    return PrimitiveResult::Success;
}

// Primitive 112: Return available memory bytes
PrimitiveResult Interpreter::primitiveBytesLeft(int argCount) {
    size_t freeBytes = memory_.freeOldSpaceBytes();

    if (Oop::canBeSmallInteger(static_cast<int64_t>(freeBytes))) {
        pop();
        push(Oop::fromSmallInteger(static_cast<int64_t>(freeBytes)));
    } else {
        pop();
        push(Oop::fromSmallInteger(Oop::smallIntegerMax()));
    }

    return PrimitiveResult::Success;
}

// Primitive 129: Return the special objects array
PrimitiveResult Interpreter::primitiveSpecialObjectsOop(int argCount) {
    pop();
    push(memory_.specialObjectsArray());
    return PrimitiveResult::Success;
}

// ===== PERMANENT SPACE PRIMITIVES (90-93) =====
// Stubs for Spur memory manager permanent space operations.
// Since we don't implement permanent space, these are no-ops that return
// sensible values to allow image code to continue working.

// Primitive 90: Move object to permanent space (no-op, return receiver)
PrimitiveResult Interpreter::primitiveMoveToPermSpace(int argCount) {
    // No permanent space implementation - just return receiver unchanged
    (void)argCount;
    // Stack: receiver -> receiver (no change needed)
    return PrimitiveResult::Success;
}

// Primitive 91: Move objects to permanent space in bulk (no-op, return receiver)
PrimitiveResult Interpreter::primitiveMoveToPermSpaceInBulk(int argCount) {
    // No permanent space implementation - just return receiver unchanged
    (void)argCount;
    // Stack: receiver -> receiver (no change needed)
    return PrimitiveResult::Success;
}

// Primitive 92: Check if object is in permanent space (always false)
PrimitiveResult Interpreter::primitiveIsInPermSpace(int argCount) {
    // No permanent space implementation - nothing is in perm space
    (void)argCount;
    pop();
    push(memory_.falseObject());
    return PrimitiveResult::Success;
}

// Primitive 93: Move all old objects to permanent space (no-op)
PrimitiveResult Interpreter::primitiveMoveToPermSpaceAllOldObjects(int argCount) {
    // No permanent space implementation - just return success
    (void)argCount;
    // Returns number moved (0)
    pop();
    push(Oop::fromSmallInteger(0));
    return PrimitiveResult::Success;
}

// ===== OBJECT ENUMERATION PRIMITIVES =====

// Primitive 77: Return the first instance of a class
PrimitiveResult Interpreter::primitiveSomeInstance(int argCount) {
    Oop classOop = stackTop();

    if (!classOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Get the class index we're looking for
    uint32_t targetClassIndex = memory_.indexOfClass(classOop);

    // Iterate through all objects in old space to find an instance
    Oop instance = memory_.firstInstanceOf(targetClassIndex);

    if (instance.isNil()) {
        return PrimitiveResult::Failure;  // No instances found
    }

    pop();
    push(instance);
    return PrimitiveResult::Success;
}

// Primitive 78: Return the next instance after this object
PrimitiveResult Interpreter::primitiveNextInstance(int argCount) {
    Oop object = stackTop();

    if (!object.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Get the class of this object
    ObjectHeader* header = object.asObjectPtr();
    uint32_t targetClassIndex = header->classIndex();

    // Find the next instance of the same class after this object
    Oop nextInstance = memory_.nextInstanceAfter(object, targetClassIndex);

    if (nextInstance.isNil()) {
        return PrimitiveResult::Failure;  // No more instances
    }

    pop();
    push(nextInstance);
    return PrimitiveResult::Success;
}

// ===== ARRAY/MEMORY PRIMITIVES =====

// Primitive 145: Fill array with a constant value
PrimitiveResult Interpreter::primitiveConstantFill(int argCount) {
    Oop value = stackValue(0);
    Oop rcvr = stackValue(1);

    if (!rcvr.isObject()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* header = rcvr.asObjectPtr();
    ObjectFormat format = header->format();
    size_t size = memory_.byteSizeOf(rcvr);

    // Handle different formats
    if (format >= ObjectFormat::Indexable8 && format <= ObjectFormat::Indexable8_7) {
        // Byte array - value must be SmallInteger 0-255
        if (!value.isSmallInteger()) {
            return PrimitiveResult::Failure;
        }
        int64_t byteVal = value.asSmallInteger();
        if (byteVal < 0 || byteVal > 255) {
            return PrimitiveResult::Failure;
        }

        uint8_t* bytes = reinterpret_cast<uint8_t*>(header + 1);
        std::memset(bytes, static_cast<uint8_t>(byteVal), size);
    } else if (format >= ObjectFormat::Indexable32 && format <= ObjectFormat::Indexable32Odd) {
        // 32-bit word array — value must be unsigned 0..0xFFFFFFFF
        if (!value.isSmallInteger()) {
            return PrimitiveResult::Failure;
        }
        int64_t wordVal = value.asSmallInteger();
        if (wordVal < 0 || wordVal > 0xFFFFFFFF) {
            return PrimitiveResult::Failure;
        }

        uint32_t* words = reinterpret_cast<uint32_t*>(header + 1);
        size_t wordCount = size / 4;
        for (size_t i = 0; i < wordCount; i++) {
            words[i] = static_cast<uint32_t>(wordVal);
        }
    } else if (format >= ObjectFormat::Indexable16 && format <= ObjectFormat::Indexable16_3) {
        // 16-bit word array — value must be unsigned 0..0xFFFF
        if (!value.isSmallInteger()) {
            return PrimitiveResult::Failure;
        }
        int64_t wordVal = value.asSmallInteger();
        if (wordVal < 0 || wordVal > 0xFFFF) {
            return PrimitiveResult::Failure;
        }

        uint16_t* words = reinterpret_cast<uint16_t*>(header + 1);
        size_t wordCount = size / 2;
        for (size_t i = 0; i < wordCount; i++) {
            words[i] = static_cast<uint16_t>(wordVal);
        }
    } else if (format == ObjectFormat::Indexable64) {
        // 64-bit word array
        uint64_t word;
        if (value.isSmallInteger()) {
            int64_t quadVal = value.asSmallInteger();
            if (quadVal < 0) return PrimitiveResult::Failure;
            word = static_cast<uint64_t>(quadVal);
        } else {
            bool isNeg = false;
            if (!isLargeInteger(memory_, value, isNeg) || isNeg) {
                return PrimitiveResult::Failure;
            }
            std::vector<uint8_t> mag = extractMagnitude(memory_, value);
            if (mag.size() > 8) return PrimitiveResult::Failure;
            word = 0;
            for (size_t i = 0; i < mag.size(); i++) {
                word |= static_cast<uint64_t>(mag[i]) << (i * 8);
            }
        }

        uint64_t* quads = reinterpret_cast<uint64_t*>(header + 1);
        size_t quadCount = size / 8;
        for (size_t i = 0; i < quadCount; i++) {
            quads[i] = word;
        }
    } else if (format <= ObjectFormat::IndexableWithFixed) {
        // Pointer array - fill slots with the value
        size_t slotCount = header->slotCount();
        for (size_t i = 0; i < slotCount; i++) {
            memory_.storePointer(i, rcvr, value);
        }
    } else {
        return PrimitiveResult::Failure;
    }

    popN(2);
    push(rcvr);  // Return receiver
    return PrimitiveResult::Success;
}

// Primitive 156: Compare two byte arrays
PrimitiveResult Interpreter::primitiveCompareBytes(int argCount) {
    // Primitive 156: Compare two byte/word indexed objects for EQUALITY
    // Per official VM: returns boolean (true if equal, false if not equal)
    // This is an equality check, NOT an ordering comparison
    if (argCount < 1) {
        return PrimitiveResult::Failure;
    }

    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    if (!rcvr.isObject() || !arg.isObject()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* rcvrHeader = rcvr.asObjectPtr();
    ObjectHeader* argHeader = arg.asObjectPtr();

    ObjectFormat rcvrFormat = rcvrHeader->format();
    ObjectFormat argFormat = argHeader->format();

    // Both must be byte-indexable or word-indexable
    bool rcvrIsBytes = (rcvrFormat >= ObjectFormat::Indexable8 && rcvrFormat <= ObjectFormat::Indexable8_7);
    bool argIsBytes = (argFormat >= ObjectFormat::Indexable8 && argFormat <= ObjectFormat::Indexable8_7);
    bool rcvrIsWords = (rcvrFormat == ObjectFormat::Indexable64 ||
                       (rcvrFormat >= ObjectFormat::Indexable32 && rcvrFormat <= ObjectFormat::Indexable32Odd));
    bool argIsWords = (argFormat == ObjectFormat::Indexable64 ||
                      (argFormat >= ObjectFormat::Indexable32 && argFormat <= ObjectFormat::Indexable32Odd));

    if (!(rcvrIsBytes || rcvrIsWords) || !(argIsBytes || argIsWords)) {
        return PrimitiveResult::Failure;
    }

    size_t rcvrSize = memory_.byteSizeOf(rcvr);
    size_t argSize = memory_.byteSizeOf(arg);

    // Different sizes means not equal
    if (rcvrSize != argSize) {
        popN(2);
        push(memory_.falseObject());
        return PrimitiveResult::Success;
    }

    uint8_t* rcvrBytes = reinterpret_cast<uint8_t*>(rcvrHeader + 1);
    uint8_t* argBytes = reinterpret_cast<uint8_t*>(argHeader + 1);

    // Compare bytes - official VM returns boolean for equality
    int result = std::memcmp(rcvrBytes, argBytes, rcvrSize);

    popN(2);
    push(result == 0 ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

// Primitive 159: Hash multiply for collections
PrimitiveResult Interpreter::primitiveHashMultiply(int argCount) {
    // Primitive 159: Hash multiply for SmallInteger or LargePositiveInteger
    // hashMultiply is: (value * 1664525) bitAnd: 16rFFFFFFF
    Oop rcvr = stackTop();

    const int64_t HashMultiplier = 1664525;
    const int64_t HashMask = 0x0FFFFFFF;  // 28 bits
    uint32_t value;

    if (rcvr.isSmallInteger()) {
        // SmallInteger case
        value = static_cast<uint32_t>(rcvr.asSmallInteger() & 0xFFFFFFFF);
    } else if (rcvr.isObject()) {
        // Check if LargePositiveInteger
        Oop largePositiveClass = memory_.specialObject(SpecialObjectIndex::ClassLargePositiveInteger);
        if (memory_.classOf(rcvr) != largePositiveClass) {
            return PrimitiveResult::Failure;  // Not a LargePositiveInteger
        }
        // Read first 32 bits of magnitude (little-endian)
        ObjectHeader* hdr = rcvr.asObjectPtr();
        size_t byteSize = hdr->byteSize();
        if (byteSize < 4) {
            // Small LargeInteger - read available bytes
            value = 0;
            const uint8_t* bytes = hdr->bytes();
            for (size_t i = 0; i < byteSize; i++) {
                value |= (static_cast<uint32_t>(bytes[i]) << (i * 8));
            }
        } else {
            // Read first 4 bytes (little-endian)
            const uint8_t* bytes = hdr->bytes();
            value = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
        }
    } else {
        return PrimitiveResult::Failure;
    }

    int64_t result = (static_cast<int64_t>(value) * HashMultiplier) & HashMask;

    pop();
    push(Oop::fromSmallInteger(result));
    return PrimitiveResult::Success;
}

// ===== PROCESS PRIMITIVES =====

// Primitive 167: Yield — put active process at back of its queue, then run
// the highest-priority ready process.  The Cog VM's primitiveYield does a
// full reschedule (addLastLink:toList: then transferTo: wakeHighestPriority)
// so that higher-priority processes that became ready (e.g. Delay scheduler
// at priority 80) get CPU even when the yielding process is at a lower
// priority.  Our previous implementation only checked same-priority peers,
// which starved higher-priority processes and broke Delay-based timeouts.
PrimitiveResult Interpreter::primitiveYield(int argCount) {
    // Get the active process
    Oop activeProcess = getActiveProcess();
    if (activeProcess.isNil()) {
        return PrimitiveResult::Failure;
    }

    // Get the process priority
    Oop priorityOop = memory_.fetchPointer(ProcessPriorityIndex, activeProcess);
    if (!priorityOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }
    int64_t priority = priorityOop.asSmallInteger();

    // Get the scheduler and its process lists
    Oop scheduler = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);
    if (scheduler.isNil()) {
        return PrimitiveResult::Failure;
    }
    Oop schedulerValue = memory_.fetchPointer(1, scheduler);  // Association value
    Oop processLists = memory_.fetchPointer(SchedulerProcessListsIndex, schedulerValue);

    // Get the list for this priority (1-based index)
    if (priority < 1) {
        return PrimitiveResult::Failure;
    }
    Oop priorityList = memory_.fetchPointer(priority - 1, processLists);

    // CRITICAL: Complete the primitive's stack effect BEFORE saving context.
    // Pop receiver (+ args), push result. For yield, result is self (Processor).
    // This ensures the saved context has the correct post-primitive stack state,
    // so when this process is later resumed, it continues with the result on stack.
    Oop receiver = stackValue(argCount);  // The Processor
    primitiveSuccess(receiver);

    // ALWAYS check the timer before scheduling decisions.
    // This is critical: if this process is in a yield loop and the Delay
    // scheduler is waiting on its timer semaphore, checkTimerSemaphore()
    // may signal the scheduler, putting it in the ready queue at priority 80.
    // Without this, a yield loop can starve the Delay scheduler forever.
    checkTimerSemaphore();

    // Put current process at the back of its priority queue
    addLastLinkToList(activeProcess, priorityList);

    // Wake the highest-priority ready process (may be at a different priority)
    Oop nextProcess = wakeHighestPriority();

    if (nextProcess.isNil() || nextProcess.rawBits() == activeProcess.rawBits()) {
        // No other process to run — remove ourselves from the queue and continue
        removeFirstLinkOfList(priorityList);
        return PrimitiveResult::Success;
    }

    g_xferReason = "primYield";
    transferTo(nextProcess);
    return PrimitiveResult::Success;
}

// ===== CONTEXT PRIMITIVES =====

// Primitive 199: Exception handler marker (NOT thisContext!)
// This primitive is used by BlockClosure>>on:do: as a marker for exception handling.
// It should ALWAYS fail so the method falls through to 'self value' which evaluates the block.
// The actual thisContext primitive is 185.
PrimitiveResult Interpreter::primitiveExceptionMarker(int argCount) {
    // Always fail - this is just a marker primitive
    // The Smalltalk code falls through to 'self value' after this fails
    return PrimitiveResult::Failure;
}

// Primitive 206: Return the number of arguments a closure expects
PrimitiveResult Interpreter::primitiveClosureNumArgs(int argCount) {
    Oop rcvr = stackTop();

    if (!rcvr.isObject()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* header = rcvr.asObjectPtr();
    ObjectFormat format = header->format();

    // Block closures (CompiledBlocks) have format 24-31
    if (format < ObjectFormat::CompiledMethod) {
        return PrimitiveResult::Failure;
    }

    // For BlockClosure, numArgs is stored in slot 3
    // BlockClosure layout: outerContext, startpc, numArgs, ...
    // Actually in Pharo, BlockClosure has: outerContext, compiledBlock, numArgs, receiver
    // Let's get numArgs from the closure's numArgs slot (index 2)

    // Check if this looks like a BlockClosure
    size_t slotCount = header->slotCount();
    if (slotCount < 3) {
        return PrimitiveResult::Failure;
    }

    // Slot 2 is numArgs for BlockClosure
    Oop numArgsOop = memory_.fetchPointer(2, rcvr);
    if (!numArgsOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    pop();
    push(numArgsOop);
    return PrimitiveResult::Success;
}

// ===== SLOT ACCESS PRIMITIVES =====

// Primitive 173: Read slot at given index (1-based)
PrimitiveResult Interpreter::primitiveSlotAt(int argCount) {
    // Primitive 173: Low-level slot access
    // Per official VM: handles byte/word/pointer objects differently
    // Index is 1-BASED from Smalltalk side
    Oop indexOop = stackValue(0);
    Oop rcvr = stackValue(1);

    if (!rcvr.isObject() || !indexOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    rcvr = memory_.followForwarded(rcvr);

    int64_t index = indexOop.asSmallInteger();
    if (index < 1) {  // 1-based indexing from Smalltalk
        return PrimitiveResult::Failure;
    }

    ObjectHeader* header = rcvr.asObjectPtr();
    ObjectFormat fmt = header->format();

    // Per official VM: fail for CompiledMethods
    if (header->isCompiledMethod()) {
        return PrimitiveResult::Failure;
    }

    // Convert to 0-based for C++ array access
    size_t zeroIndex = static_cast<size_t>(index - 1);

    // Handle byte objects
    if (fmt >= ObjectFormat::Indexable8 && fmt <= ObjectFormat::Indexable8_7) {
        size_t byteCount = header->byteSize();
        if (zeroIndex >= byteCount) {
            return PrimitiveResult::Failure;
        }
        uint8_t byte = header->bytes()[zeroIndex];
        popN(2);
        push(Oop::fromSmallInteger(byte));
        return PrimitiveResult::Success;
    }

    // Handle 16-bit objects
    if (fmt >= ObjectFormat::Indexable16 && fmt <= ObjectFormat::Indexable16_3) {
        size_t slots = header->slotCount();
        size_t count = slots * 4 - (static_cast<int>(fmt) - static_cast<int>(ObjectFormat::Indexable16));
        if (zeroIndex >= count) {
            return PrimitiveResult::Failure;
        }
        uint16_t* words = reinterpret_cast<uint16_t*>(header + 1);
        popN(2);
        push(Oop::fromSmallInteger(words[zeroIndex]));
        return PrimitiveResult::Success;
    }

    // Handle 32-bit objects
    if (fmt >= ObjectFormat::Indexable32 && fmt <= ObjectFormat::Indexable32Odd) {
        size_t slots = header->slotCount();
        size_t count = slots * 2 - (fmt == ObjectFormat::Indexable32Odd ? 1 : 0);
        if (zeroIndex >= count) {
            return PrimitiveResult::Failure;
        }
        uint32_t* words = reinterpret_cast<uint32_t*>(header + 1);
        popN(2);
        push(Oop::fromSmallInteger(words[zeroIndex]));
        return PrimitiveResult::Success;
    }

    // Handle 64-bit word objects (DoubleWordArray)
    if (fmt == ObjectFormat::Indexable64) {
        size_t slotCount = header->slotCount();
        if (zeroIndex >= slotCount) {
            return PrimitiveResult::Failure;
        }
        uint64_t* words = reinterpret_cast<uint64_t*>(header + 1);
        uint64_t val = words[zeroIndex];
        if (val <= static_cast<uint64_t>(Oop::smallIntegerMax())) {
            popN(2);
            push(Oop::fromSmallInteger(static_cast<int64_t>(val)));
        } else {
            // Create LargePositiveInteger from 64-bit value
            std::vector<uint8_t> mag;
            uint64_t tmp = val;
            while (tmp > 0) {
                mag.push_back(static_cast<uint8_t>(tmp & 0xFF));
                tmp >>= 8;
            }
            if (mag.empty()) mag.push_back(0);
            Oop result = makeLargeInteger(memory_, mag, false);
            if (result.isNil()) return PrimitiveResult::Failure;
            popN(2);
            push(result);
        }
        return PrimitiveResult::Success;
    }

    // Pointer objects
    size_t slotCount = header->slotCount();
    if (zeroIndex >= slotCount) {
        return PrimitiveResult::Failure;
    }

    Oop value = memory_.fetchPointer(zeroIndex, rcvr);
    popN(2);
    push(value);
    return PrimitiveResult::Success;
}

// Primitive 174: Write slot at given index (0-based)
// Per official VM: handles byte/word/pointer objects differently
PrimitiveResult Interpreter::primitiveSlotAtPut(int argCount) {
    // Primitive 174: Low-level slot write
    // Index is 1-BASED from Smalltalk side
    Oop value = stackValue(0);
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!rcvr.isObject() || !indexOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    rcvr = memory_.followForwarded(rcvr);

    int64_t index = indexOop.asSmallInteger();
    if (index < 1) {  // 1-based indexing from Smalltalk
        return PrimitiveResult::Failure;
    }

    ObjectHeader* header = rcvr.asObjectPtr();
    ObjectFormat fmt = header->format();

    // Check immutability
    if (header->isImmutable()) {
        return PrimitiveResult::Failure;
    }

    // Per official VM: fail for CompiledMethods
    if (header->isCompiledMethod()) {
        return PrimitiveResult::Failure;
    }

    // Convert to 0-based for C++ array access
    size_t zeroIndex = static_cast<size_t>(index - 1);

    // Handle byte objects
    if (fmt >= ObjectFormat::Indexable8 && fmt <= ObjectFormat::Indexable8_7) {
        if (!value.isSmallInteger()) {
            return PrimitiveResult::Failure;
        }
        int64_t byteVal = value.asSmallInteger();
        if (byteVal < 0 || byteVal > 255) {
            return PrimitiveResult::Failure;
        }
        size_t byteCount = header->byteSize();
        if (zeroIndex >= byteCount) {
            return PrimitiveResult::Failure;
        }
        header->bytes()[zeroIndex] = static_cast<uint8_t>(byteVal);
        popN(3);
        push(value);
        return PrimitiveResult::Success;
    }

    // Handle 16-bit objects
    if (fmt >= ObjectFormat::Indexable16 && fmt <= ObjectFormat::Indexable16_3) {
        if (!value.isSmallInteger()) {
            return PrimitiveResult::Failure;
        }
        int64_t wordVal = value.asSmallInteger();
        if (wordVal < 0 || wordVal > 0xFFFF) {
            return PrimitiveResult::Failure;
        }
        size_t slots = header->slotCount();
        size_t count = slots * 4 - (static_cast<int>(fmt) - static_cast<int>(ObjectFormat::Indexable16));
        if (zeroIndex >= count) {
            return PrimitiveResult::Failure;
        }
        uint16_t* words = reinterpret_cast<uint16_t*>(header + 1);
        words[zeroIndex] = static_cast<uint16_t>(wordVal);
        popN(3);
        push(value);
        return PrimitiveResult::Success;
    }

    // Handle 32-bit objects
    if (fmt >= ObjectFormat::Indexable32 && fmt <= ObjectFormat::Indexable32Odd) {
        if (!value.isSmallInteger()) {
            return PrimitiveResult::Failure;
        }
        int64_t wordVal = value.asSmallInteger();
        if (wordVal < 0 || wordVal > 0xFFFFFFFF) {
            return PrimitiveResult::Failure;
        }
        size_t slots = header->slotCount();
        size_t count = slots * 2 - (fmt == ObjectFormat::Indexable32Odd ? 1 : 0);
        if (zeroIndex >= count) {
            return PrimitiveResult::Failure;
        }
        uint32_t* words = reinterpret_cast<uint32_t*>(header + 1);
        words[zeroIndex] = static_cast<uint32_t>(wordVal);
        popN(3);
        push(value);
        return PrimitiveResult::Success;
    }

    // Handle 64-bit word objects (DoubleWordArray)
    if (fmt == ObjectFormat::Indexable64) {
        size_t slotCount = header->slotCount();
        if (zeroIndex >= slotCount) {
            return PrimitiveResult::Failure;
        }
        uint64_t word;
        if (value.isSmallInteger()) {
            int64_t wordVal = value.asSmallInteger();
            if (wordVal < 0) {
                return PrimitiveResult::Failure;
            }
            word = static_cast<uint64_t>(wordVal);
        } else {
            bool isNeg = false;
            if (!isLargeInteger(memory_, value, isNeg) || isNeg) {
                return PrimitiveResult::Failure;
            }
            std::vector<uint8_t> mag = extractMagnitude(memory_, value);
            if (mag.size() > 8) {
                return PrimitiveResult::Failure;
            }
            word = 0;
            for (size_t i = 0; i < mag.size(); i++) {
                word |= static_cast<uint64_t>(mag[i]) << (i * 8);
            }
        }
        uint64_t* words = reinterpret_cast<uint64_t*>(header + 1);
        words[zeroIndex] = word;
        popN(3);
        push(value);
        return PrimitiveResult::Success;
    }

    // Pointer objects
    size_t slotCount = header->slotCount();
    if (zeroIndex >= slotCount) {
        return PrimitiveResult::Failure;
    }

    memory_.storePointer(zeroIndex, rcvr, value);
    popN(3);
    push(value);
    return PrimitiveResult::Success;
}

// ===== OBJECT ENUMERATION PRIMITIVES =====

// Primitive 177: Return all instances of a class
PrimitiveResult Interpreter::primitiveAllInstances(int argCount) {
    Oop classOop = stackTop();

    if (!classOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    uint32_t targetClassIndex = memory_.indexOfClass(classOop);

    // Collect instances using allObjectsDo
    std::vector<Oop> instances;
    memory_.allObjectsDo([&](Oop obj) {
        if (obj.isObject()) {
            ObjectHeader* header = obj.asObjectPtr();
            if (header->classIndex() == targetClassIndex) {
                instances.push_back(obj);
            }
        }
    });

    // Allocate an array to hold the instances
    Oop arrayClass = memory_.specialObject(SpecialObjectIndex::ClassArray);
    uint32_t arrayClassIndex = memory_.indexOfClass(arrayClass);
    Oop result = memory_.allocateSlots(arrayClassIndex, instances.size(), ObjectFormat::Indexable);

    if (result.isNil()) {
        return PrimitiveResult::Failure;
    }

    // Fill the array
    for (size_t i = 0; i < instances.size(); i++) {
        memory_.storePointer(i, result, instances[i]);
    }

    pop();
    push(result);
    return PrimitiveResult::Success;
}

// Primitive 178: Return all objects in the system
PrimitiveResult Interpreter::primitiveAllObjects(int argCount) {
    // Collect all visible objects using allObjectsDo
    // Skip classIdx=0 objects — these are hidden VM objects (free chunks,
    // class table pages) that should never be visible to Smalltalk code.
    std::vector<Oop> objects;
    memory_.allObjectsDo([&](Oop obj) {
        if (obj.isObject()) {
            ObjectHeader* hdr = obj.asObjectPtr();
            uint32_t cls = hdr->classIndex();
            if (cls != 0) {
                // Also verify the class table has a valid entry
                Oop classOop = memory_.classAtIndex(cls);
                if (classOop.isObject() && !classOop.isNil()) {
                    objects.push_back(obj);
                }
            }
        }
    });

    // Allocate an array to hold all objects
    Oop arrayClass = memory_.specialObject(SpecialObjectIndex::ClassArray);
    uint32_t arrayClassIndex = memory_.indexOfClass(arrayClass);
    Oop result = memory_.allocateSlots(arrayClassIndex, objects.size(), ObjectFormat::Indexable);

    if (result.isNil()) {
        return PrimitiveResult::Failure;
    }

    // Fill the array
    for (size_t i = 0; i < objects.size(); i++) {
        memory_.storePointer(i, result, objects[i]);
    }

    pop();  // Pop receiver
    push(result);
    return PrimitiveResult::Success;
}

// ===== OBJECT REFERENCE PRIMITIVES =====

// Primitive 132: Does object point to another object?
// Per official VM: only checks pointer fields, handles compiled methods specially
PrimitiveResult Interpreter::primitiveObjectPointsTo(int argCount) {
    Oop target = stackValue(0);
    Oop rcvr = stackValue(1);

    // Follow forwarding pointers (created by become:)
    if (target.isObject()) {
        target = memory_.followForwarded(target);
        stackValuePut(0, target);
    }
    if (rcvr.isObject()) {
        rcvr = memory_.followForwarded(rcvr);
        stackValuePut(1, rcvr);
    }

    if (!rcvr.isObject()) {
        popN(2);
        push(memory_.falseObject());
        return PrimitiveResult::Success;
    }

    ObjectHeader* header = rcvr.asObjectPtr();
    ObjectFormat format = header->format();

    // Only check pointer slots for pointer objects
    // Non-pointer formats: byte arrays, word arrays
    if (format >= ObjectFormat::Indexable8 && format <= ObjectFormat::Indexable8_7) {
        // Byte array - no pointers
        popN(2);
        push(memory_.falseObject());
        return PrimitiveResult::Success;
    }

    if (format >= ObjectFormat::Indexable32 && format <= ObjectFormat::Indexable64) {
        // Word arrays (32-bit and 64-bit) - no pointers
        popN(2);
        push(memory_.falseObject());
        return PrimitiveResult::Success;
    }

    // Determine how many pointer slots to check
    size_t slotsToCheck;

    if (header->isCompiledMethod()) {
        // For CompiledMethods, only check header + literal slots, not bytecodes
        // Method header at slot 0 contains numLiterals in bits 0-14
        Oop methodHeader = memory_.fetchPointer(0, rcvr);
        if (methodHeader.isSmallInteger()) {
            int64_t headerBits = methodHeader.asSmallInteger();
            size_t numLiterals = headerBits & 0x7FFF;  // bits 0-14
            // Slots to check: header (1) + literals (numLiterals)
            slotsToCheck = 1 + numLiterals;
        } else {
            // Invalid method header - check no slots
            slotsToCheck = 0;
        }
    } else {
        // Regular pointer object - check all slots
        slotsToCheck = header->slotCount();
    }

    // Check each pointer slot
    for (size_t i = 0; i < slotsToCheck; i++) {
        Oop slot = memory_.fetchPointer(i, rcvr);
        if (slot.rawBits() == target.rawBits()) {
            popN(2);
            push(memory_.trueObject());
            return PrimitiveResult::Success;
        }
    }

    popN(2);
    push(memory_.falseObject());
    return PrimitiveResult::Success;
}

// ===== BECOME PRIMITIVES =====

// Helper: scan C++ execution stack and replace Oop references
// This is essential because our VM uses a C++ stack, not Smalltalk contexts.
// Without this, becomeForward:/become: would miss references on the active stack.
void Interpreter::scanStackReplace(Oop oldOop, Oop newOop) {
    uint64_t oldBits = oldOop.rawBits();

    // Scan current frame state
    if (receiver_.rawBits() == oldBits) receiver_ = newOop;
    if (method_.rawBits() == oldBits) method_ = newOop;

    // Scan the live operand stack (stackPointer_ is one past the last live value)
    for (Oop* p = stackBase_; p < stackPointer_; p++) {
        if (p->rawBits() == oldBits) *p = newOop;
    }

    // Scan saved frames
    for (size_t i = 0; i < frameDepth_; i++) {
        auto& f = savedFrames_[i];
        if (f.savedReceiver.rawBits() == oldBits) f.savedReceiver = newOop;
        if (f.savedMethod.rawBits() == oldBits) f.savedMethod = newOop;
        if (f.savedHomeMethod.rawBits() == oldBits) f.savedHomeMethod = newOop;
        if (f.savedClosure.rawBits() == oldBits) f.savedClosure = newOop;
        if (f.savedActiveContext.rawBits() == oldBits) f.savedActiveContext = newOop;
    }
}

// Primitive 72: Swap identities of two objects (two-way become)
PrimitiveResult Interpreter::primitiveBecome(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    if (!rcvr.isObject() || !arg.isObject()) {
        return PrimitiveResult::Failure;
    }

    if (rcvr.rawBits() == arg.rawBits()) {
        popN(2);
        push(rcvr);
        return PrimitiveResult::Success;
    }

    // Perform two-way become by swapping all references in heap
    memory_.allObjectsDo([&](Oop obj) {
        if (!obj.isObject()) return;

        ObjectHeader* header = obj.asObjectPtr();
        ObjectFormat format = header->format();

        // Skip non-pointer objects (byte/word arrays)
        if (format >= ObjectFormat::Indexable8 && format <= ObjectFormat::Indexable8_7) return;
        if (format >= ObjectFormat::Indexable32 && format <= ObjectFormat::Indexable64) return;
        // Also skip 16-bit arrays (format 12-15)
        if (format >= ObjectFormat::Indexable16 && format <= ObjectFormat::Indexable16_3) return;

        // For CompiledMethods, only scan the literal frame (pointer slots), not bytecodes
        size_t numPointers = header->slotCount();
        if (header->isCompiledMethod() && numPointers > 0) {
            Oop methodHeader = header->slotAt(0);
            if (methodHeader.isSmallInteger()) {
                size_t numLits = methodHeader.asSmallInteger() & 0x7FFF;
                numPointers = std::min(numPointers, numLits + 1);
            }
        }

        for (size_t i = 0; i < numPointers; i++) {
            Oop slot = memory_.fetchPointer(i, obj);
            if (slot.rawBits() == rcvr.rawBits()) {
                memory_.storePointer(i, obj, arg);
            } else if (slot.rawBits() == arg.rawBits()) {
                memory_.storePointer(i, obj, rcvr);
            }
        }
    });

    // Also scan C++ execution stack (two-way: swap both directions)
    // Need a temp to avoid double-swapping
    uint64_t rcvrBits = rcvr.rawBits();
    uint64_t argBits = arg.rawBits();
    // First pass: rcvr -> sentinel, arg -> rcvr
    // Second pass: sentinel -> arg
    // Simpler: scan once and swap
    if (receiver_.rawBits() == rcvrBits) receiver_ = arg;
    else if (receiver_.rawBits() == argBits) receiver_ = rcvr;
    if (method_.rawBits() == rcvrBits) method_ = arg;
    else if (method_.rawBits() == argBits) method_ = rcvr;

    for (Oop* p = stackBase_; p < stackPointer_; p++) {
        if (p->rawBits() == rcvrBits) *p = arg;
        else if (p->rawBits() == argBits) *p = rcvr;
    }

    for (size_t i = 0; i < frameDepth_; i++) {
        auto& f = savedFrames_[i];
        if (f.savedReceiver.rawBits() == rcvrBits) f.savedReceiver = arg;
        else if (f.savedReceiver.rawBits() == argBits) f.savedReceiver = rcvr;
        if (f.savedMethod.rawBits() == rcvrBits) f.savedMethod = arg;
        else if (f.savedMethod.rawBits() == argBits) f.savedMethod = rcvr;
        if (f.savedHomeMethod.rawBits() == rcvrBits) f.savedHomeMethod = arg;
        else if (f.savedHomeMethod.rawBits() == argBits) f.savedHomeMethod = rcvr;
        if (f.savedClosure.rawBits() == rcvrBits) f.savedClosure = arg;
        else if (f.savedClosure.rawBits() == argBits) f.savedClosure = rcvr;
        if (f.savedActiveContext.rawBits() == rcvrBits) f.savedActiveContext = arg;
        else if (f.savedActiveContext.rawBits() == argBits) f.savedActiveContext = rcvr;
    }

    popN(2);
    push(rcvr);
    return PrimitiveResult::Success;
}

// primitiveBecomeForward: Forward all references from rcvr to arg (one-way become)
// Note: Not wired to a primitive number. In Pharo, becomeForward: is implemented
// via Object>>becomeForward: which wraps in arrays and calls elementsForwardIdentityTo:
// (primitive 72). This code is kept for reference but is not called.
PrimitiveResult Interpreter::primitiveBecomeForward(int argCount) {
    // Can take 1 arg (simple forward) or 2 args (with copyHash flag)
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(argCount);

    if (!rcvr.isObject() || !arg.isObject()) {
        return PrimitiveResult::Failure;
    }

    if (rcvr.rawBits() == arg.rawBits()) {
        popN(argCount + 1);
        push(rcvr);
        return PrimitiveResult::Success;
    }

    // Perform one-way become: replace all references to rcvr with arg in heap
    memory_.becomeForward(rcvr, arg);

    // Also scan C++ execution stack
    scanStackReplace(rcvr, arg);

    // Flush method cache (critical after become)
    flushMethodCache();

    popN(argCount + 1);
    push(arg);  // After become, rcvr's identity IS arg
    return PrimitiveResult::Success;
}

// ===== BIT OPERATION PRIMITIVES =====

// Primitive 575: Return the index of the high bit (1-based, 0 if no bits set)
PrimitiveResult Interpreter::primitiveHighBit(int argCount) {
    Oop rcvr = stackTop();

    if (rcvr.isSmallInteger()) {
        int64_t value = rcvr.asSmallInteger();
        if (value < 0) return PrimitiveResult::Failure;  // Undefined for negative
        if (value == 0) {
            pop();
            push(Oop::fromSmallInteger(0));
            return PrimitiveResult::Success;
        }
        int highBit = 64 - __builtin_clzll(static_cast<uint64_t>(value));
        pop();
        push(Oop::fromSmallInteger(highBit));
        return PrimitiveResult::Success;
    }

    // LargePositiveInteger support
    bool isNeg;
    if (rcvr.isObject() && isLargeInteger(memory_, rcvr, isNeg)) {
        if (isNeg) return PrimitiveResult::Failure;  // Undefined for negative
        std::vector<uint8_t> mag = extractMagnitude(memory_, rcvr);
        if (mag.size() == 1 && mag[0] == 0) {
            pop();
            push(Oop::fromSmallInteger(0));
            return PrimitiveResult::Success;
        }
        // mag is little-endian, mag.back() is MSB (guaranteed non-zero after trim)
        int highBit = (static_cast<int>(mag.size()) - 1) * 8
                    + (32 - __builtin_clz(static_cast<uint32_t>(mag.back())));
        pop();
        push(Oop::fromSmallInteger(highBit));
        return PrimitiveResult::Success;
    }

    return PrimitiveResult::Failure;
}

// Primitive 576: Return the index of the low bit (1-based, 0 if no bits set)
PrimitiveResult Interpreter::primitiveLowBit(int argCount) {
    Oop rcvr = stackTop();

    if (rcvr.isSmallInteger()) {
        int64_t value = rcvr.asSmallInteger();
        if (value == 0) {
            pop();
            push(Oop::fromSmallInteger(0));
            return PrimitiveResult::Success;
        }
        // Works for negative values too: 2's complement preserves trailing zeros
        int lowBit = __builtin_ctzll(static_cast<uint64_t>(value)) + 1;
        pop();
        push(Oop::fromSmallInteger(lowBit));
        return PrimitiveResult::Success;
    }

    // LargeInteger support (positive or negative — lowBit of -n = lowBit of n)
    bool isNeg;
    if (rcvr.isObject() && isLargeInteger(memory_, rcvr, isNeg)) {
        std::vector<uint8_t> mag = extractMagnitude(memory_, rcvr);
        if (mag.size() == 1 && mag[0] == 0) {
            pop();
            push(Oop::fromSmallInteger(0));
            return PrimitiveResult::Success;
        }
        // Find first non-zero byte (mag is little-endian)
        for (size_t i = 0; i < mag.size(); i++) {
            if (mag[i] != 0) {
                int lowBit = static_cast<int>(i) * 8
                           + __builtin_ctz(static_cast<uint32_t>(mag[i])) + 1;
                pop();
                push(Oop::fromSmallInteger(lowBit));
                return PrimitiveResult::Success;
            }
        }
        // All zero bytes (shouldn't happen after trim, but handle gracefully)
        pop();
        push(Oop::fromSmallInteger(0));
        return PrimitiveResult::Success;
    }

    return PrimitiveResult::Failure;
}

// ===== WORD ARRAY ACCESS PRIMITIVES =====

// Primitive 165: Read a 32-bit signed integer from a word-indexable object
PrimitiveResult Interpreter::primitiveIntegerAt(int argCount) {
    Oop indexOop = stackValue(0);
    Oop rcvr = stackValue(1);

    if (!rcvr.isObject() || !indexOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t index = indexOop.asSmallInteger();
    if (index < 1) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* header = rcvr.asObjectPtr();
    ObjectFormat format = header->format();

    // Must be a 32-bit word indexable object
    if (format < ObjectFormat::Indexable32 || format > ObjectFormat::Indexable32Odd) {
        return PrimitiveResult::Failure;
    }

    size_t byteSize = memory_.byteSizeOf(rcvr);
    size_t wordCount = byteSize / 4;

    size_t zeroIndex = static_cast<size_t>(index - 1);
    if (zeroIndex >= wordCount) {
        return PrimitiveResult::Failure;
    }

    int32_t* words = reinterpret_cast<int32_t*>(header + 1);
    int64_t value = words[zeroIndex];

    popN(2);
    push(Oop::fromSmallInteger(value));
    return PrimitiveResult::Success;
}

// Primitive 166: Write a 32-bit signed integer to a word-indexable object
PrimitiveResult Interpreter::primitiveIntegerAtPut(int argCount) {
    Oop valueOop = stackValue(0);
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!rcvr.isObject() || !indexOop.isSmallInteger() || !valueOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t index = indexOop.asSmallInteger();
    int64_t value = valueOop.asSmallInteger();

    if (index < 1) {
        return PrimitiveResult::Failure;
    }

    // Check value fits in 32 bits signed
    if (value < INT32_MIN || value > INT32_MAX) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* header = rcvr.asObjectPtr();
    ObjectFormat format = header->format();

    // Must be a 32-bit word indexable object
    if (format < ObjectFormat::Indexable32 || format > ObjectFormat::Indexable32Odd) {
        return PrimitiveResult::Failure;
    }

    // Check immutability
    if (header->isImmutable()) {
        return PrimitiveResult::Failure;
    }

    size_t byteSize = memory_.byteSizeOf(rcvr);
    size_t wordCount = byteSize / 4;

    size_t zeroIndex = static_cast<size_t>(index - 1);
    if (zeroIndex >= wordCount) {
        return PrimitiveResult::Failure;
    }

    int32_t* words = reinterpret_cast<int32_t*>(header + 1);
    words[zeroIndex] = static_cast<int32_t>(value);

    popN(3);
    push(valueOop);  // Return the stored value
    return PrimitiveResult::Success;
}

// ===== CLASS/BEHAVIOR PRIMITIVES =====

// Primitive 175: Return the identity hash for a behavior (class)
// Per official VM: behaviors should have stable hashes via class table
PrimitiveResult Interpreter::primitiveBehaviorHash(int argCount) {
    Oop rcvr = stackTop();

    if (!rcvr.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Use identityHashOf which properly generates and stores hash if needed
    // This ensures hash stability across calls and GC
    uint32_t hash = memory_.identityHashOf(rcvr);

    pop();
    push(Oop::fromSmallInteger(hash));
    return PrimitiveResult::Success;
}

// Primitive 115: Change the class of an object
PrimitiveResult Interpreter::primitiveChangeClass(int argCount) {
    // Primitive 115: Change the class of the receiver to the class of the argument
    // NOTE: The argument is an INSTANCE of the target class, not the class itself!
    Oop argInstance = stackValue(0);  // An instance of the target class
    Oop rcvr = stackValue(1);

    if (!rcvr.isObject() || !argInstance.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Get the class of the argument instance (not the argument itself!)
    Oop newClass = memory_.classOf(argInstance);
    if (!newClass.isObject()) {
        return PrimitiveResult::Failure;
    }

    return changeClassOf(rcvr, newClass);
}

PrimitiveResult Interpreter::changeClassOf(Oop rcvr, Oop newClass) {
    // Shared implementation for primitiveChangeClass (115) and primitiveAdoptInstance (160)
    // Follows official VM's changeClassOfto() validation
    ObjectHeader* rcvrHeader = rcvr.asObjectPtr();

    // Check immutability
    if (rcvrHeader->isImmutable()) {
        return PrimitiveResult::Failure;
    }

    // Get new class format and fixed fields from class instSpec (slot 2)
    Oop classFormatOop = memory_.fetchPointer(2, newClass);
    if (!classFormatOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }
    int64_t classFormatRaw = classFormatOop.asSmallInteger();
    size_t fixedFields = classFormatRaw & 0xFFFF;
    uint8_t classFormat = static_cast<uint8_t>((classFormatRaw >> 16) & 0x1F);

    uint8_t instFormat = static_cast<uint8_t>(rcvrHeader->format());
    size_t instSlots = rcvrHeader->slotCount();

    // Get or register the class index for the new class
    uint32_t newClassIndex = memory_.indexOfClass(newClass);
    if (newClassIndex == 0) {
        // Class not in table yet — register it (matches reference VM's enterIntoClassTable)
        newClassIndex = memory_.registerClass(newClass);
        if (newClassIndex == 0) {
            return PrimitiveResult::Failure;  // Table full
        }
    }

    uint8_t newFormat;

    if (classFormat <= 5) {
        // New class is pointer type (0=zero, 1=fixed, 2=indexable, 3=indexable+fixed, 4=weak, 5=ephemeron)
        if (instFormat > 5) {
            return PrimitiveResult::Failure;  // Can't change non-pointer to pointer
        }
        // Check slot count compatibility:
        // - Receiver must have at least as many slots as the new class's fixed fields
        // - If receiver has MORE slots than fixed fields, new class must be indexable (format 2-4)
        if (instSlots < fixedFields) {
            return PrimitiveResult::Failure;  // Not enough slots
        }
        if (instSlots > fixedFields && (classFormat <= 1 || classFormat == 5)) {
            return PrimitiveResult::Failure;  // Extra slots but class is non-indexable
        }
        newFormat = classFormat;
    } else {
        // Non-pointer format: bytes, words, compiled methods
        // For now, check basic compatibility
        if (instFormat <= 5) {
            return PrimitiveResult::Failure;  // Can't change pointer to non-pointer
        }
        // Require same format category
        bool instIsBytes = instFormat >= 16 && instFormat <= 23;
        bool classIsBytes = classFormat >= 16 && classFormat <= 23;
        bool instIsWords32 = instFormat >= 10 && instFormat <= 11;
        bool classIsWords32 = classFormat >= 10 && classFormat <= 11;
        bool instIsWords64 = instFormat == 9;
        bool classIsWords64 = classFormat == 9;
        if (instIsBytes != classIsBytes || instIsWords32 != classIsWords32 ||
            instIsWords64 != classIsWords64) {
            return PrimitiveResult::Failure;
        }
        newFormat = instFormat;  // Keep existing format (preserves padding bits)
    }

    // Update the class index and format in the object header
    rcvrHeader->setClassIndex(newClassIndex);
    // Also update the format to match the new class
    rcvrHeader->setFormat(static_cast<ObjectFormat>(newFormat));

    popN(2);
    push(rcvr);  // Return receiver
    return PrimitiveResult::Success;
}

// ===== 16-BIT ARRAY ACCESS PRIMITIVES =====

// Primitive 143: Read a 16-bit SIGNED integer from a short-indexable object
// Per official Pharo VM, shortAt returns signed values (-32768 to 32767)
PrimitiveResult Interpreter::primitiveShortAt(int argCount) {
    Oop indexOop = stackValue(0);
    Oop rcvr = stackValue(1);

    if (!rcvr.isObject() || !indexOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t index = indexOop.asSmallInteger();
    if (index < 1) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* header = rcvr.asObjectPtr();
    size_t byteSize = memory_.byteSizeOf(rcvr);
    size_t shortCount = byteSize / 2;

    size_t zeroIndex = static_cast<size_t>(index - 1);
    if (zeroIndex >= shortCount) {
        return PrimitiveResult::Failure;
    }

    // Read as signed 16-bit integer (official VM behavior)
    int16_t* shorts = reinterpret_cast<int16_t*>(header + 1);
    int64_t value = shorts[zeroIndex];  // Sign-extends automatically

    popN(2);
    push(Oop::fromSmallInteger(value));
    return PrimitiveResult::Success;
}

// Primitive 144: Write a 16-bit SIGNED integer to a short-indexable object
// Per official Pharo VM, shortAtPut accepts signed values (-32768 to 32767)
PrimitiveResult Interpreter::primitiveShortAtPut(int argCount) {
    Oop valueOop = stackValue(0);
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!rcvr.isObject() || !indexOop.isSmallInteger() || !valueOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t index = indexOop.asSmallInteger();
    int64_t value = valueOop.asSmallInteger();

    if (index < 1) {
        return PrimitiveResult::Failure;
    }

    // Check value fits in 16 bits SIGNED (official VM behavior)
    if (value < -32768 || value > 32767) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* header = rcvr.asObjectPtr();

    // Validate format: must be pure bits object (format 9-23, not compiled methods 24-31)
    auto fmt = header->format();
    int fmtInt = static_cast<int>(fmt);
    if (fmtInt < 9 || fmtInt >= 24) {
        return PrimitiveResult::Failure;  // Not a pure bits object
    }

    // Check immutability
    if (header->isImmutable()) {
        return PrimitiveResult::Failure;
    }

    size_t byteSize = memory_.byteSizeOf(rcvr);
    size_t shortCount = byteSize / 2;

    size_t zeroIndex = static_cast<size_t>(index - 1);
    if (zeroIndex >= shortCount) {
        return PrimitiveResult::Failure;
    }

    // Store as signed 16-bit integer
    int16_t* shorts = reinterpret_cast<int16_t*>(header + 1);
    shorts[zeroIndex] = static_cast<int16_t>(value);

    popN(3);
    push(valueOop);  // Return the stored value
    return PrimitiveResult::Success;
}

// ===== RAW OBJECT ITERATION PRIMITIVES =====

// Primitive 138: Return the first object in memory
PrimitiveResult Interpreter::primitiveSomeObject(int argCount) {
    Oop first = memory_.firstObject();
    if (first.isSmallInteger()) {
        return PrimitiveResult::Failure;  // No objects found
    }
    pop();  // Pop receiver
    push(first);
    return PrimitiveResult::Success;
}

// Primitive 139: Return the next object in memory after this one
PrimitiveResult Interpreter::primitiveNextObject(int argCount) {
    Oop rcvr = stackTop();

    if (!rcvr.isObject()) {
        return PrimitiveResult::Failure;
    }

    Oop next = memory_.objectAfter(rcvr);
    pop();
    push(next);  // Returns SmallInteger 0 when no more objects
    return PrimitiveResult::Success;
}

// ===== VM ATTRIBUTE PRIMITIVE =====

// Primitive 149: Get VM attribute by index
PrimitiveResult Interpreter::primitiveGetAttribute(int argCount) {
    Oop indexOop = stackTop();

    if (!indexOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t index = indexOop.asSmallInteger();

    // Negative indices: VM parameters (flags like --headless)
    // In the standard Cog VM, VM flags before the image path are at negative indices.
    // Index -1 = vmParameters_[0], -2 = vmParameters_[1], etc.
    if (index < 0) {
        int paramIdx = static_cast<int>(-index) - 1;  // -1 → 0, -2 → 1, etc.
        const auto& params = vmParameters_;
        if (paramIdx >= 0 && paramIdx < static_cast<int>(params.size())) {
            pop();
            push(memory_.createString(params[paramIdx]));
            return PrimitiveResult::Success;
        }
        // No parameter at this index — return nil (consistent with standard VM)
        pop();
        push(memory_.nil());
        return PrimitiveResult::Success;
    }

    // Index 0: VM path
    // Index 1: Image path
    // Index 2+: Command-line arguments passed after the image path
    //   If no arguments were provided, default to "--interactive" so the GUI starts.
    // Index 1000+: VM info attributes
    if (index == 0) {
        Oop str = memory_.createString(vmPath_.empty() ? "iospharo" : vmPath_);
        pop();
        push(str);
        return PrimitiveResult::Success;
    }
    if (index == 1) {
        Oop str = memory_.createString(imageName_.empty() ? "Pharo.image" : imageName_);
        pop();
        push(str);
        return PrimitiveResult::Success;
    }
    if (index >= 2 && index < 1000) {
        int argIdx = static_cast<int>(index) - 2;
        const auto& args = imageArguments_;
        if (args.empty()) {
            // No image arguments — return nil.
            // PlatformBridge.cpp explicitly sets {"--interactive"} for GUI mode.
            // test_load_image runs headless with no image args.
            pop();
            push(memory_.nil());
            return PrimitiveResult::Success;
        }
        if (argIdx < static_cast<int>(args.size())) {
            pop();
            push(memory_.createString(args[argIdx]));
            return PrimitiveResult::Success;
        }
        // Past end of args
        pop();
        push(memory_.nil());
        return PrimitiveResult::Success;
    }

    // VM info attributes (index 1000+)
    switch (index) {
        case 1001:  // Operating system name
            pop();
#if TARGET_OS_MACCATALYST
            push(memory_.createString("Mac OS"));
#elif TARGET_OS_IOS || TARGET_OS_IPHONE
            push(memory_.createString("iOS"));
#else
            push(memory_.createString("Mac OS"));
#endif
            return PrimitiveResult::Success;
        case 1002:
            pop();
            push(memory_.createString("iospharo VM 0.1"));
            return PrimitiveResult::Success;
        case 1003:
            pop();
            push(memory_.createString("StackInterpreter"));
            return PrimitiveResult::Success;
        case 1004:
            pop();
            push(Oop::fromSmallInteger(1));
            return PrimitiveResult::Success;
        case 1005:
            pop();
            push(memory_.createString("Quartz"));
            return PrimitiveResult::Success;
        case 1006:
        case 1007:
        case 1008:
            pop();
            push(memory_.createString("iospharo 2025-01-28"));
            return PrimitiveResult::Success;
        case 1009: {
            pop();
            Oop str = memory_.createString("iospharo Date: 2025-01-28T00:00:00+00:00");
            push(str);
            return PrimitiveResult::Success;
        }
        case 1201:
            // Return nil so VirtualMachine>>maxFilenameLength returns nil
            pop();
            push(memory_.nil());
            return PrimitiveResult::Success;
        default:
            return PrimitiveResult::Failure;
    }
}

// ===== IMMUTABILITY PRIMITIVES =====

// Check if an object can be made immutable.
// Per the standard Pharo VM (canBeImmutable:), certain objects must stay mutable:
// contexts, ephemerons, weak objects, semaphores, the processor scheduler,
// process lists, linked lists, and processes.
bool Interpreter::canBeImmutable(Oop oop) {
    if (!oop.isObject()) return false;  // Immediates handled separately

    ObjectHeader* header = oop.asObjectPtr();
    ObjectFormat format = header->format();
    Oop oopClass = memory_.classOf(oop);

    // Contexts cannot be immutable
    Oop contextClass = memory_.specialObject(SpecialObjectIndex::ClassMethodContext);
    if (oopClass.rawBits() == contextClass.rawBits()) {
        return false;
    }

    // Ephemerons cannot be immutable (format 5)
    if (format == ObjectFormat::WeakWithFixed) {
        return false;
    }

    // Weak objects cannot be immutable (format 4)
    if (format == ObjectFormat::Weak) {
        return false;
    }

    // Semaphores cannot be immutable
    Oop semClass = memory_.specialObject(SpecialObjectIndex::ClassSemaphore);
    if (oopClass.rawBits() == semClass.rawBits()) {
        return false;
    }

    // ProcessorScheduler cannot be immutable
    Oop schedulerAssoc = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);
    Oop scheduler = memory_.fetchPointer(1, schedulerAssoc);  // Association value
    if (oop.rawBits() == scheduler.rawBits()) {
        return false;
    }

    // processLists array cannot be immutable
    Oop processLists = memory_.fetchPointer(SchedulerProcessListsIndex, scheduler);
    if (oop.rawBits() == processLists.rawBits()) {
        return false;
    }

    // LinkedList instances (same class as entries in processLists) cannot be immutable
    if (processLists.isObject()) {
        ObjectHeader* plHeader = processLists.asObjectPtr();
        size_t numLists = plHeader->slotCount();
        if (numLists > 0) {
            Oop aList = memory_.fetchPointer(0, processLists);
            if (aList.isObject()) {
                Oop linkedListClass = memory_.classOf(aList);
                if (oopClass.rawBits() == linkedListClass.rawBits()) {
                    return false;
                }
            }
        }
    }

    // Process instances (same class as activeProcess) cannot be immutable
    Oop activeProcess = memory_.fetchPointer(SchedulerActiveProcessIndex, scheduler);
    if (activeProcess.isObject()) {
        Oop processClass = memory_.classOf(activeProcess);
        if (oopClass.rawBits() == processClass.rawBits()) {
            return false;
        }
    }

    return true;
}

// Primitive 163: Get immutability flag of object
// Object-side (argCount=0): receiver is the object to query
// MirrorPrimitives (argCount=1): argument is the object to query
PrimitiveResult Interpreter::primitiveGetImmutability(int argCount) {
    // In both cases, the target object is stackTop():
    //   argCount=0: stack = [receiver], top = receiver
    //   argCount=1: stack = [mirrorClass, targetObj], top = targetObj
    Oop target = stackTop();

    bool isImmutable;
    if (!target.isObject()) {
        // Immediates are always immutable
        isImmutable = true;
    } else {
        isImmutable = target.asObjectPtr()->isImmutable();
    }

    popN(argCount + 1);
    push(isImmutable ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

// Primitive 164: Set immutability flag of object
// Returns the PREVIOUS immutability state (boolean), not the receiver
// Object-side (argCount=1): receiver setIsReadOnlyObject: aBoolean
// MirrorPrimitives (argCount=2): MirrorPrimitives makeObject: obj readOnly: aBool
PrimitiveResult Interpreter::primitiveSetImmutability(int argCount) {
    Oop flagOop = stackValue(0);
    Oop rcvr = stackValue(1);

    // Validate flag is boolean
    bool makeImmutable;
    if (flagOop.rawBits() == memory_.trueObject().rawBits()) {
        makeImmutable = true;
    } else if (flagOop.rawBits() == memory_.falseObject().rawBits()) {
        makeImmutable = false;
    } else {
        return PrimitiveResult::Failure;  // Flag must be true or false
    }

    // Immediates: can't change, report as always immutable
    if (!rcvr.isObject()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* header = rcvr.asObjectPtr();
    bool wasImmutable = header->isImmutable();

    if (makeImmutable) {
        // Check if this object type can be made immutable
        if (!canBeImmutable(rcvr)) {
            return PrimitiveResult::Failure;
        }
        header->setImmutable(true);
    } else {
        // Making mutable is always allowed
        header->setImmutable(false);
    }

    popN(argCount + 1);
    push(wasImmutable ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

// ===== OBJECT COPY PRIMITIVE =====

// Primitive 168: Object>>copyFrom: anotherObject — copy contents of arg into receiver
PrimitiveResult Interpreter::primitiveCopyObject(int argCount) {
    // Primitive 168: Object>>copyFrom: anotherObject
    // Copy the contents of the argument (source) into the receiver (destination).
    // Both must be non-immediate objects with the same format and slot count.
    Oop arg = stackValue(0);    // anotherObject (source)
    Oop rcvr = stackValue(1);   // self (destination)

    if (!arg.isObject() || !rcvr.isObject()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* src = arg.asObjectPtr();
    ObjectHeader* dst = rcvr.asObjectPtr();

    // Must have same format and slot count
    if (src->format() != dst->format()) return PrimitiveResult::Failure;
    if (src->slotCount() != dst->slotCount()) return PrimitiveResult::Failure;

    // Copy all slots from source to destination
    size_t numSlots = src->slotCount();
    for (size_t i = 0; i < numSlots; i++) {
        dst->slotAtPut(i, src->slotAt(i));
    }

    // Pop argument, leave receiver on stack
    popN(1);
    return PrimitiveResult::Success;
}

// ===== COMPILED METHOD CREATION PRIMITIVE =====

// Primitive 79: Create a new CompiledMethod
PrimitiveResult Interpreter::primitiveNewMethod(int argCount) {
    // Arguments: class bytecodeCount: nBytes header: headerWord
    Oop headerOop = stackValue(0);
    Oop bytecountOop = stackValue(1);
    Oop classOop = stackValue(2);

    if (!bytecountOop.isSmallInteger() || !headerOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    if (!classOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    int64_t byteCount = bytecountOop.asSmallInteger();
    int64_t header = headerOop.asSmallInteger();

    if (byteCount < 0) {
        return PrimitiveResult::Failure;
    }

    // Get class index
    uint32_t classIndex = memory_.indexOfClass(classOop);

    // Extract literal count from header (low 15 bits)
    // Note: asSmallInteger() already untags, so no extra shift needed
    int literalCount = header & 0x7FFF;

    // Total bytes: (header slot + literals) * 8 + bytecodes
    // Reference: cointerp-cpp.c line 34316
    size_t totalBytes = (static_cast<size_t>(literalCount) + 1) * 8 + static_cast<size_t>(byteCount);

    // Round up to 64-bit slots
    size_t numSlots = (totalBytes + 7) / 8;

    // Calculate format with padding: CompiledMethod (24) + unused trailing bytes
    // Reference: cointerp-cpp.c line 34325
    int padding = static_cast<int>((8 - totalBytes) & 7);
    ObjectFormat format = static_cast<ObjectFormat>(
        static_cast<int>(ObjectFormat::CompiledMethod) + padding);

    // Allocate the CompiledMethod
    Oop method = memory_.allocateSlots(classIndex, numSlots, format);
    if (method.isNil()) {
        return PrimitiveResult::Failure;
    }

    // Store the header in slot 0
    memory_.storePointer(0, method, Oop::fromSmallInteger(header));

    // Initialize literal slots to nil (slots 1..literalCount)
    for (int i = 1; i <= literalCount; i++) {
        memory_.storePointer(i, method, memory_.nil());
    }

    // Zero out the bytecode area (slots after literals)
    ObjectHeader* hdr = method.asObjectPtr();
    uint8_t* bytecodeStart = hdr->bytes() + (static_cast<size_t>(literalCount) + 1) * 8;
    std::memset(bytecodeStart, 0, static_cast<size_t>(byteCount));

    popN(3);
    push(method);
    return PrimitiveResult::Success;
}

// ===== INSTANCE ADOPTION PRIMITIVE =====

// Primitive 160: Adopt an instance - change class with format compatibility check
PrimitiveResult Interpreter::primitiveAdoptInstance(int argCount) {
    // Primitive 160: aClass adoptInstance: anInstance
    // Changes anInstance's class to aClass (the receiver)
    Oop instanceOop = stackValue(0);
    Oop newClassOop = stackValue(1);

    if (!instanceOop.isObject() || !newClassOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Use shared validation with primitiveChangeClass
    return changeClassOf(instanceOop, newClassOop);
}

// ===== OBJECT PINNING PRIMITIVES =====

// Primitive 183: Check if object is pinned
PrimitiveResult Interpreter::primitiveIsPinned(int argCount) {
    Oop rcvr = stackTop();

    // Official VM: fail on immediates (PrimErrBadReceiver)
    if (!rcvr.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Follow forwarding pointers (created by become:)
    rcvr = memory_.followForwarded(rcvr);

    ObjectHeader* header = rcvr.asObjectPtr();
    bool isPinned = header->isPinned();

    pop();
    push(isPinned ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

// Primitive 184: Pin or unpin an object
// Takes 1 argument: boolean (true = pin, false = unpin)
// Returns: true if was pinned before, false if wasn't
PrimitiveResult Interpreter::primitivePin(int argCount) {
    // Official VM signature: receiver pin: aBoolean
    if (argCount != 1) {
        return PrimitiveResult::Failure;
    }

    Oop boolArg = stackValue(0);
    Oop rcvr = stackValue(1);

    // Official VM: fail on immediates
    if (!rcvr.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Validate boolean argument
    Oop trueObj = memory_.trueObject();
    Oop falseObj = memory_.falseObject();
    bool shouldPin;
    if (boolArg.rawBits() == trueObj.rawBits()) {
        shouldPin = true;
    } else if (boolArg.rawBits() == falseObj.rawBits()) {
        shouldPin = false;
    } else {
        return PrimitiveResult::Failure;  // Must be true or false
    }

    // Follow forwarding pointers (created by become:)
    rcvr = memory_.followForwarded(rcvr);

    ObjectHeader* header = rcvr.asObjectPtr();
    bool wasPinned = header->isPinned();
    header->setPinned(shouldPin);

    popN(2);
    push(wasPinned ? trueObj : falseObj);
    return PrimitiveResult::Success;
}

// Primitive 185: Unpin an object (allow GC to move it)
PrimitiveResult Interpreter::primitiveUnpin(int argCount) {
    Oop rcvr = stackTop();

    if (!rcvr.isObject()) {
        // Can't unpin immediates
        pop();
        push(memory_.falseObject());
        return PrimitiveResult::Success;
    }

    ObjectHeader* header = rcvr.asObjectPtr();
    bool wasPinned = header->isPinned();
    header->setPinned(false);

    pop();
    push(wasPinned ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

// ===== MEMORY MANAGEMENT PRIMITIVES =====

// Primitive 176: Return maximum identity hash value
PrimitiveResult Interpreter::primitiveMaxIdentityHash(int argCount) {
    // Spur uses 22-bit identity hashes
    const int64_t maxHash = 0x3FFFFF;  // 2^22 - 1

    pop();
    push(Oop::fromSmallInteger(maxHash));
    return PrimitiveResult::Success;
}

// Primitive 180: Grow memory by at least the specified amount
PrimitiveResult Interpreter::primitiveGrowMemoryByAtLeast(int argCount) {
    // Primitive 180: Grow memory by at least the specified amount
    // Returns the number of bytes actually grown (or 0 if growth failed)
    // Official VM: segSize = growOldSpaceByAtLeast(...) then returns segSize
    Oop amountOop = stackTop();

    if (!amountOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t amount = amountOop.asSmallInteger();
    if (amount < 0) {
        return PrimitiveResult::Failure;
    }

    // We don't currently support dynamic memory growth - heap is pre-allocated
    // Return 0 to indicate no growth occurred (not current free space, which
    // would mislead the image into thinking we grew the heap)
    // TODO: Implement actual heap growth via mmap or similar

    pop();
    push(Oop::fromSmallInteger(0));  // No growth occurred
    return PrimitiveResult::Success;
}

// Primitive 125: Signal semaphore when free bytes drops below threshold
PrimitiveResult Interpreter::primitiveSignalAtBytesLeft(int argCount) {
    // Primitive 125: Set low space threshold
    // Takes 1 argument: bytes (SmallInteger >= 0)
    // The semaphore is stored separately via primitiveLowSpaceSemaphore (124)

    if (argCount != 1) {
        return PrimitiveResult::Failure;
    }

    Oop bytesOop = stackValue(0);

    if (!bytesOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t bytes = bytesOop.asSmallInteger();
    if (bytes < 0) {
        return PrimitiveResult::Failure;
    }

    // Store the threshold for GC to check
    lowSpaceThreshold_ = static_cast<size_t>(bytes);

    // Return receiver (pop arg, leave receiver)
    Oop receiver = stackValue(1);
    primitiveSuccess(receiver);
    return PrimitiveResult::Success;
}

// ===== INTERRUPT SEMAPHORE PRIMITIVE =====

// Primitive 134: Set the interrupt semaphore
PrimitiveResult Interpreter::primitiveInterruptSemaphore(int argCount) {
    // Primitive 134: Register the interrupt semaphore
    // Stores semaphore in special objects array at TheInterruptSemaphore (index 30)

    if (argCount != 1) {
        return PrimitiveResult::Failure;
    }

    Oop semaphoreOop = stackValue(0);

    // Store in special objects array using the proper constant
    memory_.setSpecialObject(SpecialObjectIndex::TheInterruptSemaphore, semaphoreOop);

    // Return receiver
    Oop receiver = stackValue(1);
    primitiveSuccess(receiver);
    return PrimitiveResult::Success;
}

// ===== CONTEXT TERMINATION PRIMITIVE =====

// Primitive 196: Terminate context chain from receiver to argument
PrimitiveResult Interpreter::primitiveTerminateTo(int argCount) {
    // Primitive 196: aContext terminateTo: aContextOrNil
    // Nil senders from receiver up to (not including) target, then set receiver's
    // sender to target. Matches Cog VM behavior: always succeeds, never fails.
    Oop aContextOrNil = stackValue(0);  // argument - target context (or nil)
    Oop thisCtx = stackValue(1);        // receiver - context to terminate from

    if (!thisCtx.isObject()) {
        return PrimitiveResult::Failure;
    }

    Oop nilObj = memory_.nil();
    const size_t SenderIndex = 0;

    // Check if target is reachable from receiver via sender chain
    bool reachable = false;
    if (aContextOrNil.rawBits() == nilObj.rawBits() || aContextOrNil.isNil()) {
        reachable = true;  // nil is always reachable (terminate entire chain)
    } else {
        Oop check = thisCtx;
        for (int i = 0; i < 10000 && check.isObject() && check.rawBits() != nilObj.rawBits(); i++) {
            if (check.rawBits() == aContextOrNil.rawBits()) {
                reachable = true;
                break;
            }
            check = memory_.fetchPointer(SenderIndex, check);
        }
    }

    // If reachable, walk sender chain and nil intermediate context senders
    if (reachable) {
        Oop current = thisCtx;
        for (int i = 0; i < 10000; i++) {
            if (current.rawBits() == aContextOrNil.rawBits()) break;
            if (!current.isObject() || current.rawBits() == nilObj.rawBits()) break;
            Oop sender = memory_.fetchPointer(SenderIndex, current);
            memory_.storePointer(SenderIndex, current, nilObj);
            current = sender;
        }
    }

    // ALWAYS set receiver's sender to target (matches Cog VM - unconditional)
    if (thisCtx.isObject() && thisCtx.rawBits() != nilObj.rawBits()) {
        memory_.storePointer(SenderIndex, thisCtx, aContextOrNil);
    }

    // Pop argument, leave receiver
    popN(2);
    push(thisCtx);
    return PrimitiveResult::Success;
}

// ===== FLOAT BIT ACCESS PRIMITIVES =====

// Primitive 38: Read 32-bit word from Float at index (1 or 2)
PrimitiveResult Interpreter::primitiveFloatAt(int argCount) {
    // Primitive 38: Read 32-bit word from Float at index (1 or 2)
    // Per official VM: index 1 = most significant word, index 2 = least significant
    Oop indexOop = stackValue(0);
    Oop rcvr = stackValue(1);

    if (!indexOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t index = indexOop.asSmallInteger();
    if (index < 1 || index > 2) {
        return PrimitiveResult::Failure;
    }

    uint64_t doubleBits;

    if (rcvr.isSmallFloat()) {
        // SmallFloat64 immediate: decode the double value
        double dval = rcvr.asSmallFloat();
        std::memcpy(&doubleBits, &dval, sizeof(double));
    } else if (rcvr.isObject()) {
        ObjectHeader* header = rcvr.asObjectPtr();
        ObjectFormat format = header->format();

        if (format == ObjectFormat::Indexable64) {
            // Standard BoxedFloat64 (format 9): read 64-bit word
            doubleBits = memory_.fetchWord64(0, rcvr);
        } else {
            // Byte-format BoxedFloat64 (formats 10-15 with 8 bytes)
            uint8_t fmtVal = static_cast<uint8_t>(format);
            if (fmtVal >= 10 && fmtVal <= 15 && header->byteSize() == 8) {
                std::memcpy(&doubleBits, header->bytes(), 8);
            } else {
                return PrimitiveResult::Failure;
            }
        }
    } else {
        return PrimitiveResult::Failure;
    }

    // Return 32-bit word: index 1 = high word, index 2 = low word
    uint32_t value;
    if (index == 1) {
        value = static_cast<uint32_t>(doubleBits >> 32);
    } else {
        value = static_cast<uint32_t>(doubleBits & 0xFFFFFFFF);
    }

    popN(2);
    push(Oop::fromSmallInteger(value));
    return PrimitiveResult::Success;
}

// Primitive 39: Write 32-bit word to Float at index (1 or 2)
// Per official VM: index 1 = most significant word, index 2 = least significant
PrimitiveResult Interpreter::primitiveFloatAtPut(int argCount) {
    Oop valueOop = stackValue(0);
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!rcvr.isObject() || !indexOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    // Reject immediate floats (SmallFloat64)
    if (rcvr.isSmallFloat()) {
        return PrimitiveResult::Failure;
    }

    int64_t index = indexOop.asSmallInteger();
    if (index < 1 || index > 2) {
        return PrimitiveResult::Failure;
    }

    // Get value - accept SmallInteger or LargePositiveInteger that fits in 32 bits
    uint32_t value;
    if (valueOop.isSmallInteger()) {
        int64_t sval = valueOop.asSmallInteger();
        if (sval < 0 || sval > 0xFFFFFFFF) {
            return PrimitiveResult::Failure;
        }
        value = static_cast<uint32_t>(sval);
    } else if (valueOop.isObject()) {
        // Try to extract from LargePositiveInteger
        ObjectHeader* valHdr = valueOop.asObjectPtr();
        if (!valHdr->isBytesObject()) {
            return PrimitiveResult::Failure;
        }
        size_t byteSize = valHdr->byteSize();
        if (byteSize > 4) {
            return PrimitiveResult::Failure;  // Too large for 32 bits
        }
        // Read little-endian bytes
        value = 0;
        uint8_t* bytes = valHdr->bytes();
        for (size_t i = 0; i < byteSize; i++) {
            value |= static_cast<uint32_t>(bytes[i]) << (i * 8);
        }
    } else {
        return PrimitiveResult::Failure;
    }

    if (!rcvr.isObject()) {
        return PrimitiveResult::Failure;  // Can't write to immediates
    }

    ObjectHeader* header = rcvr.asObjectPtr();
    ObjectFormat format = header->format();

    // Check immutability
    if (header->isImmutable()) {
        return PrimitiveResult::Failure;
    }

    // Read current 64-bit value, modify the requested 32-bit word, write back
    uint64_t doubleBits;
    uint32_t* dataPtr = nullptr;

    if (format == ObjectFormat::Indexable64) {
        dataPtr = reinterpret_cast<uint32_t*>(header + 1);
    } else {
        uint8_t fmtVal = static_cast<uint8_t>(format);
        if (fmtVal >= 10 && fmtVal <= 15 && header->byteSize() == 8) {
            dataPtr = reinterpret_cast<uint32_t*>(header->bytes());
        } else {
            return PrimitiveResult::Failure;
        }
    }

    // index 1 = high word, index 2 = low word
    // On little-endian: memory is [low][high], so index 1 -> offset 1, index 2 -> offset 0
    size_t accessIndex = static_cast<size_t>(index - 1);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    accessIndex = 1 - accessIndex;
#endif
    dataPtr[accessIndex] = value;

    popN(3);
    push(valueOop);
    return PrimitiveResult::Success;
}

// ===== LARGEINTEGER DIGIT ACCESS PRIMITIVES =====

// Primitive 19: Read a byte (digit) from a LargeInteger at 1-based index
// LargeIntegers store their magnitude as little-endian bytes
PrimitiveResult Interpreter::primitiveDigitAt(int argCount) {
    Oop indexOop = stackValue(0);
    Oop rcvr = stackValue(1);

    if (!indexOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t index = indexOop.asSmallInteger();
    if (index < 1) {
        return PrimitiveResult::Failure;
    }

    // Handle SmallInteger receiver - treat as if it were a LargeInteger
    if (rcvr.isSmallInteger()) {
        int64_t value = rcvr.asSmallInteger();
        // For negative values, use absolute value
        uint64_t magnitude = static_cast<uint64_t>(value < 0 ? -value : value);

        size_t zeroIndex = static_cast<size_t>(index - 1);
        if (zeroIndex >= 8) {
            // Index beyond 8 bytes - return 0
            popN(2);
            push(Oop::fromSmallInteger(0));
            return PrimitiveResult::Success;
        }

        // Extract the byte at the given position
        uint8_t digit = (magnitude >> (zeroIndex * 8)) & 0xFF;

        popN(2);
        push(Oop::fromSmallInteger(digit));
        return PrimitiveResult::Success;
    }

    // Must be an object (LargePositiveInteger or LargeNegativeInteger)
    if (!rcvr.isObject()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* header = rcvr.asObjectPtr();
    ObjectFormat format = header->format();

    // Must be a byte-indexable object (LargeIntegers are stored as bytes)
    if (format < ObjectFormat::Indexable8 || format > ObjectFormat::Indexable8_7) {
        return PrimitiveResult::Failure;
    }

    size_t byteSize = memory_.byteSizeOf(rcvr);
    size_t zeroIndex = static_cast<size_t>(index - 1);

    if (zeroIndex >= byteSize) {
        // Index beyond the size - return 0 (as if padded with zeros)
        popN(2);
        push(Oop::fromSmallInteger(0));
        return PrimitiveResult::Success;
    }

    uint8_t* bytes = reinterpret_cast<uint8_t*>(header + 1);
    uint8_t digit = bytes[zeroIndex];

    popN(2);
    push(Oop::fromSmallInteger(digit));
    return PrimitiveResult::Success;
}

// Primitive 20: Write a byte (digit) to a LargeInteger at 1-based index
PrimitiveResult Interpreter::primitiveDigitAtPut(int argCount) {
    Oop valueOop = stackValue(0);
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!indexOop.isSmallInteger() || !valueOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t index = indexOop.asSmallInteger();
    int64_t value = valueOop.asSmallInteger();

    if (index < 1) {
        return PrimitiveResult::Failure;
    }

    // Value must be a valid byte (0-255)
    if (value < 0 || value > 255) {
        return PrimitiveResult::Failure;
    }

    // Can't modify SmallIntegers
    if (rcvr.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    if (!rcvr.isObject()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* header = rcvr.asObjectPtr();
    ObjectFormat format = header->format();

    // Must be a byte-indexable object
    if (format < ObjectFormat::Indexable8 || format > ObjectFormat::Indexable8_7) {
        return PrimitiveResult::Failure;
    }

    // Check immutability
    if (header->isImmutable()) {
        return PrimitiveResult::Failure;
    }

    size_t byteSize = memory_.byteSizeOf(rcvr);
    size_t zeroIndex = static_cast<size_t>(index - 1);

    if (zeroIndex >= byteSize) {
        return PrimitiveResult::Failure;  // Index out of bounds
    }

    uint8_t* bytes = reinterpret_cast<uint8_t*>(header + 1);
    bytes[zeroIndex] = static_cast<uint8_t>(value);

    popN(3);
    push(valueOop);  // Return the stored value
    return PrimitiveResult::Success;
}

// ===== EXCEPTION HANDLER PRIMITIVES =====

// Context slot indices for exception handling
static constexpr size_t ContextSenderIndex = 0;
static constexpr size_t ContextPCIndex = 1;
static constexpr size_t ContextStackPIndex = 2;
static constexpr size_t ContextMethodIndex = 3;
static constexpr size_t ContextClosureOrNilIndex = 4;
static constexpr size_t ContextReceiverIndex = 5;

// Primitive 186: Mark a method context as a handler method
// Used by exception handling to identify on:do: handler contexts
PrimitiveResult Interpreter::primitiveMarkHandlerMethod(int argCount) {
    // Marker primitives MUST FAIL so the method body executes.
    // The primitive index (199) in the method header marks the context
    // as an exception handler for on:do: lookup purposes.
    return PrimitiveResult::Failure;
}

// Primitive 187: Mark a method context as an unwind protect method
// Used by exception handling to identify ensure: contexts
PrimitiveResult Interpreter::primitiveMarkUnwindMethod(int argCount) {
    // This primitive marks the current context as an unwind-protect context.
    // These are contexts that must be run even when unwinding the stack.

    Oop rcvr = stackTop();

    // Marker primitives MUST FAIL so the method body executes.
    // The primitive index (198) in the method header is what matters —
    // it tells the VM to suppress context switching during this activation.
    // Returning Success would skip the ensure: body entirely!
    return PrimitiveResult::Failure;
}

// Primitive 197: findNextHandlerOrSignalingContext
// Receiver is a context. Walk from self (not sender!) looking for
// the first context whose method has primitive 199 (handler/signaling marker).
// Returns that context, or nil if not found.
PrimitiveResult Interpreter::primitiveFindHandlerContext(int argCount) {
    (void)argCount;
    // Stack: receiver (a context)
    Oop startContext = stackTop();

    if (!startContext.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Walk from self (the Smalltalk fallback does: context := self)
    Oop ctx = startContext;
    int limit = 10000;  // safety limit
    int walked = 0;

    while (ctx.isObject() && !ctx.isNil() && limit-- > 0) {
        walked++;
        // Check if this context's method has primitive 199 (handler/signaling marker)
        Oop method = memory_.fetchPointer(3, ctx);  // method = slot 3
        if (method.isObject() && !method.isNil()) {
            int primIdx = primitiveIndexOf(method);
            if (primIdx == 199) {
                pop();
                push(ctx);
                return PrimitiveResult::Success;
            }
        }
        ctx = memory_.fetchPointer(0, ctx);  // sender = slot 0
    }

    // Not found - return nil
    {
        static int missCount = 0;
        if (missCount++ < 20) {
            static FILE* fhLog = nullptr;
            if (!fhLog) fhLog = nullptr;
            if (fhLog) {
                fprintf(fhLog, "[FIND step=%llu] MISS from 0x%llx walked %d\n",
                        (unsigned long long)g_stepNum,
                        (unsigned long long)startContext.rawBits(), walked);
                // Dump sender chain methods
                Oop ch = startContext;
                for (int i = 0; i < 10 && ch.isObject() && !ch.isNil(); i++) {
                    Oop m = memory_.fetchPointer(3, ch);
                    int pi = (m.isObject() && !m.isNil()) ? primitiveIndexOf(m) : -1;
                    fprintf(fhLog, "  [%d] ctx=0x%llx method=0x%llx prim=%d\n",
                            i, (unsigned long long)ch.rawBits(),
                            (unsigned long long)m.rawBits(), pi);
                    ch = memory_.fetchPointer(0, ch);
                }
                fflush(fhLog);
            }
        }
    }
    pop();
    push(memory_.nil());
    return PrimitiveResult::Success;
}

// Primitive 195: Find the next unwind context up to a limit
// Receiver is a context (callee). Argument is stop context (or nil).
// Walk sender chain from receiver's sender, looking for method with primitive 198
// (unwind marker). Return that context, or nil if not found before stop context.
PrimitiveResult Interpreter::primitiveFindNextUnwindContext(int argCount) {
    (void)argCount;
    // Stack: receiver (callee context), arg (stop context or nil)
    Oop stopContext = stackValue(0);
    Oop calleeContext = stackValue(1);

    if (!calleeContext.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Validate stop context if not nil
    if (!stopContext.isNil() && !stopContext.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Log calls to help debug unwind mechanism
    static int unwindSearchCount = 0;
    static FILE* unwindLog = nullptr;
    unwindSearchCount++;
    if (!unwindLog) unwindLog = nullptr;

    if (unwindLog && unwindSearchCount <= 200) {
        fprintf(unwindLog, "[UNWIND-SEARCH #%d step=%llu] from=0x%llx stop=0x%llx\n",
                unwindSearchCount, (unsigned long long)g_stepNum,
                (unsigned long long)calleeContext.rawBits(),
                (unsigned long long)stopContext.rawBits());
    }

    // Walk from callee's sender
    Oop ctx = memory_.fetchPointer(0, calleeContext);  // sender = slot 0
    int limit = 10000;  // safety limit
    int depth = 0;

    while (ctx.isObject() && !ctx.isNil() && limit-- > 0) {
        // If we reached the stop context, not found
        if (ctx.rawBits() == stopContext.rawBits()) {
            break;
        }
        // Check if this context's method has primitive 198 (unwind marker)
        Oop method = memory_.fetchPointer(3, ctx);  // method = slot 3
        if (method.isObject() && !method.isNil()) {
            int primIdx = primitiveIndexOf(method);

            if (unwindLog && unwindSearchCount <= 200 && depth < 20) {
                // Get method selector for logging
                std::string sel = "?";
                Oop mhdr = memory_.fetchPointer(0, method);
                if (mhdr.isSmallInteger()) {
                    int nLits = mhdr.asSmallInteger() & 0x7FFF;
                    if (nLits >= 2 && nLits < 100) {
                        Oop selOop = memory_.fetchPointer(nLits - 1, method);
                        if (selOop.isObject() && selOop.rawBits() > 0x10000) {
                            ObjectHeader* selH = selOop.asObjectPtr();
                            if (selH->isBytesObject() && selH->byteSize() < 80) {
                                sel = std::string((char*)selH->bytes(), selH->byteSize());
                            }
                        }
                    }
                }
                fprintf(unwindLog, "  [%d] ctx=0x%llx method=#%s prim=%d%s\n",
                        depth, (unsigned long long)ctx.rawBits(), sel.c_str(), primIdx,
                        primIdx == 198 ? " *** UNWIND ***" : "");
            }

            if (primIdx == 198) {
                // Found an unwind context
                if (unwindLog && unwindSearchCount <= 200) {
                    fprintf(unwindLog, "  -> FOUND unwind at depth %d\n", depth);
                    fflush(unwindLog);
                }
                popN(2);
                push(ctx);
                return PrimitiveResult::Success;
            }
        }
        ctx = memory_.fetchPointer(0, ctx);  // sender = slot 0
        depth++;
    }

    // Not found - return nil
    if (unwindLog && unwindSearchCount <= 200) {
        fprintf(unwindLog, "  -> NOT FOUND (depth=%d)\n", depth);
        fflush(unwindLog);
    }
    popN(2);
    push(memory_.nil());
    return PrimitiveResult::Success;
}

// ===== CONTEXT INSPECTION PRIMITIVES =====

// Context layout:
// 0: sender
// 1: pc (instruction pointer as SmallInteger)
// 2: stackp (stack pointer as SmallInteger)
// 3: method
// 4: closureOrNil
// 5: receiver
// 6+: temps and stack

static constexpr size_t ContextFixedSlots = 6;  // Fixed fields before temps/stack

// Primitive 210: Read a temp/stack slot from a context at 1-based index
// Index 1 is the first temp, after the fixed context fields
// Used by Context >> tempAt: for accessing temporary variables
PrimitiveResult Interpreter::primitiveContextAt(int argCount) {
    Oop indexOop = stackValue(0);
    Oop context = stackValue(1);

    if (!context.isObject() || !indexOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t index = indexOop.asSmallInteger();
    if (index < 1) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* header = context.asObjectPtr();
    size_t slotCount = header->slotCount();

    // Convert to 0-based index into temp/stack area
    size_t zeroIndex = static_cast<size_t>(index - 1);
    size_t actualSlot = ContextFixedSlots + zeroIndex;

    if (actualSlot >= slotCount) {
        return PrimitiveResult::Failure;  // Index out of bounds
    }

    Oop value = memory_.fetchPointer(actualSlot, context);

    popN(2);
    push(value);
    return PrimitiveResult::Success;
}

// Primitive 212: Write a temp/stack slot in a context at 1-based index
PrimitiveResult Interpreter::primitiveContextAtPut(int argCount) {
    Oop value = stackValue(0);
    Oop indexOop = stackValue(1);
    Oop context = stackValue(2);

    if (!context.isObject() || !indexOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t index = indexOop.asSmallInteger();
    if (index < 1) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* header = context.asObjectPtr();

    // Check immutability
    if (header->isImmutable()) {
        return PrimitiveResult::Failure;
    }

    size_t slotCount = header->slotCount();

    // Convert to 0-based index into temp/stack area
    size_t zeroIndex = static_cast<size_t>(index - 1);
    size_t actualSlot = ContextFixedSlots + zeroIndex;

    if (actualSlot >= slotCount) {
        return PrimitiveResult::Failure;  // Index out of bounds
    }

    memory_.storePointer(actualSlot, context, value);

    // Sync to backing C++ stack frame if one exists.
    // When Smalltalk code modifies a context temp (e.g. debugger evaluating
    // "local1 := -3.0"), the backing C++ stack must also be updated so the
    // interpreter sees the new value when execution returns to that frame.
    if (frameDepth_ == 0 && context == activeContext_) {
        // Active frame: update C++ stack directly
        *(framePointer_ + 1 + zeroIndex) = value;
    } else {
        // Check saved frames. After thisContext materializes (frameDepth_ goes to 0),
        // the context is stored in activeContext_ but currentFrameMaterializedCtx_ is
        // cleared to nil. When pushFrame saves it, materializedContext is nil.
        // So we check BOTH materializedContext and savedActiveContext.
        for (size_t i = 0; i < frameDepth_; ++i) {
            if ((savedFrames_[i].materializedContext.isObject() &&
                 savedFrames_[i].materializedContext == context) ||
                (savedFrames_[i].savedActiveContext.isObject() &&
                 savedFrames_[i].savedActiveContext == context)) {
                *(savedFrames_[i].savedFP + 1 + zeroIndex) = value;
                break;
            }
        }
    }

    popN(3);
    push(value);  // Return the stored value
    return PrimitiveResult::Success;
}

// ===== CONTEXT/VM INTROSPECTION PRIMITIVES (213-218) =====
//
// These primitives provide introspection into context and VM state.
// Some are Cog JIT-specific and fail in our interpreter-only VM.

// Primitive 213: Return an integer describing context state
// Bit 0 = is or was married to a frame
// Bit 1 = is still married to a frame
// Bit 2 = frame is executing machine code (Cog-specific, always 0)
// Bit 3 = has machine code pc (Cog-specific, always 0)
// Bit 4 = method is currently compiled to machine code (Cog-specific, always 0)
PrimitiveResult Interpreter::primitiveContextXray(int argCount) {
    Oop context = stackTop();

    if (!context.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Check if this is a context (has sender, pc, stackp, method, closure, receiver)
    ObjectHeader* header = context.asObjectPtr();
    size_t slotCount = header->slotCount();
    if (slotCount < ContextFixedSlots) {
        return PrimitiveResult::Failure;  // Not a valid context
    }

    int flags = 0;

    // Check if the context is "married" to a stack frame
    // In our interpreter, contexts can be:
    // - Vanilla heap contexts (sender is another context or nil): flags = 0
    // - Married to active frame (sender encodes a frame pointer): flags = 1 or 3
    //
    // For now, we don't have sophisticated frame marriage tracking.
    // A context is considered "married" if it's currently the activeContext.
    // For simplicity, return 0 for all heap contexts.

    // Note: In a full implementation, we'd check if the sender field
    // encodes a frame pointer rather than a context reference.

    primitiveSuccess(Oop::fromSmallInteger(flags));
    return PrimitiveResult::Success;
}

// Primitive 214: Void all internal VM state
// This clears method caches and any JIT state (we don't have JIT)
PrimitiveResult Interpreter::primitiveVoidVMState(int argCount) {
    // Clear the method cache
    flushMethodCache();

    // In a JIT VM, this would also clear the machine code zone
    // and reinitialize interpreter state from the active context.
    // For our interpreter-only VM, flushing the method cache is sufficient.

    // Return receiver (the context or VM class that received the message)
    return PrimitiveResult::Success;
}

// Primitive 215: Void VM state for a specific method
// Clears any cached/compiled state for the given method
PrimitiveResult Interpreter::primitiveVoidVMStateForMethod(int argCount) {
    // Validate argument
    Oop methodObj;
    if (argCount == 0) {
        methodObj = stackTop();  // Receiver is the method
    } else if (argCount == 1) {
        methodObj = stackValue(1);  // Receiver is the method, arg is boolean
    } else {
        return PrimitiveResult::Failure;
    }

    if (!methodObj.isObject()) {
        return PrimitiveResult::Failure;
    }

    // For our interpreter, we could selectively flush cache entries
    // that reference this method. For simplicity, we just succeed
    // (method cache entries will be naturally replaced).

    // In a full implementation, iterate through cache and clear
    // entries where entry.method == methodObj

    return PrimitiveResult::Success;
}

// Primitive 216: Return method metadata (Cog JIT-specific)
// Since we don't have JIT, this primitive fails
PrimitiveResult Interpreter::primitiveMethodXray(int argCount) {
    // Cog-specific: returns information about JIT-compiled method state
    // Not applicable for interpreter-only VM
    return PrimitiveResult::Failure;
}

// Primitive 217: Get method profiling data (Cog JIT-specific)
// Since we don't have JIT profiling, this primitive fails
PrimitiveResult Interpreter::primitiveMethodProfilingData(int argCount) {
    // Cog-specific: returns profiling counters for JIT compilation
    // Not applicable for interpreter-only VM
    return PrimitiveResult::Failure;
}

// Primitive 218: Call a named primitive with an array of arguments
// receiver primitiveWithArgs: argArray
PrimitiveResult Interpreter::primitiveDoNamedPrimitiveWithArgs(int argCount) {
    // This primitive allows calling another primitive by name with explicit args.
    // The receiver should be a plugin/module name or nil (for VM primitives).
    // The first arg is the primitive name (symbol), second is args array.
    //
    // This is complex and used primarily for FFI/plugin dispatch.
    // For now, we fail and let Smalltalk handle it through normal dispatch.
    return PrimitiveResult::Failure;
}

// ===== CACHE FLUSHING PRIMITIVES =====

// Primitive 119: Flush method cache entries for a specific method
// Called when a method is modified or replaced
PrimitiveResult Interpreter::primitiveFlushCacheByMethod(int argCount) {
    // Flush all cache entries that reference this method
    Oop method = stackTop();
    for (auto& entry : methodCache_) {
        if (entry.method.rawBits() == method.rawBits()) {
            entry.selector = Oop::nil();
            entry.classOop = Oop::nil();
            entry.method = Oop::nil();
            entry.primitive = nullptr;
            entry.primitiveIndex = 0;
        }
    }
    return PrimitiveResult::Success;
}

// Primitive 120: Flush method cache entries for a specific selector
// Called when any method with this selector might have changed
PrimitiveResult Interpreter::primitiveFlushCacheBySelector(int argCount) {
    // Flush all cache entries matching this selector
    Oop selector = stackTop();
    for (auto& entry : methodCache_) {
        if (entry.selector.rawBits() == selector.rawBits()) {
            entry.selector = Oop::nil();
            entry.classOop = Oop::nil();
            entry.method = Oop::nil();
            entry.primitive = nullptr;
            entry.primitiveIndex = 0;
        }
    }
    return PrimitiveResult::Success;
}

// ===== PERFORM IN SUPERCLASS PRIMITIVE =====

// Primitive 100: perform:withArguments:inSuperclass:
// Sends a message to self but starts lookup from a specified superclass
PrimitiveResult Interpreter::primitivePerformInSuperclass(int argCount) {
    // Stack: receiver, selector, argsArray, lookupClass
    if (argCount != 3) {
        return PrimitiveResult::Failure;
    }

    Oop lookupClass = stackValue(0);
    Oop argsArray = stackValue(1);
    Oop selector = stackValue(2);
    Oop receiver = stackValue(3);

    if (!argsArray.isObject() || !lookupClass.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Follow forwarding pointers (created by become:)
    if (receiver.isObject()) {
        receiver = memory_.followForwarded(receiver);
    }

    // CRITICAL: Validate that lookupClass is in the receiver's superclass chain
    // Without this check, code could invoke methods from unrelated classes
    Oop receiverClass = memory_.classOf(receiver);
    Oop currentClass = receiverClass;
    bool foundInHierarchy = false;

    // Walk superclass chain to verify lookupClass is an ancestor
    // Limit iterations to prevent infinite loops from corrupted images
    for (int i = 0; i < 1000 && currentClass.isObject(); i++) {
        if (currentClass.rawBits() == lookupClass.rawBits()) {
            foundInHierarchy = true;
            break;
        }
        // Get superclass (slot 0 in class objects)
        ObjectHeader* classObj = currentClass.asObjectPtr();
        if (classObj->slotCount() < 1) {
            break;  // Invalid class object
        }
        currentClass = classObj->slotAt(0);  // superclass slot
    }

    if (!foundInHierarchy) {
        // lookupClass is not in receiver's class hierarchy - security violation
        return PrimitiveResult::Failure;
    }

    // Get arguments from the array
    ObjectHeader* argsHeader = argsArray.asObjectPtr();
    size_t numArgs = argsHeader->slotCount();

    // Look up the method starting from lookupClass
    Oop method = lookupMethod(selector, lookupClass);
    if (method.isNil()) {
        // DNU - fail and let Smalltalk handle it
        return PrimitiveResult::Failure;
    }

    // Pop the perform arguments (lookupClass, argsArray, selector)
    popN(3);

    // Now stack has: receiver
    // Push arguments from array
    for (size_t i = 0; i < numArgs; ++i) {
        push(argsHeader->slotAt(i));
    }

    // Activate the method (receiver is on stack, followed by args)
    activateMethod(method, static_cast<int>(numArgs));
    return PrimitiveResult::Success;
}

// ===== CLOSURE VALUE VARIANT =====

/// Primitive 204: Evaluate a closure without switching context
// This is used for very simple blocks that shouldn't create a context
// Per official VM: executes block without creating context frame and without
// allowing context switches (atomic execution)
PrimitiveResult Interpreter::primitiveClosureValueNoContextSwitch(int argCount) {
    // IMPORTANT: The current primitiveBlockValue implementation DOES create
    // context frames and DOES allow context switches, which violates the
    // semantics of this primitive.
    //
    // Proper implementation would:
    // 1. Check block is simple enough (no sends, no non-local returns)
    // 2. Execute bytecode inline without creating context
    // 3. Disable interrupt checks during execution
    //
    // For now, fail to Smalltalk fallback which handles this correctly
    // rather than give wrong semantics (context switches during "atomic" block)
    (void)argCount;
    return PrimitiveResult::Failure;
}

// ===== CLASS STRUCTURE PRIMITIVES =====

// Primitive 254: Get the number of named instance variables of a class
// receiver instSize -> SmallInteger
PrimitiveResult Interpreter::primitiveInstSize(int argCount) {
    Oop classOop = stackTop();

    if (!classOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Class format is stored in slot 2 (after superclass and methodDict)
    // The format encodes the instance size in bits 0-15
    Oop formatOop = memory_.fetchPointer(2, classOop);
    if (!formatOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t format = formatOop.asSmallInteger();
    // Instance size is stored in the low 16 bits
    int64_t instSize = format & 0xFFFF;

    pop();
    push(Oop::fromSmallInteger(instSize));
    return PrimitiveResult::Success;
}

// Primitive 255: primitiveSizeInBytesOfInstance - reuses implementation from primitive 181

// Primitive 253: Get the superclass of a class
// receiver superclass -> Class or nil
PrimitiveResult Interpreter::primitiveSuperclass(int argCount) {
    Oop classOop = stackTop();

    if (!classOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Superclass is stored in slot 0
    Oop superclass = memory_.fetchPointer(0, classOop);

    pop();
    push(superclass);
    return PrimitiveResult::Success;
}

// ===== CONTEXT SIZE PRIMITIVE =====

// Primitive 210: Get the number of temp/stack slots in a context
// receiver contextSize -> SmallInteger
PrimitiveResult Interpreter::primitiveContextSize(int argCount) {
    Oop context = stackTop();

    if (!context.isObject()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* header = context.asObjectPtr();
    size_t slotCount = header->slotCount();

    // Subtract the fixed context fields (sender, pc, stackp, method, closureOrNil, receiver)
    size_t tempStackSize = slotCount > ContextFixedSlots ? slotCount - ContextFixedSlots : 0;

    pop();
    push(Oop::fromSmallInteger(static_cast<int64_t>(tempStackSize)));
    return PrimitiveResult::Success;
}

// ===== OBJECT SIZE PRIMITIVES (181-182) =====

// Primitive 181: Get the size in bytes of an instance of a class
// receiver sizeInBytesOfInstance: numElements -> SmallInteger
// For variable-size classes, numElements specifies the number of indexable elements
PrimitiveResult Interpreter::primitiveSizeInBytesOfInstance(int argCount) {
    Oop classOop;
    size_t numElements = 0;

    if (argCount == 0) {
        // No argument - get size for fixed-size instance
        classOop = stackTop();
    } else if (argCount == 1) {
        // One argument - number of indexable elements
        Oop numElemsOop = stackValue(0);
        classOop = stackValue(1);

        if (!numElemsOop.isSmallInteger()) {
            return PrimitiveResult::Failure;
        }
        int64_t n = numElemsOop.asSmallInteger();
        if (n < 0) {
            return PrimitiveResult::Failure;
        }
        numElements = static_cast<size_t>(n);
    } else {
        return PrimitiveResult::Failure;
    }

    if (!classOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Get the class format to determine instance structure
    // Class format is at slot 2 (after superclass, methodDict)
    Oop formatOop = memory_.fetchPointer(2, classOop);
    if (!formatOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t format = formatOop.asSmallInteger();

    // Extract instance size (number of fixed fields) from format
    // In Spur, format encodes: instSize in bits 0-15, format type in bits 16-20
    size_t instSize = static_cast<size_t>(format & 0xFFFF);
    int formatType = static_cast<int>((format >> 16) & 0x1F);

    // Calculate total slots based on format type (matches Cog VM's byteSizeOfInstanceOf:)
    size_t numSlots;

    switch (formatType) {
        case 2: // Pure indexable (Array)
            numSlots = numElements;
            break;
        case 3: // Indexable with fixed (e.g., Context)
        case 4: // Weak
        case 5: // Weak with fixed
            numSlots = instSize + numElements;
            break;
        case 9: // 64-bit indexable
            numSlots = numElements;
            break;
        case 10: // 32-bit first long format
            numSlots = (numElements + 1) / 2;
            break;
        case 12: // 16-bit first short format
            numSlots = (numElements + 3) / 4;
            break;
        case 16: // Byte format (first)
        case 24: // CompiledMethod format (first)
            numSlots = (numElements + 7) / 8;
            break;
        default:
            if (formatType <= 1) {
                // Fixed size only (argCount==0 case or no variable part)
                numSlots = instSize + numElements;
            } else {
                return PrimitiveResult::Failure;
            }
            break;
    }

    // Compute total byte size matching Spur layout:
    // - numSlots == 0: minimum 16 bytes (8 header + 8 body for forwarding ptr)
    // - numSlots < 255: numSlots * 8 + 8 (body + standard header)
    // - numSlots >= 255: numSlots * 8 + 16 (body + overflow header + standard header)
    size_t totalBytes;
    if (numSlots == 0) {
        totalBytes = 8 + 8;  // BaseHeaderSize + minimum body
    } else if (numSlots >= 255) {
        totalBytes = (numSlots * 8) + 8 + 8;  // body + 2 headers (overflow)
    } else {
        totalBytes = (numSlots * 8) + 8;  // body + 1 header
    }

    popN(argCount + 1);
    push(Oop::fromSmallInteger(static_cast<int64_t>(totalBytes)));
    return PrimitiveResult::Success;
}

// Primitive 182: Get the size in bytes of an object
// receiver sizeInBytes -> SmallInteger
// Returns the total memory size of the object including header
PrimitiveResult Interpreter::primitiveSizeInBytes(int argCount) {
    Oop obj = stackTop();

    if (obj.isImmediate()) {
        // Immediates don't have a memory size in the traditional sense
        // Return 0 or fail - we'll return 8 (the size of the Oop itself)
        pop();
        push(Oop::fromSmallInteger(8));
        return PrimitiveResult::Success;
    }

    if (!obj.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Get the total size from the object memory
    size_t totalSize = memory_.totalSizeOf(obj);

    pop();
    push(Oop::fromSmallInteger(static_cast<int64_t>(totalSize)));
    return PrimitiveResult::Success;
}

// ===== CONTEXT MANIPULATION PRIMITIVES (190-195) =====
// These primitives allow modifying the fields of a Context object
// Used for exception handling, debugging, and process manipulation

// Primitive 190: Set the sender of a context
// receiver privSender: aContext -> receiver
PrimitiveResult Interpreter::primitiveSetSender(int argCount) {
    if (argCount != 1) {
        return PrimitiveResult::Failure;
    }

    Oop newSender = stackValue(0);
    Oop context = stackValue(1);

    if (!context.isObject()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* header = context.asObjectPtr();
    if (header->isImmutable()) {
        return PrimitiveResult::Failure;
    }

    // Sender is at slot 0
    memory_.storePointer(ContextSenderIndex, context, newSender);

    popN(2);
    push(context);  // Return receiver
    return PrimitiveResult::Success;
}

// Primitive 191: Set the instruction pointer (pc) of a context
// receiver pc: aSmallInteger -> receiver
PrimitiveResult Interpreter::primitiveSetInstructionPointer(int argCount) {
    if (argCount != 1) {
        return PrimitiveResult::Failure;
    }

    Oop newPC = stackValue(0);
    Oop context = stackValue(1);

    if (!context.isObject() || !newPC.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* header = context.asObjectPtr();
    if (header->isImmutable()) {
        return PrimitiveResult::Failure;
    }

    // PC is at slot 1
    memory_.storePointer(ContextPCIndex, context, newPC);

    popN(2);
    push(context);  // Return receiver
    return PrimitiveResult::Success;
}

// Primitive 192: Set the stack pointer of a context
// receiver stackp: aSmallInteger -> receiver
// Primitive 76: Atomic store into context stackPointer
// Per official VM: also ensures any newly accessible cells are initialized to nil
PrimitiveResult Interpreter::primitiveSetStackPointer(int argCount) {
    if (argCount != 1) {
        return PrimitiveResult::Failure;
    }

    Oop newStackpOop = stackValue(0);
    Oop context = stackValue(1);

    if (!context.isObject() || !newStackpOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* header = context.asObjectPtr();
    if (header->isImmutable()) {
        return PrimitiveResult::Failure;
    }

    // Validate stack pointer is within bounds
    int64_t newStackp = newStackpOop.asSmallInteger();
    size_t slotCount = header->slotCount();
    if (newStackp < 0 || static_cast<size_t>(newStackp) > slotCount - ContextFixedSlots) {
        return PrimitiveResult::Failure;
    }

    // Get current stack pointer
    Oop oldStackpOop = memory_.fetchPointer(ContextStackPIndex, context);
    int64_t oldStackp = oldStackpOop.isSmallInteger() ? oldStackpOop.asSmallInteger() : 0;

    // Per official VM: nil any newly accessible cells when growing stack
    // Temp/stack area starts at ContextFixedSlots (slot 6)
    if (newStackp > oldStackp) {
        Oop nilObj = Oop::nil();
        for (int64_t i = oldStackp + 1; i <= newStackp; i++) {
            size_t slot = ContextFixedSlots + static_cast<size_t>(i) - 1;
            if (slot < slotCount) {
                memory_.storePointer(slot, context, nilObj);
            }
        }
    }

    // Store new stackp at slot 2
    memory_.storePointer(ContextStackPIndex, context, newStackpOop);

    popN(1);  // Pop argument, leave receiver
    return PrimitiveResult::Success;
}

// Primitive 193: Set the method of a context
// receiver method: aCompiledMethod -> receiver
PrimitiveResult Interpreter::primitiveSetMethod(int argCount) {
    if (argCount != 1) {
        return PrimitiveResult::Failure;
    }

    Oop newMethod = stackValue(0);
    Oop context = stackValue(1);

    if (!context.isObject()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* header = context.asObjectPtr();
    if (header->isImmutable()) {
        return PrimitiveResult::Failure;
    }

    // Method is at slot 3
    memory_.storePointer(ContextMethodIndex, context, newMethod);

    popN(2);
    push(context);  // Return receiver
    return PrimitiveResult::Success;
}

// Primitive 194: Set the receiver of a context
// receiver receiver: anObject -> receiver (the context)
PrimitiveResult Interpreter::primitiveSetReceiver(int argCount) {
    if (argCount != 1) {
        return PrimitiveResult::Failure;
    }

    Oop newReceiver = stackValue(0);
    Oop context = stackValue(1);

    if (!context.isObject()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* header = context.asObjectPtr();
    if (header->isImmutable()) {
        return PrimitiveResult::Failure;
    }

    // Receiver is at slot 5
    memory_.storePointer(ContextReceiverIndex, context, newReceiver);

    popN(2);
    push(context);  // Return receiver (the context)
    return PrimitiveResult::Success;
}

// Primitive 195: Set the closure (or nil) of a context
// receiver closureOrNil: aBlockClosureOrNil -> receiver
PrimitiveResult Interpreter::primitiveSetClosureOrNil(int argCount) {
    if (argCount != 1) {
        return PrimitiveResult::Failure;
    }

    Oop newClosure = stackValue(0);
    Oop context = stackValue(1);

    if (!context.isObject()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* header = context.asObjectPtr();
    if (header->isImmutable()) {
        return PrimitiveResult::Failure;
    }

    // ClosureOrNil is at slot 4
    memory_.storePointer(ContextClosureOrNilIndex, context, newClosure);

    popN(2);
    push(context);  // Return receiver
    return PrimitiveResult::Success;
}

// ===== QUICK RETURN PRIMITIVES =====
// These are optimized primitives that return special values directly

// Primitive 256: Return self (the receiver)
PrimitiveResult Interpreter::primitiveQuickReturnSelf(int argCount) {
    // Stack has receiver at stackValue(argCount)
    // For quick return, the method body just returns self
    // Nothing to do - receiver is already the result
    return PrimitiveResult::Success;
}

// Primitive 257: Return true
PrimitiveResult Interpreter::primitiveQuickReturnTrue(int argCount) {
    // Pop receiver, push true
    pop();
    push(memory_.trueObject());
    return PrimitiveResult::Success;
}

// Primitive 258: Return false
PrimitiveResult Interpreter::primitiveQuickReturnFalse(int argCount) {
    // Pop receiver, push false
    pop();
    push(memory_.falseObject());
    return PrimitiveResult::Success;
}

// Primitive 259: Return nil
PrimitiveResult Interpreter::primitiveQuickReturnNil(int argCount) {
    // Pop receiver, push nil
    pop();
    push(memory_.nil());
    return PrimitiveResult::Success;
}

// ===== OBJECT FORMAT QUERY PRIMITIVES =====

// Primitive 15 variant: Check if object contains bytes
// receiver isBytes -> Boolean
PrimitiveResult Interpreter::primitiveIsBytes(int argCount) {
    Oop obj = stackTop();

    if (obj.isImmediate()) {
        pop();
        push(memory_.falseObject());
        return PrimitiveResult::Success;
    }

    ObjectHeader* header = obj.asObjectPtr();
    ObjectFormat fmt = header->format();

    // Byte formats are 16-23 (Indexable8 through Indexable8_7)
    bool isBytes = (fmt >= ObjectFormat::Indexable8 && fmt <= ObjectFormat::Indexable8_7);

    pop();
    push(isBytes ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

// Primitive 15 variant: Check if object contains 64-bit words
// receiver isWords -> Boolean
PrimitiveResult Interpreter::primitiveIsWords(int argCount) {
    Oop obj = stackTop();

    if (obj.isImmediate()) {
        pop();
        push(memory_.falseObject());
        return PrimitiveResult::Success;
    }

    ObjectHeader* header = obj.asObjectPtr();
    ObjectFormat fmt = header->format();

    // Word64 format is 10 (Indexable64)
    bool isWords = (fmt == ObjectFormat::Indexable64);

    pop();
    push(isWords ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

// Primitive 15 variant: Check if object contains pointers (Oops)
// receiver isPointers -> Boolean
PrimitiveResult Interpreter::primitiveIsPointers(int argCount) {
    Oop obj = stackTop();

    if (obj.isImmediate()) {
        pop();
        push(memory_.falseObject());
        return PrimitiveResult::Success;
    }

    ObjectHeader* header = obj.asObjectPtr();
    ObjectFormat fmt = header->format();

    // Pointer formats are 0-4 (FixedSize, Indexable, etc.)
    bool isPointers = (fmt <= ObjectFormat::Weak);

    pop();
    push(isPointers ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

// ===== STRING HASH PRIMITIVE =====

// Primitive 146: Compute hash of a byte object (String, Symbol)
// This uses a simple polynomial rolling hash
PrimitiveResult Interpreter::primitiveStringHash(int argCount) {
    Oop obj = stackTop();

    if (!obj.isObject()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* header = obj.asObjectPtr();
    ObjectFormat fmt = header->format();

    // Must be a byte object (format 16-23)
    if (fmt < ObjectFormat::Indexable8 || fmt > ObjectFormat::Indexable8_7) {
        return PrimitiveResult::Failure;
    }

    size_t byteCount = memory_.byteSizeOf(obj);
    uint8_t* bytes = reinterpret_cast<uint8_t*>(header + 1);

    // Official VM MiscPrimitivePlugin algorithm:
    // hash := (hash + byte) * 1664525. Result masked to 28 bits.
    uint32_t hash = 0;
    for (size_t i = 0; i < byteCount; ++i) {
        hash = (hash + bytes[i]) * 1664525;
    }
    hash = hash & 0x0FFFFFFF;

    pop();
    push(Oop::fromSmallInteger(static_cast<int64_t>(hash)));
    return PrimitiveResult::Success;
}

// Primitive 146: String class >> stringHash:initialHash:
// Computes hash of a string with an initial hash value
// Arguments: receiver (String class), aString, initialHash
PrimitiveResult Interpreter::primitiveStringHashInitialHash(int argCount) {
    static FILE* prim146Log = nullptr;
    static int callCount = 0;
    if (!prim146Log) prim146Log = nullptr;

    callCount++;
    if (prim146Log && callCount <= 20) {
        fprintf(prim146Log, "[PRIM146 #%d] called with argCount=%d\n", callCount, argCount);
        fflush(prim146Log);
    }

    if (argCount != 2) {
        if (prim146Log && callCount <= 20) {
            fprintf(prim146Log, "[PRIM146 #%d] FAIL: argCount != 2\n", callCount);
            fflush(prim146Log);
        }
        return PrimitiveResult::Failure;
    }

    // Stack: receiver (class), aString, initialHash
    Oop initialHashOop = stackValue(0);  // top
    Oop stringOop = stackValue(1);

    if (prim146Log && callCount <= 20) {
        fprintf(prim146Log, "[PRIM146 #%d] initialHash=0x%llx isSmallInt=%d, string=0x%llx isObj=%d\n",
                callCount,
                (unsigned long long)initialHashOop.rawBits(), initialHashOop.isSmallInteger() ? 1 : 0,
                (unsigned long long)stringOop.rawBits(), stringOop.isObject() ? 1 : 0);
        fflush(prim146Log);
    }

    // Initial hash must be a SmallInteger
    if (!initialHashOop.isSmallInteger()) {
        if (prim146Log && callCount <= 20) {
            fprintf(prim146Log, "[PRIM146 #%d] FAIL: initialHash not SmallInteger\n", callCount);
            fflush(prim146Log);
        }
        return PrimitiveResult::Failure;
    }
    int64_t speciesHash = initialHashOop.asSmallInteger();

    // String must be a byte object
    if (!stringOop.isObject()) {
        if (prim146Log && callCount <= 20) {
            fprintf(prim146Log, "[PRIM146 #%d] FAIL: string not object\n", callCount);
            fflush(prim146Log);
        }
        return PrimitiveResult::Failure;
    }

    ObjectHeader* header = stringOop.asObjectPtr();
    ObjectFormat fmt = header->format();

    if (prim146Log && callCount <= 20) {
        fprintf(prim146Log, "[PRIM146 #%d] string format=%d (need 16-23)\n", callCount, (int)fmt);
        fflush(prim146Log);
    }

    // Must be a byte object (format 16-23)
    if (fmt < ObjectFormat::Indexable8 || fmt > ObjectFormat::Indexable8_7) {
        if (prim146Log && callCount <= 20) {
            fprintf(prim146Log, "[PRIM146 #%d] FAIL: not byte object (format=%d)\n", callCount, (int)fmt);
            fflush(prim146Log);
        }
        return PrimitiveResult::Failure;
    }

    size_t stringSize = memory_.byteSizeOf(stringOop);
    uint8_t* bytes = reinterpret_cast<uint8_t*>(header + 1);

    // Official VM MiscPrimitivePlugin (from generated C):
    // hash = (hash + aByteArray[pos]) * 1664525;
    // Result masked to 28 bits (0xFFFFFFF)
    uint32_t hash = static_cast<uint32_t>(speciesHash);
    for (size_t i = 0; i < stringSize; ++i) {
        hash = (hash + bytes[i]) * 1664525;
    }
    hash = hash & 0x0FFFFFFF;

    if (prim146Log && callCount <= 20) {
        fprintf(prim146Log, "[PRIM146 #%d] SUCCESS: hash=%u\n", callCount, hash);
        fflush(prim146Log);
    }

    // Pop arguments and receiver (3 items), push result
    popN(3);
    push(Oop::fromSmallInteger(static_cast<int64_t>(hash)));
    return PrimitiveResult::Success;
}

// MiscPrimitivePlugin: indexOfAscii:inString:startingAt:
// Arguments: receiver (class), asciiValue, aString, startIndex
PrimitiveResult Interpreter::primitiveIndexOfAscii(int argCount) {
    if (argCount != 3) return PrimitiveResult::Failure;

    // Stack: receiver (class), asciiValue, aString, startIndex
    Oop startOop = stackValue(0);
    Oop stringOop = stackValue(1);
    Oop asciiOop = stackValue(2);

    if (!startOop.isSmallInteger()) return PrimitiveResult::Failure;
    if (!asciiOop.isSmallInteger()) return PrimitiveResult::Failure;
    if (!stringOop.isObject()) return PrimitiveResult::Failure;

    int64_t start = startOop.asSmallInteger() - 1;  // Convert to 0-based
    int64_t asciiValue = asciiOop.asSmallInteger();

    if (start < 0 || asciiValue < 0 || asciiValue > 255) return PrimitiveResult::Failure;

    ObjectHeader* header = stringOop.asObjectPtr();
    ObjectFormat fmt = header->format();

    // Must be a byte object (format 16-23)
    if (fmt < ObjectFormat::Indexable8 || fmt > ObjectFormat::Indexable8_7) {
        return PrimitiveResult::Failure;
    }

    size_t stringSize = memory_.byteSizeOf(stringOop);
    uint8_t* bytes = reinterpret_cast<uint8_t*>(header + 1);
    uint8_t target = static_cast<uint8_t>(asciiValue);

    // Search for the character
    for (size_t i = static_cast<size_t>(start); i < stringSize; i++) {
        if (bytes[i] == target) {
            popN(4);  // Pop receiver and 3 args
            push(Oop::fromSmallInteger(static_cast<int64_t>(i + 1)));  // 1-based result
            return PrimitiveResult::Success;
        }
    }

    // Not found - return 0
    popN(4);
    push(Oop::fromSmallInteger(0));
    return PrimitiveResult::Success;
}

// ===== CLASS NAME PRIMITIVE =====

// Primitive 514: Get the name of a class (as a Symbol or String)
// receiver name -> Symbol
PrimitiveResult Interpreter::primitiveClassName(int argCount) {
    Oop classOop = stackTop();

    if (!classOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    // In Pharo, class name is stored at slot 6 (after superclass, methodDict, format,
    // instanceVariables, organization, subclasses)
    // But the exact layout can vary. Let's use slot 6 as a common position.
    Oop name = memory_.fetchPointer(6, classOop);

    pop();
    push(name);
    return PrimitiveResult::Success;
}

// ===== FFI AND SYSTEM PRIMITIVES (515-527) =====

// Primitive 515: Get VM information string
// index primitiveVMInformation -> string/integer
// Returns various VM information based on index
PrimitiveResult Interpreter::primitiveVMInformation(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    if (!indexOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    intptr_t index = indexOop.asSmallInteger();
    Oop result = Oop::nil();

    switch (index) {
        case 1:  // VM version string
            result = createStringObject(memory_, "iOS Pharo VM 1.0");
            break;
        case 2:  // Build timestamp
            result = createStringObject(memory_, __DATE__ " " __TIME__);
            break;
        case 3:  // Platform name
            result = createStringObject(memory_, "iOS");
            break;
        case 4:  // Compiler info
            result = createStringObject(memory_, "Clang C++17");
            break;
        default:
            // Unknown index - return nil
            break;
    }

    pop();  // index
    pop();  // receiver
    push(result);
    return PrimitiveResult::Success;
}

// Primitive 516: Get image base address
// primitiveImageBaseAddress -> integer
// Returns the base address of the loaded image in memory
PrimitiveResult Interpreter::primitiveImageBaseAddress(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Return the old space start address as a small integer (truncated)
    uintptr_t baseAddr = reinterpret_cast<uintptr_t>(memory_.oldSpaceStart());

    Oop result = uint64ToOop(memory_, baseAddr);
    if (result.isNil()) return PrimitiveResult::Failure;
    pop();
    push(result);
    return PrimitiveResult::Success;
}

// Primitive 517: Get highest available memory address
// primitiveHighestAvailableAddress -> integer
// Returns the highest usable memory address
PrimitiveResult Interpreter::primitiveHighestAvailableAddress(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    uintptr_t highAddr = reinterpret_cast<uintptr_t>(memory_.oldSpaceEnd());

    Oop result = uint64ToOop(memory_, highAddr);
    if (result.isNil()) return PrimitiveResult::Failure;
    pop();
    push(result);
    return PrimitiveResult::Success;
}

// Primitive 518: Check if context is post-mortem (dead)
// aContext primitiveIsContextPostMortem -> boolean
// Returns true if the context is no longer active
PrimitiveResult Interpreter::primitiveIsContextPostMortem(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop contextOop = stackTop();
    if (!contextOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    // A context is post-mortem if its sender is nil and it's not the active context
    // Check if the context's instruction pointer is nil or negative
    Oop ipOop = memory_.fetchPointer(1, contextOop);  // Instruction pointer at slot 1

    bool isPostMortem = ipOop.isNil();

    pop();
    push(isPostMortem ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

// Primitive 519: Get sandboxed process arguments
// primitiveSandboxedArgs -> array or nil
// Returns sanitized command line arguments (if any)
PrimitiveResult Interpreter::primitiveSandboxedArgs(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // On iOS, there are typically no command line arguments
    // Return nil for security/sandboxing
    pop();
    push(memory_.nil());
    return PrimitiveResult::Success;
}

// Primitive 520: Debug halt / breakpoint
// primitiveDebugHalt -> receiver
// Triggers a debugger breakpoint if attached
PrimitiveResult Interpreter::primitiveDebugHalt(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // In debug builds, this could trigger a breakpoint
    // For iOS release, just log and continue
#ifdef DEBUG
    // Could use: __builtin_debugtrap(); or raise(SIGTRAP);
#endif

    // Return receiver (self)
    return PrimitiveResult::Success;
}

// Primitive 521: Flush external primitive cache for a method
// aMethod primitiveFlushExternalPrimitiveOf -> receiver
// Flushes cached external primitive lookup for a compiled method
PrimitiveResult Interpreter::primitiveFlushExternalPrimitiveOf(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // External primitives are not cached in this VM implementation
    // Just succeed - no action needed
    return PrimitiveResult::Success;
}

// Primitive 522: Prepare stack for non-local return
// targetContext primitiveSetStackForNonLocalReturn -> receiver
// Prepares the stack for a non-local return to target context
PrimitiveResult Interpreter::primitivePrepareStackForNonLocalReturn(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop targetContext = stackTop();
    if (!targetContext.isObject()) {
        return PrimitiveResult::Failure;
    }

    // This primitive is used by the exception handling system
    // The actual non-local return is handled by bytecode interpretation
    // Just validate the target and succeed
    popN(1);  // pop target, leave receiver
    return PrimitiveResult::Success;
}

// Primitive 523: Get/set context instruction pointer
// aContext primitiveContextInstructionPointer -> integer
// Returns the instruction pointer of a context
PrimitiveResult Interpreter::primitiveContextInstructionPointer(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop contextOop = stackTop();
    if (!contextOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Instruction pointer is at slot 1 in MethodContext
    Oop ipOop = memory_.fetchPointer(1, contextOop);

    pop();
    push(ipOop);
    return PrimitiveResult::Success;
}

// Primitive 524: External object access (for FFI)
// index primitiveExternalObjectAccess -> object or nil
// Access external objects registered with the VM
PrimitiveResult Interpreter::primitiveExternalObjectAccess(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    if (!indexOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    // External object table is not implemented in this VM
    // Return nil for any index
    popN(1);  // index
    pop();    // receiver
    push(memory_.nil());
    return PrimitiveResult::Success;
}

// Primitive 525: Convert byte array to 32-bit integer
// byteArray offset bigEndian primitiveByteArrayToInt32 -> integer
// Reads a 32-bit integer from a byte array at given offset
PrimitiveResult Interpreter::primitiveByteArrayToInt32(int argCount) {
    if (argCount != 3) return PrimitiveResult::Failure;

    Oop bigEndianOop = stackTop();
    Oop offsetOop = stackValue(1);
    Oop byteArrayOop = stackValue(2);

    if (!offsetOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }
    if (!byteArrayOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    intptr_t offset = offsetOop.asSmallInteger();
    bool bigEndian = (bigEndianOop == memory_.trueObject());

    // Get byte array size
    size_t size = memory_.byteSizeOf(byteArrayOop);
    if (offset < 0 || static_cast<size_t>(offset) + 4 > size) {
        return PrimitiveResult::Failure;
    }

    // Read 4 bytes
    uint8_t b0 = memory_.fetchByte(offset, byteArrayOop);
    uint8_t b1 = memory_.fetchByte(offset + 1, byteArrayOop);
    uint8_t b2 = memory_.fetchByte(offset + 2, byteArrayOop);
    uint8_t b3 = memory_.fetchByte(offset + 3, byteArrayOop);

    int32_t value;
    if (bigEndian) {
        value = (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
    } else {
        value = (b3 << 24) | (b2 << 16) | (b1 << 8) | b0;
    }

    popN(3);  // arguments
    pop();    // receiver
    push(Oop::fromSmallInteger(value));
    return PrimitiveResult::Success;
}

// Primitive 526: Store 32-bit integer to byte array
// byteArray offset value bigEndian primitiveInt32ToByteArray -> byteArray
// Writes a 32-bit integer to a byte array at given offset
PrimitiveResult Interpreter::primitiveInt32ToByteArray(int argCount) {
    if (argCount != 4) return PrimitiveResult::Failure;

    Oop bigEndianOop = stackTop();
    Oop valueOop = stackValue(1);
    Oop offsetOop = stackValue(2);
    Oop byteArrayOop = stackValue(3);

    if (!offsetOop.isSmallInteger() || !valueOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }
    if (!byteArrayOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    intptr_t offset = offsetOop.asSmallInteger();
    int32_t value = static_cast<int32_t>(valueOop.asSmallInteger());
    bool bigEndian = (bigEndianOop == memory_.trueObject());

    // Get byte array size
    size_t size = memory_.byteSizeOf(byteArrayOop);
    if (offset < 0 || static_cast<size_t>(offset) + 4 > size) {
        return PrimitiveResult::Failure;
    }

    // Write 4 bytes
    if (bigEndian) {
        memory_.storeByte(offset, byteArrayOop, (value >> 24) & 0xFF);
        memory_.storeByte(offset + 1, byteArrayOop, (value >> 16) & 0xFF);
        memory_.storeByte(offset + 2, byteArrayOop, (value >> 8) & 0xFF);
        memory_.storeByte(offset + 3, byteArrayOop, value & 0xFF);
    } else {
        memory_.storeByte(offset, byteArrayOop, value & 0xFF);
        memory_.storeByte(offset + 1, byteArrayOop, (value >> 8) & 0xFF);
        memory_.storeByte(offset + 2, byteArrayOop, (value >> 16) & 0xFF);
        memory_.storeByte(offset + 3, byteArrayOop, (value >> 24) & 0xFF);
    }

    popN(4);  // arguments
    push(byteArrayOop);  // return the byte array
    return PrimitiveResult::Success;
}

// Primitive 527: Get address of object or external pointer
// anObject primitivePointerAddress -> integer
// Returns the memory address of an object (for FFI use)
PrimitiveResult Interpreter::primitivePointerAddress(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop objOop = stackTop();

    uintptr_t address = 0;
    if (objOop.isObject()) {
        // Get the actual memory address of the object header
        address = reinterpret_cast<uintptr_t>(objOop.asObjectPtr());
    } else if (objOop.isSmallInteger()) {
        // For small integers, the value IS the address (useful for external pointers)
        address = static_cast<uintptr_t>(objOop.asSmallInteger());
    }

    Oop result = uint64ToOop(memory_, address);
    if (result.isNil()) return PrimitiveResult::Failure;
    pop();
    push(result);
    return PrimitiveResult::Success;
}

// ===== FILE I/O PRIMITIVES =====

// Primitive 90: Test if at end of file
// fileHandle primitiveFileAtEnd -> boolean
PrimitiveResult Interpreter::primitiveFileAtEnd(int argCount) {
    Oop fileIdOop = stackTop();

    if (!fileIdOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int fileId = static_cast<int>(fileIdOop.asSmallInteger());
    auto it = openFiles_.find(fileId);
    if (it == openFiles_.end()) {
        return PrimitiveResult::Failure;
    }

    FILE* file = it->second;
    bool atEnd = (feof(file) != 0);

    // If not at EOF, check if next read would hit EOF
    if (!atEnd) {
        int c = fgetc(file);
        if (c == EOF) {
            atEnd = true;
        } else {
            ungetc(c, file);
        }
    }

    pop();
    push(atEnd ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

// Primitive 91: Close a file
// fileHandle primitiveFileClose -> receiver
PrimitiveResult Interpreter::primitiveFileClose(int argCount) {
    Oop fileIdOop = stackTop();

    if (!fileIdOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int fileId = static_cast<int>(fileIdOop.asSmallInteger());
    auto it = openFiles_.find(fileId);
    if (it == openFiles_.end()) {
        return PrimitiveResult::Failure;
    }

    fclose(it->second);
    openFiles_.erase(it);

    // Return receiver (leave stack as-is, receiver is below arg)
    return PrimitiveResult::Success;
}

// Primitive 92: Get current position in file
// fileHandle primitiveFileGetPosition -> position
PrimitiveResult Interpreter::primitiveFileGetPosition(int argCount) {
    Oop fileIdOop = stackTop();

    if (!fileIdOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int fileId = static_cast<int>(fileIdOop.asSmallInteger());
    auto it = openFiles_.find(fileId);
    if (it == openFiles_.end()) {
        return PrimitiveResult::Failure;
    }

    long pos = ftell(it->second);
    if (pos < 0) {
        return PrimitiveResult::Failure;
    }

    pop();
    push(int64ToOop(memory_, static_cast<int64_t>(pos)));
    return PrimitiveResult::Success;
}

// Primitive 93: Open a file
// filename writable primitiveFileOpen -> fileHandle (or nil on failure)
PrimitiveResult Interpreter::primitiveFileOpen(int argCount) {
    if (argCount < 2) {
        return PrimitiveResult::Failure;
    }

    Oop writableOop = stackValue(0);  // writable flag
    Oop filenameOop = stackValue(1);  // filename string

    // Extract filename
    std::string filename = extractString(memory_, filenameOop);
    if (filename.empty()) {
        return PrimitiveResult::Failure;
    }

    // Determine mode based on writable flag
    bool writable = (writableOop == memory_.trueObject());
    const char* mode = writable ? "r+b" : "rb";

    FILE* file = fopen(filename.c_str(), mode);

    // If opening for write failed, try creating the file
    if (!file && writable) {
        file = fopen(filename.c_str(), "w+b");
    }

    if (!file) {
        // Return nil on failure
        popN(argCount);
        push(memory_.nil());
        return PrimitiveResult::Success;
    }

    // Assign a file ID and store the handle
    int fileId = nextFileId_++;
    openFiles_[fileId] = file;

    popN(argCount);
    push(Oop::fromSmallInteger(fileId));
    return PrimitiveResult::Success;
}

// Primitive 94: Read from file
// fileHandle buffer startIndex count primitiveFileRead -> bytesRead
PrimitiveResult Interpreter::primitiveFileRead(int argCount) {
    if (argCount < 4) {
        return PrimitiveResult::Failure;
    }

    Oop countOop = stackValue(0);
    Oop startOop = stackValue(1);
    Oop bufferOop = stackValue(2);
    Oop fileIdOop = stackValue(3);

    if (!fileIdOop.isSmallInteger() || !startOop.isSmallInteger() || !countOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int fileId = static_cast<int>(fileIdOop.asSmallInteger());
    int64_t start = startOop.asSmallInteger();  // 1-based in Smalltalk
    int64_t count = countOop.asSmallInteger();

    if (start < 1 || count < 0) {
        return PrimitiveResult::Failure;
    }

    auto it = openFiles_.find(fileId);
    if (it == openFiles_.end()) {
        return PrimitiveResult::Failure;
    }

    // Buffer must be a byte object
    if (!bufferOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    size_t bufferSize = memory_.byteSizeOf(bufferOop);
    if (static_cast<size_t>(start - 1 + count) > bufferSize) {
        return PrimitiveResult::Failure;
    }

    // Read into a temporary buffer
    std::vector<uint8_t> tempBuffer(count);
    size_t bytesRead = fread(tempBuffer.data(), 1, count, it->second);

    // Copy to the Smalltalk buffer
    for (size_t i = 0; i < bytesRead; i++) {
        memory_.storeByte(start - 1 + i, bufferOop, tempBuffer[i]);
    }

    popN(argCount);
    push(Oop::fromSmallInteger(static_cast<int64_t>(bytesRead)));
    return PrimitiveResult::Success;
}

// Primitive 95: Set position in file
// fileHandle position primitiveFileSetPosition -> receiver
PrimitiveResult Interpreter::primitiveFileSetPosition(int argCount) {
    if (argCount < 2) {
        return PrimitiveResult::Failure;
    }

    Oop posOop = stackValue(0);
    Oop fileIdOop = stackValue(1);

    if (!fileIdOop.isSmallInteger() || !posOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int fileId = static_cast<int>(fileIdOop.asSmallInteger());
    int64_t pos = posOop.asSmallInteger();

    auto it = openFiles_.find(fileId);
    if (it == openFiles_.end()) {
        return PrimitiveResult::Failure;
    }

    if (fseek(it->second, static_cast<long>(pos), SEEK_SET) != 0) {
        return PrimitiveResult::Failure;
    }

    popN(argCount);
    push(fileIdOop);  // Return the file handle
    return PrimitiveResult::Success;
}

// Primitive 96: Delete a file
// filename primitiveFileDelete -> boolean
PrimitiveResult Interpreter::primitiveFileDelete(int argCount) {
    Oop filenameOop = stackTop();

    std::string filename = extractString(memory_, filenameOop);
    if (filename.empty()) {
        return PrimitiveResult::Failure;
    }

    int result = remove(filename.c_str());

    pop();
    push(result == 0 ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

// Primitive 97: Get file size
// fileHandle primitiveFileSize -> size
PrimitiveResult Interpreter::primitiveFileSize(int argCount) {
    Oop fileIdOop = stackTop();

    if (!fileIdOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int fileId = static_cast<int>(fileIdOop.asSmallInteger());
    auto it = openFiles_.find(fileId);
    if (it == openFiles_.end()) {
        return PrimitiveResult::Failure;
    }

    FILE* file = it->second;

    // Save current position
    long currentPos = ftell(file);
    if (currentPos < 0) {
        return PrimitiveResult::Failure;
    }

    // Seek to end
    if (fseek(file, 0, SEEK_END) != 0) {
        return PrimitiveResult::Failure;
    }

    long size = ftell(file);

    // Restore position
    fseek(file, currentPos, SEEK_SET);

    if (size < 0) {
        return PrimitiveResult::Failure;
    }

    pop();
    push(int64ToOop(memory_, static_cast<int64_t>(size)));
    return PrimitiveResult::Success;
}

// Primitive 98: Write to file
// fileHandle buffer startIndex count primitiveFileWrite -> bytesWritten
PrimitiveResult Interpreter::primitiveFileWrite(int argCount) {
    if (argCount < 4) {
        return PrimitiveResult::Failure;
    }

    Oop countOop = stackValue(0);
    Oop startOop = stackValue(1);
    Oop bufferOop = stackValue(2);
    Oop fileIdOop = stackValue(3);

    if (!fileIdOop.isSmallInteger() || !startOop.isSmallInteger() || !countOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int fileId = static_cast<int>(fileIdOop.asSmallInteger());
    int64_t start = startOop.asSmallInteger();  // 1-based in Smalltalk
    int64_t count = countOop.asSmallInteger();

    if (start < 1 || count < 0) {
        return PrimitiveResult::Failure;
    }

    auto it = openFiles_.find(fileId);
    if (it == openFiles_.end()) {
        return PrimitiveResult::Failure;
    }

    // Buffer must be a byte object
    if (!bufferOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    size_t bufferSize = memory_.byteSizeOf(bufferOop);
    if (static_cast<size_t>(start - 1 + count) > bufferSize) {
        return PrimitiveResult::Failure;
    }

    // Copy from Smalltalk buffer to temp buffer
    std::vector<uint8_t> tempBuffer(count);
    for (int64_t i = 0; i < count; i++) {
        tempBuffer[i] = memory_.fetchByte(start - 1 + i, bufferOop);
    }

    size_t bytesWritten = fwrite(tempBuffer.data(), 1, count, it->second);
    fflush(it->second);

    popN(argCount);
    push(Oop::fromSmallInteger(static_cast<int64_t>(bytesWritten)));
    return PrimitiveResult::Success;
}

// Primitive 99: Rename a file
// oldName newName primitiveFileRename -> boolean
PrimitiveResult Interpreter::primitiveFileRename(int argCount) {
    if (argCount < 2) {
        return PrimitiveResult::Failure;
    }

    Oop newNameOop = stackValue(0);
    Oop oldNameOop = stackValue(1);

    std::string oldName = extractString(memory_, oldNameOop);
    std::string newName = extractString(memory_, newNameOop);

    if (oldName.empty() || newName.empty()) {
        return PrimitiveResult::Failure;
    }

    int result = rename(oldName.c_str(), newName.c_str());

    popN(argCount);
    push(result == 0 ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

// ===== DIRECTORY PRIMITIVES =====

// Primitive 122: Create a directory
// pathString primitiveDirectoryCreate -> boolean
PrimitiveResult Interpreter::primitiveDirectoryCreate(int argCount) {
    Oop pathOop = stackTop();

    std::string path = extractString(memory_, pathOop);
    if (path.empty()) {
        return PrimitiveResult::Failure;
    }

    // Create directory with rwxr-xr-x permissions
    int result = mkdir(path.c_str(), 0755);

    pop();
    push(result == 0 ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

// Primitive 123: Get directory delimiter character
// primitiveDirectoryDelimitor -> Character
PrimitiveResult Interpreter::primitiveDirectoryDelimitor(int argCount) {
    pop();  // pop receiver
    push(Oop::fromCharacter('/'));
    return PrimitiveResult::Success;
}

// Primitive 124: Look up directory entry
// pathString index primitiveDirectoryLookup -> Array or nil
// Returns an Array with: (name, creationTime, modificationTime, dirFlag, fileSize)
// or nil if index is out of range
PrimitiveResult Interpreter::primitiveDirectoryLookup(int argCount) {
    if (argCount < 2) {
        return PrimitiveResult::Failure;
    }

    Oop indexOop = stackValue(0);
    Oop pathOop = stackValue(1);

    if (!indexOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    std::string path = extractString(memory_, pathOop);
    if (path.empty()) {
        return PrimitiveResult::Failure;
    }

    int64_t targetIndex = indexOop.asSmallInteger();
    if (targetIndex < 1) {
        return PrimitiveResult::Failure;
    }

    DIR* dir = opendir(path.c_str());
    if (!dir) {
        // Directory doesn't exist or can't be opened
        popN(argCount);
        push(memory_.nil());
        return PrimitiveResult::Success;
    }

    // Skip to the target entry (1-based index)
    struct dirent* entry = nullptr;
    int64_t currentIndex = 0;
    while ((entry = readdir(dir)) != nullptr) {
        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        currentIndex++;
        if (currentIndex == targetIndex) {
            break;
        }
    }

    if (!entry) {
        // Index out of range
        closedir(dir);
        popN(argCount);
        push(memory_.nil());
        return PrimitiveResult::Success;
    }

    // Get file info
    std::string fullPath = path;
    if (!fullPath.empty() && fullPath.back() != '/') {
        fullPath += '/';
    }
    fullPath += entry->d_name;

    struct stat statBuf;
    bool isDir = false;
    int64_t fileSize = 0;
    int64_t modTime = 0;
    int64_t createTime = 0;

    if (stat(fullPath.c_str(), &statBuf) == 0) {
        isDir = S_ISDIR(statBuf.st_mode);
        fileSize = statBuf.st_size;
        modTime = statBuf.st_mtime;
#ifdef __APPLE__
        createTime = statBuf.st_birthtime;
#else
        createTime = statBuf.st_ctime;  // Use ctime as fallback on Linux
#endif
    }

    closedir(dir);

    // Create the result array with 5 elements:
    // 1: name (String)
    // 2: creation time (seconds since epoch)
    // 3: modification time (seconds since epoch)
    // 4: isDirectory (boolean)
    // 5: file size (integer)

    Oop arrayClass = memory_.specialObject(SpecialObjectIndex::ClassArray);
    if (arrayClass.isNil()) {
        return PrimitiveResult::Failure;
    }

    uint32_t arrayClassIndex = memory_.indexOfClass(arrayClass);
    Oop resultArray = memory_.allocateSlots(arrayClassIndex, 5, ObjectFormat::Indexable);
    if (resultArray.isNil()) {
        return PrimitiveResult::Failure;
    }

    // Create name string
    Oop nameString = createStringObject(memory_, entry->d_name);
    if (nameString.isNil()) {
        return PrimitiveResult::Failure;
    }

    // Store the results
    memory_.storePointer(0, resultArray, nameString);
    memory_.storePointer(1, resultArray, Oop::fromSmallInteger(createTime));
    memory_.storePointer(2, resultArray, Oop::fromSmallInteger(modTime));
    memory_.storePointer(3, resultArray, isDir ? memory_.trueObject() : memory_.falseObject());
    memory_.storePointer(4, resultArray, Oop::fromSmallInteger(fileSize));

    popN(argCount);
    push(resultArray);
    return PrimitiveResult::Success;
}

// Primitive 126: Delete a directory
// pathString primitiveDirectoryDelete -> boolean
PrimitiveResult Interpreter::primitiveDirectoryDelete(int argCount) {
    Oop pathOop = stackTop();

    std::string path = extractString(memory_, pathOop);
    if (path.empty()) {
        return PrimitiveResult::Failure;
    }

    // rmdir only works on empty directories
    int result = rmdir(path.c_str());

    pop();
    push(result == 0 ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

// Primitive 127: Get Mac file type and creator (legacy, returns empty on non-Mac or modern systems)
// pathString primitiveDirectoryGetMacTypeAndCreator -> Array of (type, creator) or nil
PrimitiveResult Interpreter::primitiveDirectoryGetMacTypeAndCreator(int argCount) {
    // On modern systems, Mac type/creator codes are not commonly used
    // Return an array with empty strings for compatibility
    Oop pathOop = stackTop();

    std::string path = extractString(memory_, pathOop);
    if (path.empty()) {
        return PrimitiveResult::Failure;
    }

    // Check if path exists
    struct stat statBuf;
    if (stat(path.c_str(), &statBuf) != 0) {
        pop();
        push(memory_.nil());
        return PrimitiveResult::Success;
    }

    // Create result array with 2 empty strings (type, creator)
    Oop arrayClass = memory_.specialObject(SpecialObjectIndex::ClassArray);
    if (arrayClass.isNil()) {
        return PrimitiveResult::Failure;
    }

    uint32_t arrayClassIndex = memory_.indexOfClass(arrayClass);
    Oop resultArray = memory_.allocateSlots(arrayClassIndex, 2, ObjectFormat::Indexable);
    if (resultArray.isNil()) {
        return PrimitiveResult::Failure;
    }

    // Create empty strings for type and creator
    Oop emptyType = createStringObject(memory_, "");
    Oop emptyCreator = createStringObject(memory_, "");

    memory_.storePointer(0, resultArray, emptyType);
    memory_.storePointer(1, resultArray, emptyCreator);

    pop();
    push(resultArray);
    return PrimitiveResult::Success;
}

// primitiveGetCurrentWorkingDirectory - named primitive (no module)
// Takes 1 arg: a ByteArray buffer. Fills it with getcwd() result and returns a String.
PrimitiveResult Interpreter::primitiveGetCurrentWorkingDirectory(int argCount) {
    char buf[1024];
    if (!getcwd(buf, sizeof(buf))) {
        return PrimitiveResult::Failure;
    }

    Oop result = createStringObject(memory_, std::string(buf));
    if (result.isNil()) {
        return PrimitiveResult::Failure;
    }

    // Pop args + receiver, push result
    popN(static_cast<size_t>(argCount + 1));
    push(result);
    return PrimitiveResult::Success;
}

// ===== ADDITIONAL FILE PRIMITIVES =====

// Primitive 161: Get standard I/O file handles
// primitiveFileStdioHandles -> Array of (stdin, stdout, stderr) handles
PrimitiveResult Interpreter::primitiveFileStdioHandles(int argCount) {
    // Register stdin, stdout, stderr if not already registered
    // Use actual POSIX fd numbers so primitiveFileDescriptorType can fstat() them
    static bool stdioInitialized = false;
    static int stdinId = 0;
    static int stdoutId = 1;
    static int stderrId = 2;

    if (!stdioInitialized) {
        openFiles_[0] = stdin;
        openFiles_[1] = stdout;
        openFiles_[2] = stderr;
        stdioInitialized = true;
    }

    // Create result array with 3 elements
    Oop arrayClass = memory_.specialObject(SpecialObjectIndex::ClassArray);
    if (arrayClass.isNil()) {
        return PrimitiveResult::Failure;
    }

    uint32_t arrayClassIndex = memory_.indexOfClass(arrayClass);
    Oop resultArray = memory_.allocateSlots(arrayClassIndex, 3, ObjectFormat::Indexable);
    if (resultArray.isNil()) {
        return PrimitiveResult::Failure;
    }

    memory_.storePointer(0, resultArray, Oop::fromSmallInteger(stdinId));
    memory_.storePointer(1, resultArray, Oop::fromSmallInteger(stdoutId));
    memory_.storePointer(2, resultArray, Oop::fromSmallInteger(stderrId));

    pop();  // pop receiver
    push(resultArray);
    return PrimitiveResult::Success;
}

// Primitive 162: Get file descriptor type
// fileHandle primitiveFileDescriptorType -> integer
// Returns values matching the Cog VM's sqFileDescriptorType():
//   0 = regular file, 1 = pipe/FIFO, 2 = socket, 3 = character device, -1 = unknown/invalid
// The Pharo image checks (type between: 1 and: 3) for "available" stdio handles.
PrimitiveResult Interpreter::primitiveFileDescriptorType(int argCount) {
    Oop fileIdOop = stackTop();

    if (!fileIdOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int fd = static_cast<int>(fileIdOop.asSmallInteger());

    struct stat statBuf;
    if (fstat(fd, &statBuf) != 0) {
        pop();
        push(Oop::fromSmallInteger(-1));
        return PrimitiveResult::Success;
    }

    // Cog VM's sqFileDescriptorType() numbering:
    //   0 = no console (Windows-specific)
    //   1 = terminal (character device)
    //   2 = pipe/FIFO
    //   3 = regular file
    //   4 = Cygwin terminal (Windows-specific)
    //  -1 = unknown/invalid
    // Pharo's fileDescriptorIsAvailable: checks (type between: 1 and: 3)
    int type = -1;
    if (S_ISCHR(statBuf.st_mode)) type = 1;          // Terminal/character device
    else if (S_ISFIFO(statBuf.st_mode)) type = 2;    // Pipe/FIFO
#ifdef S_ISSOCK
    else if (S_ISSOCK(statBuf.st_mode)) type = 2;    // Socket (treat as pipe)
#endif
    else if (S_ISREG(statBuf.st_mode)) type = 3;     // Regular file

    pop();
    push(Oop::fromSmallInteger(type));
    return PrimitiveResult::Success;
}

// Primitive 163: Flush file buffer
// fileHandle primitiveFileFlush -> fileHandle
PrimitiveResult Interpreter::primitiveFileFlush(int argCount) {
    Oop fileIdOop = stackTop();

    if (!fileIdOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int fileId = static_cast<int>(fileIdOop.asSmallInteger());
    auto it = openFiles_.find(fileId);
    if (it == openFiles_.end()) {
        return PrimitiveResult::Failure;
    }

    fflush(it->second);

    // Return the file handle (leave stack unchanged)
    return PrimitiveResult::Success;
}

// Primitive 164: Truncate file to given size
// fileHandle newSize primitiveFileTruncate -> fileHandle
PrimitiveResult Interpreter::primitiveFileTruncate(int argCount) {
    if (argCount < 2) {
        return PrimitiveResult::Failure;
    }

    Oop sizeOop = stackValue(0);
    Oop fileIdOop = stackValue(1);

    if (!fileIdOop.isSmallInteger() || !sizeOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int fileId = static_cast<int>(fileIdOop.asSmallInteger());
    int64_t newSize = sizeOop.asSmallInteger();

    if (newSize < 0) {
        return PrimitiveResult::Failure;
    }

    auto it = openFiles_.find(fileId);
    if (it == openFiles_.end()) {
        return PrimitiveResult::Failure;
    }

    FILE* file = it->second;
    int fd = fileno(file);

    // Flush before truncating
    fflush(file);

    if (ftruncate(fd, static_cast<off_t>(newSize)) != 0) {
        return PrimitiveResult::Failure;
    }

    popN(argCount);
    push(fileIdOop);  // Return the file handle
    return PrimitiveResult::Success;
}

// ===== SYSTEM PRIMITIVES (152-155) =====

// Primitive 152: Set or query full screen mode
// bool primitiveSetFullScreen -> self (sets mode)
// primitiveSetFullScreen -> bool (queries mode)
PrimitiveResult Interpreter::primitiveSetFullScreen(int argCount) {
    static bool isFullScreen = false;

    if (argCount == 1) {
        // Set full screen mode
        Oop arg = stackTop();
        isFullScreen = (arg == memory_.trueObject());
        pop();  // pop argument, leave receiver
        return PrimitiveResult::Success;
    } else {
        // Query full screen mode
        pop();
        push(isFullScreen ? memory_.trueObject() : memory_.falseObject());
        return PrimitiveResult::Success;
    }
}

// Primitive 153: Set the input semaphore
// semaphore primitiveInputSemaphore -> self
PrimitiveResult Interpreter::primitiveInputSemaphore(int argCount) {
    if (argCount < 1) {
        return PrimitiveResult::Failure;
    }

    // Get the semaphore argument - this is the semaphore index
    Oop semArg = stackTop();
    if (semArg.isSmallInteger()) {
        int64_t semIndex = semArg.asSmallInteger();
        gEventQueue.setInputSemaphoreIndex(static_cast<int>(semIndex));
    }

    pop();  // pop semaphore, leave receiver
    return PrimitiveResult::Success;
}

// Primitive 154: Get raw input word (for low-level input handling)
// primitiveInputWord -> integer
PrimitiveResult Interpreter::primitiveInputWord(int argCount) {
    // In headless mode, return 0 (no input)
    // A full implementation would return encoded keyboard/mouse state
    pop();
    push(Oop::fromSmallInteger(0));
    return PrimitiveResult::Success;
}

// Primitive 155: Compare two strings (case-sensitive byte comparison)
// string1 string2 primitiveCompareString -> integer
// Returns: -1 if string1 < string2, 0 if equal, 1 if string1 > string2
PrimitiveResult Interpreter::primitiveCompareString(int argCount) {
    if (argCount < 2) {
        return PrimitiveResult::Failure;
    }

    Oop string2Oop = stackValue(0);
    Oop string1Oop = stackValue(1);

    if (!string1Oop.isObject() || !string2Oop.isObject()) {
        return PrimitiveResult::Failure;
    }

    size_t len1 = memory_.byteSizeOf(string1Oop);
    size_t len2 = memory_.byteSizeOf(string2Oop);
    size_t minLen = (len1 < len2) ? len1 : len2;

    int result = 0;
    for (size_t i = 0; i < minLen; i++) {
        uint8_t c1 = memory_.fetchByte(i, string1Oop);
        uint8_t c2 = memory_.fetchByte(i, string2Oop);
        if (c1 < c2) {
            result = -1;
            break;
        } else if (c1 > c2) {
            result = 1;
            break;
        }
    }

    // If all compared bytes are equal, shorter string is "less"
    if (result == 0) {
        if (len1 < len2) {
            result = -1;
        } else if (len1 > len2) {
            result = 1;
        }
    }

    popN(argCount + 1);  // pop args + receiver
    push(Oop::fromSmallInteger(result));
    return PrimitiveResult::Success;
}

// ===== STRING PRIMITIVES (157-158) =====

// Helper: Convert character to lowercase for case-insensitive comparison
static inline uint8_t toLower(uint8_t c) {
    if (c >= 'A' && c <= 'Z') {
        return c + ('a' - 'A');
    }
    return c;
}

// Primitive 157: Compare strings with collation order
// string1 string2 orderTable primitiveCompareStringCollated -> integer
// orderTable is a 256-byte array mapping characters to their sort order
// Returns: 1 if string1 < string2, 2 if equal, 3 if string1 > string2
PrimitiveResult Interpreter::primitiveCompareStringCollated(int argCount) {
    if (argCount < 3) {
        return PrimitiveResult::Failure;
    }

    Oop orderTableOop = stackValue(0);
    Oop string2Oop = stackValue(1);
    Oop string1Oop = stackValue(2);

    if (!string1Oop.isObject() || !string2Oop.isObject() || !orderTableOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    size_t len1 = memory_.byteSizeOf(string1Oop);
    size_t len2 = memory_.byteSizeOf(string2Oop);
    size_t orderTableSize = memory_.byteSizeOf(orderTableOop);

    // Order table should have 256 entries
    if (orderTableSize < 256) {
        return PrimitiveResult::Failure;
    }

    size_t minLen = (len1 < len2) ? len1 : len2;

    int result = 2;  // Default: equal (using Smalltalk convention: 1=less, 2=equal, 3=greater)
    for (size_t i = 0; i < minLen; i++) {
        uint8_t c1 = memory_.fetchByte(i, string1Oop);
        uint8_t c2 = memory_.fetchByte(i, string2Oop);

        // Look up collation order
        uint8_t order1 = memory_.fetchByte(c1, orderTableOop);
        uint8_t order2 = memory_.fetchByte(c2, orderTableOop);

        if (order1 < order2) {
            result = 1;  // string1 < string2
            break;
        } else if (order1 > order2) {
            result = 3;  // string1 > string2
            break;
        }
    }

    // If all compared bytes are equal, shorter string is "less"
    if (result == 2) {
        if (len1 < len2) {
            result = 1;
        } else if (len1 > len2) {
            result = 3;
        }
    }

    popN(argCount + 1);  // pop args + receiver
    push(Oop::fromSmallInteger(result));
    return PrimitiveResult::Success;
}

// Primitive 158: Compare strings case-insensitively
// string1 string2 primitiveCompareStringNoCase -> integer
// Returns: 1 if string1 < string2, 2 if equal, 3 if string1 > string2
PrimitiveResult Interpreter::primitiveCompareStringNoCase(int argCount) {
    if (argCount < 2) {
        return PrimitiveResult::Failure;
    }

    Oop string2Oop = stackValue(0);
    Oop string1Oop = stackValue(1);

    if (!string1Oop.isObject() || !string2Oop.isObject()) {
        return PrimitiveResult::Failure;
    }

    size_t len1 = memory_.byteSizeOf(string1Oop);
    size_t len2 = memory_.byteSizeOf(string2Oop);
    size_t minLen = (len1 < len2) ? len1 : len2;

    int result = 2;  // Default: equal
    for (size_t i = 0; i < minLen; i++) {
        uint8_t c1 = toLower(memory_.fetchByte(i, string1Oop));
        uint8_t c2 = toLower(memory_.fetchByte(i, string2Oop));

        if (c1 < c2) {
            result = 1;  // string1 < string2
            break;
        } else if (c1 > c2) {
            result = 3;  // string1 > string2
            break;
        }
    }

    // If all compared bytes are equal, shorter string is "less"
    if (result == 2) {
        if (len1 < len2) {
            result = 1;
        } else if (len1 > len2) {
            result = 3;
        }
    }

    popN(argCount + 1);  // pop args + receiver
    push(Oop::fromSmallInteger(result));
    return PrimitiveResult::Success;
}

// ===== BECOME PRIMITIVES (197-198) =====

// Primitive 197: One-way become for arrays
// fromArray toArray primitiveArrayBecomeOneWay -> fromArray
// All references to objects in fromArray become references to corresponding objects in toArray
PrimitiveResult Interpreter::primitiveArrayBecomeOneWay(int argCount) {
    // Primitive 72: One-way become with hash copying
    // receiver elementsForwardIdentityTo: anotherArray
    // Per official VM: 1 argument, twoWay: false, copyHash: true
    if (argCount != 1) {
        return PrimitiveResult::Failure;
    }

    Oop toArrayOop = stackValue(0);
    Oop fromArrayOop = stackValue(1);

    if (!fromArrayOop.isObject() || !toArrayOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* fromHdr = fromArrayOop.asObjectPtr();
    ObjectHeader* toHdr = toArrayOop.asObjectPtr();

    // Must be indexable pointer objects (Arrays)
    if (fromHdr->format() != ObjectFormat::Indexable ||
        toHdr->format() != ObjectFormat::Indexable) {
        return PrimitiveResult::Failure;
    }

    size_t fromSize = fromHdr->slotCount();
    size_t toSize = toHdr->slotCount();

    // Arrays must be the same size
    if (fromSize != toSize) {
        return PrimitiveResult::Failure;
    }

    // Perform one-way become for each pair, copying hash (per official VM)
    for (size_t i = 0; i < fromSize; i++) {
        Oop fromObj = memory_.fetchPointer(i, fromArrayOop);
        Oop toObj = memory_.fetchPointer(i, toArrayOop);

        // Skip if either is an immediate or nil
        if (!fromObj.isObject() || fromObj.isNil()) {
            continue;
        }
        if (!toObj.isObject()) {
            continue;
        }

        // Copy identity hash from source to target (per official VM copyHash: true)
        ObjectHeader* fromObjHdr = fromObj.asObjectPtr();
        ObjectHeader* toObjHdr = toObj.asObjectPtr();
        if (fromObjHdr->hasIdentityHash()) {
            toObjHdr->setIdentityHash(fromObjHdr->identityHash());
        }

        // Perform one-way become: all references to fromObj become toObj
        memory_.becomeForward(fromObj, toObj);

        // Also scan C++ execution stack - critical for stack-based VM
        // Without this, local variables (temps) on the stack still point to old objects
        scanStackReplace(fromObj, toObj);
    }

    // Flush method cache (critical after become)
    flushMethodCache();

    popN(1);  // Pop argument, leave receiver
    return PrimitiveResult::Success;
}

// Primitive 198: One-way become with hash copying
// receiver elementsForwardIdentityTo: toArray -> receiver  (1 arg, copyHash=false)
// receiver elementsForwardIdentityTo: toArray copyHash: bool -> receiver (2 args)
// Like 197, but optionally copies identity hash from source to target
PrimitiveResult Interpreter::primitiveArrayBecomeOneWayCopyHash(int argCount) {
    if (argCount < 1) {
        return PrimitiveResult::Failure;
    }

    bool copyHash = false;
    Oop toArrayOop;
    Oop fromArrayOop;

    if (argCount >= 2) {
        // 2-argument form: receiver elementsForwardIdentityTo: toArray copyHash: bool
        Oop copyHashOop = stackValue(0);
        toArrayOop = stackValue(1);
        fromArrayOop = stackValue(2);
        copyHash = (copyHashOop == memory_.trueObject());
    } else {
        // 1-argument form: receiver elementsForwardIdentityTo: toArray
        toArrayOop = stackValue(0);
        fromArrayOop = stackValue(1);
        copyHash = false;
    }

    if (!fromArrayOop.isObject() || !toArrayOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    size_t fromSize = memory_.slotCountOf(fromArrayOop);
    size_t toSize = memory_.slotCountOf(toArrayOop);

    // Arrays must be the same size
    if (fromSize != toSize) {
        return PrimitiveResult::Failure;
    }

    // Perform one-way become for each pair
    for (size_t i = 0; i < fromSize; i++) {
        Oop fromObj = memory_.fetchPointer(i, fromArrayOop);
        Oop toObj = memory_.fetchPointer(i, toArrayOop);

        // Skip if either is an immediate or nil
        if (!fromObj.isObject() || fromObj.isNil()) {
            continue;
        }
        if (!toObj.isObject() || toObj.isNil()) {
            continue;
        }

        // Copy identity hash from source to target if requested
        if (copyHash) {
            memory_.ensureIdentityHash(fromObj);
            uint32_t hash = fromObj.asObjectPtr()->identityHash();
            toObj.asObjectPtr()->setIdentityHash(hash);
        }

        // Perform one-way become: all references to fromObj become toObj
        memory_.becomeForward(fromObj, toObj);

        // Also scan C++ execution stack - critical for stack-based VM
        scanStackReplace(fromObj, toObj);
    }

    // Flush method cache (critical after become)
    flushMethodCache();

    popN(argCount);
    push(fromArrayOop);
    return PrimitiveResult::Success;
}

// Primitive 248: One-way become without hash copying
// receiver elementsForwardIdentityTo: toArray -> receiver
// Like primitiveArrayBecomeOneWayCopyHash but never copies identity hash
PrimitiveResult Interpreter::primitiveArrayBecomeOneWayNoCopyHash(int argCount) {
    if (argCount != 1) {
        return PrimitiveResult::Failure;
    }

    Oop toArrayOop = stackValue(0);
    Oop fromArrayOop = stackValue(1);

    if (!fromArrayOop.isObject() || !toArrayOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    size_t fromSize = memory_.slotCountOf(fromArrayOop);
    size_t toSize = memory_.slotCountOf(toArrayOop);

    // Arrays must be the same size
    if (fromSize != toSize) {
        return PrimitiveResult::Failure;
    }

    // Perform one-way become for each pair (without copying hash)
    for (size_t i = 0; i < fromSize; i++) {
        Oop fromObj = memory_.fetchPointer(i, fromArrayOop);
        Oop toObj = memory_.fetchPointer(i, toArrayOop);

        // Skip if either is an immediate or nil
        if (!fromObj.isObject() || fromObj.isNil()) {
            continue;
        }
        if (!toObj.isObject() || toObj.isNil()) {
            continue;
        }

        // Do NOT copy identity hash (that's the difference from primitive 249)

        // Perform one-way become: all references to fromObj become toObj
        memory_.becomeForward(fromObj, toObj);

        // Also scan C++ execution stack - critical for stack-based VM
        scanStackReplace(fromObj, toObj);
    }

    // Flush method cache (critical after become)
    flushMethodCache();

    popN(1);  // Pop argument, leave receiver
    return PrimitiveResult::Success;
}

// ===== CONTEXT PRIMITIVE (203) =====

// Primitive 203: Evaluate block value uninterruptably
// block primitiveValueUninterruptably -> result
// Evaluates the block without allowing process switches
// In our cooperative/single-threaded VM, this is essentially the same as value
PrimitiveResult Interpreter::primitiveValueUninterruptably(int argCount) {
    // Get the block closure (receiver)
    Oop blockOop = stackTop();

    if (!blockOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Check that it's a block closure (has numArgs field)
    // BlockClosure layout: outerContext, startpc/compiledBlock, numArgs, ...
    size_t slotCount = memory_.slotCountOf(blockOop);
    if (slotCount < 3) {
        return PrimitiveResult::Failure;
    }

    // Get numArgs - block should take 0 arguments for valueUninterruptably
    Oop numArgsOop = memory_.fetchPointer(2, blockOop);
    if (!numArgsOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t numArgs = numArgsOop.asSmallInteger();
    if (numArgs != 0) {
        // Block requires arguments, fail to let Smalltalk handle it
        return PrimitiveResult::Failure;
    }

    // In a cooperative VM without preemptive scheduling, valueUninterruptably
    // is essentially the same as value. We fail the primitive to let the
    // Smalltalk fallback code handle the block evaluation, which will:
    // 1. Disable process switching
    // 2. Evaluate the block with value
    // 3. Re-enable process switching
    // This is the standard pattern - the primitive validates and then fails
    // to trigger the Smalltalk implementation.
    return PrimitiveResult::Failure;
}

// ===== PROCESS/SYSTEM PRIMITIVES (172, 179) =====

// Primitive 172: Set the GC semaphore (or finalization semaphore)
// semaphore primitiveSetGCSemaphore -> semaphore
// Sets the semaphore to be signaled when GC occurs or finalization is needed
PrimitiveResult Interpreter::primitiveSetGCSemaphore(int argCount) {
    if (argCount < 1) {
        return PrimitiveResult::Failure;
    }

    Oop semaphoreOop = stackTop();

    // The semaphore can be nil (to disable) or a Semaphore object
    // Store it for later use when GC signals finalization
    // In a full implementation, this would be stored and signaled during GC
    // For now, we just accept and acknowledge the setting

    // Could store in: memory_.setSpecialObject(SpecialObjectIndex::TheFinalizationSemaphore, semaphoreOop);
    // But we don't have that index defined, so just accept it

    pop();  // pop argument, leave receiver
    return PrimitiveResult::Success;
}

// Primitive 179: Relinquish processor for given milliseconds
// milliseconds primitiveRelinquishProcessor -> self
// Allows other processes to run, sleeping for the specified time
PrimitiveResult Interpreter::primitiveRelinquishProcessor(int argCount) {
    if (argCount < 1) {
        return PrimitiveResult::Failure;
    }

    Oop microSecondsOop = stackTop();

    if (!microSecondsOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    // CRITICAL: Argument is in MICROSECONDS, not milliseconds!
    // Per official VM: ioRelinquishProcessorForMicroseconds(microSecs)
    int64_t microSeconds = microSecondsOop.asSmallInteger();

    // In a cooperative VM, relinquishing the processor means:
    // 1. Check for pending events/signals
    // 2. Optionally sleep for the requested time
    // 3. Allow process scheduler to run other processes

    // Cap sleep time to avoid blocking event loop (10ms = 10000 microseconds max)
    const int64_t MAX_SLEEP_US = 10000;  // 10ms in microseconds
    int64_t sleepUs = std::min(microSeconds, MAX_SLEEP_US);

    // Pop microseconds argument FIRST, before any process switch can happen.
    // processPendingSignals/checkTimerSemaphore can call transferTo() which
    // materializes and switches stacks. If we pop after, we'd corrupt the
    // new process's stack.
    pop();  // pop microseconds argument, leave receiver

    // Process any pending events first
    processInputEvents();
    processPendingSignals();

    // Short sleep if requested
    if (sleepUs > 0) {
        relinquishSlept_ = true;  // Signal to test harness that VM is idle
        if (relinquishCallback_) {
            // Use platform callback (e.g., CFRunLoopRunInMode on main thread)
            relinquishCallback_(static_cast<int>(sleepUs));
        } else {
            #ifdef _WIN32
            Sleep(static_cast<DWORD>(sleepUs / 1000));
            #else
            usleep(static_cast<useconds_t>(sleepUs));
            #endif
        }
    }

    // Process events again after sleep
    processInputEvents();
    processPendingSignals();
    checkTimerSemaphore();

    // Try to yield to higher priority ready processes
    // This is the key part that makes process switching work!
    Oop activeProcess = getActiveProcess();
    Oop activePriorityOop = memory_.fetchPointer(ProcessPriorityIndex, activeProcess);
    int activePriority = activePriorityOop.isSmallInteger() ?
                         static_cast<int>(activePriorityOop.asSmallInteger()) : 10;

    // Check scheduler for higher priority ready processes
    Oop nilObj = memory_.nil();
    Oop schedulerAssoc = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);
    if (schedulerAssoc.isObject() && schedulerAssoc.rawBits() != nilObj.rawBits()) {
        Oop scheduler = memory_.fetchPointer(1, schedulerAssoc);
        if (scheduler.isObject()) {
            Oop schedLists = memory_.fetchPointer(SchedulerProcessListsIndex, scheduler);
            if (schedLists.isObject()) {
                ObjectHeader* queuesHdr = schedLists.asObjectPtr();
                size_t numQueues = queuesHdr->slotCount();

                // Search from highest priority down to priority 1 (include ALL priorities)
                // relinquishProcessor means "give up CPU" - any runnable process should get a turn
                // NOTE: Priority is 1-based, but array indices are 0-based
                for (int pri = static_cast<int>(numQueues); pri >= 1; pri--) {
                    int index = pri - 1;  // Convert 1-based priority to 0-based index
                    Oop queue = memory_.fetchPointer(index, schedLists);
                    if (!queue.isObject() || queue.rawBits() == nilObj.rawBits()) continue;

                    Oop firstProcess = memory_.fetchPointer(LinkedListFirstLinkIndex, queue);
                    if (firstProcess.isObject() && firstProcess.rawBits() != nilObj.rawBits() &&
                        firstProcess.rawBits() != activeProcess.rawBits()) {
                        // Found a higher priority process - yield to it
                        // Remove the process from queue
                        Oop nextProcess = removeFirstLinkOfList(queue);
                        if (nextProcess.isObject() && nextProcess.rawBits() != nilObj.rawBits()) {
                            // Arg already popped above before signal processing.
                            // Put current process back in its queue
                            putToSleep(activeProcess);
                            // Switch to the new process
                            g_xferReason = "primRelinquish";
                            transferTo(nextProcess);
                            return PrimitiveResult::Success;  // Don't pop again below
                        }
                    }
                }
            }
        }
    }

    // Arg already popped above before signal processing
    return PrimitiveResult::Success;
}

// Primitive 231: Return the object format from header
// Used for object introspection
PrimitiveResult Interpreter::primitiveFormat(int argCount) {
    (void)argCount;
    Oop rcvr = stackTop();

    if (!rcvr.isObject()) {
        // Immediates have no object header, return 0 for SmallInteger
        pop();
        push(Oop::fromSmallInteger(0));
        return PrimitiveResult::Success;
    }

    ObjectHeader* hdr = rcvr.asObjectPtr();
    int format = static_cast<int>(hdr->format());

    pop();
    push(Oop::fromSmallInteger(format));
    return PrimitiveResult::Success;
}

// ===== DISPLAY PRIMITIVES (101-104) =====

// Primitive 101: Set the mouse cursor
// cursorForm maskForm primitiveBeCursor -> self
// In headless mode, this is a no-op
PrimitiveResult Interpreter::primitiveBeCursor(int argCount) {
    // In headless/iOS mode, cursor changes are handled by the platform
    // Just accept the arguments and return success
    if (argCount >= 1) {
        pop();  // pop mask or offset
    }
    if (argCount >= 2) {
        pop();  // pop cursor form
    }
    // Leave receiver on stack
    return PrimitiveResult::Success;
}

// Primitive 102: Make a Form the display
// Already implemented as primitiveBeDisplay in I/O section
// This just ensures the primitive table entry works

// Primitive 103: Scan characters for text rendering
// This is a complex primitive used by CharacterScanner for text layout
// string startIndex stopIndex stops destX charMap destX lastIndex
// Returns: stopIndex reached, or last index scanned
PrimitiveResult Interpreter::primitiveScanCharacters(int argCount) {
    // CharacterScanner uses this for efficient text scanning
    // In a minimal implementation, we fail to let Smalltalk handle it
    // A full implementation would scan characters and compute stop positions

    // The primitive is performance-critical but not essential for correctness
    // Failing causes fallback to Smalltalk code which is slower but works
    return PrimitiveResult::Failure;
}

// Primitive 104: BitBlt draw loop
// This primitive performs the actual BitBlt copy/combine operations
// In headless mode or without BitBlt plugin, fail to Smalltalk fallback
PrimitiveResult Interpreter::primitiveDrawLoop(int argCount) {
    // BitBlt drawLoop is used for efficient graphics operations
    // A full implementation would:
    // 1. Extract BitBlt parameters from receiver
    // 2. Perform the specified combination rule
    // 3. Copy pixels from source to destination

    // In headless mode, we can either:
    // a) Fail to let Smalltalk simulate it (slow but correct)
    // b) Succeed as no-op if no display

    // For now, fail to ensure correctness through Smalltalk simulation
    return PrimitiveResult::Failure;
}

// Primitive 107: Show a rectangle of the display
// left top right bottom primitiveShowDisplayRect -> self
// Updates the specified rectangle of the display
PrimitiveResult Interpreter::primitiveShowDisplayRect(int argCount) {
    // Allow Display-based rendering to coexist with SDL2 rendering.

    // Pop all arguments (left, top, right, bottom) but use them for partial update
    int left = 0, top = 0, right = 0, bottom = 0;
    if (argCount >= 4) {
        Oop bOop = stackTop();
        Oop rOop = stackValue(1);
        Oop tOop = stackValue(2);
        Oop lOop = stackValue(3);
        if (lOop.isSmallInteger()) left = lOop.asSmallInteger();
        if (tOop.isSmallInteger()) top = tOop.asSmallInteger();
        if (rOop.isSmallInteger()) right = rOop.asSmallInteger();
        if (bOop.isSmallInteger()) bottom = bOop.asSmallInteger();
        popN(4);
    } else if (argCount > 0) {
        popN(argCount);
    }

    if (!pharo::gDisplaySurface) return PrimitiveResult::Success;

    // Always refresh Display form (Pharo may change it, GC may move it)
    {
        Oop display = memory_.findGlobal("Display");
        if (!display.isNil() && display.isObject()) {
            displayForm_ = display;
        }
    }
    if (displayForm_.isNil() || !displayForm_.isObject()) return PrimitiveResult::Success;

    // Get Form fields: 0=bits, 1=width, 2=height, 3=depth
    Oop bits = memory_.fetchPointer(0, displayForm_);
    if (bits.isNil() || !bits.isObject()) return PrimitiveResult::Success;

    ObjectHeader* bitsHdr = bits.asObjectPtr();
    uint32_t* srcPixels = reinterpret_cast<uint32_t*>(bitsHdr->bytes());

    Oop widthOop = memory_.fetchPointer(1, displayForm_);
    Oop heightOop = memory_.fetchPointer(2, displayForm_);
    int srcWidth = widthOop.isSmallInteger() ? widthOop.asSmallInteger() : screenWidth_;
    int srcHeight = heightOop.isSmallInteger() ? heightOop.asSmallInteger() : screenHeight_;

    uint32_t* dstPixels = pharo::gDisplaySurface->pixels();
    int dstWidth = pharo::gDisplaySurface->width();
    int dstHeight = pharo::gDisplaySurface->height();

    // Clamp rect to valid range
    if (right <= 0 && bottom <= 0) { right = srcWidth; bottom = srcHeight; }
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > srcWidth) right = srcWidth;
    if (bottom > srcHeight) bottom = srcHeight;
    if (right > dstWidth) right = dstWidth;
    if (bottom > dstHeight) bottom = dstHeight;

    // When SDL2 rendering is active, skip the pixel copy to avoid overwriting
    // SDL2 content with stale Display Form data. The primitive still succeeds
    // so the Pharo side doesn't see errors.
    if (ffi_isSDLRenderingActive()) {
        return PrimitiveResult::Success;
    }

    // Copy the rectangle from Form bits to display surface
    for (int y = top; y < bottom; y++) {
        memcpy(dstPixels + y * dstWidth + left, srcPixels + y * srcWidth + left,
               (right - left) * sizeof(uint32_t));
    }

    pharo::gDisplaySurface->update();
    return PrimitiveResult::Success;
}

// showDisplayBits - Copy affected rectangle from Display form bitmap to platform display surface.
// Called after every successful BitBlt to mirror the reference VM's behavior
// (BitBltPlugin calls showDisplayBitsLeftTopRightBottom after copyBits).
void Interpreter::showDisplayBits(Oop destForm, int left, int top, int right, int bottom) {
    static int callCount = 0;
    static int bailSdl = 0, bailNotObj = 0, bailSmallInt = 0, bailNil = 0;
    static int bailFmt = 0, bailSize = 0, bailEmpty = 0, successCount = 0;
    callCount++;

    if (!pharo::gDisplaySurface) return;

    // When SDL2 rendering is active, skip Display Form copies to avoid
    // overwriting SDL2 content.
    if (ffi_isSDLRenderingActive()) {
        return;
    }

    // Match by form dimensions instead of identity (OSSDL2Driver creates a new form
    // that doesn't match the Display global, and GC can move objects).
    if (!destForm.isObject()) {
        if (++bailNotObj <= 3) fprintf(stderr, "[SDB] #%d bail: destForm not object\n", callCount);
        return;
    }
    Oop bits = memory_.fetchPointer(0, destForm);  // FormBits

    // bits can be: Bitmap object (regular Form), SmallInteger (ManualSurface handle),
    // or ExternalAddress (ExternalForm pointer). We can only copy from Bitmap objects.
    if (bits.isSmallInteger()) {
        if (++bailSmallInt <= 3) fprintf(stderr, "[SDB] #%d bail: bits is SmallInt=%lld\n", callCount, (long long)bits.asSmallInteger());
        return;
    }
    if (!bits.isObject() || bits.isNil()) {
        if (++bailNil <= 3) fprintf(stderr, "[SDB] #%d bail: bits nil/not-obj\n", callCount);
        return;
    }

    // Check if bits is a Bitmap (format 10 = 32-bit indexable words) vs ExternalAddress
    ObjectHeader* bitsHdr = bits.asObjectPtr();
    auto fmt = bitsHdr->format();
    if (fmt != ObjectFormat::Indexable32) {
        if (++bailFmt <= 3) fprintf(stderr, "[SDB] #%d bail: bits format=%d (expected Indexable32)\n", callCount, static_cast<int>(fmt));
        return;
    }

    Oop widthOop = memory_.fetchPointer(1, destForm);
    Oop heightOop = memory_.fetchPointer(2, destForm);
    if (!widthOop.isSmallInteger() || !heightOop.isSmallInteger()) return;
    int srcWidth = static_cast<int>(widthOop.asSmallInteger());
    int srcHeight = static_cast<int>(heightOop.asSmallInteger());
    int surfWidth = pharo::gDisplaySurface->width();
    int surfHeight = pharo::gDisplaySurface->height();
    if (srcWidth != surfWidth || srcHeight != surfHeight) {
        if (++bailSize <= 3) fprintf(stderr, "[SDB] #%d bail: size mismatch src=%dx%d surf=%dx%d\n", callCount, srcWidth, srcHeight, surfWidth, surfHeight);
        return;
    }

    uint32_t* srcPixels = reinterpret_cast<uint32_t*>(bitsHdr->bytes());
    uint32_t* dstPixels = pharo::gDisplaySurface->pixels();
    int dstWidth = pharo::gDisplaySurface->width();
    int dstHeight = pharo::gDisplaySurface->height();

    // Clamp rect to valid range
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > srcWidth) right = srcWidth;
    if (bottom > srcHeight) bottom = srcHeight;
    if (right > dstWidth) right = dstWidth;
    if (bottom > dstHeight) bottom = dstHeight;
    if (right <= left || bottom <= top) return;

    // Copy the rectangle from Form bits to display surface
    for (int y = top; y < bottom; y++) {
        memcpy(dstPixels + y * dstWidth + left,
               srcPixels + y * srcWidth + left,
               (right - left) * sizeof(uint32_t));
    }

    successCount++;
    if (successCount <= 5 || successCount % 500 == 0) {
        fprintf(stderr, "[SDB] #%d SUCCESS copy (%d,%d)-(%d,%d) total=%d bails: sdl=%d si=%d fmt=%d size=%d\n",
                callCount, left, top, right, bottom, successCount,
                bailSdl, bailSmallInt, bailFmt, bailSize);
    }

    pharo::gDisplaySurface->update();
}

// Primitive 109: Snapshot with embedded sources
// filename embedded primitiveSnapshotEmbedded -> boolean
// Creates an image snapshot, optionally embedding sources
PrimitiveResult Interpreter::primitiveSnapshotEmbedded(int argCount) {
    // This is a variant of snapshot that can embed sources in the image
    // For now, fail to let Smalltalk handle it or use the basic snapshot

    // In a full implementation:
    // 1. Get filename and embedded flag
    // 2. Write image file with optional embedded sources
    // 3. Return true on success

    // For now, fail to trigger Smalltalk fallback
    // which may use the standard snapshot mechanism
    return PrimitiveResult::Failure;
}

// ===== FFI/EXTERNAL PRIMITIVES (116-118, 147) =====

// Primitive 116: Flush external primitives cache
// primitiveFlushExternalPrimitives -> self
// Clears the cache of loaded external/plugin primitives
PrimitiveResult Interpreter::primitiveFlushExternalPrimitives(int argCount) {
    // In a VM with external plugins, this would:
    // 1. Unload all dynamically loaded primitives
    // 2. Clear the external primitive lookup cache
    // 3. Force re-lookup on next call

    // For our minimal VM without external plugins, this is a no-op
    // Just return success
    return PrimitiveResult::Success;
}

// Primitive 571: Unload an external module
// moduleName primitiveUnloadModule -> self
// Unloads the module with the given name
PrimitiveResult Interpreter::primitiveUnloadModule(int argCount) {
    if (argCount != 1) {
        return PrimitiveResult::Failure;
    }

    // We don't have external plugins, so this is a no-op
    // Just succeed as if the module was unloaded (or never loaded)
    pop();  // Pop argument, leave receiver
    return PrimitiveResult::Success;
}

// Primitive 572: List builtin modules
// index primitiveListBuiltinModule -> moduleName or nil
// Returns the n-th builtin module name (1-based index), nil if no more
PrimitiveResult Interpreter::primitiveListBuiltinModule(int argCount) {
    if (argCount != 1) {
        return PrimitiveResult::Failure;
    }

    Oop indexOop = stackTop();
    if (!indexOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    // We don't have any builtin modules to list
    // Return nil for any index
    pop();  // Pop argument
    pop();  // Pop receiver
    push(memory_.nil());
    return PrimitiveResult::Success;
}

// Primitive 573: List external (loaded) modules
// index primitiveListExternalModule -> moduleName or nil
// Returns the n-th loaded external module name (1-based index), nil if no more
PrimitiveResult Interpreter::primitiveListExternalModule(int argCount) {
    if (argCount != 1) {
        return PrimitiveResult::Failure;
    }

    Oop indexOop = stackTop();
    if (!indexOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    // We don't have any external modules loaded
    // Return nil for any index
    pop();  // Pop argument
    pop();  // Pop receiver
    push(memory_.nil());
    return PrimitiveResult::Success;
}

// Primitive 574: SIMD Float64 Array Addition
// arg1 arg2 res primitiveFloat64ArrayAdd -> res
// Adds corresponding elements of arg1 and arg2, stores in res
// All arrays must be Float64Array (64-bit indexable words)
PrimitiveResult Interpreter::primitiveFloat64ArrayAdd(int argCount) {
    if (argCount != 3) {
        return PrimitiveResult::Failure;
    }

    Oop resOop = stackValue(0);   // Result array (top of stack)
    Oop arg2Oop = stackValue(1);  // Second source array
    Oop arg1Oop = stackValue(2);  // First source array

    // All must be non-immediate objects
    if (!resOop.isObject() || !arg2Oop.isObject() || !arg1Oop.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Get object headers
    ObjectHeader* resHeader = resOop.asObjectPtr();
    ObjectHeader* arg1Header = arg1Oop.asObjectPtr();
    ObjectHeader* arg2Header = arg2Oop.asObjectPtr();

    if (!resHeader || !arg1Header || !arg2Header) {
        return PrimitiveResult::Failure;
    }

    // Check format - must be 64-bit indexable (Indexable64 = format 9)
    ObjectFormat resFormat = resHeader->format();
    ObjectFormat arg1Format = arg1Header->format();
    ObjectFormat arg2Format = arg2Header->format();

    if (resFormat != ObjectFormat::Indexable64 ||
        arg1Format != ObjectFormat::Indexable64 ||
        arg2Format != ObjectFormat::Indexable64) {
        return PrimitiveResult::Failure;
    }

    // Get sizes (in 64-bit slots)
    size_t resSize = resHeader->slotCount();
    size_t arg1Size = arg1Header->slotCount();
    size_t arg2Size = arg2Header->slotCount();

    // All must have the same size
    if (resSize != arg1Size || resSize != arg2Size) {
        return PrimitiveResult::Failure;
    }

    // Get pointers to the data (64-bit slots contain doubles)
    double* resData = reinterpret_cast<double*>(resHeader->slots());
    double* arg1Data = reinterpret_cast<double*>(arg1Header->slots());
    double* arg2Data = reinterpret_cast<double*>(arg2Header->slots());

    // Perform addition (simple loop - no actual SIMD instructions)
    for (size_t i = 0; i < resSize; i++) {
        resData[i] = arg1Data[i] + arg2Data[i];
    }

    // Pop arguments and receiver, push result
    popN(3);
    pop();  // Pop receiver
    push(resOop);
    return PrimitiveResult::Success;
}

// Primitive 117: Call out to FFI (Foreign Function Interface)
// externalFunction args primitiveCalloutToFFI -> result
// Calls a foreign function through FFI mechanism
PrimitiveResult Interpreter::primitiveCalloutToFFI(int argCount) {
    // --- Named primitive dispatch ---
    // Primitive 117 is used for both old-style FFI callouts (SDL2-specific)
    // and new TFFI primitives (via <primitive: 'name'>).
    // Check named primitives FIRST so TFFI primitives get dispatched properly.
    {
        Oop method = newMethod_.isObject() ? newMethod_ : method_;
        if (method.isObject()) {
            Oop methodHeader = memory_.fetchPointer(0, method);
            if (methodHeader.isSmallInteger()) {
                size_t numLiterals = methodHeader.asSmallInteger() & 0x7FFF;
                // Scan literals for string names that match registered named primitives
                for (size_t i = 1; i <= numLiterals && i < 20; i++) {
                    Oop lit = memory_.fetchPointer(i, method);
                    if (!lit.isObject() || !memory_.isValidPointer(lit)) continue;
                    ObjectHeader* litHdr = lit.asObjectPtr();
                    if (!litHdr->isBytesObject() || litHdr->byteSize() > 100) continue;
                    std::string name((char*)litHdr->bytes(), litHdr->byteSize());
                    // Try as named primitive with empty module
                    auto it = namedPrimitives_.find(":" + name);
                    if (it != namedPrimitives_.end()) {
                        return (this->*(it->second))(argCount);
                    }
                }
                // Also search inside arrays (Pragma objects store name in arguments array)
                for (size_t i = 1; i <= numLiterals && i < 20; i++) {
                    Oop lit = memory_.fetchPointer(i, method);
                    if (!lit.isObject() || !memory_.isValidPointer(lit)) continue;
                    ObjectHeader* litHdr = lit.asObjectPtr();
                    if (litHdr->isBytesObject()) continue;
                    size_t slots = litHdr->slotCount();
                    if (slots < 1 || slots > 10) continue;
                    for (size_t j = 0; j < slots; j++) {
                        Oop sub = memory_.fetchPointer(j, lit);
                        if (!sub.isObject() || !memory_.isValidPointer(sub)) continue;
                        ObjectHeader* sh = sub.asObjectPtr();
                        if (!sh->isBytesObject() || sh->byteSize() > 100) continue;
                        std::string name((char*)sh->bytes(), sh->byteSize());
                        auto it = namedPrimitives_.find(":" + name);
                        if (it != namedPrimitives_.end()) {
                            return (this->*(it->second))(argCount);
                        }
                    }
                }
            }
        }
    }

    // --- Legacy SDL2-specific FFI callout path ---
    static bool ffiInitialized = false;

    // Initialize FFI on first call
    if (!ffiInitialized) {
        ffiInitialized = ffi::initializeFFI();
        if (!ffiInitialized) {
            return PrimitiveResult::Failure;
        }
    }

    // The FFI call specification comes from the method's literals
    // We need to find it in the current method
    Oop method = method_;  // Current method being executed
    if (method.isNil()) {
        return PrimitiveResult::Failure;
    }

    // Get the literal frame to find the FFI spec
    // The spec is typically an array like #( returnType funcName ( argTypes... ) )
    // For now, we'll try to extract function name from the method pragmas

    // Get receiver - for SDL2 calls this is usually the SDL2 class or an external object
    Oop receiver = stackValue(argCount);

    // Try to find function name from method literals
    // In Pharo FFI, the ffiCall: pragma contains the spec
    ObjectHeader* methodHdr = method.asObjectPtr();

    // BUG FIX: Read actual numLiterals from method header, NOT slotCount()!
    // Method header is in slot 0 as SmallInteger. Bits 0-14 = numLiterals.
    Oop methodHeader = memory_.fetchPointer(0, method);
    if (!methodHeader.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }
    int64_t headerBits = methodHeader.asSmallInteger();
    size_t numLiterals = headerBits & 0x7FFF;  // bits 0-14 are numLiterals

    // Safety check for unreasonable literal count
    if (numLiterals > 1000) {
        return PrimitiveResult::Failure;
    }

    std::string funcName;
    std::vector<std::string> argTypeNames;
    std::string returnTypeName = "int";

    // Scan literals for an array that looks like an FFI spec
    // Literals are at slots 1..numLiterals (slot 0 is the method header)
    for (size_t i = 1; i <= numLiterals && funcName.empty(); i++) {
        Oop lit = memory_.fetchPointer(i, method);
        if (lit.isObject()) {
            // Validate pointer is in heap before dereferencing
            if (!memory_.isValidPointer(lit)) {
                continue;
            }
            ObjectHeader* litHdr = lit.asObjectPtr();
            uint32_t format = static_cast<uint32_t>(litHdr->format());

            // Check if it's an Array (format 2 = indexable pointers)
            if (format == 2) {
                size_t arrSize = litHdr->slotCount();
                if (arrSize >= 2) {
                    // First element might be return type, second might be function name
                    Oop first = memory_.fetchPointer(0, lit);
                    Oop second = memory_.fetchPointer(1, lit);

                    // Check if second element is a symbol (function name)
                    if (second.isObject()) {
                        ObjectHeader* secondHdr = second.asObjectPtr();
                        uint32_t secondFormat = static_cast<uint32_t>(secondHdr->format());
                        // Format 16-23 are byte strings
                        if (secondFormat >= 16 && secondFormat <= 23) {
                            size_t len = secondHdr->byteSize();
                            const char* bytes = reinterpret_cast<const char*>(secondHdr->bytes());
                            std::string name(bytes, len);

                            // Check if it looks like an SDL function
                            if (name.find("SDL_") == 0) {
                                funcName = name;

                                // Get return type from first element
                                if (first.isObject()) {
                                    ObjectHeader* firstHdr = first.asObjectPtr();
                                    uint32_t firstFormat = static_cast<uint32_t>(firstHdr->format());
                                    if (firstFormat >= 16 && firstFormat <= 23) {
                                        size_t retLen = firstHdr->byteSize();
                                        const char* retBytes = reinterpret_cast<const char*>(firstHdr->bytes());
                                        returnTypeName = std::string(retBytes, retLen);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Check if we found a function name from literals - if not, try extracting selector
    if (funcName.empty()) {
        // Try to find a Symbol that might be the function name
        // Literals are at slots 1..numLiterals (slot 0 is the method header)
        for (size_t i = 1; i <= numLiterals && funcName.empty(); i++) {
            Oop lit = memory_.fetchPointer(i, method);
            if (lit.isObject() && memory_.isValidPointer(lit)) {
                ObjectHeader* litHdr = lit.asObjectPtr();
                uint32_t format = static_cast<uint32_t>(litHdr->format());
                // Symbol/String format 16-23
                if (format >= 16 && format <= 23 && litHdr->byteSize() > 3) {
                    std::string str((char*)litHdr->bytes(), litHdr->byteSize());
                    // Check for known internal FFI functions that we don't support
                    // Return appropriate values instead of failing (which raises exceptions)
                    if (str == "primNextPendingCallback" || str == "nextPendingCallback") {
                        // Return nil - no pending callbacks (we don't support FFI callbacks)
                        // IMPORTANT: Use memory_.nil() not Oop::nil() - the image's nil is at a real address
                        popN(static_cast<size_t>(argCount + 1));  // Pop args and receiver
                        push(memory_.nil());
                        return PrimitiveResult::Success;
                    }
                    if (str == "primNumberOfCallbacks" || str == "numberOfCallbacks") {
                        // Return 0 - no pending callbacks
                        popN(static_cast<size_t>(argCount + 1));  // Pop args and receiver
                        push(Oop::fromSmallInteger(0));
                        return PrimitiveResult::Success;
                    }
                }
            }
        }
    }

    if (funcName.empty()) {
        return PrimitiveResult::Failure;
    }

    // Look up the function
    void* funcPtr = ffi::lookupFunction("SDL2", funcName);
    if (!funcPtr) {
        return PrimitiveResult::Failure;
    }

    // Marshal arguments from stack
    std::vector<ffi::FFIType> argTypes;
    std::vector<uint64_t> argValues;

    for (int i = argCount - 1; i >= 0; i--) {
        Oop arg = stackValue(i);

        if (arg.isSmallInteger()) {
            argTypes.push_back(ffi::FFIType::Int64);
            argValues.push_back(static_cast<uint64_t>(arg.asSmallInteger()));
        } else if (arg.isObject()) {
            // Could be an ExternalAddress, ByteArray, or String
            ObjectHeader* argHdr = arg.asObjectPtr();
            uint32_t argFormat = static_cast<uint32_t>(argHdr->format());

            if (argFormat >= 16 && argFormat <= 23) {
                // Byte object - pass pointer to bytes
                argTypes.push_back(ffi::FFIType::Pointer);
                argValues.push_back(reinterpret_cast<uint64_t>(argHdr->bytes()));
            } else if (argFormat == 2 && argHdr->slotCount() >= 1) {
                // Could be ExternalAddress - check first slot
                Oop firstSlot = memory_.fetchPointer(0, arg);
                if (firstSlot.isSmallInteger()) {
                    // Treat as address
                    argTypes.push_back(ffi::FFIType::Pointer);
                    argValues.push_back(static_cast<uint64_t>(firstSlot.asSmallInteger()));
                } else {
                    argTypes.push_back(ffi::FFIType::Pointer);
                    argValues.push_back(reinterpret_cast<uint64_t>(arg.asObjectPtr()));
                }
            } else {
                // Pass object pointer
                argTypes.push_back(ffi::FFIType::Pointer);
                argValues.push_back(reinterpret_cast<uint64_t>(arg.asObjectPtr()));
            }
        } else {
            // Unknown type
            argTypes.push_back(ffi::FFIType::Int64);
            argValues.push_back(0);
        }
    }

    // Determine return type
    ffi::FFIType returnType = ffi::parseType(returnTypeName);

    // Call the function
    ffi::FFIResult result = ffi::callFunction(funcPtr, argTypes, argValues, returnType);

    if (!result.success) {
        return PrimitiveResult::Failure;
    }

    // Marshal result back to Smalltalk
    Oop resultOop;
    switch (result.type) {
        case ffi::FFIType::Void:
            resultOop = receiver;  // Return receiver for void functions
            break;

        case ffi::FFIType::Bool:
            resultOop = result.intValue ? memory_.trueObject() : memory_.falseObject();
            break;

        case ffi::FFIType::Int8:
        case ffi::FFIType::Int16:
        case ffi::FFIType::Int32:
        case ffi::FFIType::UInt8:
        case ffi::FFIType::UInt16:
        case ffi::FFIType::UInt32:
            resultOop = Oop::fromSmallInteger(static_cast<int64_t>(result.intValue));
            break;

        case ffi::FFIType::Int64:
            resultOop = int64ToOop(memory_, static_cast<int64_t>(result.intValue));
            if (resultOop.isNil()) return PrimitiveResult::Failure;
            break;

        case ffi::FFIType::UInt64:
            resultOop = uint64ToOop(memory_, result.intValue);
            if (resultOop.isNil()) return PrimitiveResult::Failure;
            break;

        case ffi::FFIType::Pointer:
            if (result.ptrValue == nullptr) {
                resultOop = Oop::nil();
            } else {
                // Create ExternalAddress to hold the pointer
                Oop externalAddressClass = memory_.findGlobal("ExternalAddress");
                if (externalAddressClass.isNil()) {
                    // Fallback: return as positive integer
                    resultOop = uint64ToOop(memory_, reinterpret_cast<uintptr_t>(result.ptrValue));
                    if (resultOop.isNil()) return PrimitiveResult::Failure;
                } else {
                    uint32_t classIndex = memory_.indexOfClass(externalAddressClass);
                    if (classIndex == 0) return PrimitiveResult::Failure;
                    constexpr size_t ptrSize = sizeof(void*);
                    resultOop = memory_.allocateBytes(classIndex, ptrSize);
                    if (resultOop.isNil()) return PrimitiveResult::Failure;
                    ObjectHeader* hdr = resultOop.asObjectPtr();
                    memcpy(hdr->bytes(), &result.ptrValue, ptrSize);
                }
            }
            break;

        default:
            resultOop = Oop::nil();
            break;
    }

    // Use primitiveSuccess which handles stack correctly
    primitiveSuccess(resultOop);

    return PrimitiveResult::Success;
}

// Primitive 118: DLL/shared library call
// moduleName functionName args primitiveDLLCall -> result
// Calls a function in a dynamically loaded library
PrimitiveResult Interpreter::primitiveDLLCall(int argCount) {
    // DLL call requires:
    // 1. dlopen/LoadLibrary to load the module
    // 2. dlsym/GetProcAddress to find the function
    // 3. Calling convention handling
    // 4. Argument/result marshalling

    // Without full DLL support, fail to Smalltalk
    return PrimitiveResult::Failure;
}

// Primitive 147: External primitive call (named primitive)
// primitiveExternalCall -> result
// Calls a primitive defined in an external plugin module
PrimitiveResult Interpreter::primitiveExternalCall(int argCount) {
    // Use newMethod_ (the method being activated) rather than method_ (the caller)
    // because executePrimitive runs BEFORE activateMethod
    Oop method = newMethod_.isObject() ? newMethod_ : method_;
    if (!method.isObject()) {
        return PrimitiveResult::Failure;
    }

    // The primitive spec is typically in the method's literal frame
    // In Spur, the format is usually:
    // - Literal at index 0 or 1 contains an Array: #(moduleName primitiveName flags)
    // Or an ExternalLibraryFunction object

    ObjectHeader* methodHdr = method.asObjectPtr();

    // BUG FIX: Must read actual numLiterals from method header, NOT slotCount()!
    // slotCount() returns total slots including bytecode area, which would cause
    // us to read bytecode bytes as oop values, creating "corrupted" pointers.
    // Method header is in slot 0 as a SmallInteger. Bits 0-14 = numLiterals.
    Oop methodHeader = memory_.fetchPointer(0, method);
    if (!methodHeader.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }
    int64_t headerBits = methodHeader.asSmallInteger();
    size_t numLiterals = headerBits & 0x7FFF;  // bits 0-14 are numLiterals

    // Fast path: Check for known primitives by name in literals
    // Note: literals are at slots 1..numLiterals (slot 0 is the header)
    // Pragma objects store primitive names in their arguments array

    // Helper to recursively search for a string containing a pattern
    std::function<bool(Oop, const std::string&, int)> containsString;
    containsString = [&](Oop obj, const std::string& pattern, int depth) -> bool {
        if (depth > 5 || !obj.isObject() || !memory_.isValidPointer(obj)) return false;
        ObjectHeader* hdr = obj.asObjectPtr();

        if (hdr->isBytesObject() && hdr->byteSize() < 200) {
            std::string str((char*)hdr->bytes(), hdr->byteSize());
            if (str.find(pattern) != std::string::npos) return true;
        }

        if ((hdr->format() == ObjectFormat::Indexable || hdr->format() == ObjectFormat::FixedSize)
            && hdr->slotCount() >= 1 && hdr->slotCount() <= 30) {
            for (size_t i = 0; i < hdr->slotCount(); i++) {
                Oop slot = memory_.fetchPointer(i, obj);
                if (containsString(slot, pattern, depth + 1)) return true;
            }
        }
        return false;
    };

    // Check all literals for our known named primitives (direct string scan)
    // Collect all string literals for module:name lookup
    std::vector<std::string> literalStrings;
    for (size_t i = 1; i <= numLiterals; i++) {
        Oop literal = memory_.fetchPointer(i, method);
        if (!literal.isObject() || !memory_.isValidPointer(literal)) continue;
        ObjectHeader* litHdr = literal.asObjectPtr();
        if (litHdr->isBytesObject() && litHdr->byteSize() < 100) {
            literalStrings.emplace_back((char*)litHdr->bytes(), litHdr->byteSize());
        }
        // Also search inside arrays/fixed objects
        if (!litHdr->isBytesObject() && litHdr->slotCount() >= 1 && litHdr->slotCount() <= 10) {
            for (size_t j = 0; j < litHdr->slotCount(); j++) {
                Oop sub = memory_.fetchPointer(j, literal);
                if (sub.isObject() && memory_.isValidPointer(sub)) {
                    ObjectHeader* sh = sub.asObjectPtr();
                    if (sh->isBytesObject() && sh->byteSize() < 100) {
                        literalStrings.emplace_back((char*)sh->bytes(), sh->byteSize());
                    }
                }
            }
        }
    }
    // Try all combinations of module:name from literal strings
    for (size_t a = 0; a < literalStrings.size(); a++) {
        // Try as direct name with empty module
        auto it = namedPrimitives_.find(":" + literalStrings[a]);
        if (it != namedPrimitives_.end()) {
            return (this->*(it->second))(argCount);
        }
        // Try external plugin primitives
        auto xit = externalPrimitives_.find(":" + literalStrings[a]);
        if (xit != externalPrimitives_.end()) {
            return callExternalPrimitive(xit->second);
        }
        // Try as module:name with other literals as names
        for (size_t b = 0; b < literalStrings.size(); b++) {
            if (a == b) continue;
            std::string key = literalStrings[a] + ":" + literalStrings[b];
            auto it2 = namedPrimitives_.find(key);
            if (it2 != namedPrimitives_.end()) {
                return (this->*(it2->second))(argCount);
            }
            // Try external plugin primitives
            auto xit2 = externalPrimitives_.find(key);
            if (xit2 != externalPrimitives_.end()) {
                return callExternalPrimitive(xit2->second);
            }
        }
    }

    for (size_t i = 1; i <= numLiterals; i++) {
        Oop literal = memory_.fetchPointer(i, method);
        if (!literal.isObject() || !memory_.isValidPointer(literal)) continue;

        ObjectHeader* litHdr = literal.asObjectPtr();

        // Direct string check
        if (litHdr->isBytesObject() && litHdr->byteSize() < 100) {
            std::string str((char*)litHdr->bytes(), litHdr->byteSize());
            if (str == "stringHash:initialHash:") {
                return primitiveStringHashInitialHash(argCount);
            }
            if (str == "indexOfAscii:inString:startingAt:") {
                return primitiveIndexOfAscii(argCount);
            }
            if (str == "primitiveLoadSymbolFromModule") {
                return primitiveLoadSymbolFromModule(argCount);
            }
            if (str == "primitiveLoadModule") {
                return primitiveLoadModule(argCount);
            }
        }

        // Deep search for FFI primitives in Pragma objects
        if (containsString(literal, "LoadSymbolFromModule", 0)) {
            return primitiveLoadSymbolFromModule(argCount);
        }
        if (containsString(literal, "LoadModule", 0)) {
            if (containsString(literal, "LoadSymbol", 0)) {
                continue;  // This is LoadSymbolFromModule, not LoadModule
            }
            return primitiveLoadModule(argCount);
        }
    }

    // Search literals for the primitive spec (usually an Array with module/name)
    // Literals are at slots 1..numLiterals (slot 0 is the method header)
    for (size_t i = 1; i <= numLiterals && i < 10; i++) {
        Oop literal = memory_.fetchPointer(i, method);
        if (!literal.isObject()) continue;
        if (!memory_.isValidPointer(literal)) continue;  // Validate before dereference

        ObjectHeader* litHdr = literal.asObjectPtr();

        // Check for ThreadedFFI callback primitives directly in string literals
        // This is needed because the TFCallbackInvocation methods have the selector
        // as a string literal, but the spec array has a class object, not a string
        if (litHdr->isBytesObject() && litHdr->byteSize() < 100) {
            std::string str((char*)litHdr->bytes(), litHdr->byteSize());
            if (str == "primNextPendingCallback" || str == "nextPendingCallback") {
                // Return nil - no pending callbacks (we don't support FFI callbacks)
                // IMPORTANT: Use memory_.nil() not Oop::nil() - the image's nil is at a real address
                popN(static_cast<size_t>(argCount + 1));  // Pop args and receiver
                push(memory_.nil());
                return PrimitiveResult::Success;
            }
            if (str == "primNumberOfCallbacks" || str == "numberOfCallbacks") {
                // Return 0 - no pending callbacks
                popN(static_cast<size_t>(argCount + 1));  // Pop args and receiver
                push(Oop::fromSmallInteger(0));
                return PrimitiveResult::Success;
            }
            // CRITICAL: isVMDisplayUsingSDL2 check - OSSDL2Driver uses this to decide event handling
            if (str == "isVMDisplayUsingSDL2") {
                return primitiveIsVMDisplayUsingSDL2(argCount);
            }
            // SDL2 input semaphore - enables SDL2 event polling
            if (str == "primitiveSetVMSDL2Input:" || str == "primitiveSetVMSDL2Input") {
                return primitiveSetVMSDL2Input(argCount);
            }
        }

        // Check if it's an Array (format 2 = indexable pointers) or ExternalLibraryFunction (format 1)
        // Also check Fixed format objects that might contain module/name references
        if ((litHdr->format() == ObjectFormat::Indexable || litHdr->format() == ObjectFormat::FixedSize)
            && litHdr->slotCount() >= 2) {
            // Could be #(moduleName primitiveName ...) or ExternalLibraryFunction
            Oop moduleOop = memory_.fetchPointer(0, literal);
            Oop nameOop = memory_.fetchPointer(1, literal);

            // Extract strings
            std::string moduleName, primName;

            if (moduleOop.isObject() && memory_.isValidPointer(moduleOop)) {
                ObjectHeader* modHdr = moduleOop.asObjectPtr();
                if (modHdr->isBytesObject() && modHdr->byteSize() < 100) {
                    moduleName = std::string((char*)modHdr->bytes(), modHdr->byteSize());
                }
            }

            if (nameOop.isObject() && memory_.isValidPointer(nameOop)) {
                ObjectHeader* nameHdr = nameOop.asObjectPtr();
                if (nameHdr->isBytesObject() && nameHdr->byteSize() < 100) {
                    primName = std::string((char*)nameHdr->bytes(), nameHdr->byteSize());
                }
                // If nameOop is a FixedSize object, check its slots for module/name
                if (nameHdr->format() == ObjectFormat::FixedSize && nameHdr->slotCount() >= 2) {
                    // This could be ExternalLibraryFunction - check slots
                    for (size_t j = 0; j < nameHdr->slotCount() && j < 8; j++) {
                        Oop innerOop = memory_.fetchPointer(j, nameOop);
                        if (innerOop.isObject() && memory_.isValidPointer(innerOop)) {
                            ObjectHeader* innerHdr = innerOop.asObjectPtr();
                            if (innerHdr->isBytesObject() && innerHdr->byteSize() < 100) {
                                std::string str((char*)innerHdr->bytes(), innerHdr->byteSize());
                                // If this is slot 3 or 5, it might be module or function name
                                if (j == 3) {
                                    moduleName = str;
                                } else if (j == 0 || j == 1) {
                                    primName = str;
                                }
                            } else if ((innerHdr->format() == ObjectFormat::FixedSize ||
                                       innerHdr->format() == ObjectFormat::Indexable) && innerHdr->slotCount() >= 1) {
                                // Check one more level for the function name
                                for (size_t k = 0; k < innerHdr->slotCount() && k < 6; k++) {
                                    Oop deepOop = memory_.fetchPointer(k, innerOop);
                                    if (deepOop.isObject() && memory_.isValidPointer(deepOop)) {
                                        ObjectHeader* deepHdr = deepOop.asObjectPtr();
                                        if (deepHdr->isBytesObject() && deepHdr->byteSize() < 100) {
                                            std::string str((char*)deepHdr->bytes(), deepHdr->byteSize());
                                            // Extract module and primitive names
                                            if (str == "MiscPrimitivePlugin" || str == "primitiveStringHash") {
                                                if (str.find("Plugin") != std::string::npos) {
                                                    moduleName = str;
                                                } else {
                                                    primName = str;
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

            if (!moduleName.empty() && !primName.empty()) {
                std::string key = moduleName + ":" + primName;

                // Handle ThreadedFFI callback primitives - return nil/0 instead of failing
                // This prevents exception handling from consuming startup cycles
                if (primName == "primNextPendingCallback" || primName == "nextPendingCallback") {
                    // Return nil - no pending callbacks (we don't support FFI callbacks)
                    // IMPORTANT: Use memory_.nil() not Oop::nil() - the image's nil is at a real address
                    popN(static_cast<size_t>(argCount + 1));  // Pop args and receiver
                    push(memory_.nil());
                    return PrimitiveResult::Success;
                }
                if (primName == "primNumberOfCallbacks" || primName == "numberOfCallbacks") {
                    // Return 0 - no pending callbacks
                    popN(static_cast<size_t>(argCount + 1));  // Pop args and receiver
                    push(Oop::fromSmallInteger(0));
                    return PrimitiveResult::Success;
                }

                auto it = namedPrimitives_.find(key);
                if (it != namedPrimitives_.end()) {
                    return (this->*(it->second))(argCount);
                }

                // Try external plugin primitives
                auto xit = externalPrimitives_.find(key);
                if (xit != externalPrimitives_.end()) {
                    return callExternalPrimitive(xit->second);
                }

                // Try without module prefix (for compatibility)
                auto it2 = namedPrimitives_.find(":" + primName);
                if (it2 != namedPrimitives_.end()) {
                    return (this->*(it2->second))(argCount);
                }

                // Try external primitives without module prefix
                auto xit2 = externalPrimitives_.find(":" + primName);
                if (xit2 != externalPrimitives_.end()) {
                    return callExternalPrimitive(xit2->second);
                }
            }
        }
    }

    // Last resort: Try to find and call the external function dynamically
    // This is needed for SDL2 and other FFI calls
    // Search for the external call spec in literal 0 (slot 1) - reference VM format
    if (numLiterals >= 1) {
        Oop lit0 = memory_.fetchPointer(1, method);  // literal 0 is at slot 1
        if (lit0.isObject() && memory_.isValidPointer(lit0)) {
            ObjectHeader* lit0Hdr = lit0.asObjectPtr();
            // Should be an Array with 4 slots: #(moduleName functionName accessorDepth cachedIndex)
            if ((lit0Hdr->format() == ObjectFormat::Indexable || lit0Hdr->format() == ObjectFormat::FixedSize)
                && lit0Hdr->slotCount() >= 2) {
                Oop moduleOop = memory_.fetchPointer(0, lit0);
                Oop functionOop = memory_.fetchPointer(1, lit0);

                std::string moduleName, functionName;

                // Extract module name
                if (moduleOop.isObject() && memory_.isValidPointer(moduleOop) &&
                    moduleOop.rawBits() != memory_.nil().rawBits()) {
                    ObjectHeader* modHdr = moduleOop.asObjectPtr();
                    if (modHdr->isBytesObject() && modHdr->byteSize() < 100) {
                        moduleName = std::string((char*)modHdr->bytes(), modHdr->byteSize());
                    }
                }

                // Extract function name
                if (functionOop.isObject() && memory_.isValidPointer(functionOop)) {
                    ObjectHeader* funcHdr = functionOop.asObjectPtr();
                    if (funcHdr->isBytesObject() && funcHdr->byteSize() < 100) {
                        functionName = std::string((char*)funcHdr->bytes(), funcHdr->byteSize());
                    }
                }

                if (!functionName.empty()) {
                    // First try named primitive lookup
                    std::string key = moduleName + ":" + functionName;
                    auto it = namedPrimitives_.find(key);
                    if (it != namedPrimitives_.end()) {
                        return (this->*(it->second))(argCount);
                    }
                    // Try external plugin primitives
                    auto xit = externalPrimitives_.find(key);
                    if (xit != externalPrimitives_.end()) {
                        return callExternalPrimitive(xit->second);
                    }
                    // Try without module
                    auto it2 = namedPrimitives_.find(":" + functionName);
                    if (it2 != namedPrimitives_.end()) {
                        return (this->*(it2->second))(argCount);
                    }
                    // Try external without module
                    auto xit2 = externalPrimitives_.find(":" + functionName);
                    if (xit2 != externalPrimitives_.end()) {
                        return callExternalPrimitive(xit2->second);
                    }

                }
            }
        }
    }

    // Last resort: try FFI callout (for Threaded FFI / UFFI methods)
    // This handles SDL2 and other FFI calls where the method literal contains
    // a TFExternalFunction or similar FFI spec object
    {
        PrimitiveResult ffiResult = primitiveCalloutToFFI(argCount);
        if (ffiResult == PrimitiveResult::Success) {
            return ffiResult;
        }
    }

    return PrimitiveResult::Failure;
}

// ===== SOCKET PRIMITIVE (133) =====

// Primitive 133: Socket operations
// This is a dispatcher primitive - the actual operation is determined by
// the first argument which specifies the socket function to perform
// socketOp args... primitiveSocket -> result
PrimitiveResult Interpreter::primitiveSocket(int argCount) {
    // Socket primitive is typically used as a gateway to multiple socket operations:
    // - Create socket
    // - Connect
    // - Bind
    // - Listen
    // - Accept
    // - Send/Receive
    // - Close
    // etc.

    // The first argument usually specifies which operation to perform
    // Without full socket support, fail to Smalltalk fallback
    // Smalltalk networking code may use alternative mechanisms or report unavailable

    // A full implementation would:
    // 1. Check operation code in first argument
    // 2. Dispatch to appropriate socket operation
    // 3. Handle platform-specific socket API

    return PrimitiveResult::Failure;
}

// ===== IMAGE SEGMENT PRIMITIVES (213-216) =====

// Primitive 213: Store image segment
// arrayOfRoots arrayOfObjects segmentWordArray primitiveStoreImageSegment -> rootsArray or fail
// Stores objects into a segment format suitable for file storage
PrimitiveResult Interpreter::primitiveStoreImageSegment(int argCount) {
    if (argCount != 3) return PrimitiveResult::Failure;

    Oop segmentWordArray = stackTop();
    Oop arrayOfObjects = stackValue(1);
    Oop arrayOfRoots = stackValue(2);

    // Validate all arguments are arrays
    if (segmentWordArray.isImmediate() || arrayOfObjects.isImmediate() || arrayOfRoots.isImmediate()) {
        return PrimitiveResult::Failure;
    }

    // Image segment storage is a complex operation that:
    // 1. Traverses all objects reachable from arrayOfObjects
    // 2. Copies them into a portable binary format in segmentWordArray
    // 3. Records external references in arrayOfRoots
    // 4. Handles object identity and class table mapping

    // Without full image segment support, fail to Smalltalk
    // The Smalltalk code can handle serialization via other means
    return PrimitiveResult::Failure;
}

// Primitive 214: Load image segment
// segmentWordArray outPointers primitiveLoadImageSegment -> arrayOfObjects or fail
// Loads objects from a segment format back into the heap
PrimitiveResult Interpreter::primitiveLoadImageSegment(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop outPointers = stackTop();
    Oop segmentWordArray = stackValue(1);

    // Validate arguments
    if (segmentWordArray.isImmediate() || outPointers.isImmediate()) {
        return PrimitiveResult::Failure;
    }

    // Image segment loading is a complex operation that:
    // 1. Parses the binary format from segmentWordArray
    // 2. Allocates and reconstructs all objects
    // 3. Resolves external references using outPointers
    // 4. Updates class indices to match current class table

    // Without full image segment support, fail to Smalltalk
    return PrimitiveResult::Failure;
}

// Primitive 215: Array swap
// array1 array2 primitiveArraySwap -> receiver
// Swaps contents of two arrays element by element
PrimitiveResult Interpreter::primitiveArraySwap(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop array2 = stackTop();
    Oop array1 = stackValue(1);
    Oop receiver = stackValue(2);

    // Validate arguments are non-immediate
    if (array1.isImmediate() || array2.isImmediate()) {
        return PrimitiveResult::Failure;
    }

    // Get sizes
    size_t size1 = memory_.slotCountOf(array1);
    size_t size2 = memory_.slotCountOf(array2);

    // Arrays must be same size
    if (size1 != size2) {
        return PrimitiveResult::Failure;
    }

    // Check immutability
    if (memory_.isImmutable(array1) || memory_.isImmutable(array2)) {
        return PrimitiveResult::Failure;
    }

    // Swap all elements
    for (size_t i = 0; i < size1; i++) {
        Oop temp = memory_.fetchPointer(i, array1);
        memory_.storePointer(i, array1, memory_.fetchPointer(i, array2));
        memory_.storePointer(i, array2, temp);
    }

    popN(2);  // Pop arguments, leave receiver
    return PrimitiveResult::Success;
}

// Primitive 216: Find roots (objects pointing to an object)
// anObject primitiveFindRoots -> arrayOfRoots
// Finds all objects that reference the given object
PrimitiveResult Interpreter::primitiveFindRoots(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop targetObject = stackTop();

    // We need to find all objects that reference targetObject
    // This requires a full heap scan

    // Collect all objects that point to target
    std::vector<Oop> roots;

    memory_.allObjectsDo([&](Oop obj) {
        if (obj.isImmediate()) return;
        if (!obj.isObject()) return;

        // Skip hidden objects (classIdx=0) and objects with invalid class
        ObjectHeader* hdr = obj.asObjectPtr();
        uint32_t cls = hdr->classIndex();
        if (cls == 0) return;
        Oop classOop = memory_.classAtIndex(cls);
        if (!classOop.isObject() || classOop.isNil()) return;

        // Check each slot
        size_t slotCount = memory_.slotCountOf(obj);
        for (size_t i = 0; i < slotCount; i++) {
            Oop slot = memory_.fetchPointer(i, obj);
            if (slot == targetObject) {
                roots.push_back(obj);
                break;  // Only add each object once
            }
        }
    });

    // Allocate result array
    uint32_t arrayClassIndex = memory_.classOf(memory_.specialObject(SpecialObjectIndex::ClassArray)).isImmediate()
        ? static_cast<uint32_t>(memory_.specialObject(SpecialObjectIndex::ClassArray).asSmallInteger())
        : memory_.indexOfClass(memory_.specialObject(SpecialObjectIndex::ClassArray));

    Oop result = memory_.allocateSlots(arrayClassIndex, roots.size(), ObjectFormat::Indexable);
    if (result.isNil()) {
        return PrimitiveResult::Failure;
    }

    // Fill result array
    for (size_t i = 0; i < roots.size(); i++) {
        memory_.storePointer(i, result, roots[i]);
    }

    pop();
    push(result);
    return PrimitiveResult::Success;
}

// ===== OBJECT/MEMORY PRIMITIVES (217-221) =====

// Primitive 217: VM Functionality
// primitiveVMFunctionality -> capabilities integer
// Returns a bitmap of VM capabilities
PrimitiveResult Interpreter::primitiveVMFunctionality(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Return a bitmap of supported features:
    // Bit 0: supportsClosures (yes)
    // Bit 1: supportsGarbageCollection (yes)
    // Bit 2: supportsFFI (limited)
    // Bit 3: supports64BitArithmetic (yes)
    // etc.

    uint64_t capabilities = 0;
    capabilities |= (1 << 0);  // closures
    capabilities |= (1 << 1);  // GC
    // capabilities |= (1 << 2);  // FFI (not fully supported)
    capabilities |= (1 << 3);  // 64-bit arithmetic
    capabilities |= (1 << 4);  // immutability
    capabilities |= (1 << 5);  // pinning
    capabilities |= (1 << 6);  // ephemerons (TBD)

    pop();  // receiver
    push(Oop::fromSmallInteger(static_cast<int64_t>(capabilities)));
    return PrimitiveResult::Success;
}

// Primitive 218: Identity hash (32-bit)
// anObject primitiveIdentityHash32 -> hash32
// Returns the full 32-bit identity hash
PrimitiveResult Interpreter::primitiveIdentityHash32(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop receiver = stackTop();

    // SmallIntegers use their value as hash
    if (receiver.isSmallInteger()) {
        int64_t val = receiver.asSmallInteger();
        pop();
        push(Oop::fromSmallInteger(val & 0xFFFFFFFF));
        return PrimitiveResult::Success;
    }

    // Characters use their code point
    if (receiver.isCharacter()) {
        pop();
        push(Oop::fromSmallInteger(receiver.asCharacter()));
        return PrimitiveResult::Success;
    }

    // Get full 32-bit hash from object header
    uint32_t hash = memory_.identityHashOf(receiver);
    pop();
    push(Oop::fromSmallInteger(static_cast<int64_t>(hash)));
    return PrimitiveResult::Success;
}

// Primitive 219: Grow memory by at least N bytes
// bytesToGrow primitiveGrowMemoryByAtLeastByAtLeast -> actualBytesGrown or 0
PrimitiveResult Interpreter::primitiveGrowMemoryByAtLeastByAtLeast(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop bytesArg = stackTop();
    if (!bytesArg.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t bytesRequested = bytesArg.asSmallInteger();
    if (bytesRequested < 0) {
        return PrimitiveResult::Failure;
    }

    // Memory growth is handled by the GC system
    // For now, we don't support dynamic memory growth
    // Return 0 to indicate no growth occurred
    pop();  // argument
    pop();  // receiver
    push(Oop::fromSmallInteger(0));
    return PrimitiveResult::Success;
}

// Primitive 220: Image format version
// primitiveImageFormatVersion -> formatVersion
// Returns the image format this VM supports
PrimitiveResult Interpreter::primitiveImageFormatVersion(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Return Spur 64-bit format (68002 is typical Spur 64-bit)
    // We support the iOS-specific format with low-bit encoding
    int64_t formatVersion = 68002;

    pop();  // receiver
    push(Oop::fromSmallInteger(formatVersion));
    return PrimitiveResult::Success;
}

// Primitive 221: Closure value with args (alternative entry)
// closureOrBlock argArray primitiveClosureValueWithArgs -> result
// Evaluates a closure with arguments from an array
PrimitiveResult Interpreter::primitiveClosureValueWithArgs(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop argArray = stackTop();
    Oop closure = stackValue(1);

    // Validate closure
    if (closure.isImmediate()) {
        return PrimitiveResult::Failure;
    }

    // Validate args is an array
    if (argArray.isImmediate()) {
        return PrimitiveResult::Failure;
    }

    // Get number of arguments from array
    size_t numArgs = memory_.slotCountOf(argArray);

    // Get closure's expected argument count from numArgs field
    // Closure format: outerContext, startpc, numArgs, ...
    Oop numArgsOop = memory_.fetchPointer(2, closure);  // numArgs at index 2
    if (!numArgsOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    size_t expectedArgs = static_cast<size_t>(numArgsOop.asSmallInteger());
    if (numArgs != expectedArgs) {
        return PrimitiveResult::Failure;
    }

    // Pop argArray, then push each argument from array
    pop();  // argArray

    // Push arguments onto stack (closure is already there)
    for (size_t i = 0; i < numArgs; i++) {
        Oop arg = memory_.fetchPointer(i, argArray);
        push(arg);
    }

    // Now the stack has: closure arg1 arg2 ... argN
    // Activate the closure - this will be handled by the caller
    // For now, fail to Smalltalk which will handle block activation
    return PrimitiveResult::Failure;
}

// ===== SYSTEM PRIMITIVES (528-530) =====

// Primitive 528: Get extra word from object header
// anObject index primitiveGetExtraWordAt -> word
// Gets extra header data (used for overflow header, extra class data, etc.)
PrimitiveResult Interpreter::primitiveGetExtraWordAt(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    Oop receiver = stackValue(1);

    if (!indexOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    if (receiver.isImmediate()) {
        return PrimitiveResult::Failure;
    }

    // Extra words are typically used for overflow headers in Spur
    // For basic implementation, we only support index 0 which is the
    // first word before the main header (if present)

    int64_t index = indexOop.asSmallInteger();
    if (index < 0 || index > 0) {
        // Only support index 0 for now
        return PrimitiveResult::Failure;
    }

    // Get the raw header - in Spur, objects with >254 slots have an
    // extra word before the header containing the actual size
    // For now, return 0 indicating no extra word
    popN(2);  // pop index and receiver
    push(Oop::fromSmallInteger(0));
    return PrimitiveResult::Success;
}

// Primitive 529: Set extra word in object header
// anObject index value primitiveSetExtraWordAt -> receiver
// Sets extra header data
PrimitiveResult Interpreter::primitiveSetExtraWordAt(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop valueOop = stackTop();
    Oop indexOop = stackValue(1);
    Oop receiver = stackValue(2);

    if (!indexOop.isSmallInteger() || !valueOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    if (receiver.isImmediate()) {
        return PrimitiveResult::Failure;
    }

    // Setting extra header words is not generally supported
    // as it could corrupt the heap
    // Fail to Smalltalk
    return PrimitiveResult::Failure;
}

// Primitive 530: Immediate as integer
// anImmediate primitiveImmediateAsInteger -> integerValue
// Returns the raw integer encoding of an immediate
PrimitiveResult Interpreter::primitiveImmediateAsInteger(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop receiver = stackTop();

    // Extract the underlying bits as an integer
    // This is useful for debugging and low-level operations
    if (receiver.isSmallInteger()) {
        // Already an integer, return as-is
        return PrimitiveResult::Success;  // receiver is already on stack
    }

    if (receiver.isCharacter()) {
        pop();
        push(Oop::fromSmallInteger(receiver.asCharacter()));
        return PrimitiveResult::Success;
    }

    if (receiver.isSmallFloat()) {
        // Return the SmallFloat encoding as a signed integer
        // In our low-bit tag scheme: value = oop >> 3 (arithmetic shift)
        int64_t value = static_cast<int64_t>(receiver.rawBits()) >> 3;
        if (Oop::canBeSmallInteger(value)) {
            pop();
            push(Oop::fromSmallInteger(value));
            return PrimitiveResult::Success;
        }
        // Extremely large/small SmallFloat encodings - fail to Smalltalk
        return PrimitiveResult::Failure;
    }

    // Non-immediate objects fail
    return PrimitiveResult::Failure;
}

// ===== STRING/ENCODING PRIMITIVES (531-534) =====

// Primitive 531: String encode
// aString encoding primitiveStringEncode -> encodedBytes
// Converts a string to bytes in specified encoding
PrimitiveResult Interpreter::primitiveStringEncode(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop encodingOop = stackTop();
    Oop stringOop = stackValue(1);

    if (stringOop.isImmediate()) {
        return PrimitiveResult::Failure;
    }

    // For simplicity, only support UTF-8 (encoding 0) and Latin-1 (encoding 1)
    // In reality, Smalltalk has a rich encoding system
    // Fail to let Smalltalk handle complex encodings
    return PrimitiveResult::Failure;
}

// Primitive 532: String decode
// aByteArray encoding primitiveStringDecode -> string
// Converts bytes in specified encoding to a string
PrimitiveResult Interpreter::primitiveStringDecode(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    // Similar to encode, fail to Smalltalk for complex handling
    return PrimitiveResult::Failure;
}

// Primitive 533: Character ASCII value
// aCharacter primitiveCharacterAsciiValue -> integer
// Returns the ASCII/Unicode code point of a character
PrimitiveResult Interpreter::primitiveCharacterAsciiValue(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop receiver = stackTop();

    // For Character immediate
    if (receiver.isCharacter()) {
        pop();
        push(Oop::fromSmallInteger(receiver.asCharacter()));
        return PrimitiveResult::Success;
    }

    // For Character object (legacy boxed characters)
    if (!receiver.isImmediate()) {
        // Character objects have their value stored in first slot
        Oop value = memory_.fetchPointer(0, receiver);
        if (value.isSmallInteger()) {
            pop();
            push(value);
            return PrimitiveResult::Success;
        }
    }

    return PrimitiveResult::Failure;
}

// Primitive 534: All objects in memory (debugging/development)
// primitiveAllObjectsInMemory -> array
// Returns an array of all objects currently in the heap
PrimitiveResult Interpreter::primitiveAllObjectsInMemory(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // This is an expensive operation - collects all heap objects
    std::vector<Oop> allObjects;

    memory_.allObjectsDo([&](Oop obj) {
        if (!obj.isImmediate() && obj.isObject()) {
            ObjectHeader* hdr = obj.asObjectPtr();
            uint32_t cls = hdr->classIndex();
            if (cls != 0) {
                Oop classOop = memory_.classAtIndex(cls);
                if (classOop.isObject() && !classOop.isNil()) {
                    allObjects.push_back(obj);
                }
            }
        }
    });

    // Allocate result array
    uint32_t arrayClassIndex = memory_.indexOfClass(
        memory_.specialObject(SpecialObjectIndex::ClassArray));

    Oop result = memory_.allocateSlots(arrayClassIndex, allObjects.size(), ObjectFormat::Indexable);
    if (result.isNil()) {
        return PrimitiveResult::Failure;
    }

    // Fill result array
    for (size_t i = 0; i < allObjects.size(); i++) {
        memory_.storePointer(i, result, allObjects[i]);
    }

    pop();  // receiver
    push(result);
    return PrimitiveResult::Success;
}

// ===== REFLECTION PRIMITIVES (535-538) =====

// Primitive 535: Object slot at (0-based raw access)
// anObject index primitiveObjectSlotAt -> value
// Raw slot access without inst var mapping
PrimitiveResult Interpreter::primitiveObjectSlotAt(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    Oop receiver = stackValue(1);

    if (!indexOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    if (receiver.isImmediate()) {
        return PrimitiveResult::Failure;
    }

    int64_t index = indexOop.asSmallInteger();
    if (index < 0) {
        return PrimitiveResult::Failure;
    }

    size_t slotCount = memory_.slotCountOf(receiver);
    if (static_cast<size_t>(index) >= slotCount) {
        return PrimitiveResult::Failure;
    }

    Oop value = memory_.fetchPointer(static_cast<size_t>(index), receiver);
    popN(2);  // pop index and receiver
    push(value);
    return PrimitiveResult::Success;
}

// Primitive 536: Object slot at put (0-based raw access)
// anObject index value primitiveObjectSlotAtPut -> value
// Raw slot store without inst var mapping
PrimitiveResult Interpreter::primitiveObjectSlotAtPut(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop value = stackTop();
    Oop indexOop = stackValue(1);
    Oop receiver = stackValue(2);

    if (!indexOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    if (receiver.isImmediate()) {
        return PrimitiveResult::Failure;
    }

    if (memory_.isImmutable(receiver)) {
        return PrimitiveResult::Failure;
    }

    int64_t index = indexOop.asSmallInteger();
    if (index < 0) {
        return PrimitiveResult::Failure;
    }

    size_t slotCount = memory_.slotCountOf(receiver);
    if (static_cast<size_t>(index) >= slotCount) {
        return PrimitiveResult::Failure;
    }

    memory_.storePointer(static_cast<size_t>(index), receiver, value);
    popN(3);  // pop value, index, and receiver
    push(value);
    return PrimitiveResult::Success;
}

// Primitive 537: Object num slots
// anObject primitiveObjectNumSlots -> integer
// Returns the total number of slots in an object
PrimitiveResult Interpreter::primitiveObjectNumSlots(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop receiver = stackTop();

    if (receiver.isImmediate()) {
        // Immediates have 0 slots
        pop();
        push(Oop::fromSmallInteger(0));
        return PrimitiveResult::Success;
    }

    size_t slotCount = memory_.slotCountOf(receiver);
    pop();
    push(Oop::fromSmallInteger(static_cast<int64_t>(slotCount)));
    return PrimitiveResult::Success;
}

// Primitive 538: Object format
// anObject primitiveObjectFormat -> formatCode
// Returns the object format code from the header
PrimitiveResult Interpreter::primitiveObjectFormat(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop receiver = stackTop();

    if (receiver.isImmediate()) {
        // Return a special code for immediates
        // SmallInteger = -1, Character = -2, SmallFloat = -3
        int64_t code;
        if (receiver.isSmallInteger()) {
            code = -1;
        } else if (receiver.isCharacter()) {
            code = -2;
        } else {
            code = -3;  // SmallFloat
        }
        pop();
        push(Oop::fromSmallInteger(code));
        return PrimitiveResult::Success;
    }

    // Get format from object header
    ObjectHeader* header = receiver.asObjectPtr();
    int64_t format = static_cast<int64_t>(header->format());
    pop();
    push(Oop::fromSmallInteger(format));
    return PrimitiveResult::Success;
}

// ===== ADVANCED OBJECT PRIMITIVES (539-550) =====

// Primitive 539: Get object's class
// anObject primitiveObjectClass -> class
PrimitiveResult Interpreter::primitiveObjectClass(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop receiver = stackTop();
    Oop classOop = memory_.classOf(receiver);

    pop();
    push(classOop);
    return PrimitiveResult::Success;
}

// Primitive 540: Get object's class index
// anObject primitiveObjectClassIndex -> smallInteger
PrimitiveResult Interpreter::primitiveObjectClassIndex(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop receiver = stackTop();

    uint32_t classIndex = 0;
    if (receiver.isObject()) {
        ObjectHeader* header = receiver.asObjectPtr();
        classIndex = header->classIndex();
    } else if (receiver.isSmallInteger()) {
        // SmallInteger class index
        classIndex = static_cast<uint32_t>(SpecialObjectIndex::ClassSmallInteger);
    } else if (receiver.isCharacter()) {
        classIndex = static_cast<uint32_t>(SpecialObjectIndex::ClassCharacter);
    } else if (receiver.isSmallFloat()) {
        classIndex = static_cast<uint32_t>(SpecialObjectIndex::ClassFloat);
    }

    pop();
    push(Oop::fromSmallInteger(classIndex));
    return PrimitiveResult::Success;
}

// Primitive 541: Check if object is pinned
// anObject primitiveObjectIsPinned -> boolean
PrimitiveResult Interpreter::primitiveObjectIsPinned(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop receiver = stackTop();

    bool isPinned = false;
    if (receiver.isObject()) {
        isPinned = memory_.isPinned(receiver);
    }

    pop();
    push(isPinned ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

// Primitive 542: Set object pinned state
// anObject boolean primitiveObjectSetPinned -> anObject
PrimitiveResult Interpreter::primitiveObjectSetPinned(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop pinOop = stackTop();
    Oop receiver = stackValue(1);

    if (!receiver.isObject()) {
        return PrimitiveResult::Failure;
    }

    bool pin = (pinOop == memory_.trueObject());
    if (pin) {
        memory_.pinObject(receiver);
    }
    // Note: unpinning would need additional support

    pop();  // pin flag
    // Leave receiver on stack
    return PrimitiveResult::Success;
}

// Primitive 543: Check if object is read-only (immutable)
// anObject primitiveObjectIsReadOnly -> boolean
PrimitiveResult Interpreter::primitiveObjectIsReadOnly(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop receiver = stackTop();

    bool isReadOnly = false;
    if (receiver.isObject()) {
        isReadOnly = memory_.isImmutable(receiver);
    } else {
        // Immediates are always immutable
        isReadOnly = true;
    }

    pop();
    push(isReadOnly ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

// Primitive 544: Set object read-only state
// anObject boolean primitiveObjectSetReadOnly -> anObject
PrimitiveResult Interpreter::primitiveObjectSetReadOnly(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop readOnlyOop = stackTop();
    Oop receiver = stackValue(1);

    if (!receiver.isObject()) {
        return PrimitiveResult::Failure;
    }

    bool makeReadOnly = (readOnlyOop == memory_.trueObject());
    receiver.asObjectPtr()->setImmutable(makeReadOnly);

    pop();  // readOnly flag
    // Leave receiver on stack
    return PrimitiveResult::Success;
}

// Primitive 545: Get byte size of object's data
// anObject primitiveObjectBytesSize -> smallInteger
PrimitiveResult Interpreter::primitiveObjectBytesSize(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop receiver = stackTop();

    size_t byteSize = 0;
    if (receiver.isObject()) {
        byteSize = memory_.byteSizeOf(receiver);
    }

    pop();
    push(Oop::fromSmallInteger(static_cast<intptr_t>(byteSize)));
    return PrimitiveResult::Success;
}

// Primitive 546: Get word (64-bit) count of object
// anObject primitiveObjectWordsSize -> smallInteger
PrimitiveResult Interpreter::primitiveObjectWordsSize(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop receiver = stackTop();

    size_t wordSize = 0;
    if (receiver.isObject()) {
        wordSize = memory_.byteSizeOf(receiver) / 8;
    }

    pop();
    push(Oop::fromSmallInteger(static_cast<intptr_t>(wordSize)));
    return PrimitiveResult::Success;
}

// Primitive 547: Get pointer slot count of object
// anObject primitiveObjectPointersSize -> smallInteger
PrimitiveResult Interpreter::primitiveObjectPointersSize(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop receiver = stackTop();

    size_t slotCount = 0;
    if (receiver.isObject()) {
        slotCount = memory_.slotCountOf(receiver);
    }

    pop();
    push(Oop::fromSmallInteger(static_cast<intptr_t>(slotCount)));
    return PrimitiveResult::Success;
}

// Primitive 548: Get raw object header
// anObject primitiveObjectHeader -> smallInteger
PrimitiveResult Interpreter::primitiveObjectHeader(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop receiver = stackTop();

    if (!receiver.isObject()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* header = receiver.asObjectPtr();
    uint64_t headerBits = *reinterpret_cast<uint64_t*>(header);

    Oop result = uint64ToOop(memory_, headerBits);
    if (result.isNil()) return PrimitiveResult::Failure;
    pop();
    push(result);
    return PrimitiveResult::Success;
}

// Primitive 549: Set raw object header (dangerous!)
// anObject headerValue primitiveObjectHeaderPut -> anObject
PrimitiveResult Interpreter::primitiveObjectHeaderPut(int argCount) {
    // This is a dangerous primitive - fail for safety
    return PrimitiveResult::Failure;
}

// Primitive 550: Identity hash that works for SmallIntegers too
// anObject primitiveIdentityHashSmallInteger -> smallInteger
PrimitiveResult Interpreter::primitiveIdentityHashSmallInteger(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop receiver = stackTop();

    uint32_t hash;
    if (receiver.isSmallInteger()) {
        // For SmallIntegers, use the value itself as the hash
        hash = static_cast<uint32_t>(receiver.asSmallInteger() & 0x3FFFFFFF);
    } else if (receiver.isCharacter()) {
        hash = receiver.asCharacter();
    } else if (receiver.isObject()) {
        hash = memory_.identityHashOf(receiver);
    } else {
        // SmallFloat - use bits as hash
        hash = static_cast<uint32_t>(receiver.rawBits() >> 3);
    }

    pop();
    push(Oop::fromSmallInteger(hash));
    return PrimitiveResult::Success;
}

// ===== COMPILED METHOD PRIMITIVES (551-560) =====

// Primitive 551: Get number of literals in compiled method
// aMethod primitiveCompiledMethodNumLiterals -> smallInteger
PrimitiveResult Interpreter::primitiveCompiledMethodNumLiterals(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop methodOop = stackTop();
    if (!methodOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Method header is at slot 0, contains numLiterals in low bits
    Oop headerOop = memory_.fetchPointer(0, methodOop);
    if (!headerOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    intptr_t header = headerOop.asSmallInteger();
    // Literal count is in bits 0-14 of the header
    intptr_t numLiterals = header & 0x7FFF;

    pop();
    push(Oop::fromSmallInteger(numLiterals));
    return PrimitiveResult::Success;
}

// Primitive 552: Get literal at index from compiled method
// aMethod index primitiveCompiledMethodLiteralAt -> literal
PrimitiveResult Interpreter::primitiveCompiledMethodLiteralAt(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    Oop methodOop = stackValue(1);

    if (!indexOop.isSmallInteger() || !methodOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    intptr_t index = indexOop.asSmallInteger();
    if (index < 1) {
        return PrimitiveResult::Failure;
    }

    // Literals start at slot 1 (slot 0 is the header)
    Oop literal = memory_.fetchPointer(static_cast<size_t>(index), methodOop);

    popN(2);
    push(literal);
    return PrimitiveResult::Success;
}

// Primitive 553: Set literal at index in compiled method
// aMethod index literal primitiveCompiledMethodLiteralAtPut -> aMethod
PrimitiveResult Interpreter::primitiveCompiledMethodLiteralAtPut(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop literalOop = stackTop();
    Oop indexOop = stackValue(1);
    Oop methodOop = stackValue(2);

    if (!indexOop.isSmallInteger() || !methodOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    intptr_t index = indexOop.asSmallInteger();
    if (index < 1) {
        return PrimitiveResult::Failure;
    }

    memory_.storePointer(static_cast<size_t>(index), methodOop, literalOop);

    popN(3);
    push(methodOop);
    return PrimitiveResult::Success;
}

// Primitive 554: Get bytecode at offset from compiled method
// aMethod offset primitiveCompiledMethodBytecodeAt -> byte
PrimitiveResult Interpreter::primitiveCompiledMethodBytecodeAt(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop offsetOop = stackTop();
    Oop methodOop = stackValue(1);

    if (!offsetOop.isSmallInteger() || !methodOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    intptr_t offset = offsetOop.asSmallInteger();
    if (offset < 0) {
        return PrimitiveResult::Failure;
    }

    uint8_t byte = memory_.fetchByte(static_cast<size_t>(offset), methodOop);

    popN(2);
    push(Oop::fromSmallInteger(byte));
    return PrimitiveResult::Success;
}

// Primitive 555: Set bytecode at offset in compiled method
// aMethod offset byte primitiveCompiledMethodBytecodeAtPut -> aMethod
PrimitiveResult Interpreter::primitiveCompiledMethodBytecodeAtPut(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop byteOop = stackTop();
    Oop offsetOop = stackValue(1);
    Oop methodOop = stackValue(2);

    if (!byteOop.isSmallInteger() || !offsetOop.isSmallInteger() || !methodOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    intptr_t offset = offsetOop.asSmallInteger();
    intptr_t byteVal = byteOop.asSmallInteger();
    if (offset < 0 || byteVal < 0 || byteVal > 255) {
        return PrimitiveResult::Failure;
    }

    memory_.storeByte(static_cast<size_t>(offset), methodOop, static_cast<uint8_t>(byteVal));

    popN(3);
    push(methodOop);
    return PrimitiveResult::Success;
}

// Primitive 556: Get number of arguments of compiled method
// aMethod primitiveCompiledMethodNumArgs -> smallInteger
PrimitiveResult Interpreter::primitiveCompiledMethodNumArgs(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop methodOop = stackTop();
    if (!methodOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    Oop headerOop = memory_.fetchPointer(0, methodOop);
    if (!headerOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    intptr_t header = headerOop.asSmallInteger();
    // NumArgs is in bits 24-27 of the method header
    intptr_t numArgs = (header >> 24) & 0xF;

    pop();
    push(Oop::fromSmallInteger(numArgs));
    return PrimitiveResult::Success;
}

// Primitive 557: Get number of temps of compiled method
// aMethod primitiveCompiledMethodNumTemps -> smallInteger
PrimitiveResult Interpreter::primitiveCompiledMethodNumTemps(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop methodOop = stackTop();
    if (!methodOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    Oop headerOop = memory_.fetchPointer(0, methodOop);
    if (!headerOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    intptr_t header = headerOop.asSmallInteger();
    // NumTemps is in bits 18-23 of the method header
    intptr_t numTemps = (header >> 18) & 0x3F;

    pop();
    push(Oop::fromSmallInteger(numTemps));
    return PrimitiveResult::Success;
}

// Primitive 558: Get frame size needed for compiled method
// aMethod primitiveCompiledMethodFrameSize -> smallInteger
PrimitiveResult Interpreter::primitiveCompiledMethodFrameSize(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop methodOop = stackTop();
    if (!methodOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    Oop headerOop = memory_.fetchPointer(0, methodOop);
    if (!headerOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    intptr_t header = headerOop.asSmallInteger();
    // Frame size flag is in bit 17
    bool largeFrame = (header >> 17) & 1;
    intptr_t frameSize = largeFrame ? 56 : 16;

    pop();
    push(Oop::fromSmallInteger(frameSize));
    return PrimitiveResult::Success;
}

// Primitive 559: Get primitive index of compiled method
// aMethod primitiveCompiledMethodPrimitive -> smallInteger
PrimitiveResult Interpreter::primitiveCompiledMethodPrimitive(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop methodOop = stackTop();
    if (!methodOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Use the existing primitiveIndexOf() which correctly reads from bytecodes
    int primitiveIndex = primitiveIndexOf(methodOop);

    pop();
    push(Oop::fromSmallInteger(primitiveIndex));
    return PrimitiveResult::Success;
}

// Primitive 560: Get selector of compiled method (last literal)
// aMethod primitiveCompiledMethodSelector -> selector
PrimitiveResult Interpreter::primitiveCompiledMethodSelector(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop methodOop = stackTop();
    if (!methodOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    Oop headerOop = memory_.fetchPointer(0, methodOop);
    if (!headerOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    intptr_t header = headerOop.asSmallInteger();
    intptr_t numLiterals = header & 0x7FFF;

    // Selector is typically the last literal (or second to last in some encodings)
    Oop selector = Oop::nil();
    if (numLiterals > 0) {
        selector = memory_.fetchPointer(static_cast<size_t>(numLiterals), methodOop);
    }

    pop();
    push(selector);
    return PrimitiveResult::Success;
}

// ===== SYSTEM AND DEBUG PRIMITIVES (561-570) =====

// Primitive 561: Get heap statistics
// primitiveVMHeapStatistics -> array of stats
PrimitiveResult Interpreter::primitiveVMHeapStatistics(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Return simple statistics as array
    // For now, just return nil - would need to allocate an array
    pop();
    push(memory_.nil());
    return PrimitiveResult::Success;
}

// Primitive 562: Get GC statistics
// primitiveVMGCStatistics -> array of stats
PrimitiveResult Interpreter::primitiveVMGCStatistics(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    auto stats = memory_.statistics();

    // Return just the GC count for now
    pop();
    push(Oop::fromSmallInteger(static_cast<intptr_t>(stats.gcCount)));
    return PrimitiveResult::Success;
}

// Primitive 563: Get current stack depth
// primitiveVMStackDepth -> smallInteger
PrimitiveResult Interpreter::primitiveVMStackDepth(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Calculate stack depth from stack pointer
    intptr_t depth = static_cast<intptr_t>(stackPointer_ - stack_.data());

    pop();
    push(Oop::fromSmallInteger(depth));
    return PrimitiveResult::Success;
}

// Primitive 564: Get bytecode execution count
// primitiveVMBytecodeCount -> largeInteger
PrimitiveResult Interpreter::primitiveVMBytecodeCount(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Bytecode count not tracked in this VM
    pop();
    push(Oop::fromSmallInteger(0));
    return PrimitiveResult::Success;
}

// Primitive 565: Get message send count
// primitiveVMSendCount -> largeInteger
PrimitiveResult Interpreter::primitiveVMSendCount(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Send count not tracked in this VM
    pop();
    push(Oop::fromSmallInteger(0));
    return PrimitiveResult::Success;
}

// Primitive 566: Get primitive call count
// primitiveVMPrimitiveCount -> largeInteger
PrimitiveResult Interpreter::primitiveVMPrimitiveCount(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Primitive count not tracked in this VM
    pop();
    push(Oop::fromSmallInteger(0));
    return PrimitiveResult::Success;
}

// Primitive 567: Get context switch count
// primitiveVMContextSwitchCount -> largeInteger
PrimitiveResult Interpreter::primitiveVMContextSwitchCount(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Context switch count not tracked in this VM
    pop();
    push(Oop::fromSmallInteger(0));
    return PrimitiveResult::Success;
}

// Primitive 568: Get VM uptime in milliseconds
// primitiveVMUptime -> largeInteger
PrimitiveResult Interpreter::primitiveVMUptime(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Get uptime using clock
    auto now = std::chrono::steady_clock::now();
    static auto startTime = now;
    auto uptime = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count();

    pop();
    push(Oop::fromSmallInteger(static_cast<intptr_t>(uptime)));
    return PrimitiveResult::Success;
}

// Primitive 569: Get CPU time used in milliseconds
// primitiveVMCPUTime -> largeInteger
PrimitiveResult Interpreter::primitiveVMCPUTime(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Get CPU time
    clock_t cpuTime = clock();
    intptr_t milliseconds = (cpuTime * 1000) / CLOCKS_PER_SEC;

    pop();
    push(Oop::fromSmallInteger(milliseconds));
    return PrimitiveResult::Success;
}

// Primitive 570: Get idle time in milliseconds
// primitiveVMIdleTime -> largeInteger
PrimitiveResult Interpreter::primitiveVMIdleTime(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Idle time not tracked
    pop();
    push(Oop::fromSmallInteger(0));
    return PrimitiveResult::Success;
}

// ===== ADDITIONAL BIT PRIMITIVES (571-574) =====

// Primitive 571: Count number of 1 bits (popcount)
// anInteger primitiveBitCount -> smallInteger
PrimitiveResult Interpreter::primitiveBitCount(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop receiver = stackTop();
    if (!receiver.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t value = receiver.asSmallInteger();
    // Handle negative numbers by counting bits in two's complement
    uint64_t bits = static_cast<uint64_t>(value < 0 ? -value : value);
    int count = __builtin_popcountll(bits);

    pop();
    push(Oop::fromSmallInteger(count));
    return PrimitiveResult::Success;
}

// Primitive 572: Reverse bits
// anInteger primitiveBitReverse -> integer
PrimitiveResult Interpreter::primitiveBitReverse(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop receiver = stackTop();
    if (!receiver.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    uint64_t value = static_cast<uint64_t>(receiver.asSmallInteger());

    // Reverse bits using standard technique
    uint64_t result = 0;
    for (int i = 0; i < 64; i++) {
        result = (result << 1) | (value & 1);
        value >>= 1;
    }

    Oop res = uint64ToOop(memory_, result);
    if (res.isNil()) return PrimitiveResult::Failure;
    pop();
    push(res);
    return PrimitiveResult::Success;
}

// Primitive 573: Swap bytes in 32-bit integer
// anInteger primitiveByteSwap32 -> integer
PrimitiveResult Interpreter::primitiveByteSwap32(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop receiver = stackTop();
    if (!receiver.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    uint32_t value = static_cast<uint32_t>(receiver.asSmallInteger());

    // Byte swap: ABCD -> DCBA
    uint32_t result = ((value & 0xFF000000) >> 24) |
                      ((value & 0x00FF0000) >> 8) |
                      ((value & 0x0000FF00) << 8) |
                      ((value & 0x000000FF) << 24);

    pop();
    push(Oop::fromSmallInteger(result));
    return PrimitiveResult::Success;
}

// Primitive 574: Swap bytes in 64-bit integer
// anInteger primitiveByteSwap64 -> integer
PrimitiveResult Interpreter::primitiveByteSwap64(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop receiver = stackTop();
    if (!receiver.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    uint64_t value = static_cast<uint64_t>(receiver.asSmallInteger());

    // Byte swap using built-in
    uint64_t result = __builtin_bswap64(value);

    Oop res = uint64ToOop(memory_, result);
    if (res.isNil()) return PrimitiveResult::Failure;
    pop();
    push(res);
    return PrimitiveResult::Success;
}

// ===== TIME/TIMEZONE PRIMITIVES (242-246) - OFFICIAL VM PRIMITIVES =====
//
// These match the official Pharo VM primitive table:
// 242: primitiveSignalAtUTCMicroseconds
// 243: primitiveUpdateTimezone
// 244: primitiveUtcAndTimezoneOffset
// 245: primitiveCoarseUTCMicrosecondClock
// 246: primitiveCoarseLocalMicrosecondClock

// Smalltalk epoch offset: seconds from Unix epoch (1970-01-01) to Smalltalk epoch (1901-01-01)
// This is approximately 2177452800 seconds
static constexpr int64_t SmalltalkEpochOffset = 2177452800LL;
static constexpr int64_t SmalltalkEpochOffsetMicroseconds = SmalltalkEpochOffset * 1000000LL;

// Primitive 242: Signal semaphore at UTC microsecond time
// Called as: ticker primSignal: sema atUTCMicroseconds: usecs
// Signals the timer semaphore when UTC clock reaches usecs. Value 0 disables.
// Stack: receiver ticker, arg1 sema, arg2 usecs
PrimitiveResult Interpreter::primitiveSignalAtUTCMicroseconds(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop usecsOop = stackTop();        // usecs is 2nd argument (stackValue(0))
    Oop sema = stackValue(1);         // semaphore is 1st argument (stackValue(1))

    // usecs can be a SmallInteger or a LargePositiveInteger
    int64_t usecs = 0;
    if (usecsOop.isSmallInteger()) {
        usecs = usecsOop.asSmallInteger();
    } else if (usecsOop.isObject() && usecsOop.rawBits() > 0x10000) {
        // Try to read as a LargePositiveInteger
        ObjectHeader* hdr = usecsOop.asObjectPtr();
        if (hdr->isBytesObject()) {
            size_t byteLen = hdr->byteSize();
            if (byteLen <= 8) {
                uint8_t* bytes = (uint8_t*)hdr->bytes();
                usecs = 0;
                for (size_t i = 0; i < byteLen; i++) {
                    usecs |= ((int64_t)bytes[i]) << (i * 8);
                }
            } else {
                return PrimitiveResult::Failure;  // Too large
            }
        } else {
            return PrimitiveResult::Failure;
        }
    } else {
        return PrimitiveResult::Failure;
    }

    // Store the timer info
    static int set242Count = 0;
    if (usecs == 0 || sema.isNil()) {
        // Disable timer
        timerSemaphore_ = Oop::nil();
        nextWakeupUsec_ = INT64_MAX;
    } else {
        // Detect conflict: ms timer also armed?
        if (nextWakeupTime_ != 0) {
            fprintf(stderr, "[TIMER-CONFLICT] prim242 arming while ms timer also armed! msTarget=%lld\n",
                    (long long)nextWakeupTime_);
        }
        // Schedule the timer
        timerSemaphore_ = sema;
        nextWakeupUsec_ = usecs;
        set242Count++;
    }

    popN(2);  // Pop both arguments, leave receiver
    return PrimitiveResult::Success;
}

// Primitive 243: Update timezone info
// Refresh the VM's notion of the current timezone
PrimitiveResult Interpreter::primitiveUpdateTimezone(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Force timezone update by calling tzset()
    // This updates the TZ environment variable handling
    tzset();

    // Return receiver (no change to stack)
    return PrimitiveResult::Success;
}

// Primitive 244: Get UTC time and timezone offset together
// receiver primitiveUtcAndTimezoneOffset -> array or fills provided array
// Returns {UTC microseconds since Smalltalk epoch, timezone offset in seconds}
PrimitiveResult Interpreter::primitiveUtcAndTimezoneOffset(int argCount) {
    Oop resultArray;

    if (argCount == 0) {
        // Allocate new 2-element array
        uint32_t arrayClassIndex = memory_.indexOfClass(
            memory_.specialObject(SpecialObjectIndex::ClassArray));
        resultArray = memory_.allocateSlots(arrayClassIndex, 2, ObjectFormat::Indexable);
        if (resultArray.isNil()) {
            return PrimitiveResult::Failure;
        }
    } else if (argCount == 1) {
        // Use provided array
        resultArray = stackTop();
        if (!resultArray.isObject()) {
            return PrimitiveResult::Failure;
        }
        ObjectHeader* hdr = resultArray.asObjectPtr();
        if (hdr->slotCount() < 2) {
            return PrimitiveResult::Failure;
        }
    } else {
        return PrimitiveResult::Failure;
    }

    // Get current UTC time
    auto now = std::chrono::system_clock::now();
    auto unixUs = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();
    int64_t smalltalkUs = unixUs + SmalltalkEpochOffsetMicroseconds;

    // Get timezone offset
    time_t nowTime = time(nullptr);
    struct tm* local = localtime(&nowTime);
    int64_t offsetSeconds = local->tm_gmtoff;

    // Store results
    memory_.storePointer(0, resultArray, Oop::fromSmallInteger(smalltalkUs));
    memory_.storePointer(1, resultArray, Oop::fromSmallInteger(offsetSeconds));

    popN(argCount + 1);  // Pop args and receiver
    push(resultArray);
    return PrimitiveResult::Success;
}

// Primitive 245: Coarse (cached) UTC microsecond clock
// Returns the UTC microsecond clock value cached by the heartbeat.
// Faster but less precise than primitiveUTCMicrosecondClock (240).
PrimitiveResult Interpreter::primitiveCoarseUTCMicrosecondClock(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // For simplicity, we just return the current time.
    // A full implementation would cache this in the heartbeat for speed.
    auto now = std::chrono::system_clock::now();
    auto unixUs = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();
    int64_t smalltalkUs = unixUs + SmalltalkEpochOffsetMicroseconds;

    pop();  // receiver
    push(Oop::fromSmallInteger(smalltalkUs));
    return PrimitiveResult::Success;
}

// Primitive 246: Coarse (cached) local microsecond clock
// Returns the local microsecond clock value cached by the heartbeat.
// Faster but less precise than primitiveLocalMicrosecondClock (241).
PrimitiveResult Interpreter::primitiveCoarseLocalMicrosecondClock(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // For simplicity, we just call the UTC clock.
    // The difference between UTC and local is handled by the image.
    auto now = std::chrono::system_clock::now();
    auto unixUs = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();
    int64_t smalltalkUs = unixUs + SmalltalkEpochOffsetMicroseconds;

    pop();  // receiver
    push(Oop::fromSmallInteger(smalltalkUs));
    return PrimitiveResult::Success;
}

// Named primitive: primitiveUtcWithOffset
// Used by DateAndTime class>>now via <primitive: 'primitiveUtcWithOffset' module: ''>
// Returns a 2-element array: {UTC microseconds since Posix epoch, timezone offset in seconds}
PrimitiveResult Interpreter::primitiveUtcWithOffset(int argCount) {
    Oop resultArray;

    if (argCount > 1) return PrimitiveResult::Failure;

    if (argCount == 1) {
        resultArray = stackTop();
        if (!resultArray.isObject()) return PrimitiveResult::Failure;
        ObjectHeader* hdr = resultArray.asObjectPtr();
        if (hdr->slotCount() < 2) return PrimitiveResult::Failure;
    } else {
        uint32_t arrayClassIndex = memory_.indexOfClass(
            memory_.specialObject(SpecialObjectIndex::ClassArray));
        if (arrayClassIndex == 0) {
            arrayClassIndex = memory_.registerClass(
                memory_.specialObject(SpecialObjectIndex::ClassArray));
        }
        resultArray = memory_.allocateSlots(arrayClassIndex, 2, ObjectFormat::Indexable);
        if (resultArray.isNil() || resultArray.rawBits() == memory_.nil().rawBits()) {
            return PrimitiveResult::Failure;
        }
    }

    // Get UTC microseconds since Posix epoch (Jan 1, 1970)
    auto now = std::chrono::system_clock::now();
    auto unixUs = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();

    // Get timezone offset in seconds
    time_t nowTime = time(nullptr);
    struct tm* local = localtime(&nowTime);
    int64_t offsetSeconds = local->tm_gmtoff;

    // Store UTC microseconds since Posix epoch (NOT Smalltalk epoch)
    // The image expects Posix epoch microseconds here
    memory_.storePointer(0, resultArray, Oop::fromSmallInteger(unixUs));
    memory_.storePointer(1, resultArray, Oop::fromSmallInteger(offsetSeconds));

    popN(argCount + 1);
    push(resultArray);
    return PrimitiveResult::Success;
}

// ===== VM PROFILING PRIMITIVES (250-253) =====
//
// These are primarily for JIT/Cog VMs. For our interpreter-only VM,
// most of these fail or return minimal data.

// Primitive 250: Clear VM profiling data
PrimitiveResult Interpreter::primitiveClearVMProfile(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;
    // No profiling data to clear in interpreter-only VM
    return PrimitiveResult::Success;
}

// Primitive 251: Start/stop VM profiling
PrimitiveResult Interpreter::primitiveControlVMProfiling(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;
    // Profiling not supported in interpreter-only VM
    // Just succeed silently
    pop();  // Pop argument, leave receiver
    return PrimitiveResult::Success;
}

// primitiveVMProfileSamplesInto (252) is implemented below in PROFILING PRIMITIVES section

// Primitive 253: Collect JIT code constituents
// Returns information about compiled machine code (Cog-specific)
PrimitiveResult Interpreter::primitiveCollectCogCodeConstituents(int argCount) {
    // Cog JIT-specific, not applicable for interpreter-only VM
    return PrimitiveResult::Failure;
}

// ===== MISC PRIMITIVES (222-230) =====

// Primitive 222: Closure value no context switch (variant 2)
// closure primitiveClosureValueNoContextSwitch2 -> result
// Evaluates closure without allowing context switch
PrimitiveResult Interpreter::primitiveClosureValueNoContextSwitch2(int argCount) {
    // This is similar to primitiveClosureValueNoContextSwitch (204)
    // but may have slightly different semantics
    // For now, fail to Smalltalk fallback
    return PrimitiveResult::Failure;
}

// Primitive 223: Closure value with args, no context switch
// closure argsArray primitiveClosureValueWithArgsNoContextSwitch -> result
// Evaluates closure with args without allowing context switch
PrimitiveResult Interpreter::primitiveClosureValueWithArgsNoContextSwitch(int argCount) {
    // Similar to primitiveClosureValueWithArgs but prevents context switching
    // For now, fail to Smalltalk fallback
    return PrimitiveResult::Failure;
}

// Primitive 224: Set identity hash
// Primitive 161: setOrHasIdentityHash
// Supports 0, 1, or 2 arguments:
// - 0 args: Check if receiver has identity hash (return boolean)
// - 1 arg: Set identity hash to given value (return receiver)
// - 2 args: Complex behavior with boolean flag
PrimitiveResult Interpreter::primitiveSetOrHasIdentityHash(int argCount) {
    if (argCount == 0) {
        // Check if receiver has identity hash
        Oop receiver = stackTop();
        bool hasHash = false;
        if (receiver.isObject()) {
            ObjectHeader* header = receiver.asObjectPtr();
            hasHash = header->hasIdentityHash();
        }
        // Immediates never have hash (they ARE their hash conceptually)
        pop();
        push(hasHash ? memory_.trueObject() : memory_.falseObject());
        return PrimitiveResult::Success;
    }

    if (argCount == 1) {
        // Set identity hash
        Oop hashOop = stackTop();
        Oop receiver = stackValue(1);

        if (!hashOop.isSmallInteger()) {
            return PrimitiveResult::Failure;
        }

        if (!receiver.isObject()) {
            // Can't set hash on immediates
            return PrimitiveResult::Failure;
        }

        int64_t hash = hashOop.asSmallInteger();
        if (hash < 0 || hash > 0x3FFFFF) {  // 22-bit hash max
            return PrimitiveResult::Failure;
        }

        ObjectHeader* header = receiver.asObjectPtr();
        header->setIdentityHash(static_cast<uint32_t>(hash));

        popN(2);
        push(receiver);
        return PrimitiveResult::Success;
    }

    if (argCount == 2) {
        // Complex form: receiver setIdentityHash: hash isClass: aBoolean
        // The boolean indicates if receiver is a class (affects behavior hash)
        Oop boolOop = stackValue(0);
        Oop hashOop = stackValue(1);
        Oop receiver = stackValue(2);

        // Validate boolean
        if (boolOop.rawBits() != memory_.trueObject().rawBits() &&
            boolOop.rawBits() != memory_.falseObject().rawBits()) {
            return PrimitiveResult::Failure;
        }

        if (!hashOop.isSmallInteger() || !receiver.isObject()) {
            return PrimitiveResult::Failure;
        }

        int64_t hash = hashOop.asSmallInteger();
        if (hash < 0 || hash > 0x3FFFFF) {
            return PrimitiveResult::Failure;
        }

        ObjectHeader* header = receiver.asObjectPtr();
        header->setIdentityHash(static_cast<uint32_t>(hash));

        popN(3);
        push(receiver);
        return PrimitiveResult::Success;
    }

    return PrimitiveResult::Failure;
}

// Primitive 225: Load instance variable (optimized)
// receiver index primitiveLoadInstVar -> value
// Optimized load of instance variable by index
PrimitiveResult Interpreter::primitiveLoadInstVar(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    Oop receiver = stackValue(1);

    if (!indexOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    if (receiver.isImmediate()) {
        return PrimitiveResult::Failure;
    }

    int64_t index = indexOop.asSmallInteger();
    if (index < 0) {
        return PrimitiveResult::Failure;
    }

    size_t slotCount = memory_.slotCountOf(receiver);
    if (static_cast<size_t>(index) >= slotCount) {
        return PrimitiveResult::Failure;
    }

    Oop value = memory_.fetchPointer(static_cast<size_t>(index), receiver);
    popN(2);
    push(value);
    return PrimitiveResult::Success;
}

// Primitive 226: String compare (case-sensitive, returns ordering)
// string1 string2 primitiveStringCompare -> -1/0/1
// Returns -1 if string1 < string2, 0 if equal, 1 if string1 > string2
PrimitiveResult Interpreter::primitiveStringCompare(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop string2 = stackTop();
    Oop string1 = stackValue(1);

    if (string1.isImmediate() || string2.isImmediate()) {
        return PrimitiveResult::Failure;
    }

    // Extract strings
    std::string str1 = extractString(memory_, string1);
    std::string str2 = extractString(memory_, string2);

    int result;
    if (str1 < str2) {
        result = -1;
    } else if (str1 > str2) {
        result = 1;
    } else {
        result = 0;
    }

    popN(2);
    push(Oop::fromSmallInteger(result));
    return PrimitiveResult::Success;
}

// Primitive 227: String replace (optimized bulk copy)
// dest destStart destEnd source sourceStart primitiveStringReplace -> dest
// Copies elements from source to dest (works for both byte objects and pointer objects)
// This is primitive 105, used by replaceFrom:to:with:startingAt:
PrimitiveResult Interpreter::primitiveStringReplace(int argCount) {
    if (argCount != 4) return PrimitiveResult::Failure;

    Oop sourceStartOop = stackTop();
    Oop sourceOop = stackValue(1);
    Oop destEndOop = stackValue(2);
    Oop destStartOop = stackValue(3);
    Oop destOop = stackValue(4);

    // Validate integer arguments
    if (!sourceStartOop.isSmallInteger() || !destEndOop.isSmallInteger() ||
        !destStartOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    if (destOop.isImmediate() || sourceOop.isImmediate()) {
        return PrimitiveResult::Failure;
    }

    if (memory_.isImmutable(destOop)) {
        return PrimitiveResult::Failure;
    }

    int64_t sourceStart = sourceStartOop.asSmallInteger();
    int64_t destStart = destStartOop.asSmallInteger();
    int64_t destEnd = destEndOop.asSmallInteger();

    // Convert to 0-based indices
    if (sourceStart < 1 || destStart < 1 || destEnd < destStart - 1) {
        return PrimitiveResult::Failure;
    }

    size_t srcIdx = static_cast<size_t>(sourceStart - 1);
    size_t dstStartIdx = static_cast<size_t>(destStart - 1);
    size_t count = static_cast<size_t>(destEnd - destStart + 1);

    ObjectHeader* destHdr = destOop.asObjectPtr();
    ObjectHeader* srcHdr = sourceOop.asObjectPtr();

    // Handle POINTER objects (Arrays, etc.)
    if (destHdr->isPointersObject() && srcHdr->isPointersObject()) {
        ObjectFormat destFmt = destHdr->format();
        ObjectFormat srcFmt = srcHdr->format();

        // Non-indexable objects (like Interval with FixedSize format) must not be
        // accessed by raw slot — fail so the Smalltalk fallback uses at: instead
        if (destFmt == ObjectFormat::FixedSize || destFmt == ObjectFormat::ZeroSized ||
            srcFmt == ObjectFormat::FixedSize || srcFmt == ObjectFormat::ZeroSized) {
            return PrimitiveResult::Failure;
        }

        // Account for fixed fields in IndexableWithFixed / WeakWithFixed objects
        size_t destFixed = 0, srcFixed = 0;
        if (destFmt == ObjectFormat::IndexableWithFixed || destFmt == ObjectFormat::WeakWithFixed) {
            Oop destClass = memory_.classOf(destOop);
            if (destClass.isObject()) {
                Oop instSpec = memory_.fetchPointer(2, destClass);
                if (instSpec.isSmallInteger()) destFixed = instSpec.asSmallInteger() & 0xFFFF;
            }
        }
        if (srcFmt == ObjectFormat::IndexableWithFixed || srcFmt == ObjectFormat::WeakWithFixed) {
            Oop srcClass = memory_.classOf(sourceOop);
            if (srcClass.isObject()) {
                Oop instSpec = memory_.fetchPointer(2, srcClass);
                if (instSpec.isSmallInteger()) srcFixed = instSpec.asSmallInteger() & 0xFFFF;
            }
        }

        size_t destSlots = destHdr->slotCount();
        size_t srcSlots = srcHdr->slotCount();
        size_t destIndexable = destSlots > destFixed ? destSlots - destFixed : 0;
        size_t srcIndexable = srcSlots > srcFixed ? srcSlots - srcFixed : 0;

        // Bounds check against indexable portion
        if (dstStartIdx + count > destIndexable || srcIdx + count > srcIndexable) {
            return PrimitiveResult::Failure;
        }

        // Copy slots (offset by fixed fields)
        for (size_t i = 0; i < count; i++) {
            Oop value = srcHdr->slotAt(srcFixed + srcIdx + i);
            destHdr->slotAtPut(destFixed + dstStartIdx + i, value);
        }

        popN(4);  // Pop 4 args, leave dest
        return PrimitiveResult::Success;
    }

    // Handle BYTE objects (Strings, ByteArrays, etc.)
    if (destHdr->isBytesObject() && srcHdr->isBytesObject()) {
        size_t destSize = destHdr->byteSize();
        size_t sourceSize = srcHdr->byteSize();

        // Bounds check
        if (dstStartIdx + count > destSize || srcIdx + count > sourceSize) {
            return PrimitiveResult::Failure;
        }

        // Copy bytes
        for (size_t i = 0; i < count; i++) {
            uint8_t byte = srcHdr->byteAt(srcIdx + i);
            destHdr->byteAtPut(dstStartIdx + i, byte);
        }

        popN(4);  // Pop 4 args, leave dest
        return PrimitiveResult::Success;
    }

    // Handle WORD objects (32-bit arrays)
    // Format 10 = Indexable32, Format 11 = Indexable32Odd
    auto destFmt = destHdr->format();
    auto srcFmt = srcHdr->format();
    bool destIs32 = (destFmt == ObjectFormat::Indexable32 || destFmt == ObjectFormat::Indexable32Odd);
    bool srcIs32 = (srcFmt == ObjectFormat::Indexable32 || srcFmt == ObjectFormat::Indexable32Odd);
    if (destIs32 && srcIs32) {
        // Calculate actual 32-bit word count
        size_t destSlots = destHdr->slotCount();
        size_t srcSlots = srcHdr->slotCount();
        size_t destWords = destSlots * 2 - (destFmt == ObjectFormat::Indexable32Odd ? 1 : 0);
        size_t srcWords = srcSlots * 2 - (srcFmt == ObjectFormat::Indexable32Odd ? 1 : 0);

        if (dstStartIdx + count > destWords || srcIdx + count > srcWords) {
            return PrimitiveResult::Failure;
        }

        // Access as 32-bit words
        uint32_t* destData = reinterpret_cast<uint32_t*>(destHdr + 1);
        uint32_t* srcData = reinterpret_cast<uint32_t*>(srcHdr + 1);
        for (size_t i = 0; i < count; i++) {
            destData[dstStartIdx + i] = srcData[srcIdx + i];
        }

        popN(4);
        return PrimitiveResult::Success;
    }

    // Handle 64-bit word arrays (Indexable64)
    if (destHdr->format() == ObjectFormat::Indexable64 &&
        srcHdr->format() == ObjectFormat::Indexable64) {
        size_t destWords = destHdr->slotCount();
        size_t srcWords = srcHdr->slotCount();

        if (dstStartIdx + count > destWords || srcIdx + count > srcWords) {
            return PrimitiveResult::Failure;
        }

        // Access as 64-bit words
        uint64_t* destData = reinterpret_cast<uint64_t*>(destHdr + 1);
        uint64_t* srcData = reinterpret_cast<uint64_t*>(srcHdr + 1);
        for (size_t i = 0; i < count; i++) {
            destData[dstStartIdx + i] = srcData[srcIdx + i];
        }

        popN(4);
        return PrimitiveResult::Success;
    }

    // Incompatible types - fail
    return PrimitiveResult::Failure;
}

// Primitive 228: Screen scale factor
// primitiveScreenScale -> float (scale factor for HiDPI)
PrimitiveResult Interpreter::primitiveScreenScale(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Return default scale factor of 1.0 for non-HiDPI
    // On iOS this would return the actual screen scale
    // For now, return 2.0 as typical for Retina displays

    // Allocate a boxed float
    uint32_t floatClassIndex = memory_.indexOfClass(
        memory_.specialObject(SpecialObjectIndex::ClassFloat));
    Oop result = memory_.allocateWords(floatClassIndex, 1);
    if (result.isNil()) {
        return PrimitiveResult::Failure;
    }

    // Store 2.0 as IEEE 754 double
    double scale = 2.0;
    uint64_t bits;
    std::memcpy(&bits, &scale, sizeof(bits));
    memory_.storeWord64(0, result, bits);

    pop();  // receiver
    push(result);
    return PrimitiveResult::Success;
}

// Primitive 229: String hash (alternative algorithm)
// aString primitiveStringHash2 -> hash
// Computes hash using alternative algorithm
PrimitiveResult Interpreter::primitiveStringHash2(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop receiver = stackTop();

    if (receiver.isImmediate()) {
        return PrimitiveResult::Failure;
    }

    // Use FNV-1a hash algorithm
    size_t size = memory_.byteSizeOf(receiver);
    uint64_t hash = 14695981039346656037ULL;  // FNV offset basis
    const uint64_t fnvPrime = 1099511628211ULL;

    for (size_t i = 0; i < size; i++) {
        uint8_t byte = memory_.fetchByte(i, receiver);
        hash ^= byte;
        hash *= fnvPrime;
    }

    // Reduce to SmallInteger range
    int64_t result = static_cast<int64_t>(hash & 0x3FFFFFFFFFFFFFFFULL);

    pop();
    push(Oop::fromSmallInteger(result));
    return PrimitiveResult::Success;
}

// Primitive 230: Shrink memory
// bytesToShrink primitiveShrinkMemory -> actualBytesShrunk
// Attempts to return memory to the OS
PrimitiveResult Interpreter::primitiveShrinkMemory(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop bytesOop = stackTop();
    if (!bytesOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    // Memory shrinking is handled by the GC system
    // For now, we don't support dynamic memory shrinking
    // Return 0 to indicate no shrinkage occurred
    pop();  // argument
    pop();  // receiver
    push(Oop::fromSmallInteger(0));
    return PrimitiveResult::Success;
}

// ===== MISC PRIMITIVES (232-239) =====

// Primitive 232: Form print (print a Form to printer)
// aForm primitiveFormPrint -> success
// Sends a Form to the default printer
PrimitiveResult Interpreter::primitiveFormPrint(int argCount) {
    // Printing is platform-specific and not commonly used
    // Fail to Smalltalk fallback
    return PrimitiveResult::Failure;
}

// Primitive 233: Set display mode
// depth fullscreen primitiveSetDisplayMode -> success
// Changes display depth and/or fullscreen mode
PrimitiveResult Interpreter::primitiveSetDisplayMode(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop fullscreenOop = stackTop();
    Oop depthOop = stackValue(1);

    // Display mode changes are platform-specific
    // On iOS, the display is managed by the system
    // Just acknowledge the request
    popN(2);  // pop arguments, leave receiver
    return PrimitiveResult::Success;
}

// Primitive 91: Test display depth
// depth primitiveTestDisplayDepth -> boolean
// Tests if a given display depth is supported
PrimitiveResult Interpreter::primitiveTestDisplayDepth(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop depthOop = stackTop();
    if (!depthOop.isSmallInteger()) return PrimitiveResult::Failure;

    int depth = static_cast<int>(depthOop.asSmallInteger());

    // iOS supports 32-bit color
    bool supported = (depth == 32 || depth == 16 || depth == 8 || depth == 1);

    popN(argCount + 1);  // pop arg and receiver
    push(supported ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

// Primitive 234: Bitmap decompress from byte array
// byteArray bitmap primitiveDecompress -> success
// Decompresses RLE-encoded bitmap data
PrimitiveResult Interpreter::primitiveBitmapDecompress(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop bitmapOop = stackTop();
    Oop byteArrayOop = stackValue(1);

    if (byteArrayOop.isImmediate() || bitmapOop.isImmediate()) {
        return PrimitiveResult::Failure;
    }

    // Bitmap decompression would decode RLE data
    // This is used for compressed image data
    // Fail to Smalltalk fallback which has pure Smalltalk implementation
    return PrimitiveResult::Failure;
}

// Primitive 158: String compare
// ByteString >> compareWith: anotherString (argCount=1)
// ByteString >> compareWith: anotherString collated: order (argCount=2)
// Always returns -1/0/1 (negative=less, 0=equal, positive=greater).
// Callers convert to 1/2/3 via `sign + 2` when needed.
PrimitiveResult Interpreter::primitiveStringCompareWith(int argCount) {
    if (argCount < 1 || argCount > 2) return PrimitiveResult::Failure;

    Oop string2Oop, string1Oop;
    const uint8_t* order = nullptr;

    if (argCount == 1) {
        string2Oop = stackTop();
        string1Oop = stackValue(1);
    } else {
        Oop orderOop = stackTop();
        string2Oop = stackValue(1);
        string1Oop = stackValue(2);

        if (!orderOop.isObject()) return PrimitiveResult::Failure;
        ObjectHeader* orderHdr = orderOop.asObjectPtr();
        if (!orderHdr->isBytesObject() || orderHdr->byteSize() < 256) {
            return PrimitiveResult::Failure;
        }
        order = orderHdr->bytes();
    }

    if (!string1Oop.isObject() || !string2Oop.isObject()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* hdr1 = string1Oop.asObjectPtr();
    ObjectHeader* hdr2 = string2Oop.asObjectPtr();

    uint8_t fmt1 = static_cast<uint8_t>(hdr1->format());
    uint8_t fmt2 = static_cast<uint8_t>(hdr2->format());

    // Only handle ByteStrings (format 16-23); fail for WideString
    if (fmt1 < 16 || fmt1 > 23 || fmt2 < 16 || fmt2 > 23) {
        return PrimitiveResult::Failure;
    }

    const uint8_t* bytes1 = hdr1->bytes();
    const uint8_t* bytes2 = hdr2->bytes();
    size_t len1 = hdr1->byteSize();
    size_t len2 = hdr2->byteSize();

    // Compare character by character using optional collation order
    size_t minLen = std::min(len1, len2);
    for (size_t i = 0; i < minLen; i++) {
        uint8_t c1 = order ? order[bytes1[i]] : bytes1[i];
        uint8_t c2 = order ? order[bytes2[i]] : bytes2[i];
        if (c1 < c2) {
            popN(argCount + 1);
            push(Oop::fromSmallInteger(-1));
            return PrimitiveResult::Success;
        }
        if (c1 > c2) {
            popN(argCount + 1);
            push(Oop::fromSmallInteger(1));
            return PrimitiveResult::Success;
        }
    }

    // All compared characters equal; shorter string is "less"
    int result;
    if (len1 < len2) {
        result = -1;
    } else if (len1 > len2) {
        result = 1;
    } else {
        result = 0;
    }

    popN(argCount + 1);
    push(Oop::fromSmallInteger(result));
    return PrimitiveResult::Success;
}

// Primitive 236: Sampled sound convert
// srcBuffer destBuffer start count primitiveSampledSoundConvert -> count
// Converts sampled sound data between formats
PrimitiveResult Interpreter::primitiveSampledSoundConvert(int argCount) {
    // Sound conversion is complex and platform-specific
    // Fail to Smalltalk fallback
    return PrimitiveResult::Failure;
}

// Primitive 237: Serial port operation
// portNum operation data primitiveSerialPortOp -> result
// Performs serial port operations (open, close, read, write)
PrimitiveResult Interpreter::primitiveSerialPortOp(int argCount) {
    // Serial port access is not typically available on iOS
    // Fail to Smalltalk fallback
    return PrimitiveResult::Failure;
}

// Primitive 238: Plugin callback
// callbackID args primitivePluginCallback -> result
// Invokes a callback registered by a plugin
PrimitiveResult Interpreter::primitivePluginCallback(int argCount) {
    // Plugin callbacks require the plugin infrastructure
    // Fail to Smalltalk fallback
    return PrimitiveResult::Failure;
}

// Primitive 239: Long running primitive
// primitiveIndex args primitiveLongRunningPrimitive -> result
// Wraps a primitive that may take a long time, allowing interrupts
PrimitiveResult Interpreter::primitiveLongRunningPrimitive(int argCount) {
    if (argCount < 1) return PrimitiveResult::Failure;

    Oop primIndexOop = stackValue(argCount - 1);

    if (!primIndexOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    // This primitive is meant to wrap other primitives that might
    // take a long time, checking for interrupts periodically
    // For now, just fail to let Smalltalk handle it
    return PrimitiveResult::Failure;
}

// ===== EVENT/INPUT PRIMITIVES (264-269) =====

// Primitive 264: Get next event from event queue
// eventBuffer primitiveGetNextEvent -> eventBuffer (filled) or nil
// Fills the event buffer with the next pending event
// Can be called with argCount=0 (receiver is event buffer) or argCount=1 (event buffer as argument)
PrimitiveResult Interpreter::primitiveGetNextEvent(int argCount) {
    // CRITICAL: For primitives, receiver_ is NOT automatically set!
    // We must get the receiver from the stack. The receiver is under the arguments.
    Oop actualReceiver = stackValue(static_cast<size_t>(argCount));

    // Process input events first - this handles menu bar clicks natively
    processInputEvents();

    // Get event buffer from argument (argCount=1) or receiver's first inst var (argCount=0)
    // With argCount=0, Pharo's InputEventSensor>>primGetNextEvent passes
    // the eventBuffer as the FIRST instance variable of the receiver (not the receiver itself)
    Oop eventBuffer;
    if (argCount == 1) {
        // eventBuffer passed as explicit argument
        eventBuffer = stackTop();
    } else if (argCount == 0) {
        // Try to find eventBuffer in receiver's instance variables
        // In Pharo's InputEventSensor, eventBuffer is NOT at slot 0 due to inheritance
        // We need to find the slot that contains a WordArray/Array with format 2-4
        if (!actualReceiver.isObject()) {
            return PrimitiveResult::Failure;
        }
        ObjectHeader* rcvrHdr = actualReceiver.asObjectPtr();

        // Debug: Log all slots to find the event buffer
        static FILE* slotLog = nullptr;
        static int slotLogCount = 0;
        if (!slotLog) slotLog = nullptr;
        if (slotLog && slotLogCount < 5) {
            slotLogCount++;
            fprintf(slotLog, "[SCAN #%d] Receiver has %zu slots:\n", slotLogCount, rcvrHdr->slotCount());
            for (size_t i = 0; i < rcvrHdr->slotCount() && i < 15; i++) {
                Oop slot = memory_.fetchPointer(i, actualReceiver);
                if (slot.isObject() && !slot.isImmediate()) {
                    ObjectHeader* slotHdr = slot.asObjectPtr();
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
                    fprintf(slotLog, "  slot[%zu]: raw=0x%llx format=%d slotCount=%zu class='%s'\n",
                            i, (unsigned long long)slot.rawBits(), slotHdr->format(),
                            slotHdr->slotCount(), className.c_str());
                } else if (slot.isSmallInteger()) {
                    fprintf(slotLog, "  slot[%zu]: SmallInteger %lld\n", i, slot.asSmallInteger());
                } else if (slot.isNil()) {
                    fprintf(slotLog, "  slot[%zu]: nil\n", i);
                } else {
                    fprintf(slotLog, "  slot[%zu]: raw=0x%llx (immediate or special)\n",
                            i, (unsigned long long)slot.rawBits());
                }
            }
            fflush(slotLog);
        }

        // Search for the event buffer: look for an Array or WordArray (format 2-4)
        eventBuffer = Oop::nil();
        for (size_t i = 0; i < rcvrHdr->slotCount() && i < 15; i++) {
            Oop slot = memory_.fetchPointer(i, actualReceiver);
            if (slot.isObject() && !slot.isImmediate() && !slot.isNil()) {
                ObjectHeader* slotHdr = slot.asObjectPtr();
                int fmt = static_cast<int>(slotHdr->format());
                // Format 2-4 are indexable (arrays)
                // Format 2: indexable with no inst vars (Array, WordArray, etc.)
                // Format 3: indexable with inst vars
                // Format 4-5: weak indexable
                // Also check for format 10-11 (32-bit indexable) for WordArray
                if ((fmt >= 2 && fmt <= 5) || (fmt >= 10 && fmt <= 11)) {
                    size_t slotCount = slotHdr->slotCount();
                    // Event buffer should have at least 4 elements (type, timestamp, x, y)
                    if (slotCount >= 4 && slotCount <= 16) {
                        eventBuffer = slot;
                        break;
                    }
                }
            }
        }

        // If we couldn't find an array, fall back to old behavior (slot 0)
        if (eventBuffer.isNil() && rcvrHdr->slotCount() >= 1) {
            eventBuffer = memory_.fetchPointer(0, actualReceiver);
        }
    } else {
        return PrimitiveResult::Failure;
    }

    // Debug: Log buffer validation
    static FILE* bufLog = nullptr;
    static int bufCount = 0;
    bufCount++;
    if (bufCount <= 10 || bufCount % 5000 == 0) {
        fprintf(stderr, "[P264] #%d called, passthrough=%zu argCount=%d\n",
                bufCount, passThroughEvents_.size(), argCount);
    }

    // Debug: Log what eventBuffer is AND what receiver is
    if (bufLog && bufCount <= 20) {
        // First log receiver details
        if (actualReceiver.isObject()) {
            ObjectHeader* rcvrHdr = actualReceiver.asObjectPtr();
            // Try to get class name of receiver - check if receiver IS a class
            Oop rcvrClass = memory_.classOf(actualReceiver);
            std::string rcvrClassName = "<unknown>";
            std::string isMetaclass = "no";
            if (rcvrClass.isObject()) {
                // Get class name
                Oop nameOop = memory_.fetchPointer(6, rcvrClass);  // class name at slot 6
                if (nameOop.isObject()) {
                    ObjectHeader* nameHdr = nameOop.asObjectPtr();
                    if (nameHdr->isBytesObject() && nameHdr->byteSize() < 100) {
                        rcvrClassName = std::string((char*)nameHdr->bytes(), nameHdr->byteSize());
                    }
                }
                // Check if receiver is a class (its class would be a Metaclass)
                if (rcvrClassName.find("class") != std::string::npos ||
                    rcvrClassName == "Metaclass" || rcvrClassName == "Class") {
                    isMetaclass = "yes";
                    // For classes, try to get the class's own name from slot 6
                    Oop selfName = memory_.fetchPointer(6, actualReceiver);
                    if (selfName.isObject()) {
                        ObjectHeader* selfNameHdr = selfName.asObjectPtr();
                        if (selfNameHdr->isBytesObject() && selfNameHdr->byteSize() < 100) {
                            rcvrClassName = std::string((char*)selfNameHdr->bytes(), selfNameHdr->byteSize()) + " (class obj)";
                        }
                    }
                }
            }
            fprintf(bufLog, "[BUF] #%d RECEIVER raw=0x%llx classIdx=%d className='%s' isClass=%s slotCount=%zu\n",
                    bufCount, (unsigned long long)actualReceiver.rawBits(),
                    rcvrHdr->classIndex(), rcvrClassName.c_str(), isMetaclass.c_str(), rcvrHdr->slotCount());
        }

        // Then log eventBuffer details
        fprintf(bufLog, "[BUF] #%d eventBuffer raw=0x%llx isObj=%d isImm=%d isNil=%d argCount=%d sameAsReceiver=%d\n",
                bufCount, (unsigned long long)eventBuffer.rawBits(),
                eventBuffer.isObject() ? 1 : 0,
                eventBuffer.isImmediate() ? 1 : 0,
                eventBuffer.isNil() ? 1 : 0,
                argCount,
                (eventBuffer.rawBits() == actualReceiver.rawBits()) ? 1 : 0);
        if (eventBuffer.isObject()) {
            ObjectHeader* hdr = eventBuffer.asObjectPtr();
            // Get eventBuffer class name
            Oop bufClass = memory_.classOf(eventBuffer);
            std::string bufClassName = "<unknown>";
            if (bufClass.isObject()) {
                Oop nameOop = memory_.fetchPointer(6, bufClass);
                if (nameOop.isObject()) {
                    ObjectHeader* nameHdr = nameOop.asObjectPtr();
                    if (nameHdr->isBytesObject() && nameHdr->byteSize() < 100) {
                        bufClassName = std::string((char*)nameHdr->bytes(), nameHdr->byteSize());
                    }
                }
            }
            fprintf(bufLog, "[BUF] #%d obj classIdx=%d format=%d slotCount=%zu className='%s'\n",
                    bufCount, hdr->classIndex(), hdr->format(), hdr->slotCount(), bufClassName.c_str());
        }
        fflush(bufLog);
    }

    if (eventBuffer.isImmediate()) {
        if (bufLog && bufCount <= 20) {
            fprintf(bufLog, "[BUF] #%d FAIL: eventBuffer is immediate (raw=0x%llx)\n",
                    bufCount, (unsigned long long)eventBuffer.rawBits());
            fflush(bufLog);
        }
        return PrimitiveResult::Failure;
    }

    // Event buffer format (8 slots):
    // 0: event type (0=none, 1=mouse, 2=key, 6=window, etc.)
    // 1: timestamp
    // 2-6: event-specific data
    // 7: window index

    // Check buffer has enough slots
    // Note: Pharo versions vary - some use 4 slots (minimal), 6 slots, or 8 slots
    // We need at least 4 slots for basic event data (type, timestamp, x, y)
    size_t slotCount = memory_.slotCountOf(eventBuffer);
    if (slotCount < 4) {
        if (bufLog && bufCount <= 20) {
            fprintf(bufLog, "[BUF] #%d FAIL: slotCount=%zu < 4\n", bufCount, slotCount);
            fflush(bufLog);
        }
        return PrimitiveResult::Failure;
    }

    if (bufLog && bufCount <= 100) {  // Log more calls
        fprintf(bufLog, "[BUF] #%d OK: slotCount=%zu, passthrough=%zu\n",
                bufCount, slotCount, passThroughEvents_.size());
        fflush(bufLog);
    }

    // Try to get next event - first from pass-through buffer, then from queue
    Event event;
    bool hasEvent = false;

    // Debug: track calls - always log to stderr for first 50 calls
    static int callCount = 0;
    callCount++;

    // Always log when passthrough has events or when buttons=2 (world menu)
    static FILE* prim264Log = nullptr;
    if (prim264Log && (callCount <= 200 || !passThroughEvents_.empty())) {
        fprintf(prim264Log, "[PRIM264] #%d passthrough=%zu queueEmpty=%d\n",
                callCount, passThroughEvents_.size(), gEventQueue.isEmpty() ? 1 : 0);
        fflush(prim264Log);
    }

    // IMPORTANT: Only read from passThroughEvents_, which is populated by processInputEvents().
    // processInputEvents() filters menu-related events and passes non-menu events through.
    // We do NOT read directly from gEventQueue here - that would bypass menu handling.
    if (!passThroughEvents_.empty()) {
        event = passThroughEvents_.front();
        passThroughEvents_.erase(passThroughEvents_.begin());
        hasEvent = true;
        // Always log mouse events with buttons (especially buttons=2 for world menu)
        if (prim264Log && (callCount <= 200 || event.arg3 != 0)) {
            fprintf(prim264Log, "[PRIM264] #%d Got event: eventType=%d subtype=%d x=%d y=%d buttons=%d\n",
                    callCount, event.type, event.arg5, event.arg1, event.arg2, event.arg3);
            fflush(prim264Log);
        }
    }
    // Note: We intentionally do NOT fall back to gEventQueue.pop() here.
    // All events must go through processInputEvents() first for menu handling.

    if (hasEvent) {
        // Fill buffer with event data - only write slots that exist
        memory_.storePointer(0, eventBuffer, Oop::fromSmallInteger(event.type));
        if (slotCount > 1) memory_.storePointer(1, eventBuffer, Oop::fromSmallInteger(event.timeStamp));
        if (slotCount > 2) memory_.storePointer(2, eventBuffer, Oop::fromSmallInteger(event.arg1));
        if (slotCount > 3) memory_.storePointer(3, eventBuffer, Oop::fromSmallInteger(event.arg2));
        if (slotCount > 4) memory_.storePointer(4, eventBuffer, Oop::fromSmallInteger(event.arg3));
        if (slotCount > 5) memory_.storePointer(5, eventBuffer, Oop::fromSmallInteger(event.arg4));
        if (slotCount > 6) memory_.storePointer(6, eventBuffer, Oop::fromSmallInteger(event.arg5));
        if (slotCount > 7) {
            memory_.storePointer(7, eventBuffer, Oop::fromSmallInteger(event.windowIndex));
        }

        // Log mouse events returned to Pharo
        if (event.type == 1) {
            static FILE* mouseLog = nullptr;
            if (mouseLog) {
                const char* subtype = (event.arg5 == 1) ? "down" : (event.arg5 == 2) ? "up" : "move";
                fprintf(mouseLog, "[TO-PHARO] Mouse %s at (%d,%d) buttons=%d mods=%d\n",
                        subtype, event.arg1, event.arg2, event.arg3, event.arg4);
                fflush(mouseLog);
            }
        }
    } else {
        // No event available
        memory_.storePointer(0, eventBuffer, Oop::fromSmallInteger(0));
    }

    // Debug: Verify what we stored in slot 0
    static FILE* slot0Log = nullptr;
    static int slot0Count = 0;
    if (!slot0Log) slot0Log = nullptr;
    if (slot0Log && slot0Count < 100) {
        Oop storedType = memory_.fetchPointer(0, eventBuffer);
        slot0Count++;
        fprintf(slot0Log, "[SLOT0 #%d] stored=%lld isSmallInt=%d hasEvent=%d eventBuffer=0x%llx\n",
                slot0Count, storedType.asSmallInteger(), storedType.isSmallInteger() ? 1 : 0,
                hasEvent ? 1 : 0, (unsigned long long)eventBuffer.rawBits());
        fflush(slot0Log);
    }

    if (argCount == 1) {
        pop();  // pop eventBuffer argument, leave receiver
    }
    // For argCount=0: Pharo primitives return self (leave receiver on stack)
    // The primitive fills the receiver's eventBuffer instance variable.
    // Pharo code will then access the eventBuffer through the receiver.
    // We do NOT modify the stack for argCount=0 - receiver stays as return value.

    // Debug: Log completion and what's on stack
    static FILE* compLog = nullptr;
    static int compCount = 0;
    compCount++;
    if (compLog && compCount <= 50) {
        Oop tos = stackTop();
        fprintf(compLog, "[COMPLETE] #%d hasEvent=%d stackTop=0x%llx receiver=0x%llx\n",
                compCount, hasEvent ? 1 : 0,
                (unsigned long long)tos.rawBits(),
                (unsigned long long)receiver_.rawBits());
        fflush(compLog);
    }

    // Debug: Check class hierarchy of receiver for circularity
    static FILE* hierLog = nullptr;
    static int hierCount = 0;
    if (!hierLog) hierLog = nullptr;
    if (hierLog && hierCount < 3) {
        hierCount++;
        fprintf(hierLog, "[HIER #%d] Receiver class hierarchy:\n", hierCount);

        Oop currentClass = memory_.classOf(actualReceiver);
        std::set<uint64_t> seenClasses;
        int depth = 0;
        while (currentClass.isObject() && !currentClass.isNil() && depth < 50) {
            uint64_t clsBits = currentClass.rawBits();
            if (seenClasses.count(clsBits)) {
                fprintf(hierLog, "  [CIRCULAR at depth %d! class=0x%llx]\n",
                        depth, (unsigned long long)clsBits);
                break;
            }
            seenClasses.insert(clsBits);

            // Get class name
            std::string className = "<unknown>";
            ObjectHeader* clsHdr = currentClass.asObjectPtr();
            Oop nameOop = memory_.fetchPointer(6, currentClass);
            if (nameOop.isObject()) {
                ObjectHeader* nameHdr = nameOop.asObjectPtr();
                if (nameHdr->isBytesObject() && nameHdr->byteSize() < 100) {
                    className = std::string((char*)nameHdr->bytes(), nameHdr->byteSize());
                }
            }
            fprintf(hierLog, "  [%d] 0x%llx classIdx=%d '%s'\n",
                    depth, (unsigned long long)clsBits, clsHdr->classIndex(), className.c_str());

            // Get superclass (slot 0 of class)
            currentClass = memory_.fetchPointer(0, currentClass);
            depth++;
        }
        if (currentClass.isNil() || !currentClass.isObject()) {
            fprintf(hierLog, "  [END] nil/non-object at depth %d\n", depth);
        }
        fflush(hierLog);
    }

    // Flag to enable detailed send tracing after prim 264
    // Variable defined in Interpreter.cpp
    g_traceSendsAfterPrim264 = 50;  // Trace next 50 user sends (internal lookup filtered)

    return PrimitiveResult::Success;
}

// Primitive 265: Set input semaphore (variant 2)
// semaphoreIndex primitiveInputSemaphore2 -> receiver
// Sets the semaphore to signal when input is available
// NOTE: In some images, this is a unary message where the receiver is the semaphore index
PrimitiveResult Interpreter::primitiveInputSemaphore2(int argCount) {
    // Debug: Log calls to this primitive
    static FILE* semLog = nullptr;
    static int semCallCount = 0;
    semCallCount++;
    if (semLog && semCallCount <= 20) {
        fprintf(semLog, "[INPUT-SEM] Call #%d argCount=%d\n", semCallCount, argCount);
        fflush(semLog);
    }

    Oop semIndexOop;
    if (argCount == 0) {
        semIndexOop = stackTop();  // receiver
    } else if (argCount == 1) {
        semIndexOop = stackTop();
    } else {
        return PrimitiveResult::Failure;
    }

    if (!semIndexOop.isSmallInteger()) {
        if (semIndexOop.isObject()) {
            ObjectHeader* hdr = semIndexOop.asObjectPtr();
            // Try to get semaphore index from object's slots
            for (size_t i = 0; i < std::min(hdr->slotCount(), (size_t)4); i++) {
                Oop slot = hdr->slotAt(i);
                if (slot.isSmallInteger()) {
                    int64_t val = slot.asSmallInteger();
                    if (val > 0 && val < 100) {
                        gEventQueue.setInputSemaphoreIndex(static_cast<int>(val));
                        return PrimitiveResult::Success;
                    }
                }
            }
        }
        // Don't use fallback - wrong semaphore is worse than no semaphore
        // Let the primitive fail so Smalltalk can handle it
        return PrimitiveResult::Failure;
    }

    int64_t semIndex = semIndexOop.asSmallInteger();
    gEventQueue.setInputSemaphoreIndex(static_cast<int>(semIndex));

    // Debug: Log the semaphore index being set
    if (semLog && semCallCount <= 20) {
        fprintf(semLog, "[INPUT-SEM] Set semaphore index to %lld\n", semIndex);
        fflush(semLog);
    }

    if (argCount == 1) {
        pop();  // pop argument, leave receiver
    }
    return PrimitiveResult::Success;
}

// Primitive 266: Event processing control
// controlCode primitiveEventProcessingControl -> result
// Controls event processing behavior
PrimitiveResult Interpreter::primitiveEventProcessingControl(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop controlCode = stackTop();

    if (!controlCode.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t code = controlCode.asSmallInteger();

    // Control codes:
    // 0: Query current state
    // 1: Enable event processing
    // 2: Disable event processing
    // 3: Flush event queue

    int64_t result = 0;
    switch (code) {
        case 0:  // Query - return "enabled"
            result = 1;
            break;
        case 1:  // Enable
            result = 1;
            break;
        case 2:  // Disable
            result = 0;
            break;
        case 3:  // Flush event queue
            gEventQueue.clear();
            result = 0;
            break;
        default:
            return PrimitiveResult::Failure;
    }

    pop();  // pop argument
    pop();  // pop receiver
    push(Oop::fromSmallInteger(result));
    return PrimitiveResult::Success;
}

// Primitive 267: Sampled sound operations
// args primitiveSampledSound -> result
// Handles sampled sound playback
PrimitiveResult Interpreter::primitiveSampledSound(int argCount) {
    // Sound primitives are not supported on iOS/headless
    // Return success with nil/0 to indicate no sound is playing
    // This prevents Pharo from retrying in a tight loop

    // Pop arguments, leave receiver
    for (int i = 0; i < argCount; i++) {
        pop();
    }
    // Replace receiver with nil or 0 as appropriate
    // Most sound queries expect a SmallInteger result
    stackTop() = Oop::fromSmallInteger(0);
    return PrimitiveResult::Success;
}

// Primitive 268: Mixed sound operations
// args primitiveMixedSound -> result
// Handles mixed/synthesized sound
PrimitiveResult Interpreter::primitiveMixedSound(int argCount) {
    // Sound primitives require platform-specific audio support
    // Fail to Smalltalk fallback
    return PrimitiveResult::Failure;
}

// Primitive 269: Control OS process
// controlCode primitiveControlOSProcess -> result
// Controls the VM's OS process (priority, affinity, etc.)
PrimitiveResult Interpreter::primitiveControlOSProcess(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop controlCode = stackTop();

    if (!controlCode.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t code = controlCode.asSmallInteger();

    // Control codes:
    // 0: Query process ID
    // 1: Query thread count
    // 2: Set process priority (requires additional arg)
    // etc.

    int64_t result = 0;
    switch (code) {
        case 0:  // Get process ID
            result = static_cast<int64_t>(getpid());
            break;
        case 1:  // Thread count - return 1 for single-threaded
            result = 1;
            break;
        default:
            // Unknown control code - return 0
            result = 0;
            break;
    }

    pop();  // pop argument
    pop();  // pop receiver
    push(Oop::fromSmallInteger(result));
    return PrimitiveResult::Success;
}

// ===== BITBLT PRIMITIVES (290-299) =====

// BitBlt helper: Extract integer field from BitBlt object.
// Handles SmallInteger, SmallFloat, boxed Float, Fraction, and nil.
// Float values are truncated to integer (matching Smalltalk's BitBlt behavior
// where coordinate fields like destX/destY accept Floats from Canvas origins
// computed via Float arithmetic on morph positions).
static intptr_t bitBltField(ObjectMemory& memory, Oop bitBlt, size_t index) {
    Oop field = memory.fetchPointer(index, bitBlt);
    if (field.isSmallInteger()) {
        return field.asSmallInteger();
    }
    if (field.isNil()) {
        return 0;
    }
    // SmallFloat immediate — truncate to integer
    if (field.isSmallFloat()) {
        double val = field.asSmallFloat();
        intptr_t result = static_cast<intptr_t>(val);
        // Write back as SmallInteger for consistency
        memory.storePointer(index, bitBlt, Oop::fromSmallInteger(result));
        return result;
    }
    if (field.isObject()) {
        ObjectHeader* hdr = field.asObjectPtr();
        auto fmt = hdr->format();
        // Boxed Float (64-bit indexable, 1 slot = 8 bytes)
        if (fmt == ObjectFormat::Indexable64 && hdr->slotCount() == 1) {
            double val;
            std::memcpy(&val, hdr->bytes(), sizeof(double));
            intptr_t result = static_cast<intptr_t>(val);
            memory.storePointer(index, bitBlt, Oop::fromSmallInteger(result));
            return result;
        }
        // Fraction (2 SmallInteger inst vars: numerator, denominator)
        if (fmt == ObjectFormat::FixedSize && hdr->slotCount() == 2) {
            Oop num = memory.fetchPointer(0, field);
            Oop den = memory.fetchPointer(1, field);
            if (num.isSmallInteger() && den.isSmallInteger()) {
                intptr_t n = num.asSmallInteger();
                intptr_t d = den.asSmallInteger();
                if (d != 0) {
                    intptr_t result = n / d; // truncate toward zero
                    memory.storePointer(index, bitBlt, Oop::fromSmallInteger(result));
                    return result;
                }
            }
        }
    }
    return 0;
}

// BitBlt field indices (standard Squeak/Pharo layout)
enum BitBltFields {
    BBDestForm = 0,
    BBSourceForm = 1,
    BBHalftoneForm = 2,
    BBCombinationRule = 3,
    BBDestX = 4,
    BBDestY = 5,
    BBWidth = 6,
    BBHeight = 7,
    BBSourceX = 8,
    BBSourceY = 9,
    BBClipX = 10,
    BBClipY = 11,
    BBClipWidth = 12,
    BBClipHeight = 13,
    BBColorMap = 14
};

// Form field indices
enum FormFields {
    FormBits = 0,
    FormWidth = 1,
    FormHeight = 2,
    FormDepth = 3,
    FormOffset = 4  // Optional Point for offset
};

// Primitive 290: Copy bits (main BitBlt operation)
// aBitBlt primitiveCopyBits -> aBitBlt
// The core BitBlt operation that copies pixels from source to destination
// Combination rules: 0=AND 1=AND+NOT 2=NOT+AND 3=STORE 4=NOT+OR 5=DEST 6=XOR 7=OR
// 24=alphaBlend 25=paint(OR) 34=sourceWord(copy)
PrimitiveResult Interpreter::primitiveCopyBits(int argCount) {
    if (argCount != 0) { return PrimitiveResult::Failure; }

    Oop bitBlt = stackTop();
    if (!bitBlt.isObject()) { return PrimitiveResult::Failure; }

    // Extract BitBlt parameters
    Oop destForm = memory_.fetchPointer(BBDestForm, bitBlt);
    Oop sourceForm = memory_.fetchPointer(BBSourceForm, bitBlt);
    intptr_t combinationRule = bitBltField(memory_, bitBlt, BBCombinationRule);
    intptr_t destX = bitBltField(memory_, bitBlt, BBDestX);
    intptr_t destY = bitBltField(memory_, bitBlt, BBDestY);
    intptr_t width = bitBltField(memory_, bitBlt, BBWidth);
    intptr_t height = bitBltField(memory_, bitBlt, BBHeight);
    intptr_t sourceX = bitBltField(memory_, bitBlt, BBSourceX);
    intptr_t sourceY = bitBltField(memory_, bitBlt, BBSourceY);
    intptr_t clipX = bitBltField(memory_, bitBlt, BBClipX);
    intptr_t clipY = bitBltField(memory_, bitBlt, BBClipY);
    intptr_t clipWidth = bitBltField(memory_, bitBlt, BBClipWidth);
    intptr_t clipHeight = bitBltField(memory_, bitBlt, BBClipHeight);

    // Validate destination form
    if (destForm.isNil() || !destForm.isObject()) { return PrimitiveResult::Failure; }

    Oop destBits = memory_.fetchPointer(FormBits, destForm);
    intptr_t destWidth = bitBltField(memory_, destForm, FormWidth);
    intptr_t destHeight = bitBltField(memory_, destForm, FormHeight);
    intptr_t destDepth = bitBltField(memory_, destForm, FormDepth);

    // Resolve destination bits: can be an object (Bitmap) or SmallInteger (surface handle)
    uint32_t* destPixels = nullptr;
    size_t destBitsSize = 0;
    intptr_t destPitch = destWidth; // in 32-bit words for 32-bit depth
    int destSurfaceID = -1;

    if (destBits.isSmallInteger()) {
        // Surface handle — resolve via SurfacePlugin table
        destSurfaceID = static_cast<int>(destBits.asSmallInteger());
        ManualSurface* s = lookupSurface(destSurfaceID);
        if (!s || !s->bits) {
            return PrimitiveResult::Failure;
        }
        destPixels = reinterpret_cast<uint32_t*>(s->bits);
        destBitsSize = static_cast<size_t>(s->rowPitch) * s->height;
        destPitch = s->rowPitch / 4; // rowPitch is in bytes, destPitch is in 32-bit words
    } else if (destBits.isObject() && !destBits.isNil() && destBits.rawBits() >= 0x10000) {
        ObjectHeader* destBitsHdr = destBits.asObjectPtr();
        destPixels = reinterpret_cast<uint32_t*>(destBitsHdr->bytes());
        destBitsSize = destBitsHdr->byteSize();
    } else {
        return PrimitiveResult::Failure;
    }

    // Clip to clip rect
    if (destX < clipX) { intptr_t d = clipX - destX; width -= d; sourceX += d; destX = clipX; }
    if (destY < clipY) { intptr_t d = clipY - destY; height -= d; sourceY += d; destY = clipY; }
    if (destX + width > clipX + clipWidth) width = clipX + clipWidth - destX;
    if (destY + height > clipY + clipHeight) height = clipY + clipHeight - destY;

    // Clip to dest form bounds
    if (destX < 0) { intptr_t d = -destX; width -= d; sourceX += d; destX = 0; }
    if (destY < 0) { intptr_t d = -destY; height -= d; sourceY += d; destY = 0; }
    if (destX + width > destWidth) width = destWidth - destX;
    if (destY + height > destHeight) height = destHeight - destY;

    if (width <= 0 || height <= 0) {
        // For counting rules, return 0 when nothing to count
        if (combinationRule >= 30 && combinationRule <= 32) {
            primitiveSuccess(Oop::fromSmallInteger(0));
            return PrimitiveResult::Success;
        }
        return PrimitiveResult::Success;
    }

    // Counting modes (rules 30-32): count pixels, return count as SmallInteger
    // Rule 30: count dest pixels != 0
    // Rule 31: count dest pixels == 0
    // Rule 32: count source pixels != 0 (tally)
    if (combinationRule >= 30 && combinationRule <= 32) {
        intptr_t count = 0;

        if (combinationRule == 32 && !sourceForm.isNil() && sourceForm.isObject()) {
            // Rule 32: count non-zero source pixels
            Oop srcBits = memory_.fetchPointer(FormBits, sourceForm);
            intptr_t srcDepth = bitBltField(memory_, sourceForm, FormDepth);
            intptr_t srcWidth2 = bitBltField(memory_, sourceForm, FormWidth);

            if (srcBits.isObject() && !srcBits.isNil() && srcBits.rawBits() >= 0x10000) {
                ObjectHeader* srcHdr = srcBits.asObjectPtr();
                size_t srcBitsSize = srcHdr->byteSize();

                if (srcDepth == 32) {
                    uint32_t* srcPixels = reinterpret_cast<uint32_t*>(srcHdr->bytes());
                    intptr_t srcPitch = srcWidth2;
                    for (intptr_t y = 0; y < height; y++) {
                        intptr_t sy = sourceY + y;
                        if (sy < 0 || static_cast<size_t>((sy + 1) * srcPitch) * 4 > srcBitsSize) continue;
                        uint32_t* row = srcPixels + sy * srcPitch + sourceX;
                        for (intptr_t x = 0; x < width; x++) {
                            if (row[x] != 0) count++;
                        }
                    }
                } else if (srcDepth == 1) {
                    intptr_t srcWordsPerRow = (srcWidth2 + 31) / 32;
                    uint32_t* srcWords = reinterpret_cast<uint32_t*>(srcHdr->bytes());
                    for (intptr_t y = 0; y < height; y++) {
                        intptr_t sy = sourceY + y;
                        if (sy < 0 || static_cast<size_t>((sy + 1) * srcWordsPerRow) * 4 > srcBitsSize) continue;
                        uint32_t* row = srcWords + sy * srcWordsPerRow;
                        for (intptr_t x = 0; x < width; x++) {
                            intptr_t sx = sourceX + x;
                            if ((row[sx / 32] >> (31 - sx % 32)) & 1) count++;
                        }
                    }
                } else if (srcDepth == 8) {
                    intptr_t srcBytesPerRow = ((srcWidth2 + 3) / 4) * 4;
                    uint8_t* srcBytes8 = srcHdr->bytes();
                    for (intptr_t y = 0; y < height; y++) {
                        intptr_t sy = sourceY + y;
                        if (sy < 0 || static_cast<size_t>((sy + 1) * srcBytesPerRow) > srcBitsSize) continue;
                        uint8_t* row = srcBytes8 + sy * srcBytesPerRow + sourceX;
                        for (intptr_t x = 0; x < width; x++) {
                            if (row[x] != 0) count++;
                        }
                    }
                }
            }
        } else if (combinationRule == 30 || combinationRule == 31) {
            // Rules 30/31: count dest pixels that are non-zero/zero
            if (destDepth == 32) {
                for (intptr_t y = 0; y < height; y++) {
                    uint32_t* row = destPixels + (destY + y) * destPitch + destX;
                    for (intptr_t x = 0; x < width; x++) {
                        bool nonZero = (row[x] != 0);
                        if (combinationRule == 30 ? nonZero : !nonZero) count++;
                    }
                }
            } else if (destDepth == 8) {
                intptr_t destBytesPerRow = ((destWidth + 3) / 4) * 4;
                uint8_t* destBytes8 = reinterpret_cast<uint8_t*>(destPixels);
                for (intptr_t y = 0; y < height; y++) {
                    uint8_t* row = destBytes8 + (destY + y) * destBytesPerRow + destX;
                    for (intptr_t x = 0; x < width; x++) {
                        bool nonZero = (row[x] != 0);
                        if (combinationRule == 30 ? nonZero : !nonZero) count++;
                    }
                }
            } else if (destDepth == 1) {
                intptr_t destWordsPerRow = (destWidth + 31) / 32;
                for (intptr_t y = 0; y < height; y++) {
                    uint32_t* row = destPixels + (destY + y) * destWordsPerRow;
                    for (intptr_t x = 0; x < width; x++) {
                        intptr_t dx = destX + x;
                        bool nonZero = (row[dx / 32] >> (31 - dx % 32)) & 1;
                        if (combinationRule == 30 ? nonZero : !nonZero) count++;
                    }
                }
            }
        }

        // Return count as SmallInteger (replace receiver on stack)
        primitiveSuccess(Oop::fromSmallInteger(count));
        return PrimitiveResult::Success;
    }

    // Halftone form extraction (used as fill color for no-source ops)
    Oop halftoneForm = memory_.fetchPointer(BBHalftoneForm, bitBlt);
    uint32_t halftoneWord = 0xFFFFFFFF;
    uint32_t* halftoneWords = nullptr;
    intptr_t halftoneHeight = 0;
    if (!halftoneForm.isNil()) {
        if (halftoneForm.isSmallInteger()) {
            halftoneWord = static_cast<uint32_t>(halftoneForm.asSmallInteger());
        } else if (halftoneForm.isObject()) {
            // Halftone can be a Form or a Bitmap (word array)
            ObjectHeader* htHdr = halftoneForm.asObjectPtr();
            if (htHdr->isBytesObject() ||
                htHdr->format() == ObjectFormat::Indexable32 ||
                htHdr->format() == ObjectFormat::Indexable32Odd) {
                halftoneWords = reinterpret_cast<uint32_t*>(htHdr->bytes());
                halftoneHeight = static_cast<intptr_t>(htHdr->byteSize() / 4);
            } else {
                // It's a Form - get its bits
                Oop htBits = memory_.fetchPointer(FormBits, halftoneForm);
                if (htBits.isObject() && !htBits.isNil()) {
                    ObjectHeader* htBitsHdr = htBits.asObjectPtr();
                    halftoneWords = reinterpret_cast<uint32_t*>(htBitsHdr->bytes());
                    halftoneHeight = static_cast<intptr_t>(htBitsHdr->byteSize() / 4);
                }
            }
        }
    }

    // No-source operations (fill)
    if (sourceForm.isNil()) {
        if (destDepth == 1) {
            // Fill 1-bit destination: halftone/fill word applied per bit
            intptr_t destWordsPerRow = (destWidth + 31) / 32;
            uint32_t* destWords = destPixels;
            size_t requiredDestWords = static_cast<size_t>((destY + height - 1) * destWordsPerRow + (destX + width + 31) / 32);
            if (requiredDestWords * 4 > destBitsSize) return PrimitiveResult::Failure;
            for (intptr_t y = 0; y < height; y++) {
                uint32_t fill = halftoneWords ? halftoneWords[(destY + y) % halftoneHeight] : halftoneWord;
                uint32_t* destRow = destWords + (destY + y) * destWordsPerRow;
                for (intptr_t x = 0; x < width; x++) {
                    intptr_t dx = destX + x;
                    uint32_t bitMask = 0x80000000u >> (dx % 32);
                    uint32_t fillBit = (fill != 0) ? bitMask : 0; // simplification: non-zero fill → 1
                    switch (combinationRule) {
                        case 3: case 34:
                            destRow[dx / 32] = (destRow[dx / 32] & ~bitMask) | fillBit; break;
                        case 0:
                            if (!fillBit) destRow[dx / 32] &= ~bitMask; break;
                        case 7: case 25:
                            if (fillBit) destRow[dx / 32] |= bitMask; break;
                        case 6:
                            if (fillBit) destRow[dx / 32] ^= bitMask; break;
                        default:
                            destRow[dx / 32] = (destRow[dx / 32] & ~bitMask) | fillBit; break;
                    }
                }
            }
            showDisplayBits(destForm, destX, destY, destX + width, destY + height);
            return PrimitiveResult::Success;
        }
        if (destDepth != 32) {
            return PrimitiveResult::Failure;
        }
        size_t requiredBytes = static_cast<size_t>((destY + height - 1) * destWidth + destX + width) * 4;
        if (requiredBytes > destBitsSize) { return PrimitiveResult::Failure; }

        for (intptr_t y = 0; y < height; y++) {
            uint32_t fill = halftoneWords ? halftoneWords[(destY + y) % halftoneHeight] : halftoneWord;
            uint32_t* row = destPixels + (destY + y) * destPitch + destX;
            switch (combinationRule) {
                case 3: // store
                    for (intptr_t x = 0; x < width; x++) row[x] = fill;
                    break;
                case 7: // OR
                case 25: // paint (same as OR)
                    for (intptr_t x = 0; x < width; x++) row[x] |= fill;
                    break;
                case 0: // AND
                    for (intptr_t x = 0; x < width; x++) row[x] &= fill;
                    break;
                case 6: // XOR
                    for (intptr_t x = 0; x < width; x++) row[x] ^= fill;
                    break;
                case 1: // AND NOT
                    for (intptr_t x = 0; x < width; x++) row[x] &= ~fill;
                    break;
                case 4: // OR NOT
                    for (intptr_t x = 0; x < width; x++) row[x] |= ~fill;
                    break;
                case 24: { // alpha blend fill
                    uint32_t sa = (fill >> 24) & 0xFF;
                    if (sa == 255) {
                        for (intptr_t x = 0; x < width; x++) row[x] = fill;
                    } else if (sa > 0) {
                        uint32_t da = 255 - sa;
                        uint32_t fillRB = fill & 0xFF00FF;
                        uint32_t fillG  = fill & 0x00FF00;
                        for (intptr_t x = 0; x < width; x++) {
                            uint32_t d = row[x];
                            uint32_t rb = (fillRB * sa + (d & 0xFF00FF) * da + 0x800080) >> 8;
                            uint32_t g  = (fillG  * sa + (d & 0x00FF00) * da + 0x008000) >> 8;
                            row[x] = (rb & 0xFF00FF) | (g & 0x00FF00) | 0xFF000000;
                        }
                    }
                    // sa == 0: fully transparent, nothing to do
                    break;
                }
                default: {
                    return PrimitiveResult::Failure;
                }
            }
        }
        showDisplayBits(destForm, destX, destY, destX + width, destY + height);
        return PrimitiveResult::Success;
    }

    // --- Source operations ---
    if (!sourceForm.isObject()) {
        return PrimitiveResult::Failure;
    }

    Oop srcBits = memory_.fetchPointer(FormBits, sourceForm);
    intptr_t srcWidth = bitBltField(memory_, sourceForm, FormWidth);
    intptr_t srcHeight = bitBltField(memory_, sourceForm, FormHeight);
    intptr_t srcDepth = bitBltField(memory_, sourceForm, FormDepth);

    if (srcBits.isNil() || !srcBits.isObject() || srcBits.isSmallInteger() || srcBits.rawBits() < 0x10000) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* srcBitsHdr = srcBits.asObjectPtr();
    uint8_t* srcBytes = srcBitsHdr->bytes();
    size_t srcBitsSize = srcBitsHdr->byteSize();

    // Clip to source form bounds
    if (sourceX < 0) { intptr_t d = -sourceX; width -= d; destX += d; sourceX = 0; }
    if (sourceY < 0) { intptr_t d = -sourceY; height -= d; destY += d; sourceY = 0; }
    if (sourceX + width > srcWidth) width = srcWidth - sourceX;
    if (sourceY + height > srcHeight) height = srcHeight - sourceY;
    if (width <= 0 || height <= 0) return PrimitiveResult::Success;

    // Color map extraction
    Oop colorMap = memory_.fetchPointer(BBColorMap, bitBlt);
    uint32_t* cmTable = nullptr;
    size_t cmSize = 0;
    if (!colorMap.isNil() && colorMap.isObject()) {
        ObjectHeader* cmHdr = colorMap.asObjectPtr();
        cmTable = reinterpret_cast<uint32_t*>(cmHdr->bytes());
        cmSize = cmHdr->byteSize() / 4;
    }

    // Detect compressed source form (ByteArray bits that are too small for claimed dimensions)
    {
        auto srcBitsFmt = srcBitsHdr->format();
        bool isByteArray = (srcBitsFmt >= ObjectFormat::Indexable8 && srcBitsFmt <= ObjectFormat::Indexable8_7);
        if (isByteArray && srcDepth == 32) {
            size_t expectedBytes = static_cast<size_t>(srcWidth) * srcHeight * 4;
            if (srcBitsSize < expectedBytes) {
                static int compressedCount = 0;
                if (compressedCount++ < 5) {
                    fprintf(stderr, "[BITBLT] Compressed source form detected: %zux%ld d=%ld bits=%zu (need %zu). "
                            "Returning Failure to trigger unhibernate.\n",
                            (size_t)srcWidth, srcHeight, srcDepth, srcBitsSize, expectedBytes);
                }
                return PrimitiveResult::Failure;
            }
        }
    }

    // --- Handle non-32-bit destination depths ---
    // Depth-1 destinations (cursor masks, bitmap operations)
    if (destDepth == 1 && srcDepth == 32) {
        // In 1-bit forms, bits are stored MSB-first in 32-bit words.
        // Each word holds 32 pixels; pixel at x: word[x/32] bit (31 - x%32).
        intptr_t destWordsPerRow = (destWidth + 31) / 32;
        intptr_t srcPitch = srcWidth;
        uint32_t* srcPixels = reinterpret_cast<uint32_t*>(srcBytes);
        uint32_t* destWords = destPixels; // destPixels is already uint32_t*

        // Bounds check: ensure we can access dest rows
        size_t requiredDestWords = static_cast<size_t>((destY + height - 1) * destWordsPerRow + (destX + width + 31) / 32);
        if (requiredDestWords * 4 > destBitsSize) return PrimitiveResult::Failure;

        // Bounds check source
        intptr_t srcMaxPixels = static_cast<intptr_t>(srcBitsSize / 4);
        intptr_t srcActualHeight = (srcPitch > 0) ? (srcMaxPixels / srcPitch) : 0;
        if (sourceY + height > srcActualHeight) height = srcActualHeight - sourceY;
        if (sourceX + width > srcWidth) width = srcWidth - sourceX;
        if (height <= 0 || width <= 0) return PrimitiveResult::Success;

        for (intptr_t y = 0; y < height; y++) {
            uint32_t* srcRow = srcPixels + (sourceY + y) * srcPitch + sourceX;
            uint32_t* destRow = destWords + (destY + y) * destWordsPerRow;
            for (intptr_t x = 0; x < width; x++) {
                intptr_t dx = destX + x;
                intptr_t wordIdx = dx / 32;
                uint32_t bitMask = 0x80000000u >> (dx % 32);
                // Convert 32-bit pixel to 1-bit: non-zero → 1
                uint32_t srcBit = (srcRow[x] != 0) ? bitMask : 0;
                switch (combinationRule) {
                    case 3: case 34: // store
                        destRow[wordIdx] = (destRow[wordIdx] & ~bitMask) | srcBit;
                        break;
                    case 0: // AND
                        if (!srcBit) destRow[wordIdx] &= ~bitMask;
                        break;
                    case 7: case 25: // OR
                        if (srcBit) destRow[wordIdx] |= bitMask;
                        break;
                    case 6: // XOR
                        if (srcBit) destRow[wordIdx] ^= bitMask;
                        break;
                    case 1: // AND complement
                        if (srcBit) destRow[wordIdx] &= ~bitMask;
                        break;
                    default: // For unsupported rules, just store
                        destRow[wordIdx] = (destRow[wordIdx] & ~bitMask) | srcBit;
                        break;
                }
            }
        }
        showDisplayBits(destForm, destX, destY, destX + width, destY + height);
        return PrimitiveResult::Success;
    }

    // Depth-8 destinations (cursor forms, grayscale operations)
    if (destDepth == 8 && srcDepth == 32) {
        intptr_t destBytesPerRow = ((destWidth + 3) / 4) * 4; // word-aligned
        uint8_t* destBytes8 = reinterpret_cast<uint8_t*>(destPixels);
        intptr_t srcPitch = srcWidth;
        uint32_t* srcPixels = reinterpret_cast<uint32_t*>(srcBytes);

        size_t requiredDestBytes8 = static_cast<size_t>((destY + height - 1) * destBytesPerRow + destX + width);
        if (requiredDestBytes8 > destBitsSize) return PrimitiveResult::Failure;

        intptr_t srcMaxPixels = static_cast<intptr_t>(srcBitsSize / 4);
        intptr_t srcActualHeight = (srcPitch > 0) ? (srcMaxPixels / srcPitch) : 0;
        if (sourceY + height > srcActualHeight) height = srcActualHeight - sourceY;
        if (sourceX + width > srcWidth) width = srcWidth - sourceX;
        if (height <= 0 || width <= 0) return PrimitiveResult::Success;

        for (intptr_t y = 0; y < height; y++) {
            uint32_t* srcRow = srcPixels + (sourceY + y) * srcPitch + sourceX;
            uint8_t* dstRow = destBytes8 + (destY + y) * destBytesPerRow + destX;
            for (intptr_t x = 0; x < width; x++) {
                // Convert 32-bit ARGB to 8-bit grayscale (luminance)
                uint32_t s = srcRow[x];
                uint8_t r = (s >> 16) & 0xFF;
                uint8_t g = (s >> 8) & 0xFF;
                uint8_t b = s & 0xFF;
                uint8_t gray = static_cast<uint8_t>((r * 77 + g * 150 + b * 29) >> 8);
                switch (combinationRule) {
                    case 3: case 34: dstRow[x] = gray; break;
                    case 0: dstRow[x] &= gray; break;
                    case 7: case 25: dstRow[x] |= gray; break;
                    case 6: dstRow[x] ^= gray; break;
                    default: dstRow[x] = gray; break;
                }
            }
        }
        showDisplayBits(destForm, destX, destY, destX + width, destY + height);
        return PrimitiveResult::Success;
    }

    // Depth-8 dest with 1-bit source (cursor form rendering)
    if (destDepth == 8 && srcDepth == 1) {
        intptr_t destBytesPerRow = ((destWidth + 3) / 4) * 4; // word-aligned
        uint8_t* destBytes8 = reinterpret_cast<uint8_t*>(destPixels);
        intptr_t srcWordsPerRow = (srcWidth + 31) / 32;

        size_t requiredDestBytes8 = static_cast<size_t>((destY + height - 1) * destBytesPerRow + destX + width);
        if (requiredDestBytes8 > destBitsSize) return PrimitiveResult::Failure;

        size_t requiredSrcBytes = static_cast<size_t>((sourceY + height) * srcWordsPerRow * 4);
        if (requiredSrcBytes > srcBitsSize) return PrimitiveResult::Failure;

        // Color map: map bit 0 → color0, bit 1 → color1
        // For 8-bit depth, default: 0→black(0), 1→white(255)
        uint8_t color0 = 0;
        uint8_t color1 = 255;
        if (cmTable && cmSize >= 2) {
            color0 = static_cast<uint8_t>(cmTable[0] & 0xFF);
            color1 = static_cast<uint8_t>(cmTable[1] & 0xFF);
        }

        uint32_t* srcWords = reinterpret_cast<uint32_t*>(srcBytes);
        for (intptr_t y = 0; y < height; y++) {
            uint32_t* srcRow = srcWords + (sourceY + y) * srcWordsPerRow;
            uint8_t* dstRow = destBytes8 + (destY + y) * destBytesPerRow + destX;
            for (intptr_t x = 0; x < width; x++) {
                intptr_t sx = sourceX + x;
                uint32_t srcBit = (srcRow[sx / 32] >> (31 - sx % 32)) & 1;
                uint8_t srcVal = srcBit ? color1 : color0;
                switch (combinationRule) {
                    case 3: case 34: dstRow[x] = srcVal; break;
                    case 0: dstRow[x] &= srcVal; break;
                    case 7: case 25: dstRow[x] |= srcVal; break;
                    case 6: dstRow[x] ^= srcVal; break;
                    case 1: if (srcBit) dstRow[x] &= ~srcVal; break;
                    default: dstRow[x] = srcVal; break;
                }
            }
        }
        showDisplayBits(destForm, destX, destY, destX + width, destY + height);
        return PrimitiveResult::Success;
    }

    // Depth-1 source to depth-1 dest (common for mask operations)
    if (destDepth == 1 && srcDepth == 1) {
        intptr_t destWordsPerRow = (destWidth + 31) / 32;
        intptr_t srcWordsPerRow = (srcWidth + 31) / 32;
        uint32_t* srcWords = reinterpret_cast<uint32_t*>(srcBytes);
        uint32_t* destWords = destPixels;

        size_t requiredDestWords = static_cast<size_t>((destY + height - 1) * destWordsPerRow + (destX + width + 31) / 32);
        if (requiredDestWords * 4 > destBitsSize) return PrimitiveResult::Failure;

        for (intptr_t y = 0; y < height; y++) {
            uint32_t* srcRow = srcWords + (sourceY + y) * srcWordsPerRow;
            uint32_t* destRow = destWords + (destY + y) * destWordsPerRow;
            for (intptr_t x = 0; x < width; x++) {
                intptr_t sx = sourceX + x;
                intptr_t dx = destX + x;
                uint32_t srcBit = (srcRow[sx / 32] >> (31 - sx % 32)) & 1;
                uint32_t destBitMask = 0x80000000u >> (dx % 32);
                switch (combinationRule) {
                    case 3: case 34:
                        destRow[dx / 32] = (destRow[dx / 32] & ~destBitMask) | (srcBit ? destBitMask : 0);
                        break;
                    case 0:
                        if (!srcBit) destRow[dx / 32] &= ~destBitMask;
                        break;
                    case 7: case 25:
                        if (srcBit) destRow[dx / 32] |= destBitMask;
                        break;
                    case 6:
                        if (srcBit) destRow[dx / 32] ^= destBitMask;
                        break;
                    default:
                        destRow[dx / 32] = (destRow[dx / 32] & ~destBitMask) | (srcBit ? destBitMask : 0);
                        break;
                }
            }
        }
        showDisplayBits(destForm, destX, destY, destX + width, destY + height);
        return PrimitiveResult::Success;
    }

    // Unsupported depth combinations
    if (destDepth != 32) {
        static int nonD32Count = 0;
        if (nonD32Count++ < 5) {
            fprintf(stderr, "[BITBLT-FAIL] destDepth=%ld srcDepth=%ld rule=%ld src=%ldx%ld dst=%ldx%ld\n",
                    destDepth, srcDepth, combinationRule, srcWidth, srcHeight, destWidth, destHeight);
        }
        return PrimitiveResult::Failure;
    }
    size_t requiredDestBytes = static_cast<size_t>((destY + height - 1) * destWidth + destX + width) * 4;
    if (requiredDestBytes > destBitsSize) {
        return PrimitiveResult::Failure;
    }

    // Calculate source pitch in bytes
    intptr_t srcPixelsPerWord = (srcDepth > 0) ? (32 / srcDepth) : 1;
    intptr_t srcPitchBytes = ((srcWidth + srcPixelsPerWord - 1) / srcPixelsPerWord) * 4;

    // 32-bit source to 32-bit dest
    if (srcDepth == 32) {
        uint32_t* srcPixels = reinterpret_cast<uint32_t*>(srcBytes);
        intptr_t srcPitch = srcWidth;

        // Clip to available source data (standard VM clips rather than failing)
        intptr_t srcMaxPixels = static_cast<intptr_t>(srcBitsSize / 4);
        intptr_t srcActualHeight = (srcPitch > 0) ? (srcMaxPixels / srcPitch) : 0;
        if (sourceY + height > srcActualHeight) {
            height = srcActualHeight - sourceY;
        }
        if (sourceX + width > srcWidth) {
            width = srcWidth - sourceX;
        }
        if (height <= 0 || width <= 0) {
            // Nothing to copy after clipping
            showDisplayBits(destForm, destX, destY, destX, destY);
            return PrimitiveResult::Success;
        }

        for (intptr_t y = 0; y < height; y++) {
            uint32_t* srcRow = srcPixels + (sourceY + y) * srcPitch + sourceX;
            uint32_t* dstRow = destPixels + (destY + y) * destPitch + destX;
            uint32_t ht = halftoneWords ? halftoneWords[(destY + y) % halftoneHeight] : 0xFFFFFFFF;
            switch (combinationRule) {
                case 3:  // store (source AND halftone)
                case 34: // sourceWord
                    if (ht == 0xFFFFFFFF) {
                        memcpy(dstRow, srcRow, width * 4);
                    } else {
                        for (intptr_t x = 0; x < width; x++) dstRow[x] = srcRow[x] & ht;
                    }
                    break;
                case 0:  // AND: dest = dest AND (source AND halftone)
                    for (intptr_t x = 0; x < width; x++) dstRow[x] &= (srcRow[x] & ht);
                    break;
                case 6:  // XOR: dest = dest XOR (source AND halftone)
                    for (intptr_t x = 0; x < width; x++) dstRow[x] ^= (srcRow[x] & ht);
                    break;
                case 7:  // OR: dest = dest OR (source AND halftone)
                case 25: // paint (same as OR for 32-bit)
                    for (intptr_t x = 0; x < width; x++) dstRow[x] |= (srcRow[x] & ht);
                    break;
                case 1:  // AND complement: dest = dest AND NOT(source AND halftone)
                    for (intptr_t x = 0; x < width; x++) dstRow[x] &= ~(srcRow[x] & ht);
                    break;
                case 4:  // OR NOT: dest = dest OR NOT(source AND halftone)
                    for (intptr_t x = 0; x < width; x++) dstRow[x] |= ~(srcRow[x] & ht);
                    break;
                case 24: { // alpha blend
                    for (intptr_t x = 0; x < width; x++) {
                        uint32_t s = srcRow[x];
                        uint32_t d = dstRow[x];
                        uint32_t sa = (s >> 24) & 0xFF;
                        if (sa == 255) { dstRow[x] = s; continue; }
                        if (sa == 0) continue;
                        uint32_t da = 255 - sa;
                        uint32_t rb = ((s & 0xFF00FF) * sa + (d & 0xFF00FF) * da + 0x800080) >> 8;
                        uint32_t g  = ((s & 0x00FF00) * sa + (d & 0x00FF00) * da + 0x008000) >> 8;
                        dstRow[x] = (rb & 0xFF00FF) | (g & 0x00FF00) | 0xFF000000;
                    }
                    break;
                }
                case 20: { // rgbAdd: dest = colorMap(source AND halftone) + dest, clamped per component
                    for (intptr_t x = 0; x < width; x++) {
                        uint32_t s = srcRow[x] & ht;
                        // Apply colorMap to remap source pixels (used in two-pass font rendering)
                        // Standard BitBlt: rgbMapPixel extracts top 4 bits of R,G,B → 12-bit index
                        // Masks from setupColorMasksFrom:8 to:4:
                        //   Red:   (pixel & 0x00F00000) >> 12  → bits 11-8 of index
                        //   Green: (pixel & 0x0000F000) >> 8   → bits 7-4 of index
                        //   Blue:  (pixel & 0x000000F0) >> 4   → bits 3-0 of index
                        if (cmTable && cmSize == 4096) {
                            uint32_t idx = ((s >> 12) & 0xF00) |
                                           ((s >> 8)  & 0x0F0) |
                                           ((s >> 4)  & 0x00F);
                            s = cmTable[idx];
                        }
                        uint32_t d = dstRow[x];
                        // partitionedAdd: add each byte lane with saturation at 255
                        uint32_t rA = std::min(((s >> 24) & 0xFF) + ((d >> 24) & 0xFF), 255u);
                        uint32_t rR = std::min(((s >> 16) & 0xFF) + ((d >> 16) & 0xFF), 255u);
                        uint32_t rG = std::min(((s >> 8) & 0xFF) + ((d >> 8) & 0xFF), 255u);
                        uint32_t rB = std::min((s & 0xFF) + (d & 0xFF), 255u);
                        dstRow[x] = (rA << 24) | (rR << 16) | (rG << 8) | rB;
                    }
                    break;
                }
                case 21: { // rgbSub: dest = (source AND halftone) - dest, clamped per component
                    for (intptr_t x = 0; x < width; x++) {
                        uint32_t s = srcRow[x] & ht;
                        uint32_t d = dstRow[x];
                        int rA = (int)((s >> 24) & 0xFF) - (int)((d >> 24) & 0xFF);
                        int rR = (int)((s >> 16) & 0xFF) - (int)((d >> 16) & 0xFF);
                        int rG = (int)((s >> 8) & 0xFF) - (int)((d >> 8) & 0xFF);
                        int rB = (int)(s & 0xFF) - (int)(d & 0xFF);
                        dstRow[x] = ((uint32_t)std::max(rA, 0) << 24) |
                                    ((uint32_t)std::max(rR, 0) << 16) |
                                    ((uint32_t)std::max(rG, 0) << 8) |
                                    (uint32_t)std::max(rB, 0);
                    }
                    break;
                }
                case 27: { // rgbMax: dest = max(source, dest) per component
                    for (intptr_t x = 0; x < width; x++) {
                        uint32_t s = srcRow[x] & ht;
                        uint32_t d = dstRow[x];
                        uint32_t rR = std::max((s >> 16) & 0xFF, (d >> 16) & 0xFF);
                        uint32_t rG = std::max((s >> 8) & 0xFF, (d >> 8) & 0xFF);
                        uint32_t rB = std::max(s & 0xFF, d & 0xFF);
                        uint32_t rA = std::max((s >> 24) & 0xFF, (d >> 24) & 0xFF);
                        dstRow[x] = (rA << 24) | (rR << 16) | (rG << 8) | rB;
                    }
                    break;
                }
                case 28: { // rgbMin: dest = min(source, dest) per component
                    for (intptr_t x = 0; x < width; x++) {
                        uint32_t s = srcRow[x] & ht;
                        uint32_t d = dstRow[x];
                        uint32_t rR = std::min((s >> 16) & 0xFF, (d >> 16) & 0xFF);
                        uint32_t rG = std::min((s >> 8) & 0xFF, (d >> 8) & 0xFF);
                        uint32_t rB = std::min(s & 0xFF, d & 0xFF);
                        uint32_t rA = std::min((s >> 24) & 0xFF, (d >> 24) & 0xFF);
                        dstRow[x] = (rA << 24) | (rR << 16) | (rG << 8) | rB;
                    }
                    break;
                }
                case 26: { // erase: dest = dest AND NOT source
                    for (intptr_t x = 0; x < width; x++) dstRow[x] &= ~srcRow[x];
                    break;
                }
                case 37: { // rgbMul: dest = (source AND halftone) * dest / 255 per component
                    for (intptr_t x = 0; x < width; x++) {
                        uint32_t s = srcRow[x] & ht;
                        uint32_t d = dstRow[x];
                        uint32_t rA = (((s >> 24) & 0xFF) * ((d >> 24) & 0xFF) + 127) / 255;
                        uint32_t rR = (((s >> 16) & 0xFF) * ((d >> 16) & 0xFF) + 127) / 255;
                        uint32_t rG = (((s >> 8) & 0xFF) * ((d >> 8) & 0xFF) + 127) / 255;
                        uint32_t rB = ((s & 0xFF) * (d & 0xFF) + 127) / 255;
                        dstRow[x] = (rA << 24) | (rR << 16) | (rG << 8) | rB;
                    }
                    break;
                }
                case 41: { // rgbComponentAlpha: source channels are per-channel alpha for halftone color
                    // On Retina/non-LCD displays, sub-pixel rendering creates visible color fringing.
                    // Convert per-channel alpha to uniform grayscale alpha for correct appearance.
                    uint32_t cR = (ht >> 16) & 0xFF;
                    uint32_t cG = (ht >> 8) & 0xFF;
                    uint32_t cB = ht & 0xFF;
                    for (intptr_t x = 0; x < width; x++) {
                        uint32_t s = srcRow[x];
                        uint32_t d = dstRow[x];
                        uint32_t aR = (s >> 16) & 0xFF;
                        uint32_t aG = (s >> 8) & 0xFF;
                        uint32_t aB = s & 0xFF;
                        if ((aR | aG | aB) == 0) continue;
                        // Use uniform alpha (average of per-channel values)
                        uint32_t a = (aR + aG + aB + 1) / 3;
                        uint32_t da = 255 - a;
                        uint32_t rR = (cR * a + ((d >> 16) & 0xFF) * da) / 255;
                        uint32_t rG = (cG * a + ((d >> 8) & 0xFF) * da) / 255;
                        uint32_t rB = (cB * a + (d & 0xFF) * da) / 255;
                        dstRow[x] = 0xFF000000 | (rR << 16) | (rG << 8) | rB;
                    }
                    break;
                }
                default: {
                    static int def32Count = 0;
                    if (def32Count++ < 20) {
                        fprintf(stderr, "[BITBLT-32to32-FAIL] rule=%ld src=%ldx%ld dst=%ldx%ld "
                                "dXY=(%ld,%ld) WH=(%ld,%ld) ht=%s\n",
                                combinationRule, srcWidth, srcHeight, destWidth, destHeight,
                                destX, destY, width, height,
                                halftoneWords ? "present" : "nil");
                    }
                    return PrimitiveResult::Failure;
                }
            }
        }
        showDisplayBits(destForm, destX, destY, destX + width, destY + height);
        return PrimitiveResult::Success;
    }

    // 1-bit source to 32-bit dest (used for strike font text rendering)
    if (srcDepth == 1) {
        // Default colors: 0 = black (0xFF000000), 1 = white (0xFFFFFFFF)
        uint32_t color0 = 0xFF000000;
        uint32_t color1 = 0xFFFFFFFF;
        if (cmTable && cmSize >= 2) {
            color0 = cmTable[0];
            color1 = cmTable[1];
        }

        size_t requiredSrcBytes = static_cast<size_t>(sourceY + height) * srcPitchBytes;
        if (requiredSrcBytes > srcBitsSize) { return PrimitiveResult::Failure; }

        for (intptr_t y = 0; y < height; y++) {
            uint32_t* dstRow = destPixels + (destY + y) * destPitch + destX;
            intptr_t srcRowOffset = (sourceY + y) * srcPitchBytes;
            uint32_t ht = halftoneWords ? halftoneWords[(destY + y) % halftoneHeight] : 0xFFFFFFFF;
            for (intptr_t x = 0; x < width; x++) {
                intptr_t sx = sourceX + x;
                // Bit ordering: MSB first within each 32-bit word
                intptr_t wordIndex = sx / 32;
                intptr_t bitIndex = 31 - (sx % 32);
                uint32_t srcWord = *reinterpret_cast<uint32_t*>(srcBytes + srcRowOffset + wordIndex * 4);
                int bit = (srcWord >> bitIndex) & 1;
                uint32_t srcPixel = bit ? color1 : color0;
                srcPixel &= ht;

                switch (combinationRule) {
                    case 3: case 34:
                        dstRow[x] = srcPixel;
                        break;
                    case 25: case 7: // paint/OR
                        dstRow[x] |= srcPixel;
                        break;
                    case 0: // AND
                        dstRow[x] &= srcPixel;
                        break;
                    case 6: // XOR
                        dstRow[x] ^= srcPixel;
                        break;
                    case 1: // AND NOT
                        dstRow[x] &= ~srcPixel;
                        break;
                    case 24: { // alpha blend
                        uint32_t sa = (srcPixel >> 24) & 0xFF;
                        if (sa == 255) { dstRow[x] = srcPixel; break; }
                        if (sa == 0) break;
                        uint32_t da = 255 - sa;
                        uint32_t d = dstRow[x];
                        uint32_t rb = ((srcPixel & 0xFF00FF) * sa + (d & 0xFF00FF) * da + 0x800080) >> 8;
                        uint32_t g  = ((srcPixel & 0x00FF00) * sa + (d & 0x00FF00) * da + 0x008000) >> 8;
                        dstRow[x] = (rb & 0xFF00FF) | (g & 0x00FF00) | 0xFF000000;
                        break;
                    }
                    default: {
                        return PrimitiveResult::Failure;
                    }
                }
            }
        }
        showDisplayBits(destForm, destX, destY, destX + width, destY + height);
        return PrimitiveResult::Success;
    }

    // 8-bit source to 32-bit dest (used for grayscale forms)
    if (srcDepth == 8) {
        size_t requiredSrcBytes = static_cast<size_t>(sourceY + height) * srcPitchBytes;
        if (requiredSrcBytes > srcBitsSize) { return PrimitiveResult::Failure; }

        for (intptr_t y = 0; y < height; y++) {
            uint32_t* dstRow = destPixels + (destY + y) * destPitch + destX;
            intptr_t srcRowOffset = (sourceY + y) * srcPitchBytes;
            uint32_t ht = halftoneWords ? halftoneWords[(destY + y) % halftoneHeight] : 0xFFFFFFFF;
            for (intptr_t x = 0; x < width; x++) {
                intptr_t sx = sourceX + x;
                // 8-bit: 4 pixels per word, MSB first
                intptr_t byteOffset = srcRowOffset + sx;
                // Pharo stores 8-bit pixels in 32-bit words, big-endian byte order within word
                intptr_t wordIdx = sx / 4;
                intptr_t byteInWord = 3 - (sx % 4);  // MSB first
                uint8_t pixelIdx = *(srcBytes + srcRowOffset + wordIdx * 4 + (3 - byteInWord));

                uint32_t srcPixel;
                if (cmTable && pixelIdx < cmSize) {
                    srcPixel = cmTable[pixelIdx];
                } else {
                    // Default: treat as grayscale
                    srcPixel = 0xFF000000 | (pixelIdx << 16) | (pixelIdx << 8) | pixelIdx;
                }
                srcPixel &= ht;

                switch (combinationRule) {
                    case 3: case 34:
                        dstRow[x] = srcPixel;
                        break;
                    case 25: case 7:
                        dstRow[x] |= srcPixel;
                        break;
                    case 24: {
                        uint32_t sa = (srcPixel >> 24) & 0xFF;
                        if (sa == 255) { dstRow[x] = srcPixel; break; }
                        if (sa == 0) break;
                        uint32_t da = 255 - sa;
                        uint32_t d = dstRow[x];
                        uint32_t rb = ((srcPixel & 0xFF00FF) * sa + (d & 0xFF00FF) * da + 0x800080) >> 8;
                        uint32_t g  = ((srcPixel & 0x00FF00) * sa + (d & 0x00FF00) * da + 0x008000) >> 8;
                        dstRow[x] = (rb & 0xFF00FF) | (g & 0x00FF00) | 0xFF000000;
                        break;
                    }
                    default: {
                        return PrimitiveResult::Failure;
                    }
                }
            }
        }
        showDisplayBits(destForm, destX, destY, destX + width, destY + height);
        return PrimitiveResult::Success;
    }

    // 16-bit source to 32-bit dest
    if (srcDepth == 16) {
        size_t requiredSrcBytes = static_cast<size_t>(sourceY + height) * srcPitchBytes;
        if (requiredSrcBytes > srcBitsSize) { return PrimitiveResult::Failure; }

        uint16_t* srcPixels16 = reinterpret_cast<uint16_t*>(srcBytes);
        intptr_t srcPitch16 = srcPitchBytes / 2;

        for (intptr_t y = 0; y < height; y++) {
            uint16_t* srcRow = srcPixels16 + (sourceY + y) * srcPitch16 + sourceX;
            uint32_t* dstRow = destPixels + (destY + y) * destPitch + destX;
            for (intptr_t x = 0; x < width; x++) {
                uint16_t s16 = srcRow[x];
                // Convert 5-5-5 to 8-8-8-8 (ARGB)
                uint32_t r = ((s16 >> 10) & 0x1F) * 255 / 31;
                uint32_t g = ((s16 >> 5) & 0x1F) * 255 / 31;
                uint32_t b = (s16 & 0x1F) * 255 / 31;
                uint32_t srcPixel = 0xFF000000 | (r << 16) | (g << 8) | b;
                switch (combinationRule) {
                    case 3: case 34: dstRow[x] = srcPixel; break;
                    case 25: case 7: dstRow[x] |= srcPixel; break;
                    default: {
                        return PrimitiveResult::Failure;
                    }
                }
            }
        }
        showDisplayBits(destForm, destX, destY, destX + width, destY + height);
        return PrimitiveResult::Success;
    }

    // 2-bit and 4-bit sources to 32-bit dest
    if (srcDepth == 2 || srcDepth == 4) {
        size_t requiredSrcBytes = static_cast<size_t>(sourceY + height) * srcPitchBytes;
        if (requiredSrcBytes > srcBitsSize) { return PrimitiveResult::Failure; }

        intptr_t pixelsPerWord = 32 / srcDepth;
        intptr_t pixelMask = (1 << srcDepth) - 1;

        for (intptr_t y = 0; y < height; y++) {
            uint32_t* dstRow = destPixels + (destY + y) * destPitch + destX;
            intptr_t srcRowOffset = (sourceY + y) * srcPitchBytes;
            for (intptr_t x = 0; x < width; x++) {
                intptr_t sx = sourceX + x;
                intptr_t wordIndex = sx / pixelsPerWord;
                intptr_t pixelInWord = (pixelsPerWord - 1) - (sx % pixelsPerWord); // MSB first
                uint32_t srcWord = *reinterpret_cast<uint32_t*>(srcBytes + srcRowOffset + wordIndex * 4);
                uint32_t pixelIdx = (srcWord >> (pixelInWord * srcDepth)) & pixelMask;

                uint32_t srcPixel;
                if (cmTable && pixelIdx < cmSize) {
                    srcPixel = cmTable[pixelIdx];
                } else {
                    uint32_t gray = pixelIdx * 255 / pixelMask;
                    srcPixel = 0xFF000000 | (gray << 16) | (gray << 8) | gray;
                }

                switch (combinationRule) {
                    case 3: case 34: dstRow[x] = srcPixel; break;
                    case 25: case 7: dstRow[x] |= srcPixel; break;
                    case 24: {
                        uint32_t sa = (srcPixel >> 24) & 0xFF;
                        if (sa == 255) { dstRow[x] = srcPixel; break; }
                        if (sa == 0) break;
                        uint32_t da = 255 - sa;
                        uint32_t d = dstRow[x];
                        uint32_t rb = ((srcPixel & 0xFF00FF) * sa + (d & 0xFF00FF) * da + 0x800080) >> 8;
                        uint32_t g  = ((srcPixel & 0x00FF00) * sa + (d & 0x00FF00) * da + 0x008000) >> 8;
                        dstRow[x] = (rb & 0xFF00FF) | (g & 0x00FF00) | 0xFF000000;
                        break;
                    }
                    default: {
                        return PrimitiveResult::Failure;
                    }
                }
            }
        }
        showDisplayBits(destForm, destX, destY, destX + width, destY + height);
        return PrimitiveResult::Success;
    }

    // Unsupported source depth / destination depth
    {
        static int unsupCount = 0;
        if (unsupCount++ < 20) {
            fprintf(stderr, "[BITBLT-UNSUP] srcDepth=%ld destDepth=%ld rule=%ld "
                    "src=%ldx%ld dst=%ldx%ld dXY=(%ld,%ld) WH=(%ld,%ld)\n",
                    srcDepth, destDepth, combinationRule,
                    srcWidth, srcHeight, destWidth, destHeight,
                    destX, destY, width, height);
        }
    }
    return PrimitiveResult::Failure;
}

// ===== SurfacePlugin Named Primitives =====
// These are dispatched via primitiveExternalCall as named primitives
// in module 'SurfacePlugin'.

// primitiveCreateManualSurface (module: SurfacePlugin)
// rcvr primCreateManualSurfaceWidth: w height: h rowPitch: p depth: d isMSB: msb
// Stack: rcvr w h p d msb (5 args)
// Returns: surface ID (SmallInteger)
PrimitiveResult Interpreter::primitiveCreateManualSurface(int argCount) {
    if (argCount != 5) return PrimitiveResult::Failure;

    Oop msbOop = stackTop();       // arg 5: isMSB (Boolean)
    Oop depthOop = stackValue(1);  // arg 4: depth
    Oop pitchOop = stackValue(2);  // arg 3: rowPitch
    Oop heightOop = stackValue(3); // arg 2: height
    Oop widthOop = stackValue(4);  // arg 1: width
    // stackValue(5) is receiver

    if (!widthOop.isSmallInteger() || !heightOop.isSmallInteger() ||
        !pitchOop.isSmallInteger() || !depthOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int w = static_cast<int>(widthOop.asSmallInteger());
    int h = static_cast<int>(heightOop.asSmallInteger());
    int p = static_cast<int>(pitchOop.asSmallInteger());
    int d = static_cast<int>(depthOop.asSmallInteger());
    bool msb = !msbOop.isNil() && msbOop != memory_.falseObject();

    // Find a free slot
    int surfaceID = -1;
    for (int i = 1; i < kMaxSurfaces; i++) {
        if (!g_surfaces[i].active) {
            surfaceID = i;
            break;
        }
    }
    if (surfaceID < 0) return PrimitiveResult::Failure;  // table full

    g_surfaces[surfaceID] = { true, w, h, p, d, msb, nullptr };

    // Pop args and receiver, push result
    popN(argCount + 1);
    push(Oop::fromSmallInteger(surfaceID));
    return PrimitiveResult::Success;
}

// primitiveDestroyManualSurface (module: SurfacePlugin)
// rcvr primDestroyManualSurface: surfaceID
// Stack: rcvr surfaceID (1 arg)
PrimitiveResult Interpreter::primitiveDestroyManualSurface(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop idOop = stackTop();
    if (!idOop.isSmallInteger()) return PrimitiveResult::Failure;

    int surfaceID = static_cast<int>(idOop.asSmallInteger());
    ManualSurface* s = lookupSurface(surfaceID);
    if (!s) return PrimitiveResult::Failure;

    s->active = false;
    s->bits = nullptr;

    // Pop arg, leave receiver
    pop();
    return PrimitiveResult::Success;
}

// primitiveSetManualSurfacePointer (module: SurfacePlugin)
// rcvr primManualSurfaceSetPointer: surfaceID pointer: aPointer
// Stack: rcvr surfaceID aPointer (2 args)
PrimitiveResult Interpreter::primitiveSetManualSurfacePointer(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop ptrOop = stackTop();       // arg 2: pointer (ExternalAddress)
    Oop idOop = stackValue(1);     // arg 1: surfaceID

    if (!idOop.isSmallInteger()) return PrimitiveResult::Failure;

    int surfaceID = static_cast<int>(idOop.asSmallInteger());
    ManualSurface* s = lookupSurface(surfaceID);
    if (!s) return PrimitiveResult::Failure;

    // Extract pointer from ExternalAddress (first slot contains the raw pointer)
    void* ptr = nullptr;
    if (ptrOop.isObject() && !ptrOop.isNil()) {
        ObjectHeader* ptrHdr = ptrOop.asObjectPtr();
        if (ptrHdr->byteSize() >= sizeof(void*)) {
            std::memcpy(&ptr, ptrHdr->bytes(), sizeof(void*));
        }
    } else if (ptrOop.isSmallInteger()) {
        // Sometimes pointers are passed as SmallIntegers
        ptr = reinterpret_cast<void*>(static_cast<uintptr_t>(ptrOop.asSmallInteger()));
    }

    s->bits = ptr;

    static int setPointerCount = 0;
    setPointerCount++;
    if (setPointerCount <= 20 || setPointerCount % 100 == 0) {
        const char* type = "unknown";
        if (ptrOop.isNil()) type = "nil";
        else if (ptrOop.isSmallInteger()) type = "SmallInt";
        else if (ptrOop.isObject()) {
            ObjectHeader* h = ptrOop.asObjectPtr();
            if (h->isBytesObject()) type = "BytesObj";
            else type = "PointerObj";
        }
        fprintf(stderr, "[SURFACE] setPointer: #%d surfID=%d ptr=%p type=%s oop=0x%llx (w=%d h=%d pitch=%d depth=%d)\n",
                setPointerCount, surfaceID, ptr, type, (unsigned long long)ptrOop.rawBits(),
                s->width, s->height, s->rowPitch, s->depth);
    }

    // Pop args, leave receiver
    popN(argCount);
    return PrimitiveResult::Success;
}

// Primitive 291: Draw loop (line drawing for BitBlt)
// Uses existing primitiveDrawLoop implementation (also primitive 104)

// Primitive 292: Compress bitmap to byte array (Pharo RLE compression)
// receiver compress: bm toByteArray: ba -> compressedSize
// TODO: Implement correct Pharo RLE format (encodeInt: with codes 0-3).
// For now, return Failure to use the Smalltalk fallback implementation.
PrimitiveResult Interpreter::primitiveCompressToByteArray(int argCount) {
    return PrimitiveResult::Failure;
}

// MiscPrimitivePlugin: primitiveDecompressFromByteArray
// receiver decompress: bm fromByteArray: ba at: index
// Pharo RLE format: sequence of {N D}* pairs
//   N = count*4 + code, variable-length encoded (decodeIntFrom:)
//   code 0: skip count words (no data)
//   code 1: count words, all 4 bytes = next byte
//   code 2: count words, all = next 4 bytes
//   code 3: count literal words (4*count bytes follow)
PrimitiveResult Interpreter::primitiveDecompressFromByteArray(int argCount) {
    if (argCount != 3) return PrimitiveResult::Failure;

    Oop indexOop = stackValue(0);    // index (1-based)
    Oop ba = stackValue(1);          // byteArray (compressed source)
    Oop bm = stackValue(2);          // bitmap (destination)
    // stackValue(3) = receiver (same as bm)

    if (!indexOop.isSmallInteger() || !ba.isObject() || !bm.isObject())
        return PrimitiveResult::Failure;

    size_t baSize = memory_.byteSizeOf(ba);
    size_t bmWords = memory_.byteSizeOf(bm) / 4;
    uint8_t* baBytes = ba.asObjectPtr()->bytes();
    uint32_t* bmData = reinterpret_cast<uint32_t*>(bm.asObjectPtr()->bytes());

    size_t i = static_cast<size_t>(indexOop.asSmallInteger()) - 1;  // convert 1-based to 0-based
    size_t k = 0;  // bitmap word write index

    while (i < baSize) {
        // Decode N using decodeIntFrom: encoding
        uint32_t anInt = baBytes[i]; i++;
        if (anInt > 223) {
            if (anInt <= 254) {
                if (i >= baSize) break;
                anInt = (anInt - 224) * 256 + baBytes[i]; i++;
            } else {
                // anInt == 255: next 4 bytes
                if (i + 4 > baSize) break;
                anInt = (static_cast<uint32_t>(baBytes[i]) << 24) |
                        (static_cast<uint32_t>(baBytes[i+1]) << 16) |
                        (static_cast<uint32_t>(baBytes[i+2]) << 8) |
                         static_cast<uint32_t>(baBytes[i+3]);
                i += 4;
            }
        }

        size_t n = anInt >> 2;
        uint32_t code = anInt & 3;

        if (k + n > bmWords) return PrimitiveResult::Failure;

        if (code == 0) {
            // Skip n words (leave destination unchanged)
            k += n;
        } else if (code == 1) {
            // n words with all 4 bytes = next byte
            if (i >= baSize) break;
            uint32_t b = baBytes[i]; i++;
            uint32_t data = b | (b << 8);
            data = data | (data << 16);
            for (size_t j = 0; j < n; j++) {
                bmData[k++] = data;
            }
        } else if (code == 2) {
            // n words all = next 4 bytes (big-endian)
            if (i + 4 > baSize) break;
            uint32_t data = (static_cast<uint32_t>(baBytes[i]) << 24) |
                            (static_cast<uint32_t>(baBytes[i+1]) << 16) |
                            (static_cast<uint32_t>(baBytes[i+2]) << 8) |
                             static_cast<uint32_t>(baBytes[i+3]);
            i += 4;
            for (size_t j = 0; j < n; j++) {
                bmData[k++] = data;
            }
        } else {
            // code == 3: n literal words (4n bytes, big-endian)
            if (i + n * 4 > baSize) break;
            for (size_t j = 0; j < n; j++) {
                uint32_t data = (static_cast<uint32_t>(baBytes[i]) << 24) |
                                (static_cast<uint32_t>(baBytes[i+1]) << 16) |
                                (static_cast<uint32_t>(baBytes[i+2]) << 8) |
                                 static_cast<uint32_t>(baBytes[i+3]);
                i += 4;
                bmData[k++] = data;
            }
        }
    }

    popN(3);  // pop 3 arguments
    // Leave receiver on stack (success)
    return PrimitiveResult::Success;
}

// Primitive 294: Find first occurrence of character in string
// MiscPrimitivePlugin: primitiveFindFirstInString
// ByteString class >> findFirstInString: aString inSet: inclusionMap startingAt: start
// Returns 1-based index of first byte where inclusionMap[byte] != 0, or 0 if none found.
PrimitiveResult Interpreter::primitiveFindFirstInString(int argCount) {
    if (argCount != 3) return PrimitiveResult::Failure;

    Oop startOop = stackTop();           // arg 3: start index
    Oop inclusionMapOop = stackValue(1); // arg 2: ByteArray of 256 entries
    Oop stringOop = stackValue(2);       // arg 1: the string to search

    if (!startOop.isSmallInteger()) return PrimitiveResult::Failure;
    if (!stringOop.isObject()) return PrimitiveResult::Failure;
    if (!inclusionMapOop.isObject()) return PrimitiveResult::Failure;

    // inclusionMap must be 256 bytes
    size_t mapSize = memory_.byteSizeOf(inclusionMapOop);
    if (mapSize != 256) return PrimitiveResult::Failure;

    intptr_t start = startOop.asSmallInteger();
    size_t stringSize = memory_.byteSizeOf(stringOop);

    if (start < 1) start = 1;

    // Search for first byte where inclusionMap[byte] != 0
    for (size_t i = static_cast<size_t>(start - 1); i < stringSize; i++) {
        uint8_t byte = memory_.fetchByte(i, stringOop);
        uint8_t mapEntry = memory_.fetchByte(byte, inclusionMapOop);
        if (mapEntry != 0) {
            popN(argCount + 1);  // args + receiver
            push(Oop::fromSmallInteger(static_cast<intptr_t>(i + 1)));  // 1-based
            return PrimitiveResult::Success;
        }
    }

    // Not found
    popN(argCount + 1);
    push(Oop::fromSmallInteger(0));
    return PrimitiveResult::Success;
}

// Primitive 295: Translate string characters using table
// aString startIndex stopIndex table primitiveTranslateStringWithTable -> aString
PrimitiveResult Interpreter::primitiveTranslateStringWithTable(int argCount) {
    if (argCount != 4) return PrimitiveResult::Failure;

    Oop tableOop = stackTop();
    Oop stopIndexOop = stackValue(1);
    Oop startIndexOop = stackValue(2);
    Oop stringOop = stackValue(3);

    if (!startIndexOop.isSmallInteger() || !stopIndexOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }
    if (!stringOop.isObject() || !tableOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Both string and table must be byte objects (format 16-23)
    // Match official VM: isBytes() check — fail for WideString tables
    ObjectHeader* strHdr = stringOop.asObjectPtr();
    ObjectHeader* tblHdr = tableOop.asObjectPtr();
    int strFmt = static_cast<int>(strHdr->format());
    int tblFmt = static_cast<int>(tblHdr->format());
    if (strFmt < 16 || strFmt > 23 || tblFmt < 16 || tblFmt > 23) {
        return PrimitiveResult::Failure;
    }

    intptr_t startIndex = startIndexOop.asSmallInteger();
    intptr_t stopIndex = stopIndexOop.asSmallInteger();
    size_t stringSize = memory_.byteSizeOf(stringOop);
    size_t tableSize = memory_.byteSizeOf(tableOop);

    if (tableSize < 256) {
        return PrimitiveResult::Failure;
    }

    // Smalltalk uses 1-based indexing
    if (startIndex < 1 || stopIndex > static_cast<intptr_t>(stringSize)) {
        return PrimitiveResult::Failure;
    }

    // Translate each character using byte-indexed table
    for (intptr_t i = startIndex - 1; i < stopIndex; i++) {
        uint8_t ch = memory_.fetchByte(static_cast<size_t>(i), stringOop);
        uint8_t translated = memory_.fetchByte(ch, tableOop);
        memory_.storeByte(static_cast<size_t>(i), stringOop, translated);
    }

    popN(4);  // arguments
    push(stringOop);
    return PrimitiveResult::Success;
}

// Primitive 296: Find substring in string
// aString key startIndex matchTable primitiveFindSubstring -> index or 0
PrimitiveResult Interpreter::primitiveFindSubstring(int argCount) {
    if (argCount != 4) return PrimitiveResult::Failure;

    Oop matchTableOop = stackTop();        // arg4: matchTable
    Oop startIndexOop = stackValue(1);     // arg3: startIndex
    Oop stringOop = stackValue(2);         // arg2: body (string to search in)
    Oop keyOop = stackValue(3);            // arg1: key (substring to find)

    if (!startIndexOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }
    if (!stringOop.isObject() || !keyOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    intptr_t startIndex = startIndexOop.asSmallInteger();
    size_t stringSize = memory_.byteSizeOf(stringOop);
    size_t keySize = memory_.byteSizeOf(keyOop);

    if (keySize == 0) {
        // Empty key: return 0 (not found) to prevent infinite loops in callers
        // like allRangesOfSubstring: which loop until findString returns 0
        popN(4);
        pop();
        push(Oop::fromSmallInteger(0));
        return PrimitiveResult::Success;
    }

    // Check if match table is provided for case-insensitive search
    bool useMatchTable = matchTableOop.isObject() && memory_.byteSizeOf(matchTableOop) >= 256;

    // Smalltalk uses 1-based indexing
    if (startIndex < 1) startIndex = 1;

    // Simple substring search
    for (size_t i = static_cast<size_t>(startIndex - 1); i + keySize <= stringSize; i++) {
        bool match = true;
        for (size_t j = 0; j < keySize && match; j++) {
            uint8_t strChar = memory_.fetchByte(i + j, stringOop);
            uint8_t keyChar = memory_.fetchByte(j, keyOop);

            if (useMatchTable) {
                strChar = memory_.fetchByte(strChar, matchTableOop);
                keyChar = memory_.fetchByte(keyChar, matchTableOop);
            }

            if (strChar != keyChar) {
                match = false;
            }
        }
        if (match) {
            popN(4);
            pop();
            push(Oop::fromSmallInteger(static_cast<intptr_t>(i + 1)));  // 1-based
            return PrimitiveResult::Success;
        }
    }

    // Not found
    popN(4);
    pop();
    push(Oop::fromSmallInteger(0));
    return PrimitiveResult::Success;
}

// Primitive 297: Get pixel value at coordinates
// aForm x y primitivePixelValueAt -> pixelValue
PrimitiveResult Interpreter::primitivePixelValueAt(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop yOop = stackTop();
    Oop xOop = stackValue(1);
    Oop formOop = stackValue(2);

    if (!xOop.isSmallInteger() || !yOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }
    if (!formOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    intptr_t x = xOop.asSmallInteger();
    intptr_t y = yOop.asSmallInteger();

    // Get form parameters
    Oop bits = memory_.fetchPointer(FormBits, formOop);
    intptr_t width = bitBltField(memory_, formOop, FormWidth);
    intptr_t height = bitBltField(memory_, formOop, FormHeight);
    intptr_t depth = bitBltField(memory_, formOop, FormDepth);

    if (bits.isNil() || !bits.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Bounds check
    if (x < 0 || x >= width || y < 0 || y >= height) {
        popN(2);  // args
        pop();    // receiver
        push(Oop::fromSmallInteger(0));
        return PrimitiveResult::Success;
    }

    uint32_t pixelValue = 0;

    if (depth == 32) {
        size_t offset = static_cast<size_t>(y * width + x);
        pixelValue = memory_.fetchWord32(offset, bits);
    } else if (depth == 16) {
        size_t wordOffset = static_cast<size_t>(y * width + x) / 2;
        uint32_t word = memory_.fetchWord32(wordOffset, bits);
        if ((x & 1) == 0) {
            pixelValue = (word >> 16) & 0xFFFF;
        } else {
            pixelValue = word & 0xFFFF;
        }
    } else if (depth == 8) {
        size_t byteOffset = static_cast<size_t>(y * width + x);
        pixelValue = memory_.fetchByte(byteOffset, bits);
    } else if (depth == 1) {
        size_t wordOffset = static_cast<size_t>(y * ((width + 31) / 32) + x / 32);
        uint32_t word = memory_.fetchWord32(wordOffset, bits);
        int bitPos = 31 - (x % 32);
        pixelValue = (word >> bitPos) & 1;
    } else {
        return PrimitiveResult::Failure;
    }

    popN(2);
    pop();
    push(Oop::fromSmallInteger(static_cast<intptr_t>(pixelValue)));
    return PrimitiveResult::Success;
}

// Primitive 298: Set pixel value at coordinates
// aForm x y value primitivePixelValueAtPut -> aForm
PrimitiveResult Interpreter::primitivePixelValueAtPut(int argCount) {
    if (argCount != 3) return PrimitiveResult::Failure;

    Oop valueOop = stackTop();
    Oop yOop = stackValue(1);
    Oop xOop = stackValue(2);
    Oop formOop = stackValue(3);

    if (!xOop.isSmallInteger() || !yOop.isSmallInteger() || !valueOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }
    if (!formOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    intptr_t x = xOop.asSmallInteger();
    intptr_t y = yOop.asSmallInteger();
    uint32_t pixelValue = static_cast<uint32_t>(valueOop.asSmallInteger());

    // Get form parameters
    Oop bits = memory_.fetchPointer(FormBits, formOop);
    intptr_t width = bitBltField(memory_, formOop, FormWidth);
    intptr_t height = bitBltField(memory_, formOop, FormHeight);
    intptr_t depth = bitBltField(memory_, formOop, FormDepth);

    if (bits.isNil() || !bits.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Bounds check
    if (x < 0 || x >= width || y < 0 || y >= height) {
        popN(3);
        push(formOop);
        return PrimitiveResult::Success;
    }

    if (depth == 32) {
        size_t offset = static_cast<size_t>(y * width + x);
        memory_.storeWord32(offset, bits, pixelValue);
    } else if (depth == 16) {
        size_t wordOffset = static_cast<size_t>(y * width + x) / 2;
        uint32_t word = memory_.fetchWord32(wordOffset, bits);
        if ((x & 1) == 0) {
            word = (word & 0x0000FFFF) | ((pixelValue & 0xFFFF) << 16);
        } else {
            word = (word & 0xFFFF0000) | (pixelValue & 0xFFFF);
        }
        memory_.storeWord32(wordOffset, bits, word);
    } else if (depth == 8) {
        size_t byteOffset = static_cast<size_t>(y * width + x);
        memory_.storeByte(byteOffset, bits, static_cast<uint8_t>(pixelValue));
    } else if (depth == 1) {
        size_t wordOffset = static_cast<size_t>(y * ((width + 31) / 32) + x / 32);
        uint32_t word = memory_.fetchWord32(wordOffset, bits);
        int bitPos = 31 - (x % 32);
        if (pixelValue & 1) {
            word |= (1U << bitPos);
        } else {
            word &= ~(1U << bitPos);
        }
        memory_.storeWord32(wordOffset, bits, word);
    } else {
        return PrimitiveResult::Failure;
    }

    popN(3);
    push(formOop);
    return PrimitiveResult::Success;
}

// Primitive 299: Warp bits (texture mapping/rotation)
// aBitBlt primitiveWarpBits -> aBitBlt
// Advanced BitBlt with arbitrary quadrilateral source mapping
PrimitiveResult Interpreter::primitiveWarpBits(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // WarpBlt is complex - it does arbitrary quadrilateral-to-rectangle mapping
    // Used for rotation, scaling, and texture mapping
    // For now, fail to Smalltalk fallback
    return PrimitiveResult::Failure;
}

// ===== SOUND PRIMITIVES (300-329) =====
// Sound primitives require platform-specific audio APIs (CoreAudio on iOS).
// Most fail to Smalltalk fallback which provides software synthesis.
// A full implementation would integrate with AVFoundation/AudioToolbox.

// Sound system state (minimal stub)
static bool soundOutputRunning = false;
static bool soundInputRunning = false;
static int soundSampleRate = 44100;
static int soundVolume = 100;  // 0-100
static int soundBalance = 50;  // 0=left, 50=center, 100=right

// Primitive 300: Start sound output
// sampleRate stereo semaIndex primitiveSoundStart -> success
PrimitiveResult Interpreter::primitiveSoundStart(int argCount) {
    if (argCount != 3) return PrimitiveResult::Failure;

    Oop semaIndexOop = stackTop();
    Oop stereoOop = stackValue(1);
    Oop sampleRateOop = stackValue(2);

    if (!sampleRateOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    soundSampleRate = static_cast<int>(sampleRateOop.asSmallInteger());
    soundOutputRunning = true;

    popN(3);
    // Return success (true)
    push(memory_.trueObject());
    return PrimitiveResult::Success;
}

// Primitive 301: Start sound with semaphore notification
// sampleRate stereo semaIndex primitiveSoundStartWithSemaphore -> success
PrimitiveResult Interpreter::primitiveSoundStartWithSemaphore(int argCount) {
    if (argCount != 3) return PrimitiveResult::Failure;

    Oop semaIndexOop = stackTop();
    Oop stereoOop = stackValue(1);
    Oop sampleRateOop = stackValue(2);

    if (!sampleRateOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    soundSampleRate = static_cast<int>(sampleRateOop.asSmallInteger());
    soundOutputRunning = true;

    // Would register semaphore for buffer-ready notifications
    popN(3);
    push(memory_.trueObject());
    return PrimitiveResult::Success;
}

// Primitive 302: Stop sound output
// primitiveSoundStop -> self
PrimitiveResult Interpreter::primitiveSoundStop(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    soundOutputRunning = false;
    return PrimitiveResult::Success;
}

// Primitive 303: Get available buffer space for samples
// primitiveSoundAvailableSpace -> byteCount
PrimitiveResult Interpreter::primitiveSoundAvailableSpace(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Return a reasonable buffer size (8KB worth of samples)
    // In a real implementation, this would query the audio buffer
    intptr_t availableBytes = soundOutputRunning ? 8192 : 0;

    pop();
    push(Oop::fromSmallInteger(availableBytes));
    return PrimitiveResult::Success;
}

// Primitive 304: Play sound samples from buffer
// buffer startIndex count primitiveSoundPlaySamples -> samplesPlayed
PrimitiveResult Interpreter::primitiveSoundPlaySamples(int argCount) {
    if (argCount != 3) return PrimitiveResult::Failure;

    Oop countOop = stackTop();
    Oop startIndexOop = stackValue(1);
    Oop bufferOop = stackValue(2);

    if (!countOop.isSmallInteger() || !startIndexOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }
    if (!bufferOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    intptr_t count = countOop.asSmallInteger();

    // In a real implementation, samples would be queued to audio output
    // For now, pretend we played them all
    popN(3);
    pop();
    push(Oop::fromSmallInteger(soundOutputRunning ? count : 0));
    return PrimitiveResult::Success;
}

// Primitive 305: Play silence (used for timing/padding)
// sampleCount primitiveSoundPlaySilence -> samplesPlayed
PrimitiveResult Interpreter::primitiveSoundPlaySilence(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop countOop = stackTop();
    if (!countOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    intptr_t count = countOop.asSmallInteger();

    pop();
    pop();
    push(Oop::fromSmallInteger(soundOutputRunning ? count : 0));
    return PrimitiveResult::Success;
}

// Primitive 306: Get current volume
// primitiveSoundGetVolume -> volume (0-100) or array of left/right
PrimitiveResult Interpreter::primitiveSoundGetVolume(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    pop();
    push(Oop::fromSmallInteger(soundVolume));
    return PrimitiveResult::Success;
}

// Primitive 307: Set volume
// volume primitiveSoundSetVolume -> self
PrimitiveResult Interpreter::primitiveSoundSetVolume(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop volumeOop = stackTop();
    if (!volumeOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    intptr_t vol = volumeOop.asSmallInteger();
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    soundVolume = static_cast<int>(vol);

    pop();  // volume arg
    return PrimitiveResult::Success;
}

// Primitive 308: Set stereo balance
// balance primitiveSoundSetStereoBalance -> self
PrimitiveResult Interpreter::primitiveSoundSetStereoBalance(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop balanceOop = stackTop();
    if (!balanceOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    intptr_t bal = balanceOop.asSmallInteger();
    if (bal < 0) bal = 0;
    if (bal > 100) bal = 100;
    soundBalance = static_cast<int>(bal);

    pop();
    return PrimitiveResult::Success;
}

// Primitive 309: Get sample rate
// primitiveSoundGetSampleRate -> sampleRate
PrimitiveResult Interpreter::primitiveSoundGetSampleRate(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    pop();
    push(Oop::fromSmallInteger(soundSampleRate));
    return PrimitiveResult::Success;
}

// Primitive 310: Set sample rate
// sampleRate primitiveSoundSetSampleRate -> success
PrimitiveResult Interpreter::primitiveSoundSetSampleRate(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop rateOop = stackTop();
    if (!rateOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    intptr_t rate = rateOop.asSmallInteger();
    if (rate > 0 && rate <= 192000) {
        soundSampleRate = static_cast<int>(rate);
    }

    pop();
    push(memory_.trueObject());
    return PrimitiveResult::Success;
}

// Primitive 311: Start sound recording
// sampleRate stereo semaIndex primitiveSoundRecordStart -> success
PrimitiveResult Interpreter::primitiveSoundRecordStart(int argCount) {
    if (argCount != 3) return PrimitiveResult::Failure;

    // Recording requires microphone permission on iOS
    // For now, indicate recording started (but no actual recording)
    soundInputRunning = true;

    popN(3);
    push(memory_.trueObject());
    return PrimitiveResult::Success;
}

// Primitive 312: Stop sound recording
// primitiveSoundRecordStop -> self
PrimitiveResult Interpreter::primitiveSoundRecordStop(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    soundInputRunning = false;
    return PrimitiveResult::Success;
}

// Primitive 313: Read recorded samples into buffer
// buffer primitiveSoundRecordSamplesInto -> sampleCount
PrimitiveResult Interpreter::primitiveSoundRecordSamplesInto(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop bufferOop = stackTop();
    if (!bufferOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    // No actual recording - return 0 samples
    pop();
    pop();
    push(Oop::fromSmallInteger(0));
    return PrimitiveResult::Success;
}

// Primitive 314: Get recording level (for level meters)
// primitiveSoundGetRecordLevel -> level (0-100)
PrimitiveResult Interpreter::primitiveSoundGetRecordLevel(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Return 0 since no actual recording
    pop();
    push(Oop::fromSmallInteger(0));
    return PrimitiveResult::Success;
}

// Primitive 315: Set recording level/gain
// level primitiveSoundSetRecordLevel -> self
PrimitiveResult Interpreter::primitiveSoundSetRecordLevel(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    // Accept but ignore - no actual recording
    pop();
    return PrimitiveResult::Success;
}

// Primitive 316: Get available recorded samples count
// primitiveSoundRecordSamplesAvailable -> sampleCount
PrimitiveResult Interpreter::primitiveSoundRecordSamplesAvailable(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // No actual recording - return 0
    pop();
    push(Oop::fromSmallInteger(0));
    return PrimitiveResult::Success;
}

// Primitive 317: Get codec status/capabilities
// primitiveSoundCodecStatus -> statusCode
PrimitiveResult Interpreter::primitiveSoundCodecStatus(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Return 0 = no hardware codec
    pop();
    push(Oop::fromSmallInteger(0));
    return PrimitiveResult::Success;
}

// Primitive 318: Start sound mixer
// channelCount primitiveSoundMixerStart -> success
PrimitiveResult Interpreter::primitiveSoundMixerStart(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    // Mixer would allow multiple simultaneous sounds
    pop();
    push(memory_.trueObject());
    return PrimitiveResult::Success;
}

// Primitive 319: Stop sound mixer
// primitiveSoundMixerStop -> self
PrimitiveResult Interpreter::primitiveSoundMixerStop(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    return PrimitiveResult::Success;
}

// Primitive 320: Play on mixer channel
// channel buffer sampleCount primitiveSoundMixerPlayChannel -> success
PrimitiveResult Interpreter::primitiveSoundMixerPlayChannel(int argCount) {
    if (argCount != 3) return PrimitiveResult::Failure;

    popN(3);
    push(memory_.trueObject());
    return PrimitiveResult::Success;
}

// Primitive 321: Set mixer channel volume
// channel volume primitiveSoundMixerSetVolume -> self
PrimitiveResult Interpreter::primitiveSoundMixerSetVolume(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    popN(2);
    return PrimitiveResult::Success;
}

// Primitive 322: Set mixer channel pan
// channel pan primitiveSoundMixerSetPan -> self
PrimitiveResult Interpreter::primitiveSoundMixerSetPan(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    popN(2);
    return PrimitiveResult::Success;
}

// Primitive 323: Stop mixer channel
// channel primitiveSoundMixerStopChannel -> self
PrimitiveResult Interpreter::primitiveSoundMixerStopChannel(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    pop();
    return PrimitiveResult::Success;
}

// Primitive 324: Check if mixer channel is done playing
// channel primitiveSoundMixerChannelDone -> boolean
PrimitiveResult Interpreter::primitiveSoundMixerChannelDone(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    // Always report done since no actual playback
    pop();
    pop();
    push(memory_.trueObject());
    return PrimitiveResult::Success;
}

// Primitive 325: Get mixer channel playback position
// channel primitiveSoundMixerChannelPosition -> samplePosition
PrimitiveResult Interpreter::primitiveSoundMixerChannelPosition(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    pop();
    pop();
    push(Oop::fromSmallInteger(0));
    return PrimitiveResult::Success;
}

// Primitive 326: Insert samples at position (for streaming)
// buffer startIndex count primitiveSoundInsertSamples -> samplesInserted
PrimitiveResult Interpreter::primitiveSoundInsertSamples(int argCount) {
    if (argCount != 3) return PrimitiveResult::Failure;

    Oop countOop = stackTop();
    intptr_t count = countOop.isSmallInteger() ? countOop.asSmallInteger() : 0;

    popN(3);
    pop();
    push(Oop::fromSmallInteger(count));
    return PrimitiveResult::Success;
}

// Primitive 327: Start buffered sound output
// bufferSize sampleRate stereo semaIndex primitiveSoundStartBuffered -> success
PrimitiveResult Interpreter::primitiveSoundStartBuffered(int argCount) {
    if (argCount != 4) return PrimitiveResult::Failure;

    Oop sampleRateOop = stackValue(2);
    if (sampleRateOop.isSmallInteger()) {
        soundSampleRate = static_cast<int>(sampleRateOop.asSmallInteger());
    }
    soundOutputRunning = true;

    popN(4);
    push(memory_.trueObject());
    return PrimitiveResult::Success;
}

// Primitive 328: Enable/disable Acoustic Echo Cancellation
// enable primitiveSoundEnableAEC -> success
PrimitiveResult Interpreter::primitiveSoundEnableAEC(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    // AEC requires platform audio session configuration
    pop();
    push(memory_.trueObject());
    return PrimitiveResult::Success;
}

// Primitive 329: Check if AEC is supported
// primitiveSoundSupportsAEC -> boolean
PrimitiveResult Interpreter::primitiveSoundSupportsAEC(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // iOS does support AEC through AVAudioSession
    pop();
    push(memory_.trueObject());
    return PrimitiveResult::Success;
}

// ===== MIDI PRIMITIVES (330-349) =====

// MIDI state
static bool midiInitialized = false;
static int midiPortCount = 0;

// Primitive 330: Get MIDI port count
// primitiveMIDIGetPortCount -> count
PrimitiveResult Interpreter::primitiveMIDIGetPortCount(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // On iOS, would use CoreMIDI to enumerate ports
    // For now, return 0 (no MIDI ports available)
    pop();
    push(Oop::fromSmallInteger(midiPortCount));
    return PrimitiveResult::Success;
}

// Primitive 331: Get MIDI port name
// portIndex primitiveMIDIGetPortName -> string
PrimitiveResult Interpreter::primitiveMIDIGetPortName(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;

    // No ports available, so any index is invalid
    return PrimitiveResult::Failure;
}

// Primitive 332: Open MIDI port
// portIndex primitiveMIDIOpenPort -> handle
PrimitiveResult Interpreter::primitiveMIDIOpenPort(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;

    // No ports available
    return PrimitiveResult::Failure;
}

// Primitive 333: Close MIDI port
// handle primitiveMIDIClosePort -> self
PrimitiveResult Interpreter::primitiveMIDIClosePort(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop handleOop = stackTop();
    if (!handleOop.isSmallInteger()) return PrimitiveResult::Failure;

    // Would close the MIDI port
    popN(1);
    return PrimitiveResult::Success;
}

// Primitive 334: Read MIDI data
// handle buffer primitiveMIDIRead -> bytesRead
PrimitiveResult Interpreter::primitiveMIDIRead(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    // No data available from non-existent ports
    popN(2);
    push(Oop::fromSmallInteger(0));
    return PrimitiveResult::Success;
}

// Primitive 335: Write MIDI data
// handle buffer primitiveMIDIWrite -> bytesWritten
PrimitiveResult Interpreter::primitiveMIDIWrite(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop bufferOop = stackTop();
    Oop handleOop = stackValue(1);

    if (handleOop.isImmediate() || bufferOop.isImmediate()) {
        return PrimitiveResult::Failure;
    }

    size_t byteCount = memory_.byteSizeOf(bufferOop);

    // Would write MIDI data to the port
    popN(2);
    push(Oop::fromSmallInteger(static_cast<int64_t>(byteCount)));
    return PrimitiveResult::Success;
}

// Primitive 336: Get MIDI clock
// primitiveMIDIGetClock -> microseconds
PrimitiveResult Interpreter::primitiveMIDIGetClock(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Return system time in microseconds
    auto now = std::chrono::steady_clock::now();
    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();

    pop();
    // Return lower 31 bits as SmallInteger
    push(Oop::fromSmallInteger(micros & 0x7FFFFFFF));
    return PrimitiveResult::Success;
}

// Primitive 337: Set MIDI clock
// microseconds primitiveMIDISetClock -> self
PrimitiveResult Interpreter::primitiveMIDISetClock(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    // Cannot set system MIDI clock
    popN(1);
    return PrimitiveResult::Success;
}

// Primitive 338: Get MIDI parameter
// paramID primitiveMIDIParameterGet -> value
PrimitiveResult Interpreter::primitiveMIDIParameterGet(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop paramOop = stackTop();
    if (!paramOop.isSmallInteger()) return PrimitiveResult::Failure;

    // Return 0 for all parameters
    popN(1);
    push(Oop::fromSmallInteger(0));
    return PrimitiveResult::Success;
}

// Primitive 339: Set MIDI parameter
// paramID value primitiveMIDIParameterSet -> self
PrimitiveResult Interpreter::primitiveMIDIParameterSet(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    // Ignore parameter setting
    popN(2);
    return PrimitiveResult::Success;
}

// Primitive 340: Get MIDI driver version
// primitiveMIDIDriverVersion -> version
PrimitiveResult Interpreter::primitiveMIDIDriverVersion(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Return version 1
    pop();
    push(Oop::fromSmallInteger(1));
    return PrimitiveResult::Success;
}

// Primitive 341: Get MIDI port type
// portIndex primitiveMIDIPortType -> type
PrimitiveResult Interpreter::primitiveMIDIPortType(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    // No ports available
    return PrimitiveResult::Failure;
}

// Primitive 342: Get MIDI device ID
// portIndex primitiveMIDIDeviceID -> deviceID
PrimitiveResult Interpreter::primitiveMIDIDeviceID(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    // No ports available
    return PrimitiveResult::Failure;
}

// Primitive 343: Flush MIDI port
// handle primitiveMIDIFlushPort -> self
PrimitiveResult Interpreter::primitiveMIDIFlushPort(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    popN(1);
    return PrimitiveResult::Success;
}

// Primitive 344: Send MIDI Note On
// handle channel note velocity primitiveMIDISendNoteOn -> self
PrimitiveResult Interpreter::primitiveMIDISendNoteOn(int argCount) {
    if (argCount != 4) return PrimitiveResult::Failure;

    Oop velocityOop = stackTop();
    Oop noteOop = stackValue(1);
    Oop channelOop = stackValue(2);
    Oop handleOop = stackValue(3);

    if (!velocityOop.isSmallInteger() || !noteOop.isSmallInteger() ||
        !channelOop.isSmallInteger() || !handleOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    // Would send: 0x90 | channel, note, velocity
    popN(4);
    return PrimitiveResult::Success;
}

// Primitive 345: Send MIDI Note Off
// handle channel note velocity primitiveMIDISendNoteOff -> self
PrimitiveResult Interpreter::primitiveMIDISendNoteOff(int argCount) {
    if (argCount != 4) return PrimitiveResult::Failure;

    Oop velocityOop = stackTop();
    Oop noteOop = stackValue(1);
    Oop channelOop = stackValue(2);
    Oop handleOop = stackValue(3);

    if (!velocityOop.isSmallInteger() || !noteOop.isSmallInteger() ||
        !channelOop.isSmallInteger() || !handleOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    // Would send: 0x80 | channel, note, velocity
    popN(4);
    return PrimitiveResult::Success;
}

// Primitive 346: Send MIDI Controller
// handle channel controller value primitiveMIDISendController -> self
PrimitiveResult Interpreter::primitiveMIDISendController(int argCount) {
    if (argCount != 4) return PrimitiveResult::Failure;

    Oop valueOop = stackTop();
    Oop controllerOop = stackValue(1);
    Oop channelOop = stackValue(2);
    Oop handleOop = stackValue(3);

    if (!valueOop.isSmallInteger() || !controllerOop.isSmallInteger() ||
        !channelOop.isSmallInteger() || !handleOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    // Would send: 0xB0 | channel, controller, value
    popN(4);
    return PrimitiveResult::Success;
}

// Primitive 347: Send MIDI Program Change
// handle channel program primitiveMIDISendProgramChange -> self
PrimitiveResult Interpreter::primitiveMIDISendProgramChange(int argCount) {
    if (argCount != 3) return PrimitiveResult::Failure;

    Oop programOop = stackTop();
    Oop channelOop = stackValue(1);
    Oop handleOop = stackValue(2);

    if (!programOop.isSmallInteger() || !channelOop.isSmallInteger() ||
        !handleOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    // Would send: 0xC0 | channel, program
    popN(3);
    return PrimitiveResult::Success;
}

// Primitive 348: Send MIDI Pitch Bend
// handle channel value primitiveMIDISendPitchBend -> self
PrimitiveResult Interpreter::primitiveMIDISendPitchBend(int argCount) {
    if (argCount != 3) return PrimitiveResult::Failure;

    Oop valueOop = stackTop();
    Oop channelOop = stackValue(1);
    Oop handleOop = stackValue(2);

    if (!valueOop.isSmallInteger() || !channelOop.isSmallInteger() ||
        !handleOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    // Would send: 0xE0 | channel, lsb, msb (14-bit value)
    popN(3);
    return PrimitiveResult::Success;
}

// Primitive 349: Send MIDI System Exclusive
// handle buffer primitiveMIDISendSysEx -> self
PrimitiveResult Interpreter::primitiveMIDISendSysEx(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop bufferOop = stackTop();
    Oop handleOop = stackValue(1);

    if (handleOop.isImmediate() || bufferOop.isImmediate()) {
        return PrimitiveResult::Failure;
    }

    // Would send SysEx data (buffer should start with 0xF0 and end with 0xF7)
    popN(2);
    return PrimitiveResult::Success;
}

// ===== SERIAL PORT PRIMITIVES (270-279) =====

// Serial port state
static int serialPortCount = 0;

// Primitive 270: Get serial port count
// primitiveSerialPortCount -> count
PrimitiveResult Interpreter::primitiveSerialPortCount(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // iOS doesn't expose traditional serial ports
    // Would use External Accessory framework for MFi devices
    pop();
    push(Oop::fromSmallInteger(serialPortCount));
    return PrimitiveResult::Success;
}

// Primitive 271: Get serial port name
// portIndex primitiveSerialPortName -> string
PrimitiveResult Interpreter::primitiveSerialPortName(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;

    // No ports available
    return PrimitiveResult::Failure;
}

// Primitive 272: Open serial port
// portIndex baudRate dataBits stopBits parity primitiveSerialPortOpen -> handle
PrimitiveResult Interpreter::primitiveSerialPortOpen(int argCount) {
    if (argCount != 5) return PrimitiveResult::Failure;

    // No ports available on iOS
    return PrimitiveResult::Failure;
}

// Primitive 273: Close serial port
// handle primitiveSerialPortClose -> self
PrimitiveResult Interpreter::primitiveSerialPortClose(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    popN(1);
    return PrimitiveResult::Success;
}

// Primitive 274: Read from serial port
// handle buffer primitiveSerialPortRead -> bytesRead
PrimitiveResult Interpreter::primitiveSerialPortRead(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    // No data available
    popN(2);
    push(Oop::fromSmallInteger(0));
    return PrimitiveResult::Success;
}

// Primitive 275: Write to serial port
// handle buffer primitiveSerialPortWrite -> bytesWritten
PrimitiveResult Interpreter::primitiveSerialPortWrite(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop bufferOop = stackTop();
    if (bufferOop.isImmediate()) return PrimitiveResult::Failure;

    size_t byteCount = memory_.byteSizeOf(bufferOop);

    popN(2);
    push(Oop::fromSmallInteger(static_cast<int64_t>(byteCount)));
    return PrimitiveResult::Success;
}

// Primitive 276: Set serial port parameters
// handle baudRate dataBits stopBits parity primitiveSerialPortSetParams -> self
PrimitiveResult Interpreter::primitiveSerialPortSetParams(int argCount) {
    if (argCount != 5) return PrimitiveResult::Failure;

    popN(5);
    return PrimitiveResult::Success;
}

// Primitive 277: Get serial port parameters
// handle primitiveSerialPortGetParams -> array (baudRate dataBits stopBits parity)
PrimitiveResult Interpreter::primitiveSerialPortGetParams(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    // No ports available
    return PrimitiveResult::Failure;
}

// Primitive 278: Check if data available on serial port
// handle primitiveSerialPortDataAvailable -> bytesAvailable
PrimitiveResult Interpreter::primitiveSerialPortDataAvailable(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    popN(1);
    push(Oop::fromSmallInteger(0));
    return PrimitiveResult::Success;
}

// Primitive 279: Flush serial port buffers
// handle primitiveSerialPortFlush -> self
PrimitiveResult Interpreter::primitiveSerialPortFlush(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    popN(1);
    return PrimitiveResult::Success;
}

// ===== JOYSTICK PRIMITIVES (280-289) =====

// Joystick state
static int joystickCount = 0;

// Primitive 280: Get joystick count
// primitiveJoystickCount -> count
PrimitiveResult Interpreter::primitiveJoystickCount(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // iOS supports game controllers via GameController framework
    // For now, return 0
    pop();
    push(Oop::fromSmallInteger(joystickCount));
    return PrimitiveResult::Success;
}

// Primitive 281: Get joystick name
// joystickIndex primitiveJoystickName -> string
PrimitiveResult Interpreter::primitiveJoystickName(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;

    // No joysticks available
    return PrimitiveResult::Failure;
}

// Primitive 282: Open joystick
// joystickIndex primitiveJoystickOpen -> handle
PrimitiveResult Interpreter::primitiveJoystickOpen(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    // No joysticks available
    return PrimitiveResult::Failure;
}

// Primitive 283: Close joystick
// handle primitiveJoystickClose -> self
PrimitiveResult Interpreter::primitiveJoystickClose(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    popN(1);
    return PrimitiveResult::Success;
}

// Primitive 284: Read joystick state (all axes and buttons)
// handle primitiveJoystickRead -> array
PrimitiveResult Interpreter::primitiveJoystickRead(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    // No joysticks available
    return PrimitiveResult::Failure;
}

// Primitive 285: Get joystick button count
// handle primitiveJoystickButtonCount -> count
PrimitiveResult Interpreter::primitiveJoystickButtonCount(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    popN(1);
    push(Oop::fromSmallInteger(0));
    return PrimitiveResult::Success;
}

// Primitive 286: Get joystick axis count
// handle primitiveJoystickAxisCount -> count
PrimitiveResult Interpreter::primitiveJoystickAxisCount(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    popN(1);
    push(Oop::fromSmallInteger(0));
    return PrimitiveResult::Success;
}

// Primitive 287: Get joystick button state
// handle buttonIndex primitiveJoystickButtonState -> boolean
PrimitiveResult Interpreter::primitiveJoystickButtonState(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    popN(2);
    push(memory_.falseObject());
    return PrimitiveResult::Success;
}

// Primitive 288: Get joystick axis value
// handle axisIndex primitiveJoystickAxisValue -> value (-1.0 to 1.0 as SmallInteger -32768 to 32767)
PrimitiveResult Interpreter::primitiveJoystickAxisValue(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    popN(2);
    push(Oop::fromSmallInteger(0));  // Center position
    return PrimitiveResult::Success;
}

// Primitive 289: Get joystick hat/POV value
// handle hatIndex primitiveJoystickHatValue -> value (direction in degrees, -1 for center)
PrimitiveResult Interpreter::primitiveJoystickHatValue(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    popN(2);
    push(Oop::fromSmallInteger(-1));  // Center/neutral position
    return PrimitiveResult::Success;
}

// ===== SOCKET PRIMITIVES (350-359) =====

// Socket status values
enum SocketStatus {
    SocketInvalid = -1,
    SocketUnconnected = 0,
    SocketWaitingForConnection = 1,
    SocketConnected = 2,
    SocketOtherEndClosed = 3,
    SocketThisEndClosed = 4
};

// Primitive 350: Create a socket
// type primitiveSocketCreate -> handle
PrimitiveResult Interpreter::primitiveSocketCreate(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop typeOop = stackTop();
    if (!typeOop.isSmallInteger()) return PrimitiveResult::Failure;

    // Would create a socket using BSD sockets
    // For now, return failure (not implemented)
    return PrimitiveResult::Failure;
}

// Primitive 351: Destroy a socket
// handle primitiveSocketDestroy -> self
PrimitiveResult Interpreter::primitiveSocketDestroy(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    popN(1);
    return PrimitiveResult::Success;
}

// Primitive 352: Connect socket to address
// handle address port primitiveSocketConnect -> self
PrimitiveResult Interpreter::primitiveSocketConnect(int argCount) {
    if (argCount != 3) return PrimitiveResult::Failure;

    // Would initiate connection
    return PrimitiveResult::Failure;
}

// Primitive 353: Listen on socket
// handle backlog primitiveSocketListen -> self
PrimitiveResult Interpreter::primitiveSocketListen(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    return PrimitiveResult::Failure;
}

// Primitive 354: Accept connection on socket
// handle primitiveSocketAccept -> newHandle
PrimitiveResult Interpreter::primitiveSocketAccept(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    return PrimitiveResult::Failure;
}

// Primitive 355: Send data on socket
// handle buffer start count primitiveSocketSend -> bytesSent
PrimitiveResult Interpreter::primitiveSocketSend(int argCount) {
    if (argCount != 4) return PrimitiveResult::Failure;

    // Would send data
    popN(4);
    push(Oop::fromSmallInteger(0));
    return PrimitiveResult::Success;
}

// Primitive 356: Receive data from socket
// handle buffer start count primitiveSocketReceive -> bytesReceived
PrimitiveResult Interpreter::primitiveSocketReceive(int argCount) {
    if (argCount != 4) return PrimitiveResult::Failure;

    // Would receive data
    popN(4);
    push(Oop::fromSmallInteger(0));
    return PrimitiveResult::Success;
}

// Primitive 357: Get socket status
// handle primitiveSocketStatus -> status
PrimitiveResult Interpreter::primitiveSocketStatus(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    popN(1);
    push(Oop::fromSmallInteger(SocketInvalid));
    return PrimitiveResult::Success;
}

// Primitive 358: Get socket error
// handle primitiveSocketError -> errorCode
PrimitiveResult Interpreter::primitiveSocketError(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    popN(1);
    push(Oop::fromSmallInteger(0));  // No error
    return PrimitiveResult::Success;
}

// Primitive 359: Get socket local address
// handle primitiveSocketLocalAddress -> byteArray
PrimitiveResult Interpreter::primitiveSocketLocalAddress(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    return PrimitiveResult::Failure;
}

// ===== CLIPBOARD/DRAG-DROP PRIMITIVES (360-369) =====

// Primitive 360: primitiveClipboardText is reused from primitive 141

// Primitive 361: Store text to clipboard
// string primitiveClipboardTextStore -> self
PrimitiveResult Interpreter::primitiveClipboardTextStore(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop stringOop = stackTop();
    if (stringOop.isImmediate()) return PrimitiveResult::Failure;

    // Would store to UIPasteboard
    popN(1);
    return PrimitiveResult::Success;
}

// Primitive 362: Check if clipboard has text
// primitiveClipboardHasText -> boolean
PrimitiveResult Interpreter::primitiveClipboardHasText(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    pop();
    push(memory_.falseObject());
    return PrimitiveResult::Success;
}

// Primitive 363: Clear clipboard
// primitiveClipboardClear -> self
PrimitiveResult Interpreter::primitiveClipboardClear(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    pop();
    return PrimitiveResult::Success;
}

// Primitive 364: Get drag-drop file count
// primitiveDragDropFileCount -> count
PrimitiveResult Interpreter::primitiveDragDropFileCount(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    pop();
    push(Oop::fromSmallInteger(0));
    return PrimitiveResult::Success;
}

// Primitive 365: Get drag-drop file name
// index primitiveDragDropFileName -> string
PrimitiveResult Interpreter::primitiveDragDropFileName(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    return PrimitiveResult::Failure;
}

// Primitive 366: Request drag-drop file
// index primitiveDragDropRequestFile -> self
PrimitiveResult Interpreter::primitiveDragDropRequestFile(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    popN(1);
    return PrimitiveResult::Success;
}

// Primitive 367: Cancel drag-drop
// primitiveDragDropCancel -> self
PrimitiveResult Interpreter::primitiveDragDropCancel(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    pop();
    return PrimitiveResult::Success;
}

// Primitive 368: Get clipboard formats
// primitiveClipboardFormats -> array
PrimitiveResult Interpreter::primitiveClipboardFormats(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Return empty array
    Oop emptyArray = memory_.allocateSlots(
        memory_.indexOfClass(memory_.specialObject(SpecialObjectIndex::ClassArray)),
        0, ObjectFormat::Indexable);
    if (emptyArray.isNil()) return PrimitiveResult::Failure;

    pop();
    push(emptyArray);
    return PrimitiveResult::Success;
}

// Primitive 369: Get clipboard data for format
// format primitiveClipboardDataForFormat -> data
PrimitiveResult Interpreter::primitiveClipboardDataForFormat(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    return PrimitiveResult::Failure;
}

// ===== MISC PLUGIN PRIMITIVES (370-379) =====

// Primitive 370: Generate UUID
// byteArray primitiveUUIDGenerate -> self (fills byteArray with 16 random bytes)
PrimitiveResult Interpreter::primitiveUUIDGenerate(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop bufferOop = stackTop();
    if (bufferOop.isImmediate()) return PrimitiveResult::Failure;

    size_t size = memory_.byteSizeOf(bufferOop);
    if (size < 16) return PrimitiveResult::Failure;

    // Generate 16 random bytes for UUID
    for (size_t i = 0; i < 16; i++) {
        memory_.storeByte(i, bufferOop, static_cast<uint8_t>(rand() & 0xFF));
    }

    // Set version (4) and variant bits
    uint8_t byte6 = memory_.fetchByte(6, bufferOop);
    memory_.storeByte(6, bufferOop, (byte6 & 0x0F) | 0x40);  // Version 4
    uint8_t byte8 = memory_.fetchByte(8, bufferOop);
    memory_.storeByte(8, bufferOop, (byte8 & 0x3F) | 0x80);  // Variant 1

    popN(1);
    return PrimitiveResult::Success;
}

// Primitive 371: Parse UUID from string
// string byteArray primitiveUUIDParse -> boolean
PrimitiveResult Interpreter::primitiveUUIDParse(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop bufferOop = stackTop();
    Oop stringOop = stackValue(1);

    if (stringOop.isImmediate() || bufferOop.isImmediate()) {
        return PrimitiveResult::Failure;
    }

    std::string uuidStr = extractString(memory_, stringOop);
    if (uuidStr.length() < 32) {
        popN(2);
        push(memory_.falseObject());
        return PrimitiveResult::Success;
    }

    // Simple UUID parsing (accepts with or without dashes)
    size_t byteIndex = 0;
    size_t strIndex = 0;
    while (byteIndex < 16 && strIndex < uuidStr.length()) {
        if (uuidStr[strIndex] == '-') {
            strIndex++;
            continue;
        }
        if (strIndex + 1 >= uuidStr.length()) break;

        char hex[3] = { uuidStr[strIndex], uuidStr[strIndex + 1], 0 };
        uint8_t byte = static_cast<uint8_t>(strtol(hex, nullptr, 16));
        memory_.storeByte(byteIndex, bufferOop, byte);
        byteIndex++;
        strIndex += 2;
    }

    popN(2);
    push(byteIndex == 16 ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

// Primitive 372: Convert UUID to string
// byteArray primitiveUUIDToString -> string
PrimitiveResult Interpreter::primitiveUUIDToString(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop bufferOop = stackTop();
    if (bufferOop.isImmediate()) return PrimitiveResult::Failure;

    size_t size = memory_.byteSizeOf(bufferOop);
    if (size < 16) return PrimitiveResult::Failure;

    // Format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
    char uuidStr[37];
    int pos = 0;
    for (size_t i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            uuidStr[pos++] = '-';
        }
        uint8_t byte = memory_.fetchByte(i, bufferOop);
        snprintf(&uuidStr[pos], 3, "%02x", byte);
        pos += 2;
    }
    uuidStr[36] = '\0';

    Oop result = createStringObject(memory_, uuidStr);
    if (result.isNil()) return PrimitiveResult::Failure;

    popN(1);
    push(result);
    return PrimitiveResult::Success;
}

// Primitive 373: Create SSL context
// primitiveSSLCreate -> handle
PrimitiveResult Interpreter::primitiveSSLCreate(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Would use Security framework on iOS
    return PrimitiveResult::Failure;
}

// Primitive 374: Destroy SSL context
// handle primitiveSSLDestroy -> self
PrimitiveResult Interpreter::primitiveSSLDestroy(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    popN(1);
    return PrimitiveResult::Success;
}

// Primitive 375: SSL connect (handshake on existing socket)
// handle socketHandle primitiveSSLConnect -> status
PrimitiveResult Interpreter::primitiveSSLConnect(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    return PrimitiveResult::Failure;
}

// Primitive 376: SSL accept (server handshake)
// handle socketHandle primitiveSSLAccept -> status
PrimitiveResult Interpreter::primitiveSSLAccept(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    return PrimitiveResult::Failure;
}

// Primitive 377: SSL send
// handle buffer start count primitiveSSLSend -> bytesSent
PrimitiveResult Interpreter::primitiveSSLSend(int argCount) {
    if (argCount != 4) return PrimitiveResult::Failure;

    popN(4);
    push(Oop::fromSmallInteger(0));
    return PrimitiveResult::Success;
}

// Primitive 378: SSL receive
// handle buffer start count primitiveSSLReceive -> bytesReceived
PrimitiveResult Interpreter::primitiveSSLReceive(int argCount) {
    if (argCount != 4) return PrimitiveResult::Failure;

    popN(4);
    push(Oop::fromSmallInteger(0));
    return PrimitiveResult::Success;
}

// Primitive 379: SSL status
// handle primitiveSSLStatus -> status
PrimitiveResult Interpreter::primitiveSSLStatus(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    popN(1);
    push(Oop::fromSmallInteger(0));  // Not connected
    return PrimitiveResult::Success;
}

// ===== SSL EXTENDED PRIMITIVES (380-389) =====

// Primitive 380: Set SSL certificate
// handle certData primitiveSSLSetCertificate -> status
PrimitiveResult Interpreter::primitiveSSLSetCertificate(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    popN(2);
    push(Oop::fromSmallInteger(0));  // Success
    return PrimitiveResult::Success;
}

// Primitive 381: Set SSL private key
// handle keyData primitiveSSLSetPrivateKey -> status
PrimitiveResult Interpreter::primitiveSSLSetPrivateKey(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    popN(2);
    push(Oop::fromSmallInteger(0));  // Success
    return PrimitiveResult::Success;
}

// Primitive 382: Get peer certificate
// handle primitiveSSLGetPeerCertificate -> byteArray
PrimitiveResult Interpreter::primitiveSSLGetPeerCertificate(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    // No certificate available
    return PrimitiveResult::Failure;
}

// Primitive 383: Get certificate name (subject/issuer)
// handle nameType primitiveSSLGetCertificateName -> string
PrimitiveResult Interpreter::primitiveSSLGetCertificateName(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    return PrimitiveResult::Failure;
}

// Primitive 384: Set SSL verify mode
// handle mode primitiveSSLSetVerifyMode -> self
PrimitiveResult Interpreter::primitiveSSLSetVerifyMode(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    popN(2);
    return PrimitiveResult::Success;
}

// Primitive 385: Get SSL verify result
// handle primitiveSSLGetVerifyResult -> resultCode
PrimitiveResult Interpreter::primitiveSSLGetVerifyResult(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    popN(1);
    push(Oop::fromSmallInteger(0));  // OK
    return PrimitiveResult::Success;
}

// Primitive 386: Set SNI (Server Name Indication)
// handle serverName primitiveSSLSetSNI -> self
PrimitiveResult Interpreter::primitiveSSLSetSNI(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    popN(2);
    return PrimitiveResult::Success;
}

// Primitive 387: Get SSL version
// handle primitiveSSLGetVersion -> string
PrimitiveResult Interpreter::primitiveSSLGetVersion(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop version = createStringObject(memory_, "TLS 1.2");
    if (version.isNil()) return PrimitiveResult::Failure;

    popN(1);
    push(version);
    return PrimitiveResult::Success;
}

// Primitive 388: Get SSL cipher
// handle primitiveSSLGetCipher -> string
PrimitiveResult Interpreter::primitiveSSLGetCipher(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    return PrimitiveResult::Failure;
}

// Primitive 389: Close SSL connection
// handle primitiveSSLClose -> self
PrimitiveResult Interpreter::primitiveSSLClose(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    popN(1);
    return PrimitiveResult::Success;
}

// ===== LOCALE PRIMITIVES (390-399) =====

// Primitive 390: Get system language
// primitiveLocaleLanguage -> string (e.g., "en", "fr", "de")
PrimitiveResult Interpreter::primitiveLocaleLanguage(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Would use NSLocale on iOS
    Oop lang = createStringObject(memory_, "en");
    if (lang.isNil()) return PrimitiveResult::Failure;

    pop();
    push(lang);
    return PrimitiveResult::Success;
}

// Primitive 391: Get system country
// primitiveLocaleCountry -> string (e.g., "US", "FR", "DE")
PrimitiveResult Interpreter::primitiveLocaleCountry(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop country = createStringObject(memory_, "US");
    if (country.isNil()) return PrimitiveResult::Failure;

    pop();
    push(country);
    return PrimitiveResult::Success;
}

// Primitive 392: Get currency symbol
// primitiveLocaleCurrencySymbol -> string
PrimitiveResult Interpreter::primitiveLocaleCurrencySymbol(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop symbol = createStringObject(memory_, "$");
    if (symbol.isNil()) return PrimitiveResult::Failure;

    pop();
    push(symbol);
    return PrimitiveResult::Success;
}

// Primitive 393: Get decimal separator
// primitiveLocaleDecimalSeparator -> string
PrimitiveResult Interpreter::primitiveLocaleDecimalSeparator(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop sep = createStringObject(memory_, ".");
    if (sep.isNil()) return PrimitiveResult::Failure;

    pop();
    push(sep);
    return PrimitiveResult::Success;
}

// Primitive 394: Get thousands separator
// primitiveLocaleThousandsSeparator -> string
PrimitiveResult Interpreter::primitiveLocaleThousandsSeparator(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop sep = createStringObject(memory_, ",");
    if (sep.isNil()) return PrimitiveResult::Failure;

    pop();
    push(sep);
    return PrimitiveResult::Success;
}

// Primitive 395: Get date format
// primitiveLocaleDateFormat -> string
PrimitiveResult Interpreter::primitiveLocaleDateFormat(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop fmt = createStringObject(memory_, "MM/dd/yyyy");
    if (fmt.isNil()) return PrimitiveResult::Failure;

    pop();
    push(fmt);
    return PrimitiveResult::Success;
}

// Primitive 396: Get time format
// primitiveLocaleTimeFormat -> string
PrimitiveResult Interpreter::primitiveLocaleTimeFormat(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop fmt = createStringObject(memory_, "HH:mm:ss");
    if (fmt.isNil()) return PrimitiveResult::Failure;

    pop();
    push(fmt);
    return PrimitiveResult::Success;
}

// Primitive 397: Get timezone name
// primitiveLocaleTimezone -> string
PrimitiveResult Interpreter::primitiveLocaleTimezone(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop tz = createStringObject(memory_, "UTC");
    if (tz.isNil()) return PrimitiveResult::Failure;

    pop();
    push(tz);
    return PrimitiveResult::Success;
}

// Primitive 398: Get timezone offset in seconds
// primitiveLocaleTimezoneOffset -> minutes offset from UTC
PrimitiveResult Interpreter::primitiveLocaleTimezoneOffset(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    time_t now = time(nullptr);
    struct tm* local = localtime(&now);
    int64_t offsetMinutes = local->tm_gmtoff / 60;

    pop();
    push(Oop::fromSmallInteger(offsetMinutes));
    return PrimitiveResult::Success;
}

// Primitive 399: Check if daylight saving is active
// primitiveLocaleDaylightSaving -> boolean
PrimitiveResult Interpreter::primitiveLocaleDaylightSaving(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    pop();
    push(memory_.falseObject());
    return PrimitiveResult::Success;
}

// ===== IMAGE/GRAPHICS PRIMITIVES (400-409) =====

// Primitive 400: Read image header (get dimensions, format)
// byteArray primitiveImageReadHeader -> array (width height depth format)
PrimitiveResult Interpreter::primitiveImageReadHeader(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    // Would parse PNG/JPEG header
    return PrimitiveResult::Failure;
}

// Primitive 401: Read image pixels into Form
// byteArray form primitiveImageReadPixels -> boolean
PrimitiveResult Interpreter::primitiveImageReadPixels(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    // Would decode image into Form bits
    return PrimitiveResult::Failure;
}

// Primitive 402: Write Form as PNG
// form primitiveImageWritePNG -> byteArray
PrimitiveResult Interpreter::primitiveImageWritePNG(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    // Would encode Form as PNG
    return PrimitiveResult::Failure;
}

// Primitive 403: Write Form as JPEG
// form quality primitiveImageWriteJPEG -> byteArray
PrimitiveResult Interpreter::primitiveImageWriteJPEG(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    // Would encode Form as JPEG
    return PrimitiveResult::Failure;
}

// Primitive 404: Scale image
// form newWidth newHeight primitiveImageScale -> newForm
PrimitiveResult Interpreter::primitiveImageScale(int argCount) {
    if (argCount != 3) return PrimitiveResult::Failure;

    // Would scale using Core Graphics
    return PrimitiveResult::Failure;
}

// Primitive 405: Rotate image
// form degrees primitiveImageRotate -> newForm
PrimitiveResult Interpreter::primitiveImageRotate(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    // Would rotate using Core Graphics
    return PrimitiveResult::Failure;
}

// Primitive 406: Composite images
// destForm srcForm x y rule primitiveImageComposite -> self
PrimitiveResult Interpreter::primitiveImageComposite(int argCount) {
    if (argCount != 5) return PrimitiveResult::Failure;

    // Would composite using Core Graphics
    return PrimitiveResult::Failure;
}

// Primitive 407: Convert color space
// form colorSpace primitiveImageColorConvert -> newForm
PrimitiveResult Interpreter::primitiveImageColorConvert(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    return PrimitiveResult::Failure;
}

// Primitive 408: Apply image filter
// form filterType params primitiveImageFilter -> newForm
PrimitiveResult Interpreter::primitiveImageFilter(int argCount) {
    if (argCount != 3) return PrimitiveResult::Failure;

    // Would use Core Image filters
    return PrimitiveResult::Failure;
}

// Primitive 409: Get image metadata (EXIF, etc.)
// byteArray primitiveImageGetMetadata -> dictionary
PrimitiveResult Interpreter::primitiveImageGetMetadata(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    return PrimitiveResult::Failure;
}

// ===== SYSTEM INFO PRIMITIVES (410-419) =====

// Primitive 410: Get battery level (0-100)
// primitiveSystemBatteryLevel -> percentage
PrimitiveResult Interpreter::primitiveSystemBatteryLevel(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Would use UIDevice.current.batteryLevel on iOS
    pop();
    push(Oop::fromSmallInteger(100));  // Fully charged
    return PrimitiveResult::Success;
}

// Primitive 411: Get battery state
// primitiveSystemBatteryState -> state (0=unknown, 1=unplugged, 2=charging, 3=full)
PrimitiveResult Interpreter::primitiveSystemBatteryState(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    pop();
    push(Oop::fromSmallInteger(3));  // Full
    return PrimitiveResult::Success;
}

// Primitive 412: Get screen brightness (0-100)
// primitiveSystemScreenBrightness -> percentage
PrimitiveResult Interpreter::primitiveSystemScreenBrightness(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Would use UIScreen.main.brightness
    pop();
    push(Oop::fromSmallInteger(80));
    return PrimitiveResult::Success;
}

// Primitive 413: Set screen brightness
// percentage primitiveSystemSetScreenBrightness -> self
PrimitiveResult Interpreter::primitiveSystemSetScreenBrightness(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    popN(1);
    return PrimitiveResult::Success;
}

// Primitive 414: Get device model
// primitiveSystemDeviceModel -> string
PrimitiveResult Interpreter::primitiveSystemDeviceModel(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop model = createStringObject(memory_, "iPhone");
    if (model.isNil()) return PrimitiveResult::Failure;

    pop();
    push(model);
    return PrimitiveResult::Success;
}

// Primitive 415: Get device UUID
// primitiveSystemDeviceUUID -> string
PrimitiveResult Interpreter::primitiveSystemDeviceUUID(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Would use UIDevice.current.identifierForVendor
    Oop uuid = createStringObject(memory_, "00000000-0000-0000-0000-000000000000");
    if (uuid.isNil()) return PrimitiveResult::Failure;

    pop();
    push(uuid);
    return PrimitiveResult::Success;
}

// Primitive 416: Get app version
// primitiveSystemAppVersion -> string
PrimitiveResult Interpreter::primitiveSystemAppVersion(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop version = createStringObject(memory_, "1.0.0");
    if (version.isNil()) return PrimitiveResult::Failure;

    pop();
    push(version);
    return PrimitiveResult::Success;
}

// Primitive 417: Get app build number
// primitiveSystemAppBuild -> string
PrimitiveResult Interpreter::primitiveSystemAppBuild(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop build = createStringObject(memory_, "1");
    if (build.isNil()) return PrimitiveResult::Failure;

    pop();
    push(build);
    return PrimitiveResult::Success;
}

// Primitive 418: Get available memory in bytes
// primitiveSystemAvailableMemory -> bytes
PrimitiveResult Interpreter::primitiveSystemAvailableMemory(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Return a reasonable default (256MB)
    pop();
    push(Oop::fromSmallInteger(256 * 1024 * 1024));
    return PrimitiveResult::Success;
}

// Primitive 419: Get available disk space in bytes
// primitiveSystemDiskSpace -> bytes
PrimitiveResult Interpreter::primitiveSystemDiskSpace(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Return a reasonable default (1GB)
    int64_t oneGB = 1024LL * 1024 * 1024;
    if (Oop::canBeSmallInteger(oneGB)) {
        pop();
        push(Oop::fromSmallInteger(oneGB));
        return PrimitiveResult::Success;
    }
    return PrimitiveResult::Failure;
}

// ===== HARDWARE/SENSOR PRIMITIVES (420-429) =====

// Sensor state
static bool accelerometerRunning = false;
static bool gyroscopeRunning = false;
static bool magnetometerRunning = false;

// Primitive 420: Start accelerometer
// interval primitiveAccelerometerStart -> self
PrimitiveResult Interpreter::primitiveAccelerometerStart(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    // Would use CMMotionManager on iOS
    accelerometerRunning = true;
    popN(1);
    return PrimitiveResult::Success;
}

// Primitive 421: Stop accelerometer
// primitiveAccelerometerStop -> self
PrimitiveResult Interpreter::primitiveAccelerometerStop(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    accelerometerRunning = false;
    pop();
    return PrimitiveResult::Success;
}

// Primitive 422: Read accelerometer
// primitiveAccelerometerRead -> array (x y z) or nil
PrimitiveResult Interpreter::primitiveAccelerometerRead(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    if (!accelerometerRunning) {
        pop();
        push(memory_.nil());
        return PrimitiveResult::Success;
    }

    // Return default values (no acceleration, device at rest)
    Oop array = memory_.allocateSlots(
        memory_.indexOfClass(memory_.specialObject(SpecialObjectIndex::ClassArray)),
        3, ObjectFormat::Indexable);
    if (array.isNil()) return PrimitiveResult::Failure;

    memory_.storePointer(0, array, Oop::fromSmallInteger(0));  // x
    memory_.storePointer(1, array, Oop::fromSmallInteger(0));  // y
    memory_.storePointer(2, array, Oop::fromSmallInteger(-1000)); // z (gravity, scaled)

    pop();
    push(array);
    return PrimitiveResult::Success;
}

// Primitive 423: Start gyroscope
// interval primitiveGyroscopeStart -> self
PrimitiveResult Interpreter::primitiveGyroscopeStart(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    gyroscopeRunning = true;
    popN(1);
    return PrimitiveResult::Success;
}

// Primitive 424: Stop gyroscope
// primitiveGyroscopeStop -> self
PrimitiveResult Interpreter::primitiveGyroscopeStop(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    gyroscopeRunning = false;
    pop();
    return PrimitiveResult::Success;
}

// Primitive 425: Read gyroscope
// primitiveGyroscopeRead -> array (x y z) or nil
PrimitiveResult Interpreter::primitiveGyroscopeRead(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    if (!gyroscopeRunning) {
        pop();
        push(memory_.nil());
        return PrimitiveResult::Success;
    }

    Oop array = memory_.allocateSlots(
        memory_.indexOfClass(memory_.specialObject(SpecialObjectIndex::ClassArray)),
        3, ObjectFormat::Indexable);
    if (array.isNil()) return PrimitiveResult::Failure;

    memory_.storePointer(0, array, Oop::fromSmallInteger(0));
    memory_.storePointer(1, array, Oop::fromSmallInteger(0));
    memory_.storePointer(2, array, Oop::fromSmallInteger(0));

    pop();
    push(array);
    return PrimitiveResult::Success;
}

// Primitive 426: Start magnetometer
// interval primitiveMagnetometerStart -> self
PrimitiveResult Interpreter::primitiveMagnetometerStart(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    magnetometerRunning = true;
    popN(1);
    return PrimitiveResult::Success;
}

// Primitive 427: Stop magnetometer
// primitiveMagnetometerStop -> self
PrimitiveResult Interpreter::primitiveMagnetometerStop(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    magnetometerRunning = false;
    pop();
    return PrimitiveResult::Success;
}

// Primitive 428: Read magnetometer
// primitiveMagnetometerRead -> array (x y z) or nil
PrimitiveResult Interpreter::primitiveMagnetometerRead(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    if (!magnetometerRunning) {
        pop();
        push(memory_.nil());
        return PrimitiveResult::Success;
    }

    Oop array = memory_.allocateSlots(
        memory_.indexOfClass(memory_.specialObject(SpecialObjectIndex::ClassArray)),
        3, ObjectFormat::Indexable);
    if (array.isNil()) return PrimitiveResult::Failure;

    memory_.storePointer(0, array, Oop::fromSmallInteger(0));
    memory_.storePointer(1, array, Oop::fromSmallInteger(0));
    memory_.storePointer(2, array, Oop::fromSmallInteger(0));

    pop();
    push(array);
    return PrimitiveResult::Success;
}

// Primitive 429: Read combined device motion
// primitiveDeviceMotionRead -> array or nil
PrimitiveResult Interpreter::primitiveDeviceMotionRead(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Would return combined sensor data
    pop();
    push(memory_.nil());
    return PrimitiveResult::Success;
}

// ===== LOCATION PRIMITIVES (430-439) =====

static bool locationRunning = false;
static bool headingRunning = false;

// Primitive 430: Start location updates
// accuracy primitiveLocationStart -> self
PrimitiveResult Interpreter::primitiveLocationStart(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    // Would use CLLocationManager on iOS
    locationRunning = true;
    popN(1);
    return PrimitiveResult::Success;
}

// Primitive 431: Stop location updates
// primitiveLocationStop -> self
PrimitiveResult Interpreter::primitiveLocationStop(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    locationRunning = false;
    pop();
    return PrimitiveResult::Success;
}

// Primitive 432: Read current location
// primitiveLocationRead -> array (lat lon alt accuracy) or nil
PrimitiveResult Interpreter::primitiveLocationRead(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    if (!locationRunning) {
        pop();
        push(memory_.nil());
        return PrimitiveResult::Success;
    }

    // Return a default location (0, 0)
    Oop array = memory_.allocateSlots(
        memory_.indexOfClass(memory_.specialObject(SpecialObjectIndex::ClassArray)),
        4, ObjectFormat::Indexable);
    if (array.isNil()) return PrimitiveResult::Failure;

    memory_.storePointer(0, array, Oop::fromSmallInteger(0));  // lat
    memory_.storePointer(1, array, Oop::fromSmallInteger(0));  // lon
    memory_.storePointer(2, array, Oop::fromSmallInteger(0));  // alt
    memory_.storePointer(3, array, Oop::fromSmallInteger(0));  // accuracy

    pop();
    push(array);
    return PrimitiveResult::Success;
}

// Primitive 433: Set location accuracy
// accuracy primitiveLocationAccuracy -> self
PrimitiveResult Interpreter::primitiveLocationAccuracy(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    popN(1);
    return PrimitiveResult::Success;
}

// Primitive 434: Calculate distance between two points
// lat1 lon1 lat2 lon2 primitiveLocationDistance -> meters
PrimitiveResult Interpreter::primitiveLocationDistance(int argCount) {
    if (argCount != 4) return PrimitiveResult::Failure;

    // Would calculate haversine distance
    popN(4);
    push(Oop::fromSmallInteger(0));
    return PrimitiveResult::Success;
}

// Primitive 435: Start heading updates
// primitiveHeadingStart -> self
PrimitiveResult Interpreter::primitiveHeadingStart(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    headingRunning = true;
    pop();
    return PrimitiveResult::Success;
}

// Primitive 436: Stop heading updates
// primitiveHeadingStop -> self
PrimitiveResult Interpreter::primitiveHeadingStop(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    headingRunning = false;
    pop();
    return PrimitiveResult::Success;
}

// Primitive 437: Read current heading
// primitiveHeadingRead -> degrees or nil
PrimitiveResult Interpreter::primitiveHeadingRead(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    if (!headingRunning) {
        pop();
        push(memory_.nil());
        return PrimitiveResult::Success;
    }

    pop();
    push(Oop::fromSmallInteger(0));  // North
    return PrimitiveResult::Success;
}

// Primitive 438: Geocode address to coordinates
// addressString primitiveGeocode -> array (lat lon) or nil
PrimitiveResult Interpreter::primitiveGeocode(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    // Would use CLGeocoder
    popN(1);
    push(memory_.nil());
    return PrimitiveResult::Success;
}

// Primitive 439: Reverse geocode coordinates to address
// lat lon primitiveReverseGeocode -> string or nil
PrimitiveResult Interpreter::primitiveReverseGeocode(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    // Would use CLGeocoder
    popN(2);
    push(memory_.nil());
    return PrimitiveResult::Success;
}

// ===== CAMERA PRIMITIVES (440-449) =====

static int cameraCount = 0;  // Would be 2 on most iOS devices

// Primitive 440: Get camera count
// primitiveCameraCount -> count
PrimitiveResult Interpreter::primitiveCameraCount(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Would use AVCaptureDevice.devices on iOS
    pop();
    push(Oop::fromSmallInteger(cameraCount));
    return PrimitiveResult::Success;
}

// Primitive 441: Open camera
// cameraIndex primitiveCameraOpen -> handle
PrimitiveResult Interpreter::primitiveCameraOpen(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    // No cameras available
    return PrimitiveResult::Failure;
}

// Primitive 442: Close camera
// handle primitiveCameraClose -> self
PrimitiveResult Interpreter::primitiveCameraClose(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    popN(1);
    return PrimitiveResult::Success;
}

// Primitive 443: Capture still image
// handle primitiveCameraCapture -> byteArray or nil
PrimitiveResult Interpreter::primitiveCameraCapture(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    return PrimitiveResult::Failure;
}

// Primitive 444: Start camera preview
// handle primitiveCameraStartPreview -> self
PrimitiveResult Interpreter::primitiveCameraStartPreview(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    return PrimitiveResult::Failure;
}

// Primitive 445: Stop camera preview
// handle primitiveCameraStopPreview -> self
PrimitiveResult Interpreter::primitiveCameraStopPreview(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    popN(1);
    return PrimitiveResult::Success;
}

// Primitive 446: Get preview frame
// handle form primitiveCameraGetFrame -> boolean
PrimitiveResult Interpreter::primitiveCameraGetFrame(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    popN(2);
    push(memory_.falseObject());
    return PrimitiveResult::Success;
}

// Primitive 447: Set flash mode
// handle mode primitiveCameraSetFlash -> self
PrimitiveResult Interpreter::primitiveCameraSetFlash(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    popN(2);
    return PrimitiveResult::Success;
}

// Primitive 448: Set focus point
// handle x y primitiveCameraSetFocus -> self
PrimitiveResult Interpreter::primitiveCameraSetFocus(int argCount) {
    if (argCount != 3) return PrimitiveResult::Failure;

    popN(3);
    return PrimitiveResult::Success;
}

// Primitive 449: Set exposure point
// handle x y primitiveCameraSetExposure -> self
PrimitiveResult Interpreter::primitiveCameraSetExposure(int argCount) {
    if (argCount != 3) return PrimitiveResult::Failure;

    popN(3);
    return PrimitiveResult::Success;
}

// ===== NOTIFICATION PRIMITIVES (450-459) =====

static int badgeCount = 0;

// Primitive 450: Schedule local notification
// title body delay primitiveNotificationSchedule -> id
PrimitiveResult Interpreter::primitiveNotificationSchedule(int argCount) {
    if (argCount != 3) return PrimitiveResult::Failure;

    // Would use UNUserNotificationCenter on iOS
    popN(3);
    push(Oop::fromSmallInteger(0));  // notification ID
    return PrimitiveResult::Success;
}

// Primitive 451: Cancel notification by ID
// id primitiveNotificationCancel -> self
PrimitiveResult Interpreter::primitiveNotificationCancel(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    popN(1);
    return PrimitiveResult::Success;
}

// Primitive 452: Cancel all notifications
// primitiveNotificationCancelAll -> self
PrimitiveResult Interpreter::primitiveNotificationCancelAll(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    pop();
    return PrimitiveResult::Success;
}

// Primitive 453: Get pending notifications
// primitiveNotificationGetPending -> array
PrimitiveResult Interpreter::primitiveNotificationGetPending(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop emptyArray = memory_.allocateSlots(
        memory_.indexOfClass(memory_.specialObject(SpecialObjectIndex::ClassArray)),
        0, ObjectFormat::Indexable);
    if (emptyArray.isNil()) return PrimitiveResult::Failure;

    pop();
    push(emptyArray);
    return PrimitiveResult::Success;
}

// Primitive 454: Request notification permission
// primitiveNotificationRequestPermission -> self
PrimitiveResult Interpreter::primitiveNotificationRequestPermission(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Would show permission dialog
    pop();
    return PrimitiveResult::Success;
}

// Primitive 455: Get notification permission status
// primitiveNotificationGetPermission -> status (0=notDetermined, 1=denied, 2=authorized)
PrimitiveResult Interpreter::primitiveNotificationGetPermission(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    pop();
    push(Oop::fromSmallInteger(2));  // Authorized
    return PrimitiveResult::Success;
}

// Primitive 456: Set app badge count
// count primitiveNotificationSetBadge -> self
PrimitiveResult Interpreter::primitiveNotificationSetBadge(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop countOop = stackTop();
    if (countOop.isSmallInteger()) {
        badgeCount = static_cast<int>(countOop.asSmallInteger());
    }

    popN(1);
    return PrimitiveResult::Success;
}

// Primitive 457: Get app badge count
// primitiveNotificationGetBadge -> count
PrimitiveResult Interpreter::primitiveNotificationGetBadge(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    pop();
    push(Oop::fromSmallInteger(badgeCount));
    return PrimitiveResult::Success;
}

// Primitive 458: Register for push notifications
// primitiveNotificationRegisterPush -> self
PrimitiveResult Interpreter::primitiveNotificationRegisterPush(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Would register with APNs
    pop();
    return PrimitiveResult::Success;
}

// Primitive 459: Get push notification token
// primitiveNotificationGetToken -> string or nil
PrimitiveResult Interpreter::primitiveNotificationGetToken(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Would return device token if registered
    pop();
    push(memory_.nil());
    return PrimitiveResult::Success;
}

// ===== IN-APP PURCHASE PRIMITIVES (460-469) =====

// Primitive 460: Check if device can make payments
// primitiveIAPCanMakePayments -> boolean
PrimitiveResult Interpreter::primitiveIAPCanMakePayments(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Would use SKPaymentQueue.canMakePayments()
    pop();
    push(memory_.trueObject());
    return PrimitiveResult::Success;
}

// Primitive 461: Request product info from App Store
// productIds primitiveIAPRequestProducts -> self
PrimitiveResult Interpreter::primitiveIAPRequestProducts(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    // Would use SKProductsRequest
    popN(1);
    return PrimitiveResult::Success;
}

// Primitive 462: Get loaded product info
// primitiveIAPGetProducts -> array of product info
PrimitiveResult Interpreter::primitiveIAPGetProducts(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop emptyArray = memory_.allocateSlots(
        memory_.indexOfClass(memory_.specialObject(SpecialObjectIndex::ClassArray)),
        0, ObjectFormat::Indexable);
    if (emptyArray.isNil()) return PrimitiveResult::Failure;

    pop();
    push(emptyArray);
    return PrimitiveResult::Success;
}

// Primitive 463: Initiate purchase
// productId primitiveIAPPurchase -> self
PrimitiveResult Interpreter::primitiveIAPPurchase(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    // Would add payment to queue
    popN(1);
    return PrimitiveResult::Success;
}

// Primitive 464: Restore previous purchases
// primitiveIAPRestore -> self
PrimitiveResult Interpreter::primitiveIAPRestore(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Would call restoreCompletedTransactions
    pop();
    return PrimitiveResult::Success;
}

// Primitive 465: Get pending transactions
// primitiveIAPGetTransactions -> array
PrimitiveResult Interpreter::primitiveIAPGetTransactions(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop emptyArray = memory_.allocateSlots(
        memory_.indexOfClass(memory_.specialObject(SpecialObjectIndex::ClassArray)),
        0, ObjectFormat::Indexable);
    if (emptyArray.isNil()) return PrimitiveResult::Failure;

    pop();
    push(emptyArray);
    return PrimitiveResult::Success;
}

// Primitive 466: Finish transaction
// transactionId primitiveIAPFinishTransaction -> self
PrimitiveResult Interpreter::primitiveIAPFinishTransaction(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    popN(1);
    return PrimitiveResult::Success;
}

// Primitive 467: Get app receipt data
// primitiveIAPGetReceipt -> byteArray or nil
PrimitiveResult Interpreter::primitiveIAPGetReceipt(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    pop();
    push(memory_.nil());
    return PrimitiveResult::Success;
}

// Primitive 468: Refresh receipt from App Store
// primitiveIAPRefreshReceipt -> self
PrimitiveResult Interpreter::primitiveIAPRefreshReceipt(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    pop();
    return PrimitiveResult::Success;
}

// Primitive 469: Get subscription status
// productId primitiveIAPGetSubscriptionStatus -> status
PrimitiveResult Interpreter::primitiveIAPGetSubscriptionStatus(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    popN(1);
    push(Oop::fromSmallInteger(0));  // Not subscribed
    return PrimitiveResult::Success;
}

// ===== SHARING/SOCIAL PRIMITIVES (470-479) =====

// Primitive 470: Share text
// text primitiveShareText -> self
PrimitiveResult Interpreter::primitiveShareText(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    // Would use UIActivityViewController
    popN(1);
    return PrimitiveResult::Success;
}

// Primitive 471: Share image
// imageData primitiveShareImage -> self
PrimitiveResult Interpreter::primitiveShareImage(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    popN(1);
    return PrimitiveResult::Success;
}

// Primitive 472: Share URL
// urlString primitiveShareURL -> self
PrimitiveResult Interpreter::primitiveShareURL(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    popN(1);
    return PrimitiveResult::Success;
}

// Primitive 473: Share file
// filePath primitiveShareFile -> self
PrimitiveResult Interpreter::primitiveShareFile(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    popN(1);
    return PrimitiveResult::Success;
}

// Primitive 474: Open URL in browser/app
// urlString primitiveOpenURL -> boolean
PrimitiveResult Interpreter::primitiveOpenURL(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    // Would use UIApplication.shared.open()
    popN(1);
    push(memory_.trueObject());
    return PrimitiveResult::Success;
}

// Primitive 475: Check if URL can be opened
// urlString primitiveCanOpenURL -> boolean
PrimitiveResult Interpreter::primitiveCanOpenURL(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    popN(1);
    push(memory_.trueObject());
    return PrimitiveResult::Success;
}

// Primitive 476: Compose email
// to subject body primitiveMailCompose -> self
PrimitiveResult Interpreter::primitiveMailCompose(int argCount) {
    if (argCount != 3) return PrimitiveResult::Failure;

    // Would use MFMailComposeViewController
    popN(3);
    return PrimitiveResult::Success;
}

// Primitive 477: Compose SMS/iMessage
// to body primitiveMessageCompose -> self
PrimitiveResult Interpreter::primitiveMessageCompose(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    // Would use MFMessageComposeViewController
    popN(2);
    return PrimitiveResult::Success;
}

// Primitive 478: Post to social network
// service text primitiveSocialPost -> self
PrimitiveResult Interpreter::primitiveSocialPost(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    popN(2);
    return PrimitiveResult::Success;
}

// Primitive 479: Print content
// content primitivePrint -> self
PrimitiveResult Interpreter::primitivePrint(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    // Would use UIPrintInteractionController
    popN(1);
    return PrimitiveResult::Success;
}

// ===== KEYCHAIN/SECURITY PRIMITIVES (480-489) =====

// Primitive 480: Store value in keychain
// key value primitiveKeychainSet -> boolean
PrimitiveResult Interpreter::primitiveKeychainSet(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    // Would use Security framework
    popN(2);
    push(memory_.trueObject());
    return PrimitiveResult::Success;
}

// Primitive 481: Get value from keychain
// key primitiveKeychainGet -> value or nil
PrimitiveResult Interpreter::primitiveKeychainGet(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    popN(1);
    push(memory_.nil());
    return PrimitiveResult::Success;
}

// Primitive 482: Delete value from keychain
// key primitiveKeychainDelete -> boolean
PrimitiveResult Interpreter::primitiveKeychainDelete(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    popN(1);
    push(memory_.trueObject());
    return PrimitiveResult::Success;
}

// Primitive 483: Check if key exists in keychain
// key primitiveKeychainHas -> boolean
PrimitiveResult Interpreter::primitiveKeychainHas(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    popN(1);
    push(memory_.falseObject());
    return PrimitiveResult::Success;
}

// Primitive 484: Check biometric availability
// primitiveBiometricAvailable -> type (0=none, 1=touchID, 2=faceID)
PrimitiveResult Interpreter::primitiveBiometricAvailable(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Would use LAContext.canEvaluatePolicy
    pop();
    push(Oop::fromSmallInteger(0));  // None available
    return PrimitiveResult::Success;
}

// Primitive 485: Authenticate with biometrics
// reason primitiveBiometricAuthenticate -> boolean
PrimitiveResult Interpreter::primitiveBiometricAuthenticate(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    // Would use LAContext.evaluatePolicy
    popN(1);
    push(memory_.falseObject());
    return PrimitiveResult::Success;
}

// Primitive 486: Generate cryptographically secure random bytes
// count primitiveCryptoRandomBytes -> byteArray
PrimitiveResult Interpreter::primitiveCryptoRandomBytes(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop countOop = stackTop();
    if (!countOop.isSmallInteger()) return PrimitiveResult::Failure;

    int64_t count = countOop.asSmallInteger();
    if (count < 0 || count > 65536) return PrimitiveResult::Failure;

    Oop bytes = memory_.allocateBytes(
        memory_.indexOfClass(memory_.specialObject(SpecialObjectIndex::ClassByteArray)),
        static_cast<size_t>(count));
    if (bytes.isNil()) return PrimitiveResult::Failure;

    // Fill with random bytes (would use SecRandomCopyBytes on iOS)
    for (size_t i = 0; i < static_cast<size_t>(count); i++) {
        memory_.storeByte(i, bytes, static_cast<uint8_t>(rand() & 0xFF));
    }

    popN(1);
    push(bytes);
    return PrimitiveResult::Success;
}

// Primitive 487: Compute hash (SHA-256)
// data primitiveHashCrypto -> byteArray
PrimitiveResult Interpreter::primitiveCryptoHash(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop dataOop = stackTop();
    if (dataOop.isImmediate()) return PrimitiveResult::Failure;

    // Would use CC_SHA256 from CommonCrypto
    // Return a dummy 32-byte hash
    Oop hash = memory_.allocateBytes(
        memory_.indexOfClass(memory_.specialObject(SpecialObjectIndex::ClassByteArray)),
        32);
    if (hash.isNil()) return PrimitiveResult::Failure;

    for (size_t i = 0; i < 32; i++) {
        memory_.storeByte(i, hash, 0);
    }

    popN(1);
    push(hash);
    return PrimitiveResult::Success;
}

// Primitive 488: Compute HMAC
// key data primitiveCryptoHMAC -> byteArray
PrimitiveResult Interpreter::primitiveCryptoHMAC(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    // Would use CCHmac from CommonCrypto
    Oop hmac = memory_.allocateBytes(
        memory_.indexOfClass(memory_.specialObject(SpecialObjectIndex::ClassByteArray)),
        32);
    if (hmac.isNil()) return PrimitiveResult::Failure;

    for (size_t i = 0; i < 32; i++) {
        memory_.storeByte(i, hmac, 0);
    }

    popN(2);
    push(hmac);
    return PrimitiveResult::Success;
}

// Primitive 489: Encrypt/decrypt data (AES)
// key data encrypt primitiveCryptoEncrypt -> byteArray
PrimitiveResult Interpreter::primitiveCryptoEncrypt(int argCount) {
    if (argCount != 3) return PrimitiveResult::Failure;

    // Would use CCCrypt from CommonCrypto
    return PrimitiveResult::Failure;
}

// ===== MISC PLATFORM PRIMITIVES (490-499) =====

// Primitive 490: Trigger haptic feedback
// type primitiveHapticFeedback -> self
PrimitiveResult Interpreter::primitiveHapticFeedback(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    // Would use UIFeedbackGenerator
    popN(1);
    return PrimitiveResult::Success;
}

// Primitive 491: Vibrate device
// primitiveVibrate -> self
PrimitiveResult Interpreter::primitiveVibrate(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Would use AudioServicesPlaySystemSound
    pop();
    return PrimitiveResult::Success;
}

// Primitive 492: Control flashlight/torch
// on primitiveFlashlight -> boolean
PrimitiveResult Interpreter::primitiveFlashlight(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    // Would use AVCaptureDevice.torchMode
    popN(1);
    push(memory_.trueObject());
    return PrimitiveResult::Success;
}

// Primitive 493: Disable/enable idle timer (screen dimming)
// disable primitiveIdleTimerDisable -> self
PrimitiveResult Interpreter::primitiveIdleTimerDisable(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    // Would set UIApplication.shared.isIdleTimerDisabled
    popN(1);
    return PrimitiveResult::Success;
}

// Primitive 494: Hide/show status bar
// hide primitiveStatusBarHide -> self
PrimitiveResult Interpreter::primitiveStatusBarHide(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    popN(1);
    return PrimitiveResult::Success;
}

// Primitive 495: Set status bar style
// style primitiveStatusBarStyle -> self
PrimitiveResult Interpreter::primitiveStatusBarStyle(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    popN(1);
    return PrimitiveResult::Success;
}

// Primitive 496: Lock orientation
// orientation primitiveOrientationLock -> self
PrimitiveResult Interpreter::primitiveOrientationLock(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    popN(1);
    return PrimitiveResult::Success;
}

// Primitive 497: Get current orientation
// primitiveOrientationGet -> orientation
PrimitiveResult Interpreter::primitiveOrientationGet(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // 1=portrait, 2=portraitUpsideDown, 3=landscapeLeft, 4=landscapeRight
    pop();
    push(Oop::fromSmallInteger(1));  // Portrait
    return PrimitiveResult::Success;
}

// Primitive 498: Request app review
// primitiveAppReview -> self
PrimitiveResult Interpreter::primitiveAppReview(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Would use SKStoreReviewController.requestReview()
    pop();
    return PrimitiveResult::Success;
}

// Primitive 499: Open app settings
// primitiveAppSettings -> self
PrimitiveResult Interpreter::primitiveAppSettings(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Would open UIApplication.openSettingsURLString
    pop();
    return PrimitiveResult::Success;
}

// ===== PROFILING PRIMITIVES (260-263) =====

// Primitive 260: VM profile samples into array
// sampleBuffer primitiveVMProfileSamplesInto -> count
// Copies profiling samples into the provided buffer
PrimitiveResult Interpreter::primitiveVMProfileSamplesInto(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop sampleBuffer = stackTop();

    if (sampleBuffer.isImmediate()) {
        return PrimitiveResult::Failure;
    }

    // Profiling samples would contain:
    // - Program counter samples
    // - Method/context information
    // - Timing data

    // For now, profiling is not implemented
    // Return 0 samples collected
    pop();  // pop buffer argument
    pop();  // pop receiver
    push(Oop::fromSmallInteger(0));
    return PrimitiveResult::Success;
}

// Primitive 261: VM profile info into array
// infoBuffer primitiveVMProfileInfoInto -> success
// Copies profiling metadata into the buffer
PrimitiveResult Interpreter::primitiveVMProfileInfoInto(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop infoBuffer = stackTop();

    if (infoBuffer.isImmediate()) {
        return PrimitiveResult::Failure;
    }

    size_t slotCount = memory_.slotCountOf(infoBuffer);
    if (slotCount < 6) {
        return PrimitiveResult::Failure;
    }

    // Profile info structure:
    // 0: total samples collected
    // 1: samples in VM code
    // 2: samples in Smalltalk code
    // 3: profiler status (0=stopped, 1=running)
    // 4: sample interval (microseconds)
    // 5: buffer size

    memory_.storePointer(0, infoBuffer, Oop::fromSmallInteger(0));  // total samples
    memory_.storePointer(1, infoBuffer, Oop::fromSmallInteger(0));  // VM samples
    memory_.storePointer(2, infoBuffer, Oop::fromSmallInteger(0));  // ST samples
    memory_.storePointer(3, infoBuffer, Oop::fromSmallInteger(0));  // status: stopped
    memory_.storePointer(4, infoBuffer, Oop::fromSmallInteger(1000));  // 1ms interval
    memory_.storePointer(5, infoBuffer, Oop::fromSmallInteger(0));  // buffer size

    pop();  // pop buffer argument
    pop();  // pop receiver
    push(memory_.trueObject());  // success
    return PrimitiveResult::Success;
}

// Primitive 262: Start VM profiler
// intervalMicroseconds primitiveVMProfileStart -> success
// Starts the VM profiler with given sampling interval
PrimitiveResult Interpreter::primitiveVMProfileStart(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop intervalOop = stackTop();

    if (!intervalOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t interval = intervalOop.asSmallInteger();
    if (interval <= 0) {
        return PrimitiveResult::Failure;
    }

    // A full implementation would:
    // 1. Set up a timer signal handler
    // 2. Configure sampling interval
    // 3. Allocate sample buffer
    // 4. Start collecting PC samples

    // For now, just acknowledge the request
    // Profiling requires platform-specific timer support
    pop();  // pop interval argument
    pop();  // pop receiver
    push(memory_.trueObject());  // return true (accepted)
    return PrimitiveResult::Success;
}

// Primitive 263: Stop VM profiler
// primitiveVMProfileStop -> sampleCount
// Stops the profiler and returns number of samples collected
PrimitiveResult Interpreter::primitiveVMProfileStop(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // A full implementation would:
    // 1. Stop the sampling timer
    // 2. Return the number of samples collected
    // 3. Keep samples available for retrieval

    // For now, return 0 samples
    pop();  // pop receiver
    push(Oop::fromSmallInteger(0));
    return PrimitiveResult::Success;
}

// ===== PLATFORM PRIMITIVES (500-513) =====

// Primitive 500: Get environment variable
// varName primitiveGetEnvironment -> value or nil
PrimitiveResult Interpreter::primitiveGetEnvironment(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop varNameOop = stackTop();

    if (varNameOop.isImmediate()) {
        return PrimitiveResult::Failure;
    }

    std::string varName = extractString(memory_, varNameOop);
    const char* value = getenv(varName.c_str());

    Oop result;
    if (value) {
        result = createStringObject(memory_, value);
        if (result.isNil()) {
            return PrimitiveResult::Failure;
        }
    } else {
        result = memory_.nil();
    }

    popN(2);  // pop varName and receiver
    push(result);
    return PrimitiveResult::Success;
}

// Primitive 501: Set environment variable
// varName value primitiveSetEnvironment -> success
PrimitiveResult Interpreter::primitiveSetEnvironment(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop valueOop = stackTop();
    Oop varNameOop = stackValue(1);

    if (varNameOop.isImmediate()) {
        return PrimitiveResult::Failure;
    }

    std::string varName = extractString(memory_, varNameOop);

    int result;
    if (valueOop.isNil()) {
        // Unset the variable
        result = unsetenv(varName.c_str());
    } else {
        if (valueOop.isImmediate()) {
            return PrimitiveResult::Failure;
        }
        std::string value = extractString(memory_, valueOop);
        result = setenv(varName.c_str(), value.c_str(), 1);
    }

    popN(3);  // pop value, varName, and receiver
    push(result == 0 ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

// Primitive 502: Get current working directory
// primitiveGetCurrentDirectory -> string
PrimitiveResult Interpreter::primitiveGetCurrentDirectory(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    char buffer[PATH_MAX];
    char* cwd = getcwd(buffer, sizeof(buffer));

    if (!cwd) {
        return PrimitiveResult::Failure;
    }

    Oop result = createStringObject(memory_, cwd);
    if (result.isNil()) {
        return PrimitiveResult::Failure;
    }

    pop();  // receiver
    push(result);
    return PrimitiveResult::Success;
}

// Primitive 503: Set current working directory
// path primitiveSetCurrentDirectory -> success
PrimitiveResult Interpreter::primitiveSetCurrentDirectory(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop pathOop = stackTop();

    if (pathOop.isImmediate()) {
        return PrimitiveResult::Failure;
    }

    std::string path = extractString(memory_, pathOop);
    int result = chdir(path.c_str());

    popN(2);  // pop path and receiver
    push(result == 0 ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

// Primitive 504: Get platform name
// primitiveGetPlatformName -> string (e.g., "iOS", "Mac OS", "unix")
PrimitiveResult Interpreter::primitiveGetPlatformName(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

#if defined(__APPLE__)
    #if TARGET_OS_IOS
        const char* platform = "iOS";
    #else
        const char* platform = "Mac OS";
    #endif
#elif defined(__linux__)
    const char* platform = "linux";
#else
    const char* platform = "unix";
#endif

    Oop result = createStringObject(memory_, platform);
    if (result.isNil()) {
        return PrimitiveResult::Failure;
    }

    pop();  // receiver
    push(result);
    return PrimitiveResult::Success;
}

// Primitive 505: Get OS version
// primitiveGetOSVersion -> string
PrimitiveResult Interpreter::primitiveGetOSVersion(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    struct utsname info;
    if (uname(&info) != 0) {
        return PrimitiveResult::Failure;
    }

    Oop result = createStringObject(memory_, info.release);
    if (result.isNil()) {
        return PrimitiveResult::Failure;
    }

    pop();  // receiver
    push(result);
    return PrimitiveResult::Success;
}

// Primitive 506: Get processor count
// primitiveGetProcessorCount -> integer
PrimitiveResult Interpreter::primitiveGetProcessorCount(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    long count = sysconf(_SC_NPROCESSORS_ONLN);
    if (count < 1) count = 1;

    pop();  // receiver
    push(Oop::fromSmallInteger(static_cast<int64_t>(count)));
    return PrimitiveResult::Success;
}

// Primitive 507: Get physical memory size
// primitiveGetPhysicalMemory -> bytes (as integer)
PrimitiveResult Interpreter::primitiveGetPhysicalMemory(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    long pages = sysconf(_SC_PHYS_PAGES);
    long pageSize = sysconf(_SC_PAGE_SIZE);

    int64_t totalMemory = static_cast<int64_t>(pages) * static_cast<int64_t>(pageSize);

    pop();  // receiver
    push(Oop::fromSmallInteger(totalMemory));
    return PrimitiveResult::Success;
}

// Primitive 508: Get host name
// primitiveGetHostName -> string
PrimitiveResult Interpreter::primitiveGetHostName(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        return PrimitiveResult::Failure;
    }

    Oop result = createStringObject(memory_, hostname);
    if (result.isNil()) {
        return PrimitiveResult::Failure;
    }

    pop();  // receiver
    push(result);
    return PrimitiveResult::Success;
}

// Primitive 509: Get user name
// primitiveGetUserName -> string
PrimitiveResult Interpreter::primitiveGetUserName(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    const char* username = getenv("USER");
    if (!username) {
        username = getenv("LOGNAME");
    }
    if (!username) {
        username = "unknown";
    }

    Oop result = createStringObject(memory_, username);
    if (result.isNil()) {
        return PrimitiveResult::Failure;
    }

    pop();  // receiver
    push(result);
    return PrimitiveResult::Success;
}

// Primitive 510: Get home directory
// primitiveGetHomeDirectory -> string
PrimitiveResult Interpreter::primitiveGetHomeDirectory(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    const char* home = getenv("HOME");
    if (!home) {
        home = "/";
    }

    Oop result = createStringObject(memory_, home);
    if (result.isNil()) {
        return PrimitiveResult::Failure;
    }

    pop();  // receiver
    push(result);
    return PrimitiveResult::Success;
}

// Primitive 511: Get temp directory
// primitiveGetTempDirectory -> string
PrimitiveResult Interpreter::primitiveGetTempDirectory(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    const char* temp = getenv("TMPDIR");
    if (!temp) {
        temp = "/tmp";
    }

    Oop result = createStringObject(memory_, temp);
    if (result.isNil()) {
        return PrimitiveResult::Failure;
    }

    pop();  // receiver
    push(result);
    return PrimitiveResult::Success;
}

// Primitive 512: Get VM version string
// primitiveGetVMVersion -> string
PrimitiveResult Interpreter::primitiveGetVMVersion(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Return our VM version string
    const char* version = "iOS Pharo VM 1.0 (Clean C++ Implementation)";

    Oop result = createStringObject(memory_, version);
    if (result.isNil()) {
        return PrimitiveResult::Failure;
    }

    pop();  // receiver
    push(result);
    return PrimitiveResult::Success;
}

// Primitive 513: Get system locale
// primitiveGetSystemLocale -> string (e.g., "en_US.UTF-8")
PrimitiveResult Interpreter::primitiveGetSystemLocale(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    const char* locale = getenv("LANG");
    if (!locale) {
        locale = getenv("LC_ALL");
    }
    if (!locale) {
        locale = "en_US.UTF-8";
    }

    Oop result = createStringObject(memory_, locale);
    if (result.isNil()) {
        return PrimitiveResult::Failure;
    }

    pop();  // receiver
    push(result);
    return PrimitiveResult::Success;
}

// ===== SMALLFLOAT PRIMITIVES (541-559) =====
//
// SmallFloat primitives are optimized versions of Float primitives.
// Since our Float primitives already handle SmallFloat via extractFloat(),
// these simply delegate to the corresponding Float primitives.

// Primitive 541: SmallFloat add
PrimitiveResult Interpreter::primitiveSmallFloatAdd(int argCount) {
    return primitiveFloatAdd(argCount);
}

// Primitive 542: SmallFloat subtract
PrimitiveResult Interpreter::primitiveSmallFloatSubtract(int argCount) {
    return primitiveFloatSubtract(argCount);
}

// Primitive 543: SmallFloat less than
PrimitiveResult Interpreter::primitiveSmallFloatLessThan(int argCount) {
    return primitiveFloatLessThan(argCount);
}

// Primitive 544: SmallFloat greater than
PrimitiveResult Interpreter::primitiveSmallFloatGreaterThan(int argCount) {
    return primitiveFloatGreaterThan(argCount);
}

// Primitive 545: SmallFloat less or equal
PrimitiveResult Interpreter::primitiveSmallFloatLessOrEqual(int argCount) {
    return primitiveFloatLessOrEqual(argCount);
}

// Primitive 546: SmallFloat greater or equal
PrimitiveResult Interpreter::primitiveSmallFloatGreaterOrEqual(int argCount) {
    return primitiveFloatGreaterOrEqual(argCount);
}

// Primitive 547: SmallFloat equal
PrimitiveResult Interpreter::primitiveSmallFloatEqual(int argCount) {
    return primitiveFloatEqual(argCount);
}

// Primitive 548: SmallFloat not equal
PrimitiveResult Interpreter::primitiveSmallFloatNotEqual(int argCount) {
    return primitiveFloatNotEqual(argCount);
}

// Primitive 549: SmallFloat multiply
PrimitiveResult Interpreter::primitiveSmallFloatMultiply(int argCount) {
    return primitiveFloatMultiply(argCount);
}

// Primitive 550: SmallFloat divide
PrimitiveResult Interpreter::primitiveSmallFloatDivide(int argCount) {
    return primitiveFloatDivide(argCount);
}

// Primitive 551: SmallFloat truncated
PrimitiveResult Interpreter::primitiveSmallFloatTruncated(int argCount) {
    return primitiveTruncated(argCount);
}

// Primitive 552: SmallFloat fractional part
PrimitiveResult Interpreter::primitiveSmallFloatFractionalPart(int argCount) {
    return primitiveFractionalPart(argCount);
}

// Primitive 553: SmallFloat exponent
PrimitiveResult Interpreter::primitiveSmallFloatExponent(int argCount) {
    return primitiveExponent(argCount);
}

// Primitive 554: SmallFloat times two power
PrimitiveResult Interpreter::primitiveSmallFloatTimesTwoPower(int argCount) {
    return primitiveTimesTwoPower(argCount);
}

// Primitive 555: SmallFloat square root
PrimitiveResult Interpreter::primitiveSmallFloatSquareRoot(int argCount) {
    return primitiveSquareRoot(argCount);
}

// Primitive 556: SmallFloat sine
PrimitiveResult Interpreter::primitiveSmallFloatSine(int argCount) {
    return primitiveSine(argCount);
}

// Primitive 557: SmallFloat arctan
PrimitiveResult Interpreter::primitiveSmallFloatArctan(int argCount) {
    return primitiveArctan(argCount);
}

// Primitive 558: SmallFloat natural log
PrimitiveResult Interpreter::primitiveSmallFloatLogN(int argCount) {
    return primitiveLogN(argCount);
}

// Primitive 559: SmallFloat exp
PrimitiveResult Interpreter::primitiveSmallFloatExp(int argCount) {
    return primitiveExp(argCount);
}

// ===== OLD SPACE / PINNED ALLOCATION PRIMITIVES (596-599) =====
//
// These primitives allocate objects with special placement or pinning:
// - OldSpace variants: allocate directly in old space (tenured)
// - Pinned variants: allocate pinned objects (won't be moved by GC)
//
// Note: Currently we allocate in the normal way (eden first) and:
// - For old space: rely on GC promotion (objects naturally move to old space)
// - For pinned: mark the object as pinned after allocation
//
// This is functionally correct but not optimal for immediate old-space placement.

// Primitive 596: Create fixed-size instance directly in old space
PrimitiveResult Interpreter::primitiveNewOldSpace(int argCount) {
    Oop rcvr = stackValue(0);  // Class

    if (!rcvr.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Get instance spec from class
    Oop formatObj = memory_.fetchPointer(2, rcvr);
    if (!formatObj.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t instSpec = formatObj.asSmallInteger();
    size_t instSize = instSpec & 0xFFFF;

    // Validate this is a fixed-size class (format < 2)
    int instFormat = (instSpec >> 16) & 0x1F;
    if (instFormat >= 2) {
        return PrimitiveResult::Failure;
    }

    uint32_t classIndex = memory_.indexOfClass(rcvr);
    Oop newObj = memory_.allocateSlots(classIndex, instSize);

    if (newObj.isNil()) {
        return PrimitiveResult::Failure;
    }

    // Note: Object is allocated in eden but will be promoted to old space by GC
    primitiveSuccess(newObj);
    return PrimitiveResult::Success;
}

// Primitive 597: Create variable-size instance directly in old space
PrimitiveResult Interpreter::primitiveNewWithArgOldSpace(int argCount) {
    Oop sizeOop = stackValue(0);
    Oop rcvr = stackValue(1);  // Class

    if (!rcvr.isObject()) {
        return PrimitiveResult::Failure;
    }

    if (!sizeOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t indexableSize = sizeOop.asSmallInteger();
    if (indexableSize < 0) {
        return PrimitiveResult::Failure;
    }

    // Get class format
    Oop formatObj = memory_.fetchPointer(2, rcvr);
    if (!formatObj.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t format = formatObj.asSmallInteger();
    size_t fixedSize = format & 0xFFFF;
    int instSpec = (format >> 16) & 0x1F;

    // Validate this is a variable-sized class (format >= 2)
    if (instSpec < 2) {
        return PrimitiveResult::Failure;
    }

    bool isBytes = instSpec >= 16;
    bool isWords32 = (instSpec >= 10 && instSpec <= 11);
    bool isWords16 = (instSpec >= 12 && instSpec <= 15);
    bool isWords64 = (instSpec == 9);
    uint32_t classIndex = memory_.indexOfClass(rcvr);

    Oop newObj;
    if (isBytes) {
        newObj = memory_.allocateBytes(classIndex, static_cast<size_t>(indexableSize));
    } else if (isWords32) {
        size_t byteCount = static_cast<size_t>(indexableSize) * 4;
        newObj = memory_.allocateBytes(classIndex, byteCount);
        if (!newObj.isNil()) {
            ObjectHeader* hdr = newObj.asObjectPtr();
            size_t padding32 = (hdr->slotCount() * 2) - static_cast<size_t>(indexableSize);
            hdr->setFormat(static_cast<ObjectFormat>(
                static_cast<int>(ObjectFormat::Indexable32) + padding32));
        }
    } else if (isWords16) {
        size_t byteCount = static_cast<size_t>(indexableSize) * 2;
        newObj = memory_.allocateBytes(classIndex, byteCount);
        if (!newObj.isNil()) {
            ObjectHeader* hdr = newObj.asObjectPtr();
            size_t padding16 = (hdr->slotCount() * 4) - static_cast<size_t>(indexableSize);
            hdr->setFormat(static_cast<ObjectFormat>(
                static_cast<int>(ObjectFormat::Indexable16) + padding16));
        }
    } else if (isWords64) {
        size_t byteCount = static_cast<size_t>(indexableSize) * 8;
        newObj = memory_.allocateBytes(classIndex, byteCount);
        if (!newObj.isNil()) {
            newObj.asObjectPtr()->setFormat(ObjectFormat::Indexable64);
        }
    } else {
        size_t totalSlots = fixedSize + static_cast<size_t>(indexableSize);
        ObjectFormat objFormat;
        switch (instSpec) {
            case 3: objFormat = ObjectFormat::IndexableWithFixed; break;
            case 4: objFormat = ObjectFormat::Weak; break;
            case 5: objFormat = ObjectFormat::WeakWithFixed; break;
            default: objFormat = ObjectFormat::Indexable; break;
        }
        newObj = memory_.allocateSlots(classIndex, totalSlots, objFormat);
    }

    if (newObj.isNil()) {
        return PrimitiveResult::Failure;
    }

    primitiveSuccess(newObj);
    return PrimitiveResult::Success;
}

// Primitive 598: Create fixed-size pinned instance
PrimitiveResult Interpreter::primitiveNewPinned(int argCount) {
    Oop rcvr = stackValue(0);  // Class

    if (!rcvr.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Get instance spec from class
    Oop formatObj = memory_.fetchPointer(2, rcvr);
    if (!formatObj.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t instSpec = formatObj.asSmallInteger();
    size_t instSize = instSpec & 0xFFFF;

    // Validate this is a fixed-size class (format < 2)
    int instFormat = (instSpec >> 16) & 0x1F;
    if (instFormat >= 2) {
        return PrimitiveResult::Failure;
    }

    uint32_t classIndex = memory_.indexOfClass(rcvr);
    Oop newObj = memory_.allocateSlots(classIndex, instSize);

    if (newObj.isNil()) {
        return PrimitiveResult::Failure;
    }

    // Mark the object as pinned so it won't be moved by GC
    memory_.pinObject(newObj);

    primitiveSuccess(newObj);
    return PrimitiveResult::Success;
}

// Primitive 599: Create variable-size pinned instance
PrimitiveResult Interpreter::primitiveNewWithArgPinned(int argCount) {
    Oop sizeOop = stackValue(0);
    Oop rcvr = stackValue(1);  // Class

    if (!rcvr.isObject()) {
        return PrimitiveResult::Failure;
    }

    if (!sizeOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t indexableSize = sizeOop.asSmallInteger();
    if (indexableSize < 0) {
        return PrimitiveResult::Failure;
    }

    // Get class format
    Oop formatObj = memory_.fetchPointer(2, rcvr);
    if (!formatObj.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t format = formatObj.asSmallInteger();
    size_t fixedSize = format & 0xFFFF;
    int instSpec = (format >> 16) & 0x1F;

    // Validate this is a variable-sized class (format >= 2)
    if (instSpec < 2) {
        return PrimitiveResult::Failure;
    }

    bool isBytes = instSpec >= 16;
    bool isWords32 = (instSpec >= 10 && instSpec <= 11);
    bool isWords16 = (instSpec >= 12 && instSpec <= 15);
    bool isWords64 = (instSpec == 9);
    uint32_t classIndex = memory_.indexOfClass(rcvr);

    Oop newObj;
    if (isBytes) {
        newObj = memory_.allocateBytes(classIndex, static_cast<size_t>(indexableSize));
    } else if (isWords32) {
        size_t byteCount = static_cast<size_t>(indexableSize) * 4;
        newObj = memory_.allocateBytes(classIndex, byteCount);
        if (!newObj.isNil()) {
            ObjectHeader* hdr = newObj.asObjectPtr();
            size_t padding32 = (hdr->slotCount() * 2) - static_cast<size_t>(indexableSize);
            hdr->setFormat(static_cast<ObjectFormat>(
                static_cast<int>(ObjectFormat::Indexable32) + padding32));
        }
    } else if (isWords16) {
        size_t byteCount = static_cast<size_t>(indexableSize) * 2;
        newObj = memory_.allocateBytes(classIndex, byteCount);
        if (!newObj.isNil()) {
            ObjectHeader* hdr = newObj.asObjectPtr();
            size_t padding16 = (hdr->slotCount() * 4) - static_cast<size_t>(indexableSize);
            hdr->setFormat(static_cast<ObjectFormat>(
                static_cast<int>(ObjectFormat::Indexable16) + padding16));
        }
    } else if (isWords64) {
        size_t byteCount = static_cast<size_t>(indexableSize) * 8;
        newObj = memory_.allocateBytes(classIndex, byteCount);
        if (!newObj.isNil()) {
            newObj.asObjectPtr()->setFormat(ObjectFormat::Indexable64);
        }
    } else {
        size_t totalSlots = fixedSize + static_cast<size_t>(indexableSize);
        ObjectFormat objFormat;
        switch (instSpec) {
            case 3: objFormat = ObjectFormat::IndexableWithFixed; break;
            case 4: objFormat = ObjectFormat::Weak; break;
            case 5: objFormat = ObjectFormat::WeakWithFixed; break;
            default: objFormat = ObjectFormat::Indexable; break;
        }
        newObj = memory_.allocateSlots(classIndex, totalSlots, objFormat);
    }

    if (newObj.isNil()) {
        return PrimitiveResult::Failure;
    }

    // Mark the object as pinned so it won't be moved by GC
    memory_.pinObject(newObj);

    primitiveSuccess(newObj);
    return PrimitiveResult::Success;
}

// ===== FFI BYTE ACCESS PRIMITIVES (600-659) =====
//
// These primitives provide low-level memory access for FFI.
// Two variants exist:
// - FromBytes/IntoBytes: access byte objects directly (ByteArray, String, etc.)
// - FromExternalAddress/IntoExternalAddress: dereference ExternalAddress first
//
// Arguments: receiver index [value]
// - receiver: byte object (ByteArray) or ExternalAddress
// - index: 0-based byte offset (SmallInteger)
// - value (store only): the value to store

// Helper: Get raw bytes pointer from a byte object
// Returns nullptr on failure
static uint8_t* getBytesPointer(ObjectMemory& memory, Oop obj, size_t& byteSize) {
    if (!obj.isObject()) return nullptr;

    // Get the object header to check format
    ObjectHeader* header = obj.asObjectPtr();
    if (!header) return nullptr;

    // Must be a bytes object (format >= 16)
    ObjectFormat format = header->format();
    if (static_cast<int>(format) < 16) return nullptr;

    // Get byte content pointer
    byteSize = memory.byteSizeOf(obj);
    return header->bytes();
}

// Helper: Get raw pointer from ExternalAddress object
// ExternalAddress stores a pointer in its first word of data
static uint8_t* getExternalAddressPointer(ObjectMemory& memory, Oop obj) {
    if (!obj.isObject()) return nullptr;

    ObjectHeader* header = obj.asObjectPtr();
    if (!header) return nullptr;

    // ExternalAddress is a byte object (format >= 16) containing a pointer
    ObjectFormat format = header->format();
    if (static_cast<int>(format) < 16) return nullptr;

    size_t byteSize = memory.byteSizeOf(obj);
    if (byteSize < sizeof(void*)) return nullptr;

    // The pointer is stored in the first bytes
    uint8_t* data = header->bytes();
    void* ptr;
    memcpy(&ptr, data, sizeof(void*));
    return reinterpret_cast<uint8_t*>(ptr);
}

// ===== LOAD FROM BYTES (600-614) =====

// Primitive 600: Load boolean (uint8 != 0) from bytes
PrimitiveResult Interpreter::primitiveLoadBoolean8FromBytes(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    Oop rcvr = stackValue(1);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    size_t byteSize;
    uint8_t* bytes = getBytesPointer(memory_, rcvr, byteSize);
    if (!bytes) return PrimitiveResult::Failure;

    if (index < 0 || static_cast<size_t>(index) >= byteSize) return PrimitiveResult::Failure;

    uint8_t value = bytes[index];
    Oop result = value != 0 ? memory_.trueObject() : memory_.falseObject();

    primitiveSuccess(result);
    return PrimitiveResult::Success;
}

// Primitive 601: Load uint8 from bytes
PrimitiveResult Interpreter::primitiveLoadUInt8FromBytes(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    Oop rcvr = stackValue(1);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    size_t byteSize;
    uint8_t* bytes = getBytesPointer(memory_, rcvr, byteSize);
    if (!bytes) return PrimitiveResult::Failure;

    if (index < 0 || static_cast<size_t>(index) >= byteSize) return PrimitiveResult::Failure;

    uint8_t value = bytes[index];
    primitiveSuccess(Oop::fromSmallInteger(value));
    return PrimitiveResult::Success;
}

// Primitive 602: Load int8 from bytes
PrimitiveResult Interpreter::primitiveLoadInt8FromBytes(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    Oop rcvr = stackValue(1);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    size_t byteSize;
    uint8_t* bytes = getBytesPointer(memory_, rcvr, byteSize);
    if (!bytes) return PrimitiveResult::Failure;

    if (index < 0 || static_cast<size_t>(index) >= byteSize) return PrimitiveResult::Failure;

    int8_t value = static_cast<int8_t>(bytes[index]);
    primitiveSuccess(Oop::fromSmallInteger(value));
    return PrimitiveResult::Success;
}

// Primitive 603: Load uint16 from bytes
PrimitiveResult Interpreter::primitiveLoadUInt16FromBytes(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    Oop rcvr = stackValue(1);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    size_t byteSize;
    uint8_t* bytes = getBytesPointer(memory_, rcvr, byteSize);
    if (!bytes) return PrimitiveResult::Failure;

    if (index < 0 || static_cast<size_t>(index) + 1 >= byteSize || byteSize < 2) return PrimitiveResult::Failure;

    uint16_t value;
    memcpy(&value, bytes + index, sizeof(value));
    primitiveSuccess(Oop::fromSmallInteger(value));
    return PrimitiveResult::Success;
}

// Primitive 604: Load int16 from bytes
PrimitiveResult Interpreter::primitiveLoadInt16FromBytes(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    Oop rcvr = stackValue(1);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    size_t byteSize;
    uint8_t* bytes = getBytesPointer(memory_, rcvr, byteSize);
    if (!bytes) return PrimitiveResult::Failure;

    if (index < 0 || static_cast<size_t>(index) + 1 >= byteSize || byteSize < 2) return PrimitiveResult::Failure;

    int16_t value;
    memcpy(&value, bytes + index, sizeof(value));
    primitiveSuccess(Oop::fromSmallInteger(value));
    return PrimitiveResult::Success;
}

// Primitive 605: Load uint32 from bytes
PrimitiveResult Interpreter::primitiveLoadUInt32FromBytes(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    Oop rcvr = stackValue(1);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    size_t byteSize;
    uint8_t* bytes = getBytesPointer(memory_, rcvr, byteSize);
    if (!bytes) return PrimitiveResult::Failure;

    if (index < 0 || static_cast<size_t>(index) + 3 >= byteSize || byteSize < 4) return PrimitiveResult::Failure;

    uint32_t value;
    memcpy(&value, bytes + index, sizeof(value));
    primitiveSuccess(Oop::fromSmallInteger(value));
    return PrimitiveResult::Success;
}

// Primitive 606: Load int32 from bytes
PrimitiveResult Interpreter::primitiveLoadInt32FromBytes(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    Oop rcvr = stackValue(1);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    size_t byteSize;
    uint8_t* bytes = getBytesPointer(memory_, rcvr, byteSize);
    if (!bytes) return PrimitiveResult::Failure;

    if (index < 0 || static_cast<size_t>(index) + 3 >= byteSize || byteSize < 4) return PrimitiveResult::Failure;

    int32_t value;
    memcpy(&value, bytes + index, sizeof(value));
    primitiveSuccess(Oop::fromSmallInteger(value));
    return PrimitiveResult::Success;
}

// Primitive 607: Load uint64 from bytes
PrimitiveResult Interpreter::primitiveLoadUInt64FromBytes(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    Oop rcvr = stackValue(1);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    size_t byteSize;
    uint8_t* bytes = getBytesPointer(memory_, rcvr, byteSize);
    if (!bytes) return PrimitiveResult::Failure;

    if (index < 0 || static_cast<size_t>(index) + 7 >= byteSize || byteSize < 8) return PrimitiveResult::Failure;

    uint64_t value;
    memcpy(&value, bytes + index, sizeof(value));

    Oop result = uint64ToOop(memory_, value);
    if (result.isNil()) return PrimitiveResult::Failure;
    primitiveSuccess(result);
    return PrimitiveResult::Success;
}

// Primitive 608: Load int64 from bytes
PrimitiveResult Interpreter::primitiveLoadInt64FromBytes(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    Oop rcvr = stackValue(1);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    size_t byteSize;
    uint8_t* bytes = getBytesPointer(memory_, rcvr, byteSize);
    if (!bytes) return PrimitiveResult::Failure;

    if (index < 0 || static_cast<size_t>(index) + 7 >= byteSize || byteSize < 8) return PrimitiveResult::Failure;

    int64_t value;
    memcpy(&value, bytes + index, sizeof(value));

    Oop result = int64ToOop(memory_, value);
    if (result.isNil()) return PrimitiveResult::Failure;
    primitiveSuccess(result);
    return PrimitiveResult::Success;
}

// Primitive 609: Load pointer from bytes
// Returns a new ExternalAddress containing the pointer
PrimitiveResult Interpreter::primitiveLoadPointerFromBytes(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    Oop rcvr = stackValue(1);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    size_t byteSize;
    uint8_t* bytes = getBytesPointer(memory_, rcvr, byteSize);
    if (!bytes) return PrimitiveResult::Failure;

    constexpr size_t ptrSize = sizeof(void*);
    if (index < 0 || static_cast<size_t>(index) + ptrSize - 1 >= byteSize || byteSize < ptrSize) {
        return PrimitiveResult::Failure;
    }

    void* ptr;
    memcpy(&ptr, bytes + index, ptrSize);

    // Create ExternalAddress to hold the pointer
    Oop externalAddressClass = memory_.findGlobal("ExternalAddress");
    if (externalAddressClass.isNil()) return PrimitiveResult::Failure;

    uint32_t classIndex = memory_.indexOfClass(externalAddressClass);
    if (classIndex == 0) return PrimitiveResult::Failure;

    Oop result = memory_.allocateBytes(classIndex, ptrSize);
    if (result.isNil()) return PrimitiveResult::Failure;

    // Store the pointer in the new ExternalAddress
    ObjectHeader* header = result.asObjectPtr();
    memcpy(header->bytes(), &ptr, ptrSize);

    primitiveSuccess(result);
    return PrimitiveResult::Success;
}

// Primitive 610: Load char8 (uint8 as Character) from bytes
PrimitiveResult Interpreter::primitiveLoadChar8FromBytes(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    Oop rcvr = stackValue(1);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    size_t byteSize;
    uint8_t* bytes = getBytesPointer(memory_, rcvr, byteSize);
    if (!bytes) return PrimitiveResult::Failure;

    if (index < 0 || static_cast<size_t>(index) >= byteSize) return PrimitiveResult::Failure;

    uint8_t value = bytes[index];
    primitiveSuccess(Oop::fromCharacter(value));
    return PrimitiveResult::Success;
}

// Primitive 611: Load char16 (uint16 as Character) from bytes
PrimitiveResult Interpreter::primitiveLoadChar16FromBytes(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    Oop rcvr = stackValue(1);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    size_t byteSize;
    uint8_t* bytes = getBytesPointer(memory_, rcvr, byteSize);
    if (!bytes) return PrimitiveResult::Failure;

    if (index < 0 || static_cast<size_t>(index) + 1 >= byteSize || byteSize < 2) return PrimitiveResult::Failure;

    uint16_t value;
    memcpy(&value, bytes + index, sizeof(value));
    primitiveSuccess(Oop::fromCharacter(value));
    return PrimitiveResult::Success;
}

// Primitive 612: Load char32 (uint32 as Character) from bytes
PrimitiveResult Interpreter::primitiveLoadChar32FromBytes(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    Oop rcvr = stackValue(1);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    size_t byteSize;
    uint8_t* bytes = getBytesPointer(memory_, rcvr, byteSize);
    if (!bytes) return PrimitiveResult::Failure;

    if (index < 0 || static_cast<size_t>(index) + 3 >= byteSize || byteSize < 4) return PrimitiveResult::Failure;

    uint32_t value;
    memcpy(&value, bytes + index, sizeof(value));

    // Character immediate only supports values up to 0x3FFFFFFF (30 bits)
    if (value > 0x3FFFFFFF) return PrimitiveResult::Failure;

    primitiveSuccess(Oop::fromCharacter(value));
    return PrimitiveResult::Success;
}

// Primitive 613: Load float32 from bytes (returns Float)
PrimitiveResult Interpreter::primitiveLoadFloat32FromBytes(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    Oop rcvr = stackValue(1);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    size_t byteSize;
    uint8_t* bytes = getBytesPointer(memory_, rcvr, byteSize);
    if (!bytes) return PrimitiveResult::Failure;

    if (index < 0 || static_cast<size_t>(index) + 3 >= byteSize || byteSize < 4) return PrimitiveResult::Failure;

    float value;
    memcpy(&value, bytes + index, sizeof(value));

    // Create Float object (boxed 64-bit double)
    Oop result = makeFloat(memory_,static_cast<double>(value));
    if (result.isNil()) return PrimitiveResult::Failure;

    primitiveSuccess(result);
    return PrimitiveResult::Success;
}

// Primitive 614: Load float64 from bytes
PrimitiveResult Interpreter::primitiveLoadFloat64FromBytes(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    Oop rcvr = stackValue(1);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    size_t byteSize;
    uint8_t* bytes = getBytesPointer(memory_, rcvr, byteSize);
    if (!bytes) return PrimitiveResult::Failure;

    if (index < 0 || static_cast<size_t>(index) + 7 >= byteSize || byteSize < 8) return PrimitiveResult::Failure;

    double value;
    memcpy(&value, bytes + index, sizeof(value));

    Oop result = makeFloat(memory_,value);
    if (result.isNil()) return PrimitiveResult::Failure;

    primitiveSuccess(result);
    return PrimitiveResult::Success;
}

// ===== STORE INTO BYTES (615-629) =====

// Primitive 615: Store boolean8 into bytes
PrimitiveResult Interpreter::primitiveStoreBoolean8IntoBytes(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop valueOop = stackTop();
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    size_t byteSize;
    uint8_t* bytes = getBytesPointer(memory_, rcvr, byteSize);
    if (!bytes) return PrimitiveResult::Failure;

    if (index < 0 || static_cast<size_t>(index) >= byteSize) return PrimitiveResult::Failure;

    // Convert to boolean (true -> 1, false -> 0)
    uint8_t value;
    if (valueOop == memory_.trueObject()) {
        value = 1;
    } else if (valueOop == memory_.falseObject()) {
        value = 0;
    } else {
        return PrimitiveResult::Failure;
    }

    bytes[index] = value;
    primitiveSuccess(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 616: Store uint8 into bytes
PrimitiveResult Interpreter::primitiveStoreUInt8IntoBytes(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop valueOop = stackTop();
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!indexOop.isSmallInteger() || !valueOop.isSmallInteger()) return PrimitiveResult::Failure;

    int64_t index = indexOop.asSmallInteger();
    int64_t value = valueOop.asSmallInteger();

    if (value < 0 || value > 255) return PrimitiveResult::Failure;

    size_t byteSize;
    uint8_t* bytes = getBytesPointer(memory_, rcvr, byteSize);
    if (!bytes) return PrimitiveResult::Failure;

    if (index < 0 || static_cast<size_t>(index) >= byteSize) return PrimitiveResult::Failure;

    bytes[index] = static_cast<uint8_t>(value);
    primitiveSuccess(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 617: Store int8 into bytes
PrimitiveResult Interpreter::primitiveStoreInt8IntoBytes(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop valueOop = stackTop();
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!indexOop.isSmallInteger() || !valueOop.isSmallInteger()) return PrimitiveResult::Failure;

    int64_t index = indexOop.asSmallInteger();
    int64_t value = valueOop.asSmallInteger();

    if (value < -128 || value > 127) return PrimitiveResult::Failure;

    size_t byteSize;
    uint8_t* bytes = getBytesPointer(memory_, rcvr, byteSize);
    if (!bytes) return PrimitiveResult::Failure;

    if (index < 0 || static_cast<size_t>(index) >= byteSize) return PrimitiveResult::Failure;

    bytes[index] = static_cast<uint8_t>(static_cast<int8_t>(value));
    primitiveSuccess(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 618: Store uint16 into bytes
PrimitiveResult Interpreter::primitiveStoreUInt16IntoBytes(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop valueOop = stackTop();
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!indexOop.isSmallInteger() || !valueOop.isSmallInteger()) return PrimitiveResult::Failure;

    int64_t index = indexOop.asSmallInteger();
    int64_t value = valueOop.asSmallInteger();

    if (value < 0 || value > 65535) return PrimitiveResult::Failure;

    size_t byteSize;
    uint8_t* bytes = getBytesPointer(memory_, rcvr, byteSize);
    if (!bytes) return PrimitiveResult::Failure;

    if (index < 0 || static_cast<size_t>(index) + 1 >= byteSize || byteSize < 2) return PrimitiveResult::Failure;

    uint16_t u16value = static_cast<uint16_t>(value);
    memcpy(bytes + index, &u16value, sizeof(u16value));
    primitiveSuccess(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 619: Store int16 into bytes
PrimitiveResult Interpreter::primitiveStoreInt16IntoBytes(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop valueOop = stackTop();
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!indexOop.isSmallInteger() || !valueOop.isSmallInteger()) return PrimitiveResult::Failure;

    int64_t index = indexOop.asSmallInteger();
    int64_t value = valueOop.asSmallInteger();

    if (value < -32768 || value > 32767) return PrimitiveResult::Failure;

    size_t byteSize;
    uint8_t* bytes = getBytesPointer(memory_, rcvr, byteSize);
    if (!bytes) return PrimitiveResult::Failure;

    if (index < 0 || static_cast<size_t>(index) + 1 >= byteSize || byteSize < 2) return PrimitiveResult::Failure;

    int16_t i16value = static_cast<int16_t>(value);
    memcpy(bytes + index, &i16value, sizeof(i16value));
    primitiveSuccess(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 620: Store uint32 into bytes
PrimitiveResult Interpreter::primitiveStoreUInt32IntoBytes(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop valueOop = stackTop();
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!indexOop.isSmallInteger() || !valueOop.isSmallInteger()) return PrimitiveResult::Failure;

    int64_t index = indexOop.asSmallInteger();
    int64_t value = valueOop.asSmallInteger();

    if (value < 0 || value > static_cast<int64_t>(UINT32_MAX)) return PrimitiveResult::Failure;

    size_t byteSize;
    uint8_t* bytes = getBytesPointer(memory_, rcvr, byteSize);
    if (!bytes) return PrimitiveResult::Failure;

    if (index < 0 || static_cast<size_t>(index) + 3 >= byteSize || byteSize < 4) return PrimitiveResult::Failure;

    uint32_t u32value = static_cast<uint32_t>(value);
    memcpy(bytes + index, &u32value, sizeof(u32value));
    primitiveSuccess(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 621: Store int32 into bytes
PrimitiveResult Interpreter::primitiveStoreInt32IntoBytes(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop valueOop = stackTop();
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!indexOop.isSmallInteger() || !valueOop.isSmallInteger()) return PrimitiveResult::Failure;

    int64_t index = indexOop.asSmallInteger();
    int64_t value = valueOop.asSmallInteger();

    if (value < INT32_MIN || value > INT32_MAX) return PrimitiveResult::Failure;

    size_t byteSize;
    uint8_t* bytes = getBytesPointer(memory_, rcvr, byteSize);
    if (!bytes) return PrimitiveResult::Failure;

    if (index < 0 || static_cast<size_t>(index) + 3 >= byteSize || byteSize < 4) return PrimitiveResult::Failure;

    int32_t i32value = static_cast<int32_t>(value);
    memcpy(bytes + index, &i32value, sizeof(i32value));
    primitiveSuccess(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 622: Store uint64 into bytes
PrimitiveResult Interpreter::primitiveStoreUInt64IntoBytes(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop valueOop = stackTop();
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!indexOop.isSmallInteger() || !valueOop.isSmallInteger()) return PrimitiveResult::Failure;

    int64_t index = indexOop.asSmallInteger();
    int64_t value = valueOop.asSmallInteger();

    // For now, only support positive values that fit in SmallInteger
    if (value < 0) return PrimitiveResult::Failure;

    size_t byteSize;
    uint8_t* bytes = getBytesPointer(memory_, rcvr, byteSize);
    if (!bytes) return PrimitiveResult::Failure;

    if (index < 0 || static_cast<size_t>(index) + 7 >= byteSize || byteSize < 8) return PrimitiveResult::Failure;

    uint64_t u64value = static_cast<uint64_t>(value);
    memcpy(bytes + index, &u64value, sizeof(u64value));
    primitiveSuccess(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 623: Store int64 into bytes
PrimitiveResult Interpreter::primitiveStoreInt64IntoBytes(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop valueOop = stackTop();
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!indexOop.isSmallInteger() || !valueOop.isSmallInteger()) return PrimitiveResult::Failure;

    int64_t index = indexOop.asSmallInteger();
    int64_t value = valueOop.asSmallInteger();

    size_t byteSize;
    uint8_t* bytes = getBytesPointer(memory_, rcvr, byteSize);
    if (!bytes) return PrimitiveResult::Failure;

    if (index < 0 || static_cast<size_t>(index) + 7 >= byteSize || byteSize < 8) return PrimitiveResult::Failure;

    memcpy(bytes + index, &value, sizeof(value));
    primitiveSuccess(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 624: Store pointer into bytes
PrimitiveResult Interpreter::primitiveStorePointerIntoBytes(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop valueOop = stackTop();
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    // Value must be an ExternalAddress
    uint8_t* valueBytes = getExternalAddressPointer(memory_, valueOop);
    if (!valueBytes) {
        // Also accept null pointer from nil
        if (!valueOop.isNil()) return PrimitiveResult::Failure;
        valueBytes = nullptr;
    }

    size_t byteSize;
    uint8_t* bytes = getBytesPointer(memory_, rcvr, byteSize);
    if (!bytes) return PrimitiveResult::Failure;

    constexpr size_t ptrSize = sizeof(void*);
    if (index < 0 || static_cast<size_t>(index) + ptrSize - 1 >= byteSize || byteSize < ptrSize) {
        return PrimitiveResult::Failure;
    }

    // Get the pointer from the ExternalAddress (or use null)
    void* ptr = valueBytes;
    memcpy(bytes + index, &ptr, ptrSize);
    primitiveSuccess(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 625: Store char8 into bytes
PrimitiveResult Interpreter::primitiveStoreChar8IntoBytes(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop valueOop = stackTop();
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    if (!valueOop.isCharacter()) return PrimitiveResult::Failure;

    int64_t index = indexOop.asSmallInteger();
    uint32_t charValue = valueOop.asCharacter();

    if (charValue > 255) return PrimitiveResult::Failure;

    size_t byteSize;
    uint8_t* bytes = getBytesPointer(memory_, rcvr, byteSize);
    if (!bytes) return PrimitiveResult::Failure;

    if (index < 0 || static_cast<size_t>(index) >= byteSize) return PrimitiveResult::Failure;

    bytes[index] = static_cast<uint8_t>(charValue);
    primitiveSuccess(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 626: Store char16 into bytes
PrimitiveResult Interpreter::primitiveStoreChar16IntoBytes(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop valueOop = stackTop();
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    if (!valueOop.isCharacter()) return PrimitiveResult::Failure;

    int64_t index = indexOop.asSmallInteger();
    uint32_t charValue = valueOop.asCharacter();

    if (charValue > 65535) return PrimitiveResult::Failure;

    size_t byteSize;
    uint8_t* bytes = getBytesPointer(memory_, rcvr, byteSize);
    if (!bytes) return PrimitiveResult::Failure;

    if (index < 0 || static_cast<size_t>(index) + 1 >= byteSize || byteSize < 2) return PrimitiveResult::Failure;

    uint16_t u16value = static_cast<uint16_t>(charValue);
    memcpy(bytes + index, &u16value, sizeof(u16value));
    primitiveSuccess(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 627: Store char32 into bytes
PrimitiveResult Interpreter::primitiveStoreChar32IntoBytes(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop valueOop = stackTop();
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    if (!valueOop.isCharacter()) return PrimitiveResult::Failure;

    int64_t index = indexOop.asSmallInteger();
    uint32_t charValue = valueOop.asCharacter();

    size_t byteSize;
    uint8_t* bytes = getBytesPointer(memory_, rcvr, byteSize);
    if (!bytes) return PrimitiveResult::Failure;

    if (index < 0 || static_cast<size_t>(index) + 3 >= byteSize || byteSize < 4) return PrimitiveResult::Failure;

    memcpy(bytes + index, &charValue, sizeof(charValue));
    primitiveSuccess(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 628: Store float32 into bytes
PrimitiveResult Interpreter::primitiveStoreFloat32IntoBytes(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop valueOop = stackTop();
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    // Get float value (from Float object or SmallFloat)
    double dvalue;
    if (!extractFloat(memory_, valueOop, dvalue)) return PrimitiveResult::Failure;

    size_t byteSize;
    uint8_t* bytes = getBytesPointer(memory_, rcvr, byteSize);
    if (!bytes) return PrimitiveResult::Failure;

    if (index < 0 || static_cast<size_t>(index) + 3 >= byteSize || byteSize < 4) return PrimitiveResult::Failure;

    float fvalue = static_cast<float>(dvalue);
    memcpy(bytes + index, &fvalue, sizeof(fvalue));
    primitiveSuccess(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 629: Store float64 into bytes
PrimitiveResult Interpreter::primitiveStoreFloat64IntoBytes(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop valueOop = stackTop();
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    double dvalue;
    if (!extractFloat(memory_, valueOop, dvalue)) return PrimitiveResult::Failure;

    size_t byteSize;
    uint8_t* bytes = getBytesPointer(memory_, rcvr, byteSize);
    if (!bytes) return PrimitiveResult::Failure;

    if (index < 0 || static_cast<size_t>(index) + 7 >= byteSize || byteSize < 8) return PrimitiveResult::Failure;

    memcpy(bytes + index, &dvalue, sizeof(dvalue));
    primitiveSuccess(valueOop);
    return PrimitiveResult::Success;
}

// ===== LOAD FROM EXTERNAL ADDRESS (630-644) =====
// These read from memory pointed to by an ExternalAddress

// Primitive 630: Load boolean8 from external address
PrimitiveResult Interpreter::primitiveLoadBoolean8FromExternalAddress(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    Oop rcvr = stackValue(1);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    uint8_t* ptr = getExternalAddressPointer(memory_, rcvr);
    if (!ptr) return PrimitiveResult::Failure;

    uint8_t value = ptr[index];
    Oop result = value != 0 ? memory_.trueObject() : memory_.falseObject();

    primitiveSuccess(result);
    return PrimitiveResult::Success;
}

// Primitive 631: Load uint8 from external address
PrimitiveResult Interpreter::primitiveLoadUInt8FromExternalAddress(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    Oop rcvr = stackValue(1);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    uint8_t* ptr = getExternalAddressPointer(memory_, rcvr);
    if (!ptr) return PrimitiveResult::Failure;

    uint8_t value = ptr[index];
    primitiveSuccess(Oop::fromSmallInteger(value));
    return PrimitiveResult::Success;
}

// Primitive 632: Load int8 from external address
PrimitiveResult Interpreter::primitiveLoadInt8FromExternalAddress(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    Oop rcvr = stackValue(1);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    uint8_t* ptr = getExternalAddressPointer(memory_, rcvr);
    if (!ptr) return PrimitiveResult::Failure;

    int8_t value = static_cast<int8_t>(ptr[index]);
    primitiveSuccess(Oop::fromSmallInteger(value));
    return PrimitiveResult::Success;
}

// Primitive 633: Load uint16 from external address
PrimitiveResult Interpreter::primitiveLoadUInt16FromExternalAddress(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    Oop rcvr = stackValue(1);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    uint8_t* ptr = getExternalAddressPointer(memory_, rcvr);
    if (!ptr) return PrimitiveResult::Failure;

    uint16_t value;
    memcpy(&value, ptr + index, sizeof(value));
    primitiveSuccess(Oop::fromSmallInteger(value));
    return PrimitiveResult::Success;
}

// Primitive 634: Load int16 from external address
PrimitiveResult Interpreter::primitiveLoadInt16FromExternalAddress(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    Oop rcvr = stackValue(1);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    uint8_t* ptr = getExternalAddressPointer(memory_, rcvr);
    if (!ptr) return PrimitiveResult::Failure;

    int16_t value;
    memcpy(&value, ptr + index, sizeof(value));
    primitiveSuccess(Oop::fromSmallInteger(value));
    return PrimitiveResult::Success;
}

// Primitive 635: Load uint32 from external address
PrimitiveResult Interpreter::primitiveLoadUInt32FromExternalAddress(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    Oop rcvr = stackValue(1);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    uint8_t* ptr = getExternalAddressPointer(memory_, rcvr);
    if (!ptr) return PrimitiveResult::Failure;

    uint32_t value;
    memcpy(&value, ptr + index, sizeof(value));
    primitiveSuccess(Oop::fromSmallInteger(value));
    return PrimitiveResult::Success;
}

// Primitive 636: Load int32 from external address
PrimitiveResult Interpreter::primitiveLoadInt32FromExternalAddress(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    Oop rcvr = stackValue(1);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    uint8_t* ptr = getExternalAddressPointer(memory_, rcvr);
    if (!ptr) return PrimitiveResult::Failure;

    int32_t value;
    memcpy(&value, ptr + index, sizeof(value));
    primitiveSuccess(Oop::fromSmallInteger(value));
    return PrimitiveResult::Success;
}

// Primitive 637: Load uint64 from external address
PrimitiveResult Interpreter::primitiveLoadUInt64FromExternalAddress(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    Oop rcvr = stackValue(1);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    uint8_t* ptr = getExternalAddressPointer(memory_, rcvr);
    if (!ptr) return PrimitiveResult::Failure;

    uint64_t value;
    memcpy(&value, ptr + index, sizeof(value));

    Oop result = uint64ToOop(memory_, value);
    if (result.isNil()) return PrimitiveResult::Failure;
    primitiveSuccess(result);
    return PrimitiveResult::Success;
}

// Primitive 638: Load int64 from external address
PrimitiveResult Interpreter::primitiveLoadInt64FromExternalAddress(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    Oop rcvr = stackValue(1);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    uint8_t* ptr = getExternalAddressPointer(memory_, rcvr);
    if (!ptr) return PrimitiveResult::Failure;

    int64_t value;
    memcpy(&value, ptr + index, sizeof(value));

    Oop result = int64ToOop(memory_, value);
    if (result.isNil()) return PrimitiveResult::Failure;
    primitiveSuccess(result);
    return PrimitiveResult::Success;
}

// Primitive 639: Load pointer from external address
PrimitiveResult Interpreter::primitiveLoadPointerFromExternalAddress(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    Oop rcvr = stackValue(1);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    uint8_t* ptr = getExternalAddressPointer(memory_, rcvr);
    if (!ptr) return PrimitiveResult::Failure;

    void* value;
    memcpy(&value, ptr + index, sizeof(value));

    // Create ExternalAddress to hold the pointer
    Oop externalAddressClass = memory_.findGlobal("ExternalAddress");
    if (externalAddressClass.isNil()) return PrimitiveResult::Failure;

    constexpr size_t ptrSize = sizeof(void*);
    uint32_t classIndex = memory_.indexOfClass(externalAddressClass);
    if (classIndex == 0) return PrimitiveResult::Failure;

    Oop result = memory_.allocateBytes(classIndex, ptrSize);
    if (result.isNil()) return PrimitiveResult::Failure;

    ObjectHeader* header = result.asObjectPtr();
    memcpy(header->bytes(), &value, ptrSize);

    primitiveSuccess(result);
    return PrimitiveResult::Success;
}

// Primitive 640: Load char8 from external address
PrimitiveResult Interpreter::primitiveLoadChar8FromExternalAddress(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    Oop rcvr = stackValue(1);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    uint8_t* ptr = getExternalAddressPointer(memory_, rcvr);
    if (!ptr) return PrimitiveResult::Failure;

    uint8_t value = ptr[index];
    primitiveSuccess(Oop::fromCharacter(value));
    return PrimitiveResult::Success;
}

// Primitive 641: Load char16 from external address
PrimitiveResult Interpreter::primitiveLoadChar16FromExternalAddress(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    Oop rcvr = stackValue(1);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    uint8_t* ptr = getExternalAddressPointer(memory_, rcvr);
    if (!ptr) return PrimitiveResult::Failure;

    uint16_t value;
    memcpy(&value, ptr + index, sizeof(value));
    primitiveSuccess(Oop::fromCharacter(value));
    return PrimitiveResult::Success;
}

// Primitive 642: Load char32 from external address
PrimitiveResult Interpreter::primitiveLoadChar32FromExternalAddress(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    Oop rcvr = stackValue(1);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    uint8_t* ptr = getExternalAddressPointer(memory_, rcvr);
    if (!ptr) return PrimitiveResult::Failure;

    uint32_t value;
    memcpy(&value, ptr + index, sizeof(value));

    if (value > 0x3FFFFFFF) return PrimitiveResult::Failure;

    primitiveSuccess(Oop::fromCharacter(value));
    return PrimitiveResult::Success;
}

// Primitive 643: Load float32 from external address
PrimitiveResult Interpreter::primitiveLoadFloat32FromExternalAddress(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    Oop rcvr = stackValue(1);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    uint8_t* ptr = getExternalAddressPointer(memory_, rcvr);
    if (!ptr) return PrimitiveResult::Failure;

    float value;
    memcpy(&value, ptr + index, sizeof(value));

    Oop result = makeFloat(memory_,static_cast<double>(value));
    if (result.isNil()) return PrimitiveResult::Failure;

    primitiveSuccess(result);
    return PrimitiveResult::Success;
}

// Primitive 644: Load float64 from external address
PrimitiveResult Interpreter::primitiveLoadFloat64FromExternalAddress(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop indexOop = stackTop();
    Oop rcvr = stackValue(1);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    uint8_t* ptr = getExternalAddressPointer(memory_, rcvr);
    if (!ptr) return PrimitiveResult::Failure;

    double value;
    memcpy(&value, ptr + index, sizeof(value));

    Oop result = makeFloat(memory_,value);
    if (result.isNil()) return PrimitiveResult::Failure;

    primitiveSuccess(result);
    return PrimitiveResult::Success;
}

// ===== STORE INTO EXTERNAL ADDRESS (645-659) =====

// Primitive 645: Store boolean8 into external address
PrimitiveResult Interpreter::primitiveStoreBoolean8IntoExternalAddress(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop valueOop = stackTop();
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    uint8_t* ptr = getExternalAddressPointer(memory_, rcvr);
    if (!ptr) return PrimitiveResult::Failure;

    uint8_t value;
    if (valueOop == memory_.trueObject()) {
        value = 1;
    } else if (valueOop == memory_.falseObject()) {
        value = 0;
    } else {
        return PrimitiveResult::Failure;
    }

    ptr[index] = value;
    primitiveSuccess(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 646: Store uint8 into external address
PrimitiveResult Interpreter::primitiveStoreUInt8IntoExternalAddress(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop valueOop = stackTop();
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!indexOop.isSmallInteger() || !valueOop.isSmallInteger()) return PrimitiveResult::Failure;

    int64_t index = indexOop.asSmallInteger();
    int64_t value = valueOop.asSmallInteger();

    if (value < 0 || value > 255) return PrimitiveResult::Failure;

    uint8_t* ptr = getExternalAddressPointer(memory_, rcvr);
    if (!ptr) return PrimitiveResult::Failure;

    ptr[index] = static_cast<uint8_t>(value);
    primitiveSuccess(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 647: Store int8 into external address
PrimitiveResult Interpreter::primitiveStoreInt8IntoExternalAddress(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop valueOop = stackTop();
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!indexOop.isSmallInteger() || !valueOop.isSmallInteger()) return PrimitiveResult::Failure;

    int64_t index = indexOop.asSmallInteger();
    int64_t value = valueOop.asSmallInteger();

    if (value < -128 || value > 127) return PrimitiveResult::Failure;

    uint8_t* ptr = getExternalAddressPointer(memory_, rcvr);
    if (!ptr) return PrimitiveResult::Failure;

    ptr[index] = static_cast<uint8_t>(static_cast<int8_t>(value));
    primitiveSuccess(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 648: Store uint16 into external address
PrimitiveResult Interpreter::primitiveStoreUInt16IntoExternalAddress(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop valueOop = stackTop();
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!indexOop.isSmallInteger() || !valueOop.isSmallInteger()) return PrimitiveResult::Failure;

    int64_t index = indexOop.asSmallInteger();
    int64_t value = valueOop.asSmallInteger();

    if (value < 0 || value > 65535) return PrimitiveResult::Failure;

    uint8_t* ptr = getExternalAddressPointer(memory_, rcvr);
    if (!ptr) return PrimitiveResult::Failure;

    uint16_t u16value = static_cast<uint16_t>(value);
    memcpy(ptr + index, &u16value, sizeof(u16value));
    primitiveSuccess(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 649: Store int16 into external address
PrimitiveResult Interpreter::primitiveStoreInt16IntoExternalAddress(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop valueOop = stackTop();
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!indexOop.isSmallInteger() || !valueOop.isSmallInteger()) return PrimitiveResult::Failure;

    int64_t index = indexOop.asSmallInteger();
    int64_t value = valueOop.asSmallInteger();

    if (value < -32768 || value > 32767) return PrimitiveResult::Failure;

    uint8_t* ptr = getExternalAddressPointer(memory_, rcvr);
    if (!ptr) return PrimitiveResult::Failure;

    int16_t i16value = static_cast<int16_t>(value);
    memcpy(ptr + index, &i16value, sizeof(i16value));
    primitiveSuccess(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 650: Store uint32 into external address
PrimitiveResult Interpreter::primitiveStoreUInt32IntoExternalAddress(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop valueOop = stackTop();
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!indexOop.isSmallInteger() || !valueOop.isSmallInteger()) return PrimitiveResult::Failure;

    int64_t index = indexOop.asSmallInteger();
    int64_t value = valueOop.asSmallInteger();

    if (value < 0 || value > static_cast<int64_t>(UINT32_MAX)) return PrimitiveResult::Failure;

    uint8_t* ptr = getExternalAddressPointer(memory_, rcvr);
    if (!ptr) return PrimitiveResult::Failure;

    uint32_t u32value = static_cast<uint32_t>(value);
    memcpy(ptr + index, &u32value, sizeof(u32value));
    primitiveSuccess(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 651: Store int32 into external address
PrimitiveResult Interpreter::primitiveStoreInt32IntoExternalAddress(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop valueOop = stackTop();
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!indexOop.isSmallInteger() || !valueOop.isSmallInteger()) return PrimitiveResult::Failure;

    int64_t index = indexOop.asSmallInteger();
    int64_t value = valueOop.asSmallInteger();

    if (value < INT32_MIN || value > INT32_MAX) return PrimitiveResult::Failure;

    uint8_t* ptr = getExternalAddressPointer(memory_, rcvr);
    if (!ptr) return PrimitiveResult::Failure;

    int32_t i32value = static_cast<int32_t>(value);
    memcpy(ptr + index, &i32value, sizeof(i32value));
    primitiveSuccess(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 652: Store uint64 into external address
PrimitiveResult Interpreter::primitiveStoreUInt64IntoExternalAddress(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop valueOop = stackTop();
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!indexOop.isSmallInteger() || !valueOop.isSmallInteger()) return PrimitiveResult::Failure;

    int64_t index = indexOop.asSmallInteger();
    int64_t value = valueOop.asSmallInteger();

    if (value < 0) return PrimitiveResult::Failure;

    uint8_t* ptr = getExternalAddressPointer(memory_, rcvr);
    if (!ptr) return PrimitiveResult::Failure;

    uint64_t u64value = static_cast<uint64_t>(value);
    memcpy(ptr + index, &u64value, sizeof(u64value));
    primitiveSuccess(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 653: Store int64 into external address
PrimitiveResult Interpreter::primitiveStoreInt64IntoExternalAddress(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop valueOop = stackTop();
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!indexOop.isSmallInteger() || !valueOop.isSmallInteger()) return PrimitiveResult::Failure;

    int64_t index = indexOop.asSmallInteger();
    int64_t value = valueOop.asSmallInteger();

    uint8_t* ptr = getExternalAddressPointer(memory_, rcvr);
    if (!ptr) return PrimitiveResult::Failure;

    memcpy(ptr + index, &value, sizeof(value));
    primitiveSuccess(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 654: Store pointer into external address
PrimitiveResult Interpreter::primitiveStorePointerIntoExternalAddress(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop valueOop = stackTop();
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    uint8_t* ptr = getExternalAddressPointer(memory_, rcvr);
    if (!ptr) return PrimitiveResult::Failure;

    // Value must be an ExternalAddress or nil
    void* valuePtr = nullptr;
    if (!valueOop.isNil()) {
        valuePtr = getExternalAddressPointer(memory_, valueOop);
        if (!valuePtr && !valueOop.isNil()) return PrimitiveResult::Failure;
    }

    memcpy(ptr + index, &valuePtr, sizeof(valuePtr));
    primitiveSuccess(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 655: Store char8 into external address
PrimitiveResult Interpreter::primitiveStoreChar8IntoExternalAddress(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop valueOop = stackTop();
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    if (!valueOop.isCharacter()) return PrimitiveResult::Failure;

    int64_t index = indexOop.asSmallInteger();
    uint32_t charValue = valueOop.asCharacter();

    if (charValue > 255) return PrimitiveResult::Failure;

    uint8_t* ptr = getExternalAddressPointer(memory_, rcvr);
    if (!ptr) return PrimitiveResult::Failure;

    ptr[index] = static_cast<uint8_t>(charValue);
    primitiveSuccess(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 656: Store char16 into external address
PrimitiveResult Interpreter::primitiveStoreChar16IntoExternalAddress(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop valueOop = stackTop();
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    if (!valueOop.isCharacter()) return PrimitiveResult::Failure;

    int64_t index = indexOop.asSmallInteger();
    uint32_t charValue = valueOop.asCharacter();

    if (charValue > 65535) return PrimitiveResult::Failure;

    uint8_t* ptr = getExternalAddressPointer(memory_, rcvr);
    if (!ptr) return PrimitiveResult::Failure;

    uint16_t u16value = static_cast<uint16_t>(charValue);
    memcpy(ptr + index, &u16value, sizeof(u16value));
    primitiveSuccess(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 657: Store char32 into external address
PrimitiveResult Interpreter::primitiveStoreChar32IntoExternalAddress(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop valueOop = stackTop();
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    if (!valueOop.isCharacter()) return PrimitiveResult::Failure;

    int64_t index = indexOop.asSmallInteger();
    uint32_t charValue = valueOop.asCharacter();

    uint8_t* ptr = getExternalAddressPointer(memory_, rcvr);
    if (!ptr) return PrimitiveResult::Failure;

    memcpy(ptr + index, &charValue, sizeof(charValue));
    primitiveSuccess(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 658: Store float32 into external address
PrimitiveResult Interpreter::primitiveStoreFloat32IntoExternalAddress(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop valueOop = stackTop();
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    double dvalue;
    if (!extractFloat(memory_, valueOop, dvalue)) return PrimitiveResult::Failure;

    uint8_t* ptr = getExternalAddressPointer(memory_, rcvr);
    if (!ptr) return PrimitiveResult::Failure;

    float fvalue = static_cast<float>(dvalue);
    memcpy(ptr + index, &fvalue, sizeof(fvalue));
    primitiveSuccess(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 659: Store float64 into external address
PrimitiveResult Interpreter::primitiveStoreFloat64IntoExternalAddress(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop valueOop = stackTop();
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!indexOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t index = indexOop.asSmallInteger();

    double dvalue;
    if (!extractFloat(memory_, valueOop, dvalue)) return PrimitiveResult::Failure;

    uint8_t* ptr = getExternalAddressPointer(memory_, rcvr);
    if (!ptr) return PrimitiveResult::Failure;

    memcpy(ptr + index, &dvalue, sizeof(dvalue));
    primitiveSuccess(valueOop);
    return PrimitiveResult::Success;
}

// ===== FFI MODULE/SYMBOL LOADING PRIMITIVES =====
// These are named primitives called via primitive 117 (primitiveExternalCall)
// They are used by UFFI to load symbols from dynamic libraries

// primitiveLoadSymbolFromModule
// Stack: receiver, symbolString, moduleStringOrNil
// Returns: ExternalAddress containing the symbol address, or fails
PrimitiveResult Interpreter::primitiveLoadSymbolFromModule(int argCount) {
    if (argCount != 2) {
        return PrimitiveResult::Failure;
    }

    // Get arguments
    Oop moduleOop = stackTop();        // Module name (string or nil)
    Oop symbolOop = stackValue(1);     // Symbol name (string)
    Oop receiver = stackValue(2);      // Receiver (ignored)

    // Symbol must be a string
    if (!symbolOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* symbolHdr = symbolOop.asObjectPtr();
    if (!symbolHdr->isBytesObject()) {
        return PrimitiveResult::Failure;
    }

    std::string symbolName((char*)symbolHdr->bytes(), symbolHdr->byteSize());

    // Module can be nil, a string name, or an ExternalAddress handle
    std::string moduleName;
    void* moduleHandle = nullptr;

    if (!moduleOop.isNil() && moduleOop.rawBits() != memory_.nil().rawBits()) {
        if (moduleOop.isObject()) {
            ObjectHeader* moduleHdr = moduleOop.asObjectPtr();

            // Check if it's an ExternalAddress (contains a dlopen handle from primitiveLoadModule)
            Oop extAddrClass = memory_.specialObject(SpecialObjectIndex::ClassExternalAddress);
            bool isExternalAddress = false;
            if (!extAddrClass.isNil()) {
                uint32_t moduleClassIdx = moduleHdr->classIndex();
                uint32_t extAddrClassIdx = memory_.indexOfClass(extAddrClass);
                isExternalAddress = (moduleClassIdx == extAddrClassIdx);
            }

            if (isExternalAddress) {
                // ExternalAddress - read the dlopen handle
                moduleHandle = tffi_readAddress(moduleOop);
                moduleName = "<ExternalAddress>";
                // Our fake SDL2 handle maps to RTLD_DEFAULT
                if (moduleHandle == reinterpret_cast<void*>(0xDEADBEEF)) {
                    moduleHandle = RTLD_DEFAULT;
                    moduleName = "<SDL2-builtin>";
                }
            } else if (moduleHdr->isBytesObject()) {
                moduleName = std::string((char*)moduleHdr->bytes(), moduleHdr->byteSize());

                // For SDL2 or common libraries, use our FFI lookup
                if (moduleName.find("SDL2") != std::string::npos ||
                    moduleName.find("SDL") != std::string::npos) {
                    moduleHandle = RTLD_DEFAULT;
                } else {
                    moduleHandle = dlopen(moduleName.c_str(), RTLD_NOW | RTLD_GLOBAL);
                    if (!moduleHandle) {
                        std::string libName = "lib" + moduleName + ".dylib";
                        moduleHandle = dlopen(libName.c_str(), RTLD_NOW | RTLD_GLOBAL);
                    }
                    if (!moduleHandle) {
                        // dlopen failed — string may be a Smalltalk object repr
                        // (e.g., "a FFIMacLibraryFinder"), not a library path.
                        // Fall back to RTLD_DEFAULT to search all loaded images.
                        moduleHandle = RTLD_DEFAULT;
                    }
                }
            }
        }
    } else {
        // nil module - search in all loaded modules (RTLD_DEFAULT)
        moduleHandle = RTLD_DEFAULT;
    }

    // Look up the symbol - check our FFI function cache first (for SDL2 stubs),
    // then fall back to dlsym. This ensures our stubs (e.g. stub_SDL_PollEvent)
    // win over force-loaded real SDL2 symbols.
    void* symbolAddr = ffi::lookupFunction(moduleName.empty() ? "" : moduleName, symbolName);

    if (!symbolAddr) {
        symbolAddr = dlsym(moduleHandle, symbolName.c_str());
    }

    if (!symbolAddr) {
        return PrimitiveResult::Failure;
    }

    // Create an ExternalAddress containing the symbol address
    // ExternalAddress is a bytes object holding sizeof(void*) bytes
    Oop externalAddressClass = memory_.specialObject(SpecialObjectIndex::ClassExternalAddress);
    if (externalAddressClass.isNil()) {
        return PrimitiveResult::Failure;
    }

    // Allocate the ExternalAddress (8 bytes for a 64-bit pointer)
    uint32_t classIndex = memory_.indexOfClass(externalAddressClass);
    Oop result = memory_.allocateBytes(classIndex, sizeof(void*));
    if (result.isNil()) {
        return PrimitiveResult::Failure;
    }

    // Store the address in the object's bytes
    ObjectHeader* resultHdr = result.asObjectPtr();
    memcpy(resultHdr->bytes(), &symbolAddr, sizeof(void*));

    // Track symbol name for TFFI call logging
    g_symbolNames[reinterpret_cast<uintptr_t>(symbolAddr)] = symbolName;

    // Pop args and push result
    popN(static_cast<size_t>(argCount + 1));  // Pop args and receiver
    push(result);

    return PrimitiveResult::Success;
}

// primitiveLoadModule
// Stack: receiver, moduleString
// Returns: ExternalAddress containing the module handle, or fails
PrimitiveResult Interpreter::primitiveLoadModule(int argCount) {
    if (argCount != 1) {
        return PrimitiveResult::Failure;
    }

    // Get module name
    Oop moduleOop = stackTop();
    Oop receiver = stackValue(1);

    if (!moduleOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* moduleHdr = moduleOop.asObjectPtr();
    if (!moduleHdr->isBytesObject()) {
        return PrimitiveResult::Failure;
    }

    std::string moduleName((char*)moduleHdr->bytes(), moduleHdr->byteSize());

    void* moduleHandle = nullptr;

    // Special handling for SDL2
    if (moduleName.find("SDL2") != std::string::npos ||
        moduleName.find("SDL") != std::string::npos) {
        // SDL2 is "built-in" via our stubs - return a non-null handle
        moduleHandle = reinterpret_cast<void*>(0xDEADBEEF);
    } else {
        // Try to load the library
        moduleHandle = dlopen(moduleName.c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (!moduleHandle) {
            // Try with common prefixes/suffixes
            std::string libName = "lib" + moduleName + ".dylib";
            moduleHandle = dlopen(libName.c_str(), RTLD_NOW | RTLD_GLOBAL);
        }
        if (!moduleHandle) {
            // Try just .dylib suffix
            std::string libName = moduleName + ".dylib";
            moduleHandle = dlopen(libName.c_str(), RTLD_NOW | RTLD_GLOBAL);
        }
    }

    if (!moduleHandle) {
        return PrimitiveResult::Failure;
    }

    // Create an ExternalAddress containing the module handle
    Oop externalAddressClass = memory_.specialObject(SpecialObjectIndex::ClassExternalAddress);
    if (externalAddressClass.isNil()) {
        return PrimitiveResult::Failure;
    }

    uint32_t classIndex = memory_.indexOfClass(externalAddressClass);
    Oop result = memory_.allocateBytes(classIndex, sizeof(void*));
    if (result.isNil()) {
        return PrimitiveResult::Failure;
    }

    // Store the handle in the object's bytes
    ObjectHeader* resultHdr = result.asObjectPtr();
    memcpy(resultHdr->bytes(), &moduleHandle, sizeof(void*));

    // Pop args and push result
    popN(static_cast<size_t>(argCount + 1));
    push(result);

    return PrimitiveResult::Success;
}

// ===== FFI MEMORY ACCESS PRIMITIVES =====
// These are required by TFFIBackend for FFI to work

// primitiveFFIAllocate
// Stack: receiver, size -> ExternalAddress
// Allocates a block of external memory
PrimitiveResult Interpreter::primitiveFFIAllocate(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop sizeOop = stackTop();
    if (!sizeOop.isSmallInteger()) return PrimitiveResult::Failure;

    int64_t size = sizeOop.asSmallInteger();
    if (size <= 0) return PrimitiveResult::Failure;

    // Allocate external memory
    void* ptr = calloc(1, static_cast<size_t>(size));
    if (!ptr) return PrimitiveResult::Failure;

    // Create ExternalAddress object
    Oop extAddrClass = memory_.specialObject(SpecialObjectIndex::ClassExternalAddress);
    if (extAddrClass.isNil()) {
        free(ptr);
        return PrimitiveResult::Failure;
    }

    uint32_t classIndex = memory_.indexOfClass(extAddrClass);
    if (classIndex == 0) {
        free(ptr);
        return PrimitiveResult::Failure;
    }

    Oop result = memory_.allocateBytes(classIndex, sizeof(void*));
    if (result.isNil()) {
        free(ptr);
        return PrimitiveResult::Failure;
    }

    // Store pointer in ExternalAddress
    ObjectHeader* resultHdr = result.asObjectPtr();
    memcpy(resultHdr->bytes(), &ptr, sizeof(void*));

    popN(2);  // pop size and receiver
    push(result);
    return PrimitiveResult::Success;
}

// primitiveFFIFree
// Pharo: ExternalAddress >> primFFIFree (0-arg method, receiver is EA to free)
// Also supports 1-arg form: receiver.free(anExternalAddress)
PrimitiveResult Interpreter::primitiveFFIFree(int argCount) {
    Oop addrOop;
    if (argCount == 0) {
        // Unary: receiver is the ExternalAddress to free
        addrOop = stackTop();
    } else if (argCount == 1) {
        // Binary: argument is the ExternalAddress to free
        addrOop = stackTop();
    } else {
        return PrimitiveResult::Failure;
    }

    if (addrOop.isNil()) {
        if (argCount > 0) popN(argCount);
        return PrimitiveResult::Success;
    }

    if (!addrOop.isObject()) return PrimitiveResult::Failure;

    ObjectHeader* addrHdr = addrOop.asObjectPtr();
    if (!addrHdr->isBytesObject() || addrHdr->byteSize() < sizeof(void*)) {
        return PrimitiveResult::Failure;
    }

    void* ptr = nullptr;
    memcpy(&ptr, addrHdr->bytes(), sizeof(void*));

    if (ptr) {
        free(ptr);
        // Zero out the external address to prevent double-free
        memset(addrHdr->bytes(), 0, sizeof(void*));
    }

    if (argCount > 0) popN(argCount);  // pop args, leave receiver
    return PrimitiveResult::Success;
}

// ===== ByteArray data access primitives (600-629) =====
// These read/write typed data within byte-format objects (ByteArray, etc.)
// Used by Pharo's FFI marshaling layer (e.g., uint32AtOffset:, float64AtOffset:put:).
// Offset is zero-based. Primitives 600-614 are reads, 615-629 are writes.

// Helper: validate byte-object receiver and zero-based offset for reads
static bool byteObjectReadSetup(pharo::Interpreter& interp, pharo::ObjectMemory& mem,
                                  int argCount, ObjectHeader*& hdr, int64_t& offset) {
    if (argCount != 1) return false;
    Oop offsetOop = interp.stackValue(0);
    if (!offsetOop.isSmallInteger()) return false;
    offset = offsetOop.asSmallInteger();
    if (offset < 0) return false;
    Oop rcvr = interp.stackValue(1);
    if (!rcvr.isObject()) return false;
    hdr = rcvr.asObjectPtr();
    return hdr->isBytesObject();
}

// Helper: validate byte-object receiver and zero-based offset for writes, check immutability
static bool byteObjectWriteSetup(pharo::Interpreter& interp, pharo::ObjectMemory& mem,
                                   int argCount, ObjectHeader*& hdr, int64_t& offset,
                                   Oop& rcvr, Oop& valueOop) {
    if (argCount != 2) return false;
    valueOop = interp.stackValue(0);
    Oop offsetOop = interp.stackValue(1);
    if (!offsetOop.isSmallInteger()) return false;
    offset = offsetOop.asSmallInteger();
    if (offset < 0) return false;
    rcvr = interp.stackValue(2);
    if (!rcvr.isObject()) return false;
    hdr = rcvr.asObjectPtr();
    return hdr->isBytesObject();
}

// --- Primitives 600-612: ByteArray reads ---

// Primitive 600: boolean8AtOffset: — read 8-bit boolean (0=false, else true)
PrimitiveResult Interpreter::primitiveBytesBoolean8Read(int argCount) {
    ObjectHeader* hdr; int64_t offset;
    if (!byteObjectReadSetup(*this, memory_, argCount, hdr, offset)) return PrimitiveResult::Failure;
    if (static_cast<size_t>(offset) + 1 > hdr->byteSize()) return PrimitiveResult::Failure;
    uint8_t val = hdr->bytes()[offset];
    popN(2);
    push(val ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

// Primitive 601: uint8AtOffset: — read unsigned 8-bit from byte object
PrimitiveResult Interpreter::primitiveBytesUint8Read(int argCount) {
    ObjectHeader* hdr; int64_t offset;
    if (!byteObjectReadSetup(*this, memory_, argCount, hdr, offset)) return PrimitiveResult::Failure;
    if (static_cast<size_t>(offset) + 1 > hdr->byteSize()) return PrimitiveResult::Failure;
    popN(2);
    push(Oop::fromSmallInteger(hdr->bytes()[offset]));
    return PrimitiveResult::Success;
}

// Primitive 602: int8AtOffset: — read signed 8-bit from byte object
PrimitiveResult Interpreter::primitiveBytesInt8Read(int argCount) {
    ObjectHeader* hdr; int64_t offset;
    if (!byteObjectReadSetup(*this, memory_, argCount, hdr, offset)) return PrimitiveResult::Failure;
    if (static_cast<size_t>(offset) + 1 > hdr->byteSize()) return PrimitiveResult::Failure;
    int8_t val;
    memcpy(&val, hdr->bytes() + offset, 1);
    popN(2);
    push(Oop::fromSmallInteger(val));
    return PrimitiveResult::Success;
}

// Primitive 603: uint16AtOffset: — read unsigned 16-bit from byte object
PrimitiveResult Interpreter::primitiveBytesUint16Read(int argCount) {
    ObjectHeader* hdr; int64_t offset;
    if (!byteObjectReadSetup(*this, memory_, argCount, hdr, offset)) return PrimitiveResult::Failure;
    if (static_cast<size_t>(offset) + 2 > hdr->byteSize()) return PrimitiveResult::Failure;
    uint16_t val;
    memcpy(&val, hdr->bytes() + offset, sizeof(uint16_t));
    popN(2);
    push(Oop::fromSmallInteger(val));
    return PrimitiveResult::Success;
}

// Primitive 604: int16AtOffset: — read signed 16-bit from byte object
PrimitiveResult Interpreter::primitiveBytesInt16Read(int argCount) {
    ObjectHeader* hdr; int64_t offset;
    if (!byteObjectReadSetup(*this, memory_, argCount, hdr, offset)) return PrimitiveResult::Failure;
    if (static_cast<size_t>(offset) + 2 > hdr->byteSize()) return PrimitiveResult::Failure;
    int16_t val;
    memcpy(&val, hdr->bytes() + offset, sizeof(int16_t));
    popN(2);
    push(Oop::fromSmallInteger(val));
    return PrimitiveResult::Success;
}

// Primitive 605: uint32AtOffset: — read unsigned 32-bit from byte object
PrimitiveResult Interpreter::primitiveBytesUint32Read(int argCount) {
    ObjectHeader* hdr; int64_t offset;
    if (!byteObjectReadSetup(*this, memory_, argCount, hdr, offset)) return PrimitiveResult::Failure;
    if (static_cast<size_t>(offset) + 4 > hdr->byteSize()) return PrimitiveResult::Failure;
    uint32_t val;
    memcpy(&val, hdr->bytes() + offset, sizeof(uint32_t));
    popN(2);
    push(Oop::fromSmallInteger(static_cast<int64_t>(val)));
    return PrimitiveResult::Success;
}

// Primitive 606: int32AtOffset: — read signed 32-bit from byte object
PrimitiveResult Interpreter::primitiveBytesInt32Read(int argCount) {
    ObjectHeader* hdr; int64_t offset;
    if (!byteObjectReadSetup(*this, memory_, argCount, hdr, offset)) return PrimitiveResult::Failure;
    if (static_cast<size_t>(offset) + 4 > hdr->byteSize()) return PrimitiveResult::Failure;
    int32_t val;
    memcpy(&val, hdr->bytes() + offset, sizeof(int32_t));
    popN(2);
    push(Oop::fromSmallInteger(static_cast<int64_t>(val)));
    return PrimitiveResult::Success;
}

// Primitive 607: uint64AtOffset: — read unsigned 64-bit from byte object
PrimitiveResult Interpreter::primitiveBytesUint64Read(int argCount) {
    ObjectHeader* hdr; int64_t offset;
    if (!byteObjectReadSetup(*this, memory_, argCount, hdr, offset)) return PrimitiveResult::Failure;
    if (static_cast<size_t>(offset) + 8 > hdr->byteSize()) return PrimitiveResult::Failure;
    uint64_t val;
    memcpy(&val, hdr->bytes() + offset, sizeof(uint64_t));
    Oop result = uint64ToOop(memory_, val);
    if (result.isNil()) return PrimitiveResult::Failure;
    popN(2);
    push(result);
    return PrimitiveResult::Success;
}

// Primitive 608: int64AtOffset: — read signed 64-bit from byte object
PrimitiveResult Interpreter::primitiveBytesInt64Read(int argCount) {
    ObjectHeader* hdr; int64_t offset;
    if (!byteObjectReadSetup(*this, memory_, argCount, hdr, offset)) return PrimitiveResult::Failure;
    if (static_cast<size_t>(offset) + 8 > hdr->byteSize()) return PrimitiveResult::Failure;
    int64_t val;
    memcpy(&val, hdr->bytes() + offset, sizeof(int64_t));
    Oop result = int64ToOop(memory_, val);
    if (result.isNil()) return PrimitiveResult::Failure;
    popN(2);
    push(result);
    return PrimitiveResult::Success;
}

// Primitive 609: pointerAtOffset: — read pointer from byte object
PrimitiveResult Interpreter::primitiveBytesPointerRead(int argCount) {
    ObjectHeader* hdr; int64_t offset;
    if (!byteObjectReadSetup(*this, memory_, argCount, hdr, offset)) return PrimitiveResult::Failure;
    if (static_cast<size_t>(offset) + sizeof(void*) > hdr->byteSize()) return PrimitiveResult::Failure;
    void* val;
    memcpy(&val, hdr->bytes() + offset, sizeof(void*));
    Oop result = tffi_newExternalAddress(val);
    if (result.isNil()) return PrimitiveResult::Failure;
    popN(2);
    push(result);
    return PrimitiveResult::Success;
}

// Primitive 610: char8AtOffset: — read 8-bit character from byte object
PrimitiveResult Interpreter::primitiveBytesChar8Read(int argCount) {
    ObjectHeader* hdr; int64_t offset;
    if (!byteObjectReadSetup(*this, memory_, argCount, hdr, offset)) return PrimitiveResult::Failure;
    if (static_cast<size_t>(offset) + 1 > hdr->byteSize()) return PrimitiveResult::Failure;
    popN(2);
    push(Oop::fromCharacter(hdr->bytes()[offset]));
    return PrimitiveResult::Success;
}

// Primitive 611: char16AtOffset: — read 16-bit character from byte object
PrimitiveResult Interpreter::primitiveBytesChar16Read(int argCount) {
    ObjectHeader* hdr; int64_t offset;
    if (!byteObjectReadSetup(*this, memory_, argCount, hdr, offset)) return PrimitiveResult::Failure;
    if (static_cast<size_t>(offset) + 2 > hdr->byteSize()) return PrimitiveResult::Failure;
    uint16_t val;
    memcpy(&val, hdr->bytes() + offset, sizeof(uint16_t));
    popN(2);
    push(Oop::fromCharacter(val));
    return PrimitiveResult::Success;
}

// Primitive 612: char32AtOffset: — read 32-bit character from byte object
PrimitiveResult Interpreter::primitiveBytesChar32Read(int argCount) {
    ObjectHeader* hdr; int64_t offset;
    if (!byteObjectReadSetup(*this, memory_, argCount, hdr, offset)) return PrimitiveResult::Failure;
    if (static_cast<size_t>(offset) + 4 > hdr->byteSize()) return PrimitiveResult::Failure;
    uint32_t val;
    memcpy(&val, hdr->bytes() + offset, sizeof(uint32_t));
    popN(2);
    push(Oop::fromCharacter(val));
    return PrimitiveResult::Success;
}

// --- Primitives 615-627: ByteArray writes ---
// Write primitives check immutability and send attemptToAssign:withIndex: if read-only.

// Primitive 615: boolean8AtOffset:put: — write 8-bit boolean into byte object
PrimitiveResult Interpreter::primitiveBytesBoolean8Write(int argCount) {
    ObjectHeader* hdr; int64_t offset; Oop rcvr, valueOop;
    if (!byteObjectWriteSetup(*this, memory_, argCount, hdr, offset, rcvr, valueOop))
        return PrimitiveResult::Failure;
    if (hdr->isImmutable()) { popN(3); push(rcvr); push(valueOop); push(Oop::fromSmallInteger(offset + 1));
        sendSelector(memory_.specialObject(SpecialObjectIndex::SelectorAttemptToAssign), 2);
        return PrimitiveResult::Success; }
    if (static_cast<size_t>(offset) + 1 > hdr->byteSize()) return PrimitiveResult::Failure;
    uint8_t val = (valueOop.rawBits() == memory_.trueObject().rawBits()) ? 1 : 0;
    hdr->bytes()[offset] = val;
    popN(3); push(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 616: uint8AtOffset:put: — write unsigned 8-bit into byte object
PrimitiveResult Interpreter::primitiveBytesUint8Write(int argCount) {
    ObjectHeader* hdr; int64_t offset; Oop rcvr, valueOop;
    if (!byteObjectWriteSetup(*this, memory_, argCount, hdr, offset, rcvr, valueOop))
        return PrimitiveResult::Failure;
    if (hdr->isImmutable()) { popN(3); push(rcvr); push(valueOop); push(Oop::fromSmallInteger(offset + 1));
        sendSelector(memory_.specialObject(SpecialObjectIndex::SelectorAttemptToAssign), 2);
        return PrimitiveResult::Success; }
    if (static_cast<size_t>(offset) + 1 > hdr->byteSize()) return PrimitiveResult::Failure;
    if (!valueOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t v = valueOop.asSmallInteger();
    if (v < 0 || v > 255) return PrimitiveResult::Failure;
    hdr->bytes()[offset] = static_cast<uint8_t>(v);
    popN(3); push(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 617: int8AtOffset:put: — write signed 8-bit into byte object
PrimitiveResult Interpreter::primitiveBytesInt8Write(int argCount) {
    ObjectHeader* hdr; int64_t offset; Oop rcvr, valueOop;
    if (!byteObjectWriteSetup(*this, memory_, argCount, hdr, offset, rcvr, valueOop))
        return PrimitiveResult::Failure;
    if (hdr->isImmutable()) { popN(3); push(rcvr); push(valueOop); push(Oop::fromSmallInteger(offset + 1));
        sendSelector(memory_.specialObject(SpecialObjectIndex::SelectorAttemptToAssign), 2);
        return PrimitiveResult::Success; }
    if (static_cast<size_t>(offset) + 1 > hdr->byteSize()) return PrimitiveResult::Failure;
    if (!valueOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t v = valueOop.asSmallInteger();
    if (v < -128 || v > 127) return PrimitiveResult::Failure;
    int8_t val = static_cast<int8_t>(v);
    memcpy(hdr->bytes() + offset, &val, 1);
    popN(3); push(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 618: uint16AtOffset:put: — write unsigned 16-bit into byte object
PrimitiveResult Interpreter::primitiveBytesUint16Write(int argCount) {
    ObjectHeader* hdr; int64_t offset; Oop rcvr, valueOop;
    if (!byteObjectWriteSetup(*this, memory_, argCount, hdr, offset, rcvr, valueOop))
        return PrimitiveResult::Failure;
    if (hdr->isImmutable()) { popN(3); push(rcvr); push(valueOop); push(Oop::fromSmallInteger(offset + 1));
        sendSelector(memory_.specialObject(SpecialObjectIndex::SelectorAttemptToAssign), 2);
        return PrimitiveResult::Success; }
    if (static_cast<size_t>(offset) + 2 > hdr->byteSize()) return PrimitiveResult::Failure;
    if (!valueOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t v = valueOop.asSmallInteger();
    if (v < 0 || v > 65535) return PrimitiveResult::Failure;
    uint16_t val = static_cast<uint16_t>(v);
    memcpy(hdr->bytes() + offset, &val, sizeof(uint16_t));
    popN(3); push(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 619: int16AtOffset:put: — write signed 16-bit into byte object
PrimitiveResult Interpreter::primitiveBytesInt16Write(int argCount) {
    ObjectHeader* hdr; int64_t offset; Oop rcvr, valueOop;
    if (!byteObjectWriteSetup(*this, memory_, argCount, hdr, offset, rcvr, valueOop))
        return PrimitiveResult::Failure;
    if (hdr->isImmutable()) { popN(3); push(rcvr); push(valueOop); push(Oop::fromSmallInteger(offset + 1));
        sendSelector(memory_.specialObject(SpecialObjectIndex::SelectorAttemptToAssign), 2);
        return PrimitiveResult::Success; }
    if (static_cast<size_t>(offset) + 2 > hdr->byteSize()) return PrimitiveResult::Failure;
    if (!valueOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t v = valueOop.asSmallInteger();
    if (v < -32768 || v > 32767) return PrimitiveResult::Failure;
    int16_t val = static_cast<int16_t>(v);
    memcpy(hdr->bytes() + offset, &val, sizeof(int16_t));
    popN(3); push(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 620: uint32AtOffset:put: — write unsigned 32-bit into byte object
PrimitiveResult Interpreter::primitiveBytesUint32Write(int argCount) {
    ObjectHeader* hdr; int64_t offset; Oop rcvr, valueOop;
    if (!byteObjectWriteSetup(*this, memory_, argCount, hdr, offset, rcvr, valueOop))
        return PrimitiveResult::Failure;
    if (hdr->isImmutable()) { popN(3); push(rcvr); push(valueOop); push(Oop::fromSmallInteger(offset + 1));
        sendSelector(memory_.specialObject(SpecialObjectIndex::SelectorAttemptToAssign), 2);
        return PrimitiveResult::Success; }
    if (static_cast<size_t>(offset) + 4 > hdr->byteSize()) return PrimitiveResult::Failure;
    if (!valueOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t v = valueOop.asSmallInteger();
    if (v < 0 || v > 4294967295LL) return PrimitiveResult::Failure;
    uint32_t val = static_cast<uint32_t>(v);
    memcpy(hdr->bytes() + offset, &val, sizeof(uint32_t));
    popN(3); push(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 621: int32AtOffset:put: — write signed 32-bit into byte object
PrimitiveResult Interpreter::primitiveBytesInt32Write(int argCount) {
    ObjectHeader* hdr; int64_t offset; Oop rcvr, valueOop;
    if (!byteObjectWriteSetup(*this, memory_, argCount, hdr, offset, rcvr, valueOop))
        return PrimitiveResult::Failure;
    if (hdr->isImmutable()) { popN(3); push(rcvr); push(valueOop); push(Oop::fromSmallInteger(offset + 1));
        sendSelector(memory_.specialObject(SpecialObjectIndex::SelectorAttemptToAssign), 2);
        return PrimitiveResult::Success; }
    if (static_cast<size_t>(offset) + 4 > hdr->byteSize()) return PrimitiveResult::Failure;
    if (!valueOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t v = valueOop.asSmallInteger();
    if (v < INT32_MIN || v > INT32_MAX) return PrimitiveResult::Failure;
    int32_t val = static_cast<int32_t>(v);
    memcpy(hdr->bytes() + offset, &val, sizeof(int32_t));
    popN(3); push(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 622: uint64AtOffset:put: — write unsigned 64-bit into byte object
PrimitiveResult Interpreter::primitiveBytesUint64Write(int argCount) {
    ObjectHeader* hdr; int64_t offset; Oop rcvr, valueOop;
    if (!byteObjectWriteSetup(*this, memory_, argCount, hdr, offset, rcvr, valueOop))
        return PrimitiveResult::Failure;
    if (hdr->isImmutable()) { popN(3); push(rcvr); push(valueOop); push(Oop::fromSmallInteger(offset + 1));
        sendSelector(memory_.specialObject(SpecialObjectIndex::SelectorAttemptToAssign), 2);
        return PrimitiveResult::Success; }
    if (static_cast<size_t>(offset) + 8 > hdr->byteSize()) return PrimitiveResult::Failure;
    if (!valueOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t v = valueOop.asSmallInteger();
    if (v < 0) return PrimitiveResult::Failure;
    uint64_t val = static_cast<uint64_t>(v);
    memcpy(hdr->bytes() + offset, &val, sizeof(uint64_t));
    popN(3); push(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 623: int64AtOffset:put: — write signed 64-bit into byte object
PrimitiveResult Interpreter::primitiveBytesInt64Write(int argCount) {
    ObjectHeader* hdr; int64_t offset; Oop rcvr, valueOop;
    if (!byteObjectWriteSetup(*this, memory_, argCount, hdr, offset, rcvr, valueOop))
        return PrimitiveResult::Failure;
    if (hdr->isImmutable()) { popN(3); push(rcvr); push(valueOop); push(Oop::fromSmallInteger(offset + 1));
        sendSelector(memory_.specialObject(SpecialObjectIndex::SelectorAttemptToAssign), 2);
        return PrimitiveResult::Success; }
    if (static_cast<size_t>(offset) + 8 > hdr->byteSize()) return PrimitiveResult::Failure;
    if (!valueOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t val = valueOop.asSmallInteger();
    memcpy(hdr->bytes() + offset, &val, sizeof(int64_t));
    popN(3); push(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 624: pointerAtOffset:put: — write pointer into byte object
PrimitiveResult Interpreter::primitiveBytesPointerWrite(int argCount) {
    ObjectHeader* hdr; int64_t offset; Oop rcvr, valueOop;
    if (!byteObjectWriteSetup(*this, memory_, argCount, hdr, offset, rcvr, valueOop))
        return PrimitiveResult::Failure;
    if (hdr->isImmutable()) { popN(3); push(rcvr); push(valueOop); push(Oop::fromSmallInteger(offset + 1));
        sendSelector(memory_.specialObject(SpecialObjectIndex::SelectorAttemptToAssign), 2);
        return PrimitiveResult::Success; }
    if (static_cast<size_t>(offset) + sizeof(void*) > hdr->byteSize()) return PrimitiveResult::Failure;
    // Value must be an ExternalAddress (byte object containing a pointer)
    if (!valueOop.isObject()) return PrimitiveResult::Failure;
    ObjectHeader* valHdr = valueOop.asObjectPtr();
    if (!valHdr->isBytesObject() || valHdr->byteSize() < sizeof(void*)) return PrimitiveResult::Failure;
    void* ptr;
    memcpy(&ptr, valHdr->bytes(), sizeof(void*));
    memcpy(hdr->bytes() + offset, &ptr, sizeof(void*));
    popN(3); push(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 625: char8AtOffset:put: — write 8-bit character into byte object
PrimitiveResult Interpreter::primitiveBytesChar8Write(int argCount) {
    ObjectHeader* hdr; int64_t offset; Oop rcvr, valueOop;
    if (!byteObjectWriteSetup(*this, memory_, argCount, hdr, offset, rcvr, valueOop))
        return PrimitiveResult::Failure;
    if (hdr->isImmutable()) { popN(3); push(rcvr); push(valueOop); push(Oop::fromSmallInteger(offset + 1));
        sendSelector(memory_.specialObject(SpecialObjectIndex::SelectorAttemptToAssign), 2);
        return PrimitiveResult::Success; }
    if (static_cast<size_t>(offset) + 1 > hdr->byteSize()) return PrimitiveResult::Failure;
    uint32_t charVal;
    if (valueOop.isCharacter()) charVal = valueOop.asCharacter();
    else if (valueOop.isSmallInteger()) charVal = static_cast<uint32_t>(valueOop.asSmallInteger());
    else return PrimitiveResult::Failure;
    if (charVal > 255) return PrimitiveResult::Failure;
    hdr->bytes()[offset] = static_cast<uint8_t>(charVal);
    popN(3); push(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 626: char16AtOffset:put: — write 16-bit character into byte object
PrimitiveResult Interpreter::primitiveBytesChar16Write(int argCount) {
    ObjectHeader* hdr; int64_t offset; Oop rcvr, valueOop;
    if (!byteObjectWriteSetup(*this, memory_, argCount, hdr, offset, rcvr, valueOop))
        return PrimitiveResult::Failure;
    if (hdr->isImmutable()) { popN(3); push(rcvr); push(valueOop); push(Oop::fromSmallInteger(offset + 1));
        sendSelector(memory_.specialObject(SpecialObjectIndex::SelectorAttemptToAssign), 2);
        return PrimitiveResult::Success; }
    if (static_cast<size_t>(offset) + 2 > hdr->byteSize()) return PrimitiveResult::Failure;
    uint32_t charVal;
    if (valueOop.isCharacter()) charVal = valueOop.asCharacter();
    else if (valueOop.isSmallInteger()) charVal = static_cast<uint32_t>(valueOop.asSmallInteger());
    else return PrimitiveResult::Failure;
    if (charVal > 65535) return PrimitiveResult::Failure;
    uint16_t val = static_cast<uint16_t>(charVal);
    memcpy(hdr->bytes() + offset, &val, sizeof(uint16_t));
    popN(3); push(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 627: char32AtOffset:put: — write 32-bit character into byte object
PrimitiveResult Interpreter::primitiveBytesChar32Write(int argCount) {
    ObjectHeader* hdr; int64_t offset; Oop rcvr, valueOop;
    if (!byteObjectWriteSetup(*this, memory_, argCount, hdr, offset, rcvr, valueOop))
        return PrimitiveResult::Failure;
    if (hdr->isImmutable()) { popN(3); push(rcvr); push(valueOop); push(Oop::fromSmallInteger(offset + 1));
        sendSelector(memory_.specialObject(SpecialObjectIndex::SelectorAttemptToAssign), 2);
        return PrimitiveResult::Success; }
    if (static_cast<size_t>(offset) + 4 > hdr->byteSize()) return PrimitiveResult::Failure;
    uint32_t charVal;
    if (valueOop.isCharacter()) charVal = valueOop.asCharacter();
    else if (valueOop.isSmallInteger()) charVal = static_cast<uint32_t>(valueOop.asSmallInteger());
    else return PrimitiveResult::Failure;
    memcpy(hdr->bytes() + offset, &charVal, sizeof(uint32_t));
    popN(3); push(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 613: float32AtOffset: — read 32-bit float from byte object
PrimitiveResult Interpreter::primitiveFloat32Read(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;
    Oop offsetOop = stackValue(0);
    Oop rcvr = stackValue(1);

    if (!offsetOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t offset = offsetOop.asSmallInteger();
    if (offset < 0) return PrimitiveResult::Failure;

    if (!rcvr.isObject()) return PrimitiveResult::Failure;
    ObjectHeader* hdr = rcvr.asObjectPtr();
    if (!hdr->isBytesObject()) return PrimitiveResult::Failure;

    if (static_cast<size_t>(offset) + sizeof(float) > hdr->byteSize())
        return PrimitiveResult::Failure;

    float fval;
    memcpy(&fval, hdr->bytes() + offset, sizeof(float));

    // makeFloat may allocate — stack values may be forwarded by GC but we don't need them
    Oop result = makeFloat(memory_, static_cast<double>(fval));
    if (result.isNil()) return PrimitiveResult::Failure;

    popN(2);
    push(result);
    return PrimitiveResult::Success;
}

// Primitive 614: float64AtOffset: — read 64-bit double from byte object
PrimitiveResult Interpreter::primitiveFloat64Read(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;
    Oop offsetOop = stackValue(0);
    Oop rcvr = stackValue(1);

    if (!offsetOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t offset = offsetOop.asSmallInteger();
    if (offset < 0) return PrimitiveResult::Failure;

    if (!rcvr.isObject()) return PrimitiveResult::Failure;
    ObjectHeader* hdr = rcvr.asObjectPtr();
    if (!hdr->isBytesObject()) return PrimitiveResult::Failure;

    if (static_cast<size_t>(offset) + sizeof(double) > hdr->byteSize())
        return PrimitiveResult::Failure;

    double dval;
    memcpy(&dval, hdr->bytes() + offset, sizeof(double));

    Oop result = makeFloat(memory_, dval);
    if (result.isNil()) return PrimitiveResult::Failure;

    popN(2);
    push(result);
    return PrimitiveResult::Success;
}

// Primitive 628: float32AtOffset:put: — write 32-bit float into byte object
PrimitiveResult Interpreter::primitiveFloat32Write(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;
    Oop valueOop = stackValue(0);
    Oop offsetOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!rcvr.isObject()) return PrimitiveResult::Failure;
    ObjectHeader* hdr = rcvr.asObjectPtr();
    if (!hdr->isBytesObject()) return PrimitiveResult::Failure;

    if (!offsetOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t offset = offsetOop.asSmallInteger();
    if (offset < 0) return PrimitiveResult::Failure;

    // Check immutability — signal attemptToAssign:withIndex: via Smalltalk
    if (hdr->isImmutable()) {
        popN(3);
        push(rcvr);
        push(valueOop);
        push(Oop::fromSmallInteger(offset + 1));  // 1-based index
        Oop selector = memory_.specialObject(SpecialObjectIndex::SelectorAttemptToAssign);
        sendSelector(selector, 2);
        return PrimitiveResult::Success;
    }

    if (static_cast<size_t>(offset) + sizeof(float) > hdr->byteSize())
        return PrimitiveResult::Failure;

    double dval;
    if (!extractFloat(memory_, valueOop, dval)) return PrimitiveResult::Failure;

    float fval = static_cast<float>(dval);
    memcpy(hdr->bytes() + offset, &fval, sizeof(float));

    popN(3);
    push(valueOop);
    return PrimitiveResult::Success;
}

// Primitive 629: float64AtOffset:put: — write 64-bit double into byte object
PrimitiveResult Interpreter::primitiveFloat64Write(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;
    Oop valueOop = stackValue(0);
    Oop offsetOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!rcvr.isObject()) return PrimitiveResult::Failure;
    ObjectHeader* hdr = rcvr.asObjectPtr();
    if (!hdr->isBytesObject()) return PrimitiveResult::Failure;

    if (!offsetOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t offset = offsetOop.asSmallInteger();
    if (offset < 0) return PrimitiveResult::Failure;

    // Check immutability — signal attemptToAssign:withIndex: via Smalltalk
    if (hdr->isImmutable()) {
        popN(3);
        push(rcvr);
        push(valueOop);
        push(Oop::fromSmallInteger(offset + 1));  // 1-based index
        Oop selector = memory_.specialObject(SpecialObjectIndex::SelectorAttemptToAssign);
        sendSelector(selector, 2);
        return PrimitiveResult::Success;
    }

    if (static_cast<size_t>(offset) + sizeof(double) > hdr->byteSize())
        return PrimitiveResult::Failure;

    double dval;
    if (!extractFloat(memory_, valueOop, dval)) return PrimitiveResult::Failure;

    memcpy(hdr->bytes() + offset, &dval, sizeof(double));

    popN(3);
    push(valueOop);
    return PrimitiveResult::Success;
}

// ===== ExternalAddress read primitives (631-639) =====
// These read from EXTERNAL MEMORY pointed to by an ExternalAddress.
// The ExternalAddress bytes contain a pointer value; these primitives
// dereference that pointer + offset to access the external data.

// Helper: get stored pointer and offset from ExternalAddress + offset arg
static bool externalAddressReadSetup(pharo::Interpreter& interp, pharo::ObjectMemory& mem,
                                      int argCount, void*& basePtr, int64_t& offset) {
    if (argCount != 1) return false;
    Oop offsetOop = interp.stackTop();
    if (!offsetOop.isSmallInteger()) return false;
    offset = offsetOop.asSmallInteger();
    if (offset < 0) return false;

    Oop receiver = interp.stackValue(1);
    if (!receiver.isObject()) return false;
    ObjectHeader* hdr = receiver.asObjectPtr();
    if (!hdr->isBytesObject() || hdr->byteSize() < sizeof(void*)) return false;

    memcpy(&basePtr, hdr->bytes(), sizeof(void*));
    return basePtr != nullptr;  // Null pointer -> let Smalltalk fallback handle it
}

// Primitive 631: uint8AtOffset: — read unsigned byte from external memory
PrimitiveResult Interpreter::primitiveExternalUint8Read(int argCount) {
    void* basePtr = nullptr;
    int64_t offset = 0;
    if (!externalAddressReadSetup(*this, memory_, argCount, basePtr, offset))
        return PrimitiveResult::Failure;

    uint8_t value = *reinterpret_cast<uint8_t*>(static_cast<uint8_t*>(basePtr) + offset);
    popN(argCount + 1);
    push(Oop::fromSmallInteger(value));
    return PrimitiveResult::Success;
}

// Primitive 633: uint16AtOffset: — read unsigned 16-bit from external memory
PrimitiveResult Interpreter::primitiveExternalUint16Read(int argCount) {
    void* basePtr = nullptr;
    int64_t offset = 0;
    if (!externalAddressReadSetup(*this, memory_, argCount, basePtr, offset))
        return PrimitiveResult::Failure;

    uint16_t value;
    memcpy(&value, static_cast<uint8_t*>(basePtr) + offset, sizeof(uint16_t));
    popN(argCount + 1);
    push(Oop::fromSmallInteger(value));
    return PrimitiveResult::Success;
}

// Primitive 635: uint32AtOffset: — read unsigned 32-bit from external memory
PrimitiveResult Interpreter::primitiveExternalUint32Read(int argCount) {
    void* basePtr = nullptr;
    int64_t offset = 0;
    if (!externalAddressReadSetup(*this, memory_, argCount, basePtr, offset))
        return PrimitiveResult::Failure;

    uint32_t value;
    memcpy(&value, static_cast<uint8_t*>(basePtr) + offset, sizeof(uint32_t));
    popN(argCount + 1);
    push(Oop::fromSmallInteger(static_cast<int64_t>(value)));
    return PrimitiveResult::Success;
}

// Primitive 636: int32AtOffset: — read signed 32-bit from external memory
PrimitiveResult Interpreter::primitiveExternalInt32Read(int argCount) {
    void* basePtr = nullptr;
    int64_t offset = 0;
    if (!externalAddressReadSetup(*this, memory_, argCount, basePtr, offset))
        return PrimitiveResult::Failure;

    int32_t value;
    memcpy(&value, static_cast<uint8_t*>(basePtr) + offset, sizeof(int32_t));
    popN(argCount + 1);
    push(Oop::fromSmallInteger(static_cast<int64_t>(value)));
    return PrimitiveResult::Success;
}

// Primitive 639: pointerAtOffset: — read pointer from external memory
PrimitiveResult Interpreter::primitiveExternalPointerRead(int argCount) {
    void* basePtr = nullptr;
    int64_t offset = 0;
    if (!externalAddressReadSetup(*this, memory_, argCount, basePtr, offset)) {
        return PrimitiveResult::Failure;
    }

    // pointerAtOffset: uses 0-based offset — use directly
    void* value = nullptr;
    memcpy(&value, static_cast<uint8_t*>(basePtr) + offset, sizeof(void*));

    Oop result = tffi_newExternalAddress(value);
    if (result.isNil()) return PrimitiveResult::Failure;

    popN(argCount + 1);
    push(result);
    return PrimitiveResult::Success;
}

// ExternalAddress WRITE primitives (646-654)
// These write to EXTERNAL MEMORY pointed to by an ExternalAddress.
// Stack: receiver (ExternalAddress), zeroBasedOffset, value -> value

static bool externalAddressWriteSetup(pharo::Interpreter& interp, pharo::ObjectMemory& mem,
                                       int argCount, void*& basePtr, int64_t& offset) {
    if (argCount != 2) return false;
    Oop offsetOop = interp.stackValue(1);
    if (!offsetOop.isSmallInteger()) return false;
    offset = offsetOop.asSmallInteger();
    if (offset < 0) return false;
    Oop receiver = interp.stackValue(2);
    if (!receiver.isObject()) return false;
    ObjectHeader* hdr = receiver.asObjectPtr();
    if (!hdr->isBytesObject() || hdr->byteSize() < sizeof(void*)) return false;
    memcpy(&basePtr, hdr->bytes(), sizeof(void*));
    return basePtr != nullptr;
}

// Primitive 646: uint8AtOffset:put:
PrimitiveResult Interpreter::primitiveExternalUint8Write(int argCount) {
    void* basePtr = nullptr;
    int64_t offset = 0;
    if (!externalAddressWriteSetup(*this, memory_, argCount, basePtr, offset))
        return PrimitiveResult::Failure;
    Oop valueOop = stackTop();
    if (!valueOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t v = valueOop.asSmallInteger();
    if (v < 0 || v > 255) return PrimitiveResult::Failure;
    *reinterpret_cast<uint8_t*>(static_cast<uint8_t*>(basePtr) + offset) = static_cast<uint8_t>(v);
    Oop result = valueOop;
    popN(argCount + 1);
    push(result);
    return PrimitiveResult::Success;
}

// Primitive 648: uint16AtOffset:put:
PrimitiveResult Interpreter::primitiveExternalUint16Write(int argCount) {
    void* basePtr = nullptr;
    int64_t offset = 0;
    if (!externalAddressWriteSetup(*this, memory_, argCount, basePtr, offset))
        return PrimitiveResult::Failure;
    Oop valueOop = stackTop();
    if (!valueOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t v = valueOop.asSmallInteger();
    if (v < 0 || v > 65535) return PrimitiveResult::Failure;
    uint16_t val16 = static_cast<uint16_t>(v);
    memcpy(static_cast<uint8_t*>(basePtr) + offset, &val16, 2);
    Oop result = valueOop;
    popN(argCount + 1);
    push(result);
    return PrimitiveResult::Success;
}

// Primitive 650: uint32AtOffset:put:
PrimitiveResult Interpreter::primitiveExternalUint32Write(int argCount) {
    void* basePtr = nullptr;
    int64_t offset = 0;
    if (!externalAddressWriteSetup(*this, memory_, argCount, basePtr, offset))
        return PrimitiveResult::Failure;
    Oop valueOop = stackTop();
    if (!valueOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t v = valueOop.asSmallInteger();
    if (v < 0) return PrimitiveResult::Failure;
    uint32_t val32 = static_cast<uint32_t>(v);
    memcpy(static_cast<uint8_t*>(basePtr) + offset, &val32, 4);
    Oop result = valueOop;
    popN(argCount + 1);
    push(result);
    return PrimitiveResult::Success;
}

// Primitive 651: int32AtOffset:put:
PrimitiveResult Interpreter::primitiveExternalInt32Write(int argCount) {
    void* basePtr = nullptr;
    int64_t offset = 0;
    if (!externalAddressWriteSetup(*this, memory_, argCount, basePtr, offset))
        return PrimitiveResult::Failure;
    Oop valueOop = stackTop();
    if (!valueOop.isSmallInteger()) return PrimitiveResult::Failure;
    int32_t val32 = static_cast<int32_t>(valueOop.asSmallInteger());
    memcpy(static_cast<uint8_t*>(basePtr) + offset, &val32, 4);
    Oop result = valueOop;
    popN(argCount + 1);
    push(result);
    return PrimitiveResult::Success;
}

// Primitive 652: uint64AtOffset:put:
PrimitiveResult Interpreter::primitiveExternalUint64Write(int argCount) {
    void* basePtr = nullptr;
    int64_t offset = 0;
    if (!externalAddressWriteSetup(*this, memory_, argCount, basePtr, offset))
        return PrimitiveResult::Failure;
    Oop valueOop = stackTop();
    uint64_t val64 = 0;
    if (valueOop.isSmallInteger()) {
        int64_t v = valueOop.asSmallInteger();
        if (v < 0) return PrimitiveResult::Failure;
        val64 = static_cast<uint64_t>(v);
    } else if (valueOop.isObject()) {
        ObjectHeader* vhdr = valueOop.asObjectPtr();
        if (!vhdr->isBytesObject()) return PrimitiveResult::Failure;
        size_t bsz = vhdr->byteSize();
        if (bsz > 8) return PrimitiveResult::Failure;
        memcpy(&val64, vhdr->bytes(), bsz);
    } else {
        return PrimitiveResult::Failure;
    }
    memcpy(static_cast<uint8_t*>(basePtr) + offset, &val64, 8);
    Oop result = valueOop;
    popN(argCount + 1);
    push(result);
    return PrimitiveResult::Success;
}

// Primitive 654: pointerAtOffset:put:
PrimitiveResult Interpreter::primitiveExternalPointerWrite(int argCount) {
    void* basePtr = nullptr;
    int64_t offset = 0;
    if (!externalAddressWriteSetup(*this, memory_, argCount, basePtr, offset))
        return PrimitiveResult::Failure;
    Oop valueOop = stackTop();
    void* ptrVal = nullptr;
    if (valueOop.isNil() || valueOop.rawBits() == memory_.nil().rawBits()) {
        ptrVal = nullptr;
    } else if (valueOop.isObject()) {
        ObjectHeader* vhdr = valueOop.asObjectPtr();
        if (vhdr->isBytesObject() && vhdr->byteSize() >= sizeof(void*)) {
            memcpy(&ptrVal, vhdr->bytes(), sizeof(void*));
        }
    } else if (valueOop.isSmallInteger()) {
        ptrVal = reinterpret_cast<void*>(static_cast<uintptr_t>(valueOop.asSmallInteger()));
    }
    memcpy(static_cast<uint8_t*>(basePtr) + offset, &ptrVal, sizeof(void*));

    Oop result = valueOop;
    popN(argCount + 1);
    push(result);
    return PrimitiveResult::Success;
}

// primitiveFFIIntegerAt
// Stack: receiver, anObject, byteOffset, nBytes, signed -> integer
// Reads an integer of nBytes size from external memory at byteOffset
PrimitiveResult Interpreter::primitiveFFIIntegerAt(int argCount) {
    if (argCount != 4) return PrimitiveResult::Failure;

    Oop signedOop = stackTop();
    Oop nBytesOop = stackValue(1);
    Oop offsetOop = stackValue(2);
    Oop objectOop = stackValue(3);

    // Parse signed flag
    bool isSigned = (signedOop.rawBits() == memory_.trueObject().rawBits());

    // Parse nBytes
    if (!nBytesOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t nBytes = nBytesOop.asSmallInteger();
    if (nBytes < 1 || nBytes > 8) return PrimitiveResult::Failure;

    // Parse offset
    if (!offsetOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t offset = offsetOop.asSmallInteger();
    if (offset < 1) return PrimitiveResult::Failure;
    offset -= 1;  // Convert 1-based to 0-based

    // Get the data pointer - depends on object class
    uint8_t* ptr = nullptr;
    size_t availBytes = 0;
    if (objectOop.isObject()) {
        ObjectHeader* objHdr = objectOop.asObjectPtr();
        if (objHdr->isBytesObject()) {
            // Check if this is an ExternalAddress (dereference pointer to external memory)
            // or a ByteArray/other bytes object (read from object's own bytes)
            Oop eaClass = memory_.specialObject(SpecialObjectIndex::ClassExternalAddress);
            bool isEA = eaClass.isObject() &&
                        memory_.classOf(objectOop).rawBits() == eaClass.rawBits();
            if (isEA) {
                // ExternalAddress: bytes contain a pointer, dereference it
                void* basePtr = nullptr;
                if (objHdr->byteSize() >= sizeof(void*)) {
                    memcpy(&basePtr, objHdr->bytes(), sizeof(void*));
                }
                if (!basePtr) return PrimitiveResult::Failure;
                ptr = reinterpret_cast<uint8_t*>(basePtr) + offset;
                availBytes = SIZE_MAX; // External memory, we trust the caller
            } else {
                // ByteArray or other bytes object: read from own bytes
                size_t objSize = objHdr->byteSize();
                if (offset + nBytes > static_cast<int64_t>(objSize))
                    return PrimitiveResult::Failure;
                ptr = objHdr->bytes() + offset;
                availBytes = objSize - offset;
            }
        }
    }

    if (!ptr) return PrimitiveResult::Failure;
    int64_t value = 0;

    switch (nBytes) {
        case 1:
            if (isSigned) {
                value = *reinterpret_cast<int8_t*>(ptr);
            } else {
                value = *reinterpret_cast<uint8_t*>(ptr);
            }
            break;
        case 2:
            if (isSigned) {
                value = *reinterpret_cast<int16_t*>(ptr);
            } else {
                value = *reinterpret_cast<uint16_t*>(ptr);
            }
            break;
        case 4:
            if (isSigned) {
                value = *reinterpret_cast<int32_t*>(ptr);
            } else {
                value = *reinterpret_cast<uint32_t*>(ptr);
            }
            break;
        case 8:
            if (isSigned) {
                value = *reinterpret_cast<int64_t*>(ptr);
            } else {
                // For unsigned 64-bit, we still read as signed since
                // Smalltalk integers are signed, but very large values
                // may not fit in SmallInteger
                value = static_cast<int64_t>(*reinterpret_cast<uint64_t*>(ptr));
            }
            break;
        default:
            return PrimitiveResult::Failure;
    }

    // For unsigned 64-bit, handle as unsigned magnitude
    if (!isSigned && nBytes == 8) {
        uint64_t uvalue = *reinterpret_cast<uint64_t*>(ptr);
        if (uvalue <= static_cast<uint64_t>(Oop::smallIntegerMax())) {
            popN(5);
            push(Oop::fromSmallInteger(static_cast<int64_t>(uvalue)));
            return PrimitiveResult::Success;
        }
        // Create LargePositiveInteger
        std::vector<uint8_t> mag;
        uint64_t tmp = uvalue;
        while (tmp > 0) {
            mag.push_back(static_cast<uint8_t>(tmp & 0xFF));
            tmp >>= 8;
        }
        if (mag.empty()) mag.push_back(0);
        Oop result = makeLargeInteger(memory_, mag, false);
        if (result.isNil()) return PrimitiveResult::Failure;
        popN(5);
        push(result);
        return PrimitiveResult::Success;
    }

    // Signed result (or unsigned < 8 bytes which fits in int64_t)
    if (value >= Oop::smallIntegerMin() && value <= Oop::smallIntegerMax()) {
        popN(5);
        push(Oop::fromSmallInteger(value));
        return PrimitiveResult::Success;
    }

    // Create LargeInteger for large signed values
    bool isNeg = value < 0;
    uint64_t absValue = isNeg ? static_cast<uint64_t>(-value) : static_cast<uint64_t>(value);
    std::vector<uint8_t> mag;
    while (absValue > 0) {
        mag.push_back(static_cast<uint8_t>(absValue & 0xFF));
        absValue >>= 8;
    }
    if (mag.empty()) mag.push_back(0);
    Oop result = makeLargeInteger(memory_, mag, isNeg);
    if (result.isNil()) return PrimitiveResult::Failure;
    popN(5);
    push(result);
    return PrimitiveResult::Success;
}

// primitiveFFIIntegerAtPut
// Stack: receiver, anObject, byteOffset, value, nBytes, signed -> value
// Writes an integer of nBytes size to external memory at byteOffset
PrimitiveResult Interpreter::primitiveFFIIntegerAtPut(int argCount) {
    if (argCount != 5) return PrimitiveResult::Failure;

    Oop signedOop = stackTop();
    Oop nBytesOop = stackValue(1);
    Oop valueOop = stackValue(2);
    Oop offsetOop = stackValue(3);
    Oop objectOop = stackValue(4);

    // Parse nBytes
    if (!nBytesOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t nBytes = nBytesOop.asSmallInteger();
    if (nBytes < 1 || nBytes > 8) return PrimitiveResult::Failure;

    // Parse offset
    if (!offsetOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t offset = offsetOop.asSmallInteger();
    if (offset < 1) return PrimitiveResult::Failure;
    offset -= 1;  // Convert 1-based to 0-based

    // Parse signed flag
    bool isSigned = (signedOop.rawBits() == memory_.trueObject().rawBits());

    // Parse value — accept SmallInteger or LargeInteger
    int64_t signedValue = 0;
    uint64_t unsignedValue = 0;
    if (isSigned) {
        if (!trySigned64BitValueOf(memory_, valueOop, signedValue))
            return PrimitiveResult::Failure;
        // Range check for smaller sizes
        if (nBytes < 8) {
            int64_t max = static_cast<int64_t>(1ULL << (8 * nBytes - 1));
            if (signedValue < -max || signedValue >= max)
                return PrimitiveResult::Failure;
        }
    } else {
        if (!tryUnsigned64BitValueOf(memory_, valueOop, unsignedValue))
            return PrimitiveResult::Failure;
        // Range check for smaller sizes
        if (nBytes < 8) {
            if (unsignedValue >= (1ULL << (8 * nBytes)))
                return PrimitiveResult::Failure;
        }
    }

    // Get the data pointer - depends on object class
    uint8_t* ptr = nullptr;
    if (objectOop.isObject()) {
        ObjectHeader* objHdr = objectOop.asObjectPtr();
        if (objHdr->isBytesObject()) {
            Oop eaClass = memory_.specialObject(SpecialObjectIndex::ClassExternalAddress);
            bool isEA = eaClass.isObject() &&
                        memory_.classOf(objectOop).rawBits() == eaClass.rawBits();
            if (isEA) {
                // ExternalAddress: bytes contain a pointer, dereference it
                void* basePtr = nullptr;
                if (objHdr->byteSize() >= sizeof(void*)) {
                    memcpy(&basePtr, objHdr->bytes(), sizeof(void*));
                }
                if (!basePtr) return PrimitiveResult::Failure;
                ptr = reinterpret_cast<uint8_t*>(basePtr) + offset;
            } else {
                // ByteArray or other bytes object: write to own bytes
                size_t objSize = objHdr->byteSize();
                if (offset + nBytes > static_cast<int64_t>(objSize))
                    return PrimitiveResult::Failure;
                ptr = objHdr->bytes() + offset;
            }
        }
    }

    if (!ptr) return PrimitiveResult::Failure;

    uint64_t rawValue = isSigned ? static_cast<uint64_t>(signedValue) : unsignedValue;
    switch (nBytes) {
        case 1:
            *reinterpret_cast<uint8_t*>(ptr) = static_cast<uint8_t>(rawValue);
            break;
        case 2:
            *reinterpret_cast<uint16_t*>(ptr) = static_cast<uint16_t>(rawValue);
            break;
        case 4:
            *reinterpret_cast<uint32_t*>(ptr) = static_cast<uint32_t>(rawValue);
            break;
        case 8:
            *reinterpret_cast<uint64_t*>(ptr) = rawValue;
            break;
        default:
            return PrimitiveResult::Failure;
    }

    popN(6);  // pop all args and receiver
    push(valueOop);  // return the value
    return PrimitiveResult::Success;
}

// primitiveGetAddressOfOOP
// Stack: receiver, anObject -> Integer
// Returns the address of the object's first data byte as an Integer.
// The reference VM (Cog) returns oop + BaseHeaderSize, so callers expect
// a pointer PAST the 8-byte header to the first indexable/named field.
// Used by PointerUtils >> oopForObject: which wraps the integer in an ExternalAddress.
PrimitiveResult Interpreter::primitiveGetAddressOfOOP(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop objectOop = stackTop();

    if (objectOop.isImmediate()) {
        return PrimitiveResult::Failure;
    }

    // Get the raw pointer to the object header
    ObjectHeader* objectPtr = objectOop.asObjectPtr();

    // Return pointer to first data byte (past the 8-byte header),
    // matching the reference VM's: oop + BaseHeaderSize
    uintptr_t dataAddr = reinterpret_cast<uintptr_t>(objectPtr) + sizeof(ObjectHeader);

    // This fits in a SmallInteger (60-bit range, our addresses are ~34 bits)
    Oop result = Oop::fromSmallInteger(static_cast<int64_t>(dataAddr));

    popN(2);  // pop object and receiver
    push(result);
    return PrimitiveResult::Success;
}

// primitiveInterpreterSourceVersion
// Returns a string with the interpreter source version.
// Format must contain "Date: <ISO8601>" for DiskStore >> checkVMVersion to parse.
PrimitiveResult Interpreter::primitiveInterpreterSourceVersion(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop result = createStringObject(memory_, "iOSPharo VM Date: 2026-02-05T00:00:00+00:00");
    if (result.isNil()) return PrimitiveResult::Failure;

    popN(1);  // pop receiver
    push(result);
    return PrimitiveResult::Success;
}

// primitiveFileMasks
// FileAttributesPlugin>>primitiveFileMasks
// Returns an array of file mask constants used by FileAttributesPlugin
PrimitiveResult Interpreter::primitiveFileMasks(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Get Array class from special objects
    Oop arrayClass = memory_.specialObject(SpecialObjectIndex::ClassArray);
    if (arrayClass.isNil()) return PrimitiveResult::Failure;

    uint32_t classIndex = memory_.indexOfClass(arrayClass);
    if (classIndex == 0) return PrimitiveResult::Failure;

    // Allocate 8-slot array (pointer object, format 2)
    Oop result = memory_.allocateSlots(classIndex, 8, ObjectFormat::Indexable);
    if (result.isNil()) return PrimitiveResult::Failure;

    // S_IFMT, S_IFSOCK, S_IFLNK, S_IFREG, S_IFBLK, S_IFDIR, S_IFCHR, S_IFIFO
    memory_.storePointer(0, result, Oop::fromSmallInteger(0170000));
    memory_.storePointer(1, result, Oop::fromSmallInteger(0140000));
    memory_.storePointer(2, result, Oop::fromSmallInteger(0120000));
    memory_.storePointer(3, result, Oop::fromSmallInteger(0100000));
    memory_.storePointer(4, result, Oop::fromSmallInteger(0060000));
    memory_.storePointer(5, result, Oop::fromSmallInteger(0040000));
    memory_.storePointer(6, result, Oop::fromSmallInteger(0020000));
    memory_.storePointer(7, result, Oop::fromSmallInteger(0010000));

    popN(1);  // pop receiver
    push(result);
    return PrimitiveResult::Success;
}

// primitiveFileAttribute
// FileAttributesPlugin>>primitiveFileAttribute
// Stack: receiver, pathString, attributeNumber -> value
// Attribute numbers: 1=fileName(nil), 2=mode, 3=ino, 4=dev, 5=nlink, 6=uid, 7=gid,
// 8=size, 9=accessDate, 10=modifiedDate, 11=changeDate, 12=creationDate(nil),
// 13=isReadable, 14=isWritable, 15=isExecutable, 16=isSymlink
PrimitiveResult Interpreter::primitiveFileAttribute(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop attrNumOop = stackTop();
    Oop pathOop = stackValue(1);

    if (!attrNumOop.isSmallInteger()) return PrimitiveResult::Failure;
    int attrNum = static_cast<int>(attrNumOop.asSmallInteger());

    if (!pathOop.isObject()) return PrimitiveResult::Failure;
    ObjectHeader* pathHdr = pathOop.asObjectPtr();
    if (!pathHdr->isBytesObject()) return PrimitiveResult::Failure;
    size_t len = memory_.byteSizeOf(pathOop);
    std::string path(reinterpret_cast<const char*>(pathHdr->bytes()), len);

    // Attributes 13-15 use access()
    if (attrNum >= 13 && attrNum <= 15) {
        int mode = (attrNum == 13) ? R_OK : (attrNum == 14) ? W_OK : X_OK;
        bool ok = (access(path.c_str(), mode) == 0);
        popN(3);
        push(ok ? memory_.trueObject() : memory_.falseObject());
        return PrimitiveResult::Success;
    }

    // Attribute 16 uses lstat for symlink check
    if (attrNum == 16) {
        struct stat st;
        bool isSymlink = (lstat(path.c_str(), &st) == 0) && S_ISLNK(st.st_mode);
        popN(3);
        push(isSymlink ? memory_.trueObject() : memory_.falseObject());
        return PrimitiveResult::Success;
    }

    // Attributes 1-12 use stat()
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return PrimitiveResult::Failure;
    }

    Oop result;
    // Squeak epoch offset: seconds from Jan 1 1901 to Jan 1 1970
    static const int64_t squeakEpochDelta = (int64_t)(52*365 + 17*366) * 24 * 60 * 60;

    switch (attrNum) {
        case 1: result = Oop::nil(); break; // fileName - nil for non-symlinks
        case 2: result = int64ToOop(memory_, static_cast<int64_t>(st.st_mode)); break;
        case 3: result = int64ToOop(memory_, static_cast<int64_t>(st.st_ino)); break;
        case 4: result = int64ToOop(memory_, static_cast<int64_t>(st.st_dev)); break;
        case 5: result = int64ToOop(memory_, static_cast<int64_t>(st.st_nlink)); break;
        case 6: result = int64ToOop(memory_, static_cast<int64_t>(st.st_uid)); break;
        case 7: result = int64ToOop(memory_, static_cast<int64_t>(st.st_gid)); break;
        case 8: result = int64ToOop(memory_, static_cast<int64_t>(st.st_size)); break;
        case 9: result = int64ToOop(memory_, static_cast<int64_t>(st.st_atime) + squeakEpochDelta); break;
        case 10: result = int64ToOop(memory_, static_cast<int64_t>(st.st_mtime) + squeakEpochDelta); break;
        case 11: result = int64ToOop(memory_, static_cast<int64_t>(st.st_ctime) + squeakEpochDelta); break;
        case 12: result = Oop::nil(); break; // creationDate - not available on Unix
        default: return PrimitiveResult::Failure;
    }
    if (result.isNil() && attrNum != 1 && attrNum != 12) return PrimitiveResult::Failure;

    popN(3);
    push(result);
    return PrimitiveResult::Success;
}

// primitiveFileExists
// FileAttributesPlugin>>primitiveFileExists
// Stack: receiver, pathString -> boolean
PrimitiveResult Interpreter::primitiveFileExists(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop pathOop = stackTop();
    if (!pathOop.isObject()) return PrimitiveResult::Failure;

    ObjectHeader* pathHdr = pathOop.asObjectPtr();
    if (!pathHdr->isBytesObject()) return PrimitiveResult::Failure;

    size_t len = memory_.byteSizeOf(pathOop);
    std::string path(reinterpret_cast<const char*>(pathHdr->bytes()), len);

    struct stat st;
    bool exists = (stat(path.c_str(), &st) == 0);

    popN(2);  // pop arg + receiver
    push(exists ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

// FileAttributesPlugin>>primitiveOpendir
// Stack: receiver, pathString -> ExternalAddress (DIR* wrapped in bytes object) or nil
PrimitiveResult Interpreter::primitiveOpendir(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop pathOop = stackTop();
    if (!pathOop.isObject()) return PrimitiveResult::Failure;

    ObjectHeader* pathHdr = pathOop.asObjectPtr();
    if (!pathHdr->isBytesObject()) return PrimitiveResult::Failure;

    size_t len = memory_.byteSizeOf(pathOop);
    std::string path(reinterpret_cast<const char*>(pathHdr->bytes()), len);

    DIR* dir = opendir(path.c_str());
    if (!dir) {
        return PrimitiveResult::Failure;
    }

    // Create a ByteArray-like object to hold the DIR* pointer
    // ExternalAddress is a subclass of ByteArray, format 16 (Bytes_0 for 8 bytes aligned)
    Oop byteArrayClass = memory_.specialObject(SpecialObjectIndex::ClassByteArray);
    if (byteArrayClass.isNil()) {
        closedir(dir);
        return PrimitiveResult::Failure;
    }

    uint32_t classIndex = memory_.indexOfClass(byteArrayClass);
    // Allocate 8 bytes to hold a pointer
    Oop result = memory_.allocateBytes(classIndex, sizeof(void*));
    if (result.isNil()) {
        closedir(dir);
        return PrimitiveResult::Failure;
    }

    // Store the DIR* pointer in the bytes
    ObjectHeader* resultHdr = result.asObjectPtr();
    memcpy(resultHdr->bytes(), &dir, sizeof(void*));

    popN(2);  // pop arg + receiver
    push(result);
    return PrimitiveResult::Success;
}

// FileAttributesPlugin>>primitiveReaddir
// Stack: receiver, dirPointerBytes -> Array({filenameByteArray, attributesOrNil}) or nil (end of dir)
// Pharo's DiskStore>>directoryAt:nodesDo: expects a 2+ element Array where:
//   element 1 = filename as ByteArray (UTF-8 encoded)
//   element 2 = symlink attributes Array or nil
PrimitiveResult Interpreter::primitiveReaddir(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop dirOop = stackTop();
    if (!dirOop.isObject()) return PrimitiveResult::Failure;

    ObjectHeader* dirHdr = dirOop.asObjectPtr();
    if (!dirHdr->isBytesObject() || memory_.byteSizeOf(dirOop) < sizeof(void*)) {
        return PrimitiveResult::Failure;
    }

    // Extract the DIR* pointer
    DIR* dir = nullptr;
    memcpy(&dir, dirHdr->bytes(), sizeof(void*));
    if (!dir) return PrimitiveResult::Failure;

    errno = 0;
    struct dirent* entry = readdir(dir);
    if (!entry) {
        // End of directory (or error)
        popN(2);  // pop arg + receiver
        push(memory_.nil());
        return PrimitiveResult::Success;
    }

    // Create ByteArray for the entry name (Pharo expects ByteArray, not String)
    size_t nameLen = strlen(entry->d_name);
    Oop byteArrayClass = memory_.specialObject(SpecialObjectIndex::ClassByteArray);
    if (byteArrayClass.isNil()) return PrimitiveResult::Failure;
    uint32_t baClassIndex = memory_.indexOfClass(byteArrayClass);
    Oop nameBytes = memory_.allocateBytes(baClassIndex, nameLen);
    if (nameBytes.isNil()) return PrimitiveResult::Failure;
    memcpy(nameBytes.asObjectPtr()->bytes(), entry->d_name, nameLen);

    // GC safety: push nameBytes onto stack so GC can update it during next allocation
    push(nameBytes);

    // Create 2-element Array: {filenameByteArray, nil}
    Oop arrayClass = memory_.specialObject(SpecialObjectIndex::ClassArray);
    if (arrayClass.isNil()) { pop(); return PrimitiveResult::Failure; }
    uint32_t arrayClassIndex = memory_.indexOfClass(arrayClass);
    Oop resultArray = memory_.allocateSlots(arrayClassIndex, 2, ObjectFormat::Indexable);
    if (resultArray.isNil()) { pop(); return PrimitiveResult::Failure; }

    // Retrieve GC-safe nameBytes from stack
    nameBytes = stackTop();
    pop();

    memory_.storePointer(0, resultArray, nameBytes);
    memory_.storePointer(1, resultArray, memory_.nil());

    popN(2);  // pop arg + receiver
    push(resultArray);
    return PrimitiveResult::Success;
}

// FileAttributesPlugin>>primitiveClosedir
// Stack: receiver, dirPointerBytes -> receiver
PrimitiveResult Interpreter::primitiveClosedir(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop dirOop = stackTop();
    if (!dirOop.isObject()) return PrimitiveResult::Failure;

    ObjectHeader* dirHdr = dirOop.asObjectPtr();
    if (!dirHdr->isBytesObject() || memory_.byteSizeOf(dirOop) < sizeof(void*)) {
        return PrimitiveResult::Failure;
    }

    // Extract and close the DIR* pointer
    DIR* dir = nullptr;
    memcpy(&dir, dirHdr->bytes(), sizeof(void*));
    if (dir) {
        closedir(dir);
        // Zero out the pointer to prevent double-close
        void* null = nullptr;
        memcpy(dirHdr->bytes(), &null, sizeof(void*));
    }

    pop();  // pop arg, leave receiver
    return PrimitiveResult::Success;
}

// FileAttributesPlugin>>primitiveRewinddir
// Stack: receiver, dirPointerBytes -> dirPointerBytes
PrimitiveResult Interpreter::primitiveRewinddir(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop dirOop = stackTop();
    if (!dirOop.isObject()) return PrimitiveResult::Failure;

    ObjectHeader* dirHdr = dirOop.asObjectPtr();
    if (!dirHdr->isBytesObject() || memory_.byteSizeOf(dirOop) < sizeof(void*)) {
        return PrimitiveResult::Failure;
    }

    DIR* dir = nullptr;
    memcpy(&dir, dirHdr->bytes(), sizeof(void*));
    if (!dir) return PrimitiveResult::Failure;

    rewinddir(dir);

    pop();  // pop arg, leave receiver
    return PrimitiveResult::Success;
}

// primitiveGetenv
// Stack: receiver, nameString -> valueString or nil
PrimitiveResult Interpreter::primitiveGetenv(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop nameOop = stackTop();
    if (!nameOop.isObject()) return PrimitiveResult::Failure;

    ObjectHeader* nameHdr = nameOop.asObjectPtr();
    if (!nameHdr->isBytesObject()) return PrimitiveResult::Failure;

    size_t len = memory_.byteSizeOf(nameOop);
    std::string name(reinterpret_cast<const char*>(nameHdr->bytes()), len);

    const char* value = getenv(name.c_str());

    popN(2);  // pop arg + receiver
    if (value) {
        Oop result = createStringObject(memory_, value);
        push(result.isNil() ? memory_.nil() : result);
    } else {
        push(memory_.nil());
    }
    return PrimitiveResult::Success;
}

// ===== THREADED FFI (TFFI) PRIMITIVES =====
// These implement the VM-side support for Pharo 13's ThreadedFFI.
// The image does all type resolution and marshalling metadata;
// the VM just provides raw ffi_prep_cif / ffi_call wrappers.

// ===== TFFI HELPERS =====

// Read a void* from an ExternalAddress (bytes object containing a pointer)
void* Interpreter::tffi_readAddress(Oop externalAddress) {
    if (!externalAddress.isObject()) return nullptr;
    ObjectHeader* hdr = externalAddress.asObjectPtr();
    if (!hdr->isBytesObject()) return nullptr;
    if (hdr->byteSize() < sizeof(void*)) return nullptr;
    void* ptr = nullptr;
    memcpy(&ptr, hdr->bytes(), sizeof(void*));
    return ptr;
}

// Write a void* into an ExternalAddress (bytes object)
void Interpreter::tffi_writeAddress(Oop externalAddress, void* value) {
    if (!externalAddress.isObject()) return;
    ObjectHeader* hdr = externalAddress.asObjectPtr();
    if (!hdr->isBytesObject()) return;
    if (hdr->byteSize() < sizeof(void*)) return;
    memcpy(hdr->bytes(), &value, sizeof(void*));
}

// Read native pointer from slot 0's ExternalAddress
// Used for TFBasicType, TFFunctionDefinition, TFExternalFunction, etc.
void* Interpreter::tffi_getHandler(Oop obj) {
    if (!obj.isObject()) return nullptr;
    if (memory_.slotCountOf(obj) < 1) return nullptr;
    Oop slot0 = memory_.fetchPointer(0, obj);
    void* result = tffi_readAddress(slot0);

    // Auto-fill TFBasicType objects whose handler hasn't been initialized yet.
    // TFBasicType has >= 3 slots: slot[0]=handler(ExternalAddress), slot[1]=..., slot[2]=typeCode(SmallInt).
    // If handler is null but typeCode is valid, fill it now. This makes FFI callouts
    // work even if TFBasicType class>>initialize hasn't completed yet.
    if (!result && memory_.slotCountOf(obj) >= 3) {
        Oop typeCodeOop = memory_.fetchPointer(2, obj);
        if (typeCodeOop.isSmallInteger()) {
            int64_t typeCode = typeCodeOop.asSmallInteger();
            ffi_type* ffiType = nullptr;
            switch (typeCode) {
                case 1:  ffiType = &ffi_type_void;    break;
                case 2:  ffiType = &ffi_type_float;   break;
                case 3:  ffiType = &ffi_type_double;  break;
                case 4:  ffiType = &ffi_type_uint8;   break;
                case 5:  ffiType = &ffi_type_uint16;  break;
                case 6:  ffiType = &ffi_type_uint32;  break;
                case 7:  ffiType = &ffi_type_uint64;  break;
                case 8:  ffiType = &ffi_type_sint8;   break;
                case 9:  ffiType = &ffi_type_sint16;  break;
                case 10: ffiType = &ffi_type_sint32;  break;
                case 11: ffiType = &ffi_type_sint64;  break;
                case 12: ffiType = &ffi_type_pointer; break;
                case 13: ffiType = &ffi_type_uchar;   break;
                case 14: ffiType = &ffi_type_schar;   break;
                case 15: ffiType = &ffi_type_ushort;  break;
                case 16: ffiType = &ffi_type_sshort;  break;
                case 17: ffiType = &ffi_type_uint;    break;
                case 18: ffiType = &ffi_type_sint;    break;
                case 19: ffiType = &ffi_type_ulong;   break;
                case 20: ffiType = &ffi_type_slong;   break;
                default: break;
            }
            if (ffiType) {
                tffi_setHandler(obj, ffiType);
                result = ffiType;
                static int autoFillCount = 0;
                autoFillCount++;
                if (autoFillCount <= 30) {
                    fprintf(stderr, "[TFFI-AUTOFILL] Auto-filled typeCode=%lld -> ffi_type @%p\n",
                            typeCode, ffiType);
                }
            }
        }
    }

    return result;
}

// Write native pointer into slot 0's ExternalAddress
void Interpreter::tffi_setHandler(Oop obj, void* value) {
    if (!obj.isObject()) return;
    if (memory_.slotCountOf(obj) < 1) return;
    Oop slot0 = memory_.fetchPointer(0, obj);
    tffi_writeAddress(slot0, value);
}

// Allocate a new ExternalAddress containing a pointer value
Oop Interpreter::tffi_newExternalAddress(void* ptr) {
    Oop extAddrClass = memory_.specialObject(SpecialObjectIndex::ClassExternalAddress);
    if (extAddrClass.isNil()) return memory_.nil();
    uint32_t classIndex = memory_.indexOfClass(extAddrClass);
    if (classIndex == 0) return memory_.nil();
    Oop result = memory_.allocateBytes(classIndex, sizeof(void*));
    if (result.isNil()) return result;
    ObjectHeader* hdr = result.asObjectPtr();
    memcpy(hdr->bytes(), &ptr, sizeof(void*));
    return result;
}

// Resolve an Oop to void* from either ExternalAddress or ByteArray
void* Interpreter::tffi_getAddressFromExternalAddressOrByteArray(Oop obj) {
    if (!obj.isObject()) return nullptr;
    ObjectHeader* hdr = obj.asObjectPtr();
    if (!hdr->isBytesObject()) return nullptr;

    // Check if it's an ExternalAddress (contains a pointer to external memory)
    Oop extAddrClass = memory_.specialObject(SpecialObjectIndex::ClassExternalAddress);
    if (memory_.classOf(obj).rawBits() == extAddrClass.rawBits()) {
        return tffi_readAddress(obj);
    }

    // Otherwise treat as ByteArray - return pointer to the bytes themselves
    return hdr->bytes();
}

// ===== TIER 1 PRIMITIVES =====

// primitiveFillBasicType (0 args)
// Receiver: TFBasicType. Read slot 2 as SmallInteger typeCode (1-20),
// map to static ffi_type*, write into receiver.slot[0] ExternalAddress.
PrimitiveResult Interpreter::primitiveFillBasicType(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop receiver = stackTop();
    if (!receiver.isObject()) return PrimitiveResult::Failure;
    if (memory_.slotCountOf(receiver) < 3) return PrimitiveResult::Failure;

    // Read typeCode from slot 2
    Oop typeCodeOop = memory_.fetchPointer(2, receiver);
    if (!typeCodeOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t typeCode = typeCodeOop.asSmallInteger();

    static int fillCount = 0;
    fillCount++;
    if (fillCount <= 30) {
        std::cerr << "[TFFI] primitiveFillBasicType #" << fillCount
                  << " typeCode=" << typeCode << " receiver=0x"
                  << std::hex << receiver.rawBits() << std::dec << "\n";
    }

    // Map typeCode to ffi_type* (Pharo's numbering, NOT libffi's FFI_TYPE_*)
    ffi_type* ffiType = nullptr;
    switch (typeCode) {
        case 1:  ffiType = &ffi_type_void;    break;
        case 2:  ffiType = &ffi_type_float;   break;
        case 3:  ffiType = &ffi_type_double;  break;
        case 4:  ffiType = &ffi_type_uint8;   break;
        case 5:  ffiType = &ffi_type_uint16;  break;
        case 6:  ffiType = &ffi_type_uint32;  break;
        case 7:  ffiType = &ffi_type_uint64;  break;
        case 8:  ffiType = &ffi_type_sint8;   break;
        case 9:  ffiType = &ffi_type_sint16;  break;
        case 10: ffiType = &ffi_type_sint32;  break;
        case 11: ffiType = &ffi_type_sint64;  break;
        case 12: ffiType = &ffi_type_pointer; break;
        case 13: ffiType = &ffi_type_uchar;   break;
        case 14: ffiType = &ffi_type_schar;   break;
        case 15: ffiType = &ffi_type_ushort;  break;
        case 16: ffiType = &ffi_type_sshort;  break;
        case 17: ffiType = &ffi_type_uint;    break;
        case 18: ffiType = &ffi_type_sint;    break;
        case 19: ffiType = &ffi_type_ulong;   break;
        case 20: ffiType = &ffi_type_slong;   break;
        default:
            return PrimitiveResult::Failure;
    }

    // Store ffi_type* into receiver's handler (slot 0's ExternalAddress)
    tffi_setHandler(receiver, ffiType);

    // Leave receiver on stack (pop 0 args + receiver, push receiver back = noop)
    return PrimitiveResult::Success;
}

// primitiveTypeByteSize (0 args)
// Receiver has handler -> ffi_type*. Return type->size as SmallInteger.
PrimitiveResult Interpreter::primitiveTypeByteSize(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop receiver = stackTop();
    ffi_type* type = static_cast<ffi_type*>(tffi_getHandler(receiver));
    if (!type) return PrimitiveResult::Failure;

    int64_t size = static_cast<int64_t>(type->size);
    primitiveSuccess(Oop::fromSmallInteger(size));
    return PrimitiveResult::Success;
}

// primitiveDefineFunction (2-3 args)
// Stack: receiver, paramsArray, returnType [, abi]
// Creates ffi_cif via ffi_prep_cif, stores in receiver's handler
PrimitiveResult Interpreter::primitiveDefineFunction(int argCount) {
    if (argCount < 2 || argCount > 3) return PrimitiveResult::Failure;

    ffi_abi abiToUse = FFI_DEFAULT_ABI;
    int returnTypePos, paramsPos;

    if (argCount == 3) {
        // ABI at stack[0], returnType at stack[1], paramsArray at stack[2]
        Oop abiOop = stackValue(0);
        if (abiOop.isSmallInteger()) {
            abiToUse = static_cast<ffi_abi>(abiOop.asSmallInteger());
        }
        returnTypePos = 1;
        paramsPos = 2;
    } else {
        // returnType at stack[0], paramsArray at stack[1]
        returnTypePos = 0;
        paramsPos = 1;
    }

    Oop returnTypeOop = stackValue(returnTypePos);
    Oop paramsArrayOop = stackValue(paramsPos);
    Oop receiver = stackValue(argCount);

    // Get return type: an ExternalAddress containing ffi_type*
    ffi_type* returnType = static_cast<ffi_type*>(tffi_readAddress(returnTypeOop));
    if (!returnType) return PrimitiveResult::Failure;

    // Get parameter types from array
    if (!paramsArrayOop.isObject()) return PrimitiveResult::Failure;
    size_t paramCount = memory_.slotCountOf(paramsArrayOop);

    // Allocate array of ffi_type* for parameters
    ffi_type** paramTypes = nullptr;
    if (paramCount > 0) {
        paramTypes = static_cast<ffi_type**>(malloc(paramCount * sizeof(ffi_type*)));
        if (!paramTypes) return PrimitiveResult::Failure;

        for (size_t i = 0; i < paramCount; i++) {
            Oop paramOop = memory_.fetchPointer(i, paramsArrayOop);
            ffi_type* pt = static_cast<ffi_type*>(tffi_readAddress(paramOop));
            if (!pt) {
                free(paramTypes);
                return PrimitiveResult::Failure;
            }
            paramTypes[i] = pt;
        }
    }

    // Allocate and fill cif
    ffi_cif* cif = static_cast<ffi_cif*>(malloc(sizeof(ffi_cif)));
    if (!cif) {
        free(paramTypes);
        return PrimitiveResult::Failure;
    }

    ffi_status status = ffi_prep_cif(cif, abiToUse,
                                     static_cast<unsigned int>(paramCount),
                                     returnType, paramTypes);
    if (status != FFI_OK) {
        free(paramTypes);
        free(cif);
        return PrimitiveResult::Failure;
    }

    // Store cif in receiver's handler
    tffi_setHandler(receiver, cif);

    // Pop args, leave receiver
    popN(argCount);
    return PrimitiveResult::Success;
}

// primitiveFreeDefinition (0 args)
// Free cif->arg_types then cif, null out handle
PrimitiveResult Interpreter::primitiveFreeDefinition(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop receiver = stackTop();
    ffi_cif* cif = static_cast<ffi_cif*>(tffi_getHandler(receiver));
    if (!cif) return PrimitiveResult::Failure;

    if (cif->arg_types) free(cif->arg_types);
    free(cif);

    tffi_setHandler(receiver, nullptr);
    return PrimitiveResult::Success;
}

// primitiveDefineVariadicFunction (3-4 args)
// Stack: receiver, paramsArray, returnType, fixedArgCount [, abi]
// Like primitiveDefineFunction but uses ffi_prep_cif_var
PrimitiveResult Interpreter::primitiveDefineVariadicFunction(int argCount) {
    if (argCount < 3 || argCount > 4) return PrimitiveResult::Failure;

    ffi_abi abiToUse = FFI_DEFAULT_ABI;
    int fixedCountPos, returnTypePos, paramsPos;

    if (argCount == 4) {
        Oop abiOop = stackValue(0);
        if (abiOop.isSmallInteger()) {
            abiToUse = static_cast<ffi_abi>(abiOop.asSmallInteger());
        }
        fixedCountPos = 1;
        returnTypePos = 2;
        paramsPos = 3;
    } else {
        fixedCountPos = 0;
        returnTypePos = 1;
        paramsPos = 2;
    }

    Oop fixedCountOop = stackValue(fixedCountPos);
    Oop returnTypeOop = stackValue(returnTypePos);
    Oop paramsArrayOop = stackValue(paramsPos);
    Oop receiver = stackValue(argCount);

    if (!fixedCountOop.isSmallInteger()) return PrimitiveResult::Failure;
    unsigned int fixedArgs = static_cast<unsigned int>(fixedCountOop.asSmallInteger());

    ffi_type* returnType = static_cast<ffi_type*>(tffi_readAddress(returnTypeOop));
    if (!returnType) return PrimitiveResult::Failure;

    if (!paramsArrayOop.isObject()) return PrimitiveResult::Failure;
    size_t paramCount = memory_.slotCountOf(paramsArrayOop);

    ffi_type** paramTypes = nullptr;
    if (paramCount > 0) {
        paramTypes = static_cast<ffi_type**>(malloc(paramCount * sizeof(ffi_type*)));
        if (!paramTypes) return PrimitiveResult::Failure;

        for (size_t i = 0; i < paramCount; i++) {
            Oop paramOop = memory_.fetchPointer(i, paramsArrayOop);
            ffi_type* pt = static_cast<ffi_type*>(tffi_readAddress(paramOop));
            if (!pt) {
                free(paramTypes);
                return PrimitiveResult::Failure;
            }
            paramTypes[i] = pt;
        }
    }

    ffi_cif* cif = static_cast<ffi_cif*>(malloc(sizeof(ffi_cif)));
    if (!cif) {
        free(paramTypes);
        return PrimitiveResult::Failure;
    }

    ffi_status status = ffi_prep_cif_var(cif, abiToUse, fixedArgs,
                                         static_cast<unsigned int>(paramCount),
                                         returnType, paramTypes);
    if (status != FFI_OK) {
        free(paramTypes);
        free(cif);
        return PrimitiveResult::Failure;
    }

    tffi_setHandler(receiver, cif);
    popN(argCount);
    return PrimitiveResult::Success;
}

// primitiveGetSameThreadRunnerAddress (0 args)
// Return new ExternalAddress pointing to a static Runner struct.
// The struct just needs to be non-null; the image only stores the pointer.
PrimitiveResult Interpreter::primitiveGetSameThreadRunnerAddress(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Static runner - just needs to be a non-null unique address
    static struct { void* a; void* b; void* c; void* d; } sameThreadRunner = {nullptr, nullptr, nullptr, nullptr};

    Oop result = tffi_newExternalAddress(&sameThreadRunner);
    if (result.isNil()) return PrimitiveResult::Failure;

    primitiveSuccess(result);
    return PrimitiveResult::Success;
}

// primitiveSameThreadCallout (2 args)
// Stack: receiver, externalFunction, argumentsArray
// This is the core FFI call primitive.
PrimitiveResult Interpreter::primitiveSameThreadCallout(int argCount) {
    if (argCount != 2) {
        return PrimitiveResult::Failure;
    }

    Oop argsArrayOop = stackValue(0);       // arguments array
    Oop externalFuncOop = stackValue(1);    // external function object
    // receiver at stackValue(2) - the runner

    if (!externalFuncOop.isObject()) {
        return PrimitiveResult::Failure;
    }
    if (memory_.slotCountOf(externalFuncOop) < 2) {
        return PrimitiveResult::Failure;
    }

    // Get function pointer from externalFunction.slot[0] (ExternalAddress)
    Oop funcAddrOop = memory_.fetchPointer(0, externalFuncOop);
    void* funcPtr = tffi_readAddress(funcAddrOop);
    if (!funcPtr) {
        return PrimitiveResult::Failure;
    }

    // Get cif from externalFunction.slot[1] (TFFunctionDefinition).slot[0] (ExternalAddress)
    Oop funcDefOop = memory_.fetchPointer(1, externalFuncOop);
    ffi_cif* cif = static_cast<ffi_cif*>(tffi_getHandler(funcDefOop));
    if (!cif) {
        return PrimitiveResult::Failure;
    }

    // Validate arguments array
    if (!argsArrayOop.isObject()) {
        return PrimitiveResult::Failure;
    }
    size_t nargs = memory_.slotCountOf(argsArrayOop);
    if (nargs != cif->nargs) {
        return PrimitiveResult::Failure;
    }

    // Marshall arguments
    void** argPtrs = nullptr;
    uint8_t* argStorage = nullptr;

    if (nargs > 0) {
        argPtrs = static_cast<void**>(alloca(nargs * sizeof(void*)));
        // Allocate generous storage for each argument (max 16 bytes each for alignment)
        argStorage = static_cast<uint8_t*>(alloca(nargs * 16));

        for (size_t i = 0; i < nargs; i++) {
            Oop argOop = memory_.fetchPointer(i, argsArrayOop);
            uint8_t* argSlot = argStorage + (i * 16);
            argPtrs[i] = argSlot;

            unsigned short argTypeId = cif->arg_types[i]->type;

            switch (argTypeId) {
                case FFI_TYPE_POINTER: {
                    void* ptrVal = nullptr;
                    if (argOop.isNil() || argOop.rawBits() == memory_.nil().rawBits()) {
                        // nil → NULL pointer
                        memcpy(argSlot, &ptrVal, sizeof(void*));
                    } else if (argOop.isObject()) {
                        ObjectHeader* argHdr = argOop.asObjectPtr();
                        if (argHdr->isBytesObject()) {
                            Oop extAddrClass = memory_.specialObject(SpecialObjectIndex::ClassExternalAddress);
                            if (memory_.classOf(argOop).rawBits() == extAddrClass.rawBits()) {
                                // ExternalAddress: pass the stored pointer value
                                // (image-side packToArity: handles output parameters)
                                void* storedPtr = tffi_readAddress(argOop);
                                memcpy(argSlot, &storedPtr, sizeof(void*));
                            } else {
                                // ByteArray: pass pointer to the bytes themselves
                                ptrVal = argHdr->bytes();
                                memcpy(argSlot, &ptrVal, sizeof(void*));
                            }
                        } else {
                            memcpy(argSlot, &ptrVal, sizeof(void*));
                        }
                    } else if (argOop.isSmallInteger()) {
                        // SmallInteger as pointer (e.g., 0 for NULL)
                        ptrVal = reinterpret_cast<void*>(static_cast<uintptr_t>(argOop.asSmallInteger()));
                        memcpy(argSlot, &ptrVal, sizeof(void*));
                    } else {
                        memcpy(argSlot, &ptrVal, sizeof(void*));
                    }
                    break;
                }
                case FFI_TYPE_FLOAT: {
                    double d = 0.0;
                    if (!extractFloat(memory_, argOop, d)) {
                        return PrimitiveResult::Failure;
                    }
                    float f = static_cast<float>(d);
                    memcpy(argSlot, &f, sizeof(float));
                    break;
                }
                case FFI_TYPE_DOUBLE: {
                    double d = 0.0;
                    if (!extractFloat(memory_, argOop, d)) {
                        return PrimitiveResult::Failure;
                    }
                    memcpy(argSlot, &d, sizeof(double));
                    break;
                }
                case FFI_TYPE_SINT8: {
                    if (!argOop.isSmallInteger()) return PrimitiveResult::Failure;
                    int8_t v = static_cast<int8_t>(argOop.asSmallInteger());
                    memcpy(argSlot, &v, sizeof(int8_t));
                    break;
                }
                case FFI_TYPE_UINT8: {
                    if (argOop.isSmallInteger()) {
                        uint8_t v = static_cast<uint8_t>(argOop.asSmallInteger());
                        memcpy(argSlot, &v, sizeof(uint8_t));
                    } else if (argOop.isCharacter()) {
                        uint8_t v = static_cast<uint8_t>(argOop.asCharacter());
                        memcpy(argSlot, &v, sizeof(uint8_t));
                    } else {
                        return PrimitiveResult::Failure;
                    }
                    break;
                }
                case FFI_TYPE_SINT16: {
                    if (!argOop.isSmallInteger()) return PrimitiveResult::Failure;
                    int16_t v = static_cast<int16_t>(argOop.asSmallInteger());
                    memcpy(argSlot, &v, sizeof(int16_t));
                    break;
                }
                case FFI_TYPE_UINT16: {
                    if (!argOop.isSmallInteger()) return PrimitiveResult::Failure;
                    uint16_t v = static_cast<uint16_t>(argOop.asSmallInteger());
                    memcpy(argSlot, &v, sizeof(uint16_t));
                    break;
                }
                case FFI_TYPE_SINT32: {
                    int64_t val = 0;
                    if (argOop.isSmallInteger()) {
                        val = argOop.asSmallInteger();
                    } else if (!trySigned64BitValueOf(memory_, argOop, val)) {
                        return PrimitiveResult::Failure;
                    }
                    int32_t v = static_cast<int32_t>(val);
                    memcpy(argSlot, &v, sizeof(int32_t));
                    break;
                }
                case FFI_TYPE_UINT32: {
                    int64_t val = 0;
                    if (argOop.isSmallInteger()) {
                        val = argOop.asSmallInteger();
                    } else if (!trySigned64BitValueOf(memory_, argOop, val)) {
                        return PrimitiveResult::Failure;
                    }
                    uint32_t v = static_cast<uint32_t>(val);
                    memcpy(argSlot, &v, sizeof(uint32_t));
                    break;
                }
                case FFI_TYPE_SINT64: {
                    int64_t val = 0;
                    if (argOop.isSmallInteger()) {
                        val = argOop.asSmallInteger();
                    } else if (!trySigned64BitValueOf(memory_, argOop, val)) {
                        return PrimitiveResult::Failure;
                    }
                    memcpy(argSlot, &val, sizeof(int64_t));
                    break;
                }
                case FFI_TYPE_UINT64: {
                    int64_t val = 0;
                    if (argOop.isSmallInteger()) {
                        val = argOop.asSmallInteger();
                    } else if (!trySigned64BitValueOf(memory_, argOop, val)) {
                        return PrimitiveResult::Failure;
                    }
                    uint64_t uval = static_cast<uint64_t>(val);
                    memcpy(argSlot, &uval, sizeof(uint64_t));
                    break;
                }
                case FFI_TYPE_INT: {
                    int64_t val = 0;
                    if (argOop.isSmallInteger()) {
                        val = argOop.asSmallInteger();
                    } else if (!trySigned64BitValueOf(memory_, argOop, val)) {
                        return PrimitiveResult::Failure;
                    }
                    int v = static_cast<int>(val);
                    memcpy(argSlot, &v, sizeof(int));
                    break;
                }
                case FFI_TYPE_STRUCT: {
                    // Struct: memcpy from ExternalAddress or ByteArray
                    void* src = tffi_getAddressFromExternalAddressOrByteArray(argOop);
                    if (!src) {
                        return PrimitiveResult::Failure;
                    }
                    size_t structSize = cif->arg_types[i]->size;
                    // For structs larger than 16 bytes, allocate dynamically
                    if (structSize > 16) {
                        void* structBuf = alloca(structSize);
                        memcpy(structBuf, src, structSize);
                        argPtrs[i] = structBuf;
                    } else {
                        memcpy(argSlot, src, structSize);
                    }
                    break;
                }
                default:
                    return PrimitiveResult::Failure;
            }
        }
    }

    // Allocate return value holder
    size_t returnSize = cif->rtype->size;
    if (returnSize < sizeof(ffi_arg)) returnSize = sizeof(ffi_arg);
    void* returnHolder = alloca(returnSize);
    memset(returnHolder, 0, returnSize);

    static int ffiCallCount = 0;
    ffiCallCount++;

    // Perform the FFI call.
    // Wrap in try/catch to handle ObjC exceptions (NSException) that may be
    // thrown by AppKit calls (e.g., setSubmenu: in Mac Catalyst where certain
    // AppKit menu APIs aren't fully supported). On Apple platforms, ObjC
    // exceptions are C++ exceptions, so catch(...) catches them.
    // Return success with zeroed return value so Smalltalk sees nil/0 and
    // continues startup (SDLOSXPlatform >> initPlatformSpecific needs to
    // complete for SDL2 initialization to proceed).
    try {
        ffi_call(cif, FFI_FN(funcPtr), returnHolder, argPtrs);
    } catch (...) {
        fprintf(stderr, "[FFI-CALL #%d] ObjC exception caught — returning zero/nil result\n",
                ffiCallCount);
        fflush(stderr);
        // Zero the return value so Smalltalk gets 0/nil/NULL
        memset(returnHolder, 0, 64);
    }


    // Marshall return value back to Smalltalk
    unsigned short returnTypeId = cif->rtype->type;
    Oop resultOop;

    switch (returnTypeId) {
        case FFI_TYPE_VOID:
            // For void, pop args + receiver, push receiver (the runner)
            resultOop = stackValue(argCount);  // save receiver before popping
            popN(argCount + 1);
            push(resultOop);
            return PrimitiveResult::Success;

        case FFI_TYPE_SINT8: {
            int8_t v; memcpy(&v, returnHolder, sizeof(int8_t));
            resultOop = Oop::fromSmallInteger(v);
            break;
        }
        case FFI_TYPE_UINT8: {
            uint8_t v; memcpy(&v, returnHolder, sizeof(uint8_t));
            resultOop = Oop::fromSmallInteger(v);
            break;
        }
        case FFI_TYPE_SINT16: {
            int16_t v; memcpy(&v, returnHolder, sizeof(int16_t));
            resultOop = Oop::fromSmallInteger(v);
            break;
        }
        case FFI_TYPE_UINT16: {
            uint16_t v; memcpy(&v, returnHolder, sizeof(uint16_t));
            resultOop = Oop::fromSmallInteger(v);
            break;
        }
        case FFI_TYPE_SINT32: {
            int32_t v; memcpy(&v, returnHolder, sizeof(int32_t));
            resultOop = Oop::fromSmallInteger(v);
            break;
        }
        case FFI_TYPE_UINT32: {
            uint32_t v; memcpy(&v, returnHolder, sizeof(uint32_t));
            resultOop = Oop::fromSmallInteger(static_cast<int64_t>(v));
            break;
        }
        case FFI_TYPE_SINT64: {
            int64_t v; memcpy(&v, returnHolder, sizeof(int64_t));
            if (Oop::canBeSmallInteger(v)) {
                resultOop = Oop::fromSmallInteger(v);
            } else {
                // Create LargeInteger
                bool neg = v < 0;
                uint64_t absVal = neg ? static_cast<uint64_t>(-v) : static_cast<uint64_t>(v);
                std::vector<uint8_t> mag;
                while (absVal > 0) { mag.push_back(absVal & 0xFF); absVal >>= 8; }
                if (mag.empty()) mag.push_back(0);
                resultOop = makeLargeInteger(memory_, mag, neg);
                if (resultOop.isNil()) return PrimitiveResult::Failure;
            }
            break;
        }
        case FFI_TYPE_UINT64: {
            uint64_t v; memcpy(&v, returnHolder, sizeof(uint64_t));
            int64_t sv = static_cast<int64_t>(v);
            if (v <= static_cast<uint64_t>(Oop::smallIntegerMax()) && Oop::canBeSmallInteger(sv)) {
                resultOop = Oop::fromSmallInteger(sv);
            } else {
                std::vector<uint8_t> mag;
                uint64_t tmp = v;
                while (tmp > 0) { mag.push_back(tmp & 0xFF); tmp >>= 8; }
                if (mag.empty()) mag.push_back(0);
                resultOop = makeLargeInteger(memory_, mag, false);
                if (resultOop.isNil()) return PrimitiveResult::Failure;
            }
            break;
        }
        case FFI_TYPE_INT: {
            int v; memcpy(&v, returnHolder, sizeof(int));
            resultOop = Oop::fromSmallInteger(v);
            break;
        }
        case FFI_TYPE_POINTER: {
            void* v; memcpy(&v, returnHolder, sizeof(void*));
            resultOop = tffi_newExternalAddress(v);
            if (resultOop.isNil()) return PrimitiveResult::Failure;
            break;
        }
        case FFI_TYPE_FLOAT: {
            float v; memcpy(&v, returnHolder, sizeof(float));
            resultOop = makeFloat(memory_, static_cast<double>(v));
            if (resultOop.isNil()) return PrimitiveResult::Failure;
            break;
        }
        case FFI_TYPE_DOUBLE: {
            double v; memcpy(&v, returnHolder, sizeof(double));
            resultOop = makeFloat(memory_, v);
            if (resultOop.isNil()) return PrimitiveResult::Failure;
            break;
        }
        case FFI_TYPE_STRUCT: {
            // Return struct as ByteArray
            size_t structSize = cif->rtype->size;
            Oop byteArrayClass = memory_.specialObject(SpecialObjectIndex::ClassByteArray);
            uint32_t classIndex = memory_.indexOfClass(byteArrayClass);
            if (classIndex == 0) return PrimitiveResult::Failure;
            resultOop = memory_.allocateBytes(classIndex, structSize);
            if (resultOop.isNil()) return PrimitiveResult::Failure;
            ObjectHeader* hdr = resultOop.asObjectPtr();
            memcpy(hdr->bytes(), returnHolder, structSize);
            break;
        }
        default:
            return PrimitiveResult::Failure;
    }

    // Pop args + receiver, push result
    popN(argCount + 1);
    push(resultOop);
    return PrimitiveResult::Success;
}

// ===== TIER 2 PRIMITIVES =====

// primitiveCopyFromTo (3 args: from, to, size)
// Memcpy between ExternalAddress/ByteArray objects
PrimitiveResult Interpreter::primitiveCopyFromTo(int argCount) {
    if (argCount != 3) return PrimitiveResult::Failure;

    Oop sizeOop = stackValue(0);
    Oop toOop = stackValue(1);
    Oop fromOop = stackValue(2);

    if (!sizeOop.isSmallInteger()) return PrimitiveResult::Failure;
    int64_t size = sizeOop.asSmallInteger();
    if (size < 0) return PrimitiveResult::Failure;

    void* from = tffi_getAddressFromExternalAddressOrByteArray(fromOop);
    void* to = tffi_getAddressFromExternalAddressOrByteArray(toOop);

    if (!from || !to) return PrimitiveResult::Failure;

    memcpy(to, from, static_cast<size_t>(size));

    popN(argCount);  // Pop args, leave receiver
    return PrimitiveResult::Success;
}

// primitiveInitializeStructType (0 args)
// Receiver: TFStructType. Build ffi_type for struct from member types.
// Reads member types from slot 1 (array of TFBasicType).
PrimitiveResult Interpreter::primitiveInitializeStructType(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop receiver = stackTop();
    if (!receiver.isObject()) return PrimitiveResult::Failure;
    if (memory_.slotCountOf(receiver) < 2) return PrimitiveResult::Failure;

    // Slot 1 contains array of member types (each has handler -> ffi_type*)
    Oop membersArrayOop = memory_.fetchPointer(1, receiver);
    if (!membersArrayOop.isObject()) return PrimitiveResult::Failure;
    size_t memberCount = memory_.slotCountOf(membersArrayOop);

    // Allocate ffi_type for struct
    ffi_type* structType = static_cast<ffi_type*>(calloc(1, sizeof(ffi_type)));
    if (!structType) return PrimitiveResult::Failure;

    structType->type = FFI_TYPE_STRUCT;
    structType->size = 0;
    structType->alignment = 0;

    // Allocate elements array (null-terminated)
    structType->elements = static_cast<ffi_type**>(calloc(memberCount + 1, sizeof(ffi_type*)));
    if (!structType->elements) {
        free(structType);
        return PrimitiveResult::Failure;
    }

    for (size_t i = 0; i < memberCount; i++) {
        Oop memberOop = memory_.fetchPointer(i, membersArrayOop);
        ffi_type* memberType = static_cast<ffi_type*>(tffi_getHandler(memberOop));
        if (!memberType) {
            free(structType->elements);
            free(structType);
            return PrimitiveResult::Failure;
        }
        structType->elements[i] = memberType;
    }
    structType->elements[memberCount] = nullptr;

    // Let libffi compute size and alignment
    ffi_cif tmpCif;
    ffi_status status = ffi_prep_cif(&tmpCif, FFI_DEFAULT_ABI, 0, structType, nullptr);
    if (status != FFI_OK) {
        free(structType->elements);
        free(structType);
        return PrimitiveResult::Failure;
    }

    tffi_setHandler(receiver, structType);
    return PrimitiveResult::Success;
}

// primitiveFreeStruct (0 args)
PrimitiveResult Interpreter::primitiveFreeStruct(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop receiver = stackTop();
    ffi_type* structType = static_cast<ffi_type*>(tffi_getHandler(receiver));
    if (!structType) return PrimitiveResult::Failure;

    if (structType->elements) free(structType->elements);
    free(structType);
    tffi_setHandler(receiver, nullptr);

    return PrimitiveResult::Success;
}

// primitiveStructByteSize (0 args)
PrimitiveResult Interpreter::primitiveStructByteSize(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop receiver = stackTop();
    ffi_type* structType = static_cast<ffi_type*>(tffi_getHandler(receiver));
    if (!structType) return PrimitiveResult::Failure;

    primitiveSuccess(Oop::fromSmallInteger(static_cast<int64_t>(structType->size)));
    return PrimitiveResult::Success;
}

// ===== FFI CALLBACK SUPPORT =====
//
// Implements libffi closures for FFI callbacks. When C code calls a registered
// callback thunk, the closure handler signals the callback semaphore so the
// Smalltalk side can process it. For same-thread callbacks (menu handlers, etc.),
// the handler returns 0 immediately since we can't re-enter the interpreter
// from within a C call without setjmp/longjmp (future improvement).
//
// TFCallback instance layout:
//   slot 0: handler (ExternalAddress - the thunk address to pass to C)
//   slot 1: callbackData (ExternalAddress - our CallbackInfo*)
//   slot 2: parameterHandlers (Array of TFBasicType)
//   slot 3: returnTypeHandler (TFBasicType)
//   slot 4: runner

struct CallbackInfo {
    ffi_closure* closure;
    ffi_cif cif;
    void* functionAddress;    // The thunk address (executable code pointer)
    ffi_type** parameterTypes;
    void* userData;           // Debug string (or nullptr)
};

// Global callback semaphore index (set by primitiveInitilizeCallbacks)
static int g_callbackSemaphoreIndex = 0;

// Simple pending callback queue
static constexpr int MAX_PENDING_CALLBACKS = 64;
static struct {
    CallbackInfo* callback;
    void* returnHolder;
    void** arguments;
} g_pendingCallbacks[MAX_PENDING_CALLBACKS];
static int g_pendingCallbackCount = 0;

// The libffi closure handler - called when C code invokes a registered callback
static void callbackClosureHandler(ffi_cif* cif, void* ret, void** args, void* userdata) {
    CallbackInfo* cbInfo = static_cast<CallbackInfo*>(userdata);

    // Queue the callback for Smalltalk processing
    if (g_pendingCallbackCount < MAX_PENDING_CALLBACKS) {
        g_pendingCallbacks[g_pendingCallbackCount].callback = cbInfo;
        g_pendingCallbacks[g_pendingCallbackCount].returnHolder = ret;
        g_pendingCallbacks[g_pendingCallbackCount].arguments = args;
        g_pendingCallbackCount++;
    }

    // Zero the return value (safe default for void, int, pointer return types)
    if (ret && cif->rtype->size > 0) {
        memset(ret, 0, cif->rtype->size);
    }

    // TODO: For full callback support, we'd need setjmp/longjmp to re-enter
    // the interpreter here. For now, we return 0 which is safe for void/int/pointer
    // callbacks (like ObjC IMPs for menu handlers).
}

// primitiveInitilizeCallbacks (sic - typo matches image)
// Stack: receiver, semaphoreIndex -> receiver
PrimitiveResult Interpreter::primitiveInitilizeCallbacks(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop semIdxOop = stackValue(0);
    if (!semIdxOop.isSmallInteger()) return PrimitiveResult::Failure;

    g_callbackSemaphoreIndex = static_cast<int>(semIdxOop.asSmallInteger());

    popN(argCount);  // Pop args, leave receiver
    return PrimitiveResult::Success;
}

// primitiveReadNextCallback
// Returns nil (no pending callbacks) or an ExternalAddress wrapping the CallbackInfo*
PrimitiveResult Interpreter::primitiveReadNextCallback(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    if (g_pendingCallbackCount > 0) {
        // Dequeue first pending callback
        CallbackInfo* cb = g_pendingCallbacks[0].callback;

        // Shift remaining
        for (int i = 1; i < g_pendingCallbackCount; i++) {
            g_pendingCallbacks[i-1] = g_pendingCallbacks[i];
        }
        g_pendingCallbackCount--;

        // Return ExternalAddress wrapping the CallbackInfo*
        Oop result = tffi_newExternalAddress(cb);
        if (result.isNil()) return PrimitiveResult::Failure;
        primitiveSuccess(result);
    } else {
        primitiveSuccess(memory_.nil());
    }
    return PrimitiveResult::Success;
}

// primitiveRegisterCallback
// Receiver: TFCallback, optional arg: debug string
// Creates a libffi closure and stores the thunk address in the receiver.
PrimitiveResult Interpreter::primitiveRegisterCallback(int argCount) {
    if (argCount > 1) return PrimitiveResult::Failure;

    Oop receiver = stackValue(argCount); // TFCallback instance

    if (!receiver.isObject()) return PrimitiveResult::Failure;

    // Read TFCallback slots
    if (memory_.slotCountOf(receiver) < 5) {
        return PrimitiveResult::Failure;
    }

    Oop callbackDataOop = memory_.fetchPointer(1, receiver);  // slot 1: callbackData (ExternalAddress)
    Oop paramArrayOop   = memory_.fetchPointer(2, receiver);  // slot 2: parameterHandlers (Array)
    Oop returnTypeOop   = memory_.fetchPointer(3, receiver);  // slot 3: returnTypeHandler (TFBasicType)
    Oop runnerOop       = memory_.fetchPointer(4, receiver);  // slot 4: runner

    // Get the return ffi_type*
    ffi_type* returnType = static_cast<ffi_type*>(tffi_getHandler(returnTypeOop));
    if (!returnType) {
        return PrimitiveResult::Failure;
    }

    // Get Runner pointer (must be non-null)
    void* runnerPtr = tffi_getHandler(runnerOop);
    if (!runnerPtr) {
        return PrimitiveResult::Failure;
    }

    // Get parameter types from the array
    if (!paramArrayOop.isObject()) return PrimitiveResult::Failure;
    size_t paramCount = memory_.slotCountOf(paramArrayOop);

    ffi_type** paramTypes = static_cast<ffi_type**>(malloc(paramCount * sizeof(ffi_type*)));
    if (!paramTypes) return PrimitiveResult::Failure;

    for (size_t i = 0; i < paramCount; i++) {
        Oop paramOop = memory_.fetchPointer(i, paramArrayOop);
        ffi_type* pt = static_cast<ffi_type*>(tffi_getHandler(paramOop));
        if (!pt) {
            free(paramTypes);
            return PrimitiveResult::Failure;
        }
        paramTypes[i] = pt;
    }

    // Allocate CallbackInfo
    CallbackInfo* cbInfo = new CallbackInfo();
    cbInfo->parameterTypes = paramTypes;
    cbInfo->userData = nullptr;

    // Handle optional debug string argument
    if (argCount == 1) {
        Oop debugOop = stackValue(0);
        if (debugOop.isObject() && !debugOop.isNil()) {
            ObjectHeader* strHdr = debugOop.asObjectPtr();
            if (strHdr->isBytesObject()) {
                size_t len = strHdr->byteSize();
                char* dbgStr = static_cast<char*>(malloc(len + 1));
                memcpy(dbgStr, strHdr->bytes(), len);
                dbgStr[len] = '\0';
                cbInfo->userData = dbgStr;
            }
        }
    }

    // Allocate libffi closure
    cbInfo->closure = static_cast<ffi_closure*>(
        ffi_closure_alloc(sizeof(ffi_closure), &cbInfo->functionAddress));

    if (!cbInfo->closure) {
        free(paramTypes);
        if (cbInfo->userData) free(cbInfo->userData);
        delete cbInfo;
        return PrimitiveResult::Failure;
    }

    // Prepare CIF
    ffi_status status = ffi_prep_cif(&cbInfo->cif, FFI_DEFAULT_ABI,
                                     static_cast<unsigned int>(paramCount),
                                     returnType, paramTypes);
    if (status != FFI_OK) {
        ffi_closure_free(cbInfo->closure);
        free(paramTypes);
        if (cbInfo->userData) free(cbInfo->userData);
        delete cbInfo;
        return PrimitiveResult::Failure;
    }

    // Prepare closure - binds the handler to the closure
    status = ffi_prep_closure_loc(cbInfo->closure, &cbInfo->cif,
                                  callbackClosureHandler, cbInfo,
                                  cbInfo->functionAddress);
    if (status != FFI_OK) {
        ffi_closure_free(cbInfo->closure);
        free(paramTypes);
        if (cbInfo->userData) free(cbInfo->userData);
        delete cbInfo;
        return PrimitiveResult::Failure;
    }

    // Store thunk address in receiver.slot[0] (handler ExternalAddress)
    tffi_setHandler(receiver, cbInfo->functionAddress);

    // Store CallbackInfo* in receiver.slot[1] (callbackData ExternalAddress)
    tffi_writeAddress(callbackDataOop, cbInfo);

    popN(argCount); // Pop args, leave receiver
    return PrimitiveResult::Success;
}

// primitiveUnregisterCallback
// Stack: receiver, callbackHandle (ExternalAddress)
PrimitiveResult Interpreter::primitiveUnregisterCallback(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop handleOop = stackValue(0);
    CallbackInfo* cbInfo = static_cast<CallbackInfo*>(tffi_readAddress(handleOop));

    if (cbInfo) {
        if (cbInfo->closure) ffi_closure_free(cbInfo->closure);
        if (cbInfo->parameterTypes) free(cbInfo->parameterTypes);
        if (cbInfo->userData) free(cbInfo->userData);
        delete cbInfo;
    }

    popN(argCount);
    return PrimitiveResult::Success;
}

// primitiveCallbackReturn
// Receiver: TFCallbackInvocation
// For now, just succeeds (the callback already returned 0 from the handler)
PrimitiveResult Interpreter::primitiveCallbackReturn(int argCount) {
    // In a full implementation, this would use longjmp to return to the C callback
    // with the computed return value. For now, callbacks return 0 immediately.
    popN(argCount);
    return PrimitiveResult::Success;
}

} // namespace pharo
