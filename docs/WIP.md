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

### 8. Float Primitives (DONE)
- Implemented all 13 float primitives (40-55):
  - **Arithmetic**: add, subtract, multiply, divide
  - **Comparison**: lessThan, equal
  - **Conversion**: truncated (Float to SmallInteger)
  - **Math**: sqrt, sin, cos, arctan, exp, ln
- Helper functions:
  - `extractFloat()` - Extract double from SmallFloat immediate or boxed Float
  - `makeFloat()` - Create Float result (tries SmallFloat first, allocates boxed if needed)
- Handles both SmallFloat immediates and boxed Float objects

### 9. Large Integer Primitives (DONE)
- Implemented arbitrary-precision arithmetic for LargePositiveInteger and LargeNegativeInteger:
  - **primitiveLargeIntegerAdd (21)**: Addition with sign handling
  - **primitiveLargeIntegerSubtract (22)**: Subtraction via negation
  - **primitiveLargeIntegerMultiply (29)**: Multiplication
  - **primitiveLargeIntegerDivide (30)**: Exact integer division
  - **primitiveLargeIntegerMod (31)**: Modulo operation
- Helper functions:
  - `isLargeInteger()` - Detect LargePositiveInteger/LargeNegativeInteger
  - `extractMagnitude()` - Get byte array from LargeInteger
  - `compareMagnitudes()` / `addMagnitudes()` / `subtractMagnitudes()` - Magnitude operations
  - `multiplyMagnitudes()` / `divideMagnitudes()` - Long multiplication/division
  - `tryConvertToSmallInteger()` - Normalize to SmallInteger when possible
  - `makeLargeInteger()` - Allocate LargeInteger from magnitude
  - `extractInteger()` - Handle both SmallInteger and LargeInteger inputs
- LargeIntegers stored as little-endian byte arrays
- Results automatically normalize to SmallInteger when they fit

### 10. String Primitives (DONE)
- Implemented Character-based string access:
  - **primitiveStringAt (63)**: Returns Character at index
  - **primitiveStringAtPut (64)**: Stores Character at index
- Supports two string formats:
  - **ByteString** (format 16-23): 1 byte per character, ASCII/Latin-1
  - **WideString** (format 10-11): 4 bytes per character, full Unicode
- Features: 1-based indexing, bounds checking, immutability checking

### 11. System Primitives (DONE)
- **primitiveQuit (113)**: Exit VM with optional exit code
  - Accepts SmallInteger exit code argument
  - Calls std::exit() to terminate process
- **primitiveSnapshot (97)**: Save image to file
  - Argument: filename as ByteString
  - Writes Spur 64-bit image header (128 bytes)
  - Translates runtime pointers to canonical image format (0x10000000000)
  - Iterates all objects and translates pointer slots
  - Returns true on success
- **primitiveVMParameter (254)**: Access VM configuration
  - 0 args: Returns array of all 86 parameters
  - 1 arg: Returns parameter at index
  - 2 args: Sets parameter (most read-only)
  - Reports heap size, GC stats, image format, VM features
- **primitiveExitToDebugger (114)**: Halt VM with debug trap

### 12. Time Primitives (DONE)
- **primitiveMillisecondClock (135)**: Milliseconds since VM start (wraps ~12 days)
- **primitiveSecondsClock (137)**: Seconds since Smalltalk epoch (Jan 1, 1901)
- **primitiveMicrosecondClock (240)**: High-resolution microsecond timer
- **primitiveLocalMicrosecondClock (241)**: Local time microseconds
- Uses C++ chrono for cross-platform timing

### 13. Character Conversion Primitives (DONE)
- **primitiveAsCharacter (170)**: Integer to Character (validates Unicode range)
- **primitiveAsInteger (171)**: Character to Integer

### 14. Point Creation (DONE)
- **primitiveMakePoint (18)**: Create Point from x and y values
  - Arguments: receiver (x value), argument (y value)
  - Creates new Point object with x and y slots
  - Used by `@` message (e.g., `3@4`)

