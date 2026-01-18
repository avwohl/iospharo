"OSiOSDriver - OSWindow driver for iOS VM with event loop support"

OSWindowDriver subclass: #OSiOSDriver
    instanceVariableNames: 'session eventBuffer'
    classVariableNames: 'EventLoopProcess WindowMap'
    package: 'OSWindow-iOS'!

!OSiOSDriver class methodsFor: 'testing'!

isSuitable
    ^ self isSupported!

isSupported
    | result |
    [result := self primScreenSize]
        on: Error
        do: [:e | ^ false].
    ^ result isPoint and: [result x > 0 and: [result y > 0]]!

driverName
    ^ 'iOS'!

priority
    ^ 5! !

!OSiOSDriver class methodsFor: 'primitives'!

primScreenSize
    <primitive: 106>
    self primitiveFailed! !

!OSiOSDriver class methodsFor: 'accessing'!

eventLoopProcess
    ^ EventLoopProcess!

windowMap
    ^ WindowMap ifNil: [WindowMap := WeakValueDictionary new]! !

!OSiOSDriver methodsFor: 'initialization'!

initialize
    super initialize.
    session := Smalltalk session.
    eventBuffer := Array new: 8.
    WindowMap ifNil: [WindowMap := WeakValueDictionary new]! !

!OSiOSDriver methodsFor: 'window creation'!

createWindowWithAttributes: attributes osWindow: osWindow
    | backendWindow |
    backendWindow := OSiOSBackendWindow new
        osWindow: osWindow;
        attributes: attributes;
        yourself.
    "Register the window"
    WindowMap at: 1 put: backendWindow.
    ^ backendWindow! !

!OSiOSDriver methodsFor: 'system startup'!

startUp: resuming
    Transcript show: 'OSiOSDriver>>startUp: ', resuming printString; cr.
    resuming
        ifTrue: [
            session := Smalltalk session.
            self forceNewEventLoop]
        ifFalse: [self ensureEventLoop].
    Transcript show: 'OSiOSDriver event loop started'; cr!

shutDown: quitting
    Transcript show: 'OSiOSDriver>>shutDown: ', quitting printString; cr.
    quitting ifTrue: [WindowMap := nil].
    self shutdownEventLoop! !

!OSiOSDriver methodsFor: 'events-processing'!

ensureEventLoop
    "Make sure an event loop is running.
    If there is already an event loop, do nothing"
    EventLoopProcess ifNotNil: [^ self].
    self setupEventLoop!

forceNewEventLoop
    "Force the (re)creation of an event loop.
    If one was available, shut it down first."
    self shutdownEventLoop.
    self setupEventLoop!

setupEventLoop
    "Create a new event loop process"
    EventLoopProcess ifNotNil: [self error: 'Cannot launch new event loop'].
    Transcript show: 'OSiOSDriver: Setting up event loop'; cr.
    EventLoopProcess := [self eventLoop]
        forkAt: Processor lowIOPriority.
    EventLoopProcess name: 'iOS Event loop'!

shutdownEventLoop
    "Kills the current event loop if any"
    EventLoopProcess ifNil: [^ self].
    Transcript show: 'OSiOSDriver: Shutting down event loop'; cr.
    EventLoopProcess terminate.
    EventLoopProcess := nil!

eventLoop
    "The main event loop - polls primitive 264 and converts events"
    | eventType |
    Transcript show: 'OSiOSDriver: Event loop starting'; cr.
    [session == Smalltalk session] whileTrue: [
        "Poll for events using primitive 264"
        [self primGetNextEvent: eventBuffer.
         eventType := eventBuffer at: 1.
         eventType ~= 0] whileTrue: [
            self processEvent: eventBuffer copy].
        "Small delay to avoid spinning"
        (Delay forMilliseconds: 5) wait].
    Transcript show: 'OSiOSDriver: Event loop exiting (session changed)'; cr!

primGetNextEvent: anEventBuffer
    "Get next event from VM event queue"
    <primitive: 264>
    "If primitive fails, return no-event"
    anEventBuffer at: 1 put: 0!

processEvent: anEventBuffer
    "Process a raw event buffer and dispatch to appropriate handler"
    | eventType |
    eventType := anEventBuffer at: 1.

    "Debug: Log events"
    Transcript show: 'OSiOSDriver: Event type ', eventType printString, ' at ',
        ((anEventBuffer at: 3) @ (anEventBuffer at: 4)) printString; cr.

    eventType = 1 ifTrue: [^ self processMouseEvent: anEventBuffer].
    eventType = 2 ifTrue: [^ self processKeyboardEvent: anEventBuffer].
    eventType = 7 ifTrue: [^ self processMouseWheelEvent: anEventBuffer].
    "eventType 0 = no event, ignore"!

