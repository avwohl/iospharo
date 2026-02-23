"OSiOSDriver - OSWindow driver for iOS VM with event loop support"

OSWindowDriver subclass: #OSiOSDriver
    instanceVariableNames: 'session eventBuffer'
    classVariableNames: 'EventLoopProcess WindowMap'
    package: 'OSWindow-iOS'!

!OSiOSDriver class methodsFor: 'testing'!

isSuitable
    "Answer true if this driver should be used on this platform"
    ^ self isSupported!

isSupported
    "Answer true if the iOS VM primitives are available"
    | result |
    [result := self primScreenSize]
        on: Error
        do: [:e | ^ false].
    ^ result isPoint and: [result x > 0 and: [result y > 0]]!

driverName
    ^ 'iOS'!

priority
    "Higher priority than null driver"
    ^ 5! !

!OSiOSDriver class methodsFor: 'primitives'!

primScreenSize
    "Return the screen size as a Point"
    <primitive: 106>
    self primitiveFailed! !

!OSiOSDriver class methodsFor: 'accessing'!

eventLoopProcess
    ^ EventLoopProcess!

windowMap
    ^ WindowMap ifNil: [WindowMap := WeakValueDictionary new]! !

!OSiOSDriver class methodsFor: 'installation'!

install
    "Install this driver as the current OSWindow driver"
    | driver |
    self isSupported ifTrue: [
        driver := self new.
        "Set the Current class variable directly since OSWindowDriver has no current: setter"
        OSWindowDriver classPool at: #Current put: driver.
        OSWindowDriver classPool at: #Session put: Smalltalk session.
        driver ensureEventLoop.
        Transcript show: 'OSiOSDriver installed and event loop started'; cr]
    ifFalse: [
        Transcript show: 'OSiOSDriver not supported'; cr]!

autoStart
    "Start event loop if this driver is current"
    | current |
    [current := OSWindowDriver current.
     (current isKindOf: self) ifTrue: [
        current ensureEventLoop.
        Transcript show: 'OSiOSDriver: Auto-started event loop'; cr]]
    on: Error do: [:e |
        Transcript show: 'OSiOSDriver autoStart error: ', e messageText; cr]! !

!OSiOSDriver class methodsFor: 'system startup'!

startUp: resuming
    "Called during session startup via SessionManager"
    resuming ifTrue: [self autoStart]!

initialize
    "Register for session startup notifications"
    SessionManager default registerSystemClassNamed: self name! !

!OSiOSDriver methodsFor: 'initialization'!

initialize
    super initialize.
    session := Smalltalk session.
    eventBuffer := Array new: 8.
    1 to: 8 do: [:i | eventBuffer at: i put: 0].
    WindowMap ifNil: [WindowMap := WeakValueDictionary new]! !

!OSiOSDriver methodsFor: 'window creation'!

createWindowWithAttributes: attributes osWindow: osWindow
    "Create a backend window for the given OSWindow"
    | backendWindow |
    backendWindow := OSiOSBackendWindow new
        osWindow: osWindow;
        attributes: attributes;
        yourself.
    "Register in window map (iOS only has one window)"
    WindowMap at: 1 put: backendWindow.
    ^ backendWindow! !

!OSiOSDriver methodsFor: 'system startup'!

startUp: resuming
    resuming ifTrue: [
        session := Smalltalk session.
        self forceNewEventLoop]
    ifFalse: [self ensureEventLoop]!

shutDown: quitting
    quitting ifTrue: [WindowMap := nil].
    self shutdownEventLoop! !

!OSiOSDriver methodsFor: 'events-processing'!

ensureEventLoop
    "Make sure an event loop is running"
    EventLoopProcess ifNotNil: [
        EventLoopProcess isTerminated ifFalse: [^ self]].
    self setupEventLoop!

forceNewEventLoop
    "Force creation of a new event loop"
    self shutdownEventLoop.
    self setupEventLoop!