### 15. Large Integer Comparisons (DONE)
Extended large integer support with comparison operations:
- **primitiveLargeIntegerLessThan (23)**: `<` comparison
- **primitiveLargeIntegerGreaterThan (24)**: `>` comparison
- **primitiveLargeIntegerLessOrEqual (25)**: `<=` comparison
- **primitiveLargeIntegerGreaterOrEqual (26)**: `>=` comparison
- **primitiveLargeIntegerEqual (27)**: `=` comparison
- **primitiveLargeIntegerNotEqual (28)**: `~=` comparison
- All handle mixed SmallInteger/LargeInteger comparisons
- Proper signed comparison (negative values less than positive)

### 16. GC Primitives (DONE)
- **primitiveFullGC (130)**: Trigger full garbage collection
  - Returns number of free bytes in old space after collection

### 17. Float Primitives (CORRECTED)
Fixed primitive numbering to match standard Pharo/Squeak VM:
- **primitiveAsFloat (40)**: Convert integer to Float
- **primitiveFloatAdd (41)**: Float addition
- **primitiveFloatSubtract (42)**: Float subtraction
- **primitiveFloatLessThan (43)**: `<` comparison
- **primitiveFloatGreaterThan (44)**: `>` comparison
- **primitiveFloatLessOrEqual (45)**: `<=` comparison
- **primitiveFloatGreaterOrEqual (46)**: `>=` comparison
- **primitiveFloatEqual (47)**: `=` comparison
- **primitiveFloatNotEqual (48)**: `~=` comparison
- **primitiveFloatMultiply (49)**: Float multiplication
- **primitiveFloatDivide (50)**: Float division
- **primitiveFloatTruncated (51)**: Truncate to integer
- **primitiveFractionalPart (52)**: Get fractional part
- **primitiveExponent (53)**: Get IEEE exponent
- **primitiveTimesTwoPower (54)**: Multiply by 2^n (ldexp)
- **primitiveFloatSquareRoot (55)**: Square root
- **primitiveFloatSin (56)**: Sine
- **primitiveFloatArctan (57)**: Arc tangent
- **primitiveFloatLn (58)**: Natural logarithm
- **primitiveFloatExp (59)**: Exponential (e^x)

### 18. Large Integer Bitwise Operations (DONE)
Extended large integer support with division and bitwise operations:
- **primitiveLargeIntegerDiv (32)**: Floor division (toward -infinity)
- **primitiveLargeIntegerQuo (33)**: Truncating division (toward zero)
- **primitiveLargeIntegerBitAnd (34)**: Bitwise AND
- **primitiveLargeIntegerBitOr (35)**: Bitwise OR
- **primitiveLargeIntegerBitXor (36)**: Bitwise XOR
- **primitiveLargeIntegerBitShift (37)**: Bit shift (positive=left, negative=right)

### 19. Utility Primitives (DONE)
- **primitiveFlushCache (89)**: Clear method cache (for dynamic method changes)
- **primitiveBytesLeft (112)**: Return free memory bytes
- **primitiveSpecialObjectsOop (129)**: Return the special objects array

### 20. Context and Closure Primitives (DONE)
- **primitiveThisContext (199)**: Return the current execution context
- **primitiveClosureNumArgs (206)**: Return number of arguments a BlockClosure expects

### 21. Slot Access Primitives (DONE)
- **primitiveSlotAt (173)**: Read object slot at 1-based index
- **primitiveSlotAtPut (174)**: Write object slot at 1-based index

### 22. Object Enumeration Primitives (DONE)
- **primitiveAllInstances (177)**: Return array of all instances of a class
- **primitiveAllObjects (178)**: Return array of all objects in the system

### 23. Object Reference Primitives (DONE)
- **primitiveObjectPointsTo (132)**: Check if object points to another object

### 24. Become Primitives (DONE)
- **primitiveBecome (72)**: Two-way identity swap between two objects
- **primitiveBecomeForward (128)**: One-way forward all references from one object to another

### 25. Bit Operation Primitives (DONE)
- **primitiveHighBit (575)**: Return index of highest set bit (1-based, 0 if no bits)
- **primitiveLowBit (576)**: Return index of lowest set bit (1-based, 0 if no bits)

### 26. Word Array Access Primitives (DONE)
- **primitiveIntegerAt (165)**: Read 32-bit signed integer from word array
- **primitiveIntegerAtPut (166)**: Write 32-bit signed integer to word array

### 27. Class/Behavior Primitives (DONE)
- **primitiveBehaviorHash (175)**: Return identity hash for a behavior/class
- **primitiveChangeClass (115)**: Change the class of an object

