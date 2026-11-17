"ErikOnBike/CodeParadise (row 121): 33 tests, 19 pass, 13 skip, 1 fail.

A. Thirteen skips are 'self skip.' at the top of a test, with the comment
   'disabled because safety checks prevent it'.  The tests are in
   CpClientEnvironmentTest, CpCommunicatorTest and CpEncodeDecodeObjectTest
   (four, six and three tests).  The safety checks are, in the package:
     - CpClientEnvironment class>>current signals an error when the class
       CpServerEnvironment exists (it exists in a server image);
     - CpObjectDecoder>>decodeClass, decodeCompiledMethod, decodeCompiledBlock,
       decodeCleanBlockClosure and decodeConstantBlockClosure signal an error
       when CpClientEnvironment isRunning is false (Current is nil);
     - CpClientCommunicator class>>newForOwner: signals an error when the class
       CpServerCommunicator exists.
   The tests run both sides in one image on purpose.  The repair makes the
   test environment that the tests need, and does not remove a check for
   other code:
     1. CpClientEnvironment class>>current answers the instance of the class
        variable Current when there is one.  Only the code that creates the
        instance on a server is still refused.  On a real server Current is
        nil, so nothing changes there.
     2. setUp of the three test classes puts a CpClientEnvironment instance in
        Current (the variable primitives of the browser plugin fail here and
        answer nil, so the instance has no id and no communicator), and
        tearDown puts the old value back.
     3. CpCommunicatorTest builds its client communicator with
        initializeWithOwner: (the code of CpCommunicator class>>newForOwner:)
        and starts its server through a helper that remembers the server,
        because tearDown must stop the server: each test uses port 1701 and
        MessageChannelTest uses it too.
     4. Six communicator tests created the Semaphore after the connection.
        The server (or client) handler runs at once at a higher priority, and
        signals nil when the messages came before the Semaphore existed.  The
        Semaphore is now made at the start of each test.
   The 'self skip.' line goes from each of the thirteen tests.

B. CpReadStreamExtensionTest>>testLookAheadAt asserts the old index of
   ReadStream>>lookAheadAt:.  Commit 9f09c7f of the package (2021) moved the
   index by one in the extension and in the three callers (CpSmalltalkParser,
   FuiElement), and did not change the test.  The test now asserts the index
   that the extension and the callers use: 0 is the element that #peek answers,
   1 is the one after, -1 is the element that #next answered last.

All entries are guarded on text that only the old source has.  The entries go
in the package or in the tests, not in the Pharo image."

Smalltalk globals at: #CpParadiseRewrite put: [ :clsName :sel :pairs |
	| cls source newSource |
	cls := resolve value: clsName.
	(cls notNil and: [ cls includesSelector: sel ])
		ifFalse: [ Stdio stdout nextPutAll: 'SKIP ', clsName, '>>', sel, ' (absent)'; lf; flush ]
		ifTrue: [
			source := (cls >> sel) sourceCode.
			(source includesSubstring: pairs first key)
				ifFalse: [ Stdio stdout nextPutAll: 'SKIP ', clsName, '>>', sel, ' (defect not present)'; lf; flush ]
				ifTrue: [
					newSource := source.
					pairs do: [ :pair | newSource := newSource copyReplaceAll: pair key with: pair value ].
					cls compile: newSource classified: (cls >> sel) protocolName.
					Stdio stdout nextPutAll: 'FIXED ', clsName, '>>', sel; lf; flush ] ] ].

