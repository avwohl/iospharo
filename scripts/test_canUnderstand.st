"Test canUnderstand: bug investigation"!

Object subclass: #CanUnderstandTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!CanUnderstandTest class methodsFor: 'running'!
run
  | stream driver driverClass |
  stream := '/tmp/canunderstand_test.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== canUnderstand: Investigation ==='; cr; cr.

    driverClass := Smalltalk at: #OSiOSDriver.
    driver := driverClass new.

    "Test 1: Check canUnderstand: for known method"
    stream nextPutAll: '--- Test 1: canUnderstand: ---'; cr.
    stream nextPutAll: 'OSiOSDriver canUnderstand: #setupEventLoop = '.
    stream print: (driverClass canUnderstand: #setupEventLoop); cr.
    stream nextPutAll: 'OSiOSDriver canUnderstand: #initialize = '.
    stream print: (driverClass canUnderstand: #initialize); cr.
    stream flush.

    "Test 2: Check lookupSelector:"
    stream cr; nextPutAll: '--- Test 2: lookupSelector: ---'; cr.
    stream nextPutAll: 'OSiOSDriver lookupSelector: #setupEventLoop = '.
    stream print: (driverClass lookupSelector: #setupEventLoop); cr.
    stream nextPutAll: 'OSiOSDriver lookupSelector: #initialize = '.
    stream print: (driverClass lookupSelector: #initialize); cr.
    stream flush.

    "Test 3: Check method dictionary directly"
    stream cr; nextPutAll: '--- Test 3: Method dictionary ---'; cr.
    stream nextPutAll: 'OSiOSDriver methodDict class = '.
    stream print: driverClass methodDict class; cr.
    stream nextPutAll: 'OSiOSDriver methodDict size = '.
    stream print: driverClass methodDict size; cr.
    stream nextPutAll: 'OSiOSDriver methodDict keys first 10 = '.
    stream print: (driverClass methodDict keys asArray copyFrom: 1 to: (driverClass methodDict keys size min: 10)); cr.
    stream flush.

    "Test 4: Check if setupEventLoop is in methodDict"
    stream cr; nextPutAll: '--- Test 4: Direct dict lookup ---'; cr.
    stream nextPutAll: 'OSiOSDriver methodDict includesKey: #setupEventLoop = '.
    stream print: (driverClass methodDict includesKey: #setupEventLoop); cr.
    stream nextPutAll: 'OSiOSDriver methodDict at: #setupEventLoop = '.
    [stream print: (driverClass methodDict at: #setupEventLoop)]
      on: Error do: [:e | stream nextPutAll: 'ERROR: '; print: e class].
    stream cr.
    stream flush.

    "Test 5: Walk superclass chain"
    stream cr; nextPutAll: '--- Test 5: Superclass chain ---'; cr.
    [| cls count |
     cls := driverClass.
     count := 0.
     [cls notNil and: [count < 10]] whileTrue: [
       stream nextPutAll: '  '; print: cls name; nextPutAll: ' ('; print: cls methodDict size; nextPutAll: ' methods)'; cr.
       cls := cls superclass.
       count := count + 1].
     stream flush]
    on: Error do: [:e | stream nextPutAll: 'ERROR: '; print: e class; cr].

    "Test 6: withAllSuperclassesDo:"
    stream cr; nextPutAll: '--- Test 6: withAllSuperclassesDo: ---'; cr.
    [driverClass withAllSuperclassesDo: [:cls |
       stream nextPutAll: '  Checking '; print: cls name; cr.
       cls compiledMethodAt: #setupEventLoop ifPresent: [:m |
         stream nextPutAll: '    FOUND: '; print: m; cr].
       stream flush]]
    on: Error do: [:e | stream nextPutAll: 'ERROR: '; print: e class; nextPutAll: ': '; print: e messageText; cr. stream flush].

    "Test 7: classAndMethodFor:do:ifAbsent:"
    stream cr; nextPutAll: '--- Test 7: classAndMethodFor:do:ifAbsent: ---'; cr.
    [| result |
     result := driverClass
       classAndMethodFor: #setupEventLoop
       do: [:c :m | stream nextPutAll: '  Found in '; print: c name; nextPutAll: ': '; print: m; cr. true]
       ifAbsent: [stream nextPutAll: '  NOT FOUND'; cr. false].
     stream nextPutAll: '  Result: '; print: result; cr]
    on: Error do: [:e | stream nextPutAll: 'ERROR: '; print: e class; nextPutAll: ': '; print: e messageText; cr. stream flush].

    "Test 8: compiledMethodAt:ifPresent: on OSiOSDriver directly"
    stream cr; nextPutAll: '--- Test 8: Direct compiledMethodAt:ifPresent: ---'; cr.
    [| found |
     found := false.
     driverClass compiledMethodAt: #setupEventLoop ifPresent: [:m |
       stream nextPutAll: '  Method: '; print: m; cr.
       found := true].
     found ifFalse: [stream nextPutAll: '  Not found via ifPresent:'; cr].
     stream flush]
    on: Error do: [:e | stream nextPutAll: 'ERROR: '; print: e class; nextPutAll: ': '; print: e messageText; cr. stream flush].

    stream cr; nextPutAll: '=== DONE ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!CanUnderstandTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!CanUnderstandTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #CanUnderstandTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
