"Test individual components of SDL2 isAvailable"!

Object subclass: #SDL2ComponentsTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!SDL2ComponentsTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/sdl2_components.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== SDL2 Components Test ==='; cr; cr.

    "Test 1: NLR after SDL2Library uniqueInstance"
    stream nextPutAll: '--- Test 1: NLR after SDL2Library uniqueInstance ---'; cr.
    [stream nextPutAll: 'test1 = '; print: self test1; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 2: NLR after SDL2Library uniqueInstance libraryName"
    stream cr; nextPutAll: '--- Test 2: NLR after SDL2Library uniqueInstance libraryName ---'; cr.
    [stream nextPutAll: 'test2 = '; print: self test2; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 3: NLR after ExternalAddress loadSymbol:from:"
    stream cr; nextPutAll: '--- Test 3: NLR after ExternalAddress loadSymbol:from: ---'; cr.
    [stream nextPutAll: 'test3 = '; print: self test3; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 4: NLR after full SDL2 isAvailable simulation"
    stream cr; nextPutAll: '--- Test 4: NLR after full SDL2 isAvailable simulation ---'; cr.
    [stream nextPutAll: 'test4 = '; print: self test4; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 5: What is SDL2Library uniqueInstance?"
    stream cr; nextPutAll: '--- Test 5: SDL2Library analysis ---'; cr.
    [| lib |
     lib := SDL2Library uniqueInstance.
     stream nextPutAll: 'SDL2Library uniqueInstance = '; print: lib; cr.
     stream nextPutAll: 'class: '; print: lib class; cr.
     stream nextPutAll: 'libraryName = '; print: lib libraryName; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 6: NLR after FFILibrary uniqueInstance"
    stream cr; nextPutAll: '--- Test 6: NLR after any FFILibrary uniqueInstance ---'; cr.
    [stream nextPutAll: 'test6 = '; print: self test6; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 7: NLR after calling any class method"
    stream cr; nextPutAll: '--- Test 7: NLR after Object class method ---'; cr.
    [stream nextPutAll: 'test7 = '; print: self test7; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!SDL2ComponentsTest class methodsFor: 'tests'!
test1
  "NLR after SDL2Library uniqueInstance"
  | lib |
  lib := SDL2Library uniqueInstance.
  #(1 2 3) do: [:x | x = 2 ifTrue: [^ #found]].
  ^ #notFound! !

!SDL2ComponentsTest class methodsFor: 'tests'!
test2
  "NLR after SDL2Library uniqueInstance libraryName"
  | name |
  name := SDL2Library uniqueInstance libraryName.
  #(1 2 3) do: [:x | x = 2 ifTrue: [^ #found]].
  ^ #notFound! !

!SDL2ComponentsTest class methodsFor: 'tests'!
test3
  "NLR after ExternalAddress loadSymbol:from:"
  | addr |
  [addr := ExternalAddress loadSymbol: 'SDL_Init' from: SDL2Library uniqueInstance libraryName]
    on: Error do: [:e | addr := nil].
  #(1 2 3) do: [:x | x = 2 ifTrue: [^ #found]].
  ^ #notFound! !

!SDL2ComponentsTest class methodsFor: 'tests'!
test4
  "Full SDL2 isAvailable simulation"
  | avail |
  avail := [ (ExternalAddress loadSymbol: 'SDL_Init' from: SDL2Library uniqueInstance libraryName) isNotNil ]
    onErrorDo: [ false ].
  #(1 2 3) do: [:x | x = 2 ifTrue: [^ #found]].
  ^ #notFound! !

!SDL2ComponentsTest class methodsFor: 'tests'!
test6
  "NLR after any FFILibrary uniqueInstance"
  | lib |
  lib := FFILibrary uniqueInstance.
  #(1 2 3) do: [:x | x = 2 ifTrue: [^ #found]].
  ^ #notFound! !

!SDL2ComponentsTest class methodsFor: 'tests'!
test7
  "NLR after calling class method"
  | val |
  val := Object new.
  #(1 2 3) do: [:x | x = 2 ifTrue: [^ #found]].
  ^ #notFound! !

!SDL2ComponentsTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!SDL2ComponentsTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #SDL2ComponentsTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
