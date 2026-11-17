"NathanFrund/blenny: repairs for the tests of the package Blenny-Tests and for
three small defects of the package code.  The load needs two more files
(patches/nathanfrund-blenny.patch and locks/nathanfrund-blenny.txt); they are
not entries of this file.

1. BlennySimpleAuthTest>>testBlacklistCleanup had the body 'self skip' with the
   remark that the blacklist and #cleanupBlacklist are not public API.  Both
   are public: BlennySimpleAuth>>cleanupBlacklist and the repository protocol
   saveBlacklistEntry:expiration: exist.  The test now stores one expired and
   one valid entry, runs the cleanup and checks that only the expired entry is
   gone.

2. BlennySimpleAuthTest>>testTokenExpiration had the body 'self skip' with the
   remark that the token needs a changed duration.  The test now signs two
   tokens with the secret of the auth object (HS256, as createJwtForUser:
   does): one with exp in the past, which validateToken: must reject, and one
   with exp in the future, which validateToken: must accept.

3. BlennyWebSocketBridgeTest>>testAddAndRemoveClient had the body 'self skip'
   because it needs a WebSocket.  The bridge only calls #sendMessage: and
   #close on the stream that it gets, so the class BlennyFakeWebSocket (in
   the package Blenny-Tests, not a TestCase) records the messages and the
   close.  The test registers a client, sends HTML to all and to one user,
   removes the client and checks the registry and the close.  The fake does
   not check the Zinc WebSocket frames.

4. BlennyTransportBridge>>registerUser:stream:intents: wrote
   (aCollection asArray join: ','), which uses the Array as the separator and
   the string ',' as the list.  The log line printed an Array with one
   comma instead of the intents.  The repair writes (',' join: ...).

5. BlennyArgConfigProvider>>parseArgs stopped with an error for an argument
   that starts with -- and has no equals sign (copyFrom: 3 to: -1).  The
   class comment says that only arguments with an equals sign count.

7. (second reader) BlennyBlacklistCleanerTest>>testPerformCleanupRemovesExpiredEntries
   computed DateAndTime now - (1 * 60 * 60).  Pharo 14 has no
   Number>>subtractReceiverToDateAndTime: (only Duration, Timespan and
   DateAndTime have it), so DateAndTime>>- with an Integer ends in
   MessageNotUnderstood.  The test now subtracts a Duration.  The same form is
   used in the new body of testBlacklistCleanup.

6. BlennyEnvConfigProvider>>at: built the name BLENNY_SERVER.PORT for the key
   server.port.  The class comment (and the shell) need BLENNY_SERVER_PORT."

