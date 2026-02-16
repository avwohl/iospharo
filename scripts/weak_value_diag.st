"Run WeakValueDictionaryTest in isolation to check for hangs"!

Object subclass: #WeakValueDiag
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Diag'!

UndefinedObject compile: 'findNextHandlerContext
  ^ nil' classified: 'private'!

!WeakValueDiag class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/weak_value_diag.txt' asFileReference writeStream.
  [self runOn: stream] ensure: [stream close].
  [Smalltalk exitSuccess] on: Error do: [:e |].
  Smalltalk quitPrimitive! !

!WeakValueDiag class methodsFor: 'running'!
runOn: s
  | sels tc p f e sk t |
  s nextPutAll: 'WeakValueDictionaryTest isolation run'; cr. s flush.
  tc := WeakValueDictionaryTest.
  sels := tc allTestSelectors asArray sort.
  s nextPutAll: 'Total selectors: '; nextPutAll: sels size printString; cr. s flush.
  p := 0. f := 0. e := 0. sk := 0. t := 0.
  sels do: [:sel |
    | result testDone testProcess |
    s nextPutAll: '  RUN: '; nextPutAll: sel; cr. s flush.
    result := nil. testDone := false.
    testProcess := [
      [[[(tc selector: sel) runCase]
        on: TestFailure do: [:ex | result := #fail.
          s nextPutAll: '  FAIL: '; nextPutAll: sel; cr. s flush]]
        on: Deprecation do: [:ex |
          [ex shouldTransform ifTrue: [ex transform]] on: Error do: [:e2 | ].
          ex resume]]
        on: Error do: [:ex | result := #error.
          s nextPutAll: '  ERROR: '; nextPutAll: sel; nextPutAll: ' (';
            nextPutAll: ex class name; nextPutAll: ': ';
            nextPutAll: (ex messageText copyFrom: 1 to: (ex messageText size min: 100));
            nextPutAll: ')'; cr. s flush].
      testDone := true] forkAt: 40.
    [(Delay forMilliseconds: 10000) wait.
     testDone ifFalse: [
       testDone := true. result := #timeout.
       s nextPutAll: '  TIMEOUT: '; nextPutAll: sel; cr. s flush.
       [testProcess suspend] on: Error do: [:e2 |]]] forkAt: 80.
    Processor activeProcess priority: 40.
    [testDone not] whileTrue: [Processor yield].
    Processor activeProcess priority: 80.
    result ifNil: [p := p + 1]
      ifNotNil: [
        result = #fail ifTrue: [f := f + 1].
        result = #error ifTrue: [e := e + 1].
        result = #skip ifTrue: [sk := sk + 1].
        result = #timeout ifTrue: [t := t + 1]]].
  s nextPutAll: 'Total: '; nextPutAll: sels size printString;
    nextPutAll: ' P:'; nextPutAll: p printString;
    nextPutAll: ' F:'; nextPutAll: f printString;
    nextPutAll: ' E:'; nextPutAll: e printString;
    nextPutAll: ' S:'; nextPutAll: sk printString;
    nextPutAll: ' T:'; nextPutAll: t printString; cr. s flush! !

!WeakValueDiag class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!WeakValueDiag class methodsFor: 'startup'!
shutDown: quitting! !

SessionManager default
  register: (ClassSessionHandler forClassNamed: #WeakValueDiag)
  inCategory: SessionManager default userCategory
  atPriority: 90!
