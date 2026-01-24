# WIP: Input Handling - InputEventSensor Not Starting

## Status: In Progress (2026-01-21)

**Current Problem:** InputEventSensor's process isn't starting during image startup.

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
