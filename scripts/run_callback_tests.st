"FFI callback test runner. Results: /tmp/callback_test_results.txt"!

Object subclass: #CallbackTestRunner
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-Runner'!

!CallbackTestRunner class methodsFor: 'running'!
runAllTests
  | stream classes testCount passCount failCount errorCount skipCount |

  stream := '/tmp/callback_test_results.txt' asFileReference writeStream.
  stream truncate.

  classes := #(
    FFICallbackTest
    FFICallbackParametersTest
    TFCallbacksTest
    TFBasicTypeMarshallingInCallbacksTest
    TFUFFICallbackTest
    TFUFFIBasicTypeMarshallingInCallbacksTest
    TFUFFIDerivedTypeMarshallingInCallbackTest
    TFUFFIMarshallingInCallbacksTest
  ).

  testCount := 0. passCount := 0. failCount := 0. errorCount := 0. skipCount := 0.

  classes do: [ :className |
    | cls suite |
    cls := Smalltalk globals at: className ifAbsent: [ nil ].
    cls ifNil: [
      skipCount := skipCount + 1.
      stream nextPutAll: 'SKIP: '; nextPutAll: className asString;
        nextPutAll: ' (class not found)'; cr.
    ] ifNotNil: [
      suite := cls buildSuite.
      suite tests do: [ :test |
        | testResult |
        testCount := testCount + 1.
        [ testResult := test run.
          testResult hasPassed
            ifTrue: [
              passCount := passCount + 1.
              stream nextPutAll: 'PASS: '; nextPutAll: test printString; cr.
            ] ifFalse: [
              testResult failures do: [ :f |
                failCount := failCount + 1.
                stream nextPutAll: 'FAIL: '; nextPutAll: test printString; cr.
              ].
              testResult errors do: [ :e |
                errorCount := errorCount + 1.
                stream nextPutAll: 'ERROR: '; nextPutAll: test printString;
                  nextPutAll: ' - '; nextPutAll: e description; cr.
              ].
            ].
        ] on: Error do: [ :ex |
          errorCount := errorCount + 1.
          stream nextPutAll: 'ERROR: '; nextPutAll: test printString;
            nextPutAll: ' - '; nextPutAll: ex description; cr.
        ].
        stream flush.
      ].
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

!CallbackTestRunner class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self runAllTests! !

!CallbackTestRunner class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup at priority 90"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #CallbackTestRunner)
  inCategory: SessionManager default userCategory
  atPriority: 90!