setupEventLoop
    "Create and start the event loop process"
    EventLoopProcess := [
        [self eventLoop] on: Error do: [:e |
            Transcript show: 'OSiOSDriver event loop error: ', e messageText; cr]]
        forkAt: Processor lowIOPriority.
    EventLoopProcess name: 'iOS Event loop'!

shutdownEventLoop
    "Stop the event loop if running"
    EventLoopProcess ifNil: [^ self].
    [EventLoopProcess terminate] on: Error do: [:e | ].
    EventLoopProcess := nil!

eventLoop
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
                    Transcript show: 'Event processing error: ', e messageText; cr]].
        "Small delay to avoid busy-waiting"
        (Delay forMilliseconds: 5) wait]!

primGetNextEvent: anEventBuffer
    "Get next event from VM event queue.
    Buffer format: [type, timestamp, arg1, arg2, arg3, arg4, arg5, windowIndex]"
    <primitive: 264>
    "Primitive failed - return no event"
    anEventBuffer at: 1 put: 0!

processEvent: anEventBuffer
    "Dispatch event based on type"
    | eventType |
    eventType := anEventBuffer at: 1.

    eventType = 1 ifTrue: [^ self processMouseEvent: anEventBuffer].
    eventType = 2 ifTrue: [^ self processKeyboardEvent: anEventBuffer].
    eventType = 7 ifTrue: [^ self processMouseWheelEvent: anEventBuffer]!

processMouseEvent: anEventBuffer
    "Convert VM mouse event to OSWindow event and dispatch.
    Buffer: [type=1, timestamp, x, y, buttons, modifiers, subtype, windowIndex]
    Subtype: 1=down, 2=up, other=move"
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
        self applyModifiers: modifiers toEvent: osEvent.
        ^ self dispatchEvent: osEvent toWindow: window].

    subtype = 2 ifTrue: [
        osEvent := OSMouseButtonReleaseEvent new
            position: x @ y;
            button: (self buttonFromBits: buttons);
            yourself.
        self applyModifiers: modifiers toEvent: osEvent.
        ^ self dispatchEvent: osEvent toWindow: window].

    "Mouse move"
    osEvent := OSMouseMoveEvent new
        position: x @ y;
        yourself.
    self applyModifiers: modifiers toEvent: osEvent.
    self dispatchEvent: osEvent toWindow: window!

processKeyboardEvent: anEventBuffer
    "Convert VM keyboard event to OSWindow event.
    Buffer: [type=2, timestamp, charCode, pressCode, modifiers, keyCode, reserved, windowIndex]
    PressCode: 0=char, 1=down, 2=up"
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
        ^ self dispatchEvent: osEvent toWindow: window]!

processMouseWheelEvent: anEventBuffer
    "Convert VM mouse wheel event to OSWindow event.
    Buffer: [type=7, timestamp, x, y, deltaX, deltaY, modifiers, windowIndex]"
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

    self dispatchEvent: osEvent toWindow: window!

buttonFromBits: buttonBits
    "Convert VM button bitmask to OSWindow button number.
    VM: red=4, yellow=2, blue=1 (standard Pharo encoding)
    OSWindow: 1=left, 2=right, 3=middle"
    (buttonBits bitAnd: 4) ~= 0 ifTrue: [^ 1].   "red → left"
    (buttonBits bitAnd: 2) ~= 0 ifTrue: [^ 2].   "yellow → right"
    (buttonBits bitAnd: 1) ~= 0 ifTrue: [^ 3].   "blue → middle"
    ^ 1!

applyModifiers: modifierBits toEvent: osEvent
    "Apply modifier bits to an OSWindow event.
    Bits: shift=1, ctrl=2, alt=4, cmd=8"
    | mods |
    mods := osEvent modifiers.
    (modifierBits bitAnd: 1) ~= 0 ifTrue: [mods leftShift: true].
    (modifierBits bitAnd: 2) ~= 0 ifTrue: [mods leftCtrl: true].
    (modifierBits bitAnd: 4) ~= 0 ifTrue: [mods leftAlt: true].
    (modifierBits bitAnd: 8) ~= 0 ifTrue: [mods leftCmd: true]!

