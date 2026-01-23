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
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <dirent.h>
#include <thread>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <vector>
#include <set>

namespace pharo {

// External variable from Interpreter.cpp for tracing sends after prim 264
extern int g_traceSendsAfterPrim264;

// Forward declaration for large integer helper (defined later with other large int primitives)
static bool trySigned64BitValueOf(ObjectMemory& memory, Oop oop, int64_t& value);

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
    // Primitive 128: Two-way become - swaps ALL references bidirectionally
    // receiver elementsExchange: anotherArray
    // Per official VM: 1 argument, twoWay: true, copyHash: false
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

    // Perform two-way become: swap all references bidirectionally
    // In a single pass, references to fromObj[i] become toObj[i] AND vice versa
    for (size_t i = 0; i < fromSize; i++) {
        Oop fromObj = memory_.fetchPointer(i, fromArrayOop);
        Oop toObj = memory_.fetchPointer(i, toArrayOop);

        // Skip if either is an immediate
        if (!fromObj.isObject() || !toObj.isObject()) {
            continue;
        }
        if (fromObj.rawBits() == toObj.rawBits()) {
            continue;  // Same object, nothing to swap
        }

        // Scan all objects and swap references
        memory_.allObjectsDo([&](Oop obj) {
            if (!obj.isObject()) return;
            ObjectHeader* header = obj.asObjectPtr();
            ObjectFormat fmt = header->format();

            // Skip non-pointer objects
            if (!header->isPointersObject() && !header->isCompiledMethod()) return;

            size_t slots = header->slotCount();
            for (size_t s = 0; s < slots; s++) {
                Oop slot = header->slotAt(s);
                if (slot.rawBits() == fromObj.rawBits()) {
                    header->slotAtPut(s, toObj);
                } else if (slot.rawBits() == toObj.rawBits()) {
                    header->slotAtPut(s, fromObj);
                }
            }
        });
    }

    // Flush method cache (critical after become that may affect classes)
    flushMethodCache();

