"Package test runner for higher-level test suites.
 Discovers TestCase subclasses matching a pattern and runs them.
 Pattern is read from /tmp/pkg_test_pattern.txt.
 Results: <imageDir>/pkg_test_results.txt
          <imageDir>/pkg_test_detail.txt"!

Object subclass: #PkgTestRunner
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-PkgRunner'!

!PkgTestRunner class methodsFor: 'running'!
run
  "Entry point — read pattern from file and run matching tests."
  | pattern |
  pattern := [('/tmp/pkg_test_pattern.txt' asFileReference) contents trimBoth]
    on: Error do: [:e | ''].
  pattern ifEmpty: [
    Stdio stderr nextPutAll: 'PkgTestRunner: no pattern in /tmp/pkg_test_pattern.txt'; cr.
    Smalltalk quitPrimitive.
    ^ self].
  Stdio stderr nextPutAll: 'PkgTestRunner: pattern = '; nextPutAll: pattern; cr.
  self runForPattern: pattern! !

!PkgTestRunner class methodsFor: 'running'!
runForPattern: patternString
  "Find and run all TestCase subclasses whose package or class name matches."
  | testClasses stream detail totalP totalF totalE |
  stream := (Smalltalk imageDirectory / 'pkg_test_results.txt') writeStream.
  detail := (Smalltalk imageDirectory / 'pkg_test_detail.txt') writeStream.
  totalP := 0. totalF := 0. totalE := 0.

  testClasses := TestCase allSubclasses select: [:cls |
    ([cls isAbstract] on: Error do: [:e | true]) not and: [
      (cls package name includesSubstring: patternString caseSensitive: false)
        or: [cls name asString includesSubstring: patternString caseSensitive: false]]].

  stream nextPutAll: '=== Package Tests: '; nextPutAll: patternString; nextPutAll: ' ==='; cr.
  stream nextPutAll: 'Found '; print: testClasses size; nextPutAll: ' test classes'; cr; cr.
  stream flush.
  Stdio stderr nextPutAll: 'PkgTestRunner: found '; print: testClasses size;
    nextPutAll: ' test classes'; cr.

  "Patch test timeouts for interpreter VM (10x)"
  [TestCase compile: 'defaultTimeLimit ^ 100 seconds' classified: 'running'.
   TestCase allSubclasses do: [:cls |
     (cls includesSelector: #defaultTimeLimit) ifTrue: [
       cls removeSelector: #defaultTimeLimit]]]
  on: Error do: [:e | ].

  testClasses asSortedCollection: [:a :b | a name < b name].
  testClasses := testClasses asSortedCollection: [:a :b | a name < b name].

  testClasses do: [:tc | | suite result startMs elapsedMs |
    stream nextPutAll: '=== '; nextPutAll: tc name; nextPutAll: ' ==='; cr. stream flush.
    Stdio stderr nextPutAll: '  Running: '; nextPutAll: tc name; cr.
    startMs := Time millisecondClockValue.
    [suite := tc buildSuite.
     result := [suite run] on: Error do: [:e |
       stream nextPutAll: '  RUNNER-ERROR: '; nextPutAll: e class name;
         nextPutAll: ' - '; nextPutAll: (e messageText copyFrom: 1 to: (e messageText size min: 200)); cr.
       nil].
     elapsedMs := Time millisecondClockValue - startMs.
     result ifNotNil: [
       stream nextPutAll: '  Pass: '; print: result passedCount;
         nextPutAll: '  Fail: '; print: result failureCount;
         nextPutAll: '  Error: '; print: result errorCount;
         nextPutAll: '  ('; print: elapsedMs; nextPutAll: 'ms)'; cr.
       totalP := totalP + result passedCount.
       totalF := totalF + result failureCount.
       totalE := totalE + result errorCount.
       result failures do: [:f |
         detail nextPutAll: 'FAIL '; nextPutAll: f class name;
           nextPutAll: ' >> '; nextPutAll: f selector; cr.
         stream nextPutAll: '  FAIL: '; nextPutAll: f selector; cr].
       result errors do: [:f |
         detail nextPutAll: 'ERROR '; nextPutAll: f class name;
           nextPutAll: ' >> '; nextPutAll: f selector; cr.
         stream nextPutAll: '  ERROR: '; nextPutAll: f selector; cr]].
     stream flush. detail flush]
    on: Error do: [:e |
      stream nextPutAll: '  CLASS-ERROR: '; nextPutAll: e class name;
        nextPutAll: ' - '; nextPutAll: (e messageText copyFrom: 1 to: (e messageText size min: 200)); cr.
      stream flush]].

  stream cr.
  stream nextPutAll: '=== TOTAL ==='; cr.
  stream nextPutAll: 'Pass: '; print: totalP; cr.
  stream nextPutAll: 'Fail: '; print: totalF; cr.
  stream nextPutAll: 'Error: '; print: totalE; cr.
  stream nextPutAll: 'Total: '; print: totalP + totalF + totalE; cr.
  stream nextPutAll: '=== COMPLETE ==='; cr.
  stream close. detail close.
  Stdio stderr nextPutAll: 'PkgTestRunner: DONE Pass='; print: totalP;
    nextPutAll: ' Fail='; print: totalF; nextPutAll: ' Error='; print: totalE; cr.
  [Smalltalk exitSuccess] on: Error do: [:e |].
  Smalltalk quitPrimitive! !

"Register for startup"
!PkgTestRunner class methodsFor: 'startup'!
startUp: isResumption
  isResumption ifTrue: [self run]! !

SessionManager default registerUserClassNamed: #PkgTestRunner!
