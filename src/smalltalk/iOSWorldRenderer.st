"
iOSWorldRenderer - Custom WorldRenderer for iOS VM

This renderer connects Pharo's Morphic world to the iOS display surface
through VM primitives.

Primitives used:
  102 - primitiveBeDisplay (register Form as display)
  103 - primitiveForceDisplayUpdate (copy Form bits to screen)

To install manually in Pharo:
  World worldState worldRenderer: iOSWorldRenderer new
"

Object subclass: #iOSWorldRenderer
    instanceVariableNames: 'world display


damagedRects

deferred
'
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
    display
 beDisplay!

damagedRects
    ^ damagedRects ifNil: [ damagedRects := OrderedCollection new ]! !

!iOSWorldRenderer methodsFor: 'initialization'!

initialize
    super initialize.
    damagedRects := OrderedCollection new.
    deferred := false.
    self initializeDisplay!

initializeDisplay
    "Create and register the display Form"
    | width height |
    width := Display
        ifNotNil: [ Display width ]
        ifNil: [ 1024 ].
    height := Display
        ifNotNil: [ Display height ]
        ifNil: [ 768 ].

    display := Form extent: width @ height depth: 32.
    display
 beDisplay.

    "Set global Display"
    Smalltalk at: #Display put: display! !

!iOSWorldRenderer methodsFor: 'display'!




checkForNewScreenSize
    "Check if screen size changed and resize display if needed"
    | newExtent |
    newExtent := self

primScreenSize.
    (display isNil or: [ display extent ~= newExtent ]) ifTrue: [
        display := Form extent: newExtent depth: 32.
        display

beDisplay.
        Smalltalk at: #Display put: display.
        world

ifNotNil: [ :w | w

restoreMorphicDisplay ] ]!

primScreenSize
    "Get screen size from VM - primitive
106"
    <primitive: 106>
    ^ 1024 @ 768!

deferUpdates: aBoolean
    deferred := aBoolean!




doOneCycle
    "Main rendering loop - called by WorldState"
    world
 ifNil: [ ^ self ].

    "Process events"
    self processEvents.

    "Run the world cycle"
    world

doOneCycleNow.

    "Flush display updates"
    self


forceDisplayUpdate! !

!iOSWorldRenderer methodsFor: 'drawing'!


displayWorldStateOf: aWorld

during:
 aBlock
    "Main rendering entry point called by WorldState"
    display ifNil: [ self initializeDisplay ].

    "Draw the world"
    aBlock value: display
 getCanvas.

    "Flush to screen"
    self

forceDisplayUpdate!



forceDisplayUpdate
    "Copy display Form bits to iOS screen - primitive 103"
    <primitive: 103>
    "Fallback: just

mark

 entire
 screen
 as

needing update"
    self


primForceUpdate!

primForceUpdate
    "Force display

update

- primitive 103"
    <primitive: 103>!




invalidate: aRectangle
    "Mark rectangle as needing redraw"
    damagedRects add: aRectangle.
    deferred ifFalse: [ self

forceDisplayUpdate ]!

fullRepaint
    "Repaint the entire display"
    display ifNil: [ ^ self ].
    world
 ifNil: [ ^ self ].
    world

fullRepaintNeeded.
    self

forceDisplayUpdate! !

!iOSWorldRenderer methodsFor: 'events'!

processEvents
    "Process pending input events from the VM"
    "Events are handled via EventSensor which reads from VM event queue"! !

!iOSWorldRenderer methodsFor: 'activation'!

activate
    "Activate this renderer for the world"
    self initializeDisplay.
    world
 ifNotNil: [ :w |
        w restoreMorphicDisplay ]!

deactivate
    "Deactivate this renderer"
    display := nil! !

!iOSWorldRenderer methodsFor: 'testing'!

isNullWorldRenderer
    "We are NOT a null renderer - we actually render"
    ^ false! !