(Smalltalk globals includesKey: #BlennyFakeWebSocket) ifFalse: [
	(Object << #BlennyFakeWebSocket
		slots: { #sent. #closed };
		package: 'Blenny-Tests';
		yourself) install ].

add value: #BlennyFakeWebSocket value: #initialize value: 'initialization' value:
'initialize
	super initialize.
	sent := OrderedCollection new.
	closed := false'.

add value: #BlennyFakeWebSocket value: #sendMessage: value: 'writing' value:
'sendMessage: aMessage
	sent add: aMessage'.

add value: #BlennyFakeWebSocket value: #close value: 'closing' value:
'close
	closed := true'.

add value: #BlennyFakeWebSocket value: #sent value: 'accessing' value:
'sent
	^ sent'.

add value: #BlennyFakeWebSocket value: #isClosed value: 'testing' value:
'isClosed
	^ closed'.

fix value: #BlennySimpleAuthTest value: #testBlacklistCleanup
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testBlacklistCleanup
	| expired valid |
	expired := ''expired-jti''.
	valid := ''valid-jti''.
	auth repository
		saveBlacklistEntry: expired expiration: DateAndTime now - (Duration seconds: 60 * 60);
		saveBlacklistEntry: valid expiration: DateAndTime now + (Duration seconds: 60 * 60).
	self assert: (auth repository exists: ''blacklist_'' , expired).
	self assert: (auth repository exists: ''blacklist_'' , valid).

	auth cleanupBlacklist.

	self deny: (auth repository exists: ''blacklist_'' , expired).
	self assert: (auth repository exists: ''blacklist_'' , valid).
	self assert: (auth repository isBlacklisted: valid)'.

fix value: #BlennyBlacklistCleanerTest value: #testPerformCleanupRemovesExpiredEntries
	value: [ :source | source includesSubstring: 'DateAndTime now - (1 * 60 * 60)' ]
	value:
'testPerformCleanupRemovesExpiredEntries
	| repository cleaner expiredJti validJti expirationDate validExpiration |
	repository := BlennyMemoryRepository new.
	cleaner := BlennyBlacklistCleaner forRepository: repository.

	expiredJti := ''expired-jti''.
	expirationDate := DateAndTime now - (Duration seconds: 60 * 60).
	repository saveBlacklistEntry: expiredJti expiration: expirationDate.

	validJti := ''valid-jti''.
	validExpiration := DateAndTime now + (Duration seconds: 60 * 60).
	repository saveBlacklistEntry: validJti expiration: validExpiration.

	cleaner performCleanup.

	self deny: (repository isBlacklisted: expiredJti).
	self assert: (repository isBlacklisted: validJti)'.

fix value: #BlennySimpleAuthTest value: #testTokenExpiration
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testTokenExpiration
	| makeToken |
	makeToken := [ :expirationUnixTime | | claims |
		claims := JWTClaimsSet new.
		claims
			sub: ''alice'';
			jti: UUID new asString;
			exp: expirationUnixTime;
			iat: DateAndTime now asUnixTime - 100.
		(JsonWebSignature new
			algorithmName: ''HS256'';
			payload: claims;
			symmetricKey: auth jwtSecret;
			yourself) compactSerialized ].

	"A token whose exp is in the past is rejected"
	self
		assert: (auth validateToken: (makeToken value: DateAndTime now asUnixTime - 10))
		equals: nil.
	"The same kind of token with exp in the future is accepted"
	self
		assert: (auth validateToken: (makeToken value: DateAndTime now asUnixTime + 3600))
		equals: ''alice'''.

fix value: #BlennyWebSocketBridgeTest value: #testAddAndRemoveClient
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testAddAndRemoveClient
	| socket handle |
	socket := BlennyFakeWebSocket new.
	self deny: (bridge hasConnectionForUser: ''alice'').

	handle := bridge registerUser: ''alice'' stream: socket intents: #(all).
	self assert: (bridge hasConnectionForUser: ''alice'').
	self assert: (bridge connectionsForUserId: ''alice'') size equals: 1.

	bridge broadcastHtml: ''<b>hi</b>''.
	self assert: socket sent asArray equals: #(''<b>hi</b>'').
	bridge directHtml: ''<i>alice</i>'' to: ''alice''.
	self assert: socket sent size equals: 2.
	bridge directHtml: ''<i>bob</i>'' to: ''bob''.
	self assert: socket sent size equals: 2.

	handle finalize.
	self deny: (bridge hasConnectionForUser: ''alice'').
	self assert: socket isClosed'.

fix value: #BlennyTransportBridge value: #registerUser:stream:intents:
	value: [ :source | source includesSubstring: 'asArray join:' ]
	value:
'registerUser: aUserId stream: aStream intents: aCollection
    | handle userDict |
    handle := BlennyConnectionHandle new.
    handle
        userId: aUserId;
        connectionId: UUID new asString;
        stream: aStream;
        intents: aCollection asSet;
        connectionsByUserId: connectionsByUserId.
    userDict := connectionsByUserId at: aUserId ifAbsentPut: [ Dictionary new ].
    userDict at: handle connectionId put: handle.
    StringSignal emit: (''{1} CONNECTED ({2}): {3}'' format: {
        transportName.
        ('','' join: aCollection asArray).
        aUserId }).
    ^ handle'.

fix value: #BlennyArgConfigProvider value: #parseArgs
	value: [ :source | (source includesSubstring: 'includes: $=') not ]
	value:
'parseArgs
    args := Dictionary new.
    (Smalltalk commandLine arguments) do: [ :arg |
        ((arg beginsWith: ''--'') and: [ arg includes: $= ]) ifTrue: [
            | key value |
            key := (arg copyFrom: 3 to: (arg indexOf: $=) - 1).
            value := (arg copyAfter: $=).
            args at: key put: value ] ]'.

fix value: #BlennyEnvConfigProvider value: #at:
	value: [ :source | (source includesSubstring: 'copyReplaceAll') not ]
	value:
'at: aKey
    | envKey |
    envKey := ''BLENNY_'', (aKey asUppercase copyReplaceAll: ''.'' with: ''_'').
    ^ OSEnvironment current at: envKey ifAbsent: [ nil ]'.
