"Test single LinkedListTest method"!

Object subclass: #LLSingleTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-Runner'!

!LLSingleTest class methodsFor: 'running'!
run
  | s result |
  s := '/tmp/ll_single_result.txt' asFileReference writeStream.
  [s nextPutAll: 'Running test14removeIfAbsent'; lf. s flush.

  "Direct copy of the test method"
  [| list |
   list := LinkedList new.
   list add: (ValueLink value: 1).

   s nextPutAll: 'Created list with one element'; lf. s flush.
   s nextPutAll: 'list size = '; print: list size; lf. s flush.

   result := list remove: (list first value) ifAbsent: [#absent].
   s nextPutAll: 'remove returned: '; print: result; lf. s flush.
   s nextPutAll: 'list size after = '; print: list size; lf. s flush.

   (result = 1)
     ifTrue: [s nextPutAll: 'PASS: result is 1'; lf]
     ifFalse: [s nextPutAll: 'FAIL: result is '; print: result; lf].
   (list isEmpty)
     ifTrue: [s nextPutAll: 'PASS: list is empty'; lf]
     ifFalse: [s nextPutAll: 'FAIL: list is not empty'; lf].

   s nextPutAll: 'DONE'; lf]
   on: Error do: [:e |
     s nextPutAll: 'ERROR: '; print: e class; lf.
     s nextPutAll: 'Message: '; nextPutAll: e messageText; lf].
  s flush]
  ensure: [s close].

  Smalltalk exitSuccess! !

!LLSingleTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!LLSingleTest class methodsFor: 'startup'!
shutDown: quitting! !

SessionManager default
  register: (ClassSessionHandler forClassNamed: #LLSingleTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!

----End fileIn----!
