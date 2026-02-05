"Test super sends"!

Object subclass: #SuperSendTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-Runner'!

!SuperSendTest class methodsFor: 'running'!
run
  | s method ctx |
  s := '/tmp/super_send_test.txt' asFileReference writeStream.
  [s nextPutAll: 'Super send test'; lf. s flush.

  method := Object >> #yourself.
  s nextPutAll: 'method = '; print: method; lf.
  s nextPutAll: 'method frameSize = '; print: method frameSize; lf. s flush.

  s nextPutAll: 'Calling Context newForMethod:'; lf. s flush.
  s nextPutAll: '(which calls super basicNew:)'; lf. s flush.

  [ctx := Context newForMethod: method.
   s nextPutAll: 'SUCCESS: ctx = '; print: ctx; lf]
  on: Error do: [:e |
    s nextPutAll: 'ERROR: '; print: e class; lf.
    s nextPutAll: 'Message: '; nextPutAll: e messageText; lf.

    "Check if this is the Context basicNew: error"
    (e messageText includesSubstring: 'must only be created') ifTrue: [
      s nextPutAll: '=> This means super send went to Context class, not Behavior'; lf]].
  s flush.

  "Also test a simpler super send"
  s nextPutAll: '---'; lf.
  s nextPutAll: 'Testing simpler super send...'; lf. s flush.
  [| arr |
   arr := Array new: 5.
   s nextPutAll: 'Array new: 5 = '; print: arr; lf.
   s nextPutAll: 'Size = '; print: arr size; lf]
  on: Error do: [:e |
    s nextPutAll: 'Array ERROR: '; print: e class; lf.
    s nextPutAll: 'Message: '; nextPutAll: e messageText; lf].
  s flush.

  s nextPutAll: 'DONE'; lf. s flush]
  ensure: [s close].

  Smalltalk exitSuccess! !

!SuperSendTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!SuperSendTest class methodsFor: 'startup'!
shutDown: quitting! !

SessionManager default
  register: (ClassSessionHandler forClassNamed: #SuperSendTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!

----End fileIn----!
