# C++ Oop Wrapper Design for iOS VM

## Overview

This document sketches a `CppCodeGenerator` subclass that generates C++ code with an `Oop` wrapper class, eliminating address-range assumptions that break under iOS ASLR.

## Part 1: The C++ Oop Class

```cpp
// oop.hpp - Oop wrapper class for iOS Pharo VM

#pragma once
#include <cstdint>
#include <cassert>

// Memory space encoding (stored in bits 1-2 of the oop)
enum class OopSpace : uint8_t {
    NewSpace  = 0,  // Young generation
    OldSpace  = 1,  // Tenured objects
    PermSpace = 2,  // Permanent objects (classes, etc.)
    Special   = 3   // Reserved
};

class Oop {
private:
    uint64_t bits_;

    // Bit layout:
    //   Bit 0:     isImmediate (1 = SmallInteger/Character, 0 = pointer)
    //   Bits 1-2:  space encoding (only valid when bit 0 = 0)
    //   Bits 3-63: payload (pointer with low 3 bits masked, or immediate value)

    static constexpr uint64_t kImmediateMask = 0x1ULL;
    static constexpr uint64_t kSpaceMask     = 0x6ULL;  // bits 1-2
    static constexpr uint64_t kSpaceShift    = 1;
    static constexpr uint64_t kPointerMask   = ~0x7ULL; // clear low 3 bits

public:
    // Constructors
    Oop() : bits_(0) {}
    explicit Oop(uint64_t raw) : bits_(raw) {}

    // Raw access (for compatibility during transition)
    uint64_t bits() const { return bits_; }
    static Oop fromBits(uint64_t b) { return Oop(b); }

    // Immediate (SmallInteger) support
    bool isImmediate() const { return bits_ & kImmediateMask; }
    bool isNonImmediate() const { return !isImmediate(); }

    // SmallInteger encoding: value << 3 | 1
    // (Using 3-bit tag for alignment, bit 0 = 1 means immediate)
    static Oop fromSmallInteger(int64_t value) {
        return Oop((static_cast<uint64_t>(value) << 3) | 1);
    }

    int64_t toSmallInteger() const {
        assert(isImmediate());
        return static_cast<int64_t>(bits_) >> 3;
    }

    bool isSmallInteger() const { return isImmediate(); }  // Simplified; real VM has more immediate types

    // Space queries (only valid for non-immediates)
    OopSpace space() const {
        assert(isNonImmediate());
        return static_cast<OopSpace>((bits_ & kSpaceMask) >> kSpaceShift);
    }

    bool isNewSpace() const { return isNonImmediate() && space() == OopSpace::NewSpace; }
    bool isOldSpace() const { return isNonImmediate() && space() == OopSpace::OldSpace; }
    bool isPermSpace() const { return isNonImmediate() && space() == OopSpace::PermSpace; }

    // Aliases for VM compatibility
    bool isYoung() const { return isNewSpace(); }
    bool isOld() const { return isOldSpace(); }
    bool isPermanent() const { return isPermSpace(); }

    // Pointer access (clears tag bits to get actual address)
    void* rawPointer() const {
        assert(isNonImmediate());
        return reinterpret_cast<void*>(bits_ & kPointerMask);
    }

    template<typename T>
    T* as() const { return static_cast<T*>(rawPointer()); }

    // Create Oop from pointer + space
    static Oop fromPointer(void* ptr, OopSpace space) {
        uint64_t addr = reinterpret_cast<uint64_t>(ptr);
        assert((addr & 0x7) == 0);  // Must be 8-byte aligned
        return Oop(addr | (static_cast<uint64_t>(space) << kSpaceShift));
    }

    // Transition helpers: convert old-style oop with address-based space
    static Oop fromLegacyOop(uint64_t legacyOop, OopSpace space) {
        if (legacyOop & kImmediateMask) {
            return Oop(legacyOop);  // Immediates unchanged
        }
        // Re-encode pointer with explicit space bits
        void* ptr = reinterpret_cast<void*>(legacyOop & kPointerMask);
        return fromPointer(ptr, space);
    }

    // Comparison operators
    bool operator==(const Oop& other) const { return bits_ == other.bits_; }
    bool operator!=(const Oop& other) const { return bits_ != other.bits_; }
    bool operator<(const Oop& other) const { return bits_ < other.bits_; }

    // Nil check (nil is typically encoded as Oop(0) or a specific sentinel)
    bool isNil() const { return bits_ == 0; }  // Adjust based on actual nil encoding

    // For debugging
    void dump() const;
};

// Ensure Oop is same size as raw pointer
static_assert(sizeof(Oop) == sizeof(uint64_t), "Oop must be 8 bytes");
```

