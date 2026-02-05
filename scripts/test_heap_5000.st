"Test heapExample with n=5000 - the actual size used"!

Object subclass: #Heap5000Test
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!Heap5000Test class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/heap_5000.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== Heap 5000 Test ==='; cr.

    "Test just the Heap part of heapExample with n=5000"
    stream nextPutAll: '--- Heap with n=5000 ---'; cr.
    [| n rnd array sorted |
     n := 5000.
     rnd := Random new.
     stream nextPutAll: 'Creating array...'; cr. stream flush.
     array := (1 to: n) collect: [:i | rnd next].
     stream nextPutAll: 'Array created, size='; print: array size; cr. stream flush.
     sorted := Heap withAll: array.
     stream nextPutAll: 'Heap created, size='; print: sorted size; cr. stream flush.
     1 to: n do: [:i |
       [sorted removeFirst] on: Error do: [:ex |
         stream nextPutAll: 'removeFirst ERROR at i='; print: i;
           nextPutAll: ' size='; print: sorted size;
           nextPutAll: ' array='; print: sorted array size;
           nextPutAll: ' err='; nextPutAll: ex class name; cr. stream flush.
         ex pass].
       [sorted add: rnd next] on: Error do: [:ex |
         stream nextPutAll: 'add ERROR at i='; print: i;
           nextPutAll: ' size='; print: sorted size;
           nextPutAll: ' array='; print: sorted array size;
           nextPutAll: ' err='; nextPutAll: ex class name; cr. stream flush.
         ex pass].
       i \\ 500 = 0 ifTrue: [
         stream nextPutAll: 'Iter '; print: i;
           nextPutAll: ' size='; print: sorted size; cr. stream flush]].
     stream nextPutAll: 'HEAP PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'HEAP FAIL: '; nextPutAll: ex class name;
        nextPutAll: ': '; nextPutAll: ex messageText; cr].

    stream cr.
    stream nextPutAll: '=== DONE ==='; cr.
    stream flush]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!Heap5000Test class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!Heap5000Test class methodsFor: 'startup'!
shutDown: quitting! !

"Register"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #Heap5000Test)
  inCategory: SessionManager default userCategory
  atPriority: 90!
