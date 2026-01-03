# WIP: iOS C++ Oop Wrapper Implementation

**Last updated:** 2026-01-02 23:30

## Problem
iOS ASLR randomizes memory addresses, breaking Pharo VM's space detection which relies on fixed address ranges (oldSpace at 0x10000000000, etc.). The crash at `0x100010db558` is an unrelocated Spur pointer.

## Solution Approach
Encode memory space (new/old/perm) explicitly in low bits of pointers using a C++ Oop wrapper class, eliminating address-range-based space detection.

## Current Status

### Completed
1. **Fork created**: https://github.com/avwohl/pharo-vm branch `feat/ios-cpp-oop`
2. **Rebased to v12.0.0** (was v10.3.9)
3. **CppCodeGenerator** created in `Slang-iOS` package
4. **oop.hpp** C++ wrapper class with space encoding
5. **VMMaker generation working**: `PharoVMMaker generate:outputDirectory:` API discovered
6. **C-to-C++ transformer** script created: `scripts/c-to-cpp-transform.py`
7. **Hybrid C/C++ build working**: CMakeLists.txt updated, builds successfully
8. **oop_wrapper.cpp/h created**: C-callable wrapper functions for space detection

### Build Verified
Hybrid build compiles successfully. The `libPharoVMCore.a` includes all oop_wrapper symbols:
- `oop_is_young`, `oop_is_old`, `oop_is_permanent`
- `oop_encode_with_space`, `oop_get_space`
- `oop_is_immediate`, `oop_is_non_immediate`
- `oop_integer_value_of`, `oop_integer_object_of`
- `oop_to_pointer`

### Completed
1. **Patched cointerp.c space checks**:
   - 60 calls patched: `isOldObject(GIV(memoryMap), oop)` → `oop_is_old((uint64_t)(oop))`
   - Also patched `getMemoryMap()` variant patterns
   - Build verified: compiles and links successfully

2. **Added space encoding during image swizzle**:
   - `swizzleObj()` now encodes space in low bits after address adjustment
   - Permanent space objects → `OOP_SPACE_PERM` encoding
   - Old space objects (from image file) → `OOP_SPACE_OLD` encoding

3. **XCFramework built**:
   - iOS device (arm64)
   - iOS Simulator (arm64 + x86_64)
   - Mac Catalyst (arm64 + x86_64)

### Remaining Work
- Runtime allocations (new objects) don't yet get space encoded
- May need to encode when storing references to object fields
- GC space transitions (new→old) need encoding updates

### Previous Blocker (Resolved via Hybrid Approach)
Pure C++ compilation of cointerp.cpp fails due to C/C++ incompatibilities:
- `class` keyword used as variable name (200+ occurrences) - FIXED with renaming
- `sqInt` (long) used to store pointers - C++ doesn't allow implicit conversions
- Anonymous structs in function pointers
- void* implicit casts

## Key Files

### In iospharo
- `src/ios/cointerp.c` - Current C interpreter (iOS patched)
- `src/ios/oop_wrapper.cpp` - C++ wrapper (NEW)
- `src/ios/oop_wrapper.h` - C header for wrapper (NEW)
- `scripts/c-to-cpp-transform.py` - C to C++ transformer
- `scripts/generate-cpp-interpreter.st` - VMMaker generation script

### In pharo-vm-ios fork
- `smalltalksrc/Slang-iOS/CppCodeGenerator.class.st` - C++ code generator
- `src/ios/oop.hpp` - C++ Oop wrapper class
- `smalltalksrc/Slang-iOS/SpurMemoryManager.extension.st` - iOS space checks
- `smalltalksrc/Slang-iOS/SpurSegmentManager.extension.st` - iOS swizzle

## VMMaker Generation Command
```bash
cd ~/src/pharo/pharo-vm/build-gen/build/vmmaker
./vm/Contents/MacOS/Pharo --headless image/VMMaker.image \
  perform PharoVMMaker generate:outputDirectory: CoInterpreter /output/path
```

## Build Commands
```bash
# C build (working)
cmake -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 ..
xcodebuild -scheme PharoVMCore -configuration Release -sdk iphonesimulator

# C++ build (not yet working)
cmake -DUSE_CPP_OOP=ON ...
```

## Oop Bit Layout
```
Bit 0:     isImmediate (1 = SmallInteger/Character)
Bits 1-2:  Space (00=new, 01=old, 10=perm) when bit 0=0
Bits 3-63: Payload (value or pointer address)
```

## Space Check Transformation
```c
// Before (address-based, broken on iOS)
isOldObject(GIV(memoryMap), oop)

// After (bit-based, ASLR compatible)
oop_is_old(oop)  // calls C++ wrapper
```
