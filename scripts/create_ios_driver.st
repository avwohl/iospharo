| driverClass |
Transcript cr; show: 'Creating OSiOSDriver class...'.

"Create OSiOSDriver as subclass of OSSDL2Driver (inherits EventLoopProcess, etc.)"
driverClass := OSSDL2Driver subclass: #OSiOSDriver
	instanceVariableNames: 'session eventBuffer'
	classVariableNames: ''
	package: 'OSWindow-iOS'.

Transcript cr; show: 'OSiOSDriver class created: ', driverClass name.

"Class-side methods"
driverClass class compile: 'eventLoopProcess
	^ EventLoopProcess'.

driverClass class compile: 'windowMap
	^ WindowMap ifNil: [WindowMap := WeakValueDictionary new]'.

driverClass class compile: 'driverName
	^ ''iOS'''.

driverClass class compile: 'isAvailable
	^ true'.

driverClass class compile: 'priority
	^ 100  "highest priority driver"'.

"Instance-side methods"
driverClass compile: 'initialize
	super initialize.
	eventBuffer ifNil: [ eventBuffer := IntegerArray new: 8 ]'.

driverClass compile: 'startUp: resuming
	resuming ifTrue: [
		session := Smalltalk session.
		self forceNewEventLoop ]
	ifFalse: [ self ensureEventLoop ]'.

driverClass compile: 'ensureEventLoop
	EventLoopProcess ifNotNil: [
		EventLoopProcess isTerminated ifFalse: [ ^ self ] ].
	self setupEventLoop'.

driverClass compile: 'forceNewEventLoop
	self shutdownEventLoop.
	self setupEventLoop'.

driverClass compile: 'setupEventLoop
	session := Smalltalk session.
	eventBuffer ifNil: [ eventBuffer := IntegerArray new: 8 ].
	EventLoopProcess := [
		[ self eventLoop ] on: Error do: [ :e |
			Transcript show: ''OSiOSDriver error: '', e messageText; cr ] ]
		forkAt: Processor lowIOPriority.
	EventLoopProcess name: ''iOSEventLoop'''.

driverClass compile: 'shutdownEventLoop
	EventLoopProcess ifNil: [ ^ self ].
	[ EventLoopProcess terminate ] on: Error do: [ :e | ].
	EventLoopProcess := nil'.

driverClass compile: 'eventLoop
	| currentSession eventType |
	currentSession := Smalltalk session.
	[ currentSession == Smalltalk session ] whileTrue: [
		[ self primGetNextEvent: eventBuffer.
		  eventType := eventBuffer at: 1.
		  eventType ~= 0 ] whileTrue: [
			[ self processEvent: eventBuffer copy ]
				on: Error do: [ :e |
					Transcript show: ''Event error: '', e messageText; cr ] ].
		(Delay forMilliseconds: 5) wait ]'.

driverClass compile: 'primGetNextEvent: anEventBuffer
	<primitive: 264>
	"Fallback: set event type to 0 (no event)"
	anEventBuffer at: 1 put: 0'.

driverClass compile: 'processEvent: anEventBuffer
	| eventType |
	eventType := anEventBuffer at: 1.
	eventType = 1 ifTrue: [ ^ self processMouseEvent: anEventBuffer ].
	eventType = 2 ifTrue: [ ^ self processKeyboardEvent: anEventBuffer ].
	eventType = 7 ifTrue: [ ^ self processMouseWheelEvent: anEventBuffer ]'.

driverClass compile: 'processMouseEvent: anEventBuffer
	| window x y buttons subtype osEvent |
	x := anEventBuffer at: 3.
	y := anEventBuffer at: 4.
	buttons := anEventBuffer at: 5.
	subtype := anEventBuffer at: 7.
	window := WindowMap at: 1 ifAbsent: [ ^ self ].
	subtype = 1 ifTrue: [
		osEvent := OSMouseButtonPressEvent new
			position: x @ y;
			button: (self buttonFromBits: buttons);
			yourself.
		^ self dispatchEvent: osEvent toWindow: window ].
	subtype = 2 ifTrue: [
		osEvent := OSMouseButtonReleaseEvent new
			position: x @ y;
			button: (self buttonFromBits: buttons);
			yourself.
		^ self dispatchEvent: osEvent toWindow: window ].
	osEvent := OSMouseMoveEvent new
		position: x @ y;
		yourself.
	self dispatchEvent: osEvent toWindow: window'.

driverClass compile: 'processKeyboardEvent: anEventBuffer
	| window pressCode keyCode osEvent |
	pressCode := anEventBuffer at: 4.
	keyCode := anEventBuffer at: 6.
	window := WindowMap at: 1 ifAbsent: [ ^ self ].
	pressCode = 1 ifTrue: [
		osEvent := OSKeyDownEvent new
			symbol: keyCode;
			scanCode: keyCode;
			yourself.
		^ self dispatchEvent: osEvent toWindow: window ].
	pressCode = 2 ifTrue: [
		osEvent := OSKeyUpEvent new
			symbol: keyCode;
			scanCode: keyCode;
			yourself.
		^ self dispatchEvent: osEvent toWindow: window ]'.

driverClass compile: 'processMouseWheelEvent: anEventBuffer
	| window x y deltaX deltaY osEvent |
	x := anEventBuffer at: 3.
	y := anEventBuffer at: 4.
	deltaX := anEventBuffer at: 5.
	deltaY := anEventBuffer at: 6.
	window := WindowMap at: 1 ifAbsent: [ ^ self ].
	osEvent := OSMouseWheelEvent new
		position: x @ y;
		scrollHorizontal: deltaX;
		scrollVertical: deltaY;
		yourself.
	self dispatchEvent: osEvent toWindow: window'.

driverClass compile: 'buttonFromBits: buttonBits
	(buttonBits bitAnd: 1) ~= 0 ifTrue: [ ^ 1 ].
	(buttonBits bitAnd: 2) ~= 0 ifTrue: [ ^ 2 ].
	(buttonBits bitAnd: 4) ~= 0 ifTrue: [ ^ 3 ].
	^ 1'.

driverClass compile: 'dispatchEvent: osEvent toWindow: window
	| osWindow |
	osWindow := window osWindow.
	osWindow ifNil: [ ^ self ].
	osWindow eventHandler ifNotNil: [ :handler |
		handler handleEvent: osEvent ]'.

"Set OSiOSDriver as the preferred driver"
OSWindowDriver classPool at: #DriverClass put: driverClass.
OSWindowDriver classPool at: #Current put: nil.
OSWindowDriver classPool at: #Session put: nil.

Transcript cr; show: 'OSiOSDriver installed as preferred driver!'.
Transcript cr; show: 'Saving image...'.
