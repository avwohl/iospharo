// InterpreterProxy.cpp - Maps VMMaker plugin API to our clean C++ VM
//
// This file provides the VirtualMachine (interpreter proxy) struct that
// VMMaker-generated plugins like B2DPlugin expect. Each function pointer
// in the struct maps to our Interpreter and ObjectMemory classes.

#include "InterpreterProxy.h"
#include "Interpreter.hpp"
#include "../platform/DisplaySurface.hpp"
#include <cstring>
#include <ctime>
#include <string>
#include <sys/time.h>

using namespace pharo;

// Global interpreter pointer (set during initialization)
static Interpreter* gInterp = nullptr;
static ObjectMemory* gMem = nullptr;

// Primitive failure flag (plugins set this to indicate failure)
static bool gFailed = false;
static sqInt gFailureCode = 0;

// Remappable oop stack (for GC-safe oop handling in plugins)
static const int kMaxRemappable = 32;
static sqInt gRemappableOops[kMaxRemappable];
static int gRemappableTop = 0;

// =====================================================================
// Helper: create a boxed float (no newFloat on ObjectMemory)
// =====================================================================
static Oop boxFloat(double value) {
    // Get Float class from special objects
    Oop floatClass = gMem->specialObject(SpecialObjectIndex::ClassFloat);
    if (!floatClass.isObject()) return Oop::nil();
    uint32_t classIdx = floatClass.asObjectPtr()->identityHash();
    Oop result = gMem->allocateBytes(classIdx, 8);
    if (result.isNil()) return Oop::nil();
    memcpy(result.asObjectPtr()->bytes(), &value, 8);
    // Set format to bytes (Indexable8 = format 16)
    result.asObjectPtr()->setFormat(ObjectFormat::Indexable8);
    return result;
}

// =====================================================================
// Helper: convert between our Oop and sqInt
// =====================================================================
static inline sqInt oopToSqInt(Oop oop) {
    return static_cast<sqInt>(oop.rawBits());
}

static inline Oop sqIntToOop(sqInt value) {
    return Oop::fromRawBits(static_cast<uint64_t>(value));
}

// =====================================================================
// Version
// =====================================================================
static sqInt proxy_minorVersion() { return VM_PROXY_MINOR; }
static sqInt proxy_majorVersion() { return VM_PROXY_MAJOR; }

// =====================================================================
// Stack access
// =====================================================================
static sqInt proxy_pop(sqInt nItems) {
    gInterp->popN(static_cast<size_t>(nItems));
    return 0;
}

static void proxy_popthenPush(sqInt nItems, sqInt oop) {
    gInterp->popN(static_cast<size_t>(nItems));
    gInterp->push(sqIntToOop(oop));
}

static void proxy_push(sqInt object) {
    gInterp->push(sqIntToOop(object));
}

static sqInt proxy_pushBool(sqInt trueOrFalse) {
    if (trueOrFalse) {
        gInterp->push(gMem->trueObject());
    } else {
        gInterp->push(gMem->falseObject());
    }
    return 0;
}

static void proxy_pushFloat(double f) {
    Oop floatObj = boxFloat(f);
    gInterp->push(floatObj);
}

static sqInt proxy_pushInteger(sqInt integerValue) {
    gInterp->push(Oop::fromSmallInteger(integerValue));
    return 0;
}

static double proxy_stackFloatValue(sqInt offset) {
    Oop oop = gInterp->stackValue(static_cast<int>(offset));
    if (oop.isSmallFloat()) {
        return oop.asSmallFloat();
    }
    if (oop.isObject()) {
        ObjectHeader* hdr = oop.asObjectPtr();
        if (hdr->isBytesObject() && hdr->byteSize() == 8) {
            double val;
            memcpy(&val, hdr->bytes(), 8);
            return val;
        }
    }
    gFailed = true;
    return 0.0;
}

static sqInt proxy_stackIntegerValue(sqInt offset) {
    Oop oop = gInterp->stackValue(static_cast<int>(offset));
    if (oop.isSmallInteger()) {
        return static_cast<sqInt>(oop.asSmallInteger());
    }
    gFailed = true;
    return 0;
}

static sqInt proxy_stackObjectValue(sqInt offset) {
    Oop oop = gInterp->stackValue(static_cast<int>(offset));
    if (oop.isSmallInteger()) {
        gFailed = true;
        return 0;
    }
    return oopToSqInt(oop);
}

static sqInt proxy_stackValue(sqInt offset) {
    return oopToSqInt(gInterp->stackValue(static_cast<int>(offset)));
}

// =====================================================================
// Object access
// =====================================================================
static sqInt proxy_argumentCountOf(sqInt methodPointer) {
    // Read from method header
    Oop method = sqIntToOop(methodPointer);
    if (!method.isObject()) return 0;
    Oop header = gMem->fetchPointer(0, method);
    if (header.isSmallInteger()) {
        int64_t hdr = header.asSmallInteger();
        return (hdr >> 24) & 0xF; // numArgs in bits 27-24
    }
    return 0;
}

static void* proxy_arrayValueOf(sqInt oop) {
    Oop obj = sqIntToOop(oop);
    if (!obj.isObject()) return nullptr;
    ObjectHeader* hdr = obj.asObjectPtr();
    if (hdr->isBytesObject()) {
        return hdr->bytes();
    }
    return hdr->slots();
}

static sqInt proxy_byteSizeOf(sqInt oop) {
    Oop obj = sqIntToOop(oop);
    if (!obj.isObject()) return 0;
    ObjectHeader* hdr = obj.asObjectPtr();
    if (hdr->isBytesObject()) {
        return static_cast<sqInt>(hdr->byteSize());
    }
    return static_cast<sqInt>(hdr->slotCount() * sizeof(Oop));
}

static void* proxy_fetchArrayofObject(sqInt fieldIndex, sqInt objectPointer) {
    Oop obj = sqIntToOop(objectPointer);
    Oop field = gMem->fetchPointer(static_cast<int>(fieldIndex), obj);
    if (!field.isObject()) return nullptr;
    ObjectHeader* hdr = field.asObjectPtr();
    if (hdr->isBytesObject()) return hdr->bytes();
    return hdr->slots();
}

static sqInt proxy_fetchClassOf(sqInt oop) {
    return oopToSqInt(gMem->classOf(sqIntToOop(oop)));
}

static double proxy_fetchFloatofObject(sqInt fieldIndex, sqInt objectPointer) {
    Oop obj = sqIntToOop(objectPointer);
    Oop field = gMem->fetchPointer(static_cast<int>(fieldIndex), obj);
    if (field.isSmallFloat()) return field.asSmallFloat();
    if (field.isObject()) {
        ObjectHeader* hdr = field.asObjectPtr();
        if (hdr->isBytesObject() && hdr->byteSize() == 8) {
            double val;
            memcpy(&val, hdr->bytes(), 8);
            return val;
        }
    }
    gFailed = true;
    return 0.0;
}

static sqInt proxy_fetchIntegerofObject(sqInt fieldIndex, sqInt objectPointer) {
    Oop obj = sqIntToOop(objectPointer);
    Oop field = gMem->fetchPointer(static_cast<int>(fieldIndex), obj);
    if (field.isSmallInteger()) {
        return static_cast<sqInt>(field.asSmallInteger());
    }
    gFailed = true;
    return 0;
}

static sqInt proxy_fetchPointerofObject(sqInt fieldIndex, sqInt oop) {
    return oopToSqInt(gMem->fetchPointer(static_cast<int>(fieldIndex), sqIntToOop(oop)));
}

static sqInt proxy_obsoleteDontUseThisFetchWordofObject(sqInt fieldIndex, sqInt oop) {
    return proxy_fetchIntegerofObject(fieldIndex, oop);
}

static void* proxy_firstFixedField(sqInt oop) {
    Oop obj = sqIntToOop(oop);
    if (!obj.isObject()) return nullptr;
    return obj.asObjectPtr()->slots();
}

static void* proxy_firstIndexableField(sqInt oop) {
    Oop obj = sqIntToOop(oop);
    if (!obj.isObject()) return nullptr;
    ObjectHeader* hdr = obj.asObjectPtr();
    if (hdr->isBytesObject()) {
        return hdr->bytes();
    }
    // For pointer objects, return pointer to first slot
    // For word objects, return pointer to raw data
    auto format = hdr->format();
    if (format == ObjectFormat::Indexable32 || format == ObjectFormat::Indexable32Odd) {
        // Indexable 32-bit words
        return hdr->bytes();
    }
    if (format >= ObjectFormat::Indexable16 && format <= ObjectFormat::Indexable16_3) {
        // Indexable 16-bit
        return hdr->bytes();
    }
    // Pointer-based indexable (format 2-4)
    // For objects with fixed fields + indexable, skip fixed fields
    // The class specifies how many fixed fields
    Oop cls = gMem->classOf(obj);
    if (cls.isObject()) {
        ObjectHeader* clsHdr = cls.asObjectPtr();
        if (clsHdr->slotCount() >= 3) {
            Oop formatOop = clsHdr->slotAt(2); // format slot
            if (formatOop.isSmallInteger()) {
                int clsFmt = static_cast<int>(formatOop.asSmallInteger());
                int fixedFields = clsFmt & 0xFFFF;
                // Return pointer past fixed fields
                return &(hdr->slots()[fixedFields]);
            }
        }
    }
    return hdr->slots();
}

static sqInt proxy_literalofMethod(sqInt offset, sqInt methodPointer) {
    Oop method = sqIntToOop(methodPointer);
    return oopToSqInt(gMem->fetchPointer(static_cast<int>(offset + 1), method)); // +1 for header
}

