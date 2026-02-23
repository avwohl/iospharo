"Regression test runner for callback changes.
 Results: /tmp/regression_test_results.txt"!

Object subclass: #RegressionRunner
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-Runner'!

!RegressionRunner class methodsFor: 'running'!
runAllTests
  | stream classes testCount passCount failCount errorCount skipCount |
  stream := '/tmp/regression_test_results.txt' asFileReference writeStream.
  stream truncate.

  classes := #(
    SmallIntegerTest
    IntegerTest
    FloatTest
    FractionTest
    CharacterTest
    PointTest
    SetTest
    DictionaryTest
    SortedCollectionTest
    FFICalloutAPITest
    FFICompilerPluginTest
    FFICallbackTest
    FFICallbackParametersTest
    TFCallbacksTest
    TFBasicTypeMarshallingInCallbacksTest
    TFUFFICallbackTest
    ProcessTest
    SemaphoreTest
    MutexTest
    DelayTest
  ).

  testCount := 0.
  passCount := 0.
  failCount := 0.
  errorCount := 0.
  skipCount := 0.

  classes do: [ :className |
    | cls suite |
    cls := Smalltalk globals at: className ifAbsent: [ nil ].
    cls ifNil: [
      skipCount := skipCount + 1.
      stream nextPutAll: 'SKIP: '; nextPutAll: className asString;
        nextPutAll: ' (class not found)'; cr.
    ] ifNotNil: [
      stream nextPutAll: '=== '; nextPutAll: className asString;
        nextPutAll: ' ==='; cr.
      suite := cls buildSuite.
      suite tests do: [ :test |
        | testResult |
        testCount := testCount + 1.
        [ testResult := test run.
          testResult hasPassed
            ifTrue: [ passCount := passCount + 1 ]
            ifFalse: [
              testResult failures do: [ :f |
                failCount := failCount + 1.
                stream nextPutAll: '  FAIL: '; nextPutAll: test printString; cr.
              ].
              testResult errors do: [ :e |
                errorCount := errorCount + 1.
                stream nextPutAll: '  ERROR: '; nextPutAll: test printString;
                  nextPutAll: ' - '; nextPutAll: e description; cr.
              ].
            ].
        ] on: Error do: [ :ex |
          errorCount := errorCount + 1.
          stream nextPutAll: '  ERROR: '; nextPutAll: test printString;
            nextPutAll: ' - '; nextPutAll: ex description; cr.
        ].
      ].
      stream nextPutAll: '  Tests: '; nextPutAll: suite tests size printString; cr.
    ].
  ].

  stream cr.
  stream nextPutAll: '=== SUMMARY ==='; cr.
  stream nextPutAll: 'Total: '; nextPutAll: testCount printString; cr.
  stream nextPutAll: 'Pass: '; nextPutAll: passCount printString; cr.
  stream nextPutAll: 'Fail: '; nextPutAll: failCount printString; cr.
  stream nextPutAll: 'Error: '; nextPutAll: errorCount printString; cr.
  stream nextPutAll: 'Skip classes: '; nextPutAll: skipCount printString; cr.
  stream close.

  Smalltalk exit: 0.! !

!RegressionRunner class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self runAllTests! !

!RegressionRunner class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #RegressionRunner)
  inCategory: SessionManager default userCategory
  atPriority: 90!
