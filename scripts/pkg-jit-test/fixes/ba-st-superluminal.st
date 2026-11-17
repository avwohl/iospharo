"ba-st/Superluminal (row 87), Pharo 14.  203 tests; before the repair P=150 F=4 E=49 on both VMs.

Cause 1 (43 errors: 22 in HttpRequestTest, 21 in RESTfulAPIClientIntegrationTest; ConnectionClosed to 127.0.0.1:80 and HTTPServerError).  The tests need an httpbin service.  The CI of the package starts the container ghcr.io/ba-st-dependencies/httpbin and sets CI_HTTPBIN_HOSTNAME.  Without the variable, SuperluminalTest>>httpbinHostname answers localhost, so the port is 80, and nothing listens there.  The machine has no such service.  Repair: the class SuperluminalHttpbinStandIn, an httpbin stand-in made from a ZnServer on a free port of 127.0.0.1, and SuperluminalTest>>httpbinHostname answers its host and port when the variable is not set.  The test code is not changed.

Cause 2 (8 tests: 3 KeyNotFound in DistributedMemoryCacheRepositoryTest, 2 failures Got 'Obtained an updated answer' in DistributedMemoryBasedExpiringCacheTest, 3 HTTPClientError in RESTfulAPIClientCachingOnDistributedMemoryTest).  The tests need a memcached server on port 11211.  The CI starts memcached:1.6-alpine and sets CI_MEMCACHED_HOSTNAME; without the variable SuperluminalTest>>memcachedHostname answers 127.0.0.1.  No server listens, and MDCacheClient hides the error: get: answers nil and set:value:expires: answers the value, so a stored resource is lost and the next get: finds nothing.  The other tests of the distributed cache pass without a server by accident, because their expectations hold for a cache that stores nothing.  Repair: the class SuperluminalMemcachedStandIn, a memcached stand-in (text protocol) on 127.0.0.1 port 11211, and SuperluminalTest>>memcachedHostname starts it when the variable is not set.

Both stand-ins start in the default execution environment.  The environment of a test ends each process that the test forks when the test ends, so a stand-in that the first test started would be gone, with the listening socket still open, at the second test.

Cause 3 (4 tests: CompositeHttpRequestCommandTest>>testApplyOn and testShortcutCreation, and, behind the connection errors of cause 1, HttpRequestTest>>testComposite and testQueryString).  The tests compare a URL with a text whose query fields are in another order.  Two such URLs are equal in Pharo 13 and were not equal in Pharo 14.  This file has no entry for it: scripts/image-fixes/34-znurl-query-order.st repairs ZnUrl>>equals: in the image.

What the stand-ins do not check (also in the notes of the row).  httpbin: only the routes that the tests use (anything, get, post, put, patch, delete, status/N, cache, headers); no basic-auth, bearer, redirect, delay, gzip or cookie routes; the answer fields are args, data, files, form, headers, json, method, origin (always 127.0.0.1) and url; status/N answers an empty body; cache answers like httpbin (304 when If-None-Match or If-Modified-Since is present; otherwise the echo with Last-Modified and an unquoted ETag) and accepts the methods GET and QUERY, because testQueryCached sends QUERY to it and the httpbin of the CI is a fork that was not available to read.  memcached: the commands set, add, replace, get, gets, delete, incr, decr, flush_all, version, verbosity, stats (empty answer) and quit; no cas, touch, append or prepend; no binary protocol; no memory limit or eviction; no key length or item size limit; an item expires with the precision of a millisecond, where memcached counts whole seconds."