static sqInt proxy_literalCountOf(sqInt methodPointer) {
    Oop method = sqIntToOop(methodPointer);
    Oop header = gMem->fetchPointer(0, method);
    if (header.isSmallInteger()) {
        return static_cast<sqInt>(header.asSmallInteger() & 0x7FFF);
    }
    return 0;
}

static sqInt proxy_methodArgumentCount() {
    return static_cast<sqInt>(gInterp->argumentCount());
}

static sqInt proxy_methodPrimitiveIndex() {
    return static_cast<sqInt>(0 /* primitiveIndex not needed by B2DPlugin */);
}

static sqInt proxy_primitiveIndexOf(sqInt methodPointer) {
    Oop method = sqIntToOop(methodPointer);
    Oop header = gMem->fetchPointer(0, method);
    if (header.isSmallInteger()) {
        int64_t hdr = header.asSmallInteger();
        if (hdr & (1 << 16)) { // hasPrimitive flag
            return static_cast<sqInt>((hdr >> 1) & 0xFFFF);
        }
    }
    return 0;
}

static sqInt proxy_sizeOfSTArrayFromCPrimitive(void* cPtr) {
    return 0; // Not used by B2DPlugin
}

static sqInt proxy_slotSizeOf(sqInt oop) {
    // In the standard VM, slotSizeOf delegates to stSizeOf which returns
    // the element count appropriate for the format:
    // - Pointer objects: slot count
    // - 32-bit word objects: 32-bit word count
    // - Byte objects: byte count
    Oop obj = sqIntToOop(oop);
    if (!obj.isObject()) return 0;
    ObjectHeader* hdr = obj.asObjectPtr();
    auto format = hdr->format();
    size_t slots = hdr->slotCount();

    // 32-bit word indexable (e.g., Bitmap, FloatArray, MatrixTransform2x3)
    if (format == ObjectFormat::Indexable32 || format == ObjectFormat::Indexable32Odd) {
        size_t bytes = hdr->byteSize();
        return static_cast<sqInt>(bytes / 4); // 32-bit word count
    }
    // Byte indexable (String, ByteArray)
    if (format >= ObjectFormat::Indexable8 && format <= ObjectFormat::Indexable8_7) {
        return static_cast<sqInt>(hdr->byteSize());
    }
    // 16-bit indexable
    if (format >= ObjectFormat::Indexable16 && format <= ObjectFormat::Indexable16_3) {
        size_t bytes = hdr->byteSize();
        return static_cast<sqInt>(bytes / 2);
    }
    // Pointer and other objects: return raw slot count
    return static_cast<sqInt>(slots);
}

static sqInt proxy_stObjectat(sqInt array, sqInt fieldIndex) {
    return oopToSqInt(gMem->fetchPointer(static_cast<int>(fieldIndex - 1), sqIntToOop(array)));
}

static sqInt proxy_stObjectatput(sqInt array, sqInt fieldIndex, sqInt value) {
    gMem->storePointer(static_cast<int>(fieldIndex - 1), sqIntToOop(array), sqIntToOop(value));
    return value;
}

static sqInt proxy_stSizeOf(sqInt oop) {
    Oop obj = sqIntToOop(oop);
    if (!obj.isObject()) return 0;
    ObjectHeader* hdr = obj.asObjectPtr();
    if (hdr->isBytesObject()) {
        return static_cast<sqInt>(hdr->byteSize());
    }
    // For word arrays, return word count
    auto format = hdr->format();
    if (format == ObjectFormat::Indexable32 || format == ObjectFormat::Indexable32Odd) {
        size_t bytes = hdr->byteSize();
        return static_cast<sqInt>(bytes / 4); // 32-bit words
    }
    return static_cast<sqInt>(hdr->slotCount());
}

static sqInt proxy_storeIntegerofObjectwithValue(sqInt fieldIndex, sqInt oop, sqInt integer) {
    Oop obj = sqIntToOop(oop);
    gMem->storePointer(static_cast<int>(fieldIndex), obj, Oop::fromSmallInteger(integer));
    return 0;
}

static sqInt proxy_storePointerofObjectwithValue(sqInt fieldIndex, sqInt oop, sqInt valuePointer) {
    gMem->storePointer(static_cast<int>(fieldIndex), sqIntToOop(oop), sqIntToOop(valuePointer));
    return valuePointer;
}

// =====================================================================
// Testing
// =====================================================================
static sqInt proxy_isKindOf(sqInt oop, char* aString) {
    if (!aString) return 0;
    Oop obj = sqIntToOop(oop);
    Oop cls = gMem->classOf(obj);
    Oop prev = Oop::nil();
    while (cls.isObject() && cls.rawBits() != prev.rawBits()) {
        std::string name = gMem->nameOfClass(cls);
        if (name != "?" && name == aString) return 1;
        prev = cls;
        ObjectHeader* hdr = cls.asObjectPtr();
        if (hdr->slotCount() < 1) break;
        cls = hdr->slotAt(0);  // superclass
    }
    return 0;
}

static sqInt proxy_isMemberOf(sqInt oop, char* aString) {
    if (!aString) return 0;
    Oop obj = sqIntToOop(oop);
    std::string name = gMem->classNameOf(obj);
    return (name != "?" && name == aString) ? 1 : 0;
}

static sqInt proxy_isBytes(sqInt oop) {
    Oop obj = sqIntToOop(oop);
    if (!obj.isObject()) return 0;
    return obj.asObjectPtr()->isBytesObject() ? 1 : 0;
}

static sqInt proxy_isFloatObject(sqInt oop) {
    Oop obj = sqIntToOop(oop);
    if (obj.isSmallFloat()) return 1;
    if (!obj.isObject()) return 0;
    // Check class == Float
    Oop cls = gMem->classOf(obj);
    return (cls.rawBits() == gMem->specialObject(SpecialObjectIndex::ClassFloat).rawBits()) ? 1 : 0; // classFloat
}

static sqInt proxy_isIndexable(sqInt oop) {
    Oop obj = sqIntToOop(oop);
    if (!obj.isObject()) return 0;
    auto format = obj.asObjectPtr()->format();
    return (format >= ObjectFormat::Indexable) ? 1 : 0; // Formats 2+ are indexable
}

static sqInt proxy_isIntegerObject(sqInt oop) {
    return sqIntToOop(oop).isSmallInteger() ? 1 : 0;
}

static sqInt proxy_isIntegerValue(sqInt intValue) {
    // Can this value fit in a SmallInteger?
    return (intValue >= -0x1000000000000LL && intValue <= 0x0FFFFFFFFFFFFFLL) ? 1 : 0;
}

static sqInt proxy_isPointers(sqInt oop) {
    Oop obj = sqIntToOop(oop);
    if (!obj.isObject()) return 0;
    auto format = obj.asObjectPtr()->format();
    return (format <= ObjectFormat::Weak) ? 1 : 0; // Formats 0-4 are pointer objects
}

static sqInt proxy_isWeak(sqInt oop) {
    Oop obj = sqIntToOop(oop);
    if (!obj.isObject()) return 0;
    return (obj.asObjectPtr()->format() == ObjectFormat::Weak) ? 1 : 0;
}

static sqInt proxy_isWords(sqInt oop) {
    Oop obj = sqIntToOop(oop);
    if (!obj.isObject()) return 0;
    auto format = obj.asObjectPtr()->format();
    return (format == ObjectFormat::Indexable32 || format == ObjectFormat::Indexable32Odd) ? 1 : 0; // Word arrays
}

static sqInt proxy_isWordsOrBytes(sqInt oop) {
    return proxy_isWords(oop) || proxy_isBytes(oop);
}

// =====================================================================
// Converting
// =====================================================================
static sqInt proxy_booleanValueOf(sqInt obj) {
    Oop oop = sqIntToOop(obj);
    if (oop.rawBits() == gMem->trueObject().rawBits()) return 1;
    if (oop.rawBits() == gMem->falseObject().rawBits()) return 0;
    gFailed = true;
    return 0;
}

static sqInt proxy_checkedIntegerValueOf(sqInt intOop) {
    Oop oop = sqIntToOop(intOop);
    if (oop.isSmallInteger()) return static_cast<sqInt>(oop.asSmallInteger());
    gFailed = true;
    return 0;
}

static sqInt proxy_floatObjectOf(double aFloat) {
    // Try to use SmallFloat encoding first
    Oop result;
    if (Oop::tryFromSmallFloat(aFloat, result)) return oopToSqInt(result);
    // Allocate boxed float
    return oopToSqInt(boxFloat(aFloat));
}

static double proxy_floatValueOf(sqInt oop) {
    Oop obj = sqIntToOop(oop);
    if (obj.isSmallFloat()) return obj.asSmallFloat();
    if (obj.isObject()) {
        ObjectHeader* hdr = obj.asObjectPtr();
        if (hdr->isBytesObject() && hdr->byteSize() == 8) {
            double val;
            memcpy(&val, hdr->bytes(), 8);
            return val;
        }
    }
    gFailed = true;
    return 0.0;
}

static sqInt proxy_integerObjectOf(sqInt value) {
    return oopToSqInt(Oop::fromSmallInteger(value));
}

static sqInt proxy_integerValueOf(sqInt oop) {
    Oop obj = sqIntToOop(oop);
    if (obj.isSmallInteger()) {
        return static_cast<sqInt>(obj.asSmallInteger());
    }
    gFailed = true;
    return 0;
}

