"Diagnose SDL2 initialization"!

Object subclass: #SDLDiagnostic
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!SDLDiagnostic class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/sdl_diagnostic.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== SDL2 Diagnostic ==='; cr; cr.

    "Step 1: Load SDL2 module"
    stream nextPutAll: '--- Step 1: Load SDL2 module ---'; cr.
    [| backend handle |
     backend := TFFIBackend new.
     stream nextPutAll: 'TFFIBackend instance created'; cr.

     "Try to load the SDL2 module"
     stream nextPutAll: 'Loading SDL2 module...'; cr.
     [handle := backend loadSymbol: 'SDL_Init' module: 'SDL2'.
      stream nextPutAll: 'SDL_Init address: '; print: handle; cr]
     on: Error do: [:e |
       stream nextPutAll: 'loadSymbol error: '; print: e messageText; cr.
       "Try without module"
       stream nextPutAll: 'Trying with nil module...'; cr.
       [handle := backend loadSymbol: 'SDL_Init' module: nil.
        stream nextPutAll: 'SDL_Init address (nil module): '; print: handle; cr]
       on: Error do: [:e2 |
         stream nextPutAll: 'Also failed: '; print: e2 messageText; cr]]]
    on: Error do: [:ex |
      stream nextPutAll: 'Step 1 ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 2: Check SDL2 class"
    stream cr; nextPutAll: '--- Step 2: SDL2 class ---'; cr.
    [| sdl2 |
     sdl2 := Smalltalk at: #SDL2 ifAbsent: [nil].
     sdl2 ifNil: [
       stream nextPutAll: 'SDL2 class NOT FOUND'; cr]
     ifNotNil: [
       stream nextPutAll: 'SDL2 class exists'; cr.
       stream nextPutAll: 'SDL2>>sdlInit: source:'; cr.
       [(sdl2 class >> #sdlInit:) sourceCode lines first: 5 do: [:line |
         stream nextPutAll: '  '; nextPutAll: line; cr]]
       on: Error do: [:e |
         stream nextPutAll: '  Error getting source: '; print: e messageText; cr]]]
    on: Error do: [:ex |
      stream nextPutAll: 'Step 2 ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 3: Try calling SDL_Init directly via FFI"
    stream cr; nextPutAll: '--- Step 3: SDL2 sdlInit: ---'; cr.
    [| result |
     stream nextPutAll: 'Calling SDL2 sdlInit: 0...'; cr.
     result := [SDL2 sdlInit: 0] on: Error do: [:e |
       stream nextPutAll: '  Error: '; print: e class name; nextPutAll: ' - '; print: e messageText; cr.
       nil].
     stream nextPutAll: 'Result: '; print: result; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Step 3 ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 4: Check OSSDL2Driver>>isApplicableFor:"
    stream cr; nextPutAll: '--- Step 4: OSSDL2Driver>>isApplicableFor: ---'; cr.
    [| src |
     src := (OSSDL2Driver class >> #isApplicableFor:) sourceCode.
     stream nextPutAll: 'Source:'; cr.
     src lines first: 20 do: [:line |
       stream nextPutAll: '  '; nextPutAll: line; cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'Step 4 ERROR: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!SDLDiagnostic class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!SDLDiagnostic class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #SDLDiagnostic)
  inCategory: SessionManager default userCategory
  atPriority: 90!