"The slots of the test classes.  Only slots that the classes do not have."
{
	{ #CpClientEnvironmentTest. #previousClientEnvironment }.
	{ #CpEncodeDecodeObjectTest. #previousClientEnvironment }.
	{ #CpCommunicatorTest. #previousClientEnvironment }.
	{ #CpCommunicatorTest. #servers }
} do: [ :spec | | cls |
	cls := resolve value: spec first.
	cls
		ifNil: [ Stdio stdout nextPutAll: 'SKIP slot ', spec first, ' (class absent)'; lf; flush ]
		ifNotNil: [
			(cls allInstVarNames includes: spec second)
				ifTrue: [ Stdio stdout nextPutAll: 'SKIP slot ', spec first, '>>', spec second, ' (already present)'; lf; flush ]
				ifFalse: [
					cls addSlot: (InstanceVariableSlot named: spec second).
					Stdio stdout nextPutAll: 'ADDED slot ', spec first, '>>', spec second; lf; flush ] ] ].

"A1. current answers an installed instance"
fix value: #'CpClientEnvironment class' value: #current
	value: [ :source | (source includesSubstring: 'Current ifNotNil:') not ]
	value:
'current

	"Answer the current instance of the receiver"

	Current ifNotNil: [ :environment | ^ environment ].

	(Smalltalk classNamed: #CpServerEnvironment)
		ifNotNil: [ self error: ''Creating ClientEnvironment instances on the server is temporarily disabled because of deadlock issues'' ].

	^ Current := self basicNew initialize ; yourself'.

"A2. setUp and tearDown of the three test classes"
#( #CpClientEnvironmentTest #CpEncodeDecodeObjectTest ) do: [ :name |
	add value: name value: #setUp value: 'running'
		value:
'setUp

	super setUp.

	"These tests run the client side code in the server image.  Make the environment that the client side checks (CpClientEnvironment isRunning) want.  The variable primitives of the browser plugin fail here and answer nil."
	previousClientEnvironment := CpClientEnvironment classVarNamed: #Current.
	CpClientEnvironment classVarNamed: #Current put: (CpClientEnvironment basicNew initialize ; yourself)'.
	add value: name value: #tearDown value: 'running'
		value:
'tearDown

	CpClientEnvironment classVarNamed: #Current put: previousClientEnvironment.

	super tearDown' ].

"A3. helpers of CpCommunicatorTest, with the list of servers."
add value: #CpCommunicatorTest value: #setUp value: 'running'
	value:
'setUp

	super setUp.

	servers := OrderedCollection new.

	"These tests run the client side code in the server image.  Make the environment that the client side checks (CpClientEnvironment isRunning) want.  The variable primitives of the browser plugin fail here and answer nil."
	previousClientEnvironment := CpClientEnvironment classVarNamed: #Current.
	CpClientEnvironment classVarNamed: #Current put: (CpClientEnvironment basicNew initialize ; yourself)'.

add value: #CpCommunicatorTest value: #tearDown value: 'running'
	value:
'tearDown

	"Each test uses port 1701: stop the servers"
	servers do: [ :each | each stop ].

	CpClientEnvironment classVarNamed: #Current put: previousClientEnvironment.

	super tearDown'.

add value: #CpCommunicatorTest value: #startServerForOwner:path: value: 'support'
	value:
'startServerForOwner: ownerObject path: aString

	"Start the server on port 1701 and remember it, tearDown stops it"

	| server |

	server := CpMessageChannel startServerForOwner: ownerObject onPort: 1701 path: aString.
	servers add: server.
	^ server'.

add value: #CpCommunicatorTest value: #newClientCommunicatorForOwner: value: 'support'
	value:
'newClientCommunicatorForOwner: ownerObject

	"CpClientCommunicator class >> #newForOwner: refuses to run in an image that has a CpServerCommunicator. These tests run both in one image, so do what CpCommunicator class >> #newForOwner: does."

	^ CpClientCommunicator basicNew
		initializeWithOwner: ownerObject ;
		yourself'.

"A4. the thirteen tests: no skip, helpers, the Semaphore at the start"
{
	"Class >> #removeMethod: of the package sends #removeInstanceMethod: while CpClientEnvironment isRunning, and that sends #removeSelector:, which sends #removeMethod: again: endless recursion.  The cleanup of the tests (ensure: block) removes a method or a class, so Current is nil again before the cleanup.  tearDown puts the old value back."
	{ 'CpClientEnvironmentTest'. #testInstallClass. { 'self skip.' -> '"enabled: setUp makes the client environment"'. 'ensure: [ ' -> 'ensure: [ CpClientEnvironment classVarNamed: #Current put: nil. ' } }.
	{ 'CpClientEnvironmentTest'. #testInstallMethod. { 'self skip.' -> '"enabled: setUp makes the client environment"'. 'ensure: [ ' -> 'ensure: [ CpClientEnvironment classVarNamed: #Current put: nil. ' } }.
	{ 'CpClientEnvironmentTest'. #testInstallMethodWithClassVarAccess. { 'self skip.' -> '"enabled: setUp makes the client environment"'. 'ensure: [ ' -> 'ensure: [ CpClientEnvironment classVarNamed: #Current put: nil. ' } }.
	{ 'CpClientEnvironmentTest'. #testInstallMethodWithInstVarAccess. { 'self skip.' -> '"enabled: setUp makes the client environment"'. 'ensure: [ ' -> 'ensure: [ CpClientEnvironment classVarNamed: #Current put: nil. ' } }.
	{ 'CpEncodeDecodeObjectTest'. #testCompiledMethod. { 'self skip.' -> '"enabled: setUp makes the client environment"' } }.
	{ 'CpEncodeDecodeObjectTest'. #testCompiledMethodWithAdditionalMethodState. { 'self skip.' -> '"enabled: setUp makes the client environment"' } }.
	{ 'CpEncodeDecodeObjectTest'. #testSortedCollection. { 'self skip.' -> '"enabled: setUp makes the client environment"' } }.
	{ 'CpCommunicatorTest'. #testConnectionStrategies. {
		'self skip.' -> '"enabled: setUp makes the client environment"'.
		'CpClientCommunicator newForOwner:' -> 'self newClientCommunicatorForOwner:'.
		'CpMessageChannel startServerForOwner: server onPort: 1701 path:' -> 'self startServerForOwner: server path:' } }
} do: [ :spec |
	(Smalltalk globals at: #CpParadiseRewrite) value: spec first value: spec second value: spec third ].

{ #testClientSendBeforeConnection. #testReconnectStrategy. #testSendReceive. #testSendWithReconnects. #testServerSendBeforeConnection } do: [ :selector |
	(Smalltalk globals at: #CpParadiseRewrite) value: 'CpCommunicatorTest' value: selector value: {
		'self skip.' -> 'semaphore := Semaphore new.'.
		'CpClientCommunicator newForOwner:' -> 'self newClientCommunicatorForOwner:'.
		'CpMessageChannel startServerForOwner: server onPort: 1701 path:' -> 'self startServerForOwner: server path:' } ].

"A4b. the later 'semaphore := Semaphore new.' of the tests would replace the Semaphore that the handlers already use.  The first pair of the entry above made the first one; remove the old ones.  Guard: the source has two of them."
{ #testClientSendBeforeConnection. #testSendReceive. #testServerSendBeforeConnection } do: [ :selector | | cls source index second |
	cls := resolve value: #CpCommunicatorTest.
	(cls notNil and: [ cls includesSelector: selector ])
		ifTrue: [
			source := (cls >> selector) sourceCode.
			index := source indexOfSubCollection: 'semaphore := Semaphore new.'.
			(index > 0 and: [ (source indexOfSubCollection: 'semaphore := Semaphore new.' startingAt: index + 1) > 0 ])
				ifTrue: [
					second := source indexOfSubCollection: 'semaphore := Semaphore new.' startingAt: index + 1.
					source := (source copyFrom: 1 to: second - 1), '"the Semaphore exists since the start of the test"', (source copyFrom: second + 'semaphore := Semaphore new.' size to: source size).
					cls compile: source classified: (cls >> selector) protocolName.
					Stdio stdout nextPutAll: 'FIXED CpCommunicatorTest>>', selector, ' (second Semaphore)'; lf; flush ] ] ].

"B. testLookAheadAt asserts the index of before commit 9f09c7f"
fix value: #CpReadStreamExtensionTest value: #testLookAheadAt
	value: [ :source | source includesSubstring: 'No current (at start)' ]
	value:
'testLookAheadAt

	"Index 0 is the element that #peek answers, 1 is the one after it and -1 is the element that #next answered last (see ReadStream >> #lookAheadAt: and its callers)"

	| readStream |

	readStream := ReadStream on: ''Hello world''.

	self assert: (readStream lookAheadAt: 0) = $H description: ''Look at next Character from start''.
	self assert: (readStream lookAheadAt: 1) = $e description: ''Look at second Character from start''.
	self assert: (readStream lookAheadAt: -1) isNil description: ''No current (at start)''.
	self assert: (readStream lookAheadAt: -2) isNil description: ''No previous (at start)''.
	self assert: (readStream lookAheadAt: readStream size - 1) = $d description: ''Look at last element''.

	readStream next: 5.
	self assert: (readStream lookAheadAt: 0) = Character space description: ''Look at next Character from middle''.
	self assert: (readStream lookAheadAt: 1) = $w description: ''Look at second Character from middle''.
	self assert: (readStream lookAheadAt: -1) = $o description: ''Current in middle''.
	self assert: (readStream lookAheadAt: -2) = $l description: ''Previous in middle''.

	readStream next: 6.
	self assert: readStream atEnd description: ''Read all Characters''.
	self assert: (readStream lookAheadAt: 0) isNil description: ''Look at next Character from end''.
	self assert: (readStream lookAheadAt: 1) isNil description: ''Look at second Character from end''.
	self assert: (readStream lookAheadAt: -1) = $d description: ''Current at end''.
	self assert: (readStream lookAheadAt: -2) = $l description: ''Previous at end'''.

Smalltalk globals removeKey: #CpParadiseRewrite ifAbsent: [ ].