static sqInt proxy_positive32BitIntegerFor(unsigned int integerValue) {
    // All 32-bit unsigned values fit in 64-bit SmallInteger range
    return oopToSqInt(Oop::fromSmallInteger(static_cast<int64_t>(integerValue)));
}

static usqInt proxy_positive32BitValueOf(sqInt oop) {
    Oop obj = sqIntToOop(oop);
    if (obj.isSmallInteger()) {
        int64_t val = obj.asSmallInteger();
        if (val >= 0 && val <= 0xFFFFFFFFLL) {
            return static_cast<usqInt>(val);
        }
    }
    if (obj.isObject()) {
        // LargePositiveInteger
        ObjectHeader* hdr = obj.asObjectPtr();
        if (hdr->isBytesObject()) {
            size_t sz = hdr->byteSize();
            uint8_t* bytes = hdr->bytes();
            uint32_t result = 0;
            for (size_t i = 0; i < sz && i < 4; i++) {
                result |= (static_cast<uint32_t>(bytes[i]) << (i * 8));
            }
            return static_cast<usqInt>(result);
        }
    }
    gFailed = true;
    return 0;
}

// =====================================================================
// Special objects
// =====================================================================
static sqInt proxy_falseObject() { return oopToSqInt(gMem->falseObject()); }
static sqInt proxy_nilObject() { return oopToSqInt(gMem->nil()); }
static sqInt proxy_trueObject() { return oopToSqInt(gMem->trueObject()); }

// =====================================================================
// Special classes
// =====================================================================
static sqInt proxy_classArray() { return oopToSqInt(gMem->specialObject(SpecialObjectIndex::ClassArray)); }
static sqInt proxy_classBitmap() { return oopToSqInt(gMem->specialObject(SpecialObjectIndex::ClassBitmap)); }
static sqInt proxy_classByteArray() { return oopToSqInt(gMem->specialObject(static_cast<SpecialObjectIndex>(26))); }
static sqInt proxy_classCharacter() { return oopToSqInt(gMem->specialObject(static_cast<SpecialObjectIndex>(19))); }
static sqInt proxy_classFloat() { return oopToSqInt(gMem->specialObject(SpecialObjectIndex::ClassFloat)); }
static sqInt proxy_classLargePositiveInteger() { return oopToSqInt(gMem->specialObject(static_cast<SpecialObjectIndex>(13))); }
static sqInt proxy_classPoint() { return oopToSqInt(gMem->specialObject(SpecialObjectIndex::ClassPoint)); }
static sqInt proxy_classSemaphore() { return oopToSqInt(gMem->specialObject(static_cast<SpecialObjectIndex>(18))); }
static sqInt proxy_classSmallInteger() { return oopToSqInt(gMem->specialObject(SpecialObjectIndex::ClassSmallInteger)); }
static sqInt proxy_classString() { return oopToSqInt(gMem->specialObject(SpecialObjectIndex::ClassByteString)); }

// =====================================================================
// Instance creation
// =====================================================================
static sqInt proxy_clone(sqInt oop) {
    // Shallow copy
    Oop obj = sqIntToOop(oop);
    if (!obj.isObject()) return oop;
    ObjectHeader* src = obj.asObjectPtr();
    size_t totalSize = src->totalSize();
    Oop copy = gMem->allocateBytes(src->classIndex(), totalSize - sizeof(ObjectHeader));
    if (copy.isNil()) { gFailed = true; return oop; }
    ObjectHeader* dst = copy.asObjectPtr();
    memcpy(dst->bytes(), src->bytes(), totalSize - sizeof(ObjectHeader));
    return oopToSqInt(copy);
}

static sqInt proxy_instantiateClassindexableSize(sqInt classPointer, sqInt size) {
    Oop cls = sqIntToOop(classPointer);
    if (!cls.isObject()) { gFailed = true; return proxy_nilObject(); }

    // Get class format to determine what kind of object to create
    ObjectHeader* clsHdr = cls.asObjectPtr();
    if (clsHdr->slotCount() < 3) { gFailed = true; return proxy_nilObject(); }
    Oop fmtOop = clsHdr->slotAt(2);
    if (!fmtOop.isSmallInteger()) { gFailed = true; return proxy_nilObject(); }
    int instSpec = (static_cast<int>(fmtOop.asSmallInteger()) >> 16) & 0x1F;

    uint32_t classIdx = clsHdr->classIndex(); // class's own identity hash / classIndex
    // For class objects, the class index to use for instances is stored differently
    // We need the class table index
    classIdx = clsHdr->identityHash();

    if (instSpec >= 16) {
        // Byte array
        Oop result = gMem->allocateBytes(classIdx, static_cast<size_t>(size));
        if (result.isNil()) gFailed = true;
        return oopToSqInt(result);
    } else if (instSpec >= 10) {
        // Word array (32-bit)
        Oop result = gMem->allocateBytes(classIdx, static_cast<size_t>(size) * 4);
        if (result.isNil()) gFailed = true;
        return oopToSqInt(result);
    } else {
        // Pointer object — instSpec maps directly to ObjectFormat
        ObjectFormat fmt = static_cast<ObjectFormat>(instSpec);
        Oop result = gMem->allocateSlots(classIdx, static_cast<size_t>(size), fmt);
        if (result.isNil()) gFailed = true;
        return oopToSqInt(result);
    }
}

static sqInt proxy_makePointwithxValueyValue(sqInt xValue, sqInt yValue) {
    // Create a Point object: class is specialObject(12)
    Oop pointClass = gMem->specialObject(SpecialObjectIndex::ClassPoint);
    if (!pointClass.isObject()) { gFailed = true; return proxy_nilObject(); }

    uint32_t classIdx = pointClass.asObjectPtr()->identityHash();
    Oop point = gMem->allocateSlots(classIdx, 2);
    if (point.isNil()) { gFailed = true; return proxy_nilObject(); }

    gMem->storePointer(0, point, Oop::fromSmallInteger(xValue));
    gMem->storePointer(1, point, Oop::fromSmallInteger(yValue));
    return oopToSqInt(point);
}

static sqInt proxy_popRemappableOop() {
    if (gRemappableTop <= 0) return proxy_nilObject();
    return gRemappableOops[--gRemappableTop];
}

static void proxy_pushRemappableOop(sqInt oop) {
    if (gRemappableTop < kMaxRemappable) {
        gRemappableOops[gRemappableTop++] = oop;
    }
}

// =====================================================================
// Other
// =====================================================================
static sqInt proxy_becomewith(sqInt array1, sqInt array2) {
    gFailed = true; // Not implemented
    return 0;
}

static sqInt proxy_byteSwapped(sqInt w) {
    return static_cast<sqInt>(__builtin_bswap64(static_cast<uint64_t>(w)));
}

static sqInt proxy_failed() {
    return gFailed ? 1 : 0;
}

static void proxy_fullGC() {
    (void)gMem->fullGC();
}

static sqInt proxy_primitiveFail() {
    gFailed = true;
    return 0;
}

static sqInt proxy_showDisplayBitsLeftTopRightBottom(sqInt aForm, sqInt l, sqInt t, sqInt r, sqInt b) {
    if (pharo::gDisplaySurface) {
        pharo::gDisplaySurface->invalidateRect(static_cast<int>(l), static_cast<int>(t),
                                                static_cast<int>(r - l), static_cast<int>(b - t));
    }
    return 0;
}

static sqInt proxy_signalSemaphoreWithIndex(sqInt semaIndex) {
    gInterp->signalExternalSemaphore(static_cast<int>(semaIndex));
    return 0;
}

static sqInt proxy_success(sqInt aBoolean) {
    if (!aBoolean) gFailed = true;
    return 0;
}

static sqInt proxy_superclassOf(sqInt classPointer) {
    Oop cls = sqIntToOop(classPointer);
    if (!cls.isObject()) return proxy_nilObject();
    ObjectHeader* hdr = cls.asObjectPtr();
    if (hdr->slotCount() < 1) return proxy_nilObject();
    return oopToSqInt(hdr->slotAt(0)); // superclass is slot 0
}

static sqInt proxy_statNumGCs() { return 0; }
static sqInt proxy_stringForCString(const char* s) {
    if (!s) return proxy_nilObject();
    Oop result = gMem->createString(std::string(s));
    if (result.isNil()) { gFailed = true; return proxy_nilObject(); }
    return oopToSqInt(result);
}

// =====================================================================
// BitBlt support for B2DPlugin
// =====================================================================

// BitBlt field indices (must match Squeak/Pharo BitBlt layout)
enum {
    ProxyBBDestForm = 0,
    ProxyBBSourceForm = 1,
    ProxyBBHalftoneForm = 2,
    ProxyBBCombinationRule = 3,
    ProxyBBDestX = 4,
    ProxyBBDestY = 5,
    ProxyBBWidth = 6,
    ProxyBBHeight = 7,
    ProxyBBSourceX = 8,
    ProxyBBSourceY = 9,
    ProxyBBClipX = 10,
    ProxyBBClipY = 11,
    ProxyBBClipWidth = 12,
    ProxyBBClipHeight = 13,
};

// Form field indices
enum {
    ProxyFormBits = 0,
    ProxyFormWidth = 1,
    ProxyFormHeight = 2,
    ProxyFormDepth = 3,
};

// Stored BitBlt state from loadBitBltFrom
static struct {
    bool loaded;
    uint32_t* destPixels;
    int destWidth;
    int destHeight;
    int destDepth;
    int destPitch;  // in pixels (32-bit words)
    uint32_t* srcPixels;
    int srcWidth;
    int srcHeight;
    int srcDepth;
    int combinationRule;
    int clipX, clipY, clipW, clipH;
} gBBState = {};

