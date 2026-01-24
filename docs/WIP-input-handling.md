# WIP: Input Handling - InputEventSensor Not Starting

## Status: In Progress (2026-01-24)

**Current Problem:** VM slows dramatically after ~24k bytecode steps during startup.

## Latest Debug Session (2026-01-24)

### Performance Issue
- Steps 0-20000: Fast (~17ms total, ~1M steps/sec)
- Steps 20000-24000: Slow (~30 seconds for 4k steps)
- Caused by extensive debug logging in hot paths

### Fixed
- Removed 200+ lines of debug logging from hot paths
- Disabled renderWorldMorphs() call in heartbeat (expensive morph tree walk)
- Removed event queue push/pop logging
- Still 92 fopen calls remaining in Interpreter.cpp

### Remaining Issue
VM still slows after ~24k steps. May need:
1. Bulk disable remaining 92 log file opens
2. Or investigate actual performance bug (not just logging)

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
