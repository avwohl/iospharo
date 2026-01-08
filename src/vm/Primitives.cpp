/*
 * Primitives.cpp - Essential Primitive Implementations
 *
 * This file implements the ~30 essential primitives needed for bootstrap.
 * Other primitives can fail and fall back to Smalltalk code.
 */

#include "Interpreter.hpp"
#include <cmath>
#include <iostream>

namespace pharo {

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

        // Check for overflow before multiplying
        if (b != 0 && (a > Oop::smallIntegerMax() / std::abs(b) ||
                       a < Oop::smallIntegerMin() / std::abs(b))) {
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

        // Smalltalk mod (\\) uses floored division
        int64_t result = a - b * static_cast<int64_t>(std::floor(
            static_cast<double>(a) / static_cast<double>(b)));
        primitiveSuccess(Oop::fromSmallInteger(result));
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

        // Floored division (//)
        int64_t result = static_cast<int64_t>(std::floor(
            static_cast<double>(a) / static_cast<double>(b)));
        primitiveSuccess(Oop::fromSmallInteger(result));
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
            if (shift >= 64) {
                return PrimitiveResult::Failure;  // Would overflow
            }
            result = value << shift;
        } else {
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

// ===== COMPARISON PRIMITIVES =====

PrimitiveResult Interpreter::primitiveLessThan(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    if (rcvr.isSmallInteger() && arg.isSmallInteger()) {
        bool result = rcvr.asSmallInteger() < arg.asSmallInteger();
        primitiveSuccess(result ? memory_.trueObject() : memory_.falseObject());
        return PrimitiveResult::Success;
    }

    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveGreaterThan(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    if (rcvr.isSmallInteger() && arg.isSmallInteger()) {
        bool result = rcvr.asSmallInteger() > arg.asSmallInteger();
        primitiveSuccess(result ? memory_.trueObject() : memory_.falseObject());
        return PrimitiveResult::Success;
    }

    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveLessOrEqual(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    if (rcvr.isSmallInteger() && arg.isSmallInteger()) {
        bool result = rcvr.asSmallInteger() <= arg.asSmallInteger();
        primitiveSuccess(result ? memory_.trueObject() : memory_.falseObject());
        return PrimitiveResult::Success;
    }

    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveGreaterOrEqual(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    if (rcvr.isSmallInteger() && arg.isSmallInteger()) {
        bool result = rcvr.asSmallInteger() >= arg.asSmallInteger();
        primitiveSuccess(result ? memory_.trueObject() : memory_.falseObject());
        return PrimitiveResult::Success;
    }

    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveEqual(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    if (rcvr.isSmallInteger() && arg.isSmallInteger()) {
        bool result = rcvr.asSmallInteger() == arg.asSmallInteger();
        primitiveSuccess(result ? memory_.trueObject() : memory_.falseObject());
        return PrimitiveResult::Success;
    }

    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveNotEqual(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    if (rcvr.isSmallInteger() && arg.isSmallInteger()) {
        bool result = rcvr.asSmallInteger() != arg.asSmallInteger();
        primitiveSuccess(result ? memory_.trueObject() : memory_.falseObject());
        return PrimitiveResult::Success;
    }

    return PrimitiveResult::Failure;
}

// ===== OBJECT ACCESS PRIMITIVES (60-75) =====

PrimitiveResult Interpreter::primitiveAt(int argCount) {
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

    if (header->isBytesObject()) {
        if (arrayIndex >= header->byteSize()) {
            return PrimitiveResult::Failure;
        }
        uint8_t byte = header->byteAt(arrayIndex);
        primitiveSuccess(Oop::fromSmallInteger(byte));
        return PrimitiveResult::Success;
    } else if (header->isPointersObject()) {
        if (arrayIndex >= header->slotCount()) {
            return PrimitiveResult::Failure;
        }
        primitiveSuccess(header->slotAt(arrayIndex));
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
        if (arrayIndex >= header->slotCount()) {
            return PrimitiveResult::Failure;
        }
        header->slotAtPut(arrayIndex, value);
        primitiveSuccess(value);
        return PrimitiveResult::Success;
    }

    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveSize(int argCount) {
    Oop rcvr = stackValue(0);

    if (!rcvr.isObject()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* header = rcvr.asObjectPtr();
    size_t size;

    if (header->isBytesObject()) {
        size = header->byteSize();
    } else {
        size = header->slotCount();
    }

    if (!Oop::canBeSmallInteger(static_cast<int64_t>(size))) {
        return PrimitiveResult::Failure;
    }

    primitiveSuccess(Oop::fromSmallInteger(static_cast<int64_t>(size)));
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveInstVarAt(int argCount) {
    Oop index = stackValue(0);
    Oop rcvr = stackValue(1);

    if (!index.isSmallInteger() || !rcvr.isObject()) {
        return PrimitiveResult::Failure;
    }

    int64_t idx = index.asSmallInteger();
    if (idx < 1) {
        return PrimitiveResult::Failure;
    }

    size_t instVarIndex = static_cast<size_t>(idx - 1);
    ObjectHeader* header = rcvr.asObjectPtr();

    if (instVarIndex >= header->slotCount()) {
        return PrimitiveResult::Failure;
    }

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

    ObjectHeader* header = rcvr.asObjectPtr();
    if (header->isImmutable()) {
        return PrimitiveResult::Failure;
    }

    int64_t idx = index.asSmallInteger();
    if (idx < 1) {
        return PrimitiveResult::Failure;
    }

    size_t instVarIndex = static_cast<size_t>(idx - 1);
    if (instVarIndex >= header->slotCount()) {
        return PrimitiveResult::Failure;
    }

    header->slotAtPut(instVarIndex, value);
    primitiveSuccess(value);
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
    // Class layout includes format, instSize, etc.
    // Slot 2 is typically the format/instSize

    Oop formatObj = memory_.fetchPointer(2, rcvr);
    if (!formatObj.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t format = formatObj.asSmallInteger();
    size_t instSize = format & 0xFFFF;  // Low bits are instance size

    uint32_t classIndex = memory_.indexOfClass(rcvr);
    Oop newObj = memory_.allocateSlots(classIndex, instSize);

    if (newObj.isNil()) {
        return PrimitiveResult::Failure;  // Out of memory
    }

    primitiveSuccess(newObj);
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveNewWithArg(int argCount) {
    Oop size = stackValue(0);
    Oop rcvr = stackValue(1);  // Class

    if (!size.isSmallInteger() || !rcvr.isObject()) {
        return PrimitiveResult::Failure;
    }

    int64_t indexableSize = size.asSmallInteger();
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
    bool isBytes = ((format >> 16) & 0xFF) >= 16;  // Simplified check

    uint32_t classIndex = memory_.indexOfClass(rcvr);
    Oop newObj;

    if (isBytes) {
        newObj = memory_.allocateBytes(classIndex, static_cast<size_t>(indexableSize));
    } else {
        size_t totalSlots = fixedSize + static_cast<size_t>(indexableSize);
        newObj = memory_.allocateSlots(classIndex, totalSlots, ObjectFormat::Indexable);
    }

    if (newObj.isNil()) {
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
    Oop rcvr = stackValue(0);
    uint32_t hash = memory_.identityHashOf(rcvr);
    primitiveSuccess(Oop::fromSmallInteger(hash));
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveClass(int argCount) {
    Oop rcvr = stackValue(0);
    Oop classOop = memory_.classOf(rcvr);
    primitiveSuccess(classOop);
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveIdentical(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);
    bool result = (rcvr == arg);
    primitiveSuccess(result ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveNotIdentical(int argCount) {
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);
    bool result = (rcvr != arg);
    primitiveSuccess(result ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

// ===== BEHAVIOR PRIMITIVES =====

PrimitiveResult Interpreter::primitivePerform(int argCount) {
    // perform: selector
    // Stack: receiver, selector
    if (argCount < 1) return PrimitiveResult::Failure;

    Oop selector = stackValue(0);

    // Remove selector from stack, leaving receiver
    popN(1);

    // Send the message with 0 additional args
    sendSelector(selector, 0);
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitivePerformWithArgs(int argCount) {
    // perform:withArguments:
    // Stack: receiver, selector, arguments array

    Oop argsArray = stackValue(0);
    Oop selector = stackValue(1);

    if (!argsArray.isObject()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* argsHeader = argsArray.asObjectPtr();
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

    // Verify arg count matches block's numArgs
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

// ===== PROCESS PRIMITIVES (stubs) =====

PrimitiveResult Interpreter::primitiveSuspend(int argCount) {
    return PrimitiveResult::Failure;  // TODO
}

PrimitiveResult Interpreter::primitiveResume(int argCount) {
    return PrimitiveResult::Failure;  // TODO
}

PrimitiveResult Interpreter::primitiveSignal(int argCount) {
    return PrimitiveResult::Failure;  // TODO
}

PrimitiveResult Interpreter::primitiveWait(int argCount) {
    return PrimitiveResult::Failure;  // TODO
}

// ===== SYSTEM PRIMITIVES =====

PrimitiveResult Interpreter::primitiveQuit(int argCount) {
    running_ = false;
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveExitToDebugger(int argCount) {
    return PrimitiveResult::Failure;  // TODO
}

PrimitiveResult Interpreter::primitiveVMParameter(int argCount) {
    return PrimitiveResult::Failure;  // TODO
}

PrimitiveResult Interpreter::primitiveSnapshot(int argCount) {
    // When resuming from a snapshot, return false to indicate "we just loaded"
    // (true = successfully saved, false = resuming from load)
    // This allows the snapshot code to know it should exit the save path
    // std::cerr << "[PRIM] primitiveSnapshot called - returning false (resuming)" << std::endl;
    primitiveSuccess(memory_.falseObject());
    return PrimitiveResult::Success;
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
    return PrimitiveResult::Failure;
}

PrimitiveResult Interpreter::primitiveForceDisplayUpdate(int argCount) {
    return PrimitiveResult::Failure;
}

// ===== STRING/ARRAY PRIMITIVES =====

PrimitiveResult Interpreter::primitiveStringAt(int argCount) {
    return primitiveAt(argCount);  // Same as at:
}

PrimitiveResult Interpreter::primitiveStringAtPut(int argCount) {
    return primitiveAtPut(argCount);  // Same as at:put:
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

// ===== FLOAT PRIMITIVES (stubs) =====

PrimitiveResult Interpreter::primitiveFloatAdd(int argCount) {
    return PrimitiveResult::Failure;  // TODO
}

PrimitiveResult Interpreter::primitiveFloatSubtract(int argCount) {
    return PrimitiveResult::Failure;  // TODO
}

PrimitiveResult Interpreter::primitiveFloatMultiply(int argCount) {
    return PrimitiveResult::Failure;  // TODO
}

PrimitiveResult Interpreter::primitiveFloatDivide(int argCount) {
    return PrimitiveResult::Failure;  // TODO
}

PrimitiveResult Interpreter::primitiveFloatLessThan(int argCount) {
    return PrimitiveResult::Failure;  // TODO
}

PrimitiveResult Interpreter::primitiveFloatEqual(int argCount) {
    return PrimitiveResult::Failure;  // TODO
}

PrimitiveResult Interpreter::primitiveFloatTruncated(int argCount) {
    return PrimitiveResult::Failure;  // TODO
}

PrimitiveResult Interpreter::primitiveFloatSquareRoot(int argCount) {
    return PrimitiveResult::Failure;  // TODO
}

PrimitiveResult Interpreter::primitiveFloatSin(int argCount) {
    return PrimitiveResult::Failure;  // TODO
}

PrimitiveResult Interpreter::primitiveFloatCos(int argCount) {
    return PrimitiveResult::Failure;  // TODO
}

PrimitiveResult Interpreter::primitiveFloatArctan(int argCount) {
    return PrimitiveResult::Failure;  // TODO
}

PrimitiveResult Interpreter::primitiveFloatExp(int argCount) {
    return PrimitiveResult::Failure;  // TODO
}

PrimitiveResult Interpreter::primitiveFloatLn(int argCount) {
    return PrimitiveResult::Failure;  // TODO
}

// ===== LARGE INTEGER PRIMITIVES (stubs) =====

PrimitiveResult Interpreter::primitiveLargeIntegerAdd(int argCount) {
    return PrimitiveResult::Failure;  // TODO
}

PrimitiveResult Interpreter::primitiveLargeIntegerSubtract(int argCount) {
    return PrimitiveResult::Failure;  // TODO
}

PrimitiveResult Interpreter::primitiveLargeIntegerMultiply(int argCount) {
    return PrimitiveResult::Failure;  // TODO
}

PrimitiveResult Interpreter::primitiveLargeIntegerDivide(int argCount) {
    return PrimitiveResult::Failure;  // TODO
}

PrimitiveResult Interpreter::primitiveLargeIntegerMod(int argCount) {
    return PrimitiveResult::Failure;  // TODO
}

} // namespace pharo
