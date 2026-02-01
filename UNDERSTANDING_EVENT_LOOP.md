# Understanding Pharo 13 Event Loop Architecture

## Summary: OSiOSDriver Does NOT Exist in Standard Pharo

The standard Pharo 13 image does NOT include `OSiOSDriver`. We need to create it from scratch.

## Event Loop Architecture in Pharo 13

### Process Hierarchy

1. **OSSDL2Driver (or OSiOSDriver)** - Event Loop Process (priority 60, LowIOPriority)
   - Separate process that runs indefinitely
   - Fetches events from the system (SDL2 or primitive 264)
   - Dispatches events to windows

2. **MorphicRenderLoop** - Main UI Loop (priority varies)
   - Runs in main Pharo process
   - Calls WorldState >> #doOneCycleFor: repeatedly
   - Processes pending events, runs step methods, updates display

### OSSDL2Driver Structure

**Inheritance:** OSWindowDriver > OSSDL2Driver

**Instance Variables:**
- `inputSemaphore` (inherited from OSWindowDriver)
- `globalListeners` (inherited from OSWindowDriver)
- `eventFilter` (inherited from OSWindowDriver)

**Class Variables:**
- `EventLoopProcess` - The event loop process
- `JoystickMap` - Maps joystick IDs
- `WindowMap` - Maps window IDs to OSWindow objects
- `WindowMapMutex` - Synchronization

**Key Methods:**

1. **setupEventLoop** (no parameters)
   ```smalltalk
   setupEventLoop
       EventLoopProcess := [[self eventLoop] on: Error do: [:e | ...]] 
           forkAt: Processor lowIOPriority.
       EventLoopProcess name: 'SDL2 Event loop'
   ```
   - Creates and starts the event loop process
   - Priority is 60 (Processor lowIOPriority in Pharo 13)

2. **eventLoop** (runs in separate process)
   ```smalltalk
   eventLoop
       | event session |
       event := SDL_Event new.
       session := Smalltalk session.
       [session == Smalltalk session] whileTrue: [
           [(SDL2 pollEvent: event) > 0] whileTrue: [
               self processEvent: event
           ].
           (Delay forMilliseconds: 5) wait
       ]
   ```
   - Infinite loop in separate process
   - Polls for events
   - Waits 5ms between polls

3. **processEvent:sdlEvent**
   ```smalltalk
   processEvent: sdlEvent
       | mappedEvent |
       [mappedEvent := sdlEvent mapped.
        self evaluateUserInterrupt: mappedEvent.
        self eventFilter dispatchEvent: mappedEvent
       ] on: UnhandledException do: [:err | ...]
   ```
   - Maps SDL event to Pharo event
   - Dispatches through event filter
   - Has error handling to keep loop running

### How Events Reach HandMorph

The event flow diagram:

```
[Event Loop Process (priority 60)]
    |
    +-> OSSDL2Driver >> eventLoop
         |
         +-> (SDL2 pollEvent:)    [or primitive 264 for OSiOSDriver]
             |
             +-> OSSDL2Driver >> processEvent:
                  |
                  +-> eventFilter >> dispatchEvent:
                       |
                       +-> OSSDL2BackendWindow >> eventHandler >> handleEvent:
                            |
                            +-> HandMorph >> pendingEventQueue [enqueued]

[Main Render Loop (higher priority)]
    |
    +-> MorphicRenderLoop >> doOneCycleWhile:
         |
         +-> WorldState >> doOneCycleFor:
              |
              +-> HandMorph >> processEvents [pulls from queue]
                   |
                   +-> HandMorph >> handleEvent:
                        |
                        +-> Morph event handling
```

**Key: Two-stage event processing**
- Stage 1: Event loop process fetches from system -> puts in HandMorph.pendingEventQueue
- Stage 2: Render loop process pulls from queue -> processes morphic event handling

## OSiOSDriver Requirements

To replace OSSDL2Driver with OSiOSDriver:

1. **Inherit from OSWindowDriver**
   - Get inputSemaphore, globalListeners, eventFilter from parent