### 28. 16-bit Array Access Primitives (DONE)
- **primitiveShortAt (143)**: Read 16-bit unsigned integer from short array
- **primitiveShortAtPut (144)**: Write 16-bit unsigned integer to short array

### 29. Raw Object Iteration Primitives (DONE)
- **primitiveSomeObject (138)**: Return first object in memory
- **primitiveNextObject (139)**: Return next object in memory after this one

### 30. VM Attribute Primitive (DONE)
- **primitiveGetAttribute (149)**: Get VM attribute by index (version, type, etc.)

### 31. Immutability Primitives (DONE)
- **primitiveGetImmutability (150)**: Get object's immutability flag
- **primitiveSetImmutability (151)**: Set object's immutability flag

### 32. Object Copy Primitive (DONE)
- **primitiveCopyObject (168)**: Create shallow copy of object with new identity

### 33. Compiled Method Creation Primitive (DONE)
- **primitiveNewMethod (79)**: Create new CompiledMethod with given header and size

### 34. Instance Adoption Primitive (DONE)
- **primitiveAdoptInstance (160)**: Change object's class (with format check)

### 35. Object Pinning Primitives (DONE)
- **primitiveIsPinned (183)**: Check if object is pinned (won't move during GC)
- **primitivePin (184)**: Pin an object to prevent GC from moving it
- **primitiveUnpin (185)**: Unpin an object to allow GC to move it

### 36. Memory Management Primitives (DONE)
- **primitiveMaxIdentityHash (176)**: Return maximum identity hash value (2^22 - 1)
- **primitiveGrowMemory (180)**: Request memory growth (returns current free space)
- **primitiveSignalAtBytesLeft (125)**: Register semaphore for low memory signal

### 37. Interrupt Semaphore Primitive (DONE)
- **primitiveInterruptSemaphore (134)**: Set the interrupt semaphore

### 38. Context Termination Primitive (DONE)
- **primitiveTerminateTo (196)**: Terminate context chain from receiver to target

### 39. Float Bit Access Primitives (DONE)
- **primitiveFloatAt (38)**: Read 32-bit word from Float at index (1 or 2)
- **primitiveFloatAtPut (39)**: Write 32-bit word to Float at index (1 or 2)

### 40. LargeInteger Digit Access Primitives (DONE)
- **primitiveDigitAt (19)**: Read byte at 1-based index from LargeInteger magnitude
- **primitiveDigitAtPut (20)**: Write byte at 1-based index to LargeInteger magnitude

### 41. Exception Handler Primitives (DONE)
- **primitiveMarkHandlerMethod (186)**: Mark context as exception handler (on:do:)
- **primitiveMarkUnwindMethod (187)**: Mark context as unwind-protect (ensure:)
- **primitiveFindHandlerContext (188)**: Find handler context for exception class
- **primitiveFindNextUnwindContext (189)**: Find next ensure: context up to limit

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

## Current Priority

**Fix Morphic Processes** - The image's Morphic event loop and InputEventSensor aren't running:
- Primitive 264 (getNextEvent) is registered but never called
- Only primitive 230 (relinquishProcessor) runs in a loop
- The scheduler works but only the idle process is active
- Event sensor process never starts

See `WIP-input-handling.md` for current investigation.

## Archived Issues (Fixed)

### Embedded VM Startup (Fixed 2026-01-14)
**Problem:** When loading a Pharo image in embedded mode, `primitiveQuit` was called and left the VM in a broken state with corrupted stack.

**Solution:** Modified `primitiveQuit` to properly handle embedded mode by popping the broken stack state and calling `tryReschedule()` to find another runnable process (MorphicRenderLoop at priority 40).

### World Menu Drawing (Fixed 2026-01-17)
**Problem:** Method lookup failures caused DNU errors for basic methods like `#owner`, `#layoutChanged`.

**Root Cause:** Method lookup had a 1024 entry limit but Morph's methodDict has 2050+ slots.

**Solution:** Removed the arbitrary limit - `size_t maxSearch = size;` instead of `std::min(size, (size_t)1024)`.

### Old C-based VM Issues (before clean rewrite)
- BitBltPlugin registration fixed with sqNamedPrims.h
- Watchdog timeout during init
- Null receiver crashes
- NullWorldRenderer selection issues

These issues are from the original C-based OpenSmalltalk VM port. The clean C++ implementation avoids many of these architectural problems.
