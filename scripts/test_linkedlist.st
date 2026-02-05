"Test LinkedList remove behavior"!

Object subclass: #LinkedListDebug
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-Runner'!

!LinkedListDebug class methodsFor: 'running'!
run
  | list link1 s |
  s := '/tmp/linkedlist_debug.txt' asFileReference writeStream.
  [s nextPutAll: 'LinkedList Remove Debug'; lf.

  list := LinkedList new.
  link1 := ValueLink value: 1.

  s nextPutAll: 'Adding link1'; lf. s flush.
  list add: link1.
  s nextPutAll: 'list size = '; print: list size; lf. s flush.

  s nextPutAll: 'About to call list remove: link1'; lf. s flush.
  s nextPutAll: 'link1 nextLink = '; print: link1 nextLink; lf. s flush.

  "Call removeLink: directly to see where it hangs"
  s nextPutAll: 'Calling removeLink:ifAbsent:'; lf. s flush.
  list removeLink: link1 ifAbsent: [s nextPutAll: 'NOT FOUND'; lf].
  s nextPutAll: 'removeLink completed'; lf. s flush.
  s nextPutAll: 'list size after remove = '; print: list size; lf. s flush.

  s nextPutAll: 'DONE'; lf. s flush]
  ensure: [s close].

  Smalltalk exitSuccess! !

!LinkedListDebug class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!LinkedListDebug class methodsFor: 'startup'!
shutDown: quitting! !

SessionManager default
  register: (ClassSessionHandler forClassNamed: #LinkedListDebug)
  inCategory: SessionManager default userCategory
  atPriority: 90!

----End fileIn----!
