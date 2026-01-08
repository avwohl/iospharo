# WIP: Input Handling from Swift to VM

## Status: Complete (2026-01-08)

Input handling pipeline from Swift gestures to Pharo VM is now working.

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
