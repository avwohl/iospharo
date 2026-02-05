"Test iOS driver loading and event loop"!

Object subclass: #IOSDriverTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!IOSDriverTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/ios_driver_test.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== iOS Driver Test ==='; cr; cr.

    "Test 1: Check OSiOSDriver exists"
    stream nextPutAll: '--- Test 1: OSiOSDriver class ---'; cr.
    (Smalltalk hasClassNamed: #OSiOSDriver)
      ifTrue: [stream nextPutAll: 'OSiOSDriver class: EXISTS'; cr]
      ifFalse: [stream nextPutAll: 'OSiOSDriver class: NOT FOUND'; cr].
    stream flush.

    "Test 2: Check if OSiOSDriver has key methods"
    stream cr; nextPutAll: '--- Test 2: OSiOSDriver methods ---'; cr.
    [| driverClass |
     driverClass := Smalltalk at: #OSiOSDriver.
     #(setupEventLoop eventLoop primGetNextEvent: processEvent:) do: [:sel |
       (driverClass canUnderstand: sel)
         ifTrue: [stream nextPutAll: sel asString; nextPutAll: ': EXISTS'; cr]
         ifFalse: [stream nextPutAll: sel asString; nextPutAll: ': MISSING'; cr]].
     stream nextPutAll: 'Test 2: done'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 2 ERROR: '; print: ex class name; cr].
    stream flush.

    "Test 3: Try to instantiate OSiOSDriver"
    stream cr; nextPutAll: '--- Test 3: Instantiate driver ---'; cr.
    [| driver |
     driver := (Smalltalk at: #OSiOSDriver) new.
     stream nextPutAll: 'Driver instance: '; print: driver class name; cr.
     stream nextPutAll: 'Test 3: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 3 ERROR: '; print: ex class name; nextPutAll: ': '; print: ex messageText; cr].
    stream flush.

    "Test 4: Check EventLoopProcess class variable"
    stream cr; nextPutAll: '--- Test 4: EventLoopProcess ---'; cr.
    [| driverClass proc |
     driverClass := Smalltalk at: #OSiOSDriver.
     proc := driverClass eventLoopProcess.
     proc
       ifNil: [stream nextPutAll: 'EventLoopProcess: nil (not started)'; cr]
       ifNotNil: [
         stream nextPutAll: 'EventLoopProcess: '; print: proc class name; cr.
         stream nextPutAll: '  isTerminated: '; print: proc isTerminated; cr.
         stream nextPutAll: '  priority: '; print: proc priority; cr].
     stream nextPutAll: 'Test 4: done'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 4 ERROR: '; print: ex class name; nextPutAll: ': '; print: ex messageText; cr].
    stream flush.

    "Test 5: Try to start event loop"
    stream cr; nextPutAll: '--- Test 5: Start event loop ---'; cr.
    [| driver |
     driver := (Smalltalk at: #OSiOSDriver) new.
     stream nextPutAll: 'Calling setupEventLoop...'; cr. stream flush.
     driver setupEventLoop.
     (Delay forMilliseconds: 100) wait.
     stream nextPutAll: 'After setupEventLoop:'; cr.
     stream nextPutAll: '  EventLoopProcess: '; print: (Smalltalk at: #OSiOSDriver) eventLoopProcess; cr.
     stream nextPutAll: 'Test 5: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 5 ERROR: '; print: ex class name; nextPutAll: ': '; print: ex messageText; cr].
    stream flush.

    "Test 6: Call primitive 264 via OSiOSDriver"
    stream cr; nextPutAll: '--- Test 6: Primitive 264 ---'; cr.
    [| driver buffer |
     driver := (Smalltalk at: #OSiOSDriver) new.
     buffer := Array new: 8.
     1 to: 8 do: [:i | buffer at: i put: 0].
     stream nextPutAll: 'Calling primGetNextEvent: via driver...'; cr. stream flush.
     driver primGetNextEvent: buffer.
     stream nextPutAll: 'Buffer after prim264: '.
     1 to: 8 do: [:i | stream print: (buffer at: i); nextPutAll: ' '].
     stream cr.
     stream nextPutAll: 'Test 6: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 6 ERROR: '; print: ex class name; nextPutAll: ': '; print: ex messageText; cr].
    stream flush.

    stream cr.
    stream nextPutAll: '=== DONE ==='; cr.
    stream flush]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!IOSDriverTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!IOSDriverTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #IOSDriverTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
