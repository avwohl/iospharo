"Test if raising exception inside do: block breaks NLR"!

Object subclass: #ExceptionNLRTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!ExceptionNLRTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/exception_nlr.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== Exception and NLR Test ==='; cr; cr.

    "Test 1: on:do: but no exception raised"
    stream nextPutAll: '--- Test 1: on:do: NO exception raised ---'; cr.
    [stream nextPutAll: 'test1 = '; print: self test1; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 2: on:do: with exception raised on first iteration"
    stream cr; nextPutAll: '--- Test 2: on:do: exception on iteration 1 ---'; cr.
    [stream nextPutAll: 'test2 = '; print: self test2; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 3: on:do: with exception raised on later iteration"
    stream cr; nextPutAll: '--- Test 3: on:do: exception on iteration 3 ---'; cr.
    [stream nextPutAll: 'test3 = '; print: self test3; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 4: Exception raised BEFORE NLR iteration"
    stream cr; nextPutAll: '--- Test 4: Exception before NLR iteration ---'; cr.
    [stream nextPutAll: 'test4 = '; print: self test4; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 5: Multiple exceptions"
    stream cr; nextPutAll: '--- Test 5: Multiple exceptions ---'; cr.
    [stream nextPutAll: 'test5 = '; print: self test5; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 6: Conditional exception (only on some iterations)"
    stream cr; nextPutAll: '--- Test 6: Conditional exception ---'; cr.
    [stream nextPutAll: 'test6 = '; print: self test6; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!ExceptionNLRTest class methodsFor: 'tests'!
test1
  "on:do: without exception - should work"
  #(1 2 3 4 5) do: [:x |
    [x + 1] on: Error do: [:e | 0].
    x = 3 ifTrue: [^ #found]].
  ^ #notFound! !

!ExceptionNLRTest class methodsFor: 'tests'!
test2
  "Exception on first iteration - NLR on iteration 3"
  #(1 2 3 4 5) do: [:x |
    [x = 1 ifTrue: [self error: 'boom']] on: Error do: [:e | ].
    x = 3 ifTrue: [^ #found]].
  ^ #notFound! !

!ExceptionNLRTest class methodsFor: 'tests'!
test3
  "Exception on iteration 3, NLR on iteration 4"
  #(1 2 3 4 5) do: [:x |
    [x = 3 ifTrue: [self error: 'boom']] on: Error do: [:e | ].
    x = 4 ifTrue: [^ #found]].
  ^ #notFound! !

!ExceptionNLRTest class methodsFor: 'tests'!
test4
  "Exception on iteration 1, NLR on iteration 1"
  #(1 2 3 4 5) do: [:x |
    [x = 1 ifTrue: [self error: 'boom']] on: Error do: [:e | ].
    x = 1 ifTrue: [^ #foundAfterException]].
  ^ #notFound! !

!ExceptionNLRTest class methodsFor: 'tests'!
test5
  "Multiple exceptions, NLR on iteration 5"
  #(1 2 3 4 5) do: [:x |
    [x odd ifTrue: [self error: 'boom']] on: Error do: [:e | ].
    x = 5 ifTrue: [^ #found]].
  ^ #notFound! !

!ExceptionNLRTest class methodsFor: 'tests'!
test6
  "NLR BEFORE any exception happens"
  #(1 2 3 4 5) do: [:x |
    x = 2 ifTrue: [^ #foundEarly].
    [x = 3 ifTrue: [self error: 'boom']] on: Error do: [:e | ]].
  ^ #notFound! !

!ExceptionNLRTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!ExceptionNLRTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #ExceptionNLRTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
