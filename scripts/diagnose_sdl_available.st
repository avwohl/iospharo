"Diagnose SDL2 isAvailable"!

Object subclass: #SDL2AvailableDiagnostic
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!SDL2AvailableDiagnostic class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/sdl_available.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== SDL2 isAvailable Diagnostic ==='; cr; cr.

    "Step 1: Check SDL2 isAvailable"
    stream nextPutAll: '--- Step 1: SDL2 isAvailable ---'; cr.
    [stream nextPutAll: 'SDL2 isAvailable: '.
     [stream print: SDL2 isAvailable] on: Error do: [:e |
       stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'].
     stream cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 2: Check SDL2Library"
    stream cr; nextPutAll: '--- Step 2: SDL2Library ---'; cr.
    [| lib |
     lib := Smalltalk at: #SDL2Library ifAbsent: [nil].
     lib ifNil: [
       stream nextPutAll: 'SDL2Library NOT FOUND'; cr]
     ifNotNil: [
       stream nextPutAll: 'SDL2Library exists'; cr.
       stream nextPutAll: 'uniqueInstance: '.
       [| inst |
        inst := lib uniqueInstance.
        stream print: inst class name; cr.
        stream nextPutAll: 'libraryName: '.
        [stream print: inst libraryName; cr] on: Error do: [:e |
          stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'; cr]]
       on: Error do: [:e |
         stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'; cr]]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 3: Try ExternalAddress loadSymbol:from:"
    stream cr; nextPutAll: '--- Step 3: ExternalAddress loadSymbol:from: ---'; cr.
    [| libName addr |
     libName := SDL2Library uniqueInstance libraryName.
     stream nextPutAll: 'Library name: '; print: libName; cr.

     stream nextPutAll: 'Loading SDL_Init from library...'; cr.
     [addr := ExternalAddress loadSymbol: 'SDL_Init' from: libName.
      stream nextPutAll: 'Result: '; print: addr; cr.
      stream nextPutAll: 'isNotNil: '; print: addr isNotNil; cr]
     on: Error do: [:e |
       stream nextPutAll: 'ERROR: '; print: e class name; nextPutAll: ' - '; print: e messageText; cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 4: Try loading from nil (main module)"
    stream cr; nextPutAll: '--- Step 4: Load from nil (main module) ---'; cr.
    [| addr |
     stream nextPutAll: 'Loading SDL_Init from nil (main module)...'; cr.
     [addr := ExternalAddress loadSymbol: 'SDL_Init' from: nil.
      stream nextPutAll: 'Result: '; print: addr; cr]
     on: Error do: [:e |
       stream nextPutAll: 'ERROR: '; print: e class name; nextPutAll: ' - '; print: e messageText; cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 5: Check ExternalAddress>>loadSymbol:from: method"
    stream cr; nextPutAll: '--- Step 5: ExternalAddress>>loadSymbol:from: source ---'; cr.
    [(ExternalAddress class >> #loadSymbol:from:) sourceCode lines do: [:line |
       stream nextPutAll: '  '; nextPutAll: line; cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!SDL2AvailableDiagnostic class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!SDL2AvailableDiagnostic class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #SDL2AvailableDiagnostic)
  inCategory: SessionManager default userCategory
  atPriority: 90!
