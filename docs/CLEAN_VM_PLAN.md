# Clean C++ VM for iOS Pharo - Implementation Plan

## Goal

Build a minimal, type-safe C++ VM that can load and run Pharo Smalltalk images on iOS, designed from the ground up to handle iOS ASLR constraints.

## Rationale

The existing VMMaker-generated C code (~95,000 lines) treats pointers and integers interchangeably, which:
1. Breaks under iOS ASLR (Address Space Layout Randomization)
2. Cannot be cleanly compiled as C++ (hundreds of type errors)
3. Uses high bits of pointers for space encoding (iOS uses those bits)

A clean rewrite is more maintainable than retrofitting type safety onto legacy code.

## Key Insight

Pharo/Squeak architecture separates:
- **VM runtime** (bytecode interpreter, memory manager, primitives) - what we're building
- **Smalltalk image** (language, compiler, libraries) - unchanged, we load existing images

Many primitives can FAIL and fall back to Smalltalk code in the image. We only need native implementations for things that can't be done in Smalltalk.

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│                    iospharo-cpp-vm                          │
├─────────────────────────────────────────────────────────────┤
│  Oop (type-safe object pointer)                             │
│  ├─ Immediate: SmallInteger (tag 001), Character (tag 011) │
│  ├─ Immediate: SmallFloat (tag 101)                         │
│  └─ Object pointer: aligned address + space bits (bits 1-2) │
├─────────────────────────────────────────────────────────────┤
│  ObjectMemory                                               │
│  ├─ Spur 64-bit object headers                              │
│  ├─ Old space, new space, perm space                        │
│  ├─ Class table (22-bit index → class object)               │
│  ├─ Simple mark-sweep GC (upgrade later)                    │
│  └─ Object allocation                                       │
├─────────────────────────────────────────────────────────────┤
│  Interpreter                                                │
│  ├─ Bytecode dispatch (Sista V1, switch/case)               │
│  ├─ Method cache (selector + class → method)                │
│  ├─ Stack frames (hybrid: stack + heap contexts)            │
│  └─ Process/scheduler support                               │
├─────────────────────────────────────────────────────────────┤
│  Primitives (~30 essential, ~200 total)                     │
│  ├─ Arithmetic: +, -, *, /, =, <, bitAnd:, etc.            │
│  ├─ Object access: at:, at:put:, size, instVarAt:          │
│  ├─ Allocation: new, new:, become:                          │
│  ├─ Control: value, value:, perform:                        │
│  └─ I/O: display, input, files                              │
├─────────────────────────────────────────────────────────────┤
│  ImageLoader                                                │
│  ├─ Parse Spur 64-bit image format (format 68021/68533)     │
│  ├─ Deserialize objects                                     │
│  └─ Relocate object pointers                                │
├─────────────────────────────────────────────────────────────┤
│  iOS Platform Layer                                         │
│  ├─ Display (Metal/UIKit)                                   │
│  ├─ Input (touch → mouse events)                            │
│  └─ File system, timers                                     │
└─────────────────────────────────────────────────────────────┘
```

---

## Phase 1: Core Types and Object Memory

### 1.1 Oop Class (src/vm/Oop.hpp)

Type-safe object pointer that encodes immediates and object references.

```cpp
// Tagging scheme (iOS ASLR compatible - uses LOW bits only)
// Bit 0 = 1: Immediate value
//   Bits 2-1 = 00: SmallInteger (tag 001), value in bits 63-3
//   Bits 2-1 = 01: Character (tag 011), codepoint in bits 31-3
//   Bits 2-1 = 10: SmallFloat (tag 101), float bits in 63-3
// Bit 0 = 0: Object pointer
//   Bits 2-1: Space encoding (00=old, 01=new, 10=perm, 11=reserved)
//   Bits 63-3: Aligned object address

class Oop {
    uint64_t bits_;
public:
    // Predicates
    bool isImmediate() const { return bits_ & 1; }
    bool isSmallInteger() const { return (bits_ & 7) == 1; }
    bool isCharacter() const { return (bits_ & 7) == 3; }
    bool isSmallFloat() const { return (bits_ & 7) == 5; }
    bool isObject() const { return !(bits_ & 1); }

    // Accessors
    int64_t asSmallInteger() const;
    uint32_t asCharacter() const;
    double asSmallFloat() const;
    ObjectHeader* asObject() const;

    // Constructors
    static Oop fromSmallInteger(int64_t value);
    static Oop fromCharacter(uint32_t codepoint);
    static Oop fromSmallFloat(double value);
    static Oop fromObject(ObjectHeader* obj, Space space);

