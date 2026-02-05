"Test factorial computation"!

Object subclass: #FactorialTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!FactorialTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/factorial_test.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== Factorial Test ==='; cr.

    "Test small factorials"
    #(5 10 20 50 100 200 500) do: [:n |
      | result str |
      result := n factorial.
      str := result printString.
      stream nextPutAll: n printString; nextPutAll: ' factorial = '.
      stream nextPutAll: (str copyFrom: 1 to: (str size min: 80)).
      str size > 80 ifTrue: [stream nextPutAll: '... ('; print: str size; nextPutAll: ' digits)'].
      stream cr].

    "Test slowFactorial vs factorial comparison - full range like testSlowFactorial"
    stream cr; nextPutAll: '=== slowFactorial vs factorial (0 to 1000) ==='; cr.
    0 to: 1000 do: [:n |
      | slow fast slowStr fastStr |
      slow := n slowFactorial.
      fast := n factorial.
      slow = fast ifFalse: [
        slowStr := slow printString.
        fastStr := fast printString.
        stream nextPutAll: 'MISMATCH at '; nextPutAll: n printString; cr.
        stream nextPutAll: '  slowFactorial digits: '; print: slowStr size; cr.
        stream nextPutAll: '  factorial digits: '; print: fastStr size; cr.
        stream nextPutAll: '  slowFactorial first 80: '; nextPutAll: (slowStr copyFrom: 1 to: (slowStr size min: 80)); cr.
        stream nextPutAll: '  factorial first 80: '; nextPutAll: (fastStr copyFrom: 1 to: (fastStr size min: 80)); cr.
        stream flush]].

    stream nextPutAll: '=== DONE ==='; cr.
    stream flush]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!FactorialTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!FactorialTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #FactorialTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
