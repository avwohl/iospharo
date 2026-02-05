"Run only LinkedListTest"!

Object subclass: #LLTestRunner
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-Runner'!

!LLTestRunner class methodsFor: 'running'!
run
  | s results testClasses |
  s := '/tmp/ll_test_results.txt' asFileReference writeStream.
  [s nextPutAll: '=== LinkedListTest Run ==='; lf. s flush.

  testClasses := { LinkedListTest }.

  testClasses do: [:testClass |
    s nextPutAll: 'Running: '; print: testClass; lf. s flush.
    [results := testClass buildSuite run.
     s nextPutAll: 'Passed: '; print: results passedCount; lf.
     s nextPutAll: 'Failed: '; print: results failureCount; lf.
     s nextPutAll: 'Errors: '; print: results errorCount; lf.
     results failures do: [:f | s nextPutAll: '  FAIL: '; nextPutAll: f asString; lf].
     results errors do: [:e | s nextPutAll: '  ERROR: '; nextPutAll: e asString; lf].
     s flush]
    on: Error do: [:e |
      s nextPutAll: 'RUNNER ERROR: '; print: e class; lf.
      s nextPutAll: 'Message: '; nextPutAll: e messageText; lf. s flush]].

  s nextPutAll: '=== DONE ==='; lf. s flush]
  ensure: [s close].

  Smalltalk exitSuccess! !

!LLTestRunner class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!LLTestRunner class methodsFor: 'startup'!
shutDown: quitting! !

SessionManager default
  register: (ClassSessionHandler forClassNamed: #LLTestRunner)
  inCategory: SessionManager default userCategory
  atPriority: 90!

----End fileIn----!
