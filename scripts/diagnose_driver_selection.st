"Diagnose driver selection"!

Object subclass: #DriverSelectionDiagnostic
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!DriverSelectionDiagnostic class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/driver_selection.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== Driver Selection Diagnostic ==='; cr; cr.

    "Step 1: Check SDL2 availability"
    stream nextPutAll: '--- Step 1: SDL2 availability ---'; cr.
    [stream nextPutAll: 'SDL2 isAvailable: '.
     [stream print: SDL2 isAvailable] on: Error do: [:e |
       stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'].
     stream cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 2: Check all driver isSuitable"
    stream cr; nextPutAll: '--- Step 2: Driver isSuitable ---'; cr.
    [OSWindowDriver allSubclasses do: [:c |
       stream nextPutAll: c name; nextPutAll: ' isSuitable: '.
       [stream print: c isSuitable] on: Error do: [:e |
         stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'].
       stream cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 3: Check pickDriverClass"
    stream cr; nextPutAll: '--- Step 3: pickDriverClass ---'; cr.
    [| picked |
     picked := [OSWindowDriver pickDriverClass] on: Error do: [:e |
       stream nextPutAll: 'pickDriverClass ERROR: '; print: e messageText; cr.
       nil].
     picked ifNotNil: [
       stream nextPutAll: 'OSWindowDriver pickDriverClass: '; print: picked name; cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 4: Check OSWindowDriver current"
    stream cr; nextPutAll: '--- Step 4: OSWindowDriver current ---'; cr.
    [| current |
     current := [OSWindowDriver current] on: Error do: [:e |
       stream nextPutAll: 'current ERROR: '; print: e messageText; cr.
       nil].
     current ifNotNil: [
       stream nextPutAll: 'OSWindowDriver current: '; print: current class name; cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 5: Create OSiOSDriver instance"
    stream cr; nextPutAll: '--- Step 5: OSiOSDriver new ---'; cr.
    [| driver |
     driver := [OSiOSDriver new] on: Error do: [:e |
       stream nextPutAll: 'new ERROR: '; print: e messageText; cr.
       nil].
     driver ifNotNil: [
       stream nextPutAll: 'OSiOSDriver created: '; print: driver class name; cr.
       stream nextPutAll: 'setupEventLoop...'; cr.
       [driver setupEventLoop.
        stream nextPutAll: 'Event loop started'; cr.
        (Delay forMilliseconds: 100) wait.
        stream nextPutAll: 'EventLoopProcess: '; print: OSiOSDriver eventLoopProcess; cr]
       on: Error do: [:e |
         stream nextPutAll: 'setupEventLoop ERROR: '; print: e messageText; cr]]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!DriverSelectionDiagnostic class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!DriverSelectionDiagnostic class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #DriverSelectionDiagnostic)
  inCategory: SessionManager default userCategory
  atPriority: 90!
