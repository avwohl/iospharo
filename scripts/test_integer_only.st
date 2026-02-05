"Run only IntegerTest to verify testSlowFactorial"!

Object subclass: #IntegerOnlyTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!IntegerOnlyTest class methodsFor: 'running'!
run
  | stream testClass suite results |
  stream := '/tmp/integer_only.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== IntegerTest Only ==='; cr; cr.

    testClass := Smalltalk at: #IntegerTest ifAbsent: [nil].
    testClass ifNil: [
      stream nextPutAll: 'ERROR: IntegerTest class not found'; cr.
      ^ self].

    "Run testSlowFactorial specifically"
    stream nextPutAll: '--- Running testSlowFactorial ---'; cr.
    [| test |
     test := testClass new.
     test setUp.
     [test testSlowFactorial.
      stream nextPutAll: 'testSlowFactorial: PASS'; cr]
     on: TestFailure do: [:ex |
       stream nextPutAll: 'testSlowFactorial: FAIL - '; print: ex messageText; cr]
     on: Error do: [:ex |
       stream nextPutAll: 'testSlowFactorial: ERROR - '; print: ex class name; nextPutAll: ': '; print: ex messageText; cr].
     test tearDown]
    on: Error do: [:ex |
      stream nextPutAll: 'Setup/teardown error: '; print: ex class name; cr].
    stream flush.

    stream cr.
    stream nextPutAll: '=== DONE ==='; cr.
    stream flush]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!IntegerOnlyTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!IntegerOnlyTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #IntegerOnlyTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