## Part 2: CppCodeGenerator Smalltalk Class

```smalltalk
"
CppCodeGenerator - Generates C++ with Oop wrapper class for iOS

This subclass of CCodeGenerator overrides type declarations and
translation methods to emit C++ code using the Oop class instead
of raw sqInt values.
"
Class {
    #name : 'CppCodeGenerator',
    #superclass : 'CCodeGenerator',
    #category : 'Slang-iOS'
}

{ #category : 'C++ code generation' }
CppCodeGenerator >> emitHeaderOn: aStream [
    "Emit C++ header with Oop class include"

    aStream
        nextPutAll: '// Generated C++ for iOS Pharo VM'; cr;
        nextPutAll: '#include "oop.hpp"'; cr;
        cr.
    super emitHeaderOn: aStream
]

{ #category : 'type mapping' }
CppCodeGenerator >> cTypeForOop [
    "Return the C++ type for oop values"
    ^ 'Oop'
]

{ #category : 'type mapping' }
CppCodeGenerator >> declarationForVariable: varName type: typeName [
    "Override to use Oop for oop-typed variables"

    (self isOopType: typeName) ifTrue: [
        ^ 'Oop ', varName
    ].
    ^ super declarationForVariable: varName type: typeName
]

{ #category : 'type mapping' }
CppCodeGenerator >> isOopType: aTypeName [
    "Test if a type represents an oop"

    ^ #('sqInt' 'usqInt' 'oop') includes: aTypeName asString
]
```

## Part 3: Translation Method Overrides

```smalltalk
"=== Immediate/SmallInteger checks ==="

{ #category : 'C translation' }
CppCodeGenerator >> generateCASTIsIntegerObject: tast [
    "Generate: oop.isSmallInteger() instead of ((oop) & 1)"

    | argNode |
    argNode := self generate: tast arguments first.
    ^ CMethodCallNode
        receiver: argNode
        selector: 'isSmallInteger'
]

{ #category : 'C translation' }
CppCodeGenerator >> generateCASTIntegerValueOf: tast [
    "Generate: oop.toSmallInteger() instead of ((oop) >> 3)"

    | argNode |
    argNode := self generate: tast arguments first.
    ^ CMethodCallNode
        receiver: argNode
        selector: 'toSmallInteger'
]

{ #category : 'C translation' }
CppCodeGenerator >> generateCASTIntegerObjectOf: tast [
    "Generate: Oop::fromSmallInteger(value) instead of ((value << 3) | 1)"

    | argNode |
    argNode := self generate: tast arguments first.
    ^ CStaticMethodCallNode
        class: 'Oop'
        selector: 'fromSmallInteger'
        argument: argNode
]

"=== Space checks - THE KEY CHANGES ==="

{ #category : 'C translation' }
CppCodeGenerator >> generateCASTIsYoungObject: tast [
    "Generate: oop.isYoung() instead of ((oop & mask) == newSpaceMask)"

    | argNode |
    argNode := self generate: tast arguments first.
    ^ CMethodCallNode
        receiver: argNode
        selector: 'isYoung'
]

{ #category : 'C translation' }
CppCodeGenerator >> generateCASTIsOldObject: tast [
    "Generate: oop.isOld() instead of ((oop & mask) == oldSpaceMask)"

    | argNode |
    argNode := self generate: tast arguments first.
    ^ CMethodCallNode
        receiver: argNode
        selector: 'isOld'
]

{ #category : 'C translation' }
CppCodeGenerator >> generateCASTIsPermanentObject: tast [
    "Generate: oop.isPermanent() instead of (oop >= permSpaceStart)"

    | argNode |
    argNode := self generate: tast arguments first.
    ^ CMethodCallNode
        receiver: argNode
        selector: 'isPermanent'
]

{ #category : 'C translation' }
CppCodeGenerator >> generateCASTIsNonImmediate: tast [
    "Generate: oop.isNonImmediate()"

    | argNode |
    argNode := self generate: tast arguments first.
    ^ CMethodCallNode
        receiver: argNode
        selector: 'isNonImmediate'
]

"=== Pointer access ==="

{ #category : 'C translation' }
CppCodeGenerator >> generateCASTRawPointerOf: tast [
    "Generate: oop.rawPointer() to extract actual memory address"

    | argNode |
    argNode := self generate: tast arguments first.
    ^ CMethodCallNode
        receiver: argNode
        selector: 'rawPointer'
]

"=== Object allocation - encode space at creation time ==="

{ #category : 'C translation' }
CppCodeGenerator >> generateOopFromPointerInSpace: pointerNode space: spaceSymbol [
    "Generate: Oop::fromPointer(ptr, OopSpace::NewSpace)"

    | spaceEnum |
    spaceEnum := Dictionary new
        at: #newSpace put: 'OopSpace::NewSpace';
        at: #oldSpace put: 'OopSpace::OldSpace';
        at: #permSpace put: 'OopSpace::PermSpace';
        at: spaceSymbol.

    ^ CStaticMethodCallNode
        class: 'Oop'
        selector: 'fromPointer'
        arguments: { pointerNode. CIdentifierNode name: spaceEnum }
]
```

