"Narrow down HeapTest SubscriptOutOfBounds"!

Object subclass: #HeapNarrowTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!HeapNarrowTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/heap_narrow.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== Heap Narrow Investigation ==='; cr; cr.

    "Test with increasing sizes to find threshold"
    #(100 200 300 400 500 600 700 800 900 1000) do: [:n |
      stream nextPutAll: '--- Testing n='; print: n; nextPutAll: ' ---'; cr.
      [| rnd array sorted |
       rnd := Random new.
       array := (1 to: n) collect: [:i | rnd next].
       sorted := Heap withAll: array.
       stream nextPutAll: 'Created Heap size='; print: sorted size; cr.
       1 to: n do: [:i |
         [sorted removeFirst.
          sorted add: rnd next]
           on: SubscriptOutOfBounds do: [:ex |
             stream nextPutAll: 'SUBSCRIPT ERROR at iteration '; print: i; cr.
             stream nextPutAll: 'Heap size='; print: sorted size; cr.
             stream nextPutAll: 'Array size='; print: sorted array size; cr.
             stream nextPutAll: 'Error: '; nextPutAll: ex messageText; cr.
             ex pass]].
       stream nextPutAll: 'Completed '; print: n; nextPutAll: ' iterations PASS'; cr]
      on: SubscriptOutOfBounds do: [:ex |
        stream nextPutAll: 'FAILED at n='; print: n; cr].
      stream flush].

    stream cr.
    stream nextPutAll: '=== DONE ==='; cr.
    stream flush]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!HeapNarrowTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!HeapNarrowTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #HeapNarrowTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
