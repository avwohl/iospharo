# WIP: Input Handling from Swift to VM

## Status: In Progress (2026-01-18)

**Current Problem:** World loop isn't running, so Morphic event handling doesn't happen.

### Key Discovery: World Loop Not Running

The Pharo world loop (`doOneCycleFor:`) is **never called**. Only the idle process runs:
- `idleProcess` sends `relinquishProcessorForMicroseconds:` repeatedly
- No world loop means no event processing, no step methods, no Hand updates

This explains why clicks don't work - there's no event processing happening at all.

### Updated Investigation Summary

1. **Events reach the VM correctly** - Mouse events flow from Swift → PlatformBridge → EventQueue → processInputEvents()

2. **World loop not running** - `doOneCycleFor:` is never sent. Selector tracking shows only `relinquishProcessorForMicroseconds:` being called.

3. **Action dispatch via idle loop** - We can intercept `relinquishProcessorForMicroseconds:` to dispatch pending actions (safe bytecode execution context).

4. **comeToFront causes DNU cascade** - When we send `comeToFront` to SpWindow:
   - Something internally calls `copyFrom:to:` on an object that doesn't understand it
   - That object also doesn't implement `doesNotUnderstand:`
   - DNU loop is detected, returning nil
   - But nil propagates and causes issues downstream

### Log Evidence
```
# selector_debug.log - only idle loop runs
[SEND #1] #idleProcess argCount=0
[SEND #2] #relinquishProcessorForMicroseconds: argCount=1
... (repeats)

# dnu_trace.log - DNU cascade from comeToFront
[DNU #1 depth=1] selector=#copyFrom:to: argCount=2
[DNU #2 depth=2] selector=#doesNotUnderstand: argCount=1

# dnu_stop.log - graceful termination
[DNU-GRACEFUL #1] DNU called with DNU selector - returning nil
```

### Changes Made

1. **Removed executeFromSync flag** - Was causing race condition where main loop cleared pending actions before processing

2. **Added idle loop interception** - Dispatch pending actions during `relinquishProcessorForMicroseconds:` (safe context)

3. **Made DNU-with-DNU graceful** - Return nil instead of stopping VM (prevents hard crash, but nil causes issues)

### Root Cause Analysis

The problem is architectural: we're trying to work around a non-running event loop by sending direct messages. This doesn't work because:
1. Some Pharo methods assume they're running in a proper Morphic context
2. Objects may not have proper error handling for edge cases
3. The call chain expects results that we can't provide

### Possible Solutions (Updated)

1. **Start the world loop** - Figure out why Morphic's world loop isn't starting and fix that. This is the proper solution but requires understanding Pharo 10's startup sequence.

2. **Direct Hand manipulation** - Instead of sending messages, directly write:
   - Mouse position to HandMorph's position slot
   - Button state to HandMorph's buttons slot
   - This bypasses message sending entirely

3. **Proper OSWindow integration** - Implement OSiOSDriver properly so it delivers events via the correct callback mechanism. Requires understanding OSWindow architecture.

4. **Skip complex actions** - For now, just skip comeToFront and similar complex messages. Focus on getting simpler interactions working first.

---
## Original Pipeline (2026-01-08)

Input handling pipeline from Swift gestures to Pharo VM was implemented as follows:

## What Was Done

### 1. C++ EventQueue (`src/platform/`)
- Thread-safe event queue with mutex protection
- Event types: Mouse (1), Keyboard (2), WindowMetrics (6), MouseWheel (7)
- Callback mechanism for semaphore signaling
- C interface via PlatformBridge for Swift interop

### 2. VM Primitives (`src/vm/Primitives.cpp`)
- **Primitive 264** (`primitiveGetNextEvent`): Dequeues events from gEventQueue, fills 8-slot Smalltalk array
- **Primitive 265** (`primitiveInputSemaphore2`): Registers semaphore index for async notification
- **Primitive 266** (`primitiveEventProcessingControl`): Query/enable/disable/flush events

### 3. External Semaphore Signaling (`src/vm/Interpreter.cpp`)
- `signalExternalSemaphore(index)`: Thread-safe pending signal storage
- `processPendingSignals()`: Called in interpret loop, wakes waiting processes
- Uses ExternalSemaphoreTable (special object index 38)

### 4. Swift Bridge Updates
- `PharoBridge.swift`: Uses `vm_postMouseEvent`, `vm_postKeyEvent`, `vm_postScrollEvent`
- `PharoCanvasView.swift`: Gesture recognizers for all input types

## Gesture Mapping

| iOS Gesture | Pharo Event |
|-------------|-------------|
| Single tap | Left click (red button) |
| Double tap | Double click |
| Single-finger pan | Drag with left button |
| Two-finger pan | Scroll wheel (deltaX, deltaY) |
| Two-finger tap | Middle click (yellow button) |
| Long press | Right click (blue button) |
| Pinch | Zoom (Cmd + scroll wheel) |

## Event Flow

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
processPendingSignals() → signals semaphore from ExternalSemaphoreTable
    ↓
Pharo InputEventSensor process wakes up
    ↓
primitiveGetNextEvent (264) dequeues event
    ↓
Pharo event handling (HandMorph, etc.)
```

## Event Buffer Format (8 slots)

**Mouse (type 1):**
- [0] type=1, [1] timestamp, [2] x, [3] y, [4] buttons, [5] modifiers, [6] reserved, [7] windowIndex

**Keyboard (type 2):**
- [0] type=2, [1] timestamp, [2] charCode, [3] pressCode, [4] modifiers, [5] keyCode, [6] reserved, [7] windowIndex

**MouseWheel (type 7):**
- [0] type=7, [1] timestamp, [2] x, [3] y, [4] deltaX, [5] deltaY, [6] modifiers, [7] windowIndex

## Files Modified

- `src/platform/EventQueue.hpp/cpp` - Event queue implementation
- `src/platform/PlatformBridge.h/cpp` - C interface for Swift
- `src/vm/Interpreter.hpp/cpp` - External semaphore signaling
- `src/vm/ObjectMemory.hpp` - ExternalSemaphoreTable index
- `src/vm/Primitives.cpp` - Event primitives
- `iospharo/Bridge/PharoBridge.swift` - Swift event methods
- `iospharo/Bridge/iospharo-Bridging-Header.h` - C declarations
- `iospharo/Views/PharoCanvasView.swift` - Gesture handlers

## Next Steps

- Test with actual Pharo image on iOS
- Verify InputEventSensor picks up events correctly
- May need to adjust event format for specific Pharo expectations
- Consider adding drag-and-drop events if needed
