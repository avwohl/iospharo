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
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
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
    if (argCount != 0) return PrimitiveResult::Failure;

    // On iOS, display updates are handled by the system
    // This primitive signals that the display should be refreshed
    // For now, just succeed - actual display update is platform-specific
    return PrimitiveResult::Success;
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
        push(Oop::nil());
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
        push(Oop::nil());
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
        push(Oop::nil());
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
        push(Oop::nil());
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
    if (argCount < 1) {
        return PrimitiveResult::Failure;
    }

    Oop semaphoreOop = stackTop();

    // Store the input semaphore for later signaling
    // In a full implementation, this would be used to signal input events
    // For now, just accept and store it (could add inputSemaphore_ field)
    (void)semaphoreOop;  // Acknowledge parameter

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
    if (argCount < 2) {
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

    // Perform one-way become for each pair
    for (size_t i = 0; i < fromSize; i++) {
        Oop fromObj = memory_.fetchPointer(i, fromArrayOop);
        Oop toObj = memory_.fetchPointer(i, toArrayOop);

        // Skip if either is an immediate or nil
        if (!fromObj.isObject() || fromObj.isNil()) {
            continue;
        }

        // Perform one-way become: all references to fromObj become toObj
        memory_.becomeForward(fromObj, toObj);
    }

    popN(argCount);
    push(fromArrayOop);
    return PrimitiveResult::Success;
}

