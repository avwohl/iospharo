"Debug testReciprocalModulo"!

Object subclass: #ReciprocalTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-Runner'!

!ReciprocalTest class methodsFor: 'running'!
runTest
  | stream count a b c res gcdVal |
  stream := '/tmp/sunit_test_results.txt' asFileReference writeStream.
  count := 0.
  [
    stream nextPutAll: '=== testReciprocalModulo Debug ==='; cr; flush.

    "Test a=2 b=20 (like iter 529)"
    a := 2. b := 20.
    stream nextPutAll: 'Testing a=2 b=20 (like iter 529)'; cr; flush.
    gcdVal := a gcd: b.
    stream nextPutAll: '  gcd='; print: gcdVal; cr; flush.
    gcdVal = 1 ifFalse: [
      stream nextPutAll: '  Calling reciprocalModulo (expect error)'; cr; flush.
      [a reciprocalModulo: b] on: Error do: [:ex | stream nextPutAll: '  Caught error: '; nextPutAll: ex messageText; cr; flush]].
    stream nextPutAll: 'Done with a=2 b=20'; cr; flush.

    "Test a=2 b=21 (like iter 530)"
    a := 2. b := 21.
    stream nextPutAll: 'Testing a=2 b=21 (like iter 530)'; cr; flush.
    gcdVal := a gcd: b.
    stream nextPutAll: '  gcd='; print: gcdVal; cr; flush.
    gcdVal = 1 ifTrue: [
      stream nextPutAll: '  Calling reciprocalModulo'; cr; flush.
      c := a reciprocalModulo: b.
      stream nextPutAll: '  c='; print: c; cr; flush.
      res := a * c \\ b.
      stream nextPutAll: '  res='; print: res; cr; flush].
    stream nextPutAll: 'Done with a=2 b=21'; cr; flush.

    "Short loop a=2 b=19 to 25"
    stream nextPutAll: 'Now running short loop a=2 b=19 to 25'; cr; flush.
    a := 2.
    19 to: 25 do: [:bb |
      stream nextPutAll: 'Loop b='; print: bb; cr; flush.
      gcdVal := a gcd: bb.
      gcdVal = 1 ifTrue: [
        c := a reciprocalModulo: bb.
        res := a * c \\ bb.
        stream nextPutAll: '  c='; print: c; nextPutAll: ' res='; print: res; cr; flush]
      ifFalse: [
        [a reciprocalModulo: bb] on: Error do: [:ex | stream nextPutAll: '  expected error'; cr; flush]]].
    stream nextPutAll: 'Short loop done'; cr; flush.

    stream nextPutAll: 'TEST PASSED'; cr.
  ] ensure: [stream close].

  Smalltalk exitSuccess! !

!ReciprocalTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifTrue: [self runTest]! !

!ReciprocalTest class methodsFor: 'startup'!
shutDown: quitting! !

SessionManager default registerUserClassNamed: #ReciprocalTest!
