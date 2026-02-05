"Test non-local returns from blocks"!

Object subclass: #NLRTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!NLRTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/nlr_test.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== Non-Local Return Test ==='; cr; cr.

    "Test 1: Simple NLR from ifTrue:"
    stream nextPutAll: '--- Test 1: NLR from ifTrue: ---'; cr.
    [| result |
     result := self testNLRIfTrue.
     stream nextPutAll: 'testNLRIfTrue result: '; print: result; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 2: NLR from do: block"
    stream cr; nextPutAll: '--- Test 2: NLR from do: ---'; cr.
    [| result |
     result := self testNLRFromDo.
     stream nextPutAll: 'testNLRFromDo result: '; print: result; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 3: NLR from nested do: and ifTrue:"
    stream cr; nextPutAll: '--- Test 3: NLR from nested do: ifTrue: ---'; cr.
    [| result |
     result := self testNLRNestedDoIfTrue.
     stream nextPutAll: 'testNLRNestedDoIfTrue result: '; print: result; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 4: NLR from subclassesDo:"
    stream cr; nextPutAll: '--- Test 4: NLR from subclassesDo: ---'; cr.
    [| result |
     result := self testNLRFromSubclassesDo.
     stream nextPutAll: 'testNLRFromSubclassesDo result: '; print: result; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 5: NLR with isSuitable check - simulating pickDriverClass"
    stream cr; nextPutAll: '--- Test 5: Simulated pickDriverClass ---'; cr.
    [| result |
     result := self testSimulatedPickDriverClass.
     stream nextPutAll: 'testSimulatedPickDriverClass result: '; print: result; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 6: Direct pickDriverClass"
    stream cr; nextPutAll: '--- Test 6: Direct pickDriverClass ---'; cr.
    [| result |
     result := OSWindowDriver pickDriverClass.
     stream nextPutAll: 'OSWindowDriver pickDriverClass: '; print: result; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 7: Manual iteration with explicit NLR check"
    stream cr; nextPutAll: '--- Test 7: Manual iteration with tracing ---'; cr.
    [| foundDriver |
     foundDriver := nil.
     OSWindowDriver subclassesDo: [:aClass |
       stream nextPutAll: '  Checking: '; print: aClass; cr.
       stream flush.
       stream nextPutAll: '    isSuitable = '.
       [| suitable |
        suitable := aClass isSuitable.
        stream print: suitable; cr.
        suitable ifTrue: [
          stream nextPutAll: '    FOUND SUITABLE: '; print: aClass; cr.
          foundDriver := aClass]]
       on: Error do: [:e |
         stream nextPutAll: 'ERROR: '; print: e messageText; cr]].
     stream nextPutAll: 'Manual search found: '; print: foundDriver; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!NLRTest class methodsFor: 'tests'!
testNLRIfTrue
  "Simple NLR from ifTrue: block"
  true ifTrue: [^ #found].
  ^ #notFound! !

!NLRTest class methodsFor: 'tests'!
testNLRFromDo
  "NLR from do: block"
  #(1 2 3 4 5) do: [:each |
    each = 3 ifTrue: [^ each]].
  ^ #notFound! !

!NLRTest class methodsFor: 'tests'!
testNLRNestedDoIfTrue
  "NLR from nested do: and ifTrue:"
  #(#a #b #c) do: [:each |
    each = #b ifTrue: [^ each]].
  ^ #notFound! !

!NLRTest class methodsFor: 'tests'!
testNLRFromSubclassesDo
  "NLR from subclassesDo:"
  OSWindowDriver subclassesDo: [:aClass |
    aClass name = #OSiOSDriver ifTrue: [^ aClass]].
  ^ #notFound! !

!NLRTest class methodsFor: 'tests'!
testSimulatedPickDriverClass
  "Simulate pickDriverClass logic"
  OSWindowDriver subclassesDo: [:aClass |
    aClass isSuitable ifTrue: [^ aClass]].
  ^ OSNullWindowDriver! !

!NLRTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!NLRTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #NLRTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
