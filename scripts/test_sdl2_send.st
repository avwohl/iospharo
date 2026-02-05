"Test sending to SDL2"!

Object subclass: #SDL2SendTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!SDL2SendTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/sdl2_send.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== SDL2 Send Test ==='; cr; cr.

    "Test 1: Direct SDL2 sends"
    stream nextPutAll: '--- Test 1: Direct SDL2 sends ---'; cr.
    [stream nextPutAll: 'SDL2: '; print: SDL2; cr.
     stream nextPutAll: 'SDL2 class: '; print: SDL2 class; cr.
     stream nextPutAll: 'SDL2 isAvailable: '.
     [stream print: SDL2 isAvailable] on: Error do: [:e |
       stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'].
     stream cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 2: isSuitable on OSiOSDriver"
    stream cr; nextPutAll: '--- Test 2: isSuitable on class ---'; cr.
    [stream nextPutAll: 'OSiOSDriver isSuitable: '.
     [stream print: OSiOSDriver isSuitable] on: Error do: [:e |
       stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'].
     stream cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 3: isSuitable on metaclass"
    stream cr; nextPutAll: '--- Test 3: isSuitable on metaclass ---'; cr.
    [| mc |
     mc := OSiOSDriver class.
     stream nextPutAll: 'mc = '; print: mc; cr.
     stream nextPutAll: 'mc isSuitable: '.
     [stream print: mc isSuitable] on: Error do: [:e |
       stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'].
     stream cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 4: Check Smalltalk global SDL2"
    stream cr; nextPutAll: '--- Test 4: Smalltalk at: SDL2 ---'; cr.
    [| sdl2Var |
     sdl2Var := Smalltalk at: #SDL2 ifAbsent: [nil].
     stream nextPutAll: 'Smalltalk at: #SDL2: '; print: sdl2Var; cr.
     sdl2Var ifNotNil: [
       stream nextPutAll: 'sdl2Var class: '; print: sdl2Var class; cr.
       stream nextPutAll: 'sdl2Var isAvailable: '.
       [stream print: sdl2Var isAvailable] on: Error do: [:e |
         stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'].
       stream cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 5: Compare global lookup"
    stream cr; nextPutAll: '--- Test 5: Global lookup ---'; cr.
    [stream nextPutAll: 'SDL2 == (Smalltalk at: #SDL2): '; print: (SDL2 == (Smalltalk at: #SDL2)); cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!SDL2SendTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!SDL2SendTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #SDL2SendTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
