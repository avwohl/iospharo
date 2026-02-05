"Simple startup test"!

Object subclass: #SimpleStartup
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!SimpleStartup class methodsFor: 'running'!
run
  '/tmp/simple_startup.txt' asFileReference writeStreamDo: [:s |
    s nextPutAll: 'SimpleStartup ran at: '.
    s nextPutAll: DateAndTime now printString.
    s cr]! !

!SimpleStartup class methodsFor: 'startup'!
startUp: resuming
  resuming ifTrue: [self run]! !

!SimpleStartup class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup at high priority"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #SimpleStartup)
  inCategory: SessionManager default userCategory
  atPriority: 50!
