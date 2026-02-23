"Callback test via qsort with multiple elements. Results: /tmp/callback_test_results.txt"!

Object subclass: #CallbackTestRunner
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-Runner'!

!CallbackTestRunner class methodsFor: 'ffi'!
qsort: base nmemb: nmemb size: size compar: compar
  ^ self ffiCall: #(void qsort(void* base, uint64 nmemb, uint64 size, void* compar)) module: 'libc.dylib'! !

!CallbackTestRunner class methodsFor: 'running'!
runAllTests
  | stream |

  stream := '/tmp/callback_test_results.txt' asFileReference writeStream.
  stream truncate.
  stream nextPutAll: 'Starting callback tests...'; cr; flush.

  "Test 1: qsort with 5 elements (multiple callback invocations)"
  [ | callback array cmpFn callCount |
    callCount := 0.
    stream nextPutAll: 'Test 1: qsort with 5 elements...'; cr; flush.

    callback := TFCallback
      forCallback: [ :a :b |
        | va vb |
        va := a signedLongAt: 1.
        vb := b signedLongAt: 1.
        callCount := callCount + 1.
        (va - vb) sign ]
      parameters: { TFBasicType pointer. TFBasicType pointer }
      returnType: TFBasicType sint32
      runner: TFRunner new.

    "Create array of 5 ints: {42, 7, 99, 1, 23}"
    array := ExternalAddress allocate: 20.  "5 * 4 bytes"
    array signedLongAt: 1 put: 42.
    array signedLongAt: 5 put: 7.
    array signedLongAt: 9 put: 99.
    array signedLongAt: 13 put: 1.
    array signedLongAt: 17 put: 23.

    stream nextPutAll: 'Before sort: '.
    1 to: 5 do: [ :i |
      stream nextPutAll: (array signedLongAt: (i - 1) * 4 + 1) printString; nextPut: Character space ].
    stream cr; flush.

    self qsort: array nmemb: 5 size: 4 compar: callback getHandle.

    stream nextPutAll: 'qsort returned after '; nextPutAll: callCount printString; nextPutAll: ' comparisons'; cr; flush.

    stream nextPutAll: 'After sort: '.
    1 to: 5 do: [ :i |
      stream nextPutAll: (array signedLongAt: (i - 1) * 4 + 1) printString; nextPut: Character space ].
    stream cr; flush.

    "Verify sorted order: 1 7 23 42 99"
    ((array signedLongAt: 1) = 1
      and: [ (array signedLongAt: 5) = 7
      and: [ (array signedLongAt: 9) = 23
      and: [ (array signedLongAt: 13) = 42
      and: [ (array signedLongAt: 17) = 99 ]]]])
      ifTrue: [ stream nextPutAll: 'PASS: 5-element qsort sorted correctly'; cr; flush ]
      ifFalse: [ stream nextPutAll: 'FAIL: 5-element sort incorrect'; cr; flush ].

    array free.
  ] on: Error do: [ :ex |
    stream nextPutAll: 'ERROR: '; nextPutAll: ex description; cr; flush.
    ex signalerContext ifNotNil: [ :ctx |
      stream nextPutAll: ctx printString; cr; flush ].
  ].

  "Test 2: qsort with 10 elements"
  [ | callback array callCount sorted |
    callCount := 0.
    stream cr; nextPutAll: 'Test 2: qsort with 10 elements...'; cr; flush.

    callback := TFCallback
      forCallback: [ :a :b |
        | va vb |
        va := a signedLongAt: 1.
        vb := b signedLongAt: 1.
        callCount := callCount + 1.
        (va - vb) sign ]
      parameters: { TFBasicType pointer. TFBasicType pointer }
      returnType: TFBasicType sint32
      runner: TFRunner new.

    "Create array of 10 ints"
    array := ExternalAddress allocate: 40.
    #(50 30 80 10 60 40 90 20 70 100) doWithIndex: [ :val :i |
      array signedLongAt: (i - 1) * 4 + 1 put: val ].

    stream nextPutAll: 'Before sort: '.
    1 to: 10 do: [ :i |
      stream nextPutAll: (array signedLongAt: (i - 1) * 4 + 1) printString; nextPut: Character space ].
    stream cr; flush.

    self qsort: array nmemb: 10 size: 4 compar: callback getHandle.

    stream nextPutAll: 'qsort returned after '; nextPutAll: callCount printString; nextPutAll: ' comparisons'; cr; flush.

    stream nextPutAll: 'After sort: '.
    1 to: 10 do: [ :i |
      stream nextPutAll: (array signedLongAt: (i - 1) * 4 + 1) printString; nextPut: Character space ].
    stream cr; flush.

    "Verify sorted"
    sorted := true.
    1 to: 9 do: [ :i |
      (array signedLongAt: (i - 1) * 4 + 1) > (array signedLongAt: i * 4 + 1)
        ifTrue: [ sorted := false ] ].
    sorted
      ifTrue: [ stream nextPutAll: 'PASS: 10-element qsort sorted correctly'; cr; flush ]
      ifFalse: [ stream nextPutAll: 'FAIL: 10-element sort incorrect'; cr; flush ].

    array free.
  ] on: Error do: [ :ex |
    stream nextPutAll: 'ERROR: '; nextPutAll: ex description; cr; flush.
    ex signalerContext ifNotNil: [ :ctx |
      stream nextPutAll: ctx printString; cr; flush ].
  ].

  stream cr.
  stream nextPutAll: '=== DONE ==='; cr.
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
