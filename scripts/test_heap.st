"Test Heap examples"!

Object subclass: #HeapDebug
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-Runner'!

!HeapDebug class methodsFor: 'running'!
run
  '/tmp/heap_debug.txt' asFileReference writeStreamDo: [:log |
    log nextPutAll: '=== Heap Debug ==='; lf.

    log nextPutAll: '1. Running Heap heapExample...'; lf. log flush.
    [Heap heapExample.
     log nextPutAll: '   heapExample completed OK'; lf]
    on: Error do: [:e |
      log nextPutAll: '   ERROR in heapExample: '; print: e class; lf.
      log nextPutAll: '   Message: '; nextPutAll: e messageText; lf].
    log flush.

    log nextPutAll: '2. Running Heap heapSortExample...'; lf. log flush.
    [Heap heapSortExample.
     log nextPutAll: '   heapSortExample completed OK'; lf]
    on: Error do: [:e |
      log nextPutAll: '   ERROR in heapSortExample: '; print: e class; lf.
      log nextPutAll: '   Message: '; nextPutAll: e messageText; lf].
    log flush.

    log nextPutAll: '=== DONE ==='; lf. log flush].

  Smalltalk exitSuccess! !

!HeapDebug class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!HeapDebug class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #HeapDebug)
  inCategory: SessionManager default userCategory
  atPriority: 90!

----End fileIn----!