"--- The two stand-in classes.  Made once; a second file-in keeps them. ---"
(Smalltalk globals includesKey: #SuperluminalHttpbinStandIn) ifFalse: [
	(Object << #SuperluminalHttpbinStandIn)
		slots: { #server };
		sharedVariables: { #Current };
		package: 'Superluminal-Test-StandIns';
		install ].
(Smalltalk globals includesKey: #SuperluminalMemcachedStandIn) ifFalse: [
	(Object << #SuperluminalMemcachedStandIn)
		slots: { #items. #lock. #listener. #process };
		sharedVariables: { #Current };
		package: 'Superluminal-Test-StandIns';
		install ].

add value: #'SuperluminalHttpbinStandIn class' value: #current value: 'accessing' value:
'current
	"The stand-in answers on a free port of 127.0.0.1 and starts when first asked for.  It is started in the default execution environment.  The environment of a test ends every process that the test forks, when the test ends, and the stand-in must stay for the next tests."

	(Current isNil or: [ Current isRunning not ]) ifTrue: [
		DefaultExecutionEnvironment beActiveDuring: [ Current := self new start; yourself ] ].
	^ Current'.

add value: #'SuperluminalHttpbinStandIn' value: #start value: 'running' value:
'start

	| delegate |
	delegate := ZnDefaultServerDelegate empty.
	#( ''anything'' ''get'' ''post'' ''put'' ''patch'' ''delete'' ''status'' ''cache'' ''headers'' ) do: [ :prefix |
		delegate map: prefix to: [ :request | self handleRequest: request ] ].
	server := ZnServer on: 0.
	server bindingAddress: #[127 0 0 1].
	server delegate: delegate.
	server start'.

add value: #'SuperluminalHttpbinStandIn' value: #isRunning value: 'testing' value:
'isRunning

	^ server notNil and: [ server isRunning ]'.

add value: #'SuperluminalHttpbinStandIn' value: #hostnameAndPort value: 'accessing' value:
'hostnameAndPort

	^ ''localhost:'' , server port printString'.

add value: #'SuperluminalHttpbinStandIn' value: #'handleRequest:' value: 'responding' value:
'handleRequest: request

	| name |
	name := request uri firstPathSegment ifNil: [ '''' ].
	name = ''anything'' ifTrue: [ ^ self echo: request ].
	(#( ''get'' ''post'' ''put'' ''patch'' ''delete'' ) includes: name) ifTrue: [
		^ request method asString = name asUppercase
			  ifTrue: [ self echo: request ]
			  ifFalse: [ ZnResponse methodNotAllowed: request ] ].
	name = ''status'' ifTrue: [ ^ self statusResponse: request ].
	name = ''cache'' ifTrue: [ ^ self cacheResponse: request ].
	name = ''headers'' ifTrue: [
		^ self jsonResponse: (Dictionary new
				   at: ''headers'' put: (self headersOf: request);
				   yourself) ].
	^ ZnResponse notFound: request uri'.

add value: #'SuperluminalHttpbinStandIn' value: #'statusResponse:' value: 'responding' value:
'statusResponse: request

	| code |
	code := request uri segments size >= 2 ifTrue: [ request uri segments second asInteger ].
	code ifNil: [ ^ ZnResponse badRequest: request ].
	^ (code = 204 or: [ code = 304 ])
		  ifTrue: [ ZnResponse statusCode: code ]
		  ifFalse: [
			  (ZnResponse statusCode: code)
				  entity: (ZnEntity text: '''');
				  yourself ]'.

add value: #'SuperluminalHttpbinStandIn' value: #'cacheResponse:' value: 'responding' value:
'cacheResponse: request

	| response |
	(#( ''GET'' ''QUERY'' ) includes: request method asString) ifFalse: [ ^ ZnResponse methodNotAllowed: request ].
	((request headers includesKey: ''If-Modified-Since'') or: [ request headers includesKey: ''If-None-Match'' ])
		ifTrue: [ ^ ZnResponse notModified ].
	response := self echo: request.
	response headers at: ''Last-Modified'' put: (ZnUtils httpDate: DateAndTime now).
	response headers at: ''ETag'' put: (UUID new asString copyWithout: $-).
	^ response'.

add value: #'SuperluminalHttpbinStandIn' value: #'jsonResponse:' value: 'responding' value:
'jsonResponse: aDictionary

	^ ZnResponse ok: (ZnEntity json: (NeoJSONWriter toStringPretty: aDictionary))'.

add value: #'SuperluminalHttpbinStandIn' value: #'echo:' value: 'responding' value:
'echo: request

	| answer |
	answer := Dictionary new.
	answer
		at: ''args'' put: (self queryOf: request);
		at: ''form'' put: Dictionary new;
		at: ''files'' put: Dictionary new;
		at: ''data'' put: '''';
		at: ''json'' put: nil.
	request entity ifNotNil: [ :entity | self describe: entity in: answer ].
	answer
		at: ''headers'' put: (self headersOf: request);
		at: ''method'' put: request method asString;
		at: ''origin'' put: ''127.0.0.1'';
		at: ''url'' put: (self urlOf: request).
	^ self jsonResponse: answer'.

add value: #'SuperluminalHttpbinStandIn' value: #'describe:in:' value: 'private' value:
'describe: entity in: answer

	| text |
	(entity isKindOf: ZnMultiPartFormDataEntity) ifTrue: [
		entity partsDo: [ :part |
			part fileName
				ifNil: [ (answer at: ''form'') at: part fieldName put: part fieldValueString ]
				ifNotNil: [ (answer at: ''files'') at: part fieldName put: (self textOf: part entity) ] ].
		^ self ].
	(entity isKindOf: ZnApplicationFormUrlEncodedEntity) ifTrue: [
		entity fieldsDo: [ :key :value | (answer at: ''form'') at: key put: value ].
		^ self ].
	text := self textOf: entity.
	answer at: ''data'' put: text.
	((entity contentType printString includesSubstring: ''json'') and: [ text notEmpty ]) ifTrue: [
		answer at: ''json'' put: ([ NeoJSONReader fromString: text ] on: Error do: [ :error | nil ]) ]'.

add value: #'SuperluminalHttpbinStandIn' value: #'textOf:' value: 'private' value:
'textOf: entity

	| contents |
	contents := entity contents.
	^ contents isString
		  ifTrue: [ contents ]
		  ifFalse: [ contents asByteArray utf8Decoded ]'.

add value: #'SuperluminalHttpbinStandIn' value: #'headersOf:' value: 'private' value:
'headersOf: request

	| answer |
	answer := Dictionary new.
	request headers headersDo: [ :key :value |
		answer at: key put: (answer at: key ifPresent: [ :old | old , '','' , value ] ifAbsent: [ value ]) ].
	^ answer'.

add value: #'SuperluminalHttpbinStandIn' value: #'queryOf:' value: 'private' value:
'queryOf: request

	| answer |
	answer := Dictionary new.
	request uri hasQuery ifTrue: [ request uri query keysAndValuesDo: [ :key :value | answer at: key put: value ] ].
	^ answer'.

add value: #'SuperluminalHttpbinStandIn' value: #'urlOf:' value: 'private' value:
'urlOf: request

	^ ''http://'' , (request headers at: ''Host'' ifAbsent: [ ''localhost'' ]) , request uri printString'.

add value: #'SuperluminalMemcachedStandIn class' value: #port value: 'accessing' value:
'port

	^ 11211'.

add value: #'SuperluminalMemcachedStandIn class' value: #current value: 'accessing' value:
'current

	"The stand-in is started in the default execution environment.  The environment of a test ends every process that the test forks, when the test ends, and the stand-in must stay for the next tests."

	(Current isNil or: [ Current isRunning not ]) ifTrue: [
		DefaultExecutionEnvironment beActiveDuring: [ Current := self new start; yourself ] ].
	^ Current'.

add value: #'SuperluminalMemcachedStandIn class' value: #ensureRunning value: 'running' value:
'ensureRunning

	^ self current'.

add value: #'SuperluminalMemcachedStandIn' value: #start value: 'running' value:
'start

	items := Dictionary new.
	lock := Semaphore forMutualExclusion.
	listener := ZnNetworkingUtils serverSocketOn: self class port interface: #[127 0 0 1].
	process := [ self acceptLoop ] forkAt: Processor userSchedulingPriority + 1'.

add value: #'SuperluminalMemcachedStandIn' value: #isRunning value: 'testing' value:
'isRunning

	^ process notNil and: [ process isTerminated not and: [ listener isValid ] ]'.

add value: #'SuperluminalMemcachedStandIn' value: #acceptLoop value: 'running' value:
'acceptLoop

	[ [ | socket |
	  socket := listener waitForAcceptFor: 5.
	  socket ifNotNil: [ [ self serve: socket ] forkAt: Processor userSchedulingPriority + 1 ] ] repeat ]
		on: Error
		do: [ :error | nil ]'.

add value: #'SuperluminalMemcachedStandIn' value: #'serve:' value: 'running' value:
'serve: socket

	| stream |
	stream := ZnNetworkingUtils socketStreamOn: socket.
	stream timeout: 86400.
	stream binary.
	[ [ [ | line |
	  line := self readLineFrom: stream.
	  line notNil and: [ self execute: line on: stream ] ] whileTrue ] on: Error do: [ :error | nil ] ]
		ensure: [ stream close ]'.

add value: #'SuperluminalMemcachedStandIn' value: #'readLineFrom:' value: 'private' value:
'readLineFrom: stream

	| bytes byte |
	bytes := WriteStream on: ByteArray new.
	[ (byte := stream next) = 10 ] whileFalse: [ byte = 13 ifFalse: [ bytes nextPut: byte ] ].
	^ bytes contents asString'.

add value: #'SuperluminalMemcachedStandIn' value: #'reply:on:' value: 'private' value:
'reply: text on: stream

	stream nextPutAll: (text , String crlf) asByteArray.
	stream flush'.

add value: #'SuperluminalMemcachedStandIn' value: #'execute:on:' value: 'private' value:
'execute: line on: stream
	"Answer true to go on with the connection."

	| words command |
	words := line substrings.
	words isEmpty ifTrue: [ self reply: ''ERROR'' on: stream. ^ true ].
	command := words first.
	(#( ''set'' ''add'' ''replace'' ) includes: command) ifTrue: [ ^ self store: words on: stream ].
	(#( ''get'' ''gets'' ) includes: command) ifTrue: [
		words allButFirst do: [ :key | self sendItemAt: key on: stream ].
		self reply: ''END'' on: stream.
		^ true ].
	command = ''delete'' ifTrue: [ ^ self delete: words on: stream ].
	(#( ''incr'' ''decr'' ) includes: command) ifTrue: [ ^ self change: words on: stream ].
	command = ''flush_all'' ifTrue: [
		lock critical: [ items removeAll ].
		self reply: ''OK'' on: stream.
		^ true ].
	command = ''version'' ifTrue: [ self reply: ''VERSION 1.6.0'' on: stream. ^ true ].
	command = ''verbosity'' ifTrue: [ self reply: ''OK'' on: stream. ^ true ].
	command = ''stats'' ifTrue: [ self reply: ''END'' on: stream. ^ true ].
	command = ''quit'' ifTrue: [ ^ false ].
	self reply: ''ERROR'' on: stream.
	^ true'.

add value: #'SuperluminalMemcachedStandIn' value: #'store:on:' value: 'private' value:
'store: words on: stream

	| data stored |
	data := stream next: (words at: 5) asInteger.
	stream next: 2.
	stored := lock critical: [
		          self
			          apply: words first
			          key: (words at: 2)
			          flags: (words at: 3) asInteger
			          expiration: (words at: 4) asInteger
			          data: data ].
	self reply: (stored ifTrue: [ ''STORED'' ] ifFalse: [ ''NOT_STORED'' ]) on: stream.
	^ true'.

add value: #'SuperluminalMemcachedStandIn' value: #'apply:key:flags:expiration:data:' value: 'private' value:
'apply: command key: key flags: flags expiration: seconds data: data

	| present |
	present := self liveItemAt: key.
	(command = ''add'' and: [ present notNil ]) ifTrue: [ ^ false ].
	(command = ''replace'' and: [ present isNil ]) ifTrue: [ ^ false ].
	items at: key put: (Array with: flags with: data with: (self expiryFor: seconds)).
	^ true'.

add value: #'SuperluminalMemcachedStandIn' value: #'expiryFor:' value: 'private' value:
'expiryFor: seconds
	"The time on the millisecond clock at which the item expires, or nil for never.  As memcached does: up to 30 days is an offset, more is a Unix time, below zero is already expired."

	| now |
	seconds = 0 ifTrue: [ ^ nil ].
	now := Time millisecondClockValue.
	seconds < 0 ifTrue: [ ^ now ].
	seconds <= 2592000 ifTrue: [ ^ now + (seconds * 1000) ].
	^ now + ((seconds - DateAndTime now asUnixTime) * 1000)'.

add value: #'SuperluminalMemcachedStandIn' value: #'liveItemAt:' value: 'private' value:
'liveItemAt: key
	"Call it inside the lock."

	| item |
	item := items at: key ifAbsent: [ ^ nil ].
	(item third notNil and: [ item third <= Time millisecondClockValue ]) ifTrue: [
		items removeKey: key.
		^ nil ].
	^ item'.

add value: #'SuperluminalMemcachedStandIn' value: #'sendItemAt:on:' value: 'private' value:
'sendItemAt: key on: stream

	| item |
	item := lock critical: [ self liveItemAt: key ].
	item ifNil: [ ^ self ].
	stream nextPutAll:
		(''VALUE '' , key , '' '' , item first printString , '' '' , item second size printString , String crlf) asByteArray.
	stream nextPutAll: item second.
	stream nextPutAll: String crlf asByteArray.
	stream flush'.

add value: #'SuperluminalMemcachedStandIn' value: #'delete:on:' value: 'private' value:
'delete: words on: stream

	| removed |
	removed := lock critical: [
		           (self liveItemAt: (words at: 2)) notNil and: [
			           items removeKey: (words at: 2).
			           true ] ].
	self reply: (removed ifTrue: [ ''DELETED'' ] ifFalse: [ ''NOT_FOUND'' ]) on: stream.
	^ true'.

add value: #'SuperluminalMemcachedStandIn' value: #'change:on:' value: 'private' value:
'change: words on: stream

	| value |
	lock critical: [
		(self liveItemAt: (words at: 2)) ifNotNil: [ :item |
			value := item second asString asInteger + ((words at: 3) asInteger * (words first = ''incr'' ifTrue: [ 1 ] ifFalse: [ -1 ])).
			value := value max: 0.
			items at: (words at: 2) put: (Array with: item first with: value printString asByteArray with: item third) ] ].
	self reply: (value ifNil: [ ''NOT_FOUND'' ] ifFalse: [ value printString ]) on: stream.
	^ true'.

"--- Where the tests find the stand-ins ---"
fix value: #SuperluminalTest value: #httpbinHostname
	value: [ :source | (source includesSubstring: 'StandIn') not ]
	value:
'httpbinHostname

	^ LanguagePlatform current os
		  environmentAt: ''CI_HTTPBIN_HOSTNAME''
		  ifPresent: [ :hostnameInVariable | hostnameInVariable ]
		  ifAbsent: [ SuperluminalHttpbinStandIn current hostnameAndPort ]'.

fix value: #SuperluminalTest value: #memcachedHostname
	value: [ :source | (source includesSubstring: 'StandIn') not ]
	value:
'memcachedHostname

	^ LanguagePlatform current os
		  environmentAt: ''CI_MEMCACHED_HOSTNAME''
		  ifPresent: [ :hostnameInVariable | hostnameInVariable ]
		  ifAbsent: [
			  SuperluminalMemcachedStandIn ensureRunning.
			  ''127.0.0.1'' ]'.
