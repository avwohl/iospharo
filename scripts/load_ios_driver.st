Transcript cr; show: 'Loading OSiOSDriver...'.

(OSiOSDriver instVarNames includes: 'session') ifFalse: [ OSiOSDriver addInstVarNamed: 'session'. OSiOSDriver addInstVarNamed: 'eventBuffer' ].

(OSiOSDriver classVarNames includes: 'EventLoopProcess') ifFalse: [ OSiOSDriver addClassVarNamed: 'EventLoopProcess'. OSiOSDriver addClassVarNamed: 'WindowMap' ].

OSiOSDriver class compile: 'eventLoopProcess ^ EventLoopProcess'.

OSiOSDriver class compile: 'windowMap ^ WindowMap ifNil: [WindowMap := WeakValueDictionary new]'.

OSiOSDriver compile: 'initialize super initialize. session := Smalltalk session. eventBuffer := Array new: 8. 1 to: 8 do: [:i | eventBuffer at: i put: 0]. WindowMap ifNil: [WindowMap := WeakValueDictionary new]'.

OSiOSDriver compile: 'startUp: resuming resuming ifTrue: [session := Smalltalk session. self forceNewEventLoop] ifFalse: [self ensureEventLoop]'.

OSiOSDriver compile: 'ensureEventLoop EventLoopProcess ifNotNil: [EventLoopProcess isTerminated ifFalse: [^ self]]. self setupEventLoop'.

OSiOSDriver compile: 'forceNewEventLoop self shutdownEventLoop. self setupEventLoop'.

OSiOSDriver compile: 'setupEventLoop EventLoopProcess := [[self eventLoop] on: Error do: [:e | Transcript show: e messageText; cr]] forkAt: Processor lowIOPriority. EventLoopProcess name: #iOSEventLoop asString'.

OSiOSDriver compile: 'shutdownEventLoop EventLoopProcess ifNil: [^ self]. [EventLoopProcess terminate] on: Error do: [:e | ]. EventLoopProcess := nil'.

OSiOSDriver compile: 'eventLoop | currentSession eventType | currentSession := Smalltalk session. [currentSession == Smalltalk session] whileTrue: [[self primGetNextEvent: eventBuffer. eventType := eventBuffer at: 1. eventType ~= 0] whileTrue: [[self processEvent: eventBuffer copy] on: Error do: [:e | Transcript show: e messageText; cr]]. (Delay forMilliseconds: 5) wait]'.

OSiOSDriver compile: 'primGetNextEvent: anEventBuffer <primitive: 264> anEventBuffer at: 1 put: 0'.

OSiOSDriver compile: 'processEvent: anEventBuffer | eventType | eventType := anEventBuffer at: 1. eventType = 1 ifTrue: [^ self processMouseEvent: anEventBuffer]. eventType = 2 ifTrue: [^ self processKeyboardEvent: anEventBuffer]. eventType = 7 ifTrue: [^ self processMouseWheelEvent: anEventBuffer]'.

OSiOSDriver compile: 'processMouseEvent: anEventBuffer | window x y buttons subtype osEvent | x := anEventBuffer at: 3. y := anEventBuffer at: 4. buttons := anEventBuffer at: 5. subtype := anEventBuffer at: 7. window := WindowMap at: 1 ifAbsent: [^ self]. subtype = 1 ifTrue: [osEvent := OSMouseButtonPressEvent new position: x @ y; button: (self buttonFromBits: buttons); yourself. ^ self dispatchEvent: osEvent toWindow: window]. subtype = 2 ifTrue: [osEvent := OSMouseButtonReleaseEvent new position: x @ y; button: (self buttonFromBits: buttons); yourself. ^ self dispatchEvent: osEvent toWindow: window]. osEvent := OSMouseMoveEvent new position: x @ y; yourself. self dispatchEvent: osEvent toWindow: window'.

OSiOSDriver compile: 'processKeyboardEvent: anEventBuffer | window pressCode keyCode osEvent | pressCode := anEventBuffer at: 4. keyCode := anEventBuffer at: 6. window := WindowMap at: 1 ifAbsent: [^ self]. pressCode = 1 ifTrue: [osEvent := OSKeyDownEvent new symbol: keyCode; scanCode: keyCode; yourself. ^ self dispatchEvent: osEvent toWindow: window]. pressCode = 2 ifTrue: [osEvent := OSKeyUpEvent new symbol: keyCode; scanCode: keyCode; yourself. ^ self dispatchEvent: osEvent toWindow: window]'.

OSiOSDriver compile: 'processMouseWheelEvent: anEventBuffer | window x y deltaX deltaY osEvent | x := anEventBuffer at: 3. y := anEventBuffer at: 4. deltaX := anEventBuffer at: 5. deltaY := anEventBuffer at: 6. window := WindowMap at: 1 ifAbsent: [^ self]. osEvent := OSMouseWheelEvent new position: x @ y; scrollHorizontal: deltaX; scrollVertical: deltaY; yourself. self dispatchEvent: osEvent toWindow: window'.

OSiOSDriver compile: 'buttonFromBits: buttonBits (buttonBits bitAnd: 1) ~= 0 ifTrue: [^ 1]. (buttonBits bitAnd: 2) ~= 0 ifTrue: [^ 2]. (buttonBits bitAnd: 4) ~= 0 ifTrue: [^ 3]. ^ 1'.

OSiOSDriver compile: 'dispatchEvent: osEvent toWindow: window | osWindow | osWindow := window osWindow. osWindow ifNil: [^ self]. osWindow eventHandler ifNotNil: [:handler | handler handleEvent: osEvent]'.

Transcript cr; show: 'Done loading OSiOSDriver methods!'.
