"Diagnose FFI backend selection"!

Object subclass: #FFIDiagnostic2
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!FFIDiagnostic2 class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/ffi_diagnostic2.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== FFI Backend Selection Diagnostic ==='; cr; cr.

    "Step 1: Check all FFIBackend subclasses"
    stream nextPutAll: '--- Step 1: FFIBackend subclasses ---'; cr.
    [FFIBackend allSubclasses do: [:c |
       stream nextPutAll: '  '; print: c name.
       stream nextPutAll: ' isAvailable='.
       [stream print: c isAvailable] on: Error do: [:e |
         stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'].
       stream cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'Step 1 ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 2: Check TFFIBackend specifically"
    stream cr; nextPutAll: '--- Step 2: TFFIBackend.isAvailable implementation ---'; cr.
    [| tfBackend |
     tfBackend := Smalltalk at: #TFFIBackend ifAbsent: [nil].
     tfBackend ifNil: [
       stream nextPutAll: 'TFFIBackend not found'; cr]
     ifNotNil: [
       stream nextPutAll: 'TFFIBackend exists'; cr.
       "Try to trace what isAvailable does"
       stream nextPutAll: 'Checking TFWorker...'; cr.
       [| worker |
        worker := Smalltalk at: #TFWorker ifAbsent: [nil].
        stream nextPutAll: '  TFWorker class: ';
          print: (worker ifNil: ['nil'] ifNotNil: [worker name]); cr.
        worker ifNotNil: [
          stream nextPutAll: '  TFWorker isAvailable: '.
          [stream print: worker isAvailable] on: Error do: [:e |
            stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'].
          stream cr]]
       on: Error do: [:e |
         stream nextPutAll: 'TFWorker check error: '; print: e messageText; cr]]]
    on: Error do: [:ex |
      stream nextPutAll: 'Step 2 ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 3: Check the actual TFFIBackend.isAvailable implementation"
    stream cr; nextPutAll: '--- Step 3: TFFIBackend>>isAvailable source ---'; cr.
    [| method |
     method := TFFIBackend class >> #isAvailable.
     stream nextPutAll: 'TFFIBackend class>>isAvailable source:'; cr.
     stream nextPutAll: method sourceCode; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Step 3 ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 4: Check what primitive TFFIBackend depends on"
    stream cr; nextPutAll: '--- Step 4: TFWorker primitives ---'; cr.
    [| worker |
     worker := Smalltalk at: #TFWorker ifAbsent: [nil].
     worker ifNotNil: [
       stream nextPutAll: 'TFWorker>>isAvailable source:'; cr.
       [(worker class >> #isAvailable) sourceCode lines do: [:line |
         stream nextPutAll: '  '; nextPutAll: line; cr]]
       on: Error do: [:e |
         stream nextPutAll: '  Error getting source: '; print: e messageText; cr]]]
    on: Error do: [:ex |
      stream nextPutAll: 'Step 4 ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 5: Check if primitive 174 (pluginAvailable) works"
    stream cr; nextPutAll: '--- Step 5: Primitive 174 test ---'; cr.
    [| result |
     result := [Smalltalk primitivePluginAvailable]
       on: Error do: [:e | 'ERROR: ', e messageText].
     stream nextPutAll: 'Smalltalk primitivePluginAvailable: '; print: result; cr.
     result := [Smalltalk primitiveSamplerCanSample]
       on: Error do: [:e | 'ERROR: ', e messageText].
     stream nextPutAll: 'Smalltalk primitiveSamplerCanSample: '; print: result; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Step 5 ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 6: Try calling TFWorker>>isWorkerRunning"
    stream cr; nextPutAll: '--- Step 6: TFWorker>>isWorkerRunning ---'; cr.
    [| worker result |
     worker := Smalltalk at: #TFWorker ifAbsent: [nil].
     worker ifNotNil: [
       result := [worker isWorkerRunning]
         on: Error do: [:e | 'ERROR: ', e messageText].
       stream nextPutAll: 'TFWorker isWorkerRunning: '; print: result; cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'Step 6 ERROR: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!FFIDiagnostic2 class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!FFIDiagnostic2 class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #FFIDiagnostic2)
  inCategory: SessionManager default userCategory
  atPriority: 90!