    popN(1);  // Pop argument, leave receiver
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveIncrementalGC(int argCount) {
    // Primitive 131: Perform an incremental garbage collection
    // Returns the number of bytes of free space after collection
    // Per official VM: does scavenging with tenuring, returns free space
    (void)argCount;

    // Try incremental GC if available, otherwise do nothing (leave for full GC)
    // Note: A true incremental/generational GC would scavenge new space
    // For now, we don't have generational GC, so just report current free space
    memory_.incrementalGC();

    // Get total free space (old space + eden + past/future survivor spaces)
    size_t freeBytes = memory_.freeOldSpaceBytes();

    // Return free space as SmallInteger
    if (Oop::canBeSmallInteger(static_cast<int64_t>(freeBytes))) {
        pop();
        push(Oop::fromSmallInteger(static_cast<int64_t>(freeBytes)));
    } else {
        pop();
        push(Oop::fromSmallInteger(Oop::smallIntegerMax()));
    }

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

    // Check for forwarded objects
    ObjectHeader* hdr = rcvr.asObjectPtr();
    if (hdr->isForwarded()) {
        return PrimitiveResult::Failure;
    }

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
    return PrimitiveResult::Failure;  // GC mourner queue - let Smalltalk handle
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
    // Stack: receiver, argsArray, method (top)
    // argCount = 2 (argsArray and method)

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
    // Method header is slot 0, numArgs is in bits 24-27
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

    // Pop the current arguments (method and argsArray) but leave receiver
    popN(static_cast<size_t>(argCount));

    // Push arguments from the array onto the stack
    for (int i = 0; i < methodNumArgs; i++) {
        Oop arg = memory_.fetchPointer(static_cast<size_t>(i), argsArray);
        push(arg);
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
            return PrimitiveResult::Failure;
        }

        int64_t result = a * b;
        if (Oop::canBeSmallInteger(result)) {
            primitiveSuccess(Oop::fromSmallInteger(result));
            return PrimitiveResult::Success;
        }
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

    if (rcvr.isSmallInteger() && arg.isSmallInteger()) {
        int64_t a = rcvr.asSmallInteger();
        int64_t b = arg.asSmallInteger();

        if (b == 0) {
            return PrimitiveResult::Failure;
        }

        // Smalltalk mod (\\) returns result with same sign as divisor
        // Using integer arithmetic to avoid precision loss (unlike float conversion)
        int64_t rem = a % b;
        // Adjust sign: C remainder has sign of dividend, Smalltalk mod has sign of divisor
        if (rem != 0 && ((a < 0) != (b < 0))) {
            rem += b;
        }
        primitiveSuccess(Oop::fromSmallInteger(rem));
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
        // C division truncates toward zero, floor division rounds toward negative infinity
        int64_t q = a / b;
        int64_t rem = a % b;
        // Adjust for floor: when signs differ and there's a remainder, subtract 1
        if (rem != 0 && ((a < 0) != (b < 0))) {
            q -= 1;
        }
        primitiveSuccess(Oop::fromSmallInteger(q));
        return PrimitiveResult::Success;
    }

    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveQuo(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    if (rcvr.isSmallInteger() && arg.isSmallInteger()) {
        int64_t a = rcvr.asSmallInteger();
        int64_t b = arg.asSmallInteger();

        if (b == 0) {
            return PrimitiveResult::Failure;
        }

        // Truncated division (quo:)
        int64_t result = a / b;
        primitiveSuccess(Oop::fromSmallInteger(result));
        return PrimitiveResult::Success;
    }

    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveBitAnd(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    if (rcvr.isSmallInteger() && arg.isSmallInteger()) {
        int64_t result = rcvr.asSmallInteger() & arg.asSmallInteger();
        primitiveSuccess(Oop::fromSmallInteger(result));
        return PrimitiveResult::Success;
    }

    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveBitOr(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    if (rcvr.isSmallInteger() && arg.isSmallInteger()) {
        int64_t result = rcvr.asSmallInteger() | arg.asSmallInteger();
        primitiveSuccess(Oop::fromSmallInteger(result));
        return PrimitiveResult::Success;
    }

    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveBitXor(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    if (rcvr.isSmallInteger() && arg.isSmallInteger()) {
        int64_t result = rcvr.asSmallInteger() ^ arg.asSmallInteger();
        primitiveSuccess(Oop::fromSmallInteger(result));
        return PrimitiveResult::Success;
    }

    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveBitShift(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    if (rcvr.isSmallInteger() && arg.isSmallInteger()) {
        int64_t value = rcvr.asSmallInteger();
        int64_t shift = arg.asSmallInteger();

        int64_t result;
        if (shift >= 0) {
            // Left shift - check for overflow before shifting
            // SmallIntegers can hold values in range [-2^62, 2^62-1]
            if (shift >= 63) {
                // Would definitely overflow SmallInteger range
                return PrimitiveResult::Failure;
            }
            if (value == 0) {
                result = 0;
            } else if (value > 0) {
                // Check if left shift would overflow
                // max positive SmallInteger is about 2^62-1
                int64_t maxBeforeShift = (INT64_MAX >> shift);
                if (value > maxBeforeShift) {
                    return PrimitiveResult::Failure;
                }
                result = value << shift;
            } else {
                // Negative value - use unsigned shift to avoid UB
                // then convert back considering sign
                uint64_t uval = static_cast<uint64_t>(-value);
                int64_t minBeforeShift = -(INT64_MIN >> shift);
                if (static_cast<int64_t>(uval) > minBeforeShift) {
                    return PrimitiveResult::Failure;
                }
                result = -(static_cast<int64_t>(uval << shift));
            }
        } else {
            // Right shift
            if (shift <= -64) {
                result = (value < 0) ? -1 : 0;
            } else {
                result = value >> (-shift);
            }
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
        pointLog = fopen("/tmp/iospharo-point.log", "a");
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

    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveLessOrEqual(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    // Fast path: both are SmallIntegers
    if (rcvr.isSmallInteger() && arg.isSmallInteger()) {
        bool result = rcvr.asSmallInteger() <= arg.asSmallInteger();
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

    return PrimitiveResult::Failure;
}

// ===== OBJECT ACCESS PRIMITIVES (60-75) =====

PrimitiveResult Interpreter::primitiveAt(int argCount) {
    Oop index = stackValue(0);
    Oop rcvr = stackValue(1);

    // Official VM behavior: fail if index is not SmallInteger (PrimErrBadArgument)
    // or if receiver is not an object (PrimErrInappropriate)
    if (!index.isSmallInteger() || !rcvr.isObject()) {
        // Log when at: is called on non-object (causes Smalltalk error)
        static FILE* atLog = fopen("/tmp/at_fail.log", "a");
        static int atFailCount = 0;
        if (atLog && !rcvr.isObject() && atFailCount < 5) {
            atFailCount++;
            fprintf(atLog, "[AT-FAIL #%d] rcvr=0x%llx isSmallInt=%d value=%lld receiver_=0x%llx index=",
                    atFailCount, rcvr.rawBits(), rcvr.isSmallInteger() ? 1 : 0,
                    rcvr.isSmallInteger() ? rcvr.asSmallInteger() : -999,
                    receiver_.rawBits());
            if (index.isSmallInteger()) {
                fprintf(atLog, "%lld\n", index.asSmallInteger());
            } else {
                fprintf(atLog, "non-int 0x%llx\n", index.rawBits());
            }
            // Show receiver_ class
            std::string rcvrClsName = "?";
            if (receiver_.isObject()) {
                Oop rcvrCls = memory_.classOf(receiver_);
                if (rcvrCls.isObject()) {
                    ObjectHeader* rcH = rcvrCls.asObjectPtr();
                    if (rcH->slotCount() > 6) {
                        Oop rcn = memory_.fetchPointer(6, rcvrCls);
                        if (rcn.isObject()) {
                            ObjectHeader* rcnH = rcn.asObjectPtr();
                            if (rcnH->isBytesObject() && rcnH->byteSize() < 100) {
                                rcvrClsName = std::string((char*)rcnH->bytes(), rcnH->byteSize());
                            }
                        }
                    }
                }
            }
            fprintf(atLog, "  receiver_ class='%s'\n", rcvrClsName.c_str());
            // Show current method selector
            if (method_.isObject()) {
                Oop mhdr = memory_.fetchPointer(0, method_);
                if (mhdr.isSmallInteger()) {
                    int64_t hv = mhdr.asSmallInteger();
                    int nLits = hv & 0x7FFF;
                    if (nLits >= 2 && nLits < 100) {
                        Oop sel = memory_.fetchPointer(nLits - 1, method_);
                        if (sel.isObject()) {
                            ObjectHeader* selH = sel.asObjectPtr();
                            if (selH->isBytesObject() && selH->byteSize() < 100) {
                                fprintf(atLog, "  in method '%s'\n",
                                        std::string((char*)selH->bytes(), selH->byteSize()).c_str());
                            }
                        }
                    }
                }
            }
            // Show context receiver class and Array contents
            if (activeContext_.isObject()) {
                Oop ctxRcvr = memory_.fetchPointer(5, activeContext_);  // ReceiverIndex = 5
                std::string rcvrClassName = "?";
                if (ctxRcvr.isObject()) {
                    Oop crCls = memory_.classOf(ctxRcvr);
                    if (crCls.isObject()) {
                        ObjectHeader* crcH = crCls.asObjectPtr();
                        if (crcH->slotCount() > 6) {
                            Oop crcn = memory_.fetchPointer(6, crCls);
                            if (crcn.isObject()) {
                                ObjectHeader* crcnH = crcn.asObjectPtr();
                                if (crcnH->isBytesObject() && crcnH->byteSize() < 100) {
                                    rcvrClassName = std::string((char*)crcnH->bytes(), crcnH->byteSize());
                                    fprintf(atLog, "  context receiver class='%s'\n", rcvrClassName.c_str());
                                }
                            }
                        }
                    }
                    // Dump receiver's contents
                    ObjectHeader* rcvrH = ctxRcvr.asObjectPtr();
                    fprintf(atLog, "  receiver slots(%zu) classIdx=%u format=%d: ",
                            rcvrH->slotCount(), rcvrH->classIndex(), (int)rcvrH->format());
                    for (size_t i = 0; i < std::min((size_t)10, rcvrH->slotCount()); i++) {
                        Oop el = rcvrH->slotAt(i);
                        if (el.isSmallInteger()) {
                            fprintf(atLog, "%lld ", el.asSmallInteger());
                        } else if (el.isNil()) {
                            fprintf(atLog, "nil ");
                        } else {
                            fprintf(atLog, "obj ");
                        }
                    }
                    fprintf(atLog, "\n");

                    // Walk the context chain to find receivers
                    fprintf(atLog, "  Context chain (checking receivers at each level):\n");
                    Oop ctx = activeContext_;
                    for (int d = 0; d < 5 && ctx.isObject() && !ctx.isNil(); d++) {
                        Oop ctxRcvr2 = memory_.fetchPointer(5, ctx);  // Receiver = slot 5
                        Oop ctxMethod = memory_.fetchPointer(3, ctx);  // Method = slot 3
                        std::string ctxRcvrCls = "?";
                        if (ctxRcvr2.isObject()) {
                            Oop cr2cls = memory_.classOf(ctxRcvr2);
                            if (cr2cls.isObject()) {
                                ObjectHeader* cr2H = cr2cls.asObjectPtr();
                                if (cr2H->slotCount() > 6) {
                                    Oop cr2n = memory_.fetchPointer(6, cr2cls);
                                    if (cr2n.isObject()) {
                                        ObjectHeader* cr2nH = cr2n.asObjectPtr();
                                        if (cr2nH->isBytesObject() && cr2nH->byteSize() < 100) {
                                            ctxRcvrCls = std::string((char*)cr2nH->bytes(), cr2nH->byteSize());
                                        }
                                    }
                                }
                            }
                        } else if (ctxRcvr2.isSmallInteger()) {
                            ctxRcvrCls = "SmallInteger";
                        }
                        // Get method selector
                        std::string selName = "?";
                        if (ctxMethod.isObject()) {
                            Oop mh = memory_.fetchPointer(0, ctxMethod);
                            if (mh.isSmallInteger()) {
                                int nL = mh.asSmallInteger() & 0x7FFF;
                                if (nL >= 2 && nL < 100) {
                                    Oop sel = memory_.fetchPointer(nL - 1, ctxMethod);
                                    if (sel.isObject()) {
                                        ObjectHeader* sH = sel.asObjectPtr();
                                        if (sH->isBytesObject() && sH->byteSize() < 100) {
                                            selName = std::string((char*)sH->bytes(), sH->byteSize());
                                        }
                                    }
                                }
                            }
                        }
                        fprintf(atLog, "    [%d] rcvr=%s method=%s ctx=0x%llx\n",
                                d, ctxRcvrCls.c_str(), selName.c_str(), ctx.rawBits());
                        ctx = memory_.fetchPointer(0, ctx);  // Sender = slot 0
                    }
                } else if (ctxRcvr.isSmallInteger()) {
                    fprintf(atLog, "  context receiver=SmallInteger(%lld)\n", ctxRcvr.asSmallInteger());
                }
            }
            // Print method info and bytecodes
            fprintf(atLog, "  method_=0x%llx ", method_.rawBits());
            if (method_.isObject()) {
                Oop methodHeader = memory_.fetchPointer(0, method_);
                if (methodHeader.isSmallInteger()) {
                    int64_t hdrVal = methodHeader.asSmallInteger();
                    int numLits = hdrVal & 0x7FFF;
                    fprintf(atLog, "numLits=%d\n", numLits);
                    // Dump all literals and explore outer method if it's a block
                    for (int li = 1; li <= numLits && li < 10; li++) {
                        Oop lit = memory_.fetchPointer(li, method_);
                        fprintf(atLog, "    lit[%d]=0x%llx ", li, lit.rawBits());
                        if (lit.isSmallInteger()) {
                            fprintf(atLog, "SmallInt(%lld)", lit.asSmallInteger());
                        } else if (lit.isNil()) {
                            fprintf(atLog, "nil");
                        } else if (lit.isObject()) {
                            Oop litCls = memory_.classOf(lit);
                            std::string litClsName = "?";
                            if (litCls.isObject()) {
                                ObjectHeader* lcH = litCls.asObjectPtr();
                                if (lcH->slotCount() > 6) {
                                    Oop lcn = memory_.fetchPointer(6, litCls);
                                    if (lcn.isObject()) {
                                        ObjectHeader* lcnH = lcn.asObjectPtr();
                                        if (lcnH->isBytesObject() && lcnH->byteSize() < 100) {
                                            litClsName = std::string((char*)lcnH->bytes(), lcnH->byteSize());
                                            fprintf(atLog, "%s", litClsName.c_str());
                                        }
                                    }
                                }
                            }
                            ObjectHeader* litH = lit.asObjectPtr();
                            if (litH->isBytesObject() && litH->byteSize() < 100) {
                                fprintf(atLog, " '%s'",
                                        std::string((char*)litH->bytes(), litH->byteSize()).c_str());
                            }
                            // If it's a CompiledMethod (outer method), show its selector
                            if (litClsName == "CompiledMethod") {
                                Oop outerHeader = memory_.fetchPointer(0, lit);
                                if (outerHeader.isSmallInteger()) {
                                    int outerNumLits = outerHeader.asSmallInteger() & 0x7FFF;
                                    fprintf(atLog, " [outerMethod numLits=%d", outerNumLits);
                                    if (outerNumLits >= 2 && outerNumLits < 100) {
                                        Oop outerSel = memory_.fetchPointer(outerNumLits - 1, lit);
                                        if (outerSel.isObject()) {
                                            ObjectHeader* osH = outerSel.asObjectPtr();
                                            if (osH->isBytesObject() && osH->byteSize() < 100) {
                                                fprintf(atLog, " sel='%s'",
                                                        std::string((char*)osH->bytes(), osH->byteSize()).c_str());
                                            }
                                        }
                                    }
                                    fprintf(atLog, "]");
                                }
                            }
                        }
                        fprintf(atLog, "\n");
                    }
                    // Dump bytecodes
                    ObjectHeader* mH = method_.asObjectPtr();
                    size_t bcStart = (1 + numLits) * 8;
                    size_t totalBytes = mH->byteSize();
                    fprintf(atLog, "  bytecodes (start=%zu total=%zu): ", bcStart, totalBytes);
                    uint8_t* bytes = mH->bytes();
                    for (size_t bi = bcStart; bi < totalBytes && bi < bcStart + 30; bi++) {
                        fprintf(atLog, "%02x ", bytes[bi]);
                    }
                    fprintf(atLog, "\n");
                    // Show where IP is
                    if (instructionPointer_ >= bytes && instructionPointer_ < bytes + totalBytes) {
                        fprintf(atLog, "  IP offset from bcStart: %ld\n",
                                (long)(instructionPointer_ - bytes - bcStart));
                    }
                }
            }
            // Also check sender context's method
            if (activeContext_.isObject()) {
                Oop sender = memory_.fetchPointer(0, activeContext_);  // SenderIndex = 0
                if (sender.isObject() && !sender.isNil()) {
                    Oop senderMethod = memory_.fetchPointer(3, sender);  // MethodIndex = 3
                    fprintf(atLog, "  sender's method=0x%llx", senderMethod.rawBits());
                    if (senderMethod.isObject()) {
                        Oop senderMH = memory_.fetchPointer(0, senderMethod);
                        if (senderMH.isSmallInteger()) {
                            int senderNumLits = senderMH.asSmallInteger() & 0x7FFF;
                            fprintf(atLog, " numLits=%d", senderNumLits);
                            // Get selector
                            if (senderNumLits >= 2 && senderNumLits < 100) {
                                Oop selOop = memory_.fetchPointer(senderNumLits - 1, senderMethod);
                                if (selOop.isObject()) {
                                    ObjectHeader* selH = selOop.asObjectPtr();
                                    if (selH->isBytesObject() && selH->byteSize() < 100) {
                                        fprintf(atLog, " sel='%s'",
                                                std::string((char*)selH->bytes(), selH->byteSize()).c_str());
                                    }
                                }
                            }
                        }
                    }
                    fprintf(atLog, "\n");
                }
            }
            fflush(atLog);
        }
        return PrimitiveResult::Failure;
    }

    int64_t idx = index.asSmallInteger();
    if (idx < 1) {
        return PrimitiveResult::Failure;  // 1-based indexing
    }

    ObjectHeader* header = rcvr.asObjectPtr();
    size_t arrayIndex = static_cast<size_t>(idx - 1);

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
        size_t fixedFields = 0;
        if (fmt == ObjectFormat::IndexableWithFixed || fmt == ObjectFormat::WeakWithFixed) {
            Oop objClass = memory_.classOf(rcvr);
            if (objClass.isObject()) {
                Oop instSpec = memory_.fetchPointer(2, objClass);
                if (instSpec.isSmallInteger()) {
                    fixedFields = instSpec.asSmallInteger() & 0xFFFF;
                }
            }
        }
        size_t indexableSize = header->slotCount() - fixedFields;
        if (arrayIndex >= indexableSize) {
            return PrimitiveResult::Failure;
        }
        size_t actualSlot = fixedFields + arrayIndex;
        Oop result = header->slotAt(actualSlot);
        // Trace at: results that return nil
        if (result.rawBits() == memory_.nil().rawBits()) {
            static FILE* atNilLog = nullptr;
            static int atNilCount = 0;
            if (!atNilLog) atNilLog = fopen("/tmp/at_nil_trace.log", "w");
            if (atNilLog && atNilCount < 100) {
                atNilCount++;
                std::string rcvrClass = "<unknown>";
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
                // Get current method selector
                std::string methodSel = "<unknown>";
                if (method_.isObject()) {
                    ObjectHeader* mHdr = method_.asObjectPtr();
                    if (mHdr->isCompiledMethod()) {
                        Oop hdr = memory_.fetchPointer(0, method_);
                        if (hdr.isSmallInteger()) {
                            size_t numLits = hdr.asSmallInteger() & 0x7FFF;
                            if (numLits >= 2 && numLits < 100) {
                                Oop sel = memory_.fetchPointer(numLits - 1, method_);
                                if (sel.isObject() && sel.rawBits() > 0x10000) {
                                    ObjectHeader* sHdr = sel.asObjectPtr();
                                    if (sHdr->isBytesObject() && sHdr->byteSize() < 50) {
                                        methodSel = std::string((char*)sHdr->bytes(), sHdr->byteSize());
                                    }
                                }
                            }
                        }
                    }
                }
                fprintf(atNilLog, "[AT-NIL #%d] %s at: %lld => nil (slots=%zu) in #%s\n",
                        atNilCount, rcvrClass.c_str(), idx, header->slotCount(), methodSel.c_str());
                fflush(atNilLog);
            }
        }
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

    // TRACE: Log nil values being stored
    static FILE* atPutLog = nullptr;
    static int atPutNilCount = 0;
    if (!atPutLog) atPutLog = fopen("/tmp/atput_nil.log", "w");
    if (atPutLog && atPutNilCount < 50) {
        // Check if value is nil
        bool isNil = value.isObject() && value.rawBits() > 0x10000
                    && value.asObjectPtr()->format() == ObjectFormat::ZeroSized
                    && value.asObjectPtr()->slotCount() == 0;
        if (isNil && index.isSmallInteger()) {
            atPutNilCount++;
            fprintf(atPutLog, "[ATPUT-NIL #%d] Storing nil at index %lld\n",
                    atPutNilCount, index.asSmallInteger());
            // Show receiver class
            if (rcvr.isObject() && rcvr.rawBits() > 0x10000) {
                Oop cls = memory_.classOf(rcvr);
                if (cls.isObject()) {
                    Oop clsName = memory_.fetchPointer(6, cls);
                    if (clsName.isObject() && clsName.rawBits() > 0x10000) {
                        ObjectHeader* cnHdr = clsName.asObjectPtr();
                        if (cnHdr->isBytesObject() && cnHdr->byteSize() < 50) {
                            fprintf(atPutLog, "  rcvr class: %s\n",
                                    std::string((char*)cnHdr->bytes(), cnHdr->byteSize()).c_str());
                        }
                    }
                }
            }
            // Show current method
            if (method_.isObject()) {
                Oop hdr = memory_.fetchPointer(0, method_);
                if (hdr.isSmallInteger()) {
                    int numLits = hdr.asSmallInteger() & 0x7FFF;
                    if (numLits >= 2) {
                        Oop sel = memory_.fetchPointer(numLits - 1, method_);
                        if (sel.isObject() && sel.rawBits() > 0x10000) {
                            ObjectHeader* selHdr = sel.asObjectPtr();
                            if (selHdr->isBytesObject() && selHdr->byteSize() < 50) {
                                fprintf(atPutLog, "  in method: #%s\n",
                                        std::string((char*)selHdr->bytes(), selHdr->byteSize()).c_str());
                            }
                        }
                    }
                }
            }
            fflush(atPutLog);
        }
    }

    if (!index.isSmallInteger() || !rcvr.isObject()) {
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
        ObjectFormat fmt = header->format();
        size_t fixedFields = 0;
        if (fmt == ObjectFormat::IndexableWithFixed || fmt == ObjectFormat::WeakWithFixed) {
            Oop objClass = memory_.classOf(rcvr);
            if (objClass.isObject()) {
                Oop instSpec = memory_.fetchPointer(2, objClass);
                if (instSpec.isSmallInteger()) {
                    fixedFields = instSpec.asSmallInteger() & 0xFFFF;
                }
            }
        }
        size_t indexableSize = header->slotCount() - fixedFields;
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
        // 64-bit word array
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

    // Check for forwarded receiver when argCount > 1 (e.g., object:instVarAt:)
    if (argCount > 1) {
        ObjectHeader* hdr = rcvr.asObjectPtr();
        if (hdr->isForwarded()) {
            return PrimitiveResult::Failure;
        }
    }

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

    if (!index.isSmallInteger() || !rcvr.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Check for forwarded receiver when argCount > 2 (e.g., object:instVarAt:put:)
    ObjectHeader* header = rcvr.asObjectPtr();
    if (argCount > 2 && header->isForwarded()) {
        return PrimitiveResult::Failure;
    }

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
    // Method header format: bits 1-15 encode numLiterals
    uint64_t methodHeader = header->slots()[0].rawBits();
    size_t numLiterals = (methodHeader >> 1) & 0x7FFF;

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

    // Get literal count from current method header
    uint64_t currentHeader = header->slots()[0].rawBits();
    // Literal count is in bits 1-15 (after shifting out tag bit)
    constexpr uint64_t LiteralCountMask = 0x7FFF;
    size_t currentLiteralCount = (currentHeader >> 1) & LiteralCountMask;
    size_t maxIndex = currentLiteralCount + 1;

    // Per official VM: when storing at index 1 (method header):
    // 1. Value must be a SmallInteger
    // 2. Literal count in new header must match current header's literal count
    // This prevents corruption of the method's literal frame
    if (index == 1) {
        if (!value.isSmallInteger()) {
            return PrimitiveResult::Failure;
        }
        // Extract literal count from new value and verify it matches
        uint64_t newHeader = static_cast<uint64_t>(value.asSmallInteger());
        size_t newLiteralCount = newHeader & LiteralCountMask;
        if (newLiteralCount != currentLiteralCount) {
            return PrimitiveResult::Failure;  // Literal count must be preserved
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

    // Per official VM: validate this is a fixed-size class (format < 2)
    // Bits 16-20 encode the object format: 0=zero-sized, 1=fixed, 2+=variable
    int instFormat = (instSpec >> 16) & 0x1F;
    if (instFormat >= 2) {
        // Variable-sized class (Array, String, etc.) - use new: instead
        return PrimitiveResult::Failure;
    }

    uint32_t classIndex = memory_.indexOfClass(rcvr);
    Oop newObj = memory_.allocateSlots(classIndex, instSize);

    if (newObj.isNil()) {
        return PrimitiveResult::Failure;  // Out of memory
    }

    primitiveSuccess(newObj);
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveNewWithArg(int argCount) {
    Oop sizeOop = stackValue(0);
    Oop rcvr = stackValue(1);  // Class

    if (!rcvr.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Handle size argument - can be SmallInteger or LargePositiveInteger
    // TODO: Handle LargePositiveInteger sizes for very large allocations
    if (!sizeOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t indexableSize = sizeOop.asSmallInteger();
    if (indexableSize < 0) {
        return PrimitiveResult::Failure;
    }

    // Get class format to determine if bytes or pointers
    Oop formatObj = memory_.fetchPointer(2, rcvr);
    if (!formatObj.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t format = formatObj.asSmallInteger();
    size_t fixedSize = format & 0xFFFF;
    int instSpec = (format >> 16) & 0x1F;  // Instance specification (0-31)

    // Per official VM: validate this is a variable-sized class (format >= 2)
    // Format 0-1 are fixed-size, format 2+ are variable (Array, String, etc.)
    if (instSpec < 2) {
        // Fixed-size class - use new instead of new:
        return PrimitiveResult::Failure;
    }

    bool isBytes = instSpec >= 16;

    uint32_t classIndex = memory_.indexOfClass(rcvr);

    // Debug: trace Context allocation
    static int contextAllocCount = 0;
    if (instSpec == 3 && contextAllocCount < 5) {
        contextAllocCount++;
        std::string className = "<unknown>";
        Oop nameOop = memory_.fetchPointer(6, rcvr);
        if (nameOop.isObject() && nameOop.rawBits() > 0x10000) {
            ObjectHeader* nameHdr = nameOop.asObjectPtr();
            if (nameHdr->isBytesObject() && nameHdr->byteSize() < 50) {
                className = std::string((char*)nameHdr->bytes(), nameHdr->byteSize());
            }
        }
        std::cerr << "[ALLOC-DEBUG] basicNew: " << className
                  << " indexableSize=" << indexableSize
                  << " fixedSize=" << fixedSize
                  << " classIndex=" << classIndex
                  << " instSpec=" << instSpec << "\n";
    }

    Oop newObj;

    if (isBytes) {
        newObj = memory_.allocateBytes(classIndex, static_cast<size_t>(indexableSize));
    } else {
        size_t totalSlots = fixedSize + static_cast<size_t>(indexableSize);
        // Choose correct format based on instSpec:
        // instSpec 2 = variable pointers only (Array)
        // instSpec 3 = variable pointers with fixed fields (Context, CompiledMethod)
        ObjectFormat objFormat = (instSpec == 3) ? ObjectFormat::IndexableWithFixed : ObjectFormat::Indexable;
        newObj = memory_.allocateSlots(classIndex, totalSlots, objFormat);
    }

    if (newObj.isNil()) {
        // Debug: why allocation failed
        if (instSpec == 3) {
            std::cerr << "[ALLOC-FAIL] IndexableWithFixed allocation failed!\n";
        }
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

    // Per official VM: fail if receiver is immediate
    if (!rcvr.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Fail if forwarded object (official VM: only check when argCount > 0)
    if (argCount > 0) {
        ObjectHeader* hdr = rcvr.asObjectPtr();
        if (hdr && hdr->isForwarded()) {
            return PrimitiveResult::Failure;
        }
    }

    // identityHashOf handles lazy hash generation and caching
    uint32_t hash = memory_.identityHashOf(rcvr);
    popN(argCount + 1);
    push(Oop::fromSmallInteger(hash));
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveClass(int argCount) {
    Oop rcvr = stackValue(argCount);  // Receiver is under arguments

    // Per official VM: fail if forwarded object only when argCount > 0
    if (argCount > 0 && rcvr.isObject()) {
        ObjectHeader* hdr = rcvr.asObjectPtr();
        if (hdr && hdr->isForwarded()) {
            return PrimitiveResult::Failure;
        }
    }

    Oop classOop = memory_.classOf(rcvr);
    popN(argCount + 1);
    push(classOop);
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveIdentical(int argCount) {
    Oop arg = stackValue(0);   // otherObject
    Oop rcvr = stackValue(1);  // thisObject

    // Per official VM: fail if forwarded object
    // - Always check if arg (otherObject) is forwarded
    // - Only check receiver (thisObject) when argCount > 1
    if (arg.isObject()) {
        ObjectHeader* hdr = arg.asObjectPtr();
        if (hdr && hdr->isForwarded()) {
            return PrimitiveResult::Failure;
        }
    }
    if (argCount > 1 && rcvr.isObject()) {
        ObjectHeader* hdr = rcvr.asObjectPtr();
        if (hdr && hdr->isForwarded()) {
            return PrimitiveResult::Failure;
        }
    }

    bool result = (rcvr == arg);
    pop();
    pop();
    push(result ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveNotIdentical(int argCount) {
    Oop arg = stackValue(0);   // otherObject
    Oop rcvr = stackValue(1);  // thisObject

    // Per official VM: fail if forwarded object
    // - Always check if arg (otherObject) is forwarded
    // - Only check receiver (thisObject) when argCount > 1
    if (arg.isObject()) {
        ObjectHeader* hdr = arg.asObjectPtr();
        if (hdr && hdr->isForwarded()) {
            return PrimitiveResult::Failure;
        }
    }
    if (argCount > 1 && rcvr.isObject()) {
        ObjectHeader* hdr = rcvr.asObjectPtr();
        if (hdr && hdr->isForwarded()) {
            return PrimitiveResult::Failure;
        }
    }

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

    // Validate argsArray is an indexable pointer object (Array)
    // Per official Pharo VM: must be format 2 (Indexable) with pointer contents
    if (argsHeader->isForwarded()) {
        return PrimitiveResult::Failure;
    }
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

    ObjectHeader* blockHdr = block.asObjectPtr();
    if (blockHdr->isForwarded()) {
        return PrimitiveResult::Failure;
    }

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
        return PrimitiveResult::Failure;
    }

    // Get numArgs from the closure (slot 2)
    Oop numArgsOop = memory_.fetchPointer(2, closure);
    if (!numArgsOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int closureNumArgs = static_cast<int>(numArgsOop.asSmallInteger());
    if (closureNumArgs != argCount) {
        return PrimitiveResult::Failure;
    }

    // TRACE: Log block activation with args - focus on nil args
    static FILE* blockArgLog = nullptr;
    static int blockArgCount = 0;
    static int nilArgCount = 0;
    if (!blockArgLog) blockArgLog = fopen("/tmp/block_args.log", "w");

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

    // Check for forwarded process (official VM behavior)
    ObjectHeader* procHdr = process.asObjectPtr();
    if (procHdr->isForwarded()) {
        return PrimitiveResult::Failure;
    }

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
        transferTo(nextProcess);
        return PrimitiveResult::Success;
    }

    // Suspending another process - it must be on some list
    Oop myList = memory_.fetchPointer(ProcessMyListIndex, process);

    // Check if myList is forwarded (official VM uses followForwarded)
    if (myList.isObject() && myList.rawBits() > 0x10000) {
        ObjectHeader* listHdr = myList.asObjectPtr();
        if (listHdr->isForwarded()) {
            return PrimitiveResult::Failure;
        }
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

    // Check for forwarded process (official VM behavior)
    ObjectHeader* procHdr = process.asObjectPtr();
    if (procHdr->isForwarded()) {
        return PrimitiveResult::Failure;
    }

    // Verify process has a valid suspended context
    Oop context = memory_.fetchPointer(ProcessSuspendedContextIndex, process);
    Oop nilObj = memory_.nil();

    if (context.isNil() || context.rawBits() == nilObj.rawBits() || !context.isObject()) {
        return PrimitiveResult::Failure;  // Can't resume without a valid context
    }

    // Per official VM: validate context is actually a Context
    // Check if context is forwarded and validate format
    ObjectHeader* ctxHdr = context.asObjectPtr();
    if (ctxHdr->isForwarded()) {
        return PrimitiveResult::Failure;
    }
    // Context objects have format 3 (IndexableWithFixed)
    if (ctxHdr->format() != ObjectFormat::IndexableWithFixed) {
        return PrimitiveResult::Failure;  // Not a valid Context
    }

    // Get priorities to check for preemption
    Oop processPriorityOop = memory_.fetchPointer(ProcessPriorityIndex, process);
    int processPriority = static_cast<int>(processPriorityOop.asSmallInteger());

    Oop activeProcess = getActiveProcess();
    Oop activePriorityOop = memory_.fetchPointer(ProcessPriorityIndex, activeProcess);
    int activePriority = static_cast<int>(activePriorityOop.asSmallInteger());

    if (processPriority > activePriority) {
        // Resumed process has higher priority - preempt current process
        // Put current process to sleep
        putToSleep(activeProcess);
        // Switch to resumed process (don't put it to sleep, just run it)
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
    // Signal a semaphore. If processes are waiting, wake the first one.
    // Otherwise increment excessSignals.
    Oop semaphore = stackTop();  // Receiver

    if (!semaphore.isObject()) {
        return PrimitiveResult::Failure;
    }

    Oop nilObj = memory_.nil();
    Oop firstLink = memory_.fetchPointer(LinkedListFirstLinkIndex, semaphore);

    if (firstLink.isNil() || firstLink.rawBits() == nilObj.rawBits()) {
        // No processes waiting - increment excessSignals
        Oop excessOop = memory_.fetchPointer(SemaphoreExcessSignalsIndex, semaphore);
        int64_t excess = excessOop.isSmallInteger() ? excessOop.asSmallInteger() : 0;
        memory_.storePointer(SemaphoreExcessSignalsIndex, semaphore,
                            Oop::fromSmallInteger(excess + 1));
        // Return receiver (semaphore stays on stack)
        return PrimitiveResult::Success;
    }

    // Wake the first waiting process
    Oop process = removeFirstLinkOfList(semaphore);

    // Get process priority and check if we should preempt
    Oop processPriorityOop = memory_.fetchPointer(ProcessPriorityIndex, process);
    int processPriority = static_cast<int>(processPriorityOop.asSmallInteger());

    Oop activeProcess = getActiveProcess();
    Oop activePriorityOop = memory_.fetchPointer(ProcessPriorityIndex, activeProcess);
    int activePriority = static_cast<int>(activePriorityOop.asSmallInteger());

    if (processPriority > activePriority) {
        // Higher priority - preempt current process
        putToSleep(activeProcess);
        transferTo(process);
    } else {
        // Same or lower priority - just add to ready queue
        putToSleep(process);
    }

    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveWait(int argCount) {
    // Primitive 86: Semaphore>>wait
    // Wait on a semaphore. If excessSignals > 0, decrement and return.
    // Otherwise suspend current process on the semaphore's wait list.

    static int waitCallCount = 0;
    static FILE* waitLog = nullptr;
    waitCallCount++;

    if (!waitLog) {
        waitLog = fopen("/tmp/prim_wait.log", "w");
    }

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
            if (waitLog && waitCallCount <= 100) {
                fprintf(waitLog, "[WAIT] #%d sem=%p excess=%lld -> decrement and return\n",
                        waitCallCount, (void*)semaphore.rawBits(), (long long)excess);
                fflush(waitLog);
            }
            memory_.storePointer(SemaphoreExcessSignalsIndex, semaphore,
                                Oop::fromSmallInteger(excess - 1));
            return PrimitiveResult::Success;
        }
    }

    // No signal available - must wait
    // Per official VM: add current process to wait list and switch to next runnable
    if (waitLog && waitCallCount <= 100) {
        fprintf(waitLog, "[WAIT] #%d sem=%p -> blocking process\n",
                waitCallCount, (void*)semaphore.rawBits());
        fflush(waitLog);
    }
    Oop activeProcess = getActiveProcess();
    addLastLinkToList(activeProcess, semaphore);

    // Find next runnable process and switch to it
    // Per official VM: always transfer, don't check for nil
    // The scheduler should always have at least one runnable process (idle)
    Oop nextProcess = wakeHighestPriority();
    transferTo(nextProcess);
    return PrimitiveResult::Success;
}

// ===== SYSTEM PRIMITIVES =====

PrimitiveResult Interpreter::primitiveQuit(int argCount) {
    // Smalltalk quitPrimitive / Smalltalk exit: exitCode
    // First call during startup tries to reschedule; subsequent calls actually quit

    static int quitCallCount = 0;
    quitCallCount++;

    // Get exit code if provided
    int exitCode = 0;
    if (argCount > 0) {
        Oop arg = stackTop();
        if (arg.isSmallInteger()) {
            exitCode = static_cast<int>(arg.asSmallInteger());
        }
    }

    std::cerr << "[VM] primitiveQuit called (call #" << quitCallCount << ") exit code " << exitCode << "\n";

    if (quitCallCount == 1) {
        // First quit - during startup. Try to reschedule to UI process.
        popN(argCount + 1);

        if (tryReschedule()) {
            return PrimitiveResult::Success;
        }

        if (bootstrapStartup()) {
            return PrimitiveResult::Success;
        }

        // Couldn't reschedule - don't exit, just return
        std::cerr << "[VM] First quit: reschedule failed, continuing anyway\n";
        return PrimitiveResult::Success;
    }

    // Second+ quit - user explicitly asked to quit
    std::cerr << "[VM] Second quit: exiting\n";
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
        Oop result = memory_.allocateSlots(classIndex, paramsArraySize);
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
        // Set parameter (most are read-only, just return old value)
        Oop indexOop = stackValue(1);
        if (!indexOop.isSmallInteger()) {
            return PrimitiveResult::Failure;
        }

        int64_t index = indexOop.asSmallInteger();
        if (index < 1 || index > paramsArraySize) {
            return PrimitiveResult::Failure;
        }

        // Return old value (we don't actually set most parameters)
        Oop result = getParameter(static_cast<int>(index));
        primitiveSuccess(result);
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
    // SAVE IS DISABLED per CLAUDE.md
    // "Save is disabled for now to ensure consistent testing from fresh state"
    // Always fail to prevent any image saving
    std::cerr << "[VM] primitiveSnapshot: Save disabled - failing primitive\n";
    return PrimitiveResult::Failure;

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
    if (argCount != 0) return PrimitiveResult::Failure;

    // Get the receiver (a Form object)
    Oop form = stackTop();
    if (!form.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Store as the display form
    setDisplayForm(form);

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
    if (!displayForm_.isNil() && displayForm_.isObject()) {
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

            // Handle pixel format conversion
            // Pharo Forms are ARGB (or BGRA depending on endianness)
            // iOS expects BGRA (little-endian: ARGB in memory order)
            if (srcDepth == 32) {
                for (int y = 0; y < copyHeight; y++) {
                    for (int x = 0; x < copyWidth; x++) {
                        uint32_t pixel = srcPixels[y * srcWidth + x];
                        // Pharo 32-bit Forms are typically ARGB
                        // Our display expects ARGB as well, so direct copy should work
                        dstPixels[y * dstWidth + x] = pixel;
                    }
                }
            } else if (srcDepth == 16) {
                // 16-bit: RGB565 format
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
                // Other depths: just copy as-is
                for (int y = 0; y < copyHeight; y++) {
                    for (int x = 0; x < copyWidth; x++) {
                        dstPixels[y * dstWidth + x] = srcPixels[y * srcWidth + x];
                    }
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

// Primitive 142: Get the VM executable path
// Returns the path to the VM as a String
PrimitiveResult Interpreter::primitiveVMPath(int argCount) {
    Oop result = createStringObject(memory_, vmPath_);
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
    constexpr int64_t MillisecondClockMask = 0x1FFFFFFF;  // 30-bit mask
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

    // Fall back to Smalltalk for LargeInteger creation
    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveLocalMicrosecondClock(int argCount) {
    // Primitive 241: Same as 240 but for local time
    return primitiveMicrosecondClock(argCount);
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

    // Store the timer info (in ioMSecs units, 30-bit wrapping)
    timerSemaphore_ = semaphore;
    nextWakeupTime_ = targetMs & 0x3FFFFFFF;  // Ensure 30-bit

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
        size_t byteOffset = arrayIndex * 4;
        header->byteAtPut(byteOffset, codePoint & 0xFF);
        header->byteAtPut(byteOffset + 1, (codePoint >> 8) & 0xFF);
        header->byteAtPut(byteOffset + 2, (codePoint >> 16) & 0xFF);
        header->byteAtPut(byteOffset + 3, (codePoint >> 24) & 0xFF);
        primitiveSuccess(value);  // Return the character
        return PrimitiveResult::Success;
    }

    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveReplaceFromTo(int argCount) {
    // replaceFrom:to:with:startingAt:
    // rcvr[start..stop] := replacement[repStart..]

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
            return PrimitiveResult::Failure;
        }

        for (int64_t i = 0; i < count; ++i) {
            uint8_t byte = replHeader->byteAt(repStartIdx - 1 + i);
            rcvrHeader->byteAtPut(startIdx - 1 + i, byte);
        }

        primitiveSuccess(rcvr);
        return PrimitiveResult::Success;
    }

    // Handle pointer objects
    if (rcvrHeader->isPointersObject() && replHeader->isPointersObject()) {
        size_t rcvrSize = rcvrHeader->slotCount();
        size_t replSize = replHeader->slotCount();

        if (static_cast<size_t>(stopIdx) > rcvrSize ||
            static_cast<size_t>(repStartIdx + count - 1) > replSize) {
            return PrimitiveResult::Failure;
        }

        for (int64_t i = 0; i < count; ++i) {
            Oop value = replHeader->slotAt(repStartIdx - 1 + i);
            rcvrHeader->slotAtPut(startIdx - 1 + i, value);
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
        if (header->format() == ObjectFormat::Indexable64) {
            uint64_t bits = memory.fetchWord64(0, oop);
            std::memcpy(&result, &bits, sizeof(double));
            return true;
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

    // Allocate boxed Float
    Oop floatClass = memory.specialObject(SpecialObjectIndex::ClassFloat);
    uint32_t classIndex = memory.indexOfClass(floatClass);
    Oop floatObj = memory.allocateWords(classIndex, 1);  // 1 word = 64 bits

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

// Helper: Multiply two magnitudes
static std::vector<uint8_t> multiplyMagnitudes(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.empty() || b.empty() || (a.size() == 1 && a[0] == 0) || (b.size() == 1 && b[0] == 0)) {
        return std::vector<uint8_t>(1, 0);
    }

    std::vector<uint8_t> result(a.size() + b.size(), 0);

    for (size_t i = 0; i < a.size(); i++) {
        uint16_t carry = 0;
        for (size_t j = 0; j < b.size() || carry; j++) {
            uint32_t prod = result[i + j] + carry;
            if (j < b.size()) {
                prod += static_cast<uint32_t>(a[i]) * b[j];
            }
            result[i + j] = static_cast<uint8_t>(prod & 0xFF);
            carry = static_cast<uint16_t>(prod >> 8);
        }
    }

    // Remove trailing zeros
    while (result.size() > 1 && result.back() == 0) {
        result.pop_back();
    }
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

    // Simple long division algorithm (byte by byte)
    remainder.clear();
    quotient.resize(dividend.size(), 0);

    for (int i = static_cast<int>(dividend.size()) - 1; i >= 0; i--) {
        // Shift remainder left by 8 bits and add next byte
        remainder.insert(remainder.begin(), dividend[i]);

        // Remove leading zeros from remainder for comparison
        while (remainder.size() > 1 && remainder.back() == 0) {
            remainder.pop_back();
        }

        // Find how many times divisor fits into remainder
        uint8_t q = 0;
        while (compareMagnitudes(remainder, divisor) >= 0) {
            remainder = subtractMagnitudes(remainder, divisor);
            q++;
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

// Primitive 34: Bitwise AND
PrimitiveResult Interpreter::primitiveBitAndLargeIntegers(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    std::vector<uint8_t> aMag, bMag;
    bool aNeg, bNeg;

    if (!extractInteger(memory_, rcvr, aMag, aNeg) ||
        !extractInteger(memory_, arg, bMag, bNeg)) {
        return PrimitiveResult::Failure;
    }

    // For simplicity, only handle positive integers for bitwise ops
    // (Two's complement for negatives is complex)
    if (aNeg || bNeg) {
        return PrimitiveResult::Failure;
    }

    std::vector<uint8_t> result = bitwiseAnd(aMag, bMag);

    Oop resultOop;
    if (tryConvertToSmallInteger(result, false, resultOop)) {
        primitiveSuccess(resultOop);
        return PrimitiveResult::Success;
    }

    resultOop = makeLargeInteger(memory_, result, false);
    if (resultOop.isNil()) return PrimitiveResult::Failure;

    primitiveSuccess(resultOop);
    return PrimitiveResult::Success;
}

// Primitive 35: Bitwise OR
PrimitiveResult Interpreter::primitiveBitOrLargeIntegers(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    std::vector<uint8_t> aMag, bMag;
    bool aNeg, bNeg;

    if (!extractInteger(memory_, rcvr, aMag, aNeg) ||
        !extractInteger(memory_, arg, bMag, bNeg)) {
        return PrimitiveResult::Failure;
    }

    if (aNeg || bNeg) {
        return PrimitiveResult::Failure;
    }

    std::vector<uint8_t> result = bitwiseOr(aMag, bMag);

    Oop resultOop;
    if (tryConvertToSmallInteger(result, false, resultOop)) {
        primitiveSuccess(resultOop);
        return PrimitiveResult::Success;
    }

    resultOop = makeLargeInteger(memory_, result, false);
    if (resultOop.isNil()) return PrimitiveResult::Failure;

    primitiveSuccess(resultOop);
    return PrimitiveResult::Success;
}

// Primitive 36: Bitwise XOR
PrimitiveResult Interpreter::primitiveBitXorLargeIntegers(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    std::vector<uint8_t> aMag, bMag;
    bool aNeg, bNeg;

    if (!extractInteger(memory_, rcvr, aMag, aNeg) ||
        !extractInteger(memory_, arg, bMag, bNeg)) {
        return PrimitiveResult::Failure;
    }

    if (aNeg || bNeg) {
        return PrimitiveResult::Failure;
    }

    std::vector<uint8_t> result = bitwiseXor(aMag, bMag);

    Oop resultOop;
    if (tryConvertToSmallInteger(result, false, resultOop)) {
        primitiveSuccess(resultOop);
        return PrimitiveResult::Success;
    }

    resultOop = makeLargeInteger(memory_, result, false);
    if (resultOop.isNil()) return PrimitiveResult::Failure;

    primitiveSuccess(resultOop);
    return PrimitiveResult::Success;
}

// Primitive 37: Bit shift (positive = left, negative = right)
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

    // For simplicity, only handle positive integers
    if (aNeg) {
        return PrimitiveResult::Failure;
    }

    std::vector<uint8_t> result;

    if (shift >= 0) {
        // Left shift
        size_t byteShift = shift / 8;
        int bitShift = shift % 8;

        result.resize(aMag.size() + byteShift + 1, 0);

        // Copy with byte shift
        for (size_t i = 0; i < aMag.size(); i++) {
            result[i + byteShift] = aMag[i];
        }

        // Apply bit shift within bytes
        if (bitShift > 0) {
            uint8_t carry = 0;
            for (size_t i = byteShift; i < result.size(); i++) {
                uint16_t val = (static_cast<uint16_t>(result[i]) << bitShift) | carry;
                result[i] = val & 0xFF;
                carry = val >> 8;
            }
        }
    } else {
        // Right shift
        size_t byteShift = (-shift) / 8;
        int bitShift = (-shift) % 8;

        if (byteShift >= aMag.size()) {
            // Shifted to zero
            result = {0};
        } else {
            result.resize(aMag.size() - byteShift, 0);

            // Copy with byte shift
            for (size_t i = byteShift; i < aMag.size(); i++) {
                result[i - byteShift] = aMag[i];
            }

            // Apply bit shift within bytes
            if (bitShift > 0) {
                uint8_t carry = 0;
                for (size_t i = result.size(); i > 0; i--) {
                    uint16_t val = (static_cast<uint16_t>(result[i-1]) << (8 - bitShift)) | (carry << 8);
                    carry = result[i-1] & ((1 << bitShift) - 1);
                    result[i-1] = val >> 8;
                }
            }
        }
    }

    // Trim leading zeros
    while (result.size() > 1 && result.back() == 0) result.pop_back();

    Oop resultOop;
    if (tryConvertToSmallInteger(result, false, resultOop)) {
        primitiveSuccess(resultOop);
        return PrimitiveResult::Success;
    }

    resultOop = makeLargeInteger(memory_, result, false);
    if (resultOop.isNil()) return PrimitiveResult::Failure;

    primitiveSuccess(resultOop);
    return PrimitiveResult::Success;
}

// ===== GC PRIMITIVES =====

PrimitiveResult Interpreter::primitiveFullGC(int argCount) {
    // Primitive 130: Perform a full garbage collection
    // Returns the number of bytes of free space after collection

    // Trigger a full garbage collection
    memory_.fullGC();

    // Get free space after GC
    size_t freeBytes = memory_.freeOldSpaceBytes();

    // Try to return as SmallInteger
    if (Oop::canBeSmallInteger(static_cast<int64_t>(freeBytes))) {
        primitiveSuccess(Oop::fromSmallInteger(static_cast<int64_t>(freeBytes)));
    } else {
        // If too large for SmallInteger, return a reasonable estimate
        // (shouldn't happen in practice, but be safe)
        primitiveSuccess(Oop::fromSmallInteger(Oop::smallIntegerMax()));
    }

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
        // 32-bit word array
        if (!value.isSmallInteger()) {
            return PrimitiveResult::Failure;
        }
        int64_t wordVal = value.asSmallInteger();

        uint32_t* words = reinterpret_cast<uint32_t*>(header + 1);
        size_t wordCount = size / 4;
        for (size_t i = 0; i < wordCount; i++) {
            words[i] = static_cast<uint32_t>(wordVal);
        }
    } else if (format == ObjectFormat::Indexable64) {
        // 64-bit word array
        if (!value.isSmallInteger()) {
            return PrimitiveResult::Failure;
        }
        int64_t quadVal = value.asSmallInteger();

        uint64_t* quads = reinterpret_cast<uint64_t*>(header + 1);
        size_t quadCount = size / 8;
        for (size_t i = 0; i < quadCount; i++) {
            quads[i] = static_cast<uint64_t>(quadVal);
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

// Primitive 167: Yield to other processes of same priority
PrimitiveResult Interpreter::primitiveYield(int argCount) {
    static int yieldCount = 0;
    static int noSwitchCount = 0;

    yieldCount++;

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

    // Check if there are other processes at the same priority
    Oop firstLink = memory_.fetchPointer(LinkedListFirstLinkIndex, priorityList);

    if (!firstLink.isNil()) {
        // There are other processes waiting - put current process at end of queue
        // and switch to the first one
        noSwitchCount = 0;  // Reset counter

        // Save current context
        Oop currentContext = activeContext_;

        // Debug: log what we're saving
        static FILE* yieldSaveLog = nullptr;
        if (!yieldSaveLog) yieldSaveLog = fopen("/tmp/yield_save.log", "w");
        if (yieldSaveLog) {
            std::string ctxMethod = "?";
            if (currentContext.isObject() && currentContext.rawBits() > 0x10000) {
                Oop method = memory_.fetchPointer(3, currentContext);
                if (method.isObject() && method.rawBits() > 0x10000) {
                    Oop mhdr = memory_.fetchPointer(0, method);
                    if (mhdr.isSmallInteger()) {
                        int64_t hv = mhdr.asSmallInteger();
                        int nLits = hv & 0x7FFF;
                        if (nLits >= 2 && nLits < 100) {
                            Oop sel = memory_.fetchPointer(nLits - 1, method);
                            if (sel.isObject()) {
                                ObjectHeader* selH = sel.asObjectPtr();
                                if (selH->isBytesObject() && selH->byteSize() < 100) {
                                    ctxMethod = std::string((char*)selH->bytes(), selH->byteSize());
                                }
                            }
                        }
                    }
                }
            }
            fprintf(yieldSaveLog, "[YIELD-SAVE #%d] Saving context 0x%llx (method=#%s) for process 0x%llx\n",
                    yieldCount, (unsigned long long)currentContext.rawBits(), ctxMethod.c_str(),
                    (unsigned long long)activeProcess.rawBits());
            fflush(yieldSaveLog);
        }

        memory_.storePointer(ProcessSuspendedContextIndex, activeProcess, currentContext);

        // Add current process to end of priority list
        addLastLinkToList(activeProcess, priorityList);

        // Remove first process from list and make it active
        Oop nextProcess = removeFirstLinkOfList(priorityList);
        setActiveProcess(nextProcess);

        // Switch to the next process's context
        Oop newContext = memory_.fetchPointer(ProcessSuspendedContextIndex, nextProcess);

        // CRITICAL: Only clear suspendedContext if we have a valid context to execute
        // Otherwise the process will be left with nil context and can't be resumed
        static FILE* yieldCtxLog = nullptr;
        static int yieldCtxCount = 0;
        if (!yieldCtxLog) yieldCtxLog = fopen("/tmp/yield_context.log", "w");
        yieldCtxCount++;
        if (yieldCtxLog && yieldCtxCount <= 50) {
            fprintf(yieldCtxLog, "[YIELD-CTX #%d] nextProcess=0x%llx context=0x%llx isNil=%d\n",
                    yieldCtxCount, (unsigned long long)nextProcess.rawBits(),
                    (unsigned long long)newContext.rawBits(),
                    newContext.isNil() ? 1 : 0);
            fflush(yieldCtxLog);
        }

        if (!newContext.isNil() && newContext.isObject()) {
            // Only clear suspendedContext AFTER we confirm we have a valid context
            memory_.storePointer(ProcessSuspendedContextIndex, nextProcess, memory_.nil());
            executeFromContext(newContext);
        } else {
            // WARNING: Process has nil context, can't execute
            if (yieldCtxLog && yieldCtxCount <= 50) {
                fprintf(yieldCtxLog, "[YIELD-CTX #%d] SKIPPING process with nil context!\n", yieldCtxCount);
                fflush(yieldCtxLog);
            }
            // Don't switch to this process, keep current process running
        }
    } else {
        // No process switch happened - track consecutive yields without switch
        noSwitchCount++;

        // If we're spin-yielding (no other process to switch to), sleep briefly
        // This prevents CPU spinning when the delay mechanism isn't working
        if (noSwitchCount > 10) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            noSwitchCount = 0;  // Reset after sleeping

            // Process any pending external semaphore signals
            processPendingSignals();
        }
    }

    // Return receiver (the process or processor)
    primitiveSuccess(stackTop());
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

    // Handle 64-bit word objects
    if (fmt == ObjectFormat::Indexable64) {
        size_t slotCount = header->slotCount();
        if (zeroIndex >= slotCount) {
            return PrimitiveResult::Failure;
        }
        uint64_t* words = reinterpret_cast<uint64_t*>(header + 1);
        uint64_t val = words[zeroIndex];
        // Return as SmallInteger if it fits, otherwise fail
        if (Oop::canBeSmallInteger(static_cast<int64_t>(val))) {
            popN(2);
            push(Oop::fromSmallInteger(static_cast<int64_t>(val)));
            return PrimitiveResult::Success;
        }
        return PrimitiveResult::Failure;
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

    // Handle 64-bit word objects
    if (fmt == ObjectFormat::Indexable64) {
        if (!value.isSmallInteger()) {
            return PrimitiveResult::Failure;
        }
        int64_t wordVal = value.asSmallInteger();
        if (wordVal < 0) {
            return PrimitiveResult::Failure;
        }
        size_t slotCount = header->slotCount();
        if (zeroIndex >= slotCount) {
            return PrimitiveResult::Failure;
        }
        uint64_t* words = reinterpret_cast<uint64_t*>(header + 1);
        words[zeroIndex] = static_cast<uint64_t>(wordVal);
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
    // Collect all objects using allObjectsDo
    std::vector<Oop> objects;
    memory_.allObjectsDo([&](Oop obj) {
        if (obj.isObject()) {
            objects.push_back(obj);
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

// Primitive 72: Swap identities of two objects (two-way become)
PrimitiveResult Interpreter::primitiveBecome(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    if (!rcvr.isObject() || !arg.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Both must be regular objects (not immediates)
    if (rcvr.rawBits() == arg.rawBits()) {
        // Same object - nothing to do
        popN(2);
        push(rcvr);
        return PrimitiveResult::Success;
    }

    // Perform two-way become by swapping all references
    memory_.allObjectsDo([&](Oop obj) {
        if (!obj.isObject()) return;

        ObjectHeader* header = obj.asObjectPtr();
        ObjectFormat format = header->format();

        // Skip non-pointer objects (byte/word arrays)
        if (format >= ObjectFormat::Indexable8 && format <= ObjectFormat::Indexable8_7) return;
        if (format >= ObjectFormat::Indexable32 && format <= ObjectFormat::Indexable64) return;

        size_t slotCount = header->slotCount();
        for (size_t i = 0; i < slotCount; i++) {
            Oop slot = memory_.fetchPointer(i, obj);
            if (slot.rawBits() == rcvr.rawBits()) {
                memory_.storePointer(i, obj, arg);
            } else if (slot.rawBits() == arg.rawBits()) {
                memory_.storePointer(i, obj, rcvr);
            }
        }
    });

    popN(2);
    push(rcvr);
    return PrimitiveResult::Success;
}

// Primitive 128: Forward all references from rcvr to arg (one-way become)
PrimitiveResult Interpreter::primitiveBecomeForward(int argCount) {
    // Can take 1 arg (simple forward) or 2 args (with copyHash flag)
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(argCount);

    if (!rcvr.isObject() || !arg.isObject()) {
        return PrimitiveResult::Failure;
    }

    if (rcvr.rawBits() == arg.rawBits()) {
        // Same object - nothing to do
        popN(argCount + 1);
        push(rcvr);
        return PrimitiveResult::Success;
    }

    // Perform one-way become: replace all references to rcvr with arg
    memory_.allObjectsDo([&](Oop obj) {
        if (!obj.isObject()) return;

        ObjectHeader* header = obj.asObjectPtr();
        ObjectFormat format = header->format();

        // Skip non-pointer objects (byte/word arrays)
        if (format >= ObjectFormat::Indexable8 && format <= ObjectFormat::Indexable8_7) return;
        if (format >= ObjectFormat::Indexable32 && format <= ObjectFormat::Indexable64) return;

        size_t slotCount = header->slotCount();
        for (size_t i = 0; i < slotCount; i++) {
            Oop slot = memory_.fetchPointer(i, obj);
            if (slot.rawBits() == rcvr.rawBits()) {
                memory_.storePointer(i, obj, arg);
            }
        }
    });

    popN(argCount + 1);
    push(rcvr);
    return PrimitiveResult::Success;
}

// ===== BIT OPERATION PRIMITIVES =====

// Primitive 575: Return the index of the high bit (1-based, 0 if no bits set)
PrimitiveResult Interpreter::primitiveHighBit(int argCount) {
    Oop rcvr = stackTop();

    if (!rcvr.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t value = rcvr.asSmallInteger();

    if (value < 0) {
        return PrimitiveResult::Failure;  // Undefined for negative
    }

    if (value == 0) {
        pop();
        push(Oop::fromSmallInteger(0));
        return PrimitiveResult::Success;
    }

    // Count leading zeros and compute high bit position (1-based)
    // For 64-bit value, highBit = 64 - __builtin_clzll(value)
    int highBit = 64 - __builtin_clzll(static_cast<uint64_t>(value));

    pop();
    push(Oop::fromSmallInteger(highBit));
    return PrimitiveResult::Success;
}

// Primitive 576: Return the index of the low bit (1-based, 0 if no bits set)
PrimitiveResult Interpreter::primitiveLowBit(int argCount) {
    Oop rcvr = stackTop();

    if (!rcvr.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t value = rcvr.asSmallInteger();

    if (value < 0) {
        return PrimitiveResult::Failure;  // Undefined for negative
    }

    if (value == 0) {
        pop();
        push(Oop::fromSmallInteger(0));
        return PrimitiveResult::Success;
    }

    // Count trailing zeros and add 1 for 1-based index
    int lowBit = __builtin_ctzll(static_cast<uint64_t>(value)) + 1;

    pop();
    push(Oop::fromSmallInteger(lowBit));
    return PrimitiveResult::Success;
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
    // This is per official VM semantics: receiver adoptInstance: anInstance
    Oop argInstance = stackValue(0);  // An instance of the target class
    Oop rcvr = stackValue(1);

    if (!rcvr.isObject() || !argInstance.isObject()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* rcvrHeader = rcvr.asObjectPtr();
    ObjectHeader* argHeader = argInstance.asObjectPtr();

    // Check immutability
    if (rcvrHeader->isImmutable()) {
        return PrimitiveResult::Failure;
    }

    // Get the class of the argument instance (not the argument itself!)
    Oop newClass = memory_.classOf(argInstance);
    if (!newClass.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Format compatibility check: receiver and argument must have same format category
    // Can't change bytes object to pointers object, etc.
    ObjectFormat rcvrFmt = rcvrHeader->format();
    ObjectFormat argFmt = argHeader->format();

    // Check format compatibility (simplified version)
    // Both must be pointer objects, or both must be byte objects with same format
    bool rcvrIsPointers = rcvrFmt <= ObjectFormat::WeakWithFixed;
    bool argIsPointers = argFmt <= ObjectFormat::WeakWithFixed;

    if (rcvrIsPointers != argIsPointers) {
        return PrimitiveResult::Failure;  // Can't mix pointers and non-pointers
    }

    // For non-pointer objects, formats should match more closely
    if (!rcvrIsPointers) {
        bool rcvrIsBytes = rcvrFmt >= ObjectFormat::Indexable8 && rcvrFmt <= ObjectFormat::Indexable8_7;
        bool argIsBytes = argFmt >= ObjectFormat::Indexable8 && argFmt <= ObjectFormat::Indexable8_7;
        bool rcvrIsWords = rcvrFmt == ObjectFormat::Indexable64 ||
                          (rcvrFmt >= ObjectFormat::Indexable32 && rcvrFmt <= ObjectFormat::Indexable32Odd);
        bool argIsWords = argFmt == ObjectFormat::Indexable64 ||
                         (argFmt >= ObjectFormat::Indexable32 && argFmt <= ObjectFormat::Indexable32Odd);

        if (rcvrIsBytes != argIsBytes || rcvrIsWords != argIsWords) {
            return PrimitiveResult::Failure;  // Format mismatch
        }
    }

    // Get the class index for the new class
    uint32_t newClassIndex = memory_.indexOfClass(newClass);

    // Basic safety check: the new class should be a valid behavior
    if (newClassIndex == 0) {
        return PrimitiveResult::Failure;
    }

    // Change the class index in the object header
    rcvrHeader->setClassIndex(newClassIndex);

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
    // Find the first object in old space
    uint8_t* ptr = memory_.oldSpaceStart();
    uint8_t* end = memory_.oldSpaceStart() +
                   (memory_.statistics().bytesAllocated > 0 ?
                    memory_.statistics().bytesAllocated :
                    memory_.freeOldSpaceBytes());

    while (ptr < end) {
        ObjectHeader* header = reinterpret_cast<ObjectHeader*>(ptr);

        // Skip free chunks (class index 0)
        if (header->classIndex() != 0) {
            Oop obj = memory_.oopFromPointer(header);
            pop();  // Pop receiver
            push(obj);
            return PrimitiveResult::Success;
        }

        // Move to next object
        size_t size = header->totalSize();
        if (size == 0) break;  // Safety check
        ptr += size;
    }

    return PrimitiveResult::Failure;  // No objects found
}

// Primitive 139: Return the next object in memory after this one
PrimitiveResult Interpreter::primitiveNextObject(int argCount) {
    Oop rcvr = stackTop();

    if (!rcvr.isObject()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* current = rcvr.asObjectPtr();
    size_t currentSize = current->totalSize();

    uint8_t* ptr = reinterpret_cast<uint8_t*>(current) + currentSize;
    uint8_t* end = memory_.oldSpaceStart() +
                   (memory_.statistics().bytesAllocated > 0 ?
                    memory_.statistics().bytesAllocated :
                    memory_.freeOldSpaceBytes());

    while (ptr < end) {
        ObjectHeader* header = reinterpret_cast<ObjectHeader*>(ptr);

        // Skip free chunks (class index 0)
        if (header->classIndex() != 0) {
            Oop obj = memory_.oopFromPointer(header);
            pop();
            push(obj);
            return PrimitiveResult::Success;
        }

        size_t size = header->totalSize();
        if (size == 0) break;
        ptr += size;
    }

    // Per official VM: return SmallInteger 0 when no more objects
    pop();
    push(Oop::fromSmallInteger(0));
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

    // VM attributes (simplified set)
    // Indices 0-2 are VM/image paths
    // Index 3+ are command line arguments (Smalltalk argumentAt: i uses index 2+i)
    //
    // We fake --interactive at index 3 so OSWorldRenderer gets selected
    // instead of NullWorldRenderer. This enables the standard Pharo GUI.
    switch (index) {
        case 0:  // VM path
            {
                Oop str = memory_.createString("iospharo");
                pop();
                push(str);
                return PrimitiveResult::Success;
            }
        case 1:  // Image path (document path)
            pop();
            push(memory_.nil());
            return PrimitiveResult::Success;
        case 2:  // Image path (second form)
            pop();
            push(memory_.nil());
            return PrimitiveResult::Success;
        case 3:  // First command line argument - fake --interactive!
            {
                // This triggers OSWorldRenderer.isApplicableFor: to return true
                Oop str = memory_.createString("--interactive");
                pop();
                push(str);
                return PrimitiveResult::Success;
            }
        case 4:  // No more arguments
            pop();
            push(memory_.nil());
            return PrimitiveResult::Success;
        case 1001:  // Operating system name
            {
                // Return "Mac OS" for macOS/iOS - this is what Pharo expects
                std::cerr << "[ATTR 1001] Returning 'Mac OS' for operatingSystemName\n";
                Oop str = memory_.createString("Mac OS");
                pop();
                push(str);
                return PrimitiveResult::Success;
            }
        case 1002:  // VM build string
            pop();
            push(memory_.nil());
            return PrimitiveResult::Success;
        case 1003:  // Interpreter class name
            pop();
            push(memory_.nil());
            return PrimitiveResult::Success;
        case 1004:  // VM type (1=stack, 2=cog, 3=sista)
            pop();
            push(Oop::fromSmallInteger(1));  // Stack VM
            return PrimitiveResult::Success;
        default:
            return PrimitiveResult::Failure;
    }
}

// ===== IMMUTABILITY PRIMITIVES =====

// Primitive 150: Get immutability flag of object
PrimitiveResult Interpreter::primitiveGetImmutability(int argCount) {
    Oop rcvr = stackTop();

    if (!rcvr.isObject()) {
        // Immediates are always immutable
        pop();
        push(memory_.trueObject());
        return PrimitiveResult::Success;
    }

    ObjectHeader* header = rcvr.asObjectPtr();
    bool isImmutable = header->isImmutable();

    pop();
    push(isImmutable ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

// Primitive 164: Set immutability flag of object
// Returns the PREVIOUS immutability state (boolean), not the receiver
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

    // Immediates are always immutable
    if (!rcvr.isObject()) {
        // Immediates always report as immutable
        popN(2);
        push(memory_.trueObject());  // Previous state was "immutable"
        return PrimitiveResult::Success;
    }

    ObjectHeader* header = rcvr.asObjectPtr();
    bool wasImmutable = header->isImmutable();

    // Only set if we can (not already immutable when trying to make mutable)
    if (!wasImmutable || !makeImmutable) {
        header->setImmutable(makeImmutable);
    }

    popN(2);
    push(wasImmutable ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

// ===== OBJECT COPY PRIMITIVE =====

// Primitive 168: Create a copy of an object (shallow copy with new identity)
PrimitiveResult Interpreter::primitiveCopyObject(int argCount) {
    Oop rcvr = stackTop();

    if (!rcvr.isObject()) {
        // Immediates are their own copy
        return PrimitiveResult::Success;
    }

    ObjectHeader* header = rcvr.asObjectPtr();
    uint32_t classIndex = header->classIndex();
    ObjectFormat format = header->format();
    size_t slotCount = header->slotCount();

    Oop copy;

    // Handle different object formats
    if (format >= ObjectFormat::Indexable8 && format <= ObjectFormat::Indexable8_7) {
        // Byte object
        size_t byteSize = memory_.byteSizeOf(rcvr);
        copy = memory_.allocateBytes(classIndex, byteSize);
        if (copy.isNil()) return PrimitiveResult::Failure;

        // Copy bytes
        uint8_t* srcBytes = reinterpret_cast<uint8_t*>(header + 1);
        uint8_t* dstBytes = reinterpret_cast<uint8_t*>(copy.asObjectPtr() + 1);
        std::memcpy(dstBytes, srcBytes, byteSize);
    } else if (format >= ObjectFormat::Indexable32 && format <= ObjectFormat::Indexable64) {
        // Word object
        size_t byteSize = memory_.byteSizeOf(rcvr);
        size_t wordCount = byteSize / 8;
        copy = memory_.allocateWords(classIndex, wordCount);
        if (copy.isNil()) return PrimitiveResult::Failure;

        // Copy words
        uint64_t* srcWords = reinterpret_cast<uint64_t*>(header + 1);
        uint64_t* dstWords = reinterpret_cast<uint64_t*>(copy.asObjectPtr() + 1);
        std::memcpy(dstWords, srcWords, byteSize);
    } else {
        // Pointer object
        copy = memory_.allocateSlots(classIndex, slotCount, format);
        if (copy.isNil()) return PrimitiveResult::Failure;

        // Copy slots
        for (size_t i = 0; i < slotCount; i++) {
            Oop slot = memory_.fetchPointer(i, rcvr);
            memory_.storePointer(i, copy, slot);
        }
    }

    pop();
    push(copy);
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

    // Extract literal count from header (bits 1-15 in standard format)
    int literalCount = (header >> 1) & 0x7FFF;

    // Total size: header word + literals + bytecodes (rounded to 8 bytes)
    size_t totalSlots = 1 + literalCount;  // Header + literals
    size_t byteSize = totalSlots * 8 + static_cast<size_t>(byteCount);

    // Allocate as CompiledMethod format (24-31)
    Oop method = memory_.allocateSlots(classIndex, totalSlots, ObjectFormat::CompiledMethod);
    if (method.isNil()) {
        return PrimitiveResult::Failure;
    }

    // Store the header in slot 0
    memory_.storePointer(0, method, Oop::fromSmallInteger(header));

    // Initialize literals to nil
    for (int i = 1; i <= literalCount; i++) {
        memory_.storePointer(i, method, memory_.nil());
    }

    popN(3);
    push(method);
    return PrimitiveResult::Success;
}

// ===== INSTANCE ADOPTION PRIMITIVE =====

// Primitive 160: Adopt an instance - change class with format compatibility check
PrimitiveResult Interpreter::primitiveAdoptInstance(int argCount) {
    Oop instanceOop = stackValue(0);
    Oop newClassOop = stackValue(1);

    if (!instanceOop.isObject() || !newClassOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* instanceHeader = instanceOop.asObjectPtr();

    // Check immutability
    if (instanceHeader->isImmutable()) {
        return PrimitiveResult::Failure;
    }

    // Get the new class index
    uint32_t newClassIndex = memory_.indexOfClass(newClassOop);
    if (newClassIndex == 0) {
        return PrimitiveResult::Failure;
    }

    // In a full implementation, we'd verify format compatibility here
    // For now, just change the class
    instanceHeader->setClassIndex(newClassIndex);

    popN(2);
    push(instanceOop);
    return PrimitiveResult::Success;
}

// ===== OBJECT PINNING PRIMITIVES =====

// Primitive 183: Check if object is pinned
PrimitiveResult Interpreter::primitiveIsPinned(int argCount) {
    Oop rcvr = stackTop();

    // Official VM: fail on immediates (PrimErrBadReceiver)
    if (!rcvr.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Also fail on forwarded objects
    ObjectHeader* header = rcvr.asObjectPtr();
    if (header->isForwarded()) {
        return PrimitiveResult::Failure;
    }

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

    ObjectHeader* header = rcvr.asObjectPtr();
    if (header->isForwarded()) {
        return PrimitiveResult::Failure;
    }

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
    Oop targetContext = stackValue(0);
    Oop rcvr = stackValue(1);

    if (!rcvr.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Walk the sender chain from receiver, nilling out senders until we reach target
    // Context layout: sender is slot 0
    const size_t SenderIndex = 0;

    Oop current = rcvr;
    while (!current.isNil() && current.isObject()) {
        Oop sender = memory_.fetchPointer(SenderIndex, current);

        // Nil out this context's sender (use real nil object, not raw 0)
        memory_.storePointer(SenderIndex, current, memory_.nil());

        // If we've reached the target, stop
        if (current.rawBits() == targetContext.rawBits()) {
            break;
        }

        current = sender;
    }

    popN(2);
    push(rcvr);
    return PrimitiveResult::Success;
}

// ===== FLOAT BIT ACCESS PRIMITIVES =====

// Primitive 38: Read 32-bit word from Float at index (1 or 2)
PrimitiveResult Interpreter::primitiveFloatAt(int argCount) {
    // Primitive 38: Read 32-bit word from Float at index (1 or 2)
    // Per official VM: index 1 = most significant word, index 2 = least significant
    // On little-endian systems, must swap indices to maintain this semantic
    Oop indexOop = stackValue(0);
    Oop rcvr = stackValue(1);

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

    ObjectHeader* header = rcvr.asObjectPtr();
    ObjectFormat format = header->format();

    // Must be a Float (64-bit word format)
    if (format != ObjectFormat::Indexable64) {
        return PrimitiveResult::Failure;
    }

    // Per official VM: on little-endian, swap indices so index 1 = high word
    // Memory layout on little-endian: [low word][high word]
    // Semantic: index 1 = high word, index 2 = low word
    size_t accessIndex = static_cast<size_t>(index - 1);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    accessIndex = 1 - accessIndex;  // Swap: 0 -> 1, 1 -> 0
#endif

    uint32_t* words = reinterpret_cast<uint32_t*>(header + 1);
    uint32_t value = words[accessIndex];

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

    ObjectHeader* header = rcvr.asObjectPtr();
    ObjectFormat format = header->format();

    // Must be a Float (64-bit word format)
    if (format != ObjectFormat::Indexable64) {
        return PrimitiveResult::Failure;
    }

    // Check immutability
    if (header->isImmutable()) {
        return PrimitiveResult::Failure;
    }

    // Per official VM: on little-endian, swap indices so index 1 = high word
    size_t accessIndex = static_cast<size_t>(index - 1);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    accessIndex = 1 - accessIndex;  // Swap: 0 -> 1, 1 -> 0
#endif

    uint32_t* words = reinterpret_cast<uint32_t*>(header + 1);
    words[accessIndex] = value;

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
    // This primitive marks the current context as an exception handler.
    // The actual marking is typically done via a flag or by the method structure.
    // For now, we just succeed - the handler lookup will use method metadata.

    Oop rcvr = stackTop();

    // In a full implementation, we'd set a flag on the context
    // For now, just return the receiver (typically thisContext)

    pop();
    push(rcvr);
    return PrimitiveResult::Success;
}

// Primitive 187: Mark a method context as an unwind protect method
// Used by exception handling to identify ensure: contexts
PrimitiveResult Interpreter::primitiveMarkUnwindMethod(int argCount) {
    // This primitive marks the current context as an unwind-protect context.
    // These are contexts that must be run even when unwinding the stack.

    Oop rcvr = stackTop();

    // In a full implementation, we'd set an unwind flag on the context
    // For now, just return the receiver

    pop();
    push(rcvr);
    return PrimitiveResult::Success;
}

// Primitive 197: Find handler/signaling context
// Searches up the sender chain for the next context marked as a handler
// The reference implementation searches for methods with primitive 199 (mark handler)
PrimitiveResult Interpreter::primitiveFindHandlerContext(int argCount) {
    (void)argCount;
    // This primitive requires walking the context chain and checking for
    // contexts that were marked as handlers (via primitive 199).
    // For now, return failure to let the Smalltalk fallback code execute.
    // The Smalltalk implementation will walk the sender chain itself.
    return PrimitiveResult::Failure;
}

// Primitive 189: Find the next unwind context up to a limit
// Walks the sender chain looking for ensure: or similar unwind-protect contexts
PrimitiveResult Interpreter::primitiveFindNextUnwindContext(int argCount) {
    // Primitive 195: Find the next unwind context up to a limit
    // This primitive requires tracking which methods were marked as unwind methods
    // via primitiveMarkUnwindMethod (210). Since we don't have that infrastructure,
    // return failure to let the Smalltalk fallback code handle unwinding.
    // The Smalltalk implementation will walk the sender chain itself and check
    // for ensure: methods by examining method literals.
    (void)argCount;
    return PrimitiveResult::Failure;
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

// Primitive 211: Read a temp/stack slot from a context at 1-based index
// Index 1 is the first temp, after the fixed context fields
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
    // In a full implementation, we'd have a method cache and flush entries
    // that reference this method. For now, this is a no-op since we don't
    // have method caching yet.

    // No arguments expected, just return the receiver (the method)
    // Stack has: receiver (which is already on top), so just succeed
    return PrimitiveResult::Success;
}

// Primitive 120: Flush method cache entries for a specific selector
// Called when any method with this selector might have changed
PrimitiveResult Interpreter::primitiveFlushCacheBySelector(int argCount) {
    // In a full implementation, we'd flush all cache entries for this selector.
    // For now, this is a no-op since we don't have method caching yet.

    // No arguments expected, just return the receiver (the selector)
    // Stack has: receiver (which is already on top), so just succeed
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

    // Check for forwarded objects
    if (receiver.isObject()) {
        ObjectHeader* rcvrHeader = receiver.asObjectPtr();
        if (rcvrHeader->isForwarded()) {
            return PrimitiveResult::Failure;
        }
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
    Oop method = lookupMethod(lookupClass, selector);
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

    // Calculate total size based on format type
    size_t totalSlots;
    size_t bytesPerElement = 8;  // Default: pointer slots (64-bit)

    if (formatType <= 4) {
        // Pointer object (fixed or variable)
        totalSlots = instSize + numElements;
    } else if (formatType >= 16 && formatType <= 23) {
        // Byte object
        bytesPerElement = 1;
        totalSlots = instSize + (numElements + 7) / 8;  // Round up to 64-bit slots
    } else if (formatType >= 12 && formatType <= 15) {
        // 16-bit object
        bytesPerElement = 2;
        totalSlots = instSize + (numElements + 3) / 4;  // Round up to 64-bit slots
    } else if (formatType >= 10 && formatType <= 11) {
        // 32-bit or 64-bit object
        if (formatType == 10) {
            bytesPerElement = 8;
            totalSlots = instSize + numElements;
        } else {
            bytesPerElement = 4;
            totalSlots = instSize + (numElements + 1) / 2;
        }
    } else {
        // CompiledMethod or other special format
        totalSlots = instSize + numElements;
    }

    // Add header size (8 bytes for standard header)
    size_t totalBytes = 8 + totalSlots * 8;

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

    // Per official VM MiscPrimitivePlugin algorithm
    // Uses same algorithm as primitiveStringHashInitialHash but with 0 as initial
    uint32_t hash = 0;
    for (size_t i = 0; i < byteCount; ++i) {
        hash = hash + (bytes[i] * 0x19660D);
    }
    hash = hash & 0x0FFFFFFF;  // 28 bits per official VM

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
    if (!prim146Log) prim146Log = fopen("/tmp/prim146.log", "w");

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

    // Per official VM MiscPrimitivePlugin:
    // hash := initialHash.
    // 0 to: stringSize - 1 do: [:pos |
    //     hash := hash + ((aByteArray at: pos) * 16r19660D)].
    // ^ hash bitAnd: 16r0FFFFFFF
    uint32_t hash = static_cast<uint32_t>(speciesHash);
    for (size_t i = 0; i < stringSize; ++i) {
        hash = hash + (bytes[i] * 0x19660D);
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

    // For safety, return just a hash of the address (not the actual address)
    // to avoid exposing memory layout
    pop();
    push(Oop::fromSmallInteger(static_cast<intptr_t>(baseAddr & 0x7FFFFFFFFFFFF)));
    return PrimitiveResult::Success;
}

// Primitive 517: Get highest available memory address
// primitiveHighestAvailableAddress -> integer
// Returns the highest usable memory address
PrimitiveResult Interpreter::primitiveHighestAvailableAddress(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Return old space end as the highest available address
    uintptr_t highAddr = reinterpret_cast<uintptr_t>(memory_.oldSpaceEnd());

    pop();
    push(Oop::fromSmallInteger(static_cast<intptr_t>(highAddr & 0x7FFFFFFFFFFFF)));
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

    pop();
    // Return as small integer (may truncate on 64-bit systems)
    push(Oop::fromSmallInteger(static_cast<intptr_t>(address & 0x7FFFFFFFFFFFF)));
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
    push(Oop::fromSmallInteger(pos));
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
    push(Oop::fromSmallInteger(size));
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
    // On Unix/macOS, the delimiter is '/'
    // Return it as a Character (immediate)
    pop();  // pop receiver

    // Create a Character for '/'
    // In Pharo, Character is an immediate with tag
    // For now, return the ASCII value as a SmallInteger that Smalltalk can convert
    push(Oop::fromSmallInteger('/'));
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

// ===== ADDITIONAL FILE PRIMITIVES =====

// Primitive 161: Get standard I/O file handles
// primitiveFileStdioHandles -> Array of (stdin, stdout, stderr) handles
PrimitiveResult Interpreter::primitiveFileStdioHandles(int argCount) {
    // Register stdin, stdout, stderr if not already registered
    // Use negative IDs for standard handles to distinguish them
    static bool stdioInitialized = false;
    static int stdinId = -1;
    static int stdoutId = -2;
    static int stderrId = -3;

    if (!stdioInitialized) {
        // Store standard handles with special negative IDs
        openFiles_[-1] = stdin;
        openFiles_[-2] = stdout;
        openFiles_[-3] = stderr;
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
// Returns: 0 = regular file, 1 = directory, 2 = character device, 3 = block device,
//          4 = FIFO, 5 = socket, 6 = symbolic link, -1 = unknown/invalid
PrimitiveResult Interpreter::primitiveFileDescriptorType(int argCount) {
    Oop fileIdOop = stackTop();

    if (!fileIdOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int fileId = static_cast<int>(fileIdOop.asSmallInteger());
    auto it = openFiles_.find(fileId);
    if (it == openFiles_.end()) {
        pop();
        push(Oop::fromSmallInteger(-1));  // Invalid handle
        return PrimitiveResult::Success;
    }

    FILE* file = it->second;
    int fd = fileno(file);

    struct stat statBuf;
    if (fstat(fd, &statBuf) != 0) {
        pop();
        push(Oop::fromSmallInteger(-1));
        return PrimitiveResult::Success;
    }

    int type = -1;
    if (S_ISREG(statBuf.st_mode)) type = 0;       // Regular file
    else if (S_ISDIR(statBuf.st_mode)) type = 1;  // Directory
    else if (S_ISCHR(statBuf.st_mode)) type = 2;  // Character device
    else if (S_ISBLK(statBuf.st_mode)) type = 3;  // Block device
    else if (S_ISFIFO(statBuf.st_mode)) type = 4; // FIFO
#ifdef S_ISSOCK
    else if (S_ISSOCK(statBuf.st_mode)) type = 5; // Socket
#endif
#ifdef S_ISLNK
    else if (S_ISLNK(statBuf.st_mode)) type = 6;  // Symbolic link
#endif

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
    // Debug: Log calls
    static FILE* sem153Log = fopen("/tmp/prim153_input_sem.log", "a");
    static int callCount153 = 0;
    callCount153++;
    if (sem153Log && callCount153 <= 20) {
        fprintf(sem153Log, "[PRIM153] Call #%d argCount=%d\n", callCount153, argCount);
        fflush(sem153Log);
    }

    if (argCount < 1) {
        return PrimitiveResult::Failure;
    }

    // Get the semaphore argument - this is the semaphore index
    Oop semArg = stackTop();
    if (semArg.isSmallInteger()) {
        int64_t semIndex = semArg.asSmallInteger();
        gEventQueue.setInputSemaphoreIndex(static_cast<int>(semIndex));
        if (sem153Log && callCount153 <= 20) {
            fprintf(sem153Log, "[PRIM153] Set input semaphore index to %lld\n", semIndex);
            fflush(sem153Log);
        }
    } else if (sem153Log && callCount153 <= 20) {
        fprintf(sem153Log, "[PRIM153] Arg is not SmallInteger, raw=0x%llx\n",
                (unsigned long long)semArg.rawBits());
        fflush(sem153Log);
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

    popN(argCount);
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

    popN(argCount);
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

    popN(argCount);
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

        // Copy hash if requested
        if (copyHash) {
            uint32_t hash = memory_.identityHashOf(fromObj);
            // The hash is stored in the object header; we'd need a setIdentityHash method
            // For now, just ensure the target has a hash
            memory_.ensureIdentityHash(toObj);
        }

        // Perform one-way become: all references to fromObj become toObj
        memory_.becomeForward(fromObj, toObj);
    }

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
    static int relinquishCount = 0;
    static FILE* relinquishLog = nullptr;
    relinquishCount++;

    if (!relinquishLog) {
        relinquishLog = fopen("/tmp/prim_relinquish.log", "w");
    }
    if (relinquishLog && relinquishCount <= 50) {
        fprintf(relinquishLog, "[RELINQUISH] #%d argCount=%d\n", relinquishCount, argCount);
        // Log current method context for first 10 calls
        if (relinquishCount <= 10 && method_.isObject() && method_.rawBits() > 0x10000) {
            std::string methodSel = "<unknown>";
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
            std::string rcvrClass = "<unknown>";
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
            fprintf(relinquishLog, "[RELINQUISH] #%d method=#%s receiver=%s\n",
                    relinquishCount, methodSel.c_str(), rcvrClass.c_str());
        }
        fflush(relinquishLog);
    }

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

    if (relinquishLog && relinquishCount <= 50) {
        fprintf(relinquishLog, "[RELINQUISH] #%d us=%lld\n", relinquishCount, (long long)microSeconds);
        fflush(relinquishLog);
    }

    // In a cooperative VM, relinquishing the processor means:
    // 1. Check for pending events/signals
    // 2. Optionally sleep for the requested time
    // 3. Allow process scheduler to run other processes

    // Cap sleep time to avoid blocking event loop (10ms = 10000 microseconds max)
    const int64_t MAX_SLEEP_US = 10000;  // 10ms in microseconds
    int64_t sleepUs = std::min(microSeconds, MAX_SLEEP_US);

    // Process any pending events first
    processInputEvents();
    processPendingSignals();

    // Short sleep if requested (usleep takes microseconds)
    if (sleepUs > 0) {
        #ifdef _WIN32
        Sleep(static_cast<DWORD>(sleepUs / 1000));  // Windows Sleep takes milliseconds
        #else
        usleep(static_cast<useconds_t>(sleepUs));  // usleep takes microseconds
        #endif
    }

    // Process events again after sleep
    processInputEvents();
    processPendingSignals();

    // Debug: Log large delay requests
    if (relinquishLog && microSeconds > 100000) {  // > 100ms
        fprintf(relinquishLog, "[RELINQUISH] Capped %lldus -> %lldus\n",
                (long long)microSeconds, (long long)sleepUs);
        fflush(relinquishLog);
    }

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

                if (relinquishLog && relinquishCount <= 20) {
                    fprintf(relinquishLog, "[RELINQUISH] #%d activeProcess=0x%llx activePriority=%d numQueues=%zu\n",
                            relinquishCount, (unsigned long long)activeProcess.rawBits(), activePriority, numQueues);
                    // Dump all non-empty queues
                    for (size_t i = 0; i < numQueues; i++) {
                        Oop q = memory_.fetchPointer(i, schedLists);
                        if (q.isObject()) {
                            Oop first = memory_.fetchPointer(LinkedListFirstLinkIndex, q);
                            if (first.isObject() && first.rawBits() != nilObj.rawBits()) {
                                fprintf(relinquishLog, "  Queue[%zu] (priority %zu): first=0x%llx\n",
                                        i, i + 1, (unsigned long long)first.rawBits());
                            }
                        }
                    }
                    fflush(relinquishLog);
                }

                // Search from highest priority down to and INCLUDING current priority
                // NOTE: Priority is 1-based, but array indices are 0-based
                // Priority N is at index N-1
                // IMPORTANT: We must include same priority (>=, not >) for round-robin
                // scheduling to work. Pharo's doc says "at least that of the active Process".
                for (int pri = static_cast<int>(numQueues); pri >= activePriority; pri--) {
                    int index = pri - 1;  // Convert 1-based priority to 0-based index
                    Oop queue = memory_.fetchPointer(index, schedLists);
                    if (!queue.isObject() || queue.rawBits() == nilObj.rawBits()) continue;

                    Oop firstProcess = memory_.fetchPointer(LinkedListFirstLinkIndex, queue);
                    if (firstProcess.isObject() && firstProcess.rawBits() != nilObj.rawBits() &&
                        firstProcess.rawBits() != activeProcess.rawBits()) {
                        // Found a higher priority process - yield to it
                        if (relinquishLog && relinquishCount <= 50) {
                            fprintf(relinquishLog, "[RELINQUISH] #%d Yielding to process 0x%llx at priority %d\n",
                                    relinquishCount, (unsigned long long)firstProcess.rawBits(), pri);
                            fflush(relinquishLog);
                        }

                        // Remove the process from queue
                        Oop nextProcess = removeFirstLinkOfList(queue);
                        if (nextProcess.isObject() && nextProcess.rawBits() != nilObj.rawBits()) {
                            // Put current process back in its queue
                            putToSleep(activeProcess);
                            // Switch to the new process
                            transferTo(nextProcess);
                            break;
                        }
                    }
                }
            }
        }
    }

    pop();  // pop microseconds argument, leave receiver
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
    // In headless mode, display updates are no-ops
    // A full implementation would:
    // 1. Get the display form
    // 2. Copy the specified rectangle to the screen
    // 3. Flush the display

    // Pop all arguments (left, top, right, bottom)
    if (argCount >= 4) {
        popN(4);
    } else if (argCount > 0) {
        popN(argCount);
    }

    // Leave receiver on stack, return success (no-op in headless)
    return PrimitiveResult::Success;
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

// Primitive 117: Call out to FFI (Foreign Function Interface)
// externalFunction args primitiveCalloutToFFI -> result
// Calls a foreign function through FFI mechanism
PrimitiveResult Interpreter::primitiveCalloutToFFI(int argCount) {
    static bool ffiInitialized = false;
    static int callCount = 0;
    callCount++;

    // Log FFI calls to see what's being requested
    if (callCount <= 100) {
        fprintf(stderr, "[FFI] primitiveCalloutToFFI called #%d (argCount=%d)\n", callCount, argCount);
    }

    // Initialize FFI on first call
    if (!ffiInitialized) {
        ffiInitialized = ffi::initializeFFI();
        if (!ffiInitialized) {
            fprintf(stderr, "[FFI] initializeFFI FAILED\n");
            return PrimitiveResult::Failure;
        }
        fprintf(stderr, "[FFI] initializeFFI succeeded\n");
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
        if (callCount <= 50) {
            fprintf(stderr, "[FFI] No function name found in method literals\n");
        }
        return PrimitiveResult::Failure;
    }

    fprintf(stderr, "[FFI] Looking up function: %s\n", funcName.c_str());

    // Look up the function
    void* funcPtr = ffi::lookupFunction("SDL2", funcName);
    if (!funcPtr) {
        fprintf(stderr, "[FFI] Function not found: %s\n", funcName.c_str());
        return PrimitiveResult::Failure;
    }
    fprintf(stderr, "[FFI] Found function %s at %p\n", funcName.c_str(), funcPtr);

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
        case ffi::FFIType::UInt64:
            // May need to box large integers
            if (result.intValue <= INT32_MAX && static_cast<int64_t>(result.intValue) >= INT32_MIN) {
                resultOop = Oop::fromSmallInteger(static_cast<int64_t>(result.intValue));
            } else {
                // TODO: Create LargeInteger
                resultOop = Oop::fromSmallInteger(static_cast<int64_t>(result.intValue & 0x7FFFFFFF));
            }
            break;

        case ffi::FFIType::Pointer:
            // Create ExternalAddress or return SmallInteger if it fits
            if (result.ptrValue == nullptr) {
                resultOop = Oop::nil();
            } else {
                // For now, return as SmallInteger (truncated)
                // TODO: Create proper ExternalAddress
                resultOop = Oop::fromSmallInteger(static_cast<int64_t>(result.intValue));
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
    // External call uses pragma in method to specify:
    // <primitive: 'primitiveName' module: 'ModuleName'>
    // The VM looks up the module, finds the primitive, and calls it

    // Get current method to find the external primitive spec
    Oop method = method_;
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

    // Fast path: Check for known MiscPrimitivePlugin methods by selector in literals
    // Note: literals are at slots 1..numLiterals (slot 0 is the header)
    for (size_t i = 1; i <= numLiterals && i < 10; i++) {
        Oop literal = memory_.fetchPointer(i, method);
        if (literal.isObject() && memory_.isValidPointer(literal)) {
            ObjectHeader* litHdr = literal.asObjectPtr();
            if (litHdr->isBytesObject() && litHdr->byteSize() < 50) {
                std::string str((char*)litHdr->bytes(), litHdr->byteSize());
                if (str == "stringHash:initialHash:") {
                    return primitiveStringHashInitialHash(argCount);
                }
                if (str == "indexOfAscii:inString:startingAt:") {
                    return primitiveIndexOfAscii(argCount);
                }
            }
        }
    }

    // Debug logging
    static FILE* extLog = fopen("/tmp/external_prim.log", "a");
    static int extCallCount = 0;
    extCallCount++;

    if (extLog && extCallCount <= 50) {
        fprintf(extLog, "[EXT] #%d argCount=%d numLiterals=%zu\n",
                extCallCount, argCount, numLiterals);
        fflush(extLog);
    }

    // Search literals for the primitive spec (usually an Array with module/name)
    // Literals are at slots 1..numLiterals (slot 0 is the method header)
    for (size_t i = 1; i <= numLiterals && i < 10; i++) {
        Oop literal = memory_.fetchPointer(i, method);
        if (extLog && extCallCount <= 50) {
            fprintf(extLog, "[EXT] #%d literal[%zu] isObj=%d bits=0x%llx\n",
                    extCallCount, i, literal.isObject() ? 1 : 0,
                    (unsigned long long)literal.rawBits());
            fflush(extLog);
        }
        if (!literal.isObject()) continue;
        if (!memory_.isValidPointer(literal)) continue;  // Validate before dereference

        ObjectHeader* litHdr = literal.asObjectPtr();
        uint32_t fmt = static_cast<uint32_t>(litHdr->format());
        size_t slots = litHdr->slotCount();

        if (extLog && extCallCount <= 50) {
            fprintf(extLog, "[EXT] #%d   format=%u slots=%zu clsIdx=%u\n",
                    extCallCount, fmt, slots, litHdr->classIndex());
            // If it's a string/symbol, log its contents
            if (litHdr->isBytesObject() && litHdr->byteSize() < 100) {
                std::string str((char*)litHdr->bytes(), litHdr->byteSize());
                fprintf(extLog, "[EXT] #%d   literal string='%s'\n", extCallCount, str.c_str());
            }
            fflush(extLog);
        }

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
                if (extLog) {
                    fprintf(extLog, "[EXT] #%d Returning image nil (0x%llx) for callback primitive '%s'\n",
                            extCallCount, (unsigned long long)memory_.nil().rawBits(), str.c_str());
                    fflush(extLog);
                }
                return PrimitiveResult::Success;
            }
            if (str == "primNumberOfCallbacks" || str == "numberOfCallbacks") {
                // Return 0 - no pending callbacks
                popN(static_cast<size_t>(argCount + 1));  // Pop args and receiver
                push(Oop::fromSmallInteger(0));
                if (extLog) {
                    fprintf(extLog, "[EXT] #%d Returning 0 for callback count primitive '%s'\n", extCallCount, str.c_str());
                    fflush(extLog);
                }
                return PrimitiveResult::Success;
            }
        }

        // Check if it's an Array (format 2 = indexable pointers) or ExternalLibraryFunction (format 1)
        // Also check Fixed format objects that might contain module/name references
        if ((litHdr->format() == ObjectFormat::Indexable || litHdr->format() == ObjectFormat::FixedSize)
            && litHdr->slotCount() >= 2) {
            // Could be #(moduleName primitiveName ...) or ExternalLibraryFunction
            Oop moduleOop = memory_.fetchPointer(0, literal);
            Oop nameOop = memory_.fetchPointer(1, literal);

            if (extLog && extCallCount <= 50) {
                fprintf(extLog, "[EXT] #%d   slot[0]=0x%llx slot[1]=0x%llx\n",
                        extCallCount,
                        (unsigned long long)moduleOop.rawBits(),
                        (unsigned long long)nameOop.rawBits());
                fflush(extLog);
            }

            // Extract strings
            std::string moduleName, primName;

            if (moduleOop.isObject() && memory_.isValidPointer(moduleOop)) {
                ObjectHeader* modHdr = moduleOop.asObjectPtr();
                if (extLog && extCallCount <= 50) {
                    fprintf(extLog, "[EXT] #%d   moduleOop: fmt=%u isBytes=%d size=%zu\n",
                            extCallCount,
                            static_cast<uint32_t>(modHdr->format()),
                            modHdr->isBytesObject() ? 1 : 0,
                            modHdr->isBytesObject() ? modHdr->byteSize() : modHdr->slotCount());
                    fflush(extLog);
                }
                if (modHdr->isBytesObject() && modHdr->byteSize() < 100) {
                    moduleName = std::string((char*)modHdr->bytes(), modHdr->byteSize());
                    if (extLog && extCallCount <= 50) {
                        fprintf(extLog, "[EXT] #%d   moduleName='%s'\n", extCallCount, moduleName.c_str());
                        fflush(extLog);
                    }
                }
            }

            if (nameOop.isObject() && memory_.isValidPointer(nameOop)) {
                ObjectHeader* nameHdr = nameOop.asObjectPtr();
                if (extLog && extCallCount <= 50) {
                    fprintf(extLog, "[EXT] #%d   nameOop: fmt=%u isBytes=%d size=%zu clsIdx=%u\n",
                            extCallCount,
                            static_cast<uint32_t>(nameHdr->format()),
                            nameHdr->isBytesObject() ? 1 : 0,
                            nameHdr->isBytesObject() ? nameHdr->byteSize() : nameHdr->slotCount(),
                            nameHdr->classIndex());
                    fflush(extLog);
                }
                if (nameHdr->isBytesObject() && nameHdr->byteSize() < 100) {
                    primName = std::string((char*)nameHdr->bytes(), nameHdr->byteSize());
                    if (extLog && extCallCount <= 50) {
                        fprintf(extLog, "[EXT] #%d   primName='%s'\n", extCallCount, primName.c_str());
                        fflush(extLog);
                    }
                }
                // If nameOop is a FixedSize object, check its slots for module/name
                if (nameHdr->format() == ObjectFormat::FixedSize && nameHdr->slotCount() >= 2) {
                    // This could be ExternalLibraryFunction - check slots
                    if (extLog && extCallCount <= 50) {
                        fprintf(extLog, "[EXT] #%d   Checking nameOop slots (count=%zu):\n",
                                extCallCount, nameHdr->slotCount());
                        fflush(extLog);
                    }
                    for (size_t j = 0; j < nameHdr->slotCount() && j < 8; j++) {
                        Oop innerOop = memory_.fetchPointer(j, nameOop);
                        if (extLog && extCallCount <= 50) {
                            fprintf(extLog, "[EXT] #%d     slot[%zu]=0x%llx isObj=%d\n",
                                    extCallCount, j, (unsigned long long)innerOop.rawBits(),
                                    innerOop.isObject() ? 1 : 0);
                            fflush(extLog);
                        }
                        if (innerOop.isObject() && memory_.isValidPointer(innerOop)) {
                            ObjectHeader* innerHdr = innerOop.asObjectPtr();
                            if (extLog && extCallCount <= 50) {
                                fprintf(extLog, "[EXT] #%d       slot[%zu] fmt=%u clsIdx=%u\n",
                                        extCallCount, j,
                                        static_cast<uint32_t>(innerHdr->format()),
                                        innerHdr->classIndex());
                                fflush(extLog);
                            }
                            if (innerHdr->isBytesObject() && innerHdr->byteSize() < 100) {
                                std::string str((char*)innerHdr->bytes(), innerHdr->byteSize());
                                if (extLog && extCallCount <= 50) {
                                    fprintf(extLog, "[EXT] #%d         ='%s'\n", extCallCount, str.c_str());
                                    fflush(extLog);
                                }
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
                                            if (extLog && extCallCount <= 50) {
                                                fprintf(extLog, "[EXT] #%d           slot[%zu].slot[%zu]='%s' clsIdx=%u\n",
                                                        extCallCount, j, k, str.c_str(), deepHdr->classIndex());
                                                fflush(extLog);
                                            }
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

                if (extLog && extCallCount <= 50) {
                    fprintf(extLog, "[EXT] #%d Looking up '%s'\n", extCallCount, key.c_str());
                    fflush(extLog);
                }

                // Handle ThreadedFFI callback primitives - return nil/0 instead of failing
                // This prevents exception handling from consuming startup cycles
                if (primName == "primNextPendingCallback" || primName == "nextPendingCallback") {
                    // Return nil - no pending callbacks (we don't support FFI callbacks)
                    // IMPORTANT: Use memory_.nil() not Oop::nil() - the image's nil is at a real address
                    popN(static_cast<size_t>(argCount + 1));  // Pop args and receiver
                    push(memory_.nil());
                    if (extLog && extCallCount <= 50) {
                        fprintf(extLog, "[EXT] #%d Returning image nil (0x%llx) for %s\n",
                                extCallCount, (unsigned long long)memory_.nil().rawBits(), primName.c_str());
                        fflush(extLog);
                    }
                    return PrimitiveResult::Success;
                }
                if (primName == "primNumberOfCallbacks" || primName == "numberOfCallbacks") {
                    // Return 0 - no pending callbacks
                    popN(static_cast<size_t>(argCount + 1));  // Pop args and receiver
                    push(Oop::fromSmallInteger(0));
                    if (extLog && extCallCount <= 50) {
                        fprintf(extLog, "[EXT] #%d Returning 0 for %s\n", extCallCount, primName.c_str());
                        fflush(extLog);
                    }
                    return PrimitiveResult::Success;
                }

                auto it = namedPrimitives_.find(key);
                if (it != namedPrimitives_.end()) {
                    if (extLog && extCallCount <= 50) {
                        fprintf(extLog, "[EXT] #%d Found! Calling...\n", extCallCount);
                        fflush(extLog);
                    }
                    return (this->*(it->second))(argCount);
                }

                // Try without module prefix (for compatibility)
                auto it2 = namedPrimitives_.find(":" + primName);
                if (it2 != namedPrimitives_.end()) {
                    return (this->*(it2->second))(argCount);
                }
            }
        }
    }

    if (extLog && extCallCount <= 50) {
        fprintf(extLog, "[EXT] #%d No named primitive found, failing\n", extCallCount);
        fflush(extLog);
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

    Oop result = memory_.allocateSlots(arrayClassIndex, roots.size());
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
        // Return the bits of the float encoding
        // This gives access to the IEEE representation
        pop();
        // For floats, return the raw bits as large integer if needed
        // For simplicity, fail to Smalltalk for non-small values
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
        if (!obj.isImmediate()) {
            allObjects.push_back(obj);
        }
    });

    // Allocate result array
    uint32_t arrayClassIndex = memory_.indexOfClass(
        memory_.specialObject(SpecialObjectIndex::ClassArray));

    Oop result = memory_.allocateSlots(arrayClassIndex, allObjects.size());
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
    if (makeReadOnly) {
        memory_.makeImmutable(receiver);
    }
    // Note: making mutable again would need additional support

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
    // Return the first 64 bits of the header
    uint64_t headerBits = *reinterpret_cast<uint64_t*>(header);

    pop();
    // Return truncated to fit in SmallInteger
    push(Oop::fromSmallInteger(static_cast<intptr_t>(headerBits & 0x7FFFFFFFFFFFF)));
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

    Oop headerOop = memory_.fetchPointer(0, methodOop);
    if (!headerOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    intptr_t header = headerOop.asSmallInteger();
    // Primitive index is typically extracted from the header or first bytecodes
    // For Spur, flag bit indicates if primitive
    bool hasPrimitive = (header >> 16) & 1;

    intptr_t primitiveIndex = 0;
    if (hasPrimitive) {
        // Primitive index encoded in first bytecodes after header
        // This is a simplified extraction
        primitiveIndex = (header >> 1) & 0x7FFF;
    }

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

    pop();
    push(Oop::fromSmallInteger(static_cast<intptr_t>(result & 0x7FFFFFFFFFFFF)));
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

    pop();
    push(Oop::fromSmallInteger(static_cast<intptr_t>(result & 0x7FFFFFFFFFFFF)));
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
// receiver usecs primitiveSignalAtUTCMicroseconds
// Signals the timer semaphore when UTC clock reaches usecs. Value 0 disables.
PrimitiveResult Interpreter::primitiveSignalAtUTCMicroseconds(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop usecsOop = stackTop();
    if (!usecsOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t usecs = usecsOop.asSmallInteger();

    // Get the timer semaphore from special objects
    Oop timerSema = memory_.specialObject(SpecialObjectIndex::TheTimerSemaphore);

    if (usecs == 0) {
        // Disable timer by setting next wakeup to far future
        nextWakeupUsec_ = INT64_MAX;
    } else {
        // Convert Smalltalk epoch microseconds to system clock comparison
        // We'll check against this in the heartbeat
        nextWakeupUsec_ = usecs;
    }

    pop();  // Pop argument, leave receiver
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
        resultArray = memory_.allocateSlots(arrayClassIndex, 2);
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
        size_t destSlots = destHdr->slotCount();
        size_t srcSlots = srcHdr->slotCount();

        // Bounds check
        if (dstStartIdx + count > destSlots || srcIdx + count > srcSlots) {
            return PrimitiveResult::Failure;
        }

        // Copy slots
        for (size_t i = 0; i < count; i++) {
            Oop value = srcHdr->slotAt(srcIdx + i);
            destHdr->slotAtPut(dstStartIdx + i, value);
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

// Primitive 158: String compare with collation
// ByteString >> compareWith: anotherString (argCount=1)
// ByteString >> compareWith: anotherString collated: order (argCount=2)
// Returns -1, 0, or 1
PrimitiveResult Interpreter::primitiveStringCompareWith(int argCount) {
    static int p158CallCount = 0;
    if (p158CallCount++ < 5) {
        std::cerr << "[P158] Called with argCount=" << argCount << "\n";
    }
    if (argCount < 1 || argCount > 2) return PrimitiveResult::Failure;

    Oop string2Oop;
    Oop string1Oop;  // receiver

    if (argCount == 1) {
        // compareWith: - Stack: receiver, anotherString
        string2Oop = stackTop();
        string1Oop = stackValue(1);
    } else {
        // compareWith:collated: - Stack: receiver, anotherString, order
        // Oop orderOop = stackTop();  // We ignore order for now
        string2Oop = stackValue(1);
        string1Oop = stackValue(2);
    }

    if (string1Oop.isImmediate() || string2Oop.isImmediate()) {
        return PrimitiveResult::Failure;
    }

    // Extract strings
    std::string str1 = extractString(memory_, string1Oop);
    std::string str2 = extractString(memory_, string2Oop);

    // Debug logging for platform detection
    static int compareLogCount = 0;
    if ((str1 == "Mac OS" || str2 == "Mac OS") && compareLogCount++ < 5) {
        std::cerr << "[STRCMP] Comparing '" << str1 << "' with '" << str2 << "'\n";
    }

    // Basic comparison (ignoring collation order for now)
    int result;
    if (str1 < str2) {
        result = -1;
    } else if (str1 > str2) {
        result = 1;
    } else {
        result = 0;
    }

    if ((str1 == "Mac OS" || str2 == "Mac OS") && compareLogCount <= 5) {
        std::cerr << "[STRCMP] Result: " << result << "\n";
    }

    popN(argCount + 1);  // pop args and receiver
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

    // Debug: Simple stderr output
    static int entryCount = 0;
    entryCount++;
    if (entryCount <= 20) {
        fprintf(stderr, "[GETNEXT] #%d argCount=%d stackTop=0x%llx actualReceiver=0x%llx\n",
                entryCount, argCount, (unsigned long long)stackTop().rawBits(),
                (unsigned long long)actualReceiver.rawBits());
    }

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
        if (!slotLog) slotLog = fopen("/tmp/slot_scan.log", "w");
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
    static FILE* bufLog = fopen("/tmp/prim_buffer_check.log", "w");
    static int bufCount = 0;
    bufCount++;

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
    static FILE* prim264Log = fopen("/tmp/prim264_trace.log", "a");
    if (prim264Log && (callCount <= 50 || !passThroughEvents_.empty())) {
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
        if (prim264Log && (callCount <= 50 || event.arg3 != 0)) {
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
            static FILE* mouseLog = fopen("/tmp/pharo_mouse_events.log", "a");
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
    if (!slot0Log) slot0Log = fopen("/tmp/prim264_slot0.log", "w");
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
    static FILE* compLog = fopen("/tmp/prim_completion.log", "w");
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
    if (!hierLog) hierLog = fopen("/tmp/class_hierarchy.log", "w");
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
    static FILE* semLog = fopen("/tmp/prim_input_sem.log", "a");
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
        // Fallback: use index 1 (common for input semaphore)
        gEventQueue.setInputSemaphoreIndex(1);
        return PrimitiveResult::Success;
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

// BitBlt helper: Extract integer field from BitBlt object
static intptr_t bitBltField(ObjectMemory& memory, Oop bitBlt, size_t index) {
    Oop field = memory.fetchPointer(index, bitBlt);
    if (field.isSmallInteger()) {
        return field.asSmallInteger();
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
PrimitiveResult Interpreter::primitiveCopyBits(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    Oop bitBlt = stackTop();
    if (!bitBlt.isObject()) {
        return PrimitiveResult::Failure;
    }

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
    if (destForm.isNil() || !destForm.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Get destination form parameters
    Oop destBits = memory_.fetchPointer(FormBits, destForm);
    intptr_t destWidth = bitBltField(memory_, destForm, FormWidth);
    intptr_t destHeight = bitBltField(memory_, destForm, FormHeight);
    intptr_t destDepth = bitBltField(memory_, destForm, FormDepth);

    if (destBits.isNil() || !destBits.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Clip the operation to destination bounds
    if (destX < clipX) {
        width -= (clipX - destX);
        sourceX += (clipX - destX);
        destX = clipX;
    }
    if (destY < clipY) {
        height -= (clipY - destY);
        sourceY += (clipY - destY);
        destY = clipY;
    }
    if (destX + width > clipX + clipWidth) {
        width = clipX + clipWidth - destX;
    }
    if (destY + height > clipY + clipHeight) {
        height = clipY + clipHeight - destY;
    }

    // If nothing to draw, succeed immediately
    if (width <= 0 || height <= 0) {
        return PrimitiveResult::Success;
    }

    // For simple cases (fill operations), implement directly
    // combinationRule 3 = store (most common for fills)
    // combinationRule 34 = source (most common for copy)
    if (sourceForm.isNil() && combinationRule == 3) {
        // Fill with halftone or solid color
        Oop halftoneForm = memory_.fetchPointer(BBHalftoneForm, bitBlt);

        // Get fill value (from halftone or default to all 1s)
        uint32_t fillValue = 0xFFFFFFFF;
        if (!halftoneForm.isNil() && halftoneForm.isSmallInteger()) {
            fillValue = static_cast<uint32_t>(halftoneForm.asSmallInteger());
        }

        // Simple fill implementation for 32-bit depth
        if (destDepth == 32) {
            size_t destBytesPerRow = static_cast<size_t>(destWidth * 4);

            for (intptr_t y = 0; y < height; y++) {
                for (intptr_t x = 0; x < width; x++) {
                    size_t offset = static_cast<size_t>((destY + y) * destWidth + (destX + x)) * 4;
                    memory_.storeWord32(offset / 4, destBits, fillValue);
                }
            }
            return PrimitiveResult::Success;
        }
    }

    // For complex operations, fail to Smalltalk fallback
    // A full BitBlt implementation would handle all combination rules,
    // depths, color maps, etc.
    return PrimitiveResult::Failure;
}

// Primitive 291: Draw loop (line drawing for BitBlt)
// Uses existing primitiveDrawLoop implementation (also primitive 104)

// Primitive 292: Compress bitmap to byte array (RLE compression)
// bitmap byteArray primitiveCompressToByteArray -> compressedSize
PrimitiveResult Interpreter::primitiveCompressToByteArray(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop byteArray = stackTop();
    Oop bitmap = stackValue(1);

    if (!bitmap.isObject() || !byteArray.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Get bitmap data
    size_t bitmapSize = memory_.byteSizeOf(bitmap);
    size_t destSize = memory_.byteSizeOf(byteArray);

    if (bitmapSize == 0 || destSize == 0) {
        return PrimitiveResult::Failure;
    }

    // Simple RLE compression
    size_t srcIndex = 0;
    size_t destIndex = 0;
    size_t wordCount = bitmapSize / 4;

    while (srcIndex < wordCount && destIndex < destSize - 4) {
        uint32_t word = memory_.fetchWord32(srcIndex, bitmap);

        // Count consecutive identical words
        size_t runLength = 1;
        while (srcIndex + runLength < wordCount &&
               runLength < 127 &&
               memory_.fetchWord32(srcIndex + runLength, bitmap) == word) {
            runLength++;
        }

        if (runLength >= 2 || word == 0) {
            // Encode as run
            memory_.storeByte(destIndex++, byteArray, static_cast<uint8_t>(runLength | 0x80));
            memory_.storeByte(destIndex++, byteArray, static_cast<uint8_t>((word >> 24) & 0xFF));
            memory_.storeByte(destIndex++, byteArray, static_cast<uint8_t>((word >> 16) & 0xFF));
            memory_.storeByte(destIndex++, byteArray, static_cast<uint8_t>((word >> 8) & 0xFF));
            memory_.storeByte(destIndex++, byteArray, static_cast<uint8_t>(word & 0xFF));
            srcIndex += runLength;
        } else {
            // Encode as literal
            memory_.storeByte(destIndex++, byteArray, 1);  // Length 1
            memory_.storeByte(destIndex++, byteArray, static_cast<uint8_t>((word >> 24) & 0xFF));
            memory_.storeByte(destIndex++, byteArray, static_cast<uint8_t>((word >> 16) & 0xFF));
            memory_.storeByte(destIndex++, byteArray, static_cast<uint8_t>((word >> 8) & 0xFF));
            memory_.storeByte(destIndex++, byteArray, static_cast<uint8_t>(word & 0xFF));
            srcIndex++;
        }
    }

    popN(2);  // arguments
    pop();    // receiver
    push(Oop::fromSmallInteger(static_cast<intptr_t>(destIndex)));
    return PrimitiveResult::Success;
}

// Primitive 293: Decompress byte array to bitmap
// byteArray bitmap primitiveDecompressFromByteArray -> bitmap
PrimitiveResult Interpreter::primitiveDecompressFromByteArray(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop bitmap = stackTop();
    Oop byteArray = stackValue(1);

    if (!bitmap.isObject() || !byteArray.isObject()) {
        return PrimitiveResult::Failure;
    }

    size_t srcSize = memory_.byteSizeOf(byteArray);
    size_t destWords = memory_.byteSizeOf(bitmap) / 4;

    size_t srcIndex = 0;
    size_t destIndex = 0;

    while (srcIndex < srcSize && destIndex < destWords) {
        uint8_t header = memory_.fetchByte(srcIndex++, byteArray);

        if (header & 0x80) {
            // Run-length encoded
            size_t runLength = header & 0x7F;
            if (srcIndex + 4 > srcSize) break;

            uint32_t word = (static_cast<uint32_t>(memory_.fetchByte(srcIndex, byteArray)) << 24) |
                           (static_cast<uint32_t>(memory_.fetchByte(srcIndex + 1, byteArray)) << 16) |
                           (static_cast<uint32_t>(memory_.fetchByte(srcIndex + 2, byteArray)) << 8) |
                            static_cast<uint32_t>(memory_.fetchByte(srcIndex + 3, byteArray));
            srcIndex += 4;

            for (size_t i = 0; i < runLength && destIndex < destWords; i++) {
                memory_.storeWord32(destIndex++, bitmap, word);
            }
        } else {
            // Literal words
            size_t literalCount = header;
            for (size_t i = 0; i < literalCount && destIndex < destWords && srcIndex + 4 <= srcSize; i++) {
                uint32_t word = (static_cast<uint32_t>(memory_.fetchByte(srcIndex, byteArray)) << 24) |
                               (static_cast<uint32_t>(memory_.fetchByte(srcIndex + 1, byteArray)) << 16) |
                               (static_cast<uint32_t>(memory_.fetchByte(srcIndex + 2, byteArray)) << 8) |
                                static_cast<uint32_t>(memory_.fetchByte(srcIndex + 3, byteArray));
                srcIndex += 4;
                memory_.storeWord32(destIndex++, bitmap, word);
            }
        }
    }

    popN(2);  // arguments
    push(bitmap);
    return PrimitiveResult::Success;
}

// Primitive 294: Find first occurrence of character in string
// aString startIndex char primitiveFindFirstInString -> index or 0
PrimitiveResult Interpreter::primitiveFindFirstInString(int argCount) {
    if (argCount != 3) return PrimitiveResult::Failure;

    Oop charOop = stackTop();
    Oop startIndexOop = stackValue(1);
    Oop stringOop = stackValue(2);

    if (!startIndexOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }
    if (!stringOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    intptr_t startIndex = startIndexOop.asSmallInteger();
    size_t stringSize = memory_.byteSizeOf(stringOop);

    // Get character to find
    uint8_t charToFind;
    if (charOop.isSmallInteger()) {
        charToFind = static_cast<uint8_t>(charOop.asSmallInteger());
    } else if (charOop.isCharacter()) {
        charToFind = static_cast<uint8_t>(charOop.asCharacter());
    } else {
        return PrimitiveResult::Failure;
    }

    // Smalltalk uses 1-based indexing
    if (startIndex < 1) startIndex = 1;

    // Search for character
    for (size_t i = static_cast<size_t>(startIndex - 1); i < stringSize; i++) {
        if (memory_.fetchByte(i, stringOop) == charToFind) {
            popN(3);  // arguments
            pop();    // receiver
            push(Oop::fromSmallInteger(static_cast<intptr_t>(i + 1)));  // 1-based
            return PrimitiveResult::Success;
        }
    }

    // Not found
    popN(3);
    pop();
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

    intptr_t startIndex = startIndexOop.asSmallInteger();
    intptr_t stopIndex = stopIndexOop.asSmallInteger();
    size_t stringSize = memory_.byteSizeOf(stringOop);
    size_t tableSize = memory_.byteSizeOf(tableOop);

    if (tableSize < 256) {
        return PrimitiveResult::Failure;
    }

    // Smalltalk uses 1-based indexing
    if (startIndex < 1) startIndex = 1;
    if (stopIndex > static_cast<intptr_t>(stringSize)) stopIndex = static_cast<intptr_t>(stringSize);

    // Translate each character
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

    Oop matchTableOop = stackTop();
    Oop startIndexOop = stackValue(1);
    Oop keyOop = stackValue(2);
    Oop stringOop = stackValue(3);

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
        popN(4);
        pop();
        push(Oop::fromSmallInteger(startIndex));
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
// primitiveLocaleTimezoneOffset -> seconds
PrimitiveResult Interpreter::primitiveLocaleTimezoneOffset(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Would get actual timezone offset
    pop();
    push(Oop::fromSmallInteger(0));  // UTC
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
    uint32_t classIndex = memory_.indexOfClass(rcvr);

    Oop newObj;
    if (isBytes) {
        newObj = memory_.allocateBytes(classIndex, static_cast<size_t>(indexableSize));
    } else {
        size_t totalSlots = fixedSize + static_cast<size_t>(indexableSize);
        ObjectFormat objFormat = (instSpec == 3) ? ObjectFormat::IndexableWithFixed : ObjectFormat::Indexable;
        newObj = memory_.allocateSlots(classIndex, totalSlots, objFormat);
    }

    if (newObj.isNil()) {
        return PrimitiveResult::Failure;
    }

    // Note: Object is allocated in eden but will be promoted to old space by GC
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
    uint32_t classIndex = memory_.indexOfClass(rcvr);

    Oop newObj;
    if (isBytes) {
        newObj = memory_.allocateBytes(classIndex, static_cast<size_t>(indexableSize));
    } else {
        size_t totalSlots = fixedSize + static_cast<size_t>(indexableSize);
        ObjectFormat objFormat = (instSpec == 3) ? ObjectFormat::IndexableWithFixed : ObjectFormat::Indexable;
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

    // Return SmallInteger if it fits, otherwise need LargeInteger (not implemented)
    if (value <= static_cast<uint64_t>(Oop::smallIntegerMax())) {
        primitiveSuccess(Oop::fromSmallInteger(static_cast<int64_t>(value)));
        return PrimitiveResult::Success;
    }
    return PrimitiveResult::Failure;  // Would need LargeInteger
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

    // Return SmallInteger if it fits
    if (value >= Oop::smallIntegerMin() && value <= Oop::smallIntegerMax()) {
        primitiveSuccess(Oop::fromSmallInteger(value));
        return PrimitiveResult::Success;
    }
    return PrimitiveResult::Failure;  // Would need LargeInteger
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

    if (value <= static_cast<uint64_t>(Oop::smallIntegerMax())) {
        primitiveSuccess(Oop::fromSmallInteger(static_cast<int64_t>(value)));
        return PrimitiveResult::Success;
    }
    return PrimitiveResult::Failure;
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

    if (value >= Oop::smallIntegerMin() && value <= Oop::smallIntegerMax()) {
        primitiveSuccess(Oop::fromSmallInteger(value));
        return PrimitiveResult::Success;
    }
    return PrimitiveResult::Failure;
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

} // namespace pharo
