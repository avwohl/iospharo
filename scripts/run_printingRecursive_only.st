"Single test runner for testPrintingRecursive with detailed debugging"!

Object subclass: #PrintTestRunner
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-Runner'!

!PrintTestRunner class methodsFor: 'running'!
runTest
  "Run testPrintingRecursive with detailed debugging"
  | stream array result |
  stream := '/tmp/sunit_test_results.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== testPrintingRecursive Debug ==='; cr.
    stream nextPutAll: 'Creating recursive array...'; cr; flush.

    array := Array new: 1.
    array at: 1 put: array.

    stream nextPutAll: 'Array created. Calling printStringLimitedTo: 50000...'; cr; flush.

    [
      result := array printStringLimitedTo: 50000.
      stream nextPutAll: 'Result size: '; print: result size; cr.

      result size >= 50000
        ifTrue: [stream nextPutAll: 'TEST PASSED'; cr]
        ifFalse: [
          stream nextPutAll: 'TEST FAILED: Got '; print: result size; nextPutAll: ' chars'; cr.
          stream nextPutAll: 'First 200 chars: '; nextPutAll: (result first: (200 min: result size)); cr.
          stream nextPutAll: 'Last 200 chars: '; nextPutAll: (result last: (200 min: result size)); cr].
    ] on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; nextPutAll: ex class name; nextPutAll: ' - '; nextPutAll: ex messageText; cr.
      stream nextPutAll: 'Stack trace:'; cr.
      ex signalerContext ifNotNil: [:ctx | | c |
        c := ctx.
        1 to: 30 do: [:i |
          c ifNotNil: [
            stream nextPutAll: '  '; print: i; nextPutAll: ': '; nextPutAll: c method methodClass name; nextPutAll: '>>'; nextPutAll: c method selector; cr.
            c := c sender]]]].

    stream nextPutAll: '=== Done ==='; cr; flush.
  ] ensure: [stream close].

  Smalltalk exitSuccess! !

!PrintTestRunner class methodsFor: 'startup'!
startUp: resuming
  resuming ifTrue: [self runTest]! !

!PrintTestRunner class methodsFor: 'startup'!
shutDown: quitting! !

SessionManager default registerUserClassNamed: #PrintTestRunner!