// Helper to read an integer field from a Smalltalk object
static int proxyIntField(Oop obj, int idx) {
    Oop field = gMem->fetchPointer(idx, obj);
    if (field.isSmallInteger()) return static_cast<int>(field.asSmallInteger());
    return 0;
}

// Helper to resolve a Form's pixel pointer and dimensions
static uint32_t* resolveFormPixels(Oop form, int& width, int& height, int& depth, int& pitch) {
    if (!form.isObject()) return nullptr;

    width = proxyIntField(form, ProxyFormWidth);
    height = proxyIntField(form, ProxyFormHeight);
    depth = proxyIntField(form, ProxyFormDepth);
    if (width <= 0 || height <= 0 || depth <= 0) return nullptr;

    Oop bits = gMem->fetchPointer(ProxyFormBits, form);
    pitch = width;  // default: pitch = width in 32-bit words

    if (bits.isSmallInteger()) {
        // Surface handle — use global display surface
        if (pharo::gDisplaySurface) {
            pitch = static_cast<int>(pharo::gDisplaySurface->pitch() / 4);
            return pharo::gDisplaySurface->pixels();
        }
        return nullptr;
    }

    if (bits.isObject() && !bits.isNil()) {
        ObjectHeader* hdr = bits.asObjectPtr();
        if (hdr->byteSize() >= static_cast<size_t>(width * height * (depth / 8))) {
            return reinterpret_cast<uint32_t*>(hdr->bytes());
        }
    }

    return nullptr;
}

static sqInt proxy_loadBitBltFrom(sqInt bbOop) {
    gBBState.loaded = false;

    Oop bb = sqIntToOop(bbOop);
    if (!bb.isObject()) {
        return 0;
    }

    // Parse destination form
    Oop destForm = gMem->fetchPointer(ProxyBBDestForm, bb);
    int dw = 0, dh = 0, dd = 0, dp = 0;
    uint32_t* dstPx = resolveFormPixels(destForm, dw, dh, dd, dp);
    if (!dstPx || dd != 32) {
        return 0;
    }

    gBBState.destPixels = dstPx;
    gBBState.destWidth = dw;
    gBBState.destHeight = dh;
    gBBState.destDepth = dd;
    gBBState.destPitch = dp;

    // Parse source form (span buffer — always a Bitmap object)
    Oop srcForm = gMem->fetchPointer(ProxyBBSourceForm, bb);
    if (!srcForm.isNil() && srcForm.isObject()) {
        int sw = 0, sh = 0, sd = 0, sp = 0;
        uint32_t* srcPx = resolveFormPixels(srcForm, sw, sh, sd, sp);
        gBBState.srcPixels = srcPx;
        gBBState.srcWidth = sw;
        gBBState.srcHeight = sh;
        gBBState.srcDepth = sd;
    } else {
        gBBState.srcPixels = nullptr;
        gBBState.srcWidth = 0;
        gBBState.srcHeight = 0;
        gBBState.srcDepth = 0;
    }

    gBBState.combinationRule = proxyIntField(bb, ProxyBBCombinationRule);
    gBBState.clipX = proxyIntField(bb, ProxyBBClipX);
    gBBState.clipY = proxyIntField(bb, ProxyBBClipY);
    gBBState.clipW = proxyIntField(bb, ProxyBBClipWidth);
    gBBState.clipH = proxyIntField(bb, ProxyBBClipHeight);

    gBBState.loaded = true;
    return 1;
}

static sqInt proxy_copyBits() {
    // Full copyBits — not used by B2DPlugin (it uses copyBitsFromtoat)
    return 0;
}

static sqInt proxy_copyBitsFromtoat(sqInt leftX, sqInt rightX, sqInt yValue) {
    if (!gBBState.loaded || !gBBState.destPixels) {
        return 0;
    }
    if (!gBBState.srcPixels) {
        return 0;
    }

    // Clip to destination bounds
    int x0 = static_cast<int>(leftX);
    int x1 = static_cast<int>(rightX);
    int y = static_cast<int>(yValue);

    if (y < 0 || y >= gBBState.destHeight) return 0;
    if (x0 < 0) x0 = 0;
    if (x1 > gBBState.destWidth) x1 = gBBState.destWidth;
    if (x0 >= x1) return 0;

    // Clip to clip rect
    if (y < gBBState.clipY || y >= gBBState.clipY + gBBState.clipH) return 0;
    if (x0 < gBBState.clipX) x0 = gBBState.clipX;
    if (x1 > gBBState.clipX + gBBState.clipW) x1 = gBBState.clipX + gBBState.clipW;
    if (x0 >= x1) return 0;

    int width = x1 - x0;
    uint32_t* dst = gBBState.destPixels + y * gBBState.destPitch + x0;

    // Source: span buffer pixel data starting at x0
    // The span buffer is indexed from 0, so source offset = x0
    uint32_t* src = gBBState.srcPixels + x0;

    // Bounds check source
    if (x1 > gBBState.srcWidth) return 0;

    int rule = gBBState.combinationRule;
    switch (rule) {
        case 3:  // store (replace)
        case 34: // sourceWord
            memcpy(dst, src, width * 4);
            break;

        case 25: // paint (OR)
        case 7:  // OR
            for (int i = 0; i < width; i++) {
                dst[i] |= src[i];
            }
            break;

        case 24: { // alpha blend (source-over)
            for (int i = 0; i < width; i++) {
                uint32_t s = src[i];
                uint32_t sa = (s >> 24) & 0xFF;
                if (sa == 255) {
                    dst[i] = s;
                } else if (sa > 0) {
                    uint32_t invSa = 255 - sa;
                    uint32_t d = dst[i];
                    uint32_t rb = ((s & 0xFF00FF) * sa + (d & 0xFF00FF) * invSa + 0x800080);
                    rb = ((rb + ((rb >> 8) & 0xFF00FF)) >> 8) & 0xFF00FF;
                    uint32_t g = ((s & 0x00FF00) * sa + (d & 0x00FF00) * invSa + 0x008000);
                    g = ((g + ((g >> 8) & 0x00FF00)) >> 8) & 0x00FF00;
                    uint32_t da = (d >> 24) & 0xFF;
                    uint32_t oa = sa + ((da * invSa + 127) / 255);
                    dst[i] = rb | g | (oa << 24);
                }
            }
            break;
        }

        case 20: { // rgbAdd (additive blend)
            for (int i = 0; i < width; i++) {
                uint32_t s = src[i];
                uint32_t d = dst[i];
                uint32_t r = ((s >> 16) & 0xFF) + ((d >> 16) & 0xFF);
                uint32_t g = ((s >> 8) & 0xFF) + ((d >> 8) & 0xFF);
                uint32_t b = (s & 0xFF) + (d & 0xFF);
                if (r > 255) r = 255;
                if (g > 255) g = 255;
                if (b > 255) b = 255;
                dst[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
            }
            break;
        }

        default:
            // Fallback: just store (covers most rendering cases)
            memcpy(dst, src, width * 4);
            break;
    }

    // Notify display surface of the changed area
    if (pharo::gDisplaySurface) {
        pharo::gDisplaySurface->invalidateRect(x0, y, x1 - x0, 1);
    }

    return 1;
}

// =====================================================================
// More functions
// =====================================================================
static sqInt proxy_classLargeNegativeInteger() { return oopToSqInt(gMem->specialObject(static_cast<SpecialObjectIndex>(42))); }
static sqInt proxy_signed32BitIntegerFor(sqInt intValue) {
    return oopToSqInt(Oop::fromSmallInteger(intValue));
}
static int proxy_signed32BitValueOf(sqInt oop) {
    Oop obj = sqIntToOop(oop);
    if (obj.isSmallInteger()) return static_cast<int>(obj.asSmallInteger());
    gFailed = true;
    return 0;
}

static sqInt proxy_includesBehaviorThatOf(sqInt aClass, sqInt aSuperClass) {
    Oop cls = sqIntToOop(aClass);
    Oop super = sqIntToOop(aSuperClass);
    Oop prev = Oop::nil();
    while (cls.isObject()) {
        if (cls.rawBits() == super.rawBits()) return 1;
        if (cls.rawBits() == prev.rawBits()) break;  // cycle guard
        prev = cls;
        ObjectHeader* hdr = cls.asObjectPtr();
        if (hdr->slotCount() < 1) break;
        cls = hdr->slotAt(0);
    }
    return 0;
}

static sqInt proxy_primitiveMethod() {
    return oopToSqInt(gInterp->activeMethod());
}

static sqInt proxy_classExternalAddress() {
    return proxy_nilObject(); // Not needed for B2DPlugin
}

static void* proxy_ioLoadModuleOfLength(sqInt modIndex, sqInt modLength) {
    return nullptr;
}

static void* proxy_ioLoadSymbolOfLengthFromModule(sqInt fnIndex, sqInt fnLength, void* handle) {
    return nullptr;
}

static sqInt proxy_isInMemory(sqInt address) {
    return 1;
}

static void* proxy_ioLoadFunctionFrom(char* fnName, char* modName) {
    // B2DPlugin uses this to load BitBlt functions
    if (fnName && strcmp(fnName, "loadBitBltFrom") == 0) {
        return (void*)proxy_loadBitBltFrom;
    }
    if (fnName && strcmp(fnName, "copyBitsFromtoat") == 0) {
        return (void*)proxy_copyBitsFromtoat;
    }
    return nullptr;
}

static sqInt proxy_ioMicroMSecs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<sqInt>((tv.tv_sec * 1000) + (tv.tv_usec / 1000));
}

