"Test just testPrintingRecursive"!

Object subclass: #PrintTestRunner
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-Runner'!

!PrintTestRunner class methodsFor: 'running'!
runTest
  | stream array result |
  stream := '/tmp/sunit_test_results.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== testPrintingRecursive ==='; cr.
    stream nextPutAll: 'Creating recursive array...'; cr; flush.

    array := Array new: 1.
    array at: 1 put: array.

    stream nextPutAll: 'Calling printStringLimitedTo: 50000...'; cr; flush.

    [
      result := array printStringLimitedTo: 50000.
      stream nextPutAll: 'Result size: '; print: result size; cr.

      result size >= 50000
        ifTrue: [stream nextPutAll: 'TEST PASSED'; cr]
        ifFalse: [stream nextPutAll: 'TEST FAILED: Got '; print: result size; nextPutAll: ' chars'; cr].
    ] on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; nextPutAll: ex class name; nextPutAll: ' - '; nextPutAll: ex messageText; cr].

    stream nextPutAll: '=== Done ==='; cr.
  ] ensure: [stream close].

  Smalltalk exitSuccess! !

!PrintTestRunner class methodsFor: 'startup'!
startUp: resuming
  resuming ifTrue: [self runTest]! !

!PrintTestRunner class methodsFor: 'startup'!
shutDown: quitting! !

SessionManager default registerUserClassNamed: #PrintTestRunner!
