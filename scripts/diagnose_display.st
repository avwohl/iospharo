"Diagnose display driver selection"!

Object subclass: #DisplayDiagnostic
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!DisplayDiagnostic class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/display_diagnostic.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== Display Driver Diagnostic ==='; cr; cr.

    "Step 1: Check FFIBackend"
    stream nextPutAll: '--- Step 1: FFI Backend ---'; cr.
    [stream nextPutAll: 'FFIBackend current: '; print: FFIBackend current class name; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 2: Check OSWindowDriver"
    stream cr; nextPutAll: '--- Step 2: OSWindowDriver ---'; cr.
    [| current |
     current := OSWindowDriver current.
     stream nextPutAll: 'OSWindowDriver current: '; print: current class name; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 3: Check available drivers"
    stream cr; nextPutAll: '--- Step 3: Available drivers ---'; cr.
    [OSWindowDriver allSubclasses do: [:c |
       stream nextPutAll: '  '; print: c name; nextPutAll: ' isApplicable: '.
       [stream print: (c isApplicableFor: nil)] on: Error do: [:e |
         stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'].
       stream cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 4: Check OSSDL2Driver specifically"
    stream cr; nextPutAll: '--- Step 4: OSSDL2Driver ---'; cr.
    [| driver |
     driver := Smalltalk at: #OSSDL2Driver ifAbsent: [nil].
     driver ifNil: [
       stream nextPutAll: 'OSSDL2Driver NOT FOUND'; cr]
     ifNotNil: [
       stream nextPutAll: 'OSSDL2Driver exists'; cr.
       stream nextPutAll: 'isApplicableFor: '.
       [stream print: (driver isApplicableFor: nil)] on: Error do: [:e |
         stream nextPutAll: '(ERROR: '; print: e class name; nextPutAll: ' - '; print: e messageText; nextPutAll: ')'].
       stream cr.

       "Try to create an instance"
       stream nextPutAll: 'Trying driver new... '.
       [| inst |
        inst := driver new.
        stream nextPutAll: 'SUCCESS: '; print: inst class name; cr]
       on: Error do: [:e |
         stream nextPutAll: 'FAILED: '; print: e messageText; cr]]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 5: WorldRenderer"
    stream cr; nextPutAll: '--- Step 5: WorldRenderer ---'; cr.
    [| wr |
     wr := World worldState worldRenderer.
     stream nextPutAll: 'WorldRenderer: '; print: wr class name; cr.
     stream nextPutAll: 'driver: '.
     [stream print: wr driver class name] on: Error do: [:e |
       stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'].
     stream cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!DisplayDiagnostic class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!DisplayDiagnostic class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #DisplayDiagnostic)
  inCategory: SessionManager default userCategory
  atPriority: 90!