## Part 4: Register Translation Methods

```smalltalk
{ #category : 'initialization' }
CppCodeGenerator >> initializeCASTTranslationDictionary [
    "Add iOS-specific translations to the dictionary"

    super initializeCASTTranslationDictionary.

    "Override space-checking translations"
    castTranslationDict
        at: #isYoungObject: put: #generateCASTIsYoungObject:;
        at: #isOldObject: put: #generateCASTIsOldObject:;
        at: #isPermanentObject: put: #generateCASTIsPermanentObject:;
        at: #isNonImmediate: put: #generateCASTIsNonImmediate:;
        at: #isImmediate: put: #generateCASTIsImmediate:.
]
```

## Part 5: Image Loading - Space Assignment

The critical change is during image loading. When swizzling pointers after load:

```smalltalk
"In SpurSegmentManager>>swizzleObj: (conceptually)"

swizzleObjForIOS: objOop fromSegment: segment [
    "Convert legacy oop to new format with explicit space encoding"

    | actualAddress space |

    "Get the relocated address"
    actualAddress := objOop + segment swizzle.

    "Determine space based on where it lands"
    space := self spaceForAddress: actualAddress.

    "Return new Oop with space encoded in low bits"
    ^ Oop fromPointer: actualAddress space: space
]

spaceForAddress: addr [
    "Determine which space an address belongs to"

    (addr >= memoryMap newSpaceStart and: [addr < memoryMap newSpaceEnd])
        ifTrue: [ ^ #newSpace ].
    (addr >= memoryMap oldSpaceStart and: [addr < memoryMap oldSpaceEnd])
        ifTrue: [ ^ #oldSpace ].
    (addr >= memoryMap permSpaceStart and: [addr < memoryMap permSpaceEnd])
        ifTrue: [ ^ #permSpace ].

    self error: 'Address not in any space'
]
```

## Part 6: Files to Create/Modify

### New Files (in iospharo fork)
```
smalltalksrc/
  Slang-iOS/
    CppCodeGenerator.class.st      # The subclass
    CppASTNodes.class.st           # C++ specific AST nodes (method calls, etc.)

src/ios/
    oop.hpp                        # The Oop C++ class
```

### Modified Files
```
smalltalksrc/VMMaker/
    SpurMemoryManager.class.st     # Add #isYoungObject: etc. as explicit sends
    VMMemoryMap.class.st           # Space checking delegated to Oop class
    SpurSegmentManager.class.st    # swizzleObj: encodes space in new format
```

## Part 7: Build Integration

```cmake
# In CMakeLists.txt for iOS build

if(IOS_BUILD)
    # Use C++ compiler
    set(CMAKE_CXX_STANDARD 17)

    # Generate with CppCodeGenerator instead of CCodeGenerator
    set(SLANG_GENERATOR "CppCodeGenerator")

    # Rename output to .cpp
    set(INTERP_SOURCE "cointerp.cpp")
endif()
```

## Migration Strategy

1. **Phase 1**: Create CppCodeGenerator, get it generating valid C++
2. **Phase 2**: Add Oop class, wire up basic operations (isImmediate, etc.)
3. **Phase 3**: Add space encoding, modify image loader to encode spaces
4. **Phase 4**: Test with simple images, fix issues
5. **Phase 5**: Full image loading with ASLR addresses

## Open Questions

1. **Character immediates**: Spur encodes Characters as immediates too. Need to handle in Oop class.
2. **SmallFloat**: 64-bit Spur has SmallFloat immediates. More tag bits needed?
3. **Hash in header**: Object hash is stored in header. Does this interact with space encoding?
4. **Performance**: Method call overhead vs inline bit ops? Should use `__attribute__((always_inline))`?