processMouseEvent: anEventBuffer
    "Convert VM mouse event to OSWindow event and dispatch
    Buffer format: [type, timestamp, x, y, buttons, modifiers, subtype, windowIndex]"
    | window x y buttons modifiers subtype osEvent |

    x := anEventBuffer at: 3.
    y := anEventBuffer at: 4.
    buttons := anEventBuffer at: 5.
    modifiers := anEventBuffer at: 6.
    subtype := anEventBuffer at: 7.  "1=down, 2=up, other=move"

    "Get the window (for now, assume single window at index 1)"
    window := WindowMap at: 1 ifAbsent: [^ self].

    "Create appropriate OSWindow event"
    subtype = 1 ifTrue: [
        "Mouse down"
        osEvent := OSMouseButtonPressEvent new
            position: x @ y;
            button: (self buttonFromBits: buttons);
            yourself.
        self applyModifiers: modifiers toEvent: osEvent].

    subtype = 2 ifTrue: [
        "Mouse up"
        osEvent := OSMouseButtonReleaseEvent new
            position: x @ y;
            button: (self buttonFromBits: buttons);
            yourself.
        self applyModifiers: modifiers toEvent: osEvent].

    (subtype ~= 1 and: [subtype ~= 2]) ifTrue: [
        "Mouse move"
        osEvent := OSMouseMoveEvent new
            position: x @ y;
            yourself.
        self applyModifiers: modifiers toEvent: osEvent].

    osEvent ifNotNil: [
        self dispatchEvent: osEvent toWindow: window]!

processKeyboardEvent: anEventBuffer
    "Convert VM keyboard event to OSWindow event"
    "Buffer format: [type, timestamp, charCode, pressCode, modifiers, keyCode, reserved, windowIndex]"
    | window charCode pressCode keyCode osEvent |

    charCode := anEventBuffer at: 3.
    pressCode := anEventBuffer at: 4.
    keyCode := anEventBuffer at: 6.

    window := WindowMap at: 1 ifAbsent: [^ self].

    "pressCode: 0=char, 1=down, 2=up"
    pressCode = 1 ifTrue: [
        osEvent := OSKeyDownEvent new
            symbol: keyCode;
            scanCode: keyCode;
            yourself].
    pressCode = 2 ifTrue: [
        osEvent := OSKeyUpEvent new
            symbol: keyCode;
            scanCode: keyCode;
            yourself].

    osEvent ifNotNil: [
        self dispatchEvent: osEvent toWindow: window]!

processMouseWheelEvent: anEventBuffer
    "Convert VM mouse wheel event to OSWindow event
    Buffer format: [type, timestamp, x, y, deltaX, deltaY, modifiers, windowIndex]"
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
    "Convert VM button bitmask to OSWindow button number
    VM: red=1, blue=2, yellow=4 (bitmask)
    OSWindow: 1=left, 2=right, 3=middle"
    (buttonBits bitAnd: 1) ~= 0 ifTrue: [^ 1].  "Red/left"
    (buttonBits bitAnd: 2) ~= 0 ifTrue: [^ 2].  "Blue/right"
    (buttonBits bitAnd: 4) ~= 0 ifTrue: [^ 3].  "Yellow/middle"
    ^ 1  "Default to left button"!

applyModifiers: modifierBits toEvent: osEvent
    "Apply modifier bits to an OSWindow event
    Modifier bits: shift=1, ctrl=2, alt/option=4, cmd=8"
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
    osWindow eventHandler ifNil: [^ self].

    "Deliver event through OSWindow's event handler"
    osWindow eventHandler handleEvent: osEvent! !

!OSiOSDriver methodsFor: 'testing'!

isNullDriver
    ^ false! !

!OSiOSDriver class methodsFor: 'installation'!

install
    | driver |
    self isSupported ifTrue: [
        driver := self new.
        OSWindowDriver current: driver.
        "Start the event loop immediately since we don't use OSWorldRenderer"
        driver ensureEventLoop.
        Transcript show: 'OSiOSDriver installed and event loop started'; cr]
    ifFalse: [
        Transcript show: 'OSiOSDriver not supported'; cr]!

autoStart
    "Try to start event loop during image startup.
    Called from class-side startUp:"
    | current |
    current := OSWindowDriver current.
    (current isKindOf: OSiOSDriver) ifTrue: [
        current ensureEventLoop.
        Transcript show: 'OSiOSDriver: Auto-started event loop'; cr]! !

!OSiOSDriver class methodsFor: 'system startup'!

startUp: resuming
    "Called during session startup via SessionManager"
    Transcript show: 'OSiOSDriver class>>startUp: ', resuming printString; cr.
    self autoStart!

initialize
    "Register for session startup notifications"
    SessionManager default registerSystemClassNamed: self name! !

"==== OSiOSBackendWindow ===="

OSBackendWindow subclass: #OSiOSBackendWindow
    instanceVariableNames: 'attributes extent title'
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

attributes: anObject
    attributes := anObject! !

!OSiOSBackendWindow methodsFor: 'accessing'!

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
    !

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
    !

hide
    ! !

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
    !

startTextInputAtRectangle: aRectangle
    !

stopTextInput
    ! !

!OSiOSBackendWindow methodsFor: 'clipboard'!

clipboardText
    ^ ''!

clipboardText: aString
    ! !

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
