"Exact test14removeIfAbsent reproduction"!

Object subclass: #LLExactTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-Runner'!

!LLExactTest class methodsFor: 'running'!
run
  | s list link1 |
  s := '/tmp/ll_exact_result.txt' asFileReference writeStream.
  [s nextPutAll: 'Exact test14removeIfAbsent reproduction'; lf. s flush.

  list := LinkedList new.
  link1 := 133.

  s nextPutAll: 'Adding link1 (133)'; lf. s flush.
  list add: link1.
  s nextPutAll: 'list size = '; print: list size; lf. s flush.

  s nextPutAll: 'Checking list first...'; lf. s flush.
  s nextPutAll: 'list first = '; print: list first; lf. s flush.

  s nextPutAll: 'First remove: link1'; lf. s flush.
  list remove: link1.
  s nextPutAll: 'After first remove, size = '; print: list size; lf. s flush.

  s nextPutAll: 'Second remove: link1 (should error)'; lf. s flush.
  [list remove: link1]
    on: Error
    do: [:e |
      s nextPutAll: 'Got expected error: '; print: e class; lf.
      s nextPutAll: 'Message: '; nextPutAll: e messageText; lf.
      s nextPutAll: 'PASS'; lf. s flush.
      s close.
      Smalltalk exitSuccess].

  s nextPutAll: 'FAIL - no error raised on second remove'; lf. s flush]
  ensure: [s close].

  Smalltalk exitSuccess! !

!LLExactTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!LLExactTest class methodsFor: 'startup'!
shutDown: quitting! !

SessionManager default
  register: (ClassSessionHandler forClassNamed: #LLExactTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!

----End fileIn----!
