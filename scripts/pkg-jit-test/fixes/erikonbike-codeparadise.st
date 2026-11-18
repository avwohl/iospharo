"ErikOnBike/CodeParadise (row 121).  Before: 33 tests, 19 pass, 13 skip, 1 fail.

A. The six tests of CpCommunicatorTest were disabled with 'self skip.' (comment:
   'safety checks prevent it').  They run a client and a server Communicator in one
   image.  They did not pass for six reasons; each is repaired where it is:
   1. CpClientCommunicator>>checkConnection reconnected also when the communicator
      never had an URL.  #reconnect calls #connectToUrl: nil, which fails, and
      #connectToUrl: ends in #messageChannel: nil, which calls #checkConnection
      again: endless recursion.  testClientSendBeforeConnection sends messages
      before #connectToUrl:, so it hit this and ran until the watchdog of the
      test runner stopped it.  (The package guards the creation of a
      CpClientCommunicator in a server image, 'deadlock issues'; this is a
      likely cause.)  Package code: with no URL there is nothing to reconnect to.
   2. CpClientCommunicator>>reconnect sent 'CpClientEnvironment current
      restartCommunication'.  Only a client has a ClientEnvironment; in a server
      image #current refuses.  Now #restartCommunication is sent when a
      ClientEnvironment is running (the real client: always).  Package code.
   3. Since commit 0fb2f0a (2021) a new CpClientCommunicator has the strategy to
      always reconnect (#initialize).  testConnectionStrategies and
      testSendWithReconnects test the state without a strategy ('Default no
      reconnect', 'Client is no longer connected'); they predate the commit.  The
      class comment says: without a strategy a lost connection is not restored.
      The tests now set 'reconnectStrategy: nil' and keep their assertions.
   4. Three tests made the Semaphore after the connection.  The handler process
      (priority highIOPriority) of the other side runs at once, finds the Semaphore
      nil and fails, and the signal is lost.  The Semaphore is made at the start.
   5. CpClientCommunicator class>>newForOwner: refuses in an image that has a
      CpServerCommunicator.  The tests build the client with #initializeWithOwner:
      (the code of CpCommunicator class>>newForOwner:) through a helper.
   6. All tests use port 1701.  A test that stops early left its ZnServer and its
      channels.  setUp and tearDown of the test class now stop them.
B. Seven tests need the client environment and keep their skip, now with a reason
   text: they cannot apply in a server image (proof in the .not-applicable.txt file).
C. CpReadStreamExtensionTest>>testLookAheadAt asserts the index of before commit
   9f09c7f (2021).  That commit moved the index by one in ReadStream>>lookAheadAt:
   and in its callers, and did not change the test.  The test now uses the index of
   the extension: 0 is the element that #peek answers, 1 the one after it, -1 the
   element that #next answered last.  No assertion is removed.

All entries are guarded on text that only the old source has."


"The helpers of CpCommunicatorTest.  Slots first."
{ #servers. #clients } do: [ :name | | cls |
	cls := resolve value: #CpCommunicatorTest.
	cls
		ifNil: [ Stdio stdout nextPutAll: 'SKIP slot CpCommunicatorTest>>', name, ' (class absent)'; lf; flush ]
		ifNotNil: [
			(cls allInstVarNames includes: name)
				ifTrue: [ Stdio stdout nextPutAll: 'SKIP slot CpCommunicatorTest>>', name, ' (already present)'; lf; flush ]
				ifFalse: [
					cls addSlot: (InstanceVariableSlot named: name).
					Stdio stdout nextPutAll: 'ADDED slot CpCommunicatorTest>>', name; lf; flush ] ] ].

add value: #CpCommunicatorTest value: #setUp value: 'running'
	value: 'setUp

	super setUp.

	servers := OrderedCollection new.
	clients := OrderedCollection new'.

add value: #CpCommunicatorTest value: #tearDown value: 'running'
	value: 'tearDown

	"Close the clients first (the server side channels then end), without a reconnect. All tests use port 1701: stop the servers"
	clients do: [ :each | each reconnectStrategy: nil ; close ].
	servers do: [ :each | each stop ].

	super tearDown'.

add value: #CpCommunicatorTest value: #startServerForOwner:path: value: 'support'
	value: 'startServerForOwner: ownerObject path: aString

	"Start the server on port 1701 and remember it, so tearDown stops it"

	| server |

	server := CpMessageChannel startServerForOwner: ownerObject onPort: 1701 path: aString.
	servers add: server.
	^ server'.

add value: #CpCommunicatorTest value: #newClientCommunicatorForOwner: value: 'support'
	value: 'newClientCommunicatorForOwner: ownerObject

	"CpClientCommunicator class >> #newForOwner: refuses to run in an image that has a CpServerCommunicator.
	These tests run both sides in one image, so do what CpCommunicator class >> #newForOwner: does.
	Remember the client, so tearDown closes it."

	| client |

	client := CpClientCommunicator basicNew
		initializeWithOwner: ownerObject ;
		yourself.
	clients add: client.
	^ client'.

fix value: #CpClientCommunicator value: #checkConnection value: [ :source | (source includesSubstring: 'url ifNil:') not ] value:
'checkConnection

	"Check connection and restore connection if needed, according to receiver''s reconnect strategy"

	(self hasOpenMessageChannel or: [ self hasReconnectStrategy not ])
		ifTrue: [ ^ self ].

	"Without an URL no connection was ever made, so there is nothing to reconnect to. (Reconnecting
	without an URL makes #connectToUrl: fail, which evaluates #checkConnection again, and so on.)"
	url ifNil: [ ^ self ].

	"Reconnect according to the receiver''s reconnect strategy"
	(self reconnectStrategy shouldReconnect: self)
		ifTrue: [ self reconnect ]'.
fix value: #CpClientCommunicator value: #reconnect value: [ :source | (source includesSubstring: 'isRunning') not ] value:
'reconnect

	"Reconnect the receiver to the previous URL"

	self connectToUrl: self url.

	"Let the server know on which WebSocket we reconnected by restarting communication.
	Only a ClientEnvironment can do this, so skip it for a Communicator which runs without one."
	CpClientEnvironment isRunning
		ifTrue: [ CpClientEnvironment current restartCommunication ]'.
fix value: #CpCommunicatorTest value: #testClientSendBeforeConnection value: [ :source | source includesSubstring: 'self skip.' ] value:
'testClientSendBeforeConnection

	| originalClientMessages originalServerMessages client clientOwner clientMessages server serverOwner serverMessages semaphore |

	semaphore := Semaphore new.

	originalClientMessages := OrderedCollection newFrom: (
		{ ''Hello'' . ''World'' . ''Welcome'' } collect: [ :each | each asByteArray ]).
	originalServerMessages := OrderedCollection newFrom: (
		{ ''42'' . ''is'' . ''the'' . ''answer'' } collect: [ :each | each asByteArray ]).

	serverMessages := OrderedCollection new.
	serverOwner := CpMessageHandler new
		onMessage: [ :message |
			serverMessages add: message.
			serverMessages size = 3
				ifTrue: [ semaphore signal ] ] ;
		yourself.
	server := CpServerCommunicator newForOwner: serverOwner.

	clientMessages := OrderedCollection new.
	clientOwner := CpMessageHandler new
		onMessage: [ :message |
			clientMessages add: message.
			clientMessages size = 4
				ifTrue: [ semaphore signal ] ] ;
		yourself.
	client := self newClientCommunicatorForOwner: clientOwner.

	"Create a server for incoming connections and have the server side Communicator own it"
	self startServerForOwner: server path: ''/test''.

	originalClientMessages do: [ :each | client sendMessage: each ].

	"Make connection"
	client connectToUrl: ''ws://localhost:1701/test''.

	"Send messages"
	[ originalServerMessages do: [ :each | server sendMessage: each ] ] fork.
	semaphore wait.
	semaphore wait.

	"Validate messages"
	self
		assertCollection: originalClientMessages equals: serverMessages ;
		assertCollection: originalServerMessages equals: clientMessages'.
fix value: #CpCommunicatorTest value: #testConnectionStrategies value: [ :source | source includesSubstring: 'self skip.' ] value:
'testConnectionStrategies

	| client clientOwner server serverOwner |

	"Server and client do not receive messages, so empty owners"
	serverOwner := CpMessageHandler new.
	server := CpServerCommunicator newForOwner: serverOwner.

	clientOwner := CpMessageHandler new.
	client := self newClientCommunicatorForOwner: clientOwner.

	"Without a reconnect strategy a lost connection is not restored. Since 2021 (commit 0fb2f0a) a new
	CpClientCommunicator has the strategy to always reconnect, which this test predates."
	client reconnectStrategy: nil.

	"Create a server for incoming connections and have the server side Communicator own it"
	self startServerForOwner: server path: ''/test''.

	"Make connection"
	client connectToUrl: ''ws://localhost:1701/test''.
	self assert: client hasOpenMessageChannel description: ''Made first connection''.

	"Close connection and perform #checkConnection"
	client
		close ;
		checkConnection.
	self assert: client hasOpenMessageChannel not description: ''No reconnect without reconnect strategy''.

	"Set reconnect strategy to ''always''"
	client reconnectStrategy: CpCommunicatorReconnectStrategy new.

	"Close connection and perform #checkConnection"
	client
		close ;
		checkConnection.
	self assert: client hasOpenMessageChannel description: ''Reconnect with reconnection strategy from client close'''.
fix value: #CpCommunicatorTest value: #testReconnectStrategy value: [ :source | source includesSubstring: 'self skip.' ] value:
'testReconnectStrategy

	| originalClientMessages client clientOwner server serverOwner serverMessages semaphore |

	originalClientMessages := OrderedCollection newFrom: (
		{ ''Hello'' . ''World'' . ''Welcome'' } collect: [ :each | each asByteArray ]).

	semaphore := Semaphore new.
	serverMessages := OrderedCollection new.
	serverOwner := CpMessageHandler new
		onMessage: [ :message |
			serverMessages add: message.
			serverMessages size = 2
				ifTrue: [ server messageChannel close ].
			serverMessages size >= 2
				ifTrue: [ semaphore signal ] ] ;
		yourself.
	server := CpServerCommunicator newForOwner: serverOwner.

	"Client does not receive messages, so empty owner"
	clientOwner := CpMessageHandler new.
	client := (self newClientCommunicatorForOwner: clientOwner)
		reconnectStrategy: CpCommunicatorReconnectStrategy new ;
		yourself.

	"Create a server for incoming connections and have the server side Communicator own it"
	self startServerForOwner: server path: ''/test''.

	"Make connection"
	client connectToUrl: ''ws://localhost:1701/test''.
	self assert: client hasOpenMessageChannel description: ''Made first connection''.

	"Send first message"
	client sendMessage: originalClientMessages first.

	"Close connection from client and send second message"
	client close.

	"Send second message (server will close as a result)"
	client sendMessage: originalClientMessages second.
	semaphore wait.

	"Send third message"	
	client sendMessage: originalClientMessages third.

	"Wait for final message"
	semaphore wait.

	"Validate messages"
	self assertCollection: originalClientMessages equals: serverMessages'.
fix value: #CpCommunicatorTest value: #testSendReceive value: [ :source | source includesSubstring: 'self skip.' ] value:
'testSendReceive

	| originalClientMessages originalServerMessages client clientOwner clientMessages server serverOwner serverMessages semaphore |

	semaphore := Semaphore new.

	originalClientMessages := OrderedCollection newFrom: (
		{ ''Hello'' . ''World'' . ''Welcome'' } collect: [ :each | each asByteArray ]).
	originalServerMessages := OrderedCollection newFrom: (
		{ ''42'' . ''is'' . ''the'' . ''answer'' } collect: [ :each | each asByteArray ]).

	serverMessages := OrderedCollection new.
	serverOwner := CpMessageHandler new
		onMessage: [ :message |
			serverMessages add: message.
			serverMessages size = 3
				ifTrue: [ semaphore signal ] ] ;
		yourself.
	server := CpServerCommunicator newForOwner: serverOwner.

	clientMessages := OrderedCollection new.
	clientOwner := CpMessageHandler new
		onMessage: [ :message |
			clientMessages add: message.
			clientMessages size = 4
				ifTrue: [ semaphore signal ] ] ;
		yourself.
	client := self newClientCommunicatorForOwner: clientOwner.

	"Create a server for incoming connections and have the server side Communicator own it"
	self startServerForOwner: server path: ''/test''.

	"Make connection"
	client connectToUrl: ''ws://localhost:1701/test''.

	"Send messages"
	[ originalClientMessages do: [ :each | client sendMessage: each ] ] fork.
	[ originalServerMessages do: [ :each | server sendMessage: each ] ] fork.
	semaphore wait.
	semaphore wait.

	"Validate messages"
	self
		assertCollection: originalClientMessages equals: serverMessages ;
		assertCollection: originalServerMessages equals: clientMessages'.
fix value: #CpCommunicatorTest value: #testSendWithReconnects value: [ :source | source includesSubstring: 'self skip.' ] value:
'testSendWithReconnects

	| originalClientMessages client clientOwner server serverOwner serverMessages semaphore |

	originalClientMessages := OrderedCollection newFrom: (
		{ ''Hello'' . ''World'' . ''Welcome'' . ''Here'' } collect: [ :each | each asByteArray ]).

	semaphore := Semaphore new.
	serverMessages := OrderedCollection new.
	serverOwner := CpMessageHandler new
		onMessage: [ :message |
			serverMessages add: message.
			serverMessages size = 4
				ifTrue: [ semaphore signal ] ] ;
		yourself.
	server := CpServerCommunicator newForOwner: serverOwner.

	"Client does not receive messages, so empty owner"
	clientOwner := CpMessageHandler new.
	client := self newClientCommunicatorForOwner: clientOwner.

	"Without a reconnect strategy a lost connection is not restored. Since 2021 (commit 0fb2f0a) a new
	CpClientCommunicator has the strategy to always reconnect, which this test predates."
	client reconnectStrategy: nil.

	"Create a server for incoming connections and have the server side Communicator own it"
	self startServerForOwner: server path: ''/test''.

	"Make connection"
	client connectToUrl: ''ws://localhost:1701/test''.
	self assert: client hasOpenMessageChannel description: ''Made first connection''.

	"Send first message"
	client sendMessage: originalClientMessages first.

	"Close connection and send second message"
	client close.
	client sendMessage: originalClientMessages second.
	self assert: client hasOpenMessageChannel not description: ''Client is no longer connected''.

	"Make new connection and send third message"	
	client connectToUrl: ''ws://127.0.0.1:1701/test''.
	self assert: client hasOpenMessageChannel description: ''Make second connection''.

	client sendMessage: originalClientMessages third.

	"Reconnect and send fourth message"
	client reconnect.
	self assert: client hasOpenMessageChannel description: ''Make third connection''.
	client sendMessage: originalClientMessages fourth.

	"Wait for final message"
	semaphore wait.

	"Validate messages"
	self assertCollection: originalClientMessages equals: serverMessages'.
fix value: #CpCommunicatorTest value: #testServerSendBeforeConnection value: [ :source | source includesSubstring: 'self skip.' ] value:
'testServerSendBeforeConnection

	| originalClientMessages originalServerMessages client clientOwner clientMessages server serverOwner serverMessages semaphore |

	semaphore := Semaphore new.

	originalClientMessages := OrderedCollection newFrom: (
		{ ''Hello'' . ''World'' . ''Welcome'' } collect: [ :each | each asByteArray ]).
	originalServerMessages := OrderedCollection newFrom: (
		{ ''42'' . ''is'' . ''the'' . ''answer'' } collect: [ :each | each asByteArray ]).

	serverMessages := OrderedCollection new.
	serverOwner := CpMessageHandler new
		onMessage: [ :message |
			serverMessages add: message.
			serverMessages size = 3
				ifTrue: [ semaphore signal ] ] ;
		yourself.
	server := CpServerCommunicator newForOwner: serverOwner.

	clientMessages := OrderedCollection new.
	clientOwner := CpMessageHandler new
		onMessage: [ :message |
			clientMessages add: message.
			clientMessages size = 4
				ifTrue: [ semaphore signal ] ] ;
		yourself.
	client := self newClientCommunicatorForOwner: clientOwner.

	"Create a server for incoming connections and have the server side Communicator own it"
	self startServerForOwner: server path: ''/test''.

	originalServerMessages do: [ :each | server sendMessage: each ].

	"Make connection"
	client connectToUrl: ''ws://localhost:1701/test''.

	"Send messages"
	[ originalClientMessages do: [ :each | client sendMessage: each ] ] fork.
	semaphore wait.
	semaphore wait.

	"Validate messages"
	self
		assertCollection: originalClientMessages equals: serverMessages ;
		assertCollection: originalServerMessages equals: clientMessages'.
fix value: #CpClientEnvironmentTest value: #testInstallClass value: [ :source | source includesSubstring: 'self skip.' ] value:
'testInstallClass

	"These tests are disabled because safety checks prevent it (to prevent CompiledMethods from being installed)"

	| clientEnvironment someClass |

	self skip: ''The test needs the singleton CpClientEnvironment current. The package refuses to make it in an image that has CpServerEnvironment (this is a server image). See erikonbike-codeparadise.not-applicable.txt.''.

	clientEnvironment := CpClientEnvironment current.
	self
		assert: (Smalltalk classNamed: #CpTestClassToBeRemoved) isNil
		description: ''Class not already present''.

	someClass := Object subclass: #CpTestClassToBeRemoved
		instanceVariableNames: ''instVar1 instVar2 instVar3''
		classVariableNames: ''classVar1 classVar2''
		package: self class category.

	[
		clientEnvironment handleMessageReceived: (MessageSend
			receiver: nil
			selector: #installClass:
			arguments: { someClass }) cpEncoded.

		self
			assert: (Smalltalk classNamed: #CpTestClassToBeRemoved) isNotNil
			description: ''Class is installed''.
		self
			assertCollection: (Smalltalk classNamed: #CpTestClassToBeRemoved) instVarNames
			equals: { #instVar1. #instVar2. #instVar3 }.
		self
			assertCollection: (Smalltalk classNamed: #CpTestClassToBeRemoved) classVarNames
			equals: {  #classVar1. #classVar2 } 
	] ensure: [ Smalltalk removeClassNamed: #CpTestClassToBeRemoved ]'.
fix value: #CpClientEnvironmentTest value: #testInstallMethod value: [ :source | source includesSubstring: 'self skip.' ] value:
'testInstallMethod

	"These tests are disabled because safety checks prevent it (to prevent CompiledMethods from being installed)"

	| clientEnvironment someMethod |

	self skip: ''The test needs the singleton CpClientEnvironment current. The package refuses to make it in an image that has CpServerEnvironment (this is a server image). See erikonbike-codeparadise.not-applicable.txt.''.

	clientEnvironment := CpClientEnvironment current.
	self
		assert: (self class methodDict includesKey: #temporaryMethod) not
		description: ''Method not already present''.

	[
		"Compile new method for class, test it did not get installed"
		someMethod := self class compiler
			source: ''temporaryMethod ^ 42'' ;
			requestor: nil ;
			failBlock: [ ^ nil ];
			compile.
		self assert: someMethod notNil description: ''Temporary method compiled''.
		self
			assert: (self class methodDict includesKey: #temporaryMethod) not
			description: ''Method not present after compilation''.

		clientEnvironment handleMessageReceived: (MessageSend
			receiver: nil
			selector: #installMethod:
			arguments: { someMethod }) cpEncoded.
		self assert: (self perform: #temporaryMethod) = 42 description: ''Compiled method answers correct value''
	] ensure: [ self class removeSelector: #temporaryMethod ]'.
fix value: #CpClientEnvironmentTest value: #testInstallMethodWithClassVarAccess value: [ :source | source includesSubstring: 'self skip.' ] value:
'testInstallMethodWithClassVarAccess

	"These tests are disabled because safety checks prevent it (to prevent CompiledMethods from being installed)"

	| clientEnvironment someMethod |

	self skip: ''The test needs the singleton CpClientEnvironment current. The package refuses to make it in an image that has CpServerEnvironment (this is a server image). See erikonbike-codeparadise.not-applicable.txt.''.

	clientEnvironment := CpClientEnvironment current.
	self
		assert: (CpClientEnvironmentTestObject classSide methodDict includesKey: #temporaryMethod) not
		description: ''Method not already present''.

	[
		"Compile new class method for class, test it did not get installed"
		someMethod := CpClientEnvironmentTestObject classSide compiler
			source: ''temporaryMethod SomeClassVar := #test'' ;
			requestor: nil ;
			failBlock: [ ^ nil ];
			compile.
		self assert: someMethod notNil description: ''Temporary method compiled''.
		self
			assert: (CpClientEnvironmentTestObject classSide methodDict includesKey: #temporaryMethod) not
			description: ''Method not present after compilation''.

		clientEnvironment handleMessageReceived: (MessageSend
			receiver: nil
			selector: #installMethod:
			arguments: { someMethod }) cpEncoded.

		CpClientEnvironmentTestObject perform: #temporaryMethod.
		self
			assert: CpClientEnvironmentTestObject someClassVar = #test
			description: ''Compiled method answers correct value''
	] ensure: [ CpClientEnvironmentTestObject classSide removeSelector: #temporaryMethod ]'.
fix value: #CpClientEnvironmentTest value: #testInstallMethodWithInstVarAccess value: [ :source | source includesSubstring: 'self skip.' ] value:
'testInstallMethodWithInstVarAccess

	"These tests are disabled because safety checks prevent it (to prevent CompiledMethods from being installed)"

	| clientEnvironment someMethod instance |

	self skip: ''The test needs the singleton CpClientEnvironment current. The package refuses to make it in an image that has CpServerEnvironment (this is a server image). See erikonbike-codeparadise.not-applicable.txt.''.

	clientEnvironment := CpClientEnvironment current.
	self
		assert: (CpClientEnvironmentTestObject instanceSide methodDict includesKey: #temporaryMethod) not
		description: ''Method not already present''.

	[
		"Compile new instance method for class, test it did not get installed"
		someMethod := CpClientEnvironmentTestObject instanceSide compiler
			source: ''temporaryMethod someInstVar := 123. otherInstVar := self'' ;
			requestor: nil ;
			failBlock: [ ^ nil ];
			compile.
		self assert: someMethod notNil description: ''Temporary method compiled''.
		self
			assert: (CpClientEnvironmentTestObject instanceSide methodDict includesKey: #temporaryMethod) not
			description: ''Method not present after compilation''.

		clientEnvironment handleMessageReceived: (MessageSend
			receiver: nil
			selector: #installMethod:
			arguments: { someMethod }) cpEncoded.

		instance := CpClientEnvironmentTestObject new.
		instance perform: #temporaryMethod.
		self
			assert: instance someInstVar = 123
			description: ''Compiled method answers correct value''.
		self
			assert: instance otherInstVar = instance
			description: ''Compiled method answers correct reference to self''
	] ensure: [ CpClientEnvironmentTestObject instanceSide removeSelector: #temporaryMethod ]'.
fix value: #CpEncodeDecodeObjectTest value: #testCompiledMethod value: [ :source | source includesSubstring: 'self skip.' ] value:
'testCompiledMethod

	"These tests are disabled because safety checks prevent it (to prevent CompiledMethods from being installed)"

	self skip: ''CpObjectDecoder>>decodeCompiledMethod runs only in the client environment, and it makes the method in the format of the tiny client image (4 byte trailer; this image has 5). See erikonbike-codeparadise.not-applicable.txt.''.

	self
		assert: (self class compareMethod: (CpObjectDecoder on: (CpObjectEncoder new encodeCompiledMethod: (CpEncodeDecodeObjectTest >> #testCompiledMethod) ; bytes)) decodeNext to: CpEncodeDecodeObjectTest >> #testCompiledMethod)
		description: ''Encode/decode CpEncodeDecodeMessageTest >> #testCompiledMethod''.
	self
		assert: (self class compareMethod: (CpObjectDecoder on: (CpObjectEncoder new encodeCompiledMethod: (ByteString >> #isByteString) ; bytes)) decodeNext to: ByteString >> #isByteString)
		description: ''Encode/decode ByteString >> #isByteString''.
	self
		assert: (self class compareMethod: (CpObjectDecoder on: (CpObjectEncoder new encodeCompiledMethod: (ByteString >> #isByteString) ; bytes)) decodeNext to: ByteSymbol >> #isByteString) not
		description: ''Encode/decode ByteString >> #isByteString and ByteSymbol >> #isByteString (same implementation but different class)'''.
fix value: #CpEncodeDecodeObjectTest value: #testCompiledMethodWithAdditionalMethodState value: [ :source | source includesSubstring: 'self skip.' ] value:
'testCompiledMethodWithAdditionalMethodState

	"These tests are disabled because safety checks prevent it (to prevent CompiledMethods from being installed)"

	<somePragma: #someValue>

	self skip: ''CpObjectDecoder>>decodeCompiledMethod runs only in the client environment, and it makes the method in the format of the tiny client image (4 byte trailer; this image has 5). See erikonbike-codeparadise.not-applicable.txt.''.

	self
		assert: (self class compareMethod: (CpObjectDecoder on: (CpObjectEncoder new encodeCompiledMethod: (CpEncodeDecodeObjectTest >> #testCompiledMethodWithAdditionalMethodState) ; bytes)) decodeNext to: CpEncodeDecodeObjectTest >> #testCompiledMethodWithAdditionalMethodState)
		description: ''Encode/decode CpEncodeDecodeMessageTest >> #testCompiledMethodWithAdditionalMethodState'''.
fix value: #CpEncodeDecodeObjectTest value: #testSortedCollection value: [ :source | source includesSubstring: 'self skip.' ] value:
'testSortedCollection

	"These tests are disabled because safety checks prevent it (to prevent CompiledMethods from being installed).
	Tests have been performed by manually removing the safe guards. Need to find a mechanism to make this
	possible from inside these tests. Since it is a security risks, I''m not too keen on this."

	self skip: ''CpObjectDecoder>>decodeCleanBlockClosure runs only in the client environment (CpClientEnvironment isRunning), which a server image does not have. See erikonbike-codeparadise.not-applicable.txt.''.

	"SortedCollection are only supported when CleanBlockClosures are enabled"

	self class compiler compilationContext optionCleanBlockClosure 
		ifFalse: [ ^ self ].

	self
		assert: (CpObjectDecoder on: (CpObjectEncoder new encodeSortedCollection: (SortedCollection sortBlock: [ :a :b | a > b ]) ; bytes)) decodeNext isEmpty
		description: ''Encode/decode empty SortedCollection has no elements''.
	self
		assert: (CpObjectDecoder on: (CpObjectEncoder new encodeSortedCollection: ((SortedCollection sortBlock: [ :a :b | a > b ]) addAll: { 1 . 3 . 2 } ; yourself) ; bytes)) decodeNext first = 3
		description: ''Encode/decode SortedCollection first is in order''.
	self
		assert: (CpObjectDecoder on: (CpObjectEncoder new encodeSortedCollection: ((SortedCollection sortBlock: [ :a :b | a > b ]) addAll: { 1 . 3 . 2 } ; yourself) ; bytes)) decodeNext last = 1
		description: ''Encode/decode SortedCollection last is in order'''.
fix value: #CpReadStreamExtensionTest value: #testLookAheadAt value: [ :source | source includesSubstring: 'No current (at start)' ] value:
'testLookAheadAt

	"Index 0 is the element that #peek answers, 1 is the one after it and -1 is the element that #next answered last
	(see ReadStream >> #lookAheadAt: and its callers)"

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
