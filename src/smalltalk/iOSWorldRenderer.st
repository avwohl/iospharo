"
iOSWorldRenderer - Custom WorldRenderer for iOS VM

This renderer connects Pharo's Morphic world to the iOS display surface
through VM primitives.

Primitives used:
  102 - primitiveBeDisplay (register Form as display)
  103 - primitiveForceDisplayUpdate (copy Form bits to screen)
  106 - primitiveScreenSize (get screen dimensions)

To install manually in Pharo:
  World worldState worldRenderer: iOSWorldRenderer new
"

Object subclass: #iOSWorldRenderer
    instanceVariableNames: 'world display damagedRects deferred eventBuffer'
    classVariableNames: ''
    package: 'iOS-VM'!

!iOSWorldRenderer class methodsFor: 'instance creation'!

forWorld: aWorld
    ^ self new
        world: aWorld;
        yourself
! !

!iOSWorldRenderer methodsFor: 'accessing'!

world
    ^ world!

world: aWorld
    world := aWorld!

display
    ^ display!

display: aForm
    display := aForm.
    display beDisplay!

damagedRects
    ^ damagedRects ifNil: [ damagedRects := OrderedCollection new ]! !

!iOSWorldRenderer methodsFor: 'initialization'!

initialize
    super initialize.
    damagedRects := OrderedCollection new.
    deferred := false.
    eventBuffer := Array new: 8.
    self initializeDisplay!

initializeDisplay
    "Create and register the display Form"
    | extent |
    extent := self primScreenSize.
    display := Form extent: extent depth: 32.
    display beDisplay.
    "Set global Display"
    Smalltalk at: #Display put: display! !

!iOSWorldRenderer methodsFor: 'display'!

checkForNewScreenSize
    "Check if screen size changed and resize display if needed"
    | newExtent |
    newExtent := self primScreenSize.
    (display isNil or: [ display extent ~= newExtent ]) ifTrue: [
        display := Form extent: newExtent depth: 32.
        display beDisplay.
        Smalltalk at: #Display put: display.
        world ifNotNil: [ :w | w restoreMorphicDisplay ] ]!

primScreenSize
    "Get screen size from VM - primitive 106"
    <primitive: 106>
    ^ 1024 @ 768!

deferUpdates: aBoolean
    deferred := aBoolean!

doOneCycle
    "Main rendering loop - called by WorldState"
    world ifNil: [ ^ self ].
    "Process events"
    self processEvents.
    "Run the world cycle"
    world doOneCycleNow.
    "Flush display updates"
    self forceDisplayUpdate! !

!iOSWorldRenderer methodsFor: 'drawing'!

displayWorldStateOf: aWorld during: aBlock
    "Main rendering entry point called by WorldState"
    display ifNil: [ self initializeDisplay ].
    "Draw the world"
    aBlock value: display getCanvas.
    "Flush to screen"
    self forceDisplayUpdate!

forceDisplayUpdate
    "Copy display Form bits to iOS screen - primitive 103"
    <primitive: 103>
    ^ self!

invalidate: aRectangle
    "Mark rectangle as needing redraw"
    damagedRects add: aRectangle.
    deferred ifFalse: [ self forceDisplayUpdate ]!

fullRepaint
    "Repaint the entire display"
    display ifNil: [ ^ self ].
    world ifNil: [ ^ self ].
    world fullRepaintNeeded.
    self forceDisplayUpdate! !

!iOSWorldRenderer methodsFor: 'events'!

processEvents
    "Process pending input events from the VM by polling primitive 264"
    | eventType |
    eventBuffer ifNil: [ eventBuffer := Array new: 8 ].

    "Poll until no more events"
    [ self primGetNextEvent: eventBuffer.
      eventType := eventBuffer at: 1.
      eventType > 0 ] whileTrue: [
        self dispatchEvent: eventBuffer copy ]!

primGetNextEvent: anArray
    "Primitive 264 - get next event from VM event queue"
    <primitive: 264>
    "On failure, set type to 0 (no event)"
    anArray at: 1 put: 0!

dispatchEvent: anEventArray
    "Convert raw event array to MorphicEvent and dispatch to active hand"
    | eventType hand event |
    eventType := anEventArray at: 1.
    world ifNil: [ ^ self ].
    hand := world activeHand.
    hand ifNil: [ ^ self ].

    "Event types: 1=mouse, 2=keyboard, 3=special (drag, resize, etc.)"
    eventType = 1 ifTrue: [
        event := self createMouseEvent: anEventArray.
        event ifNotNil: [ hand handleEvent: event ] ].
    eventType = 2 ifTrue: [
        event := self createKeyboardEvent: anEventArray.
        event ifNotNil: [ hand handleEvent: event ] ]!

createMouseEvent: anEventArray
    "Create a MouseEvent from raw event array.
     Array format: #(type timeStamp x y buttons modifiers subtype window)
     Subtype: 1=mouseDown, 2=mouseUp, 3=mouseMove"
    | x y buttons modifiers subtype |
    x := (anEventArray at: 3) ifNil: [ 0 ].
    y := (anEventArray at: 4) ifNil: [ 0 ].
    buttons := (anEventArray at: 5) ifNil: [ 0 ].
    modifiers := (anEventArray at: 6) ifNil: [ 0 ].
    subtype := (anEventArray at: 7) ifNil: [ 3 ].  "default to mouseMove"

    "Determine event class based on subtype"
    subtype = 1 ifTrue: [
        ^ MouseButtonPressEvent new
            setType: #mouseDown;
            position: x @ y;
            buttons: buttons;
            hand: world activeHand;
            yourself ].
    subtype = 2 ifTrue: [
        ^ MouseButtonReleaseEvent new
            setType: #mouseUp;
            position: x @ y;
            buttons: buttons;
            hand: world activeHand;
            yourself ].
    subtype = 3 ifTrue: [
        ^ MouseMoveEvent new
            setType: #mouseMove;
            position: x @ y;
            buttons: buttons;
            hand: world activeHand;
            yourself ].
    ^ nil!

createKeyboardEvent: anEventArray
    "Create a KeyboardEvent from raw event array.
     Array format: #(type timeStamp charCode scanCode modifiers subtype reserved window)"
    | charCode scanCode modifiers subtype |
    charCode := (anEventArray at: 3) ifNil: [ 0 ].
    scanCode := (anEventArray at: 4) ifNil: [ 0 ].
    modifiers := (anEventArray at: 5) ifNil: [ 0 ].
    subtype := (anEventArray at: 6) ifNil: [ 0 ].  "0=keyDown, 1=keyUp, 2=keyChar"

    "Return keystroke event"
    ^ KeyboardEvent new
        setType: (subtype = 1 ifTrue: [ #keyUp ] ifFalse: [ #keyDown ]);
        keyValue: charCode;
        scanCode: scanCode;
        hand: world activeHand;
        yourself! !

!iOSWorldRenderer methodsFor: 'activation'!

activate
    "Activate this renderer for the world"
    self initializeDisplay.
    world ifNotNil: [ :w | w restoreMorphicDisplay ]!

deactivate
    "Deactivate this renderer"
    display := nil! !

!iOSWorldRenderer methodsFor: 'testing'!

isNullWorldRenderer
    "We are NOT a null renderer - we actually render"
    ^ false! !