2. **Add instance variables (beyond inherited ones):**
   - May need: `session`, `eventBuffer` (for primitive 264 result)

3. **Add class variables:**
   - `EventLoopProcess`
   - `WindowMap`
   - `WindowMapMutex` (optional, for thread safety)

4. **Implement required methods:**
   - `setupEventLoop` - fork event loop process at priority 60
   - `eventLoop` - main loop calling primitive 264
   - `shutdownEventLoop` - cleanup
   - `startUp:` and `shutDown:` - lifecycle hooks
   - `isNullDriver` - return false

5. **Event processing:**
   - `processEvent:anEventBuffer` - dispatch received event
   - `processMouseEvent:`, `processKeyboardEvent:`, etc. - type-specific handlers

6. **Event dispatching:**
   - Use inherited `eventFilter` to route events
   - Or directly enqueue into HandMorph.pendingEventQueue

## Primitive 264 (getNextEvent)

Implemented in our custom VM. Returns event data in array/buffer format:
- `eventBuffer at: 1` = event type (0=none, 1=mouse, 2=keyboard, 7=wheel)
- `eventBuffer at: 2` = timestamp (unused)
- `eventBuffer at: 3` = x position (mouse) or key code
- `eventBuffer at: 4` = y position (mouse) or key state
- `eventBuffer at: 5` = button bits (mouse) or key repeat
- `eventBuffer at: 6` = key code
- `eventBuffer at: 7` = event subtype

## Priority Values in Pharo 13

```
Processor lowIOPriority          = 60
Processor userSchedulingPriority = 50
Processor userBackgroundPriority = 30
Processor systemBackgroundPriority = 10
```

Event loop should run at priority 60 (lowIOPriority).

## Important Implementation Notes

### Why OSSDL2Driver Can't Work (Without FFI Fix)

OSSDL2Driver uses:
```smalltalk
(SDL2 pollEvent: event) > 0
```

This requires:
1. SDL2 FFI library bound
2. SDL_Event structure type resolution working
3. FFI callout mechanism functional

If FFI type resolution is broken, OSSDL2Driver fails.

### OSiOSDriver Advantage

OSiOSDriver bypasses FFI entirely:
1. Uses primitive 264 directly
2. VM handles the event fetching
3. Pharo just dispatches the primitive result
4. No FFI type system dependencies

### Event Queue vs Direct Dispatch

**HandMorph.pendingEventQueue:**
- Queue is an OrderedCollection
- Added to by OSSDL2Driver >> dispatchEvent:
- Drained by HandMorph >> processEvents during render cycle
- Provides batching and consistency with render loop timing

## Detailed Method Signatures

From Pharo 13 reflection:

```
OSWindowDriver >> #setupEventLoop
OSWindowDriver >> #eventLoop
OSWindowDriver >> #shutdownEventLoop
OSWindowDriver >> #startUp:
OSWindowDriver >> #shutDown:
OSWindowDriver >> #isNullDriver
OSWindowDriver >> #eventLoopProcess
OSWindowDriver >> #eventFilter  [accessor for eventFilter instance variable]
```

Each must be implemented in OSiOSDriver or inherited/overridden.

## Next Steps for Implementation

1. Create OSiOSDriver class inheriting from OSWindowDriver
   - With proper instance variables: session, eventBuffer
   - With proper class variables: EventLoopProcess, WindowMap

2. Implement setupEventLoop
   - Fork [ self eventLoop ] at Processor lowIOPriority
   - Set EventLoopProcess

3. Implement eventLoop
   - Loop: [Smalltalk session == currentSession] whileTrue:
   - Call: self primGetNextEvent: eventBuffer
   - Process non-zero events
   - Delay 5ms between iterations

4. Implement primitive 264 wrapper
   - `primGetNextEvent: anEventBuffer <primitive: 264>`
   - Fallback: set buffer[0] = 0

5. Implement processEvent:anEventBuffer
   - Dispatch based on event type
   - Route through eventFilter or directly to windows

6. Test event flow end-to-end
   - Verify event loop process starts at priority 60
   - Verify events appear in HandMorph.pendingEventQueue
   - Verify HandMorph.processEvents consumes them

