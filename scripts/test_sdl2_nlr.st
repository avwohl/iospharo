"Test if SDL2 access breaks NLR"!

Object subclass: #SDL2NLRTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!SDL2NLRTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/sdl2_nlr.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== SDL2 and NLR Test ==='; cr; cr.

    "Test 1: NLR without any SDL2 call"
    stream nextPutAll: '--- Test 1: NLR without SDL2 ---'; cr.
    [stream nextPutAll: 'test1 = '; print: self test1; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 2: NLR after SDL2 isAvailable"
    stream cr; nextPutAll: '--- Test 2: NLR after SDL2 isAvailable ---'; cr.
    [stream nextPutAll: 'test2 = '; print: self test2; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 3: NLR with SDL2 isAvailable in condition"
    stream cr; nextPutAll: '--- Test 3: NLR with SDL2 isAvailable in condition ---'; cr.
    [stream nextPutAll: 'test3 = '; print: self test3; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 4: NLR with SDL2 isAvailable not in condition"
    stream cr; nextPutAll: '--- Test 4: NLR with SDL2 isAvailable not in condition ---'; cr.
    [stream nextPutAll: 'test4 = '; print: self test4; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 5: Simple do: with SDL2 isAvailable not"
    stream cr; nextPutAll: '--- Test 5: do: with SDL2 isAvailable not ---'; cr.
    [stream nextPutAll: 'test5 = '; print: self test5; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 6: What is SDL2 isAvailable?"
    stream cr; nextPutAll: '--- Test 6: SDL2 isAvailable analysis ---'; cr.
    [stream nextPutAll: 'SDL2 = '; print: SDL2; cr.
     stream nextPutAll: 'SDL2 class = '; print: SDL2 class; cr.
     stream nextPutAll: 'SDL2 isAvailable = '; print: SDL2 isAvailable; cr.
     stream nextPutAll: '(SDL2 isAvailable) class = '; print: SDL2 isAvailable class; cr.
     stream nextPutAll: 'SDL2 isAvailable not = '; print: SDL2 isAvailable not; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 7: NLR with literal true instead of isSuitable"
    stream cr; nextPutAll: '--- Test 7: NLR with literal true ---'; cr.
    [stream nextPutAll: 'test7 = '; print: self test7; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 8: NLR after any send to aClass"
    stream cr; nextPutAll: '--- Test 8: NLR after name send ---'; cr.
    [stream nextPutAll: 'test8 = '; print: self test8; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 9: NLR after any send that returns boolean"
    stream cr; nextPutAll: '--- Test 9: NLR after isClass ---'; cr.
    [stream nextPutAll: 'test9 = '; print: self test9; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!SDL2NLRTest class methodsFor: 'tests'!
test1
  "NLR without any SDL2 call"
  #(1 2 3) do: [:x | x = 2 ifTrue: [^ #found]].
  ^ #notFound! !

!SDL2NLRTest class methodsFor: 'tests'!
test2
  "NLR after SDL2 isAvailable - not in condition"
  #(1 2 3) do: [:x |
    SDL2 isAvailable.
    x = 2 ifTrue: [^ #found]].
  ^ #notFound! !

!SDL2NLRTest class methodsFor: 'tests'!
test3
  "NLR with SDL2 isAvailable in condition"
  #(1 2 3) do: [:x |
    SDL2 isAvailable ifFalse: [^ #found]].
  ^ #notFound! !

!SDL2NLRTest class methodsFor: 'tests'!
test4
  "NLR with SDL2 isAvailable not in condition"
  #(1 2 3) do: [:x |
    SDL2 isAvailable not ifTrue: [^ #found]].
  ^ #notFound! !

!SDL2NLRTest class methodsFor: 'tests'!
test5
  "do: with SDL2 isAvailable not over drivers"
  {OSNullWindowDriver. OSSDL2Driver. OSiOSDriver} do: [:aClass |
    SDL2 isAvailable not ifTrue: [^ aClass]].
  ^ #notFound! !

!SDL2NLRTest class methodsFor: 'tests'!
test7
  "NLR with literal true for OSiOSDriver"
  {OSNullWindowDriver. OSSDL2Driver. OSiOSDriver} do: [:aClass |
    (aClass == OSiOSDriver) ifTrue: [^ aClass]].
  ^ #notFound! !

!SDL2NLRTest class methodsFor: 'tests'!
test8
  "NLR after name send that matches"
  {OSNullWindowDriver. OSSDL2Driver. OSiOSDriver} do: [:aClass |
    | nm |
    nm := aClass name.
    nm = #OSiOSDriver ifTrue: [^ aClass]].
  ^ #notFound! !

!SDL2NLRTest class methodsFor: 'tests'!
test9
  "NLR after isClass (returns true for all)"
  {OSNullWindowDriver. OSSDL2Driver. OSiOSDriver} do: [:aClass |
    aClass isClass ifTrue: [^ aClass]].
  ^ #notFound! !

!SDL2NLRTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!SDL2NLRTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #SDL2NLRTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
