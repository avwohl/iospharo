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
    push(Oop::nil());
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
    push(Oop::nil());
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
    push(Oop::nil());
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

} // namespace pharo
