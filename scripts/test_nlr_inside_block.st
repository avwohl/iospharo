"Test NLR when SDL2 isAvailable is called inside vs outside block"!

Object subclass: #NLRInsideBlockTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!NLRInsideBlockTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/nlr_inside_block.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== NLR Inside Block Test ==='; cr; cr.

    "Test 1: SDL2 isAvailable outside block, NLR inside"
    stream nextPutAll: '--- Test 1: SDL2 isAvailable OUTSIDE block ---'; cr.
    [stream nextPutAll: 'test1 = '; print: self test1; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 2: SDL2 isAvailable inside block, NLR inside"
    stream cr; nextPutAll: '--- Test 2: SDL2 isAvailable INSIDE block ---'; cr.
    [stream nextPutAll: 'test2 = '; print: self test2; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 3: Any exception-throwing code inside block"
    stream cr; nextPutAll: '--- Test 3: Error-throwing inside block ---'; cr.
    [stream nextPutAll: 'test3 = '; print: self test3; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 4: onErrorDo: inside block"
    stream cr; nextPutAll: '--- Test 4: onErrorDo: INSIDE block ---'; cr.
    [stream nextPutAll: 'test4 = '; print: self test4; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 5: on:do: inside block"
    stream cr; nextPutAll: '--- Test 5: on:do: INSIDE block ---'; cr.
    [stream nextPutAll: 'test5 = '; print: self test5; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 6: Nested blocks with exception"
    stream cr; nextPutAll: '--- Test 6: Nested blocks with exception ---'; cr.
    [stream nextPutAll: 'test6 = '; print: self test6; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 7: Simple nested block"
    stream cr; nextPutAll: '--- Test 7: Simple nested block ---'; cr.
    [stream nextPutAll: 'test7 = '; print: self test7; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!NLRInsideBlockTest class methodsFor: 'tests'!
test1
  "SDL2 isAvailable called OUTSIDE the do: block"
  | avail |
  avail := SDL2 isAvailable.
  #(1 2 3) do: [:x | x = 2 ifTrue: [^ #found]].
  ^ #notFound! !

!NLRInsideBlockTest class methodsFor: 'tests'!
test2
  "SDL2 isAvailable called INSIDE the do: block"
  #(1 2 3) do: [:x |
    SDL2 isAvailable.
    x = 2 ifTrue: [^ #found]].
  ^ #notFound! !

!NLRInsideBlockTest class methodsFor: 'tests'!
test3
  "Error-throwing code inside block, caught"
  #(1 2 3) do: [:x |
    [self error: 'test'] on: Error do: [:e | ].
    x = 2 ifTrue: [^ #found]].
  ^ #notFound! !

!NLRInsideBlockTest class methodsFor: 'tests'!
test4
  "onErrorDo: inside do: block"
  #(1 2 3) do: [:x |
    [1 / 0] onErrorDo: [:e | 0].
    x = 2 ifTrue: [^ #found]].
  ^ #notFound! !

!NLRInsideBlockTest class methodsFor: 'tests'!
test5
  "on:do: inside do: block"
  #(1 2 3) do: [:x |
    [1 / 0] on: ZeroDivide do: [:e | 0].
    x = 2 ifTrue: [^ #found]].
  ^ #notFound! !

!NLRInsideBlockTest class methodsFor: 'tests'!
test6
  "Nested block structure similar to SDL2 isAvailable"
  #(1 2 3) do: [:x |
    | result |
    result := [self dummyWork] onErrorDo: [:e | false].
    x = 2 ifTrue: [^ #found]].
  ^ #notFound! !

!NLRInsideBlockTest class methodsFor: 'tests'!
test7
  "Simple nested block"
  #(1 2 3) do: [:x |
    [:y | y + 1] value: x.
    x = 2 ifTrue: [^ #found]].
  ^ #notFound! !

!NLRInsideBlockTest class methodsFor: 'helpers'!
dummyWork
  ^ 42! !

!NLRInsideBlockTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!NLRInsideBlockTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #NLRInsideBlockTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
