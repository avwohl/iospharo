"Test display system initialization"!

Object subclass: #DisplaySystemTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!DisplaySystemTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/display_system_test.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== Display System Test ==='; cr; cr.

    "Test 1: Check OSWindow subsystem"
    stream nextPutAll: '--- Test 1: OSWindow subsystem ---'; cr.
    [| world backend |
     world := World ifNil: [WorldMorph new].
     stream nextPutAll: 'World class: '; print: world class name; cr.
     stream nextPutAll: 'World worldState: '; print: world worldState class name; cr.
     backend := OSWindowDriver current.
     stream nextPutAll: 'OSWindowDriver current: '; print: backend class name; cr.
     stream flush]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 1 ERROR: '; print: ex class name; nextPutAll: ': '; print: ex messageText; cr].
    stream flush.

    "Test 2: Check available drivers"
    stream cr; nextPutAll: '--- Test 2: Available drivers ---'; cr.
    [OSWindowDriver allSubclasses do: [:cls |
       stream nextPutAll: '  '; print: cls name.
       [stream nextPutAll: ' isApplicable='; print: (cls isApplicableFor: nil)]
         on: Error do: [:e | stream nextPutAll: ' (error: '; print: e class name; nextPutAll: ')'].
       stream cr].
     stream flush]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 2 ERROR: '; print: ex class name; nextPutAll: ': '; print: ex messageText; cr].
    stream flush.

    "Test 3: Check WorldMorph and renderer"
    stream cr; nextPutAll: '--- Test 3: WorldMorph renderer ---'; cr.
    [| world renderer |
     world := World ifNil: [WorldMorph new].
     renderer := world worldState worldRenderer.
     stream nextPutAll: 'WorldRenderer: '; print: renderer class name; cr.
     renderer ifNotNil: [
       stream nextPutAll: '  driver: '.
       [stream print: renderer driver class name]
         on: Error do: [:e | stream nextPutAll: '(error)'].
       stream cr].
     stream flush]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 3 ERROR: '; print: ex class name; nextPutAll: ': '; print: ex messageText; cr].
    stream flush.

    "Test 4: Check HandMorph and event handling"
    stream cr; nextPutAll: '--- Test 4: HandMorph ---'; cr.
    [| hand |
     hand := World activeHand.
     hand ifNil: [hand := HandMorph new].
     stream nextPutAll: 'ActiveHand: '; print: hand class name; cr.
     stream nextPutAll: '  position: '; print: hand position; cr.
     stream nextPutAll: '  lastMouseEvent: '.
     [stream print: hand lastMouseEvent class name]
       on: Error do: [:e | stream nextPutAll: 'nil or error'].
     stream cr.
     stream flush]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 4 ERROR: '; print: ex class name; nextPutAll: ': '; print: ex messageText; cr].
    stream flush.

    "Test 5: Check InputEventSensor / EventSensor"
    stream cr; nextPutAll: '--- Test 5: Event sensor ---'; cr.
    [| sensor |
     sensor := Smalltalk at: #Sensor ifAbsent: [nil].
     sensor ifNil: [sensor := Smalltalk at: #InputEventSensor ifAbsent: [nil]].
     sensor
       ifNil: [stream nextPutAll: 'No Sensor global found'; cr]
       ifNotNil: [
         stream nextPutAll: 'Sensor class: '; print: sensor class name; cr.
         stream nextPutAll: '  hasInputSemaphore: '.
         [(sensor respondsTo: #inputSemaphore)
            ifTrue: [stream print: sensor inputSemaphore notNil]
            ifFalse: [stream nextPutAll: 'N/A']]
           on: Error do: [:e | stream nextPutAll: 'error'].
         stream cr].
     stream flush]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 5 ERROR: '; print: ex class name; nextPutAll: ': '; print: ex messageText; cr].
    stream flush.

    "Test 6: Try to get next event via primitive 264"
    stream cr; nextPutAll: '--- Test 6: Primitive 264 test ---'; cr.
    [| buffer eventType |
     buffer := Array new: 8.
     1 to: 8 do: [:i | buffer at: i put: 0].
     "Call primitive 264 directly"
     self primGetNextEvent: buffer.
     eventType := buffer at: 1.
     stream nextPutAll: 'Event type: '; print: eventType; cr.
     eventType ~= 0 ifTrue: [
       stream nextPutAll: '  timeStamp: '; print: (buffer at: 2); cr.
       stream nextPutAll: '  x: '; print: (buffer at: 3); cr.
       stream nextPutAll: '  y: '; print: (buffer at: 4); cr].
     stream nextPutAll: 'Test 6: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 6 ERROR: '; print: ex class name; nextPutAll: ': '; print: ex messageText; cr].
    stream flush.

    "Test 7: Check if display is valid"
    stream cr; nextPutAll: '--- Test 7: Display ---'; cr.
    [| display |
     display := Smalltalk at: #Display ifAbsent: [nil].
     display
       ifNil: [stream nextPutAll: 'No Display global found'; cr]
       ifNotNil: [
         stream nextPutAll: 'Display: '; print: display class name; cr.
         stream nextPutAll: '  extent: '; print: display extent; cr.
         stream nextPutAll: '  depth: '; print: display depth; cr].
     stream flush]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 7 ERROR: '; print: ex class name; nextPutAll: ': '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== DONE ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!DisplaySystemTest class methodsFor: 'primitives'!
primGetNextEvent: eventBuffer
  <primitive: 264>
  eventBuffer at: 1 put: 0! !

!DisplaySystemTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!DisplaySystemTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #DisplaySystemTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
