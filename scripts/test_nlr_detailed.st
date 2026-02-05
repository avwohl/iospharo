"Detailed NLR tests to isolate the bug"!

Object subclass: #NLRDetailedTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!NLRDetailedTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/nlr_detailed.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== Detailed NLR Tests ==='; cr; cr.

    "Test 1: NLR with name check (no temp)"
    stream nextPutAll: '--- Test 1: NLR with name check (no temp) ---'; cr.
    [stream nextPutAll: 'test1 = '; print: self test1; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 2: NLR with name check stored in temp"
    stream cr; nextPutAll: '--- Test 2: NLR with name check (with temp) ---'; cr.
    [stream nextPutAll: 'test2 = '; print: self test2; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 3: NLR with isSuitable (no temp)"
    stream cr; nextPutAll: '--- Test 3: NLR with isSuitable (no temp) ---'; cr.
    [stream nextPutAll: 'test3 = '; print: self test3; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 4: NLR with isSuitable stored in temp"
    stream cr; nextPutAll: '--- Test 4: NLR with isSuitable (with temp) ---'; cr.
    [stream nextPutAll: 'test4 = '; print: self test4; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 5: NLR with isSuitable but double evaluation"
    stream cr; nextPutAll: '--- Test 5: NLR double eval (call isSuitable twice) ---'; cr.
    [stream nextPutAll: 'test5 = '; print: self test5; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 6: Simple do: with isSuitable NLR"
    stream cr; nextPutAll: '--- Test 6: do: with isSuitable NLR ---'; cr.
    [stream nextPutAll: 'test6 = '; print: self test6; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 7: Simple do: with isSuitable temp then NLR"
    stream cr; nextPutAll: '--- Test 7: do: with isSuitable temp then NLR ---'; cr.
    [stream nextPutAll: 'test7 = '; print: self test7; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 8: Check what isSuitable actually returns"
    stream cr; nextPutAll: '--- Test 8: What does isSuitable return? ---'; cr.
    [{OSNullWindowDriver. OSSDL2Driver. OSiOSDriver} do: [:c |
      | result |
      result := c isSuitable.
      stream nextPutAll: c name; nextPutAll: ' isSuitable returns: '; print: result; cr.
      stream nextPutAll: '  result class: '; print: result class; cr.
      stream nextPutAll: '  result == true: '; print: (result == true); cr.
      stream nextPutAll: '  result = true: '; print: (result = true); cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!NLRDetailedTest class methodsFor: 'tests'!
test1
  "NLR with name check - no temp"
  OSWindowDriver subclassesDo: [:aClass |
    aClass name = #OSiOSDriver ifTrue: [^ aClass]].
  ^ #notFound! !

!NLRDetailedTest class methodsFor: 'tests'!
test2
  "NLR with name check - with temp"
  OSWindowDriver subclassesDo: [:aClass |
    | matched |
    matched := aClass name = #OSiOSDriver.
    matched ifTrue: [^ aClass]].
  ^ #notFound! !

!NLRDetailedTest class methodsFor: 'tests'!
test3
  "NLR with isSuitable - no temp"
  OSWindowDriver subclassesDo: [:aClass |
    aClass isSuitable ifTrue: [^ aClass]].
  ^ #notFound! !

!NLRDetailedTest class methodsFor: 'tests'!
test4
  "NLR with isSuitable - with temp"
  OSWindowDriver subclassesDo: [:aClass |
    | suitable |
    suitable := aClass isSuitable.
    suitable ifTrue: [^ aClass]].
  ^ #notFound! !

!NLRDetailedTest class methodsFor: 'tests'!
test5
  "NLR with double isSuitable call"
  OSWindowDriver subclassesDo: [:aClass |
    aClass isSuitable.
    aClass isSuitable ifTrue: [^ aClass]].
  ^ #notFound! !

!NLRDetailedTest class methodsFor: 'tests'!
test6
  "do: with isSuitable NLR"
  {OSNullWindowDriver. OSSDL2Driver. OSiOSDriver} do: [:aClass |
    aClass isSuitable ifTrue: [^ aClass]].
  ^ #notFound! !

!NLRDetailedTest class methodsFor: 'tests'!
test7
  "do: with isSuitable temp then NLR"
  {OSNullWindowDriver. OSSDL2Driver. OSiOSDriver} do: [:aClass |
    | suitable |
    suitable := aClass isSuitable.
    suitable ifTrue: [^ aClass]].
  ^ #notFound! !

!NLRDetailedTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!NLRDetailedTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #NLRDetailedTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