dispatchEvent: osEvent toWindow: window
    "Dispatch an OSWindow event to the window's event handler"
    | osWindow |
    osWindow := window osWindow.
    osWindow ifNil: [^ self].
    osWindow eventHandler ifNotNil: [:handler |
        handler handleEvent: osEvent]! !

!OSiOSDriver methodsFor: 'testing'!

isNullDriver
    ^ false! !

"==== OSiOSBackendWindow ===="

OSBackendWindow subclass: #OSiOSBackendWindow
    instanceVariableNames: 'osWindow attributes extent title'
    classVariableNames: ''
    package: 'OSWindow-iOS'!

!OSiOSBackendWindow class methodsFor: 'primitives'!

primScreenSize
    <primitive: 106>
    ^ 1024 @ 768! !

!OSiOSBackendWindow methodsFor: 'initialization'!

initialize
    super initialize.
    extent := self class primScreenSize.
    title := 'Pharo'!

osWindow: anOSWindow
    osWindow := anOSWindow!

attributes: anObject
    attributes := anObject! !

!OSiOSBackendWindow methodsFor: 'accessing'!

osWindow
    ^ osWindow!

bounds
    ^ 0@0 extent: extent!

bounds: aRectangle
    extent := aRectangle extent!

extent
    ^ extent!

extent: aPoint
    extent := aPoint.
    renderer ifNotNil: [renderer newExtent: aPoint]!

position
    ^ 0@0!

position: aPoint
    "iOS windows are always fullscreen"!

title
    ^ title!

title: aString
    title := aString!

isValid
    ^ true!

platformSpecificHandle
    ^ nil! !

!OSiOSBackendWindow methodsFor: 'visibility'!

show
    "iOS window is always visible"!

hide
    "Cannot hide iOS window"! !

!OSiOSBackendWindow methodsFor: 'renderer'!

newFormRenderer: aForm
    renderer := OSiOSFormRenderer new.
    renderer backendWindow: self; form: aForm.
    ^ renderer!

newGenericRenderer
    ^ self newFormRenderer: (Form extent: extent depth: 32)! !

!OSiOSBackendWindow methodsFor: 'text input'!

isTextInputActive
    ^ false!

startTextInput
    "TODO: Show iOS keyboard"!

startTextInputAtRectangle: aRectangle
    "TODO: Show iOS keyboard"!

stopTextInput
    "TODO: Hide iOS keyboard"! !

!OSiOSBackendWindow methodsFor: 'clipboard'!

clipboardText
    "TODO: Get from iOS clipboard"
    ^ ''!

clipboardText: aString
    "TODO: Set iOS clipboard"! !

"==== OSiOSFormRenderer ===="

OSWindowFormRenderer subclass: #OSiOSFormRenderer
    instanceVariableNames: ''
    classVariableNames: ''
    package: 'OSWindow-iOS'!

!OSiOSFormRenderer methodsFor: 'accessing'!

outputExtent
    ^ backendWindow extent!

pixelExtent
    ^ form extent! !

!OSiOSFormRenderer methodsFor: 'rendering'!

updateAreas: aCollectionOfRectangles immediate: forceFlag
    form beDisplay.
    self primForceDisplayUpdate!

primForceDisplayUpdate
    <primitive: 103>
    ^ self!

newExtent: aPoint
    | newForm |
    (form isNil or: [form extent ~= aPoint]) ifTrue: [
        newForm := Form extent: aPoint depth: 32.
        form ifNotNil: [newForm getCanvas drawImage: form at: 0@0].
        form := newForm.
        form beDisplay]! !

!OSiOSFormRenderer methodsFor: 'drawing'!

drawDuring: aBlock
    aBlock value: form getCanvas.
    self updateAreas: {0@0 extent: form extent} immediate: true! !