    // Comparison (for use in containers)
    bool operator==(Oop other) const { return bits_ == other.bits_; }

    // NO arithmetic operators - prevents accidental pointer math
};
```

### 1.2 Object Header (src/vm/ObjectHeader.hpp)

Spur 64-bit single-word header format:

```cpp
// 64-bit object header layout:
// Bits 0-7:   Number of slots (255 = overflow, check extended header)
// Bits 8-29:  Identity hash (22 bits)
// Bits 30-34: Object format (5 bits)
// Bits 35-56: Class index (22 bits)
// Bits 57-63: Flags (immutable, pinned, remembered, marked, etc.)

struct ObjectHeader {
    uint64_t header_;

    // Slot count
    size_t slotCount() const;
    bool hasOverflowSlots() const { return (header_ & 0xFF) == 255; }

    // Identity hash
    uint32_t identityHash() const { return (header_ >> 8) & 0x3FFFFF; }
    void setIdentityHash(uint32_t hash);

    // Object format
    enum class Format : uint8_t {
        ZeroSized = 0,
        FixedSized = 1,
        Variable = 2,
        VariableWithFixed = 3,
        Weak = 4,
        // ... etc
        CompiledMethod = 24,
    };
    Format format() const { return static_cast<Format>((header_ >> 30) & 0x1F); }

    // Class index (NOT the class object - index into class table)
    uint32_t classIndex() const { return (header_ >> 35) & 0x3FFFFF; }
    void setClassIndex(uint32_t index);

    // Flags
    bool isImmutable() const;
    bool isPinned() const;
    bool isRemembered() const;
    bool isMarked() const;

    // Slot access
    Oop* slots() { return reinterpret_cast<Oop*>(this + 1); }
    Oop slotAt(size_t index) const;
    void slotAtPut(size_t index, Oop value);
};
```

### 1.3 ObjectMemory (src/vm/ObjectMemory.hpp)

```cpp
class ObjectMemory {
    // Memory spaces
    uint8_t* oldSpaceStart_;
    uint8_t* oldSpaceEnd_;
    uint8_t* newSpaceStart_;
    uint8_t* newSpaceEnd_;
    uint8_t* permSpaceStart_;
    uint8_t* permSpaceEnd_;

    // Free pointer for allocation
    uint8_t* freePointer_;

    // Class table: classIndex → class Oop
    std::vector<Oop> classTable_;

    // Special objects (nil, true, false, etc.)
    Oop nilObject_;
    Oop trueObject_;
    Oop falseObject_;
    Oop specialObjectsArray_;

public:
    // Initialization
    void initialize(size_t heapSize);
    void loadImage(const std::string& imagePath);

    // Allocation
    Oop allocateSlots(uint32_t classIndex, size_t slotCount);
    Oop allocateBytes(uint32_t classIndex, size_t byteCount);

    // Class table
    Oop classAtIndex(uint32_t index) const { return classTable_[index]; }
    uint32_t indexOfClass(Oop classOop);

    // Object access
    Oop fetchPointer(size_t index, Oop obj);
    void storePointer(size_t index, Oop obj, Oop value);

    // Special objects
    Oop nil() const { return nilObject_; }
    Oop trueObject() const { return trueObject_; }
    Oop falseObject() const { return falseObject_; }

    // Garbage collection
    void incrementalGC();
    void fullGC();

    // Queries
    bool isYoung(Oop obj) const;
    bool isOld(Oop obj) const;
    size_t byteSizeOf(Oop obj) const;
};
```

---

## Phase 2: Image Loader

### 2.1 Image Format (Spur 64-bit)

```cpp
struct SpurImageHeader {
    uint32_t imageFormat;        // 68021 or 68533 (Sista)
    uint32_t headerSize;         // Bytes before first object
    uint64_t imageBytes;         // Total image size
    uint64_t startOfMemory;      // Base address when saved
    uint64_t specialObjectsOop;  // Oop of special objects array
    uint64_t lastHash;           // Last identity hash assigned
    uint64_t screenSize;         // Saved screen dimensions
    uint64_t imageHeaderFlags;   // Various flags
    uint32_t extraVMMemory;      // Additional memory requested
    // ... more fields
};
```

### 2.2 ImageLoader (src/vm/ImageLoader.hpp)

```cpp
class ImageLoader {
public:
    struct LoadResult {
        bool success;
        std::string error;
        Oop specialObjectsArray;
    };

