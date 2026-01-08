/*
 * Primitives.cpp - Essential Primitive Implementations
 *
 * This file implements the ~30 essential primitives needed for bootstrap.
 * Other primitives can fail and fall back to Smalltalk code.
 */

#include "Interpreter.hpp"
#include "ImageLoader.hpp"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <vector>

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

// ===== POINT PRIMITIVE =====

PrimitiveResult Interpreter::primitiveMakePoint(int argCount) {
    // Primitive 18: Number @ aNumber - create a Point
    Oop yArg = stackValue(0);
    Oop xRcvr = stackValue(1);

    // Both arguments should be numbers (SmallInteger or Float)
    // For simplicity, we accept SmallIntegers, SmallFloats, or boxed Floats
    if (!xRcvr.isSmallInteger() && !xRcvr.isSmallFloat() &&
        !(xRcvr.isObject() && memory_.classOf(xRcvr) == memory_.specialObject(SpecialObjectIndex::ClassFloat))) {
        return PrimitiveResult::Failure;
    }
    if (!yArg.isSmallInteger() && !yArg.isSmallFloat() &&
        !(yArg.isObject() && memory_.classOf(yArg) == memory_.specialObject(SpecialObjectIndex::ClassFloat))) {
        return PrimitiveResult::Failure;
    }

    // Get Point class
    Oop pointClass = memory_.specialObject(SpecialObjectIndex::ClassPoint);
    uint32_t classIndex = memory_.indexOfClass(pointClass);

    // Allocate Point with 2 slots (x, y)
    Oop point = memory_.allocateSlots(classIndex, 2);
    if (point.isNil()) {
        return PrimitiveResult::Failure;
    }

    // Store x and y
    memory_.storePointer(0, point, xRcvr);  // x
    memory_.storePointer(1, point, yArg);   // y

    primitiveSuccess(point);
    return PrimitiveResult::Success;
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

PrimitiveResult Interpreter::primitiveAsCharacter(int argCount) {
    // Primitive 170: Integer >> asCharacter - convert integer to Character
    Oop rcvr = stackValue(0);

    if (!rcvr.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t value = rcvr.asSmallInteger();

    // Valid Unicode codepoints are 0 to 0x10FFFF
    if (value < 0 || value > 0x10FFFF) {
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
    // For primitive 81, the receiver is the block, and argCount should be 0
    // But the primitive dispatch may pass the actual arg count
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

    // Activate the closure - same as regular block activation
    activateBlock(closure, argCount);
    return PrimitiveResult::Success;
}

// Primitive 208: Closure value with unwind protection
// Evaluates a closure but ensures unwind actions are executed
// Used for ensure: blocks
PrimitiveResult Interpreter::primitiveClosureValueUnwind(int argCount) {
    // This primitive is called when evaluating a block that needs
    // unwind protection (like ensure: blocks)

    // For a basic implementation, we can just evaluate the block normally
    // Full unwind semantics require integration with exception handling

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

    // Mark this activation for unwind protection
    // In a full implementation, we'd set a flag on the context
    // For now, just activate normally
    activateBlock(closure, argCount);
    return PrimitiveResult::Success;
}

// Primitive 209: Closure value without unwind (optimization)
// Evaluates a closure skipping unwind protection checks
// Used when we know no unwind is needed
PrimitiveResult Interpreter::primitiveClosureValueNoUnwind(int argCount) {
    // This is an optimized path that skips unwind checking
    // For our implementation, it's the same as normal block value

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

// ===== PROCESS PRIMITIVES =====

PrimitiveResult Interpreter::primitiveSuspend(int argCount) {
    // Primitive 88: Process>>suspend
    // Suspend the receiver process. If it's the active process, switch to next.
    // Returns the list the process was on (or nil if it was running).
    Oop process = stackTop();  // Receiver is the process to suspend

    if (!process.isObject()) {
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
    if (myList.isNil() || myList.rawBits() == nilObj.rawBits()) {
        // Process not on any list - can't suspend (already suspended?)
        return PrimitiveResult::Failure;
    }

    // Remove from its current list
    removeProcessFromList(process, myList);

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

    // Verify process has a valid suspended context
    Oop context = memory_.fetchPointer(ProcessSuspendedContextIndex, process);
    Oop nilObj = memory_.nil();
    if (context.isNil() || context.rawBits() == nilObj.rawBits() || !context.isObject()) {
        return PrimitiveResult::Failure;  // Can't resume without a valid context
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
    Oop semaphore = stackTop();  // Receiver

    if (!semaphore.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Check excessSignals (slot 2 of Semaphore)
    Oop excessOop = memory_.fetchPointer(SemaphoreExcessSignalsIndex, semaphore);
    if (excessOop.isSmallInteger()) {
        int64_t excess = excessOop.asSmallInteger();
        if (excess > 0) {
            // Semaphore is signaled - decrement and return immediately
            memory_.storePointer(SemaphoreExcessSignalsIndex, semaphore,
                                Oop::fromSmallInteger(excess - 1));
            // Return the semaphore (receiver stays on stack)
            return PrimitiveResult::Success;
        }
    }

    // No signal available - must wait
    // Add current process to semaphore's wait list
    Oop activeProcess = getActiveProcess();
    addLastLinkToList(activeProcess, semaphore);

    // Find next runnable process and switch to it
    Oop nextProcess = wakeHighestPriority();
    Oop nilObj = memory_.nil();
    if (nextProcess.isNil() || nextProcess.rawBits() == nilObj.rawBits()) {
        // No other process - this is bad, but we can't do much
        // Remove ourselves from the wait list and fail
        removeFirstLinkOfList(semaphore);
        return PrimitiveResult::Failure;
    }

    transferTo(nextProcess);

    // When we return here (after being signaled), the result is the semaphore
    return PrimitiveResult::Success;
}

// ===== SYSTEM PRIMITIVES =====

PrimitiveResult Interpreter::primitiveQuit(int argCount) {
    // Smalltalk quitPrimitive / Smalltalk exit: exitCode
    int exitCode = 0;

    if (argCount >= 1) {
        Oop arg = stackValue(0);
        if (arg.isSmallInteger()) {
            exitCode = static_cast<int>(arg.asSmallInteger());
        }
    }

    running_ = false;

    // Actually exit the process
    std::exit(exitCode);

    // Not reached, but for completeness
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveExitToDebugger(int argCount) {
    // Primitive 114: Enter debugger / halt VM
    // Used by Smalltalk Halt and Error handling
    std::cerr << "[VM] primitiveExitToDebugger called - halting" << std::endl;

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

// ===== SYSTEM PATH PRIMITIVES =====

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

// ===== TIME PRIMITIVES =====

PrimitiveResult Interpreter::primitiveMillisecondClock(int argCount) {
    // Primitive 135: Return milliseconds since VM start or epoch
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    // Return low 30 bits to fit in SmallInteger (wraps every ~12 days)
    int64_t result = ms & 0x3FFFFFFF;
    primitiveSuccess(Oop::fromSmallInteger(result));
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveSecondsClock(int argCount) {
    // Primitive 137: Return seconds since Smalltalk epoch (Jan 1, 1901)
    // Unix epoch is Jan 1, 1970 = 2177452800 seconds after Smalltalk epoch
    const int64_t unixToSmalltalkOffset = 2177452800LL;

    auto now = std::chrono::system_clock::now();
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();

    int64_t smalltalkSeconds = seconds + unixToSmalltalkOffset;
    primitiveSuccess(Oop::fromSmallInteger(smalltalkSeconds));
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveMicrosecondClock(int argCount) {
    // Primitive 240: Return microseconds (high resolution timer)
    auto now = std::chrono::high_resolution_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();

    // For large values, we need to handle potential overflow
    // Return as positive integer (may need LargeInteger for full range)
    if (Oop::canBeSmallInteger(us)) {
        primitiveSuccess(Oop::fromSmallInteger(us));
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
    // For now, just fail - requires timer integration
    return PrimitiveResult::Failure;
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
        size_t byteOffset = arrayIndex * 4;
        uint32_t codePoint = header->byteAt(byteOffset) |
                            (header->byteAt(byteOffset + 1) << 8) |
                            (header->byteAt(byteOffset + 2) << 16) |
                            (header->byteAt(byteOffset + 3) << 24);
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

// Helper: Extract double from SmallFloat or boxed Float
static bool extractFloat(ObjectMemory& memory, Oop oop, double& result) {
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
    } else {
        // Could be LargeInteger - for now just fail
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
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    double a, b;
    if (!extractFloat(memory_, rcvr, a) || !extractFloat(memory_, arg, b)) {
        return PrimitiveResult::Failure;
    }

    if (b == 0.0) {
        return PrimitiveResult::Failure;  // Division by zero
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

PrimitiveResult Interpreter::primitiveFloatTruncated(int argCount) {
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

    // Use ldexp to multiply by 2^power efficiently
    double result = std::ldexp(value, static_cast<int>(power));

    Oop resultOop = makeFloat(memory_, result);
    if (resultOop.isNil()) return PrimitiveResult::Failure;

    popN(2);
    push(resultOop);
    return PrimitiveResult::Success;
}

PrimitiveResult Interpreter::primitiveFloatSquareRoot(int argCount) {
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

PrimitiveResult Interpreter::primitiveFloatSin(int argCount) {
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

PrimitiveResult Interpreter::primitiveFloatArctan(int argCount) {
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

PrimitiveResult Interpreter::primitiveFloatExp(int argCount) {
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

PrimitiveResult Interpreter::primitiveFloatLn(int argCount) {
    Oop rcvr = stackTop();

    double value;
    if (!extractFloat(memory_, rcvr, value)) {
        return PrimitiveResult::Failure;
    }

    if (value <= 0.0) {
        return PrimitiveResult::Failure;  // Negative or zero
    }

    double result = std::log(value);
    Oop resultOop = makeFloat(memory_, result);
    if (resultOop.isNil()) return PrimitiveResult::Failure;

    pop();
    push(resultOop);
    return PrimitiveResult::Success;
}

// ===== LARGE INTEGER PRIMITIVES =====

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

PrimitiveResult Interpreter::primitiveLargeIntegerAdd(int argCount) {
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

PrimitiveResult Interpreter::primitiveLargeIntegerSubtract(int argCount) {
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

PrimitiveResult Interpreter::primitiveLargeIntegerMultiply(int argCount) {
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

PrimitiveResult Interpreter::primitiveLargeIntegerDivide(int argCount) {
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

PrimitiveResult Interpreter::primitiveLargeIntegerMod(int argCount) {
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

    // Remainder has same sign as dividend (truncated division)
    bool resultNeg = aNeg && !(remainder.size() == 1 && remainder[0] == 0);

    Oop result = makeLargeInteger(memory_, remainder, resultNeg);
    if (result.isNil()) return PrimitiveResult::Failure;

    popN(2);
    push(result);
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

PrimitiveResult Interpreter::primitiveLargeIntegerLessThan(int argCount) {
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

PrimitiveResult Interpreter::primitiveLargeIntegerGreaterThan(int argCount) {
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

PrimitiveResult Interpreter::primitiveLargeIntegerLessOrEqual(int argCount) {
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

PrimitiveResult Interpreter::primitiveLargeIntegerGreaterOrEqual(int argCount) {
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

PrimitiveResult Interpreter::primitiveLargeIntegerEqual(int argCount) {
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

PrimitiveResult Interpreter::primitiveLargeIntegerNotEqual(int argCount) {
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
PrimitiveResult Interpreter::primitiveLargeIntegerDiv(int argCount) {
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
PrimitiveResult Interpreter::primitiveLargeIntegerQuo(int argCount) {
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
PrimitiveResult Interpreter::primitiveLargeIntegerBitAnd(int argCount) {
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
PrimitiveResult Interpreter::primitiveLargeIntegerBitOr(int argCount) {
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
PrimitiveResult Interpreter::primitiveLargeIntegerBitXor(int argCount) {
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
PrimitiveResult Interpreter::primitiveLargeIntegerBitShift(int argCount) {
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
    // Arguments: receiver compareTo: arg startingAt: start1 to: stop1 startingAt: start2
    // Simplified version: receiver compareWith: arg
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

    // Both must be byte-indexable
    bool rcvrIsBytes = (rcvrFormat >= ObjectFormat::Indexable8 && rcvrFormat <= ObjectFormat::Indexable8_7);
    bool argIsBytes = (argFormat >= ObjectFormat::Indexable8 && argFormat <= ObjectFormat::Indexable8_7);

    if (!rcvrIsBytes || !argIsBytes) {
        return PrimitiveResult::Failure;
    }

    size_t rcvrSize = memory_.byteSizeOf(rcvr);
    size_t argSize = memory_.byteSizeOf(arg);

    uint8_t* rcvrBytes = reinterpret_cast<uint8_t*>(rcvrHeader + 1);
    uint8_t* argBytes = reinterpret_cast<uint8_t*>(argHeader + 1);

    // Compare byte by byte
    size_t minSize = std::min(rcvrSize, argSize);
    int result = std::memcmp(rcvrBytes, argBytes, minSize);

    if (result == 0) {
        // If equal up to minSize, shorter one is "less"
        if (rcvrSize < argSize) result = -1;
        else if (rcvrSize > argSize) result = 1;
    }

    // Return comparison result as SmallInteger (-1, 0, or 1 style, or actual diff)
    popN(2);
    push(Oop::fromSmallInteger(result < 0 ? 1 : (result > 0 ? 3 : 2)));
    // Convention: 1 = less, 2 = equal, 3 = greater
    return PrimitiveResult::Success;
}

// Primitive 159: Hash multiply for collections
PrimitiveResult Interpreter::primitiveHashMultiply(int argCount) {
    Oop rcvr = stackTop();

    if (!rcvr.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t hash = rcvr.asSmallInteger();

    // Standard hash multiply used by Squeak/Pharo
    // hashMultiply is: (hash * 1664525) bitAnd: 16rFFFFFFF
    const int64_t HashMultiplier = 1664525;
    const int64_t HashMask = 0x0FFFFFFF;  // 28 bits

    int64_t result = (hash * HashMultiplier) & HashMask;

    pop();
    push(Oop::fromSmallInteger(result));
    return PrimitiveResult::Success;
}

// ===== PROCESS PRIMITIVES =====

// Primitive 167: Yield to other processes of same priority
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

    // Check if there are other processes at the same priority
    Oop firstLink = memory_.fetchPointer(LinkedListFirstLinkIndex, priorityList);

    if (!firstLink.isNil()) {
        // There are other processes waiting - put current process at end of queue
        // and switch to the first one

        // Save current context
        Oop currentContext = activeContext_;
        memory_.storePointer(ProcessSuspendedContextIndex, activeProcess, currentContext);

        // Add current process to end of priority list
        addLastLinkToList(activeProcess, priorityList);

        // Remove first process from list and make it active
        Oop nextProcess = removeFirstLinkOfList(priorityList);
        setActiveProcess(nextProcess);

        // Switch to the next process's context
        Oop newContext = memory_.fetchPointer(ProcessSuspendedContextIndex, nextProcess);
        memory_.storePointer(ProcessSuspendedContextIndex, nextProcess, Oop::nil());

        if (!newContext.isNil() && newContext.isObject()) {
            executeFromContext(newContext);
        }
    }

    // Return receiver (the process or processor)
    primitiveSuccess(stackTop());
    return PrimitiveResult::Success;
}

// ===== CONTEXT PRIMITIVES =====

// Primitive 199: Return the current execution context (thisContext)
PrimitiveResult Interpreter::primitiveThisContext(int argCount) {
    // Return the active context
    // In our implementation, activeContext_ holds the current context
    // If we're running from a synthetic context, we may need to materialize one

    if (activeContext_.isNil() || !activeContext_.isObject()) {
        // Create a context from current execution state if needed
        // For now, just fail if no context - caller should handle
        return PrimitiveResult::Failure;
    }

    // Note: In a full implementation, we'd sync IP/SP to the context here.
    // For now, the context should be reasonably up-to-date from message sends.

    pop();  // Pop receiver
    push(activeContext_);
    return PrimitiveResult::Success;
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
    size_t slotCount = header->slotCount();

    // 1-based index
    size_t zeroIndex = static_cast<size_t>(index - 1);
    if (zeroIndex >= slotCount) {
        return PrimitiveResult::Failure;
    }

    Oop value = memory_.fetchPointer(zeroIndex, rcvr);

    popN(2);
    push(value);
    return PrimitiveResult::Success;
}

// Primitive 174: Write slot at given index (1-based)
PrimitiveResult Interpreter::primitiveSlotAtPut(int argCount) {
    Oop value = stackValue(0);
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!rcvr.isObject() || !indexOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t index = indexOop.asSmallInteger();
    if (index < 1) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* header = rcvr.asObjectPtr();

    // Check immutability
    if (header->isImmutable()) {
        return PrimitiveResult::Failure;
    }

    size_t slotCount = header->slotCount();

    // 1-based index
    size_t zeroIndex = static_cast<size_t>(index - 1);
    if (zeroIndex >= slotCount) {
        return PrimitiveResult::Failure;
    }

    memory_.storePointer(zeroIndex, rcvr, value);

    popN(3);
    push(value);  // Return the stored value
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

    // Check each slot for pointer objects
    size_t slotCount = header->slotCount();
    for (size_t i = 0; i < slotCount; i++) {
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
PrimitiveResult Interpreter::primitiveBehaviorHash(int argCount) {
    Oop rcvr = stackTop();

    if (!rcvr.isObject()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* header = rcvr.asObjectPtr();

    // Return the identity hash from the object header
    uint32_t hash = header->identityHash();

    // If hash is 0, generate one (behaviors should have stable hashes)
    if (hash == 0) {
        // Use a simple hash based on pointer for now
        hash = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(header) >> 3) & 0x3FFFFF;
        if (hash == 0) hash = 1;  // Ensure non-zero
    }

    pop();
    push(Oop::fromSmallInteger(hash));
    return PrimitiveResult::Success;
}

// Primitive 115: Change the class of an object
PrimitiveResult Interpreter::primitiveChangeClass(int argCount) {
    Oop newClassOop = stackValue(0);
    Oop rcvr = stackValue(1);

    if (!rcvr.isObject() || !newClassOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* rcvrHeader = rcvr.asObjectPtr();

    // Check immutability
    if (rcvrHeader->isImmutable()) {
        return PrimitiveResult::Failure;
    }

    // Get the class index for the new class
    uint32_t newClassIndex = memory_.indexOfClass(newClassOop);

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

// Primitive 143: Read a 16-bit unsigned integer from a short-indexable object
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

    uint16_t* shorts = reinterpret_cast<uint16_t*>(header + 1);
    int64_t value = shorts[zeroIndex];

    popN(2);
    push(Oop::fromSmallInteger(value));
    return PrimitiveResult::Success;
}

// Primitive 144: Write a 16-bit unsigned integer to a short-indexable object
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

    // Check value fits in 16 bits unsigned
    if (value < 0 || value > 65535) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* header = rcvr.asObjectPtr();

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

    uint16_t* shorts = reinterpret_cast<uint16_t*>(header + 1);
    shorts[zeroIndex] = static_cast<uint16_t>(value);

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

    // No more objects - return the original object to signal end
    // (Standard behavior is to return the same object when iteration ends)
    return PrimitiveResult::Failure;
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
    // Negative indices are command line args, positive are VM info
    switch (index) {
        case 0:  // VM path (not available, fail)
            return PrimitiveResult::Failure;
        case 1:  // Image path (not available, fail)
            return PrimitiveResult::Failure;
        case 1001:  // VM version string - return nil for now
            pop();
            push(Oop::nil());
            return PrimitiveResult::Success;
        case 1002:  // VM build string
            pop();
            push(Oop::nil());
            return PrimitiveResult::Success;
        case 1003:  // Interpreter class name
            pop();
            push(Oop::nil());
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

// Primitive 151: Set immutability flag of object
PrimitiveResult Interpreter::primitiveSetImmutability(int argCount) {
    Oop flagOop = stackValue(0);
    Oop rcvr = stackValue(1);

    if (!rcvr.isObject()) {
        // Can't change immutability of immediates
        return PrimitiveResult::Failure;
    }

    bool makeImmutable = (flagOop.rawBits() == memory_.trueObject().rawBits());

    ObjectHeader* header = rcvr.asObjectPtr();
    header->setImmutable(makeImmutable);

    popN(2);
    push(rcvr);
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
        memory_.storePointer(i, method, Oop::nil());
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

    if (!rcvr.isObject()) {
        // Immediates are not pinned (they don't move anyway)
        pop();
        push(memory_.falseObject());
        return PrimitiveResult::Success;
    }

    ObjectHeader* header = rcvr.asObjectPtr();
    bool isPinned = header->isPinned();

    pop();
    push(isPinned ? memory_.trueObject() : memory_.falseObject());
    return PrimitiveResult::Success;
}

// Primitive 184: Pin an object (prevent GC from moving it)
PrimitiveResult Interpreter::primitivePin(int argCount) {
    Oop rcvr = stackTop();

    if (!rcvr.isObject()) {
        // Can't pin immediates, but succeed anyway
        pop();
        push(memory_.falseObject());  // Return false (was not pinned)
        return PrimitiveResult::Success;
    }

    ObjectHeader* header = rcvr.asObjectPtr();
    bool wasPinned = header->isPinned();
    header->setPinned(true);

    pop();
    push(wasPinned ? memory_.trueObject() : memory_.falseObject());
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
PrimitiveResult Interpreter::primitiveGrowMemory(int argCount) {
    Oop amountOop = stackTop();

    if (!amountOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t amount = amountOop.asSmallInteger();
    if (amount < 0) {
        return PrimitiveResult::Failure;
    }

    // In a full implementation, we'd actually grow the heap
    // For now, just return the current free space
    size_t freeBytes = memory_.freeOldSpaceBytes();

    pop();
    push(Oop::fromSmallInteger(static_cast<int64_t>(freeBytes)));
    return PrimitiveResult::Success;
}

// Primitive 125: Signal semaphore when free bytes drops below threshold
PrimitiveResult Interpreter::primitiveSignalAtBytesLeft(int argCount) {
    Oop bytesOop = stackValue(0);
    Oop semaphoreOop = stackValue(1);

    if (!bytesOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    // In a full implementation, we'd register this with the GC
    // For now, just succeed (the GC will need to check this)

    popN(2);
    push(semaphoreOop);  // Return receiver
    return PrimitiveResult::Success;
}

// ===== INTERRUPT SEMAPHORE PRIMITIVE =====

// Primitive 134: Set the interrupt semaphore
PrimitiveResult Interpreter::primitiveInterruptSemaphore(int argCount) {
    Oop semaphoreOop = stackTop();

    // Store in special objects array at InterruptSemaphore index
    // Special object index 30 is the interrupt semaphore
    const size_t InterruptSemaphoreIndex = 30;

    Oop specialArray = memory_.specialObjectsArray();
    if (specialArray.isNil()) {
        return PrimitiveResult::Failure;
    }

    memory_.storePointer(InterruptSemaphoreIndex, specialArray, semaphoreOop);

    pop();
    push(semaphoreOop);
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

        // Nil out this context's sender
        memory_.storePointer(SenderIndex, current, Oop::nil());

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
    Oop indexOop = stackValue(0);
    Oop rcvr = stackValue(1);

    if (!rcvr.isObject() || !indexOop.isSmallInteger()) {
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

    uint32_t* words = reinterpret_cast<uint32_t*>(header + 1);
    uint32_t value = words[index - 1];

    popN(2);
    push(Oop::fromSmallInteger(value));
    return PrimitiveResult::Success;
}

// Primitive 39: Write 32-bit word to Float at index (1 or 2)
PrimitiveResult Interpreter::primitiveFloatAtPut(int argCount) {
    Oop valueOop = stackValue(0);
    Oop indexOop = stackValue(1);
    Oop rcvr = stackValue(2);

    if (!rcvr.isObject() || !indexOop.isSmallInteger() || !valueOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t index = indexOop.asSmallInteger();
    int64_t value = valueOop.asSmallInteger();

    if (index < 1 || index > 2) {
        return PrimitiveResult::Failure;
    }

    // Check value fits in 32 bits unsigned
    if (value < 0 || value > 0xFFFFFFFF) {
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

    uint32_t* words = reinterpret_cast<uint32_t*>(header + 1);
    words[index - 1] = static_cast<uint32_t>(value);

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

// Primitive 188: Find a handler context for an exception
// Walks the sender chain looking for a context that handles the given exception
PrimitiveResult Interpreter::primitiveFindHandlerContext(int argCount) {
    // Arguments: exception class to handle
    // Receiver: context to start searching from

    Oop exceptionClass = stackValue(0);
    Oop startContext = stackValue(1);

    if (!startContext.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Walk the sender chain looking for handler contexts
    // In Smalltalk, handler contexts are identified by:
    // 1. Being an activation of on:do: or similar
    // 2. Having the exception class match

    // For now, return nil to indicate no handler found
    // The Smalltalk code will fall back to its own implementation
    popN(2);
    push(Oop::nil());
    return PrimitiveResult::Success;
}

// Primitive 189: Find the next unwind context up to a limit
// Walks the sender chain looking for ensure: or similar unwind-protect contexts
PrimitiveResult Interpreter::primitiveFindNextUnwindContext(int argCount) {
    // Arguments: limit context (stop searching when we reach this)
    // Receiver: context to start searching from

    Oop limitContext = stackValue(0);
    Oop startContext = stackValue(1);

    if (!startContext.isObject()) {
        return PrimitiveResult::Failure;
    }

    // Walk the sender chain from startContext up to limitContext
    // looking for unwind-protect contexts (ensure: blocks)

    Oop current = startContext;

    while (!current.isNil() && current.isObject()) {
        // Check if this is the limit
        if (current.rawBits() == limitContext.rawBits()) {
            // Reached limit without finding unwind context
            popN(2);
            push(Oop::nil());
            return PrimitiveResult::Success;
        }

        // In a full implementation, we'd check if current is an unwind context
        // by looking at flags set by primitiveMarkUnwindMethod or by
        // checking if the method is an ensure: method

        // Get sender
        Oop sender = memory_.fetchPointer(ContextSenderIndex, current);
        current = sender;
    }

    // No unwind context found
    popN(2);
    push(Oop::nil());
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
    // receiver is at stackValue(3), will remain on stack after we pop the other args

    if (!argsArray.isObject() || !lookupClass.isObject()) {
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

// Primitive 204: Evaluate a closure without switching context
// This is used for very simple blocks that shouldn't create a context
PrimitiveResult Interpreter::primitiveClosureValueNoContextSwitch(int argCount) {
    // For now, just delegate to normal block value
    // A full implementation would avoid creating a context frame
    return primitiveBlockValue(argCount);
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
PrimitiveResult Interpreter::primitiveSetStackPointer(int argCount) {
    if (argCount != 1) {
        return PrimitiveResult::Failure;
    }

    Oop newStackp = stackValue(0);
    Oop context = stackValue(1);

    if (!context.isObject() || !newStackp.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    ObjectHeader* header = context.asObjectPtr();
    if (header->isImmutable()) {
        return PrimitiveResult::Failure;
    }

    // Validate stack pointer is within bounds
    int64_t sp = newStackp.asSmallInteger();
    size_t slotCount = header->slotCount();
    if (sp < 0 || static_cast<size_t>(sp) > slotCount - ContextFixedSlots) {
        return PrimitiveResult::Failure;
    }

    // Stackp is at slot 2
    memory_.storePointer(ContextStackPIndex, context, newStackp);

    popN(2);
    push(context);  // Return receiver
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
    push(Oop::nil());
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

    // Simple polynomial rolling hash
    uint32_t hash = 5381;  // DJB2 initial value
    for (size_t i = 0; i < byteCount; ++i) {
        hash = ((hash << 5) + hash) + bytes[i];  // hash * 33 + byte
    }

    // Ensure hash fits in SmallInteger range
    hash = hash & 0x3FFFFFFF;  // 30 bits

    pop();
    push(Oop::fromSmallInteger(static_cast<int64_t>(hash)));
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

} // namespace pharo
