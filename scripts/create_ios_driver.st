"Create OSiOSDriver class and install event loop methods"!

OSWindowDriver subclass: #OSiOSDriver
  instanceVariableNames: 'session eventBuffer'
  classVariableNames: 'EventLoopProcess WindowMap'
  package: 'OSiOS'!

!OSiOSDriver class methodsFor: 'accessing'!
eventLoopProcess
  ^ EventLoopProcess! !

!OSiOSDriver class methodsFor: 'accessing'!
windowMap
  ^ WindowMap ifNil: [ WindowMap := WeakValueDictionary new ]! !

!OSiOSDriver class methodsFor: 'testing'!
isSuitable
  "iOS driver is suitable when SDL2 is NOT available (running on iOS or our custom VM)"
  ^ SDL2 isAvailable not! !

!OSiOSDriver class methodsFor: 'testing'!
isSupported
  "iOS driver is supported when SDL2 is NOT available"
  ^ SDL2 isAvailable not! !

!OSiOSDriver class methodsFor: 'installation'!
installIfNeeded
  "Install this driver if SDL2 is not available.
   This bypasses pickDriverClass which has issues with our VM."
  SDL2 isAvailable ifFalse: [
    OSWindowDriver classVarNamed: 'Current' put: self new.
    Transcript cr; show: 'OSiOSDriver installed as current driver'; cr]! !

!OSiOSDriver methodsFor: 'initialization'!
initialize
  super initialize.
  session := Smalltalk session.
  eventBuffer := Array new: 8.
  1 to: 8 do: [ :i | eventBuffer at: i put: 0 ].
  WindowMap ifNil: [ WindowMap := WeakValueDictionary new ]! !

!OSiOSDriver methodsFor: 'session'!
startUp: resuming
  resuming
    ifTrue: [ session := Smalltalk session. self forceNewEventLoop ]
    ifFalse: [ self ensureEventLoop ]! !

!OSiOSDriver methodsFor: 'event loop'!
ensureEventLoop
  EventLoopProcess ifNotNil: [
    EventLoopProcess isTerminated ifFalse: [ ^ self ] ].
  self setupEventLoop! !

!OSiOSDriver methodsFor: 'event loop'!
forceNewEventLoop
  self shutdownEventLoop.
  self setupEventLoop! !

!OSiOSDriver methodsFor: 'event loop'!
setupEventLoop
  EventLoopProcess := [
    [ self eventLoop ]
      on: Error
      do: [ :e | Transcript show: 'iOSEventLoop error: ', e messageText; cr ] ]
    forkAt: Processor lowIOPriority.
  EventLoopProcess name: 'iOSEventLoop'! !

!OSiOSDriver methodsFor: 'event loop'!
shutdownEventLoop
  EventLoopProcess ifNil: [ ^ self ].
  [ EventLoopProcess terminate ] on: Error do: [ :e | ].
  EventLoopProcess := nil! !

!OSiOSDriver methodsFor: 'event loop'!
eventLoop
  | currentSession eventType |
  currentSession := Smalltalk session.
  [ currentSession == Smalltalk session ] whileTrue: [
    [ self primGetNextEvent: eventBuffer.
      eventType := eventBuffer at: 1.
      eventType ~= 0 ] whileTrue: [
      [ self processEvent: eventBuffer copy ]
        on: Error
        do: [ :e | Transcript show: 'Event error: ', e messageText; cr ] ].
    (Delay forMilliseconds: 5) wait ]! !

!OSiOSDriver methodsFor: 'primitives'!
primGetNextEvent: anEventBuffer
  <primitive: 264>
  anEventBuffer at: 1 put: 0! !

!OSiOSDriver methodsFor: 'event handling'!
processEvent: anEventBuffer
  | eventType |
  eventType := anEventBuffer at: 1.
  eventType = 1 ifTrue: [ ^ self processMouseEvent: anEventBuffer ].
  eventType = 2 ifTrue: [ ^ self processKeyboardEvent: anEventBuffer ].
  eventType = 7 ifTrue: [ ^ self processMouseWheelEvent: anEventBuffer ]! !

