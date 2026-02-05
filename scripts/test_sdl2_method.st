"Find SDL2 isAvailable method"!

Object subclass: #SDL2MethodTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!SDL2MethodTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/sdl2_method.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== SDL2 Method Test ==='; cr; cr.

    "Find isAvailable"
    stream nextPutAll: '--- SDL2 isAvailable method ---'; cr.
    [| cls method |
     cls := SDL2 class.
     stream nextPutAll: 'SDL2 class = '; print: cls; cr.
     stream nextPutAll: 'SDL2 class superclass chain:'; cr.
     [cls notNil] whileTrue: [
       stream nextPutAll: '  '; print: cls; cr.
       (cls methodDict includesKey: #isAvailable) ifTrue: [
         method := cls >> #isAvailable.
         stream nextPutAll: '    FOUND isAvailable here'; cr.
         stream nextPutAll: '    source: '; nextPutAll: method sourceCode; cr].
       cls := cls superclass]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!SDL2MethodTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!SDL2MethodTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #SDL2MethodTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
