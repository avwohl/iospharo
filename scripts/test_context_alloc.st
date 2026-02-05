"Test Context allocation"!

Object subclass: #ContextAllocTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-Runner'!

!ContextAllocTest class methodsFor: 'running'!
run
  | s method ctx |
  s := '/tmp/context_alloc.txt' asFileReference writeStream.
  [s nextPutAll: 'Context allocation test'; lf. s flush.

  "Get a simple method to test with"
  method := Object >> #yourself.
  s nextPutAll: 'method = '; print: method; lf.
  s nextPutAll: 'method frameSize = '; print: method frameSize; lf. s flush.

  "Try to allocate a Context"
  s nextPutAll: 'About to call Context newForMethod: method'; lf. s flush.

  [ctx := Context newForMethod: method.
   s nextPutAll: 'Context created: '; print: ctx; lf.
   s nextPutAll: 'ctx class = '; print: ctx class; lf]
  on: Error do: [:e |
    s nextPutAll: 'ERROR: '; print: e class; lf.
    s nextPutAll: 'Message: '; nextPutAll: e messageText; lf].
  s flush.

  s nextPutAll: 'DONE'; lf. s flush]
  ensure: [s close].

  Smalltalk exitSuccess! !

!ContextAllocTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!ContextAllocTest class methodsFor: 'startup'!
shutDown: quitting! !

SessionManager default
  register: (ClassSessionHandler forClassNamed: #ContextAllocTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!

----End fileIn----!
