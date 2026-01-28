"Load OSiOSDriver with full event loop support"
"This script loads the event handling methods that are missing from the image's stub OSiOSDriver"

Transcript cr; show: 'Loading OSiOSDriver event loop methods...'.

"Add instance variables if not present"
(OSiOSDriver instVarNames includes: 'session') ifFalse: [
    OSiOSDriver addInstVarNamed: 'session'.
    OSiOSDriver addInstVarNamed: 'eventBuffer' ].

"Add class variables if not present"
(OSiOSDriver classVarNames includes: 'EventLoopProcess') ifFalse: [
    OSiOSDriver addClassVarNamed: 'EventLoopProcess'.
    OSiOSDriver addClassVarNamed: 'WindowMap' ].

"Event loop process accessor"
OSiOSDriver class compile: 'eventLoopProcess
    ^ EventLoopProcess'.

"Window map accessor"
OSiOSDriver class compile: 'windowMap
    ^ WindowMap ifNil: [WindowMap := WeakValueDictionary new]'.

"Instance initialize"
OSiOSDriver compile: 'initialize
    super initialize.
    session := Smalltalk session.
    eventBuffer := Array new: 8.
    1 to: 8 do: [:i | eventBuffer at: i put: 0].
    WindowMap ifNil: [WindowMap := WeakValueDictionary new]'.

"Session startup"
OSiOSDriver compile: 'startUp: resuming
    resuming ifTrue: [
        session := Smalltalk session.
        self forceNewEventLoop]
    ifFalse: [self ensureEventLoop]'.

"Ensure event loop is running"
OSiOSDriver compile: 'ensureEventLoop
    EventLoopProcess ifNotNil: [
        EventLoopProcess isTerminated ifFalse: [^ self]].
    self setupEventLoop'.

"Force new event loop"
OSiOSDriver compile: 'forceNewEventLoop
    self shutdownEventLoop.
    self setupEventLoop'.

"THE KEY METHOD: Create and start the event loop process"
OSiOSDriver compile: 'setupEventLoop
    "Create and start the event loop process"
    EventLoopProcess := [
        [self eventLoop] on: Error do: [:e |
            Transcript show: ''OSiOSDriver event loop error: '', e messageText; cr]]
        forkAt: Processor lowIOPriority.
    EventLoopProcess name: ''iOS Event loop'''.

"Shutdown event loop"
OSiOSDriver compile: 'shutdownEventLoop
    EventLoopProcess ifNil: [^ self].
    [EventLoopProcess terminate] on: Error do: [:e | ].
    EventLoopProcess := nil'.

"Main event loop - polls primitive 264"
OSiOSDriver compile: 'eventLoop
    "Main event loop - polls primitive 264 for events"
    | currentSession eventType |
    currentSession := Smalltalk session.

    [currentSession == Smalltalk session] whileTrue: [
        "Poll for events"
        [self primGetNextEvent: eventBuffer.
         eventType := eventBuffer at: 1.
         eventType ~= 0] whileTrue: [
            [self processEvent: eventBuffer copy]
                on: Error do: [:e |
                    Transcript show: ''Event processing error: '', e messageText; cr]].
        "Small delay to avoid busy-waiting"
        (Delay forMilliseconds: 5) wait]'.

"Primitive 264: Get next event"
OSiOSDriver compile: 'primGetNextEvent: anEventBuffer
    "Get next event from VM event queue"
    <primitive: 264>
    anEventBuffer at: 1 put: 0'.

"Process event based on type"
OSiOSDriver compile: 'processEvent: anEventBuffer
    | eventType |
    eventType := anEventBuffer at: 1.
    eventType = 1 ifTrue: [^ self processMouseEvent: anEventBuffer].
    eventType = 2 ifTrue: [^ self processKeyboardEvent: anEventBuffer].
    eventType = 7 ifTrue: [^ self processMouseWheelEvent: anEventBuffer]'.

"Process mouse event"
OSiOSDriver compile: 'processMouseEvent: anEventBuffer
    | window x y buttons modifiers subtype osEvent |
    x := anEventBuffer at: 3.
    y := anEventBuffer at: 4.
    buttons := anEventBuffer at: 5.
    modifiers := anEventBuffer at: 6.
    subtype := anEventBuffer at: 7.

    window := WindowMap at: 1 ifAbsent: [^ self].

    subtype = 1 ifTrue: [
        osEvent := OSMouseButtonPressEvent new
            position: x @ y;
            button: (self buttonFromBits: buttons);
            yourself.
        ^ self dispatchEvent: osEvent toWindow: window].

    subtype = 2 ifTrue: [
        osEvent := OSMouseButtonReleaseEvent new
            position: x @ y;
            button: (self buttonFromBits: buttons);
            yourself.
        ^ self dispatchEvent: osEvent toWindow: window].

    osEvent := OSMouseMoveEvent new
        position: x @ y;
        yourself.
    self dispatchEvent: osEvent toWindow: window'.

"Process keyboard event"
OSiOSDriver compile: 'processKeyboardEvent: anEventBuffer
    | window pressCode keyCode osEvent |
    pressCode := anEventBuffer at: 4.
    keyCode := anEventBuffer at: 6.

    window := WindowMap at: 1 ifAbsent: [^ self].

    pressCode = 1 ifTrue: [
        osEvent := OSKeyDownEvent new
            symbol: keyCode;
            scanCode: keyCode;
            yourself.
        ^ self dispatchEvent: osEvent toWindow: window].

    pressCode = 2 ifTrue: [
        osEvent := OSKeyUpEvent new
            symbol: keyCode;
            scanCode: keyCode;
            yourself.
        ^ self dispatchEvent: osEvent toWindow: window]'.

"Process mouse wheel event"
OSiOSDriver compile: 'processMouseWheelEvent: anEventBuffer
    | window x y deltaX deltaY osEvent |
    x := anEventBuffer at: 3.
    y := anEventBuffer at: 4.
    deltaX := anEventBuffer at: 5.
    deltaY := anEventBuffer at: 6.

    window := WindowMap at: 1 ifAbsent: [^ self].

    osEvent := OSMouseWheelEvent new
        position: x @ y;
        scrollHorizontal: deltaX;
        scrollVertical: deltaY;
        yourself.

    self dispatchEvent: osEvent toWindow: window'.

"Convert button bits to button number"
OSiOSDriver compile: 'buttonFromBits: buttonBits
    (buttonBits bitAnd: 1) ~= 0 ifTrue: [^ 1].
    (buttonBits bitAnd: 2) ~= 0 ifTrue: [^ 2].
    (buttonBits bitAnd: 4) ~= 0 ifTrue: [^ 3].
    ^ 1'.

"Dispatch event to window"
OSiOSDriver compile: 'dispatchEvent: osEvent toWindow: window
    | osWindow |
    osWindow := window osWindow.
    osWindow ifNil: [^ self].
    osWindow eventHandler ifNotNil: [:handler |
        handler handleEvent: osEvent]'.

Transcript cr; show: 'OSiOSDriver event loop methods loaded!'.
Transcript cr; show: 'Starting event loop...'.

"Start the event loop on the current driver if it''s OSiOSDriver"
[
    | current |
    current := OSWindowDriver current.
    (current isKindOf: OSiOSDriver) ifTrue: [
        current ensureEventLoop.
        Transcript cr; show: 'Event loop started for OSiOSDriver!' ]
] on: Error do: [:e |
    Transcript cr; show: 'Could not start event loop: ', e messageText ].

Transcript cr; show: 'Done.'.