static sqInt proxy_positive64BitIntegerFor(usqLong v) {
    return oopToSqInt(Oop::fromSmallInteger(static_cast<int64_t>(v)));
}
static usqLong proxy_positive64BitValueOf(sqInt oop) {
    Oop obj = sqIntToOop(oop);
    if (obj.isSmallInteger()) return static_cast<usqLong>(obj.asSmallInteger());
    gFailed = true;
    return 0;
}
static sqInt proxy_signed64BitIntegerFor(sqLong v) {
    return oopToSqInt(Oop::fromSmallInteger(v));
}
static sqLong proxy_signed64BitValueOf(sqInt oop) {
    Oop obj = sqIntToOop(oop);
    if (obj.isSmallInteger()) return static_cast<sqLong>(obj.asSmallInteger());
    gFailed = true;
    return 0;
}

static sqInt proxy_isArray(sqInt oop) {
    Oop obj = sqIntToOop(oop);
    if (!obj.isObject()) return 0;
    Oop cls = gMem->classOf(obj);
    return (cls.rawBits() == gMem->specialObject(SpecialObjectIndex::ClassArray).rawBits()) ? 1 : 0;
}

static sqInt proxy_forceInterruptCheck() { return 0; }

static sqInt proxy_fetchLong32ofObject(sqInt fieldIndex, sqInt oop) {
    Oop obj = sqIntToOop(oop);
    if (!obj.isObject()) return 0;
    ObjectHeader* hdr = obj.asObjectPtr();
    uint32_t* words = reinterpret_cast<uint32_t*>(hdr->bytes());
    return static_cast<sqInt>(words[fieldIndex]);
}

static sqInt proxy_getThisSessionID() { return 0; }
static sqInt proxy_ioFilenamefromStringofLengthresolveAliases(char* a, char* b, sqInt c, sqInt d) { return 0; }
static sqInt proxy_vmEndianness() { return 0; } // Little-endian

static sqInt proxy_addGCRoot(sqInt* varLoc) { return 1; }
static sqInt proxy_removeGCRoot(sqInt* varLoc) { return 1; }

static sqInt proxy_primitiveFailFor(sqInt code) {
    gFailed = true;
    gFailureCode = code;
    return 0;
}

static sqInt proxy_sendInvokeCallbackStackRegistersJmpbuf(sqInt a, sqInt b, sqInt c, sqInt d) { return 0; }
static sqInt proxy_reestablishContextPriorToCallback(sqInt ctx) { return 0; }
static sqInt proxy_isOopImmutable(sqInt oop) {
    Oop obj = sqIntToOop(oop);
    if (!obj.isObject() || obj.rawBits() < 0x10000) return 0;
    return obj.asObjectPtr()->isImmutable() ? 1 : 0;
}
static sqInt proxy_isOopMutable(sqInt oop) {
    Oop obj = sqIntToOop(oop);
    if (!obj.isObject() || obj.rawBits() < 0x10000) return 1;
    return obj.asObjectPtr()->isImmutable() ? 0 : 1;
}

static sqInt proxy_methodReturnBool(sqInt b) {
    gInterp->popN(gInterp->argumentCount() + 1);
    gInterp->push(b ? gMem->trueObject() : gMem->falseObject());
    return 0;
}
static sqInt proxy_methodReturnFloat(double f) {
    gInterp->popN(gInterp->argumentCount() + 1);
    gInterp->push(boxFloat(f));
    return 0;
}
static sqInt proxy_methodReturnInteger(sqInt v) {
    gInterp->popN(gInterp->argumentCount() + 1);
    gInterp->push(Oop::fromSmallInteger(v));
    return 0;
}
static sqInt proxy_methodReturnString(char* s) {
    sqInt strOop = proxy_stringForCString(s);
    if (gFailed) return 0;
    gInterp->popN(gInterp->argumentCount() + 1);
    gInterp->push(sqIntToOop(strOop));
    return 0;
}
static sqInt proxy_methodReturnValue(sqInt oop) {
    gInterp->popN(gInterp->argumentCount() + 1);
    gInterp->push(sqIntToOop(oop));
    return 0;
}
static sqInt proxy_topRemappableOop() {
    if (gRemappableTop <= 0) return proxy_nilObject();
    return gRemappableOops[gRemappableTop - 1];
}

static void proxy_addHighPriorityTickee(void (*ticker)(void), unsigned periodms) {}
static void proxy_addSynchronousTickee(void (*ticker)(void), unsigned periodms, unsigned roundms) {}
static volatile unsigned long long proxy_utcMicroseconds() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<unsigned long long>(tv.tv_sec) * 1000000ULL + tv.tv_usec;
}
static void proxy_tenuringIncrementalGC() {}
static sqInt proxy_isYoung(sqInt anOop) { return 0; }
static sqInt proxy_isKindOfClass(sqInt oop, sqInt aClass) {
    return proxy_includesBehaviorThatOf(proxy_fetchClassOf(oop), aClass);
}
static sqInt proxy_primitiveErrorTable() { return proxy_nilObject(); }
static sqInt proxy_primitiveFailureCode() { return gFailureCode; }
static sqInt proxy_instanceSizeOf(sqInt aClass) {
    Oop cls = sqIntToOop(aClass);
    if (!cls.isObject()) return 0;
    ObjectHeader* hdr = cls.asObjectPtr();
    if (hdr->slotCount() < 3) return 0;
    Oop fmtOop = hdr->slotAt(2);
    if (fmtOop.isSmallInteger()) {
        return static_cast<sqInt>(fmtOop.asSmallInteger() & 0xFFFF);
    }
    return 0;
}

static sqIntptr_t proxy_signedMachineIntegerValueOf(sqInt oop) {
    return static_cast<sqIntptr_t>(proxy_integerValueOf(oop));
}
static sqIntptr_t proxy_stackSignedMachineIntegerValue(sqInt offset) {
    return static_cast<sqIntptr_t>(proxy_stackIntegerValue(offset));
}
static usqIntptr_t proxy_positiveMachineIntegerValueOf(sqInt oop) {
    return static_cast<usqIntptr_t>(proxy_positive32BitValueOf(oop));
}
static usqIntptr_t proxy_stackPositiveMachineIntegerValue(sqInt offset) {
    return static_cast<usqIntptr_t>(proxy_stackIntegerValue(offset));
}
static char* proxy_cStringOrNullFor(sqInt oop) { return nullptr; }
static sqInt proxy_signalNoResume(sqInt s) { return 0; }

static sqInt proxy_isImmediate(sqInt oop) {
    Oop obj = sqIntToOop(oop);
    return (obj.isSmallInteger() || obj.isSmallFloat() || obj.isCharacter()) ? 1 : 0;
}
static sqInt proxy_characterObjectOf(sqInt charCode) {
    return oopToSqInt(Oop::fromCharacter(static_cast<uint32_t>(charCode)));
}
static sqInt proxy_characterValueOf(sqInt oop) {
    Oop obj = sqIntToOop(oop);
    if (obj.isCharacter()) return static_cast<sqInt>(obj.asCharacter());
    gFailed = true;
    return 0;
}
static sqInt proxy_isCharacterObject(sqInt oop) {
    return sqIntToOop(oop).isCharacter() ? 1 : 0;
}
static sqInt proxy_isCharacterValue(int charCode) {
    return (charCode >= 0 && charCode <= 0x3FFFFFFF) ? 1 : 0;
}
static sqInt proxy_isPinned(sqInt oop) { return 0; }
static sqInt proxy_pinObject(sqInt oop) { return oop; }
static sqInt proxy_unpinObject(sqInt oop) { return oop; }

static sqInt proxy_primitiveFailForOSError(sqLong osErrorCode) {
    gFailed = true;
    return 0;
}
static sqInt proxy_methodReturnReceiver() {
    gInterp->popN(gInterp->argumentCount());
    return 0;
}

static sqInt proxy_isBooleanObject(sqInt oop) {
    Oop obj = sqIntToOop(oop);
    return (obj.rawBits() == gMem->trueObject().rawBits() ||
            obj.rawBits() == gMem->falseObject().rawBits()) ? 1 : 0;
}
static sqInt proxy_isPositiveMachineIntegerObject(sqInt oop) {
    Oop obj = sqIntToOop(oop);
    if (obj.isSmallInteger() && obj.asSmallInteger() >= 0) return 1;
    return 0;
}

static sqInt proxy_ptEnterInterpreterFromCallback(void* p) { return 0; }
static sqInt proxy_ptExitInterpreterToCallback(void* p) { return 0; }
static sqInt proxy_isNonImmediate(sqInt oop) {
    return !proxy_isImmediate(oop);
}
static void* proxy_platformSemaphoreNew(int initialValue) { return nullptr; }
static sqInt proxy_scheduleInMainThread(sqInt (*closure)()) { return 0; }
static void proxy_waitOnExternalSemaphoreIndex(sqInt semaphoreIndex) {}

// =====================================================================
// The proxy struct itself
// =====================================================================
static VirtualMachine theProxy;

