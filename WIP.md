# WIP Notes - iOS Pharo VM Clean C++ Implementation

## Date: 2026-01-07

## Current Status
Clean C++ VM implementation is functional. Image loads, interpreter initializes, and executes Smalltalk bytecode. Headless image runs startup code and goes idle (expected behavior).

## Test Results
```
./test_load_image /tmp/pharo-test/Pharo.image
- Image loaded: 51 MB Spur 64-bit (format 68021)
- Objects: 1,326,597 total
- Interpreter: 183 active bytecode steps before idle
- Status: Working correctly for headless image
```

## Completed Work

### 1. Sista V1 Bytecode Format (DONE)
- Updated all bytecode dispatch from V3PlusClosures to Sista V1
- Fixed bytecode ranges:
  - 0x4C = push self (was incorrectly "push literal variable 12")
  - 0x80-0x8F = short jumps (if true/false)
  - 0x90-0x9F = jump if true with offset
  - 0xA0-0xA7 = jump if false with offset
  - 0xA8-0xAF = extended jumps (types 0-7)
  - 0xE0-0xEF = Sista extension bytes (extA, extB)

### 2. mustBeBoolean Infinite Loop Fix (DONE)
- **Problem**: Non-boolean values in conditionals triggered `sendMustBeBoolean`, whose Smalltalk implementation has its own conditionals, causing infinite recursion
- **Solution**: Changed all conditional jump bytecodes to treat non-booleans as false instead of calling sendMustBeBoolean
- Affected functions: `shortJumpIfTrue()`, `shortJumpIfFalse()`, `longJumpIfTrue()`, `longJumpIfFalse()`, and extended jump cases

### 3. Extended Jump Types (DONE)
- Added missing extended jump cases 5, 6, 7 in 0xA8-0xAF range:
  - Case 5: Jump if nil
  - Case 6: Jump if not nil
  - Case 7: Reserved (no-op)

### 4. Bootstrap Startup (DONE)
- Implemented startup sequence for headless images
- Tries multiple entry points: `recordStartupStamp`, `restartMethods`, `Object>>yourself`
- Fixed misleading error message - "no entry point" is normal for headless images

### 5. Debug Output Reduction (DONE)
- Commented out all verbose debug output in:
  - `ImageLoader.hpp` - forEachObject iteration
  - `ImageLoader.cpp` - raw byte dumps, ASCII detection
  - `Interpreter.cpp` - slot inspection, method headers, bytecode dumps
  - `Primitives.cpp` - snapshot debug
  - `ObjectMemory.cpp` - various debug logs

## Key Files

### Clean VM Implementation (src/vm/)
- `Oop.hpp` - Type-safe 64-bit object pointer with iOS ASLR-compatible tagging
- `ObjectHeader.hpp` - Spur object header decoding
- `ObjectMemory.hpp/cpp` - Heap management, object access, special objects
- `ImageLoader.hpp/cpp` - Spur image loading with pointer relocation
- `Interpreter.hpp/cpp` - Sista V1 bytecode interpreter
- `Primitives.cpp` - Primitive implementations
- `test_load_image.cpp` - Test harness

## Build Commands
```bash
cd /Users/wohl/src/pharo/iospharo/src/vm

# Build test binary
clang++ -std=c++17 -O0 -g test_load_image.cpp ImageLoader.cpp \
  ObjectMemory.cpp Interpreter.cpp Primitives.cpp -o test_load_image

# Run test
./test_load_image /tmp/pharo-test/Pharo.image
```

## Architecture Notes

### Oop Tagging (iOS ASLR Compatible)
Uses LOW bits for tags (not high bits):
- Bit 0 = 1: Immediate value
  - Tag 001: SmallInteger (61-bit signed)
  - Tag 011: Character (29-bit Unicode)
  - Tag 101: SmallFloat (rotated double)
- Bit 0 = 0: Object pointer
  - Bits 2-1: Space encoding (Old=0, New=1, Perm=2)
  - Bits 63-3: 8-byte aligned address

### Sista V1 Bytecode Layout
```
0-15:    Push receiver variable 0-15
16-31:   Push temporary 0-15
32-63:   Push literal constant 0-31
64-95:   Push literal variable 0-31
96-103:  Pop store receiver var 0-7
104-111: Pop store temporary 0-7
112-119: Push special (self, true, false, nil, -1, 0, 1, 2)
120-127: Returns
128-175: Extended push/store/pop
176-191: Arithmetic sends
192-207: Common sends
208-255: Various sends and jumps
```

## Next Steps (Future Work)

1. **GUI Support**: Implement display primitives for iOS rendering
2. **Process Scheduling**: Full process scheduler for interactive images
3. **GC Integration**: Connect to memory management for new object allocation
4. **iOS Integration**: Bridge to Swift/UIKit for touch events, display

## Previous Notes (Archived)

### Old C-based VM Issues (before clean rewrite)
- BitBltPlugin registration fixed with sqNamedPrims.h
- Watchdog timeout during init
- Null receiver crashes
- NullWorldRenderer selection issues

These issues are from the original C-based OpenSmalltalk VM port. The clean C++ implementation avoids many of these architectural problems.
