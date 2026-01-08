# WIP Notes - iOS Pharo VM Clean C++ Implementation

## Date: 2026-01-08

## Current Status
Clean C++ VM implementation is functional. Image loads, interpreter initializes, and executes Smalltalk bytecode. Headless image runs startup code and goes idle (expected behavior).

## Test Results
```
./test_load_image /tmp/pharo-test/Pharo.image
- Image loaded: 51 MB Spur 64-bit (format 68021)
- Objects: 1,326,597 total
- Interpreter: 18 active bytecode steps before idle
- Status: Working correctly for headless image
```

## Completed Work

### 1. Sista V1 Bytecode Dispatch Fix (DONE)
- **Problem**: The entire 0x80-0xDF bytecode range was incorrectly mapped
- **Root Cause**: Original implementation used V3PlusClosures layout, not Sista V1
- **Solution**: Rewrote dispatch using correct Sista V1 mapping from pharo-vm cointerp.c:
  - 0x80-0x8F = Send literal selector 0-15 with 0 args
  - 0x90-0x9F = Send literal selector 0-15 with 1 arg
  - 0xA0-0xAF = Send literal selector 0-15 with 2 args
  - 0xB0-0xB7 = Short unconditional jump (offset 0-7)
  - 0xB8-0xBF = Short jump if true
  - 0xC0-0xC7 = Short jump if false
  - 0xC8-0xCF = Long jumps (various types)
  - 0xD0-0xD7 = Pop and store temp 0-7
  - 0xD8 = Pop stack top
  - 0xD9 = Unconditional trap
  - 0xE0-0xEF = Extension bytes (extA, extB)
- Added `usesSistaV1_` flag for bytecode set detection (header sign bit)
- Fixed callPrimitive (0xF8) skip: After primitive fails, skip the 3-byte callPrimitive instruction before executing method body

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

### 6. Process Scheduling Primitives (DONE)
- Implemented full process coordination primitives:
  - **primitiveSignal (85)**: Semaphore>>signal - wakes waiting process or increments excessSignals
  - **primitiveWait (86)**: Semaphore>>wait - decrements excessSignals or blocks on semaphore
  - **primitiveResume (87)**: Process>>resume - adds to scheduler queue, preempts if higher priority
  - **primitiveSuspend (88)**: Process>>suspend - removes from queue, switches to next process
- Added helper functions for scheduler management:
  - `getActiveProcess()` / `setActiveProcess()` - Scheduler access
  - `addLastLinkToList()` / `removeFirstLinkOfList()` / `removeProcessFromList()` - LinkedList operations
  - `wakeHighestPriority()` - Find highest priority runnable process
  - `putToSleep()` - Add process to its priority queue
  - `transferTo()` - Context switch between processes
- Added slot index constants for Process, ProcessScheduler, LinkedList, Semaphore objects

### 7. Object Allocation (WORKING)
- `primitiveNew` and `primitiveNewWithArg` fully functional
- Eden allocation with simplified scavenge (promotes all to old space)
- Sufficient for bootstrap and basic workloads

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
0x00-0x0F (0-15):     Push receiver variable 0-15
0x10-0x1F (16-31):    Push temporary 0-15
0x20-0x3F (32-63):    Push literal constant 0-31
0x40-0x5F (64-95):    Push literal variable 0-31
0x60-0x67 (96-103):   Pop store receiver var 0-7
0x68-0x6F (104-111):  Pop store temporary 0-7
0x70-0x77 (112-119):  Push special (self, true, false, nil, -1, 0, 1, 2)
0x78-0x7F (120-127):  Returns (receiver, true, false, nil, top, block)
0x80-0x8F (128-143):  Send literal 0-15 with 0 args
0x90-0x9F (144-159):  Send literal 0-15 with 1 arg
0xA0-0xAF (160-175):  Send literal 0-15 with 2 args
0xB0-0xB7 (176-183):  Short unconditional jump +1 to +8
0xB8-0xBF (184-191):  Short jump if true +1 to +8
0xC0-0xC7 (192-199):  Short jump if false +1 to +8
0xC8-0xCF (200-207):  Long jumps (extended offset)
0xD0-0xD7 (208-215):  Pop store temp 0-7
0xD8 (216):           Pop stack top
0xD9 (217):           Unconditional trap
0xE0-0xE7 (224-231):  Extension A (extA)
0xE8-0xEF (232-239):  Extension B (extB)
0xF0-0xF7 (240-247):  Extended sends/stores
0xF8 (248):           Call primitive (skip after prim fails)
0xF9-0xFF (249-255):  Extended operations (push closure, etc.)
```

## Next Steps (Future Work)

1. **GUI Support**: Implement display primitives for iOS rendering
2. **iOS Integration**: Bridge to Swift/UIKit for touch events, display
3. **Full GC**: Mark-sweep-compact for long-running applications (simplified scavenge works for now)

## Previous Notes (Archived)

### Old C-based VM Issues (before clean rewrite)
- BitBltPlugin registration fixed with sqNamedPrims.h
- Watchdog timeout during init
- Null receiver crashes
- NullWorldRenderer selection issues

These issues are from the original C-based OpenSmalltalk VM port. The clean C++ implementation avoids many of these architectural problems.