void initializeInterpreterProxy(Interpreter* interp) {
    gInterp = interp;
    gMem = &interp->memory();

    theProxy.minorVersion = proxy_minorVersion;
    theProxy.majorVersion = proxy_majorVersion;
    theProxy.pop = proxy_pop;
    theProxy.popthenPush = proxy_popthenPush;
    theProxy.push = proxy_push;
    theProxy.pushBool = proxy_pushBool;
    theProxy.pushFloat = proxy_pushFloat;
    theProxy.pushInteger = proxy_pushInteger;
    theProxy.stackFloatValue = proxy_stackFloatValue;
    theProxy.stackIntegerValue = proxy_stackIntegerValue;
    theProxy.stackObjectValue = proxy_stackObjectValue;
    theProxy.stackValue = proxy_stackValue;
    theProxy.argumentCountOf = proxy_argumentCountOf;
    theProxy.arrayValueOf = proxy_arrayValueOf;
    theProxy.byteSizeOf = proxy_byteSizeOf;
    theProxy.fetchArrayofObject = proxy_fetchArrayofObject;
    theProxy.fetchClassOf = proxy_fetchClassOf;
    theProxy.fetchFloatofObject = proxy_fetchFloatofObject;
    theProxy.fetchIntegerofObject = proxy_fetchIntegerofObject;
    theProxy.fetchPointerofObject = proxy_fetchPointerofObject;
    theProxy.obsoleteDontUseThisFetchWordofObject = proxy_obsoleteDontUseThisFetchWordofObject;
    theProxy.firstFixedField = proxy_firstFixedField;
    theProxy.firstIndexableField = proxy_firstIndexableField;
    theProxy.literalofMethod = proxy_literalofMethod;
    theProxy.literalCountOf = proxy_literalCountOf;
    theProxy.methodArgumentCount = proxy_methodArgumentCount;
    theProxy.methodPrimitiveIndex = proxy_methodPrimitiveIndex;
    theProxy.primitiveIndexOf = proxy_primitiveIndexOf;
    theProxy.sizeOfSTArrayFromCPrimitive = proxy_sizeOfSTArrayFromCPrimitive;
    theProxy.slotSizeOf = proxy_slotSizeOf;
    theProxy.stObjectat = proxy_stObjectat;
    theProxy.stObjectatput = proxy_stObjectatput;
    theProxy.stSizeOf = proxy_stSizeOf;
    theProxy.storeIntegerofObjectwithValue = proxy_storeIntegerofObjectwithValue;
    theProxy.storePointerofObjectwithValue = proxy_storePointerofObjectwithValue;
    theProxy.isKindOf = proxy_isKindOf;
    theProxy.isMemberOf = proxy_isMemberOf;
    theProxy.isBytes = proxy_isBytes;
    theProxy.isFloatObject = proxy_isFloatObject;
    theProxy.isIndexable = proxy_isIndexable;
    theProxy.isIntegerObject = proxy_isIntegerObject;
    theProxy.isIntegerValue = proxy_isIntegerValue;
    theProxy.isPointers = proxy_isPointers;
    theProxy.isWeak = proxy_isWeak;
    theProxy.isWords = proxy_isWords;
    theProxy.isWordsOrBytes = proxy_isWordsOrBytes;
    theProxy.booleanValueOf = proxy_booleanValueOf;
    theProxy.checkedIntegerValueOf = proxy_checkedIntegerValueOf;
    theProxy.floatObjectOf = proxy_floatObjectOf;
    theProxy.floatValueOf = proxy_floatValueOf;
    theProxy.integerObjectOf = proxy_integerObjectOf;
    theProxy.integerValueOf = proxy_integerValueOf;
    theProxy.positive32BitIntegerFor = proxy_positive32BitIntegerFor;
    theProxy.positive32BitValueOf = proxy_positive32BitValueOf;
    theProxy.falseObject = proxy_falseObject;
    theProxy.nilObject = proxy_nilObject;
    theProxy.trueObject = proxy_trueObject;
    theProxy.classArray = proxy_classArray;
    theProxy.classBitmap = proxy_classBitmap;
    theProxy.classByteArray = proxy_classByteArray;
    theProxy.classCharacter = proxy_classCharacter;
    theProxy.classFloat = proxy_classFloat;
    theProxy.classLargePositiveInteger = proxy_classLargePositiveInteger;
    theProxy.classPoint = proxy_classPoint;
    theProxy.classSemaphore = proxy_classSemaphore;
    theProxy.classSmallInteger = proxy_classSmallInteger;
    theProxy.classString = proxy_classString;
    theProxy.clone = proxy_clone;
    theProxy.instantiateClassindexableSize = proxy_instantiateClassindexableSize;
    theProxy.makePointwithxValueyValue = proxy_makePointwithxValueyValue;
    theProxy.popRemappableOop = proxy_popRemappableOop;
    theProxy.pushRemappableOop = proxy_pushRemappableOop;
    theProxy.becomewith = proxy_becomewith;
    theProxy.byteSwapped = proxy_byteSwapped;
    theProxy.failed = proxy_failed;
    theProxy.fullGC = proxy_fullGC;
    theProxy.primitiveFail = proxy_primitiveFail;
    theProxy.showDisplayBitsLeftTopRightBottom = proxy_showDisplayBitsLeftTopRightBottom;
    theProxy.signalSemaphoreWithIndex = proxy_signalSemaphoreWithIndex;
    theProxy.success = proxy_success;
    theProxy.superclassOf = proxy_superclassOf;
    theProxy.statNumGCs = proxy_statNumGCs;
    theProxy.stringForCString = proxy_stringForCString;
    theProxy.loadBitBltFrom = proxy_loadBitBltFrom;
    theProxy.copyBits = proxy_copyBits;
    theProxy.copyBitsFromtoat = proxy_copyBitsFromtoat;
    theProxy.classLargeNegativeInteger = proxy_classLargeNegativeInteger;
    theProxy.signed32BitIntegerFor = proxy_signed32BitIntegerFor;
    theProxy.signed32BitValueOf = proxy_signed32BitValueOf;
    theProxy.includesBehaviorThatOf = proxy_includesBehaviorThatOf;
    theProxy.primitiveMethod = proxy_primitiveMethod;
    theProxy.classExternalAddress = proxy_classExternalAddress;
    theProxy.ioLoadModuleOfLength = proxy_ioLoadModuleOfLength;
    theProxy.ioLoadSymbolOfLengthFromModule = proxy_ioLoadSymbolOfLengthFromModule;
    theProxy.isInMemory = proxy_isInMemory;
    theProxy.ioLoadFunctionFrom = proxy_ioLoadFunctionFrom;
    theProxy.ioMicroMSecs = proxy_ioMicroMSecs;
    theProxy.positive64BitIntegerFor = proxy_positive64BitIntegerFor;
    theProxy.positive64BitValueOf = proxy_positive64BitValueOf;
    theProxy.signed64BitIntegerFor = proxy_signed64BitIntegerFor;
    theProxy.signed64BitValueOf = proxy_signed64BitValueOf;
    theProxy.isArray = proxy_isArray;
    theProxy.forceInterruptCheck = proxy_forceInterruptCheck;
    theProxy.fetchLong32ofObject = proxy_fetchLong32ofObject;
    theProxy.getThisSessionID = proxy_getThisSessionID;
    theProxy.ioFilenamefromStringofLengthresolveAliases = proxy_ioFilenamefromStringofLengthresolveAliases;
    theProxy.vmEndianness = proxy_vmEndianness;
    theProxy.addGCRoot = proxy_addGCRoot;
    theProxy.removeGCRoot = proxy_removeGCRoot;
    theProxy.primitiveFailFor = proxy_primitiveFailFor;
    theProxy.sendInvokeCallbackStackRegistersJmpbuf = proxy_sendInvokeCallbackStackRegistersJmpbuf;
    theProxy.reestablishContextPriorToCallback = proxy_reestablishContextPriorToCallback;
    theProxy.isOopImmutable = proxy_isOopImmutable;
    theProxy.isOopMutable = proxy_isOopMutable;
    theProxy.methodReturnBool = proxy_methodReturnBool;
    theProxy.methodReturnFloat = proxy_methodReturnFloat;
    theProxy.methodReturnInteger = proxy_methodReturnInteger;
    theProxy.methodReturnString = proxy_methodReturnString;
    theProxy.methodReturnValue = proxy_methodReturnValue;
    theProxy.topRemappableOop = proxy_topRemappableOop;
    theProxy.addHighPriorityTickee = proxy_addHighPriorityTickee;
    theProxy.addSynchronousTickee = proxy_addSynchronousTickee;
    theProxy.utcMicroseconds = (volatile unsigned long long (*)(void))proxy_utcMicroseconds;
    theProxy.tenuringIncrementalGC = proxy_tenuringIncrementalGC;
    theProxy.isYoung = proxy_isYoung;
    theProxy.isKindOfClass = proxy_isKindOfClass;
    theProxy.primitiveErrorTable = proxy_primitiveErrorTable;
    theProxy.primitiveFailureCode = proxy_primitiveFailureCode;
    theProxy.instanceSizeOf = proxy_instanceSizeOf;
    theProxy.signedMachineIntegerValueOf = proxy_signedMachineIntegerValueOf;
    theProxy.stackSignedMachineIntegerValue = proxy_stackSignedMachineIntegerValue;
    theProxy.positiveMachineIntegerValueOf = proxy_positiveMachineIntegerValueOf;
    theProxy.stackPositiveMachineIntegerValue = proxy_stackPositiveMachineIntegerValue;
    theProxy.cStringOrNullFor = proxy_cStringOrNullFor;
    theProxy.signalNoResume = proxy_signalNoResume;
    theProxy.isImmediate = proxy_isImmediate;
    theProxy.characterObjectOf = proxy_characterObjectOf;
    theProxy.characterValueOf = proxy_characterValueOf;
    theProxy.isCharacterObject = proxy_isCharacterObject;
    theProxy.isCharacterValue = proxy_isCharacterValue;
    theProxy.isPinned = proxy_isPinned;
    theProxy.pinObject = proxy_pinObject;
    theProxy.unpinObject = proxy_unpinObject;
    theProxy.primitiveFailForOSError = proxy_primitiveFailForOSError;
    theProxy.methodReturnReceiver = proxy_methodReturnReceiver;
    theProxy.isBooleanObject = proxy_isBooleanObject;
    theProxy.isPositiveMachineIntegerObject = proxy_isPositiveMachineIntegerObject;
    theProxy.ptEnterInterpreterFromCallback = proxy_ptEnterInterpreterFromCallback;
    theProxy.ptExitInterpreterToCallback = proxy_ptExitInterpreterToCallback;
    theProxy.isNonImmediate = proxy_isNonImmediate;
    theProxy.platformSemaphoreNew = proxy_platformSemaphoreNew;
    theProxy.scheduleInMainThread = proxy_scheduleInMainThread;
    theProxy.waitOnExternalSemaphoreIndex = proxy_waitOnExternalSemaphoreIndex;
}

