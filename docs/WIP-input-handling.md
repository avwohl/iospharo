# WIP: Input Handling - InputEventSensor Not Starting

## Status: In Progress (2026-01-24)

**Current Problem:** InputEventSensor process not starting during image startup.

## Latest Debug Session (2026-01-24)

### Fixed: VM Now Runs 283k+ Steps
The VM was hanging during startup. Fixed multiple issues:

1. **Hang at step 260** - Null allPrimLog file pointer crash
   - Fixed by adding null checks before all allPrimLog fprintf calls

2. **Hang at step 10284** - Null defaultSendLog file pointer crash
   - Fixed by adding null check when writing to defaultSendLog in sendSelector
   - The log file is only opened when ENABLE_DEBUG_LOGGING is true

3. **Hang at step 20392** - Infinite loop in primitiveTerminateTo
   - Added safety limit (10000 iterations) to prevent infinite loops
   - The context sender chain had a cycle or was extremely long

### VM Status After Fixes
- Runs 283872 bytecode steps before becoming idle
- Becomes idle waiting for external events (semaphore wait)
- No more hangs during startup

### Also Fixed (Previous Session)
- OS name detection now returns "iOS" on iOS devices (TARGET_OS_IOS)
- Cleaned up temporary debug output from step(), sendSelector(), primitiveWait()
- Removed ~70 lines of temporary debugging code
- Wrapped 98 debug fopen calls with `if constexpr (ENABLE_DEBUG_LOGGING)`

## Original Problem

InputEventSensor's process isn't starting during image startup.

## Evidence

1. **Only 3 processes active** - Delay scheduler (priority 80), idle process (priority 10), and transient processes

2. **Input semaphore accumulates signals** - excessSignals grows to 99+ with no consumer

3. **Primitive 264 (getNextEvent) never called** - Events pile up but nothing reads them

4. **World loop runs but no event processing**:
```
[SEND #1] #doOneCycle argCount=0
[SEND #12] #processEvents argCount=0  <-- called but no events consumed
[SEND #19] #idleProcess argCount=0
[SEND #20] #relinquishProcessorForMicroseconds: argCount=1
```

## Investigation Steps

1. **SessionManager>>startUp:** - Does it complete? Does it initialize UI properly?
2. **MorphicCoreUIManager initialization** - This creates the world loop
3. **EventSensor>>startUp:** - This should fork the event polling process
4. **InputEventSensor class>>installEventSensorFramework** - Event infrastructure setup
5. **OSWindow/OSiOSDriver integration** - May need proper driver initialization

## C++ Workarounds Removed

Per CLAUDE.md policy: NO WORKAROUNDS. All C++ event dispatch code has been removed:
- `dispatchMouseEventToMorph`, `handleMenuBarClick`, `handleWorldMenuClick`
- `executeMenuItemAction`, `processPendingMenuAction`, `processPendingWorldMenu`
- Direct Hand/window manipulation

**Events MUST be handled by Smalltalk's InputEventSensor and Morphic.**

## Event Pipeline (Working)

The event flow infrastructure is implemented and working:

```
Swift Gesture
    ↓
vm_postMouseEvent() / vm_postKeyEvent() / vm_postScrollEvent()
    ↓
pharo::gEventQueue.push(event)
    ↓
eventCallback() → signalExternalSemaphore(index)
    ↓
interpret() loop checks hasPendingSignals()
    ↓
processPendingSignals() → signals semaphore
    ↓
[BLOCKED] No process waiting on semaphore
```

## Event Buffer Format (8 slots)

**Mouse (type 1):** [0] type=1, [1] timestamp, [2] x, [3] y, [4] buttons, [5] modifiers, [6] reserved, [7] windowIndex

**Keyboard (type 2):** [0] type=2, [1] timestamp, [2] charCode, [3] pressCode, [4] modifiers, [5] keyCode, [6] reserved, [7] windowIndex

**MouseWheel (type 7):** [0] type=7, [1] timestamp, [2] x, [3] y, [4] deltaX, [5] deltaY, [6] modifiers, [7] windowIndex

## Key Files

- `src/platform/EventQueue.hpp/cpp` - Event queue implementation
- `src/platform/PlatformBridge.h/cpp` - C interface for Swift
- `src/vm/Interpreter.hpp/cpp` - External semaphore signaling
- `src/vm/Primitives.cpp` - Event primitives (264, 265, 266)
- `src/smalltalk/OSiOSDriver.st` - iOS driver with event loop (NOT in standard image)

## Root Cause Analysis (2026-01-25)

### Why No Event Loop Process

Investigation reveals the exact gap:

1. **OSiOSDriver class doesn't exist in standard Pharo images**
   - Our OSiOSDriver.st defines it but it's not filed in
   - Log shows: `OSiOSDriver class: 0xcf9000000 sameAsNil=1` (not found)

2. **OSSDL2Driver is used instead**
   - Log: `Successfully set OSSDL2Driver instance as Current: 0xcfd248368`
   - But OSSDL2Driver relies on FFI to call SDL_PollEvent
   - FFI primitives (117, 118, 147) aren't fully implemented

3. **OSSDL2Driver has no ensureEventLoop**
   - Log: `ensureEventLoop instance method NOT found in OSSDL2Driver`
   - OSSDL2Driver expects FFI to poll SDL2 events, doesn't have a primitive 264 polling loop

4. **InputEventSensor doesn't exist in modern Pharo**
   - Log: `InputEventSensor class: 0xcf9000000 sameAsNil=1`
   - Modern Pharo uses OSWindow/OSWorldRenderer, not InputEventSensor

### Working Components

- **Event queue** - Events arrive from UIKit/AppKit via PlatformBridge ✓
- **Input semaphore signaling** - processPendingSignals() correctly signals ✓
- **Primitive 264** - Implemented and ready to return events ✓
- **Events accumulate** - 26+ events in passThroughEvents_, 99+ excess signals ✓

### The Gap

No Smalltalk process is waiting on the input semaphore to wake up and call primitive 264:
- OSiOSDriver would create this process but isn't in the image
- OSSDL2Driver expects FFI to work but it doesn't
- Result: semaphore signals are wasted, primitive 264 never called

### Solution Options

1. **File in OSiOSDriver.st at startup** (Recommended)
   - Implement autoLoadDriver() to file in OSiOSDriver.st
   - OSiOSDriver.install will create the event loop process
   - Process will poll primitive 264 and dispatch events

2. **Make OSSDL2Driver work via FFI**
   - Implement FFI primitives properly for SDL2 calls
   - Complex: requires full libffi integration

3. **Create event loop process from C++**
   - Would be a workaround (violates CLAUDE.md)
   - Complex: requires creating BlockClosure from C++

### Next Steps

Implement autoLoadDriver() to file in OSiOSDriver.st:
1. Read OSiOSDriver.st file from bundle/resources
2. Create a Compiler or ChunkFileReader to evaluate it
3. Or: Evaluate `'path/to/OSiOSDriver.st' asFileReference fileIn`
4. Then call `OSiOSDriver install` to start event loop