    LoadResult load(const std::string& path, ObjectMemory& memory);

private:
    void parseHeader(std::istream& file, SpurImageHeader& header);
    void loadObjects(std::istream& file, ObjectMemory& memory);
    void relocatePointers(ObjectMemory& memory, uint64_t oldBase, uint64_t newBase);
    void initializeClassTable(ObjectMemory& memory);
};
```

---

## Phase 3: Interpreter

### 3.1 Bytecode Categories (Sista V1)

```
0-15:    Push receiver instance variable 0-15
16-31:   Push temporary variable 0-15
32-63:   Push literal constant 0-31
64-95:   Push literal variable 0-31
96-103:  Pop and store receiver variable 0-7
104-111: Pop and store temporary variable 0-7
112-119: Push receiver, true, false, nil, -1, 0, 1, 2
120-127: Return receiver, true, false, nil, top, block return
128-175: Extended push/store/send (2-3 byte instructions)
176-191: Send arithmetic messages (+, -, <, >, etc.)
192-207: Send common messages (at:, at:put:, size, etc.)
208-223: Send literal selector with 0 args
224-239: Send literal selector with 1 arg
240-255: Extended sends and jumps
```

### 3.2 Interpreter (src/vm/Interpreter.hpp)

```cpp
class Interpreter {
    ObjectMemory& memory_;

    // Execution state
    Oop activeContext_;
    Oop method_;
    uint8_t* instructionPointer_;
    Oop* stackPointer_;
    Oop* framePointer_;
    Oop receiver_;

    // Method cache
    struct CacheEntry {
        Oop selector;
        Oop classOop;
        Oop method;
    };
    std::array<CacheEntry, 1024> methodCache_;

    // Primitive table
    using PrimitiveFunc = bool (Interpreter::*)(int argCount);
    std::array<PrimitiveFunc, 576> primitiveTable_;

public:
    Interpreter(ObjectMemory& memory);

    void interpret();  // Main loop

private:
    // Bytecode handlers
    void pushReceiverVariable(int index);
    void pushTemporary(int index);
    void pushLiteral(int index);
    void pushLiteralVariable(int index);
    void storeReceiverVariable(int index);
    void storeTemporary(int index);
    void sendSelector(Oop selector, int argCount);
    void jump(int offset);
    void jumpIfTrue(int offset);
    void jumpIfFalse(int offset);
    void returnValue(Oop value);

    // Method lookup
    Oop lookupMethod(Oop selector, Oop classOop);
    void activateMethod(Oop method, int argCount);

    // Stack operations
    void push(Oop value);
    Oop pop();
    Oop stackTop();

    // Primitives
    bool primitiveAdd(int argCount);
    bool primitiveSubtract(int argCount);
    bool primitiveAt(int argCount);
    bool primitiveAtPut(int argCount);
    bool primitiveNew(int argCount);
    bool primitiveClass(int argCount);
    // ... more primitives
};
```

---

## Phase 4: Essential Primitives

### Absolutely Required (~30)

| # | Name | Description |
|---|------|-------------|
| 1 | primitiveAdd | SmallInteger + |
| 2 | primitiveSubtract | SmallInteger - |
| 3 | primitiveLessThan | SmallInteger < |
| 4 | primitiveGreaterThan | SmallInteger > |
| 5 | primitiveLessOrEqual | SmallInteger <= |
| 6 | primitiveGreaterOrEqual | SmallInteger >= |
| 7 | primitiveEqual | SmallInteger = |
| 8 | primitiveNotEqual | SmallInteger ~= |
| 9 | primitiveMultiply | SmallInteger * |
| 10 | primitiveDivide | SmallInteger / |
| 11 | primitiveMod | SmallInteger \\\\ |
| 12 | primitiveDiv | SmallInteger // |
| 13 | primitiveQuo | SmallInteger quo: |
| 14 | primitiveBitAnd | SmallInteger bitAnd: |
| 15 | primitiveBitOr | SmallInteger bitOr: |
| 16 | primitiveBitXor | SmallInteger bitXor: |
| 17 | primitiveBitShift | SmallInteger bitShift: |
| 60 | primitiveAt | Array/String at: |
| 61 | primitiveAtPut | Array/String at:put: |
| 62 | primitiveSize | Object size |
| 70 | primitiveNew | Class new |
| 71 | primitiveNewWithArg | Class new: |
| 73 | primitiveInstVarAt | Object instVarAt: |
| 74 | primitiveInstVarAtPut | Object instVarAt:put: |
| 75 | primitiveIdentityHash | Object identityHash |
| 83 | primitivePerform | Object perform: |
| 84 | primitivePerformWithArgs | Object perform:withArguments: |
| 110 | primitiveIdentical | Object == |
| 111 | primitiveClass | Object class |
| 113 | primitiveQuit | Exit VM |

### Display/Input (iOS-specific)

| # | Name | Description |
|---|------|-------------|
|
|
|
|
|

---

## Phase 5: iOS Integration

### 5.1 Display (src/ios/IOSDisplay.mm)

```objc
@interface PharoView : UIView
@property (nonatomic) Interpreter* interpreter;
- (void)updateDisplay:(uint32_t*)bits
                width:(int)width
               height:(int)height;
