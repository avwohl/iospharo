"Run HeapTest only"!

Object subclass: #HeapTestRunner
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-Runner'!

!HeapTestRunner class methodsFor: 'running'!
runAllTests
  | stream |
  stream := '/tmp/heap_test_results.txt' asFileReference writeStream.
  [stream nextPutAll: '=== HeapTest Run ==='; cr.
   stream nextPutAll: 'Running: HeapTest'; cr.
   self runTestClass: HeapTest on: stream.
   stream nextPutAll: '=== DONE ==='; cr.
   stream flush]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!HeapTestRunner class methodsFor: 'running'!
runTestClass: testClass on: s
  | sels p f e |
  p := 0. f := 0. e := 0.
  sels := testClass allTestSelectors asArray sort.
  sels do: [:sel |
    s nextPutAll: '  RUN: '; nextPutAll: sel; cr. s flush.
    [[[testClass new setUp; perform: sel; tearDown.
       s nextPutAll: '  PASS: '; nextPutAll: sel; cr.
       p := p + 1]
      on: TestFailure do: [:ex |
        f := f + 1.
        s nextPutAll: '  FAIL: '; nextPutAll: sel;
          nextPutAll: ' ['; nextPutAll: (ex messageText copyFrom: 1 to: (ex messageText size min: 200)); nextPutAll: ']'; cr]]
      on: Deprecation do: [:ex | ex resume]]
      on: Exception do: [:ex |
        e := e + 1.
        s nextPutAll: '  ERROR: '; nextPutAll: sel;
          nextPutAll: ' ('; nextPutAll: ex class name;
          nextPutAll: ': '; nextPutAll: (ex messageText copyFrom: 1 to: (ex messageText size min: 200)); nextPutAll: ')'; cr].
    s flush].
  s nextPutAll: 'Passed: '; print: p; cr.
  s nextPutAll: 'Failed: '; print: f; cr.
  s nextPutAll: 'Errors: '; print: e; cr! !

!HeapTestRunner class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self runAllTests! !

!HeapTestRunner class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #HeapTestRunner)
  inCategory: SessionManager default userCategory
  atPriority: 90!