VirtualMachine* getInterpreterProxy() {
    return &theProxy;
}

// =====================================================================
// B2DPlugin integration
// =====================================================================
// B2DPlugin exports (symbol-renamed in B2DPlugin.c to avoid collisions)
extern "C" {
    sqInt B2DPlugin_setInterpreter(VirtualMachine* anInterpreter);
    sqInt B2DPlugin_initialiseModule(void);

    // All B2DPlugin primitive functions
    sqInt primitiveAbortProcessing(void);
    sqInt primitiveAddActiveEdgeEntry(void);
    sqInt primitiveAddBezier(void);
    sqInt primitiveAddBezierShape(void);
    sqInt primitiveAddBitmapFill(void);
    sqInt primitiveAddCompressedShape(void);
    sqInt primitiveAddGradientFill(void);
    sqInt primitiveAddLine(void);
    sqInt primitiveAddOval(void);
    sqInt primitiveAddPolygon(void);
    sqInt primitiveAddRect(void);
    sqInt primitiveChangedActiveEdgeEntry(void);
    sqInt primitiveCopyBuffer(void);
    sqInt primitiveDisplaySpanBuffer(void);
    sqInt primitiveDoProfileStats(void);
    sqInt primitiveFinishedProcessing(void);
    sqInt primitiveGetAALevel(void);
    sqInt primitiveGetBezierStats(void);
    sqInt primitiveGetClipRect(void);
    sqInt primitiveGetCounts(void);
    sqInt primitiveGetDepth(void);
    sqInt primitiveGetFailureReason(void);
    sqInt primitiveGetOffset(void);
    sqInt primitiveGetTimes(void);
    sqInt primitiveInitializeBuffer(void);
    sqInt primitiveInitializeProcessing(void);
    sqInt primitiveMergeFillFrom(void);
    sqInt primitiveNeedsFlush(void);
    sqInt primitiveNeedsFlushPut(void);
    sqInt primitiveNextActiveEdgeEntry(void);
    sqInt primitiveNextFillEntry(void);
    sqInt primitiveNextGlobalEdgeEntry(void);
    sqInt primitiveRegisterExternalEdge(void);
    sqInt primitiveRegisterExternalFill(void);
    sqInt primitiveRenderImage(void);
    sqInt primitiveRenderScanline(void);
    sqInt primitiveSetAALevel(void);
    sqInt primitiveSetBitBltPlugin(void);
    sqInt primitiveSetClipRect(void);
    sqInt primitiveSetColorTransform(void);
    sqInt primitiveSetDepth(void);
    sqInt primitiveSetEdgeTransform(void);
    sqInt primitiveSetOffset(void);
}

// Reset interpreter proxy for VM relaunch
void resetInterpreterProxy() {
    gInterp = nullptr;
    gMem = nullptr;
    gFailed = false;
    gFailureCode = 0;
    gRemappableTop = 0;
}

// Helper to reset the failure state before each primitive call
void resetProxyFailure() {
    gFailed = false;
    gFailureCode = 0;
}

bool proxyFailed() {
    return gFailed;
}

// Wrapper type for B2DPlugin primitives called from our Interpreter
typedef sqInt (*B2DPrimFn)(void);

void initializeB2DPlugin(Interpreter* interp) {
    // Set up the interpreter proxy
    initializeInterpreterProxy(interp);

    // Initialize B2DPlugin
    B2DPlugin_setInterpreter(&theProxy);
    B2DPlugin_initialiseModule();

    // Register all B2DPlugin primitives as named primitives under "B2DPlugin"
    struct { const char* name; B2DPrimFn fn; } prims[] = {
        {"primitiveAbortProcessing", primitiveAbortProcessing},
        {"primitiveAddActiveEdgeEntry", primitiveAddActiveEdgeEntry},
        {"primitiveAddBezier", primitiveAddBezier},
        {"primitiveAddBezierShape", primitiveAddBezierShape},
        {"primitiveAddBitmapFill", primitiveAddBitmapFill},
        {"primitiveAddCompressedShape", primitiveAddCompressedShape},
        {"primitiveAddGradientFill", primitiveAddGradientFill},
        {"primitiveAddLine", primitiveAddLine},
        {"primitiveAddOval", primitiveAddOval},
        {"primitiveAddPolygon", primitiveAddPolygon},
        {"primitiveAddRect", primitiveAddRect},
        {"primitiveChangedActiveEdgeEntry", primitiveChangedActiveEdgeEntry},
        {"primitiveCopyBuffer", primitiveCopyBuffer},
        {"primitiveDisplaySpanBuffer", primitiveDisplaySpanBuffer},
        {"primitiveDoProfileStats", primitiveDoProfileStats},
        {"primitiveFinishedProcessing", primitiveFinishedProcessing},
        {"primitiveGetAALevel", primitiveGetAALevel},
        {"primitiveGetBezierStats", primitiveGetBezierStats},
        {"primitiveGetClipRect", primitiveGetClipRect},
        {"primitiveGetCounts", primitiveGetCounts},
        {"primitiveGetDepth", primitiveGetDepth},
        {"primitiveGetFailureReason", primitiveGetFailureReason},
        {"primitiveGetOffset", primitiveGetOffset},
        {"primitiveGetTimes", primitiveGetTimes},
        {"primitiveInitializeBuffer", primitiveInitializeBuffer},
        {"primitiveInitializeProcessing", primitiveInitializeProcessing},
        {"primitiveMergeFillFrom", primitiveMergeFillFrom},
        {"primitiveNeedsFlush", primitiveNeedsFlush},
        {"primitiveNeedsFlushPut", primitiveNeedsFlushPut},
        {"primitiveNextActiveEdgeEntry", primitiveNextActiveEdgeEntry},
        {"primitiveNextFillEntry", primitiveNextFillEntry},
        {"primitiveNextGlobalEdgeEntry", primitiveNextGlobalEdgeEntry},
        {"primitiveRegisterExternalEdge", primitiveRegisterExternalEdge},
        {"primitiveRegisterExternalFill", primitiveRegisterExternalFill},
        {"primitiveRenderImage", primitiveRenderImage},
        {"primitiveRenderScanline", primitiveRenderScanline},
        {"primitiveSetAALevel", primitiveSetAALevel},
        {"primitiveSetBitBltPlugin", primitiveSetBitBltPlugin},
        {"primitiveSetClipRect", primitiveSetClipRect},
        {"primitiveSetColorTransform", primitiveSetColorTransform},
        {"primitiveSetDepth", primitiveSetDepth},
        {"primitiveSetEdgeTransform", primitiveSetEdgeTransform},
        {"primitiveSetOffset", primitiveSetOffset},
    };

    for (auto& p : prims) {
        interp->registerNamedPrimitive("B2DPlugin", p.name, reinterpret_cast<Interpreter::ExternalPrimFunc>(p.fn));
    }

}

// =====================================================================
// DSAPrims integration (CRYPTO — guarded by PHARO_WITH_CRYPTO)
// =====================================================================
#if PHARO_WITH_CRYPTO
extern "C" {
    sqInt DSAPrims_setInterpreter(VirtualMachine* anInterpreter);
    sqInt primitiveBigDivide(void);
    sqInt primitiveBigMultiply(void);
    sqInt primitiveExpandBlock(void);
    sqInt primitiveHashBlock(void);
    sqInt primitiveHasSecureHashPrimitive(void);
    sqInt primitiveHighestNonZeroDigitIndex(void);
}

void initializeDSAPrims(Interpreter* interp) {
    DSAPrims_setInterpreter(&theProxy);

    using PrimFn = sqInt (*)(void);
    struct { const char* name; PrimFn fn; } prims[] = {
        {"primitiveBigDivide", primitiveBigDivide},
        {"primitiveBigMultiply", primitiveBigMultiply},
        {"primitiveExpandBlock", primitiveExpandBlock},
        {"primitiveHashBlock", primitiveHashBlock},
        {"primitiveHasSecureHashPrimitive", primitiveHasSecureHashPrimitive},
        {"primitiveHighestNonZeroDigitIndex", primitiveHighestNonZeroDigitIndex},
    };

    for (auto& p : prims) {
        interp->registerNamedPrimitive("DSAPrims", p.name, reinterpret_cast<Interpreter::ExternalPrimFunc>(p.fn));
    }

}
#endif // PHARO_WITH_CRYPTO