// Primitive 198: One-way become with hash copying
// fromArray toArray copyHash primitiveArrayBecomeOneWayCopyHash -> fromArray
// Like 197, but optionally copies identity hash from source to target
PrimitiveResult Interpreter::primitiveArrayBecomeOneWayCopyHash(int argCount) {
    if (argCount < 3) {
        return PrimitiveResult::Failure;
    }

    Oop copyHashOop = stackValue(0);
    Oop toArrayOop = stackValue(1);
    Oop fromArrayOop = stackValue(2);

    if (!fromArrayOop.isObject() || !toArrayOop.isObject()) {
        return PrimitiveResult::Failure;
    }

    bool copyHash = (copyHashOop == memory_.trueObject());

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

    Oop millisecondsOop = stackTop();

    if (!millisecondsOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    int64_t milliseconds = millisecondsOop.asSmallInteger();

    // In a cooperative VM, relinquishing the processor means:
    // 1. Check for pending events/signals
    // 2. Optionally sleep for the requested time
    // 3. Allow process scheduler to run other processes

    if (milliseconds > 0) {
        // Sleep for the requested duration
        // Use platform sleep - on POSIX systems this is usleep or nanosleep
        #ifdef _WIN32
        Sleep(static_cast<DWORD>(milliseconds));
        #else
        usleep(static_cast<useconds_t>(milliseconds * 1000));
        #endif
    }

    // In a single-process VM, there's nothing else to schedule
    // Just return after the sleep

    pop();  // pop milliseconds, leave receiver
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

// Primitive 117: Call out to FFI (Foreign Function Interface)
// externalFunction args primitiveCalloutToFFI -> result
// Calls a foreign function through FFI mechanism
PrimitiveResult Interpreter::primitiveCalloutToFFI(int argCount) {
    // FFI callout requires:
    // 1. Function pointer or symbol lookup
    // 2. Argument marshalling (Smalltalk -> C types)
    // 3. Actual call
    // 4. Result marshalling (C -> Smalltalk)

    // Without a full FFI implementation, fail to Smalltalk fallback
    // Smalltalk code may use alternative mechanisms or report error
    return PrimitiveResult::Failure;
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

    // This is the general mechanism for plugin primitives
    // Without plugin support, fail to trigger Smalltalk fallback

    // A full implementation would:
    // 1. Get module name and primitive name from method literal frame
    // 2. Load the module if not already loaded
    // 3. Look up the primitive function
    // 4. Call it with the current stack/context

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
// bytesToGrow primitiveGrowMemoryByAtLeast -> actualBytesGrown or 0
PrimitiveResult Interpreter::primitiveGrowMemoryByAtLeast(int argCount) {
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

// ===== TIME PRIMITIVES (242-252) =====

// Primitive 242: UTC Microsecond clock
// primitiveUTCMicrosecondClock -> microseconds since Posix epoch in UTC
PrimitiveResult Interpreter::primitiveUTCMicrosecondClock(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    int64_t microseconds = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();

    pop();  // receiver
    push(Oop::fromSmallInteger(microseconds));
    return PrimitiveResult::Success;
}

// Primitive 243: Local timezone name
// primitiveLocalTimezone -> string with timezone name
PrimitiveResult Interpreter::primitiveLocalTimezone(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Get timezone name from system
    time_t now = time(nullptr);
    struct tm* local = localtime(&now);

    // tm_zone contains the timezone abbreviation (e.g., "PST", "EST")
    const char* tzName = local->tm_zone ? local->tm_zone : "UTC";

    // Create string object for timezone name
    Oop result = createStringObject(memory_, tzName);
    if (result.isNil()) {
        return PrimitiveResult::Failure;
    }

    pop();  // receiver
    push(result);
    return PrimitiveResult::Success;
}

// Primitive 244: Timezone offset from UTC in minutes
// primitiveTimezoneOffset -> offset in minutes (negative for west of UTC)
PrimitiveResult Interpreter::primitiveTimezoneOffset(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    time_t now = time(nullptr);
    struct tm* local = localtime(&now);

    // tm_gmtoff is seconds east of UTC
    int64_t offsetMinutes = local->tm_gmtoff / 60;

    pop();  // receiver
    push(Oop::fromSmallInteger(offsetMinutes));
    return PrimitiveResult::Success;
}

// Primitive 245: Daylight saving time offset in minutes
// primitiveDaylightSavingTimeOffset -> DST offset (usually 0 or 60)
PrimitiveResult Interpreter::primitiveDaylightSavingTimeOffset(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    time_t now = time(nullptr);
    struct tm* local = localtime(&now);

    // tm_isdst > 0 means DST is in effect
    int64_t dstOffset = local->tm_isdst > 0 ? 60 : 0;

    pop();  // receiver
    push(Oop::fromSmallInteger(dstOffset));
    return PrimitiveResult::Success;
}

// Primitive 246: VM's offset to UTC in microseconds
// primitiveVMOffsetToUTC -> offset in microseconds
PrimitiveResult Interpreter::primitiveVMOffsetToUTC(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    time_t now = time(nullptr);
    struct tm* local = localtime(&now);

    // Convert seconds to microseconds
    int64_t offsetMicroseconds = static_cast<int64_t>(local->tm_gmtoff) * 1000000LL;

    pop();  // receiver
    push(Oop::fromSmallInteger(offsetMicroseconds));
    return PrimitiveResult::Success;
}

// Primitive 247: Posix microsecond clock with UTC offset
// primitivePosixMicrosecondClockWithOffset -> { microseconds. offsetMicroseconds }
PrimitiveResult Interpreter::primitivePosixMicrosecondClockWithOffset(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Get current time
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    int64_t microseconds = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();

    // Get timezone offset
    time_t nowTime = time(nullptr);
    struct tm* local = localtime(&nowTime);
    int64_t offsetMicroseconds = static_cast<int64_t>(local->tm_gmtoff) * 1000000LL;

    // Allocate a 2-element array for result
    uint32_t arrayClassIndex = memory_.indexOfClass(
        memory_.specialObject(SpecialObjectIndex::ClassArray));
    Oop result = memory_.allocateSlots(arrayClassIndex, 2);
    if (result.isNil()) {
        return PrimitiveResult::Failure;
    }

    memory_.storePointer(0, result, Oop::fromSmallInteger(microseconds));
    memory_.storePointer(1, result, Oop::fromSmallInteger(offsetMicroseconds));

    pop();  // receiver
    push(result);
    return PrimitiveResult::Success;
}

// Primitive 248: System timezone (full name)
// primitiveSystemTimezone -> string with full timezone name
PrimitiveResult Interpreter::primitiveSystemTimezone(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    // Try to get TZ environment variable or use system default
    const char* tz = getenv("TZ");
    if (!tz || strlen(tz) == 0) {
        // Fall back to abbreviation
        time_t now = time(nullptr);
        struct tm* local = localtime(&now);
        tz = local->tm_zone ? local->tm_zone : "UTC";
    }

    Oop result = createStringObject(memory_, tz);
    if (result.isNil()) {
        return PrimitiveResult::Failure;
    }

    pop();  // receiver
    push(result);
    return PrimitiveResult::Success;
}

// Primitive 249: High resolution clock (monotonic)
// primitiveHighResClock -> nanoseconds from monotonic clock
PrimitiveResult Interpreter::primitiveHighResClock(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    auto now = std::chrono::steady_clock::now();
    auto duration = now.time_since_epoch();
    int64_t nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();

    pop();  // receiver
    push(Oop::fromSmallInteger(nanoseconds));
    return PrimitiveResult::Success;
}

// Primitive 250: UTC date and time components
// primitiveUTCDateAndTime -> array of {year, month, day, hour, minute, second, microsecond}
PrimitiveResult Interpreter::primitiveUTCDateAndTime(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();

    time_t nowTime = std::chrono::system_clock::to_time_t(now);
    struct tm* utc = gmtime(&nowTime);

    // Allocate 7-element array
    uint32_t arrayClassIndex = memory_.indexOfClass(
        memory_.specialObject(SpecialObjectIndex::ClassArray));
    Oop result = memory_.allocateSlots(arrayClassIndex, 7);
    if (result.isNil()) {
        return PrimitiveResult::Failure;
    }

    memory_.storePointer(0, result, Oop::fromSmallInteger(utc->tm_year + 1900));
    memory_.storePointer(1, result, Oop::fromSmallInteger(utc->tm_mon + 1));
    memory_.storePointer(2, result, Oop::fromSmallInteger(utc->tm_mday));
    memory_.storePointer(3, result, Oop::fromSmallInteger(utc->tm_hour));
    memory_.storePointer(4, result, Oop::fromSmallInteger(utc->tm_min));
    memory_.storePointer(5, result, Oop::fromSmallInteger(utc->tm_sec));
    memory_.storePointer(6, result, Oop::fromSmallInteger(microseconds % 1000000));

    pop();  // receiver
    push(result);
    return PrimitiveResult::Success;
}

// Primitive 251: Local date and time components
// primitiveLocalDateAndTime -> array of {year, month, day, hour, minute, second, microsecond, offset}
PrimitiveResult Interpreter::primitiveLocalDateAndTime(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();

    time_t nowTime = std::chrono::system_clock::to_time_t(now);
    struct tm* local = localtime(&nowTime);

    // Allocate 8-element array (includes timezone offset)
    uint32_t arrayClassIndex = memory_.indexOfClass(
        memory_.specialObject(SpecialObjectIndex::ClassArray));
    Oop result = memory_.allocateSlots(arrayClassIndex, 8);
    if (result.isNil()) {
        return PrimitiveResult::Failure;
    }

    memory_.storePointer(0, result, Oop::fromSmallInteger(local->tm_year + 1900));
    memory_.storePointer(1, result, Oop::fromSmallInteger(local->tm_mon + 1));
    memory_.storePointer(2, result, Oop::fromSmallInteger(local->tm_mday));
    memory_.storePointer(3, result, Oop::fromSmallInteger(local->tm_hour));
    memory_.storePointer(4, result, Oop::fromSmallInteger(local->tm_min));
    memory_.storePointer(5, result, Oop::fromSmallInteger(local->tm_sec));
    memory_.storePointer(6, result, Oop::fromSmallInteger(microseconds % 1000000));
    memory_.storePointer(7, result, Oop::fromSmallInteger(local->tm_gmtoff / 60));  // offset in minutes

    pop();  // receiver
    push(result);
    return PrimitiveResult::Success;
}

// Primitive 252: Nanosecond clock
// primitiveNanosecondClock -> nanoseconds since epoch
PrimitiveResult Interpreter::primitiveNanosecondClock(int argCount) {
    if (argCount != 0) return PrimitiveResult::Failure;

    auto now = std::chrono::high_resolution_clock::now();
    auto duration = now.time_since_epoch();
    int64_t nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();

    pop();  // receiver
    push(Oop::fromSmallInteger(nanoseconds));
    return PrimitiveResult::Success;
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
// anObject hash primitiveSetIdentityHash -> anObject
// Sets the identity hash of an object
PrimitiveResult Interpreter::primitiveSetIdentityHash(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop hashOop = stackTop();
    Oop receiver = stackValue(1);

    if (!hashOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    if (receiver.isImmediate()) {
        // Can't set hash on immediates
        return PrimitiveResult::Failure;
    }

    int64_t hash = hashOop.asSmallInteger();
    if (hash < 0 || hash > 0x3FFFFF) {  // 22-bit hash max
        return PrimitiveResult::Failure;
    }

    // Get object header and set hash
    ObjectHeader* header = receiver.asObjectPtr();
    header->setIdentityHash(static_cast<uint32_t>(hash));

    pop();  // pop hash argument, leave receiver
    return PrimitiveResult::Success;
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
// Copies characters from source to dest
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
    size_t dstStart = static_cast<size_t>(destStart - 1);
    size_t dstEnd = static_cast<size_t>(destEnd);
    size_t count = dstEnd - dstStart;

    size_t destSize = memory_.byteSizeOf(destOop);
    size_t sourceSize = memory_.byteSizeOf(sourceOop);

    if (dstEnd > destSize || srcIdx + count > sourceSize) {
        return PrimitiveResult::Failure;
    }

    // Copy bytes
    for (size_t i = 0; i < count; i++) {
        uint8_t byte = memory_.fetchByte(srcIdx + i, sourceOop);
        memory_.storeByte(dstStart + i, destOop, byte);
    }

    popN(4);  // Pop 4 args, leave dest
    return PrimitiveResult::Success;
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

// Primitive 235: String compare with collation
// string1 string2 order primitiveStringCompareWith -> -1/0/1
// Compares strings using specified collation order
PrimitiveResult Interpreter::primitiveStringCompareWith(int argCount) {
    if (argCount != 2) return PrimitiveResult::Failure;

    Oop orderOop = stackTop();
    Oop string2Oop = stackValue(1);
    Oop string1Oop = stackValue(2);

    if (string1Oop.isImmediate() || string2Oop.isImmediate()) {
        return PrimitiveResult::Failure;
    }

    // Extract strings
    std::string str1 = extractString(memory_, string1Oop);
    std::string str2 = extractString(memory_, string2Oop);

    // Basic comparison (ignoring collation order for now)
    int result;
    if (str1 < str2) {
        result = -1;
    } else if (str1 > str2) {
        result = 1;
    } else {
        result = 0;
    }

    popN(3);  // pop order, string2, string1
    pop();    // pop receiver
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
PrimitiveResult Interpreter::primitiveGetNextEvent(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop eventBuffer = stackTop();

    if (eventBuffer.isImmediate()) {
        return PrimitiveResult::Failure;
    }

    // Event buffer format (8 slots):
    // 0: event type (0=none, 1=mouse, 2=key, 3=window, etc.)
    // 1-7: event-specific data

    // Check buffer has enough slots
    size_t slotCount = memory_.slotCountOf(eventBuffer);
    if (slotCount < 8) {
        return PrimitiveResult::Failure;
    }

    // For now, return "no event" (type 0)
    // A full implementation would dequeue from an event queue
    // populated by the iOS event loop
    memory_.storePointer(0, eventBuffer, Oop::fromSmallInteger(0));  // No event

    pop();  // pop eventBuffer argument, leave receiver
    return PrimitiveResult::Success;
}

// Primitive 265: Set input semaphore (variant 2)
// semaphoreIndex primitiveInputSemaphore2 -> receiver
// Sets the semaphore to signal when input is available
PrimitiveResult Interpreter::primitiveInputSemaphore2(int argCount) {
    if (argCount != 1) return PrimitiveResult::Failure;

    Oop semIndexOop = stackTop();

    if (!semIndexOop.isSmallInteger()) {
        return PrimitiveResult::Failure;
    }

    // Store the semaphore index for later signaling
    // A full implementation would register this with the event system
    // int64_t semIndex = semIndexOop.asSmallInteger();
    // inputSemaphoreIndex_ = semIndex;

    pop();  // pop argument, leave receiver
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
        case 3:  // Flush
            result = 0;  // Return count of flushed events
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
    // Sound primitives require platform-specific audio support
    // For now, fail to Smalltalk fallback which can handle
    // sound through alternative means or report unavailable
    return PrimitiveResult::Failure;
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

} // namespace pharo
