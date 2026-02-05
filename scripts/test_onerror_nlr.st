"Test if onErrorDo: breaks NLR"!

Object subclass: #OnErrorNLRTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!OnErrorNLRTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/onerror_nlr.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== onErrorDo: and NLR Test ==='; cr; cr.

    "Test 1: NLR without onErrorDo:"
    stream nextPutAll: '--- Test 1: NLR without onErrorDo: ---'; cr.
    [stream nextPutAll: 'test1 = '; print: self test1; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 2: NLR after onErrorDo: block"
    stream cr; nextPutAll: '--- Test 2: NLR after onErrorDo: ---'; cr.
    [stream nextPutAll: 'test2 = '; print: self test2; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 3: NLR after on:do: block"
    stream cr; nextPutAll: '--- Test 3: NLR after on:do: ---'; cr.
    [stream nextPutAll: 'test3 = '; print: self test3; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 4: NLR using on:do: directly in condition"
    stream cr; nextPutAll: '--- Test 4: NLR with on:do: in condition ---'; cr.
    [stream nextPutAll: 'test4 = '; print: self test4; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 5: NLR after ifError:"
    stream cr; nextPutAll: '--- Test 5: NLR after ifError: ---'; cr.
    [stream nextPutAll: 'test5 = '; print: self test5; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 6: What does onErrorDo: do?"
    stream cr; nextPutAll: '--- Test 6: onErrorDo: analysis ---'; cr.
    [| method |
     method := BlockClosure lookupSelector: #onErrorDo:.
     stream nextPutAll: 'BlockClosure >> #onErrorDo: defined in: '.
     method ifNil: [stream nextPutAll: 'NOT FOUND']
            ifNotNil: [stream print: method methodClass; cr.
                       stream nextPutAll: 'Source: '; cr; nextPutAll: method sourceCode].
     stream cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 7: Simplified isAvailable without onErrorDo:"
    stream cr; nextPutAll: '--- Test 7: Simplified isAvailable check ---'; cr.
    [stream nextPutAll: 'test7 = '; print: self test7; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 8: Simple exception handling"
    stream cr; nextPutAll: '--- Test 8: Simple exception handling ---'; cr.
    [stream nextPutAll: 'test8 = '; print: self test8; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!OnErrorNLRTest class methodsFor: 'tests'!
test1
  "NLR without any exception handling"
  #(1 2 3) do: [:x | x = 2 ifTrue: [^ #found]].
  ^ #notFound! !

!OnErrorNLRTest class methodsFor: 'tests'!
test2
  "NLR after using onErrorDo:"
  | result |
  result := [1 + 1] onErrorDo: [:e | 0].
  #(1 2 3) do: [:x | x = 2 ifTrue: [^ #found]].
  ^ #notFound! !

!OnErrorNLRTest class methodsFor: 'tests'!
test3
  "NLR after using on:do:"
  | result |
  result := [1 + 1] on: Error do: [:e | 0].
  #(1 2 3) do: [:x | x = 2 ifTrue: [^ #found]].
  ^ #notFound! !

!OnErrorNLRTest class methodsFor: 'tests'!
test4
  "NLR with on:do: wrapped condition"
  #(1 2 3) do: [:x |
    | check |
    check := [x = 2] on: Error do: [:e | false].
    check ifTrue: [^ #found]].
  ^ #notFound! !

!OnErrorNLRTest class methodsFor: 'tests'!
test5
  "NLR after ifError:"
  | result |
  result := [1 + 1] ifError: [:msg :rcvr | 0].
  #(1 2 3) do: [:x | x = 2 ifTrue: [^ #found]].
  ^ #notFound! !

!OnErrorNLRTest class methodsFor: 'tests'!
test7
  "Simplified isAvailable - just return false directly"
  {OSNullWindowDriver. OSSDL2Driver. OSiOSDriver} do: [:aClass |
    | avail |
    avail := false. "Hardcoded - no FFI"
    avail not ifTrue: [^ aClass]].
  ^ #notFound! !

!OnErrorNLRTest class methodsFor: 'tests'!
test8
  "Simple exception test - does exception handling work at all?"
  | result |
  result := [1 / 0] on: ZeroDivide do: [:e | #caught].
  ^ result! !

!OnErrorNLRTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!OnErrorNLRTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #OnErrorNLRTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