// =====================================================================
// JPEGReaderPlugin integration
// =====================================================================
extern "C" {
    sqInt JPEGReaderPlugin_setInterpreter(VirtualMachine* anInterpreter);
    sqInt primitiveColorConvertGrayscaleMCU(void);
    sqInt primitiveColorConvertMCU(void);
    sqInt primitiveDecodeMCU(void);
    sqInt primitiveIdctInt(void);
}

void initializeJPEGReaderPlugin(Interpreter* interp) {
    JPEGReaderPlugin_setInterpreter(&theProxy);

    using PrimFn = sqInt (*)(void);
    struct { const char* name; PrimFn fn; } prims[] = {
        {"primitiveColorConvertGrayscaleMCU", primitiveColorConvertGrayscaleMCU},
        {"primitiveColorConvertMCU", primitiveColorConvertMCU},
        {"primitiveDecodeMCU", primitiveDecodeMCU},
        {"primitiveIdctInt", primitiveIdctInt},
    };

    for (auto& p : prims) {
        interp->registerNamedPrimitive("JPEGReaderPlugin", p.name, reinterpret_cast<Interpreter::ExternalPrimFunc>(p.fn));
    }

}

// =====================================================================
// JPEGReadWriter2Plugin integration
// =====================================================================
extern "C" {
    sqInt JPEGReadWriter2Plugin_setInterpreter(VirtualMachine* anInterpreter);
    sqInt primImageHeight(void);
    sqInt primImageNumComponents(void);
    sqInt primImageWidth(void);
    sqInt primJPEGCompressStructSize(void);
    sqInt primJPEGDecompressStructSize(void);
    sqInt primJPEGErrorMgr2StructSize(void);
    sqInt primJPEGPluginIsPresent(void);
    sqInt primJPEGReadHeaderfromByteArrayerrorMgr(void);
    sqInt primJPEGReadImagefromByteArrayonFormdoDitheringerrorMgr(void);
    sqInt primJPEGWriteImageonByteArrayformqualityprogressiveJPEGerrorMgr(void);
    sqInt primSupports8BitGrayscaleJPEGs(void);
}

void initializeJPEGReadWriter2Plugin(Interpreter* interp) {
    JPEGReadWriter2Plugin_setInterpreter(&theProxy);

    using PrimFn = sqInt (*)(void);
    struct { const char* name; PrimFn fn; } prims[] = {
        {"primImageHeight", primImageHeight},
        {"primImageNumComponents", primImageNumComponents},
        {"primImageWidth", primImageWidth},
        {"primJPEGCompressStructSize", primJPEGCompressStructSize},
        {"primJPEGDecompressStructSize", primJPEGDecompressStructSize},
        {"primJPEGErrorMgr2StructSize", primJPEGErrorMgr2StructSize},
        {"primJPEGPluginIsPresent", primJPEGPluginIsPresent},
        {"primJPEGReadHeaderfromByteArrayerrorMgr", primJPEGReadHeaderfromByteArrayerrorMgr},
        {"primJPEGReadImagefromByteArrayonFormdoDitheringerrorMgr", primJPEGReadImagefromByteArrayonFormdoDitheringerrorMgr},
        {"primJPEGWriteImageonByteArrayformqualityprogressiveJPEGerrorMgr", primJPEGWriteImageonByteArrayformqualityprogressiveJPEGerrorMgr},
        {"primSupports8BitGrayscaleJPEGs", primSupports8BitGrayscaleJPEGs},
    };

    for (auto& p : prims) {
        interp->registerNamedPrimitive("JPEGReadWriter2Plugin", p.name, reinterpret_cast<Interpreter::ExternalPrimFunc>(p.fn));
    }

}

// =====================================================================
// SqueakSSL integration (CRYPTO — guarded by PHARO_WITH_CRYPTO)
// =====================================================================
#if PHARO_WITH_CRYPTO
extern "C" {
    sqInt SqueakSSL_setInterpreter(VirtualMachine* anInterpreter);
    sqInt primitiveAccept(void);
    sqInt primitiveConnect(void);
    sqInt primitiveCreate(void);
    sqInt primitiveDecrypt(void);
    sqInt primitiveDestroy(void);
    sqInt primitiveEncrypt(void);
    sqInt primitiveGetIntProperty(void);
    sqInt primitiveGetStringProperty(void);
    sqInt primitiveSetIntProperty(void);
    sqInt primitiveSetStringProperty(void);
}

void initializeSqueakSSL(Interpreter* interp) {
    SqueakSSL_setInterpreter(&theProxy);

    using PrimFn = sqInt (*)(void);
    struct { const char* name; PrimFn fn; } prims[] = {
        {"primitiveAccept", primitiveAccept},
        {"primitiveConnect", primitiveConnect},
        {"primitiveCreate", primitiveCreate},
        {"primitiveDecrypt", primitiveDecrypt},
        {"primitiveDestroy", primitiveDestroy},
        {"primitiveEncrypt", primitiveEncrypt},
        {"primitiveGetIntProperty", primitiveGetIntProperty},
        {"primitiveGetStringProperty", primitiveGetStringProperty},
        {"primitiveSetIntProperty", primitiveSetIntProperty},
        {"primitiveSetStringProperty", primitiveSetStringProperty},
    };

    for (auto& p : prims) {
        interp->registerNamedPrimitive("SqueakSSL", p.name, reinterpret_cast<Interpreter::ExternalPrimFunc>(p.fn));
    }

}
#endif // PHARO_WITH_CRYPTO

// =====================================================================
// SocketPlugin integration (TCP sockets)
// =====================================================================
extern "C" {
    sqInt SocketPlugin_setInterpreter(VirtualMachine* anInterpreter);
    sqInt sp_primitiveSocketCreate3Semaphores(void);
    sqInt sp_primitiveSocketDestroy(void);
    sqInt sp_primitiveSocketConnectToPort(void);
    sqInt sp_primitiveSocketConnectionStatus(void);
    sqInt sp_primitiveSocketCloseConnection(void);
    sqInt sp_primitiveSocketAbortConnection(void);
    sqInt sp_primitiveSocketSendDataBufCount(void);
    sqInt sp_primitiveSocketSendDone(void);
    sqInt sp_primitiveSocketReceiveDataAvailable(void);
    sqInt sp_primitiveSocketReceiveDataBufCount(void);
    sqInt sp_primitiveSocketLocalPort(void);
    sqInt sp_primitiveSocketLocalAddress(void);
    sqInt sp_primitiveSocketRemoteAddress(void);
    sqInt sp_primitiveSocketRemotePort(void);
    sqInt sp_primitiveSocketError(void);
    sqInt sp_primitiveSocketGetOptions(void);
    sqInt sp_primitiveSocketSetOptions(void);
    sqInt sp_primitiveSocketListenOnPortBacklog(void);
    sqInt sp_primitiveSocketListenOnPortBacklogInterface(void);
    sqInt sp_primitiveSocketAccept3Semaphores(void);
    sqInt sp_primitiveHasSocketAccess(void);
    sqInt sp_primitiveSocketSendUDPDataBufCount(void);
    sqInt sp_primitiveSocketReceiveUDPDataBufCount(void);
}

void initializeSocketPlugin(Interpreter* interp) {
    SocketPlugin_setInterpreter(&theProxy);

    using PrimFn = sqInt (*)(void);
    struct { const char* name; PrimFn fn; } prims[] = {
        {"primitiveSocketCreate3Semaphores", sp_primitiveSocketCreate3Semaphores},
        {"primitiveSocketDestroy", sp_primitiveSocketDestroy},
        {"primitiveSocketConnectToPort", sp_primitiveSocketConnectToPort},
        {"primitiveSocketConnectionStatus", sp_primitiveSocketConnectionStatus},
        {"primitiveSocketCloseConnection", sp_primitiveSocketCloseConnection},
        {"primitiveSocketAbortConnection", sp_primitiveSocketAbortConnection},
        {"primitiveSocketSendDataBufCount", sp_primitiveSocketSendDataBufCount},
        {"primitiveSocketSendDone", sp_primitiveSocketSendDone},
        {"primitiveSocketReceiveDataAvailable", sp_primitiveSocketReceiveDataAvailable},
        {"primitiveSocketReceiveDataBufCount", sp_primitiveSocketReceiveDataBufCount},
        {"primitiveSocketLocalPort", sp_primitiveSocketLocalPort},
        {"primitiveSocketLocalAddress", sp_primitiveSocketLocalAddress},
        {"primitiveSocketRemoteAddress", sp_primitiveSocketRemoteAddress},
        {"primitiveSocketRemotePort", sp_primitiveSocketRemotePort},
        {"primitiveSocketError", sp_primitiveSocketError},
        {"primitiveSocketGetOptions", sp_primitiveSocketGetOptions},
        {"primitiveSocketSetOptions", sp_primitiveSocketSetOptions},
        {"primitiveSocketListenOnPortBacklog", sp_primitiveSocketListenOnPortBacklog},
        {"primitiveSocketListenOnPortBacklogInterface", sp_primitiveSocketListenOnPortBacklogInterface},
        {"primitiveSocketAccept3Semaphores", sp_primitiveSocketAccept3Semaphores},
        {"primitiveHasSocketAccess", sp_primitiveHasSocketAccess},
        {"primitiveSocketSendUDPDataBufCount", sp_primitiveSocketSendUDPDataBufCount},
        {"primitiveSocketReceiveUDPDataBufCount", sp_primitiveSocketReceiveUDPDataBufCount},
    };

    for (auto& p : prims) {
        interp->registerNamedPrimitive("SocketPlugin", p.name, reinterpret_cast<Interpreter::ExternalPrimFunc>(p.fn));
    }
}
