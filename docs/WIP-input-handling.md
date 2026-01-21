# WIP: Input Handling from Swift to VM

## Status: In Progress (2026-01-21)

**Current Problem:** InputEventSensor's process isn't starting during image startup.

### All C++ Workarounds Removed (2026-01-21)

Per CLAUDE.md policy: NO WORKAROUNDS. All C++ event dispatch code has been removed:

- `dispatchMouseEventToMorph` - C++ hit testing that bypassed InputEventSensor
- `handleMenuBarClick` - C++ menu bar click handling
- `handleWorldMenuClick` - C++ world menu invocation
- `executeMenuItemAction` - C++ menu action execution
- `processPendingMenuAction` - C++ pending action processing
- `processPendingWorldMenu` - C++ world menu processing
- `drawClickIndicator` - C++ visual click feedback
- `lookupMethodByName` - helper for C++ method lookup
- `updateActiveHandPosition` - direct Hand bounds manipulation
- Direct window-to-front array manipulation

**Events MUST be handled by Smalltalk's InputEventSensor and Morphic.**
If events aren't working, fix InputEventSensor startup - don't add C++ workarounds.

### Root Cause: InputEventSensor Process Not Running

The Pharo world loop (`doOneCycleFor:`) is **never called**. Only the idle process runs:
- `idleProcess` sends `relinquishProcessorForMicroseconds:` repeatedly
- No world loop means no event processing, no step methods, no proper Hand updates

InputEventSensor process never wakes up:
- Events are pushed to queue and semaphore is signaled
- But no process is waiting on the semaphore (excessSignals increments instead)
- Primitive 264 (getNextEvent) is never called

### Investigation Summary

1. **Events reach the VM correctly** - Mouse events flow from Swift → PlatformBridge → EventQueue → processInputEvents()

2. **World loop runs but InputEventSensor doesn't start** - `doOneCycleFor:` runs, but the event processing process is missing

3. **Only 3 processes active** - Delay scheduler (priority 80), idle process (priority 10), and transient processes

4. **Input semaphore accumulates signals** - excessSignals grows to 99+ with no consumer

### Log Evidence
```
# process_switch.log - only 3 processes
[XFER #1] old=0x41032b890 new=0x40fc442e8
  newProc will resume in method: #suspendAtTimingPriority

# selector_debug.log - world loop runs but no event processing
[SEND #1] #doOneCycle argCount=0
[SEND #12] #processEvents argCount=0  <-- called but no events consumed
[SEND #19] #idleProcess argCount=0
[SEND #20] #relinquishProcessorForMicroseconds: argCount=1
```

### Next Steps to Fix InputEventSensor

1. **Investigate SessionManager>>startUp:** - Does it complete? Does it initialize UI properly?
2. **Check MorphicCoreUIManager initialization** - This creates the world loop
3. **Verify EventSensor>>startUp:** - This should fork the event polling process
4. **Check InputEventSensor class>>installEventSensorFramework** - Event infrastructure setup
5. **Look at OSWindow/OSiOSDriver integration** - May need proper driver initialization
   - Avoids DNU cascade entirely

### Root Cause Analysis

The problem is architectural: we're trying to work around a non-running event loop by sending direct messages. This doesn't work because:
1. Some Pharo methods assume they're running in a proper Morphic context
2. Objects may not have proper error handling for edge cases
3. The call chain expects results that we can't provide

### Possible Solutions (Updated)

1. **Start the world loop** - Figure out why Morphic's world loop isn't starting and fix that. This is the proper solution but requires understanding Pharo 10's startup sequence.

2. **Direct Hand manipulation** - Instead of sending messages, directly write:
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