!OSiOSDriver methodsFor: 'event handling'!
processMouseEvent: anEventBuffer
  | window x y buttons subtype osEvent |
  x := anEventBuffer at: 3.
  y := anEventBuffer at: 4.
  buttons := anEventBuffer at: 5.
  subtype := anEventBuffer at: 7.
  window := WindowMap at: 1 ifAbsent: [ ^ self ].
  subtype = 1 ifTrue: [
    osEvent := OSMouseButtonPressEvent new position: x @ y; button: (self buttonFromBits: buttons); yourself.
    ^ self dispatchEvent: osEvent toWindow: window ].
  subtype = 2 ifTrue: [
    osEvent := OSMouseButtonReleaseEvent new position: x @ y; button: (self buttonFromBits: buttons); yourself.
    ^ self dispatchEvent: osEvent toWindow: window ].
  osEvent := OSMouseMoveEvent new position: x @ y; yourself.
  self dispatchEvent: osEvent toWindow: window! !

!OSiOSDriver methodsFor: 'event handling'!
processKeyboardEvent: anEventBuffer
  | window pressCode keyCode osEvent |
  pressCode := anEventBuffer at: 4.
  keyCode := anEventBuffer at: 6.
  window := WindowMap at: 1 ifAbsent: [ ^ self ].
  pressCode = 1 ifTrue: [
    osEvent := OSKeyDownEvent new symbol: keyCode; scanCode: keyCode; yourself.
    ^ self dispatchEvent: osEvent toWindow: window ].
  pressCode = 2 ifTrue: [
    osEvent := OSKeyUpEvent new symbol: keyCode; scanCode: keyCode; yourself.
    ^ self dispatchEvent: osEvent toWindow: window ]! !

!OSiOSDriver methodsFor: 'event handling'!
processMouseWheelEvent: anEventBuffer
  | window x y deltaX deltaY osEvent |
  x := anEventBuffer at: 3.
  y := anEventBuffer at: 4.
  deltaX := anEventBuffer at: 5.
  deltaY := anEventBuffer at: 6.
  window := WindowMap at: 1 ifAbsent: [ ^ self ].
  osEvent := OSMouseWheelEvent new position: x @ y; scrollHorizontal: deltaX; scrollVertical: deltaY; yourself.
  self dispatchEvent: osEvent toWindow: window! !

!OSiOSDriver methodsFor: 'event handling'!
buttonFromBits: buttonBits
  (buttonBits bitAnd: 1) ~= 0 ifTrue: [ ^ 1 ].
  (buttonBits bitAnd: 2) ~= 0 ifTrue: [ ^ 2 ].
  (buttonBits bitAnd: 4) ~= 0 ifTrue: [ ^ 3 ].
  ^ 1! !

!OSiOSDriver methodsFor: 'event handling'!
dispatchEvent: osEvent toWindow: window
  | osWindow |
  osWindow := window osWindow.
  osWindow ifNil: [ ^ self ].
  osWindow eventHandler ifNotNil: [ :handler | handler handleEvent: osEvent ]! !

!OSiOSDriver methodsFor: 'window creation'!
createWindowWithAttributes: anOSWindowAttributes osWindow: osWindow
  "Create a backend window for iOS. We use OSNullBackendWindow with our renderer."
  | backend |
  backend := OSNullBackendWindow fromAttributes: anOSWindowAttributes for: osWindow.
  "Register this window in our map so events can be dispatched to it"
  WindowMap at: 1 put: osWindow.
  ^ backend! !

!OSiOSDriver methodsFor: 'testing'!
isNullDriver
  "We are not a null driver - we handle events"
  ^ false! !

Transcript cr; show: 'OSiOSDriver loaded'; cr!
