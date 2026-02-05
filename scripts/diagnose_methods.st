"Diagnose method dictionary issues"!

Object subclass: #MethodDiagnostic
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!MethodDiagnostic class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/method_diagnostic.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== Method Dictionary Diagnostic ==='; cr; cr.

    "Step 1: Check a simple class"
    stream nextPutAll: '--- Step 1: Object class ---'; cr.
    [| cls md |
     cls := Object class.
     stream nextPutAll: 'Object class: '; print: cls; cr.
     md := cls methodDict.
     stream nextPutAll: 'methodDict class: '; print: md class name; cr.
     stream nextPutAll: 'methodDict size: '; print: md size; cr.
     stream nextPutAll: 'includes #new: '; print: (md includesKey: #new); cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 2: Check canUnderstand:"
    stream cr; nextPutAll: '--- Step 2: canUnderstand: ---'; cr.
    [stream nextPutAll: 'Object class canUnderstand: #new: '; print: (Object class canUnderstand: #new); cr.
     stream nextPutAll: 'Object canUnderstand: #yourself: '; print: (Object canUnderstand: #yourself); cr.
     stream nextPutAll: 'OSSDL2Driver class canUnderstand: #isApplicableFor:: '; print: (OSSDL2Driver class canUnderstand: #isApplicableFor:); cr.
     stream nextPutAll: 'SDL2 class canUnderstand: #sdlInit:: '; print: (SDL2 class canUnderstand: #sdlInit:); cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 3: Check OSSDL2Driver method dict"
    stream cr; nextPutAll: '--- Step 3: OSSDL2Driver method dict ---'; cr.
    [| cls md |
     cls := OSSDL2Driver class.
     stream nextPutAll: 'OSSDL2Driver class: '; print: cls; cr.
     md := cls methodDict.
     stream nextPutAll: 'methodDict class: '; print: md class name; cr.
     stream nextPutAll: 'methodDict size: '; print: md size; cr.
     stream nextPutAll: 'First 10 keys: '.
     md keys first: 10 do: [:k | stream print: k; space].
     stream cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 4: Check lookupSelector"
    stream cr; nextPutAll: '--- Step 4: lookupSelector: ---'; cr.
    [| method |
     stream nextPutAll: 'OSSDL2Driver class lookupSelector: #isApplicableFor:'; cr.
     method := OSSDL2Driver class lookupSelector: #isApplicableFor:.
     stream nextPutAll: '  result: '; print: method; cr.
     method ifNotNil: [
       stream nextPutAll: '  method class: '; print: method class name; cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 5: Check SDL2"
    stream cr; nextPutAll: '--- Step 5: SDL2 ---'; cr.
    [| cls md |
     cls := SDL2 class.
     stream nextPutAll: 'SDL2 class: '; print: cls; cr.
     md := cls methodDict.
     stream nextPutAll: 'methodDict size: '; print: md size; cr.
     stream nextPutAll: 'includes #sdlInit:: '; print: (md includesKey: #sdlInit:); cr.
     (md includesKey: #sdlInit:)
       ifTrue: [
         stream nextPutAll: 'Method for #sdlInit:: '; print: (md at: #sdlInit:); cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 6: Direct message send"
    stream cr; nextPutAll: '--- Step 6: Direct message sends ---'; cr.
    [| inst |
     stream nextPutAll: 'Creating OSSDL2Driver new...'; cr.
     [inst := OSSDL2Driver new.
      stream nextPutAll: '  Success: '; print: inst; cr]
     on: Error do: [:e |
       stream nextPutAll: '  Error: '; print: e class name; nextPutAll: ' - '; print: e messageText; cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!MethodDiagnostic class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!MethodDiagnostic class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #MethodDiagnostic)
  inCategory: SessionManager default userCategory
  atPriority: 90!
