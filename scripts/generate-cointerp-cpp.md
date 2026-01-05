# Generating cointerp.cpp for iOS

This document describes how to generate the C++ interpreter (`cointerp.cpp`) using the `CppCodeGenerator` from our pharo-vm-ios fork.

**VM Base Version: v12.0.0** (rebased 2026-01-02)

## Prerequisites

1. A VMMaker image (Pharo 12 with VMMaker loaded)
2. The pharo-vm-ios fork cloned at `~/src/pharo/pharo-vm-ios`

## Steps

### 1. Launch VMMaker Image

```bash
./scripts/launch-vmmaker.sh
```

Or manually:
```bash
/path/to/pharo Pharo.image
```

### 2. Load the Slang-iOS Package

In a Pharo Playground:

```smalltalk
"Load the Slang-iOS package from our fork"
Metacello new
    baseline: 'VMMaker';
    repository: 'tonel:///Users/wohl/src/pharo/pharo-vm-ios/smalltalksrc';
    load: #('Slang-iOS').
```

Or manually file in the package:

```smalltalk
| repo |
repo := TonelRepository new directory: '/Users/wohl/src/pharo/pharo-vm-ios/smalltalksrc' asFileReference.
repo loadPackage: 'Slang-iOS'.
```

### 3. Generate cointerp.cpp

```smalltalk
"Generate C++ interpreter using CppCodeGenerator"
| vmMaker generator outputFile |

"Create the C++ code generator"
generator := CppCodeGenerator new.

"Configure for iOS/Stack VM (no JIT)"
generator
    vmClass: StackInterpreter;
    options: #(
        SPURVM true
        STACKVM true
        COGVM false  "No JIT on iOS"
    ).

"Add all required classes"
generator
    addClass: SpurMemoryManager;
    addClass: StackInterpreter;
    addClass: SpurSegmentManager;
    addClass: SpurGenerationScavenger;
    addClass: VMMemoryMap.

"Generate to file"
outputFile := '/Users/wohl/src/pharo/iospharo/src/ios/cointerp.cpp' asFileReference.
outputFile writeStreamDo: [ :stream |
    generator emitCCodeOn: stream
].

Transcript show: 'Generated: ', outputFile fullName; cr.
```

### 4. Verify the Output

The generated `cointerp.cpp` should have:

1. `#include "oop.hpp"` at the top
2. Space checks using Oop methods:
   - `oop.isYoung()` instead of `(oop & mask) == newMask`
   - `oop.isOld()` instead of `(oop & mask) == oldMask`
   - `oop.isPermanent()` instead of `oop >= permSpaceStart`
3. Integer operations using Oop methods:
   - `oop.isSmallInteger()` instead of `(oop & 1)`
   - `oop.integerValueOf()` instead of `(oop >> 3)`
   - `Oop::integerObjectOf(value)` instead of `((value << 3) | 1)`

### 5. Build with C++

```bash
cd ~/src/pharo/iospharo
mkdir build-cpp
cd build-cpp
cmake -G Xcode \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
    -DUSE_CPP_OOP=ON \
    ..
xcodebuild -scheme PharoVMCore -configuration Release -sdk iphonesimulator
```

## Current Status

The `CppCodeGenerator` is implemented in:
- `pharo-vm-ios/smalltalksrc/Slang-iOS/CppCodeGenerator.class.st`

It overrides these key methods:
- `generateCASTIntegerValueOf:` → `oop.integerValueOf()`
- `generateCASTIntegerObjectOf:` → `Oop::integerObjectOf()`
- `generateCASTIsIntegerObject:` → `oop.isSmallInteger()`
- `generateCASTForBuiltinConstructFor:` → intercepts `memoryMap isYoungObject:` etc.

## Troubleshooting

### "Class not found" errors
Make sure all VMMaker packages are loaded before loading Slang-iOS.

### Type mismatches
The generator uses heuristics to identify oop-typed variables. If a variable is incorrectly typed, you may need to add it to `shouldTransformToOop:inMethod:` in CppCodeGenerator.

### Missing translations
Some Slang constructs may not have C++ translations yet. Check the translation dictionary in `initializeIOSTranslations`.
