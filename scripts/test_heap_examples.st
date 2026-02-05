"Test HeapTest >> testExamples"!

Object subclass: #HeapExamplesTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!HeapExamplesTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/heap_examples_test.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== HeapTest >> testExamples Investigation ==='; cr; cr.

    "Test 1: Basic heap operations"
    stream nextPutAll: '--- Test 1: Basic Heap ---'; cr.
    [| heap |
     heap := Heap new.
     heap add: 5.
     heap add: 3.
     heap add: 7.
     stream nextPutAll: 'Created heap with 3 elements'; cr.
     stream nextPutAll: 'First element: '; print: heap first; cr.
     heap removeFirst.
     stream nextPutAll: 'After removeFirst, first: '; print: heap first; cr.
     stream nextPutAll: 'Test 1: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 1: ERROR - '; nextPutAll: ex class name;
        nextPutAll: ': '; nextPutAll: ex messageText; cr].

    stream cr.

    "Test 2: Random number generation"
    stream nextPutAll: '--- Test 2: Random ---'; cr.
    [| rnd values |
     rnd := Random new.
     values := (1 to: 10) collect: [:i | rnd next].
     stream nextPutAll: '10 random values: '.
     values do: [:v | stream print: (v * 100) truncated; nextPutAll: ' '].
     stream cr.
     stream nextPutAll: 'Test 2: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 2: ERROR - '; nextPutAll: ex class name;
        nextPutAll: ': '; nextPutAll: ex messageText; cr].

    stream cr.

    "Test 3: Heap withAll"
    stream nextPutAll: '--- Test 3: Heap withAll ---'; cr.
    [| rnd array heap |
     rnd := Random new.
     array := (1 to: 100) collect: [:i | rnd next].
     stream nextPutAll: 'Created array of 100 random floats'; cr.
     heap := Heap withAll: array.
     stream nextPutAll: 'Created Heap withAll: array'; cr.
     stream nextPutAll: 'Heap size: '; print: heap size; cr.
     stream nextPutAll: 'First: '; print: heap first; cr.
     stream nextPutAll: 'Test 3: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 3: ERROR - '; nextPutAll: ex class name;
        nextPutAll: ': '; nextPutAll: ex messageText; cr].

    stream cr.

    "Test 4: Heap removeFirst loop"
    stream nextPutAll: '--- Test 4: Heap removeFirst loop ---'; cr.
    [| rnd array heap count |
     rnd := Random new.
     array := (1 to: 100) collect: [:i | rnd next].
     heap := Heap withAll: array.
     count := 0.
     [heap isEmpty] whileFalse: [
       heap removeFirst.
       count := count + 1].
     stream nextPutAll: 'Removed all '; print: count; nextPutAll: ' elements'; cr.
     stream nextPutAll: 'Test 4: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 4: ERROR - '; nextPutAll: ex class name;
        nextPutAll: ': '; nextPutAll: ex messageText; cr].

    stream cr.

    "Test 5: heapExample (reduced size)"
    stream nextPutAll: '--- Test 5: heapExample (n=100) ---'; cr.
    [| n rnd array sorted |
     n := 100.
     rnd := Random new.
     array := (1 to: n) collect: [:i | rnd next].
     sorted := Heap withAll: array.
     1 to: n do: [:i |
       sorted removeFirst.
       sorted add: rnd next].
     stream nextPutAll: 'Completed '; print: n; nextPutAll: ' iterations'; cr.
     stream nextPutAll: 'Test 5: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 5: ERROR - '; nextPutAll: ex class name;
        nextPutAll: ': '; nextPutAll: ex messageText; cr].

    stream cr.

    "Test 6: heapExample full (n=5000)"
    stream nextPutAll: '--- Test 6: heapExample full (n=5000) ---'; cr.
    [| n rnd array sorted |
     n := 5000.
     rnd := Random new.
     array := (1 to: n) collect: [:i | rnd next].
     sorted := Heap withAll: array.
     1 to: n do: [:i |
       sorted removeFirst.
       sorted add: rnd next].
     stream nextPutAll: 'Completed '; print: n; nextPutAll: ' iterations'; cr.
     stream nextPutAll: 'Test 6: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 6: ERROR - '; nextPutAll: ex class name;
        nextPutAll: ': '; nextPutAll: ex messageText; cr].

    stream cr.
    stream nextPutAll: '=== DONE ==='; cr.
    stream flush]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!HeapExamplesTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!HeapExamplesTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #HeapExamplesTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