@end
```

### 5.2 Input

Map iOS touch events to Smalltalk mouse events.

### 5.3 Main Entry Point

```objc
- (void)startPharo:(NSString*)imagePath {
    ObjectMemory memory;
    memory.initialize(256 * 1024 * 1024);  // 256MB heap

    ImageLoader loader;
    auto result = loader.load([imagePath UTF8String], memory);
    if (!result.success) {
        NSLog(@"Failed to load image: %s", result.error.c_str());
        return;
    }

    Interpreter interpreter(memory);
    interpreter.interpret();
}
```

---

## Implementation Order

1. **Week 1: Core Types**
   - [ ] Oop class with tagging
   - [ ] ObjectHeader struct
   - [ ] Basic ObjectMemory (allocation only)
   - [ ] Unit tests for immediates

2. **Week 2: Image Loading**
   - [ ] Parse image header
   - [ ] Load object memory
   - [ ] Relocate pointers
   - [ ] Initialize class table

3. **Week 3: Basic Interpreter**
   - [ ] Bytecode dispatch loop
   - [ ] Push/pop/store bytecodes
   - [ ] Method lookup and cache
   - [ ] Primitive dispatch

4. **Week 4: Essential Primitives**
   - [ ] Arithmetic (1-17)
   - [ ] Object access (60-75)
   - [ ] Control (83-89)
   - [ ] Identity (110-113)

5. **Week 5: iOS Integration**
   - [ ] Basic display
   - [ ] Touch input
   - [ ] File primitives

6. **Week 6: GC and Polish**
   - [ ] Simple mark-sweep GC
   - [ ] Process scheduler
   - [ ] Debugging support

---

## Files to Create

```
src/vm/
├── Oop.hpp              # Type-safe object pointer
├── Oop.cpp
├── ObjectHeader.hpp     # Spur 64-bit header
├── ObjectMemory.hpp     # Heap management
├── ObjectMemory.cpp
├── ImageLoader.hpp      # Image file parser
├── ImageLoader.cpp
├── Interpreter.hpp      # Bytecode interpreter
├── Interpreter.cpp
├── Primitives.cpp       # Primitive implementations
├── MethodCache.hpp      # Method lookup cache
└── SpecialObjects.hpp   # Well-known objects

src/ios/
├── IOSDisplay.mm        # Display handling
├── IOSInput.mm          # Touch/keyboard
├── IOSFiles.mm          # File system
└── IOSMain.mm           # App entry point

tests/
├── OopTests.cpp
├── ObjectMemoryTests.cpp
├── ImageLoaderTests.cpp
└── InterpreterTests.cpp
```

---

## Reference Materials

- **SqueakJS**: https://github.com/codefrau/SqueakJS - Best reference implementation
- **RSqueak**: https://github.com/hpi-swa/RSqueak - Minimal primitive approach
- **Spur format**: https://clementbera.wordpress.com/2014/01/16/spurs-new-object-format/
- **Sista bytecodes**: https://clementbera.wordpress.com/2017/07/19/sista-open-alpha-release/
- **Image format**: https://wiki.squeak.org/squeak/6290

---

## Current Status

- [x] Research completed
- [x] Architecture designed
- [x] Plan documented
- [x] Phase 1: Core Types
  - [x] Oop.hpp - Type-safe object pointer with iOS ASLR-compatible tagging
  - [x] ObjectHeader.hpp - Spur 64-bit object header parsing
  - [x] ObjectMemory.hpp/.cpp - Heap management, allocation, class table
- [x] Phase 2: Image Loader
  - [x] ImageLoader.hpp/.cpp - Spur 64-bit image file parser
- [x] Phase 3: Interpreter
  - [x] Interpreter.hpp/.cpp - Sista V1 bytecode dispatch
  - [x] Frame management and method activation
  - [x] Method cache
- [x] Phase 4: Essential Primitives
  - [x] Primitives.cpp - ~30 essential primitives
  - [x] Arithmetic (1-17), Comparison, Object access (60-75)
  - [x] Identity (110-113), Behavior (83-84)
- [ ] Phase 5: iOS Integration (NEXT)
- [ ] Phase 6: GC and Polish
