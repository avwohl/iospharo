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

### Current Blocker
Pure C++ compilation of cointerp.cpp fails due to C/C++ incompatibilities:
- `class` keyword used as variable name (200+ occurrences) - FIXED with renaming
- `sqInt` (long) used to store pointers - C++ doesn't allow implicit conversions
- Anonymous structs in function pointers
- void* implicit casts

### Next Steps (Hybrid Approach)
1. Keep `cointerp.c` as C
2. Add `oop_wrapper.cpp` (C++ with extern "C") for space-aware functions
3. Modify C code to call wrapper functions for space checks
4. Update CMakeLists.txt to compile both C and C++

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
