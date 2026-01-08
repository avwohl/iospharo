# WIP: iOS Type-Safe C++ Oop Class for ASLR Compatibility

**Last updated:** 2026-01-06

## Problem
iOS ASLR randomizes memory addresses, breaking Pharo VM's space detection which relies on fixed address ranges. Previous attempts using runtime masking had bugs because code like `longAt(oop + 8)` corrupts tag bits before masking.

## Solution: Type-Safe C++ with Compile-Time Error Detection

Use C++ types that catch pointer manipulation errors at compile time rather than runtime.

### Key Classes

1. **Oop** - Tagged object pointer, NO arithmetic operators
   - Trying `oop + 8` is a compile error
   - Must use `oop.longAt(8)` or `oop.rawAddress()` instead

2. **RawAddress** - Untagged memory address, allows arithmetic
   - Use for pointer math: `addr + 8`, `addr1 - addr2`
   - Obtained via `oop.rawAddress()`

### iOS Tagging Scheme (modified from Spur)

```
Bit 0 = 1: Immediate (all immediates have bit 0 set)
  001 = SmallInteger
  011 = Character (was 010 in Spur)
  101 = SmallFloat (was 100 in Spur)

Bit 0 = 0: Object pointer
  Bits 1-2: Space encoding
    00 = New space (young generation)
    01 = Old space (tenured)
    10 = Perm space (permanent)
    11 = Code space (reserved)
  Bits 3-63: Heap address (always 8-byte aligned)
```

The key change from Spur: Character and SmallFloat tags now have bit 0 = 1, which frees bits 1-2 for space encoding in object pointers.

## Implementation Status

### Completed (2026-01-06)

1. **oop.hpp** - Type-safe C++ Oop class (`/Users/wohl/src/pharo/iospharo/src/ios/oop.hpp`)
   - `Oop` class with NO arithmetic operators
   - `RawAddress` class for safe pointer arithmetic
   - Space encoding in bits 1-2
   - All compile-time tests pass

2. **CppCodeGenerator** - VMMaker code generator (`/Users/wohl/src/pharo/pharo-vm-ios/smalltalksrc/Slang-iOS/CppCodeGenerator.class.st`)
   - Transforms VMMaker output to use Oop/RawAddress types
   - Converts memory access patterns automatically

3. **Test suite** - (`/Users/wohl/src/pharo/iospharo/test-cpp-generation/test_oop.cpp`)
   - Tests all immediate types (SmallInteger, Character)
   - Tests object pointers with space encoding
   - Tests memory access
   - Verifies compile-time error detection

## Compile-Time Error Detection

The following are compile errors (not runtime bugs):

```cpp
Oop obj = Oop::fromPointer(&heap[0], Space::New);

// These all fail to compile:
Oop bad1 = obj + 8;           // No arithmetic on Oop
Oop bad2 = 12345;             // No implicit int->Oop
int64_t bad3 = obj;           // No implicit Oop->int
RawAddress bad4 = obj;        // No implicit Oop->RawAddress

// These are correct:
RawAddress addr = obj.rawAddress();     // Explicit extraction
RawAddress addr2 = addr + 8;            // Arithmetic on RawAddress OK
int64_t val = obj.longAt(8);            // Memory access with offset
Oop slot = obj.oopAt(8);                // Oop access with offset
```

## Key Files

| File | Description |
|------|-------------|
| `src/ios/oop.hpp` | C++ Oop and RawAddress classes |
| `test-cpp-generation/test_oop.cpp` | Test suite |
| `pharo-vm-ios/smalltalksrc/Slang-iOS/CppCodeGenerator.class.st` | Code generator |

## Build and Test

```bash
cd test-cpp-generation
clang++ -std=c++17 -I../src/ios -o test_oop test_oop.cpp
./test_oop
```

## Next Steps

1. Use CppCodeGenerator to generate cointerp.cpp from VMMaker
2. Compile generated code with oop.hpp
3. Run the full VM with iOS space encoding
