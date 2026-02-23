"Find and run callback test classes. Results: /tmp/callback_suite_results.txt"!

Object subclass: #CallbackSuiteRunner
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-Runner'!

!CallbackSuiteRunner class methodsFor: 'running'!
runAllTests
  | stream totalPass totalFail totalError callbackClasses |

  stream := '/tmp/callback_suite_results.txt' asFileReference writeStream.
  stream truncate.
  stream nextPutAll: 'Starting callback test suite...'; cr; flush.

  totalPass := 0.
  totalFail := 0.
  totalError := 0.

  "Find all test classes with 'callback' in name (case insensitive)"
  callbackClasses := TestCase allSubclasses select: [ :c |
    c name asLowercase includesSubstring: 'callback' ].

  stream nextPutAll: 'Found '; nextPutAll: callbackClasses size printString.
  stream nextPutAll: ' callback test classes:'; cr.
  callbackClasses do: [ :c | stream nextPutAll: '  '; nextPutAll: c name; cr ].
  stream flush.

  callbackClasses do: [ :testClass |
    | suite result |
    stream cr; nextPutAll: '=== '; nextPutAll: testClass name; nextPutAll: ' ==='; cr; flush.

    [ suite := testClass buildSuite.
      result := suite run.
      stream nextPutAll: 'Passed: '; nextPutAll: result passedCount printString; cr.
      stream nextPutAll: 'Failed: '; nextPutAll: result failureCount printString; cr.
      stream nextPutAll: 'Errors: '; nextPutAll: result errorCount printString; cr.
      totalPass := totalPass + result passedCount.
      totalFail := totalFail + result failureCount.
      totalError := totalError + result errorCount.

      "Log failures"
      result failures do: [ :f |
        stream nextPutAll: '  FAIL: '; nextPutAll: f printString; cr ].
      result errors do: [ :e |
        stream nextPutAll: '  ERROR: '; nextPutAll: e printString; cr ].
      stream flush.
    ] on: Error do: [ :ex |
      stream nextPutAll: 'SUITE ERROR: '; nextPutAll: ex description; cr; flush.
      totalError := totalError + 1.
    ].
  ].

  stream cr; nextPutAll: '=== SUMMARY ==='; cr.
  stream nextPutAll: 'Total passed: '; nextPutAll: totalPass printString; cr.
  stream nextPutAll: 'Total failed: '; nextPutAll: totalFail printString; cr.
  stream nextPutAll: 'Total errors: '; nextPutAll: totalError printString; cr.
  stream nextPutAll: '=== DONE ==='; cr.
  stream close.

  Smalltalk exit: 0.! !

!CallbackSuiteRunner class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self runAllTests! !

!CallbackSuiteRunner class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup at priority 90"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #CallbackSuiteRunner)
  inCategory: SessionManager default userCategory
  atPriority: 90!
