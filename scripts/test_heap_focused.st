"Focused HeapTest investigation - find SubscriptOutOfBounds cause"!

Object subclass: #HeapFocusedTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!HeapFocusedTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/heap_focused.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== Heap Focused Investigation ==='; cr; cr.

    "Test 1: Basic heap add/remove"
    stream nextPutAll: '--- Test 1: Basic add/removeFirst ---'; cr.
    [| heap |
     heap := Heap new.
     1 to: 10 do: [:i | heap add: i].
     stream nextPutAll: 'Added 10 items, size='; print: heap size; cr.
     1 to: 10 do: [:i |
       | first |
       first := heap removeFirst.
       stream nextPutAll: 'Removed: '; print: first; nextPutAll: ' size='; print: heap size; cr].
     stream nextPutAll: 'Test 1: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 1: ERROR - '; nextPutAll: ex class name;
        nextPutAll: ': '; nextPutAll: ex messageText; cr].

    stream cr.

    "Test 2: Heap withAll + removeFirst"
    stream nextPutAll: '--- Test 2: Heap withAll + removeFirst ---'; cr.
    [| array heap |
     array := #(0.5 0.2 0.8 0.1 0.9).
     heap := Heap withAll: array.
     stream nextPutAll: 'Heap withAll: '; print: array; cr.
     stream nextPutAll: 'Heap array: '; print: heap array; cr.
     stream nextPutAll: 'Size: '; print: heap size; cr.
     1 to: 5 do: [:i |
       | first |
       first := heap removeFirst.
       stream nextPutAll: 'Removed '; print: first; nextPutAll: ' size='; print: heap size; cr].
     stream nextPutAll: 'Test 2: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 2: ERROR - '; nextPutAll: ex class name;
        nextPutAll: ': '; nextPutAll: ex messageText; cr].

    stream cr.

    "Test 3: Random + Heap withAll"
    stream nextPutAll: '--- Test 3: Random + Heap withAll ---'; cr.
    [| rnd array heap |
     rnd := Random new.
     array := (1 to: 20) collect: [:i | rnd next].
     stream nextPutAll: 'Generated 20 random floats'; cr.
     stream nextPutAll: 'First 5: '.
     1 to: 5 do: [:i | stream print: (array at: i) truncated; space].
     stream cr.
     heap := Heap withAll: array.
     stream nextPutAll: 'Created Heap, size='; print: heap size; cr.
     1 to: 10 do: [:i |
       heap removeFirst.
       heap add: rnd next].
     stream nextPutAll: 'Did 10 remove/add cycles'; cr.
     stream nextPutAll: 'Final size: '; print: heap size; cr.
     stream nextPutAll: 'Test 3: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 3: ERROR - '; nextPutAll: ex class name;
        nextPutAll: ': '; nextPutAll: ex messageText; cr].

    stream cr.

    "Test 4: heapExample with small n"
    stream nextPutAll: '--- Test 4: heapExample n=100 ---'; cr.
    [| n rnd array sorted |
     n := 100.
     rnd := Random new.
     array := (1 to: n) collect: [:i | rnd next].
     sorted := Heap withAll: array.
     stream nextPutAll: 'Created Heap with '; print: n; nextPutAll: ' elements'; cr.
     1 to: n do: [:i |
       sorted removeFirst.
       sorted add: rnd next.
       i \\ 20 = 0 ifTrue: [
         stream nextPutAll: 'Iteration '; print: i; nextPutAll: ' size='; print: sorted size; cr]].
     stream nextPutAll: 'Completed '; print: n; nextPutAll: ' iterations'; cr.
     stream nextPutAll: 'Test 4: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 4: ERROR at some iteration - '; nextPutAll: ex class name;
        nextPutAll: ': '; nextPutAll: ex messageText; cr.
      stream nextPutAll: 'Stack: '; cr.
      (ex signalerContext stackOfSize: 10) do: [:ctx |
        stream nextPutAll: '  '; nextPutAll: ctx printString; cr]].

    stream cr.

    "Test 5: heapExample with n=1000"
    stream nextPutAll: '--- Test 5: heapExample n=1000 ---'; cr.
    [| n rnd array sorted |
     n := 1000.
     rnd := Random new.
     array := (1 to: n) collect: [:i | rnd next].
     sorted := Heap withAll: array.
     stream nextPutAll: 'Created Heap with '; print: n; nextPutAll: ' elements'; cr.
     1 to: n do: [:i |
       sorted removeFirst.
       sorted add: rnd next.
       i \\ 200 = 0 ifTrue: [
         stream nextPutAll: 'Iteration '; print: i; nextPutAll: ' size='; print: sorted size; cr]].
     stream nextPutAll: 'Completed '; print: n; nextPutAll: ' iterations'; cr.
     stream nextPutAll: 'Test 5: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 5: ERROR - '; nextPutAll: ex class name;
        nextPutAll: ': '; nextPutAll: ex messageText; cr].

    stream cr.
    stream nextPutAll: '=== DONE ==='; cr.
    stream flush]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!HeapFocusedTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!HeapFocusedTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #HeapFocusedTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
