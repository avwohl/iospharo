"Diagnose TFFIBackend availability check"!

Object subclass: #FFIDiagnostic3
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!FFIDiagnostic3 class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/ffi_diagnostic3.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== TFFIBackend isAvailable Diagnostic ==='; cr; cr.

    "Step 1: Check TFFIBackend>>isAvailable source"
    stream nextPutAll: '--- Step 1: TFFIBackend>>isAvailable ---'; cr.
    [| src |
     src := (TFFIBackend >> #isAvailable) sourceCode.
     stream nextPutAll: src; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Step 1 ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 2: Check what loadSymbol:module: does"
    stream cr; nextPutAll: '--- Step 2: TFFIBackend>>loadSymbol:module: ---'; cr.
    [| src |
     src := (TFFIBackend >> #loadSymbol:module:) sourceCode.
     stream nextPutAll: src; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Step 2 ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 3: Try to call loadSymbol directly"
    stream cr; nextPutAll: '--- Step 3: Try loadSymbol ---'; cr.
    [| backend result |
     backend := TFFIBackend new.
     stream nextPutAll: 'Created TFFIBackend instance'; cr.
     result := [backend loadSymbol: #primitiveLoadSymbolFromModule module: nil]
       on: Error do: [:e |
         stream nextPutAll: 'loadSymbol error: '; print: e class name;
           nextPutAll: ' - '; print: e messageText; cr.
         nil].
     stream nextPutAll: 'loadSymbol result: '; print: result; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Step 3 ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 4: Check ExternalLibrary"
    stream cr; nextPutAll: '--- Step 4: ExternalLibrary loadSymbol ---'; cr.
    [| lib result |
     lib := Smalltalk at: #ExternalLibrary ifAbsent: [nil].
     lib ifNil: [
       stream nextPutAll: 'ExternalLibrary not found'; cr]
     ifNotNil: [
       stream nextPutAll: 'ExternalLibrary exists'; cr.
       result := [lib loadSymbol: 'primitiveLoadSymbolFromModule' from: nil]
         on: Error do: [:e |
           stream nextPutAll: 'loadSymbol error: '; print: e messageText; cr.
           nil].
       stream nextPutAll: 'result: '; print: result; cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'Step 4 ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 5: Check primitive 576 directly (primitiveLoadSymbolFromModule)"
    stream cr; nextPutAll: '--- Step 5: Try primitive call directly ---'; cr.
    [| extAddr |
     "Create an ExternalAddress to hold the result"
     extAddr := ExternalAddress new.
     stream nextPutAll: 'Created ExternalAddress'; cr.

     "Try the primitive - it takes symbol name and module name"
     [extAddr loadSymbol: 'printf' from: nil.
      stream nextPutAll: 'loadSymbol:from: succeeded, addr='; print: extAddr; cr]
     on: Error do: [:e |
       stream nextPutAll: 'loadSymbol:from: failed: '; print: e messageText; cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'Step 5 ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 6: Check if primitive 576 is defined"
    stream cr; nextPutAll: '--- Step 6: Check primitive 576 method ---'; cr.
    [| method |
     "Check ExternalAddress>>loadSymbol:from:"
     method := ExternalAddress >> #loadSymbol:from:.
     stream nextPutAll: 'ExternalAddress>>loadSymbol:from: source:'; cr.
     method sourceCode lines do: [:line |
       stream nextPutAll: '  '; nextPutAll: line; cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'Step 6 ERROR: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!FFIDiagnostic3 class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!FFIDiagnostic3 class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #FFIDiagnostic3)
  inCategory: SessionManager default userCategory
  atPriority: 90!
