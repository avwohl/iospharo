"Diagnose driver isSuitable"!

Object subclass: #SuitableDiagnostic
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!SuitableDiagnostic class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/suitable_diagnostic.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== Driver isSuitable Diagnostic ==='; cr; cr.

    "Step 1: Check isSuitable for all drivers"
    stream nextPutAll: '--- Step 1: isSuitable for drivers ---'; cr.
    [OSWindowDriver allSubclasses do: [:c |
       stream nextPutAll: c name; nextPutAll: ' isSuitable: '.
       [stream print: c isSuitable] on: Error do: [:e |
         stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'].
       stream cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 2: Check OSSDL2Driver>>isSuitable source"
    stream cr; nextPutAll: '--- Step 2: OSSDL2Driver>>isSuitable source ---'; cr.
    [(OSSDL2Driver class >> #isSuitable) sourceCode lines do: [:line |
       stream nextPutAll: '  '; nextPutAll: line; cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 3: Check what isSuitable actually does"
    stream cr; nextPutAll: '--- Step 3: Trace OSSDL2Driver>>isSuitable ---'; cr.
    [stream nextPutAll: 'Checking OSPlatform current...'; cr.
     stream nextPutAll: '  OSPlatform current: '; print: OSPlatform current class name; cr.
     stream nextPutAll: '  isWindows: '; print: OSPlatform current isWindows; cr.
     stream nextPutAll: '  isMacOSX: '; print: OSPlatform current isMacOSX; cr.
     stream nextPutAll: '  isUnix: '; print: OSPlatform current isUnix; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 4: Check isSupported"
    stream cr; nextPutAll: '--- Step 4: isSupported ---'; cr.
    [stream nextPutAll: 'OSSDL2Driver isSupported: '.
     [stream print: OSSDL2Driver isSupported] on: Error do: [:e |
       stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'].
     stream cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 5: Check OSWindowDriver current"
    stream cr; nextPutAll: '--- Step 5: OSWindowDriver current ---'; cr.
    [| current |
     current := [OSWindowDriver current] on: Error do: [:e |
       stream nextPutAll: 'ERROR getting current: '; print: e messageText; cr.
       nil].
     current ifNotNil: [
       stream nextPutAll: 'OSWindowDriver current: '; print: current class name; cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!SuitableDiagnostic class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!SuitableDiagnostic class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #SuitableDiagnostic)
  inCategory: SessionManager default userCategory
  atPriority: 90!
