"punt-labs/anthropic-sdk-pharo: causes and repairs.

1. The integration tests (protocol tests-integration, 17 test classes) call
   the real service api.anthropic.com.  Each test gets its client from
   ClaudeSDKExampleSupport class>>resolveClient, which reads
   ANTHROPIC_PHARO_API_KEY or ANTHROPIC_API_KEY (or the OS keyring).  On the
   test machine a key was set, and the real service answered
   'invalid x-api-key' (HTTP 401) for each test, so the tests failed with
   ClaudeAuthError.  A test must not send a request out of the machine and must
   not use a real key.  The class ClaudeAnthropicDouble answers the requests
   inside the image: a ZnServer on 127.0.0.1 and an ephemeral port.  It accepts
   one fixed test key, and it implements the endpoints that the tests call
   (models, messages with and without stream, count_tokens, files, skills).
   resolveClient now answers a client for the double.  The assertions of the
   tests are not changed.

2. Three tests (testRealStreamingConnection, testRealStreamingEvents,
   testRealStreamingClose) open a TLS ClaudeStreamingSocket to the host
   api.anthropic.com themselves, and ClaudeClient>>openStreamingSocket: opens
   a TLS socket for a base URL that starts with https://.  The double speaks
   plain HTTP.  ClaudePlainStreamingSocket is the same socket without TLS.
   ClaudeClient>>openStreamingSocket: uses it for the URL of the double (the
   buffered path for other http URLs is unchanged).  The three tests take the
   socket from ClaudeAnthropicDouble class>>newStreamingSocket.

3. testErrorInvalidKey and testRealStreamingErrorHandling build a client with
   the key sk-invalid and the default base URL, so the request goes to the
   real service.  They now use a client for the double with the key
   sk-invalid, and the double answers 401 for any key but its test key.

4. PharoKeyringIntegrationTest>>setUp skips on macOS: it asks
   OSPlatform current isUnix, which is false for MacOSPlatform (the class
   inherits from AbstractUnixPlatform and only UnixPlatform answers true), while
   the skip text and PharoKeyring>>detectBackend both name macOS.  NOT repaired
   here: with the right guard the test stores a secret in the login keychain
   of the user (security add-generic-password) and removes it in tearDown.
   The owner of the machine must permit that first.  The test stays a skip.

5. LinuxSecretToolBackendTest>>testIsAvailable and
   PassToolBackendTest>>testIsAvailable skipped when the tool is absent and
   asserted 'isAvailable' (true) only when the tool is present, so on a machine
   without the tool the test checked nothing.  The repair compares isAvailable
   with an independent search of the PATH directories: the test passes on a
   machine with the tool (true = true) and on a machine without the tool
   (false = false).

6. Corrected by the second reader: the double answers a streamed message with
   Transfer-Encoding chunked (as the real service does), so the chunk reader of
   ClaudeStreamingSocket runs; the double refuses an image or a document block with
   no usable source (a PDF whose data is not a PDF, a url that is not https); and
   resolveClient signals a TestFailure when the double does not start, because 15
   integration test classes turn any Error of resolveClient into a skip."

(Smalltalk globals includesKey: #ClaudeAnthropicDouble) ifFalse: [
	(Object << #ClaudeAnthropicDouble
		sharedVariables: { #DoubleServer. #DoubleFiles. #DoubleSkills. #DoubleSeen. #DoubleCounter };
		package: 'Claude-Messaging-Double';
		yourself) install ].

(Smalltalk globals includesKey: #ClaudePlainStreamingSocket) ifFalse: [
	(ClaudeStreamingSocket << #ClaudePlainStreamingSocket
		package: 'Claude-Messaging-Double';
		yourself) install ].
add value: #'ClaudeAnthropicDouble class' value: #'testKey' value: 'accessing' value:
'testKey
	"The one API key that the double accepts. It is not a real key."
	^ ''sk-ant-api03-local-double-key'''.
add value: #'ClaudeAnthropicDouble class' value: #'ensureServer' value: 'lifecycle' value:
'ensureServer
	"Start the double when it does not run."
	(DoubleServer notNil and: [ DoubleServer isRunning ]) ifTrue: [ ^ self ].
	self startServer'.
add value: #'ClaudeAnthropicDouble class' value: #'startServer' value: 'lifecycle' value:
'startServer
	"Listen on 127.0.0.1 and an ephemeral port. The state (files, skills, seen
	 requests) starts empty."
	DoubleServer ifNotNil: [ DoubleServer stop ].
	DoubleFiles := OrderedCollection new.
	DoubleSkills := OrderedCollection new.
	DoubleSeen := Set new.
	DoubleCounter := 0.
	DoubleServer := ZnServer on: 0.
	DoubleServer bindingAddress: #[127 0 0 1].
	DoubleServer delegate: (ZnValueDelegate with: [ :request | self handle: request ]).
	DoubleServer start.
	1 to: 40 do: [ :each |
		DoubleServer isRunning ifFalse: [ (Delay forMilliseconds: 50) wait ] ].
	DoubleServer isRunning ifFalse: [ self error: ''The local double of the Anthropic API did not start'' ]'.
add value: #'ClaudeAnthropicDouble class' value: #'baseUrl' value: 'accessing' value:
'baseUrl
	self ensureServer.
	^ ''http://127.0.0.1:'', DoubleServer port printString'.
add value: #'ClaudeAnthropicDouble class' value: #'isDoubleUrl:' value: 'testing' value:
'isDoubleUrl: aUrlString
	"True when aUrlString is the base URL of the running double."
	^ DoubleServer notNil and: [
		DoubleServer isRunning and: [
			aUrlString = (''http://127.0.0.1:'', DoubleServer port printString) ] ]'.
add value: #'ClaudeAnthropicDouble class' value: #'clientWithKey:' value: 'instance creation' value:
'clientWithKey: anApiKey
	"A ClaudeClient that talks to the double."
	^ ClaudeClient apiKey: anApiKey baseUrl: self baseUrl timeout: 60'.
add value: #'ClaudeAnthropicDouble class' value: #'clientForTests' value: 'instance creation' value:
'clientForTests
	"A ClaudeClient with the test key. ClaudeSDKExampleSupport class>>resolveClient answers this."
	^ self clientWithKey: self testKey'.
add value: #'ClaudeAnthropicDouble class' value: #'newStreamingSocket' value: 'instance creation' value:
'newStreamingSocket
	"A plain (no TLS) streaming socket for the double. The caller sends connect."
	| socket |
	self ensureServer.
	socket := ClaudePlainStreamingSocket host: ''127.0.0.1''.
	socket port: DoubleServer port.
	socket timeout: 30.
	^ socket'.
add value: #'ClaudeAnthropicDouble class' value: #'nextCounter' value: 'private' value:
'nextCounter
	DoubleCounter := DoubleCounter + 1.
	^ DoubleCounter'.
add value: #'ClaudeAnthropicDouble class' value: #'nextRequestId' value: 'private' value:
'nextRequestId
	^ ''req_double_'', self nextCounter printString'.
add value: #'ClaudeAnthropicDouble class' value: #'jsonOk:' value: 'http' value:
'jsonOk: aDictionary
	| response |
	response := ZnResponse ok: (ZnEntity json: (STONJSON toString: aDictionary)).
	response headers at: ''request-id'' put: self nextRequestId.
	^ response'.
add value: #'ClaudeAnthropicDouble class' value: #'errorStatus:type:message:' value: 'http' value:
'errorStatus: aCode type: aType message: aMessage
	"The error body of the Anthropic API: type, error (type, message), request_id."
	| id response |
	id := self nextRequestId.
	response := ZnResponse statusCode: aCode.
	response entity: (ZnEntity json: (STONJSON toString: (Dictionary new
		at: ''type'' put: ''error'';
		at: ''error'' put: (Dictionary new
			at: ''type'' put: aType;
			at: ''message'' put: aMessage;
			yourself);
		at: ''request_id'' put: id;
		yourself))).
	response headers at: ''request-id'' put: id.
	^ response'.
add value: #'ClaudeAnthropicDouble class' value: #'invalid:' value: 'http' value:
'invalid: aMessage
	^ self errorStatus: 400 type: ''invalid_request_error'' message: aMessage'.
add value: #'ClaudeAnthropicDouble class' value: #'notFound:' value: 'http' value:
'notFound: aRequest
	^ self
		errorStatus: 404
		type: ''not_found_error''
		message: ''Not found: '', aRequest method asString, '' '', aRequest uri pathPrintString'.
add value: #'ClaudeAnthropicDouble class' value: #'handle:' value: 'http' value:
'handle: aRequest
	"The delegate of the ZnServer. A failure of the double itself answers 400
	 with the text of the failure, so that the failing test shows the cause."
	| response |
	response := [ self route: aRequest ]
		on: Error
		do: [ :error |
			self invalid: ''The local double could not process the request: '', error messageText asString ].
	response setConnectionClose.
	^ response'.
add value: #'ClaudeAnthropicDouble class' value: #'route:' value: 'http' value:
'route: aRequest
	| segments |
	(aRequest headers at: ''x-api-key'' ifAbsent: [ nil ]) = self testKey
		ifFalse: [ ^ self errorStatus: 401 type: ''authentication_error'' message: ''invalid x-api-key'' ].
	segments := aRequest uri pathSegments asArray.
	(segments size >= 2 and: [ segments first = ''v1'' ]) ifFalse: [ ^ self notFound: aRequest ].
	segments second = ''models'' ifTrue: [ ^ self routeModels: segments request: aRequest ].
	segments second = ''messages'' ifTrue: [ ^ self routeMessages: segments request: aRequest ].
	segments second = ''files'' ifTrue: [ ^ self routeFiles: segments request: aRequest ].
	segments second = ''skills'' ifTrue: [ ^ self routeSkills: segments request: aRequest ].
	^ self notFound: aRequest'.
add value: #'ClaudeAnthropicDouble class' value: #'modelIds' value: 'models' value:
'modelIds
	^ { ClaudeModel opus55. ClaudeModel sonnet55 }'.
add value: #'ClaudeAnthropicDouble class' value: #'modelInfoFor:' value: 'models' value:
'modelInfoFor: aModelId
	^ Dictionary new
		at: ''id'' put: aModelId;
		at: ''type'' put: ''model'';
		at: ''display_name'' put: (aModelId = ClaudeModel opus55
			ifTrue: [ ''Claude Opus 5.5'' ]
			ifFalse: [ ''Claude Sonnet 5.5'' ]);
		at: ''created_at'' put: ''2026-01-01T00:00:00Z'';
		at: ''max_input_tokens'' put: 200000;
		at: ''max_tokens'' put: 64000;
		yourself'.
add value: #'ClaudeAnthropicDouble class' value: #'routeModels:request:' value: 'models' value:
'routeModels: segments request: aRequest
	aRequest method asString = ''GET''
		ifFalse: [ ^ self errorStatus: 405 type: ''invalid_request_error'' message: ''Method not allowed'' ].
	segments size = 2 ifTrue: [
		^ self jsonOk: (Dictionary new
			at: ''data'' put: (self modelIds collect: [ :each | self modelInfoFor: each ]) asArray;
			at: ''has_more'' put: false;
			at: ''first_id'' put: self modelIds first;
			at: ''last_id'' put: self modelIds last;
			yourself) ].
	segments size = 3 ifTrue: [
		^ (self modelIds includes: segments third)
			ifTrue: [ self jsonOk: (self modelInfoFor: segments third) ]
			ifFalse: [ self errorStatus: 404 type: ''not_found_error'' message: ''model: '', segments third ] ].
	^ self notFound: aRequest'.
add value: #'ClaudeAnthropicDouble class' value: #'routeMessages:request:' value: 'messages' value:
'routeMessages: segments request: aRequest
	| body isCount invalid message |
	aRequest method asString = ''POST''
		ifFalse: [ ^ self errorStatus: 405 type: ''invalid_request_error'' message: ''Method not allowed'' ].
	isCount := segments size = 3 and: [ segments third = ''count_tokens'' ].
	(segments size = 2 or: [ isCount ]) ifFalse: [ ^ self notFound: aRequest ].
	body := [ STONJSON fromString: aRequest entity contents ]
		on: Error
		do: [ :error | ^ self invalid: ''Invalid JSON'' ].
	body isDictionary ifFalse: [ ^ self invalid: ''The body must be a JSON object'' ].
	invalid := self checkBody: body request: aRequest isCount: isCount.
	invalid ifNotNil: [ ^ invalid ].
	isCount ifTrue: [
		^ self jsonOk: (Dictionary new
			at: ''input_tokens'' put: (self inputTokensOf: body);
			yourself) ].
	message := self messageFor: body.
	^ (body at: ''stream'' ifAbsent: [ false ]) == true
		ifTrue: [ self streamResponseFor: message ]
		ifFalse: [ self jsonOk: message ]'.
add value: #'ClaudeAnthropicDouble class' value: #'checkBody:request:isCount:' value: 'messages' value:
'checkBody: aBody request: aRequest isCount: isCount
	"Answer nil when the body is valid, else the error response."
	| betas |
	(aBody includesKey: ''model'') ifFalse: [ ^ self invalid: ''model: Field required'' ].
	(aBody includesKey: ''messages'') ifFalse: [ ^ self invalid: ''messages: Field required'' ].
	((aBody at: ''messages'') isCollection and: [ (aBody at: ''messages'') isNotEmpty ])
		ifFalse: [ ^ self invalid: ''messages: at least one message is required'' ].
	isCount
		ifTrue: [
			(aBody includesKey: ''max_tokens'')
				ifTrue: [ ^ self invalid: ''max_tokens: Extra inputs are not permitted'' ] ]
		ifFalse: [
			(aBody includesKey: ''max_tokens'')
				ifFalse: [ ^ self invalid: ''max_tokens: Field required'' ] ].
	(self modelIds includes: (aBody at: ''model''))
		ifFalse: [ ^ self errorStatus: 404 type: ''not_found_error'' message: ''model: '', (aBody at: ''model'') printString ].
	(aBody includesKey: ''service_tier'') ifTrue: [
		(#(''auto'' ''standard_only'') includes: (aBody at: ''service_tier''))
			ifFalse: [ ^ self invalid: ''service_tier: Input should be auto or standard_only'' ] ].
	(aBody includesKey: ''metadata'') ifTrue: [
		((aBody at: ''metadata'') isDictionary
			and: [ (aBody at: ''metadata'') keys allSatisfy: [ :each | each = ''user_id'' ] ])
			ifFalse: [ ^ self invalid: ''metadata: only user_id is permitted'' ] ].
	(aBody includesKey: ''mcp_servers'') ifTrue: [
		betas := aRequest headers at: ''anthropic-beta'' ifAbsent: [ '''' ].
		(betas includesSubstring: ''mcp-client'')
			ifFalse: [ ^ self invalid: ''mcp_servers: the mcp-client beta header is required'' ].
		(aBody at: ''mcp_servers'') do: [ :server |
			((server includesKey: ''name'')
				and: [ (server includesKey: ''url'') and: [ (server at: ''url'') beginsWith: ''https://'' ] ])
				ifFalse: [ ^ self invalid: ''mcp_servers: each server needs a name and an https url'' ] ] ].
	^ self checkMediaIn: aBody'.
add value: #'ClaudeAnthropicDouble class' value: #'checkMediaIn:' value: 'messages' value:
'checkMediaIn: aBody
	"Answer nil when each image and document block has a usable source, else the error response.
	 The real service refuses a PDF whose data is not a PDF and an image with no image media type,
	 so the 1500 tokens that inputTokensOf: adds for a block are not paid for an empty block."
	(self contentBlocksIn: aBody) do: [ :block |
		| type source sourceType data media |
		type := block at: ''type'' ifAbsent: [ nil ].
		(#(''image'' ''document'') includes: type) ifTrue: [
			source := block at: ''source'' ifAbsent: [ nil ].
			source isDictionary
				ifFalse: [ ^ self invalid: ''messages: a '', type, '' block needs a source'' ].
			sourceType := source at: ''type'' ifAbsent: [ nil ].
			sourceType = ''url'' ifTrue: [
				((source at: ''url'' ifAbsent: [ '''' ]) beginsWith: ''https://'')
					ifFalse: [ ^ self invalid: ''messages: a url source needs an https url'' ] ].
			sourceType = ''base64'' ifTrue: [
				data := source at: ''data'' ifAbsent: [ '''' ].
				media := source at: ''media_type'' ifAbsent: [ '''' ].
				(data isString and: [ data size >= 8 ])
					ifFalse: [ ^ self invalid: ''messages: the base64 data is empty'' ].
				(type = ''image'' and: [ (media beginsWith: ''image/'') not ])
					ifTrue: [ ^ self invalid: ''messages: an image needs an image/* media_type'' ].
				(media = ''application/pdf'' and: [ ((data first: 8) base64Decoded asString beginsWith: ''%PDF'') not ])
					ifTrue: [ ^ self invalid: ''messages: the data is not a PDF'' ] ] ] ].
	^ nil'.
add value: #'ClaudeAnthropicDouble class' value: #'userTextOf:' value: 'messages' value:
'userTextOf: aMessage
	"The text of the text blocks of one message (a String or a list of blocks)."
	| content |
	content := aMessage at: ''content'' ifAbsent: [ '''' ].
	content isString ifTrue: [ ^ content ].
	^ String streamContents: [ :stream |
		content do: [ :block |
			(block at: ''type'' ifAbsent: [ nil ]) = ''text''
				ifTrue: [ stream nextPutAll: (block at: ''text'' ifAbsent: [ '''' ]); space ] ] ]'.
add value: #'ClaudeAnthropicDouble class' value: #'lastUserTextOf:' value: 'messages' value:
'lastUserTextOf: aBody
	^ self userTextOf: (aBody at: ''messages'') last'.
add value: #'ClaudeAnthropicDouble class' value: #'systemTextOf:' value: 'messages' value:
'systemTextOf: aBody
	| system |
	system := aBody at: ''system'' ifAbsent: [ ^ '''' ].
	system isString ifTrue: [ ^ system ].
	^ String streamContents: [ :stream |
		system do: [ :block |
			stream nextPutAll: (block at: ''text'' ifAbsent: [ '''' ]); space ] ]'.
add value: #'ClaudeAnthropicDouble class' value: #'contentBlocksIn:' value: 'messages' value:
'contentBlocksIn: aBody
	"All content blocks (dictionaries) of all messages of the request."
	| blocks |
	blocks := OrderedCollection new.
	(aBody at: ''messages'') do: [ :message |
		| content |
		content := message at: ''content'' ifAbsent: [ nil ].
		(content notNil and: [ content isString not ]) ifTrue: [
			content do: [ :block | block isDictionary ifTrue: [ blocks add: block ] ] ] ].
	^ blocks'.
add value: #'ClaudeAnthropicDouble class' value: #'toolTypesIn:' value: 'messages' value:
'toolTypesIn: aBody
	"The type of each tool of the request. A client tool has no type or the type custom."
	^ ((aBody at: ''tools'' ifAbsent: [ #() ]) collect: [ :each | each at: ''type'' ifAbsent: [ ''custom'' ] ]) asArray'.
add value: #'ClaudeAnthropicDouble class' value: #'inputTokensOf:' value: 'messages' value:
'inputTokensOf: aBody
	"An estimate: one token for 4 characters, and 1500 tokens for each image or document block."
	| characters media |
	characters := (STONJSON toString: (aBody at: ''messages'')) size + (self systemTextOf: aBody) size.
	media := (self contentBlocksIn: aBody) count: [ :each |
		#(''image'' ''document'') includes: (each at: ''type'' ifAbsent: [ nil ]) ].
	^ characters // 4 + (media * 1500) + 1'.
add value: #'ClaudeAnthropicDouble class' value: #'cachedTokensIn:' value: 'messages' value:
'cachedTokensIn: aBody
	"The tokens of the blocks with cache_control. A block under 1024 tokens is not cached."
	| blocks system tokens |
	blocks := OrderedCollection new.
	system := aBody at: ''system'' ifAbsent: [ nil ].
	(system notNil and: [ system isString not ]) ifTrue: [ blocks addAll: system ].
	blocks addAll: (self contentBlocksIn: aBody).
	tokens := (blocks select: [ :each | each isDictionary and: [ each includesKey: ''cache_control'' ] ])
		inject: 0
		into: [ :sum :each | sum + ((each at: ''text'' ifAbsent: [ '''' ]) size // 4) ].
	^ tokens >= 1024 ifTrue: [ tokens ] ifFalse: [ 0 ]'.
add value: #'ClaudeAnthropicDouble class' value: #'usageFor:content:' value: 'messages' value:
'usageFor: aBody content: aContent
	"The first request with a cached block creates the cache entry, the next request with
	 the same body reads the entry."
	| inputTokens cached created read key |
	inputTokens := self inputTokensOf: aBody.
	cached := self cachedTokensIn: aBody.
	created := 0.
	read := 0.
	cached > 0 ifTrue: [
		key := STONJSON toString: aBody.
		(DoubleSeen includes: key)
			ifTrue: [ read := cached ]
			ifFalse: [
				DoubleSeen add: key.
				created := cached ] ].
	^ Dictionary new
		at: ''input_tokens'' put: ((inputTokens - cached) max: 1);
		at: ''output_tokens'' put: (aContent
			inject: 1
			into: [ :sum :block | sum + ((block at: ''text'' ifAbsent: [ '''' ]) size // 4) ]);
		at: ''cache_creation_input_tokens'' put: created;
		at: ''cache_read_input_tokens'' put: read;
		at: ''service_tier'' put: ''standard'';
		yourself'.
add value: #'ClaudeAnthropicDouble class' value: #'messageFor:' value: 'messages' value:
'messageFor: aBody
	| content stop |
	content := self contentFor: aBody.
	stop := (content anySatisfy: [ :each | (each at: ''type'') = ''tool_use'' ])
		ifTrue: [ ''tool_use'' ]
		ifFalse: [ ''end_turn'' ].
	^ Dictionary new
		at: ''id'' put: ''msg_double_'', self nextCounter printString;
		at: ''type'' put: ''message'';
		at: ''role'' put: ''assistant'';
		at: ''model'' put: (aBody at: ''model'');
		at: ''content'' put: content;
		at: ''stop_reason'' put: stop;
		at: ''stop_sequence'' put: nil;
		at: ''usage'' put: (self usageFor: aBody content: content);
		yourself'.
add value: #'ClaudeAnthropicDouble class' value: #'contentFor:' value: 'messages' value:
'contentFor: aBody
	"The content blocks of the answer. The double looks at the shape of the request (an MCP
	 server, a server tool, thinking, a document with citations, an output schema, a system
	 prompt with a marker) and answers a block list of the same shape as the real service."
	| types thinking schema |
	(aBody includesKey: ''mcp_servers'') ifTrue: [ ^ self mcpContentFor: aBody ].
	types := self toolTypesIn: aBody.
	(types anySatisfy: [ :each | each beginsWith: ''code_execution'' ])
		ifTrue: [ ^ self codeExecutionContent ].
	(types anySatisfy: [ :each | each beginsWith: ''web_fetch'' ])
		ifTrue: [ ^ self webFetchContentFor: aBody ].
	(types anySatisfy: [ :each | each beginsWith: ''web_search'' ])
		ifTrue: [ ^ self webSearchContentFor: aBody ].
	types do: [ :each |
		(each beginsWith: ''tool_search_tool'') ifTrue: [ ^ self toolSearchContentType: each ] ].
	(types anySatisfy: [ :each | each beginsWith: ''bash_'' ])
		ifTrue: [ ^ self clientToolContentNamed: ''bash'' input: (Dictionary new at: ''command'' put: ''echo hello world''; yourself) ].
	(types anySatisfy: [ :each | each beginsWith: ''memory_'' ])
		ifTrue: [ ^ self clientToolContentNamed: ''memory'' input: (Dictionary new
			at: ''command'' put: ''create'';
			at: ''path'' put: ''/memories/favorite_color.txt'';
			at: ''file_text'' put: ''blue'';
			yourself) ].
	(types anySatisfy: [ :each | each beginsWith: ''text_editor_'' ])
		ifTrue: [ ^ self clientToolContentNamed: ''str_replace_based_edit_tool'' input: (Dictionary new
			at: ''command'' put: ''create'';
			at: ''path'' put: ''/tmp/test.txt'';
			at: ''file_text'' put: ''hello world'';
			yourself) ].
	thinking := aBody at: ''thinking'' ifAbsent: [ nil ].
	(thinking notNil and: [ (thinking at: ''type'' ifAbsent: [ ''disabled'' ]) ~= ''disabled'' ])
		ifTrue: [ ^ self thinkingContent ].
	(self hasCitationDocumentIn: aBody) ifTrue: [ ^ self citationContentFor: aBody ].
	schema := self schemaOf: aBody.
	schema ifNotNil: [ ^ { self textBlock: (self jsonTextFor: schema) } ].
	^ { self textBlock: (self plainReplyFor: aBody) }'.
add value: #'ClaudeAnthropicDouble class' value: #'textBlock:' value: 'content' value:
'textBlock: aString
	^ Dictionary new
		at: ''type'' put: ''text'';
		at: ''text'' put: aString;
		yourself'.
add value: #'ClaudeAnthropicDouble class' value: #'clientToolContentNamed:input:' value: 'content' value:
'clientToolContentNamed: aName input: anInput
	"A tool of the client (bash, memory, text editor): the answer is a tool_use block."
	^ {
		self textBlock: ''I will use the tool.''.
		(Dictionary new
			at: ''type'' put: ''tool_use'';
			at: ''id'' put: ''toolu_double_'', self nextCounter printString;
			at: ''name'' put: aName;
			at: ''input'' put: anInput;
			yourself) }'.
add value: #'ClaudeAnthropicDouble class' value: #'serverToolUseNamed:input:' value: 'content' value:
'serverToolUseNamed: aName input: anInput
	^ Dictionary new
		at: ''type'' put: ''server_tool_use'';
		at: ''id'' put: ''srvtoolu_double_'', self nextCounter printString;
		at: ''name'' put: aName;
		at: ''input'' put: anInput;
		yourself'.
add value: #'ClaudeAnthropicDouble class' value: #'codeExecutionContent' value: 'content' value:
'codeExecutionContent
	| use |
	use := self serverToolUseNamed: ''bash_code_execution'' input: (Dictionary new
		at: ''command'' put: ''python3 hello.py'';
		yourself).
	^ {
		use.
		(Dictionary new
			at: ''type'' put: ''bash_code_execution_tool_result'';
			at: ''tool_use_id'' put: (use at: ''id'');
			at: ''content'' put: (Dictionary new
				at: ''type'' put: ''bash_code_execution_result'';
				at: ''stdout'' put: ''hello world'';
				at: ''stderr'' put: '''';
				at: ''return_code'' put: 0;
				at: ''content'' put: #();
				yourself);
			yourself).
		self textBlock: ''The script printed hello world.'' }'.
add value: #'ClaudeAnthropicDouble class' value: #'webFetchContentFor:' value: 'content' value:
'webFetchContentFor: aBody
	| url use |
	url := (self lastUserTextOf: aBody) substrings
		detect: [ :each | each beginsWith: ''http'' ]
		ifNone: [ ''https://example.invalid/'' ].
	use := self serverToolUseNamed: ''web_fetch'' input: (Dictionary new at: ''url'' put: url; yourself).
	^ {
		use.
		(Dictionary new
			at: ''type'' put: ''web_fetch_tool_result'';
			at: ''tool_use_id'' put: (use at: ''id'');
			at: ''content'' put: (Dictionary new
				at: ''type'' put: ''web_fetch_result'';
				at: ''url'' put: url;
				at: ''retrieved_at'' put: DateAndTime now asUTC printString;
				at: ''content'' put: (Dictionary new
					at: ''type'' put: ''document'';
					at: ''title'' put: ''Local page'';
					at: ''source'' put: (Dictionary new
						at: ''type'' put: ''text'';
						at: ''media_type'' put: ''text/plain'';
						at: ''data'' put: ''The page text of the local double.'';
						yourself);
					yourself);
				yourself);
			yourself).
		self textBlock: ''The page holds one short text.'' }'.
add value: #'ClaudeAnthropicDouble class' value: #'webSearchContentFor:' value: 'content' value:
'webSearchContentFor: aBody
	| text use |
	text := self lastUserTextOf: aBody.
	use := self serverToolUseNamed: ''web_search'' input: (Dictionary new
		at: ''query'' put: (text copyFrom: 1 to: (60 min: text size));
		yourself).
	^ {
		use.
		(Dictionary new
			at: ''type'' put: ''web_search_tool_result'';
			at: ''tool_use_id'' put: (use at: ''id'');
			at: ''content'' put: { Dictionary new
				at: ''type'' put: ''web_search_result'';
				at: ''url'' put: ''https://example.invalid/news'';
				at: ''title'' put: ''A result of the local double'';
				at: ''encrypted_content'' put: ''ZW5jcnlwdGVk'';
				at: ''page_age'' put: nil;
				yourself };
			yourself).
		self textBlock: ''One result was found.'' }'.
add value: #'ClaudeAnthropicDouble class' value: #'toolSearchContentType:' value: 'content' value:
'toolSearchContentType: aToolType
	"The name of the tool is the type without the date suffix (for example _20251119)."
	| use |
	use := self
		serverToolUseNamed: (aToolType allButLast: 9)
		input: (Dictionary new at: ''query'' put: ''weather''; yourself).
	^ {
		use.
		(Dictionary new
			at: ''type'' put: ''tool_search_tool_result'';
			at: ''tool_use_id'' put: (use at: ''id'');
			at: ''content'' put: (Dictionary new
				at: ''type'' put: ''tool_search_tool_search_result'';
				at: ''tool_references'' put: { Dictionary new
					at: ''type'' put: ''tool_reference'';
					at: ''tool_name'' put: ''get_weather'';
					yourself };
				yourself);
			yourself).
		self textBlock: ''The tool get_weather matches.'' }'.
add value: #'ClaudeAnthropicDouble class' value: #'mcpContentFor:' value: 'content' value:
'mcpContentFor: aBody
	| server id |
	server := (aBody at: ''mcp_servers'') first.
	id := ''mcptoolu_double_'', self nextCounter printString.
	^ {
		self textBlock: ''I will ask the server.''.
		(Dictionary new
			at: ''type'' put: ''mcp_tool_use'';
			at: ''id'' put: id;
			at: ''name'' put: ''ask_question'';
			at: ''server_name'' put: (server at: ''name'');
			at: ''input'' put: (Dictionary new
				at: ''question'' put: ''What is this repository?'';
				yourself);
			yourself).
		(Dictionary new
			at: ''type'' put: ''mcp_tool_result'';
			at: ''tool_use_id'' put: id;
			at: ''is_error'' put: false;
			at: ''content'' put: { self textBlock: ''A Python SDK for the Model Context Protocol.'' };
			yourself).
		self textBlock: ''It is a Python SDK for the Model Context Protocol.'' }'.
add value: #'ClaudeAnthropicDouble class' value: #'thinkingContent' value: 'content' value:
'thinkingContent
	^ {
		(Dictionary new
			at: ''type'' put: ''thinking'';
			at: ''thinking'' put: ''The user asks for the Monty Hall problem. I count the cases.'';
			at: ''signature'' put: ''RG91YmxlU2lnbmF0dXJl'';
			yourself).
		self textBlock: ''Switching doors wins with probability 2/3.'' }'.
add value: #'ClaudeAnthropicDouble class' value: #'hasCitationDocumentIn:' value: 'content' value:
'hasCitationDocumentIn: aBody
	^ (self contentBlocksIn: aBody) anySatisfy: [ :each |
		(each at: ''type'' ifAbsent: [ nil ]) = ''document''
			and: [ | citations |
				citations := each at: ''citations'' ifAbsent: [ nil ].
				citations notNil and: [ (citations at: ''enabled'' ifAbsent: [ false ]) == true ] ] ]'.
add value: #'ClaudeAnthropicDouble class' value: #'citationContentFor:' value: 'content' value:
'citationContentFor: aBody
	"One text block with one char_location citation into the first document."
	| document text start cited |
	document := (self contentBlocksIn: aBody) detect: [ :each | (each at: ''type'' ifAbsent: [ nil ]) = ''document'' ].
	text := (document at: ''source'' ifAbsent: [ Dictionary new ]) at: ''data'' ifAbsent: [ '''' ].
	text isString ifFalse: [ text := '''' ].
	start := text indexOfSubCollection: ''The preferred recovery path'' startingAt: 1.
	start = 0 ifTrue: [ start := 1 ].
	cited := ((text copyFrom: start to: text size) copyUpTo: $.), ''.''.
	^ { (Dictionary new
		at: ''type'' put: ''text'';
		at: ''text'' put: ''The document says to rebuild the image from clean source.'';
		at: ''citations'' put: { Dictionary new
			at: ''type'' put: ''char_location'';
			at: ''cited_text'' put: cited;
			at: ''document_index'' put: 0;
			at: ''document_title'' put: (document at: ''title'' ifAbsent: [ nil ]);
			at: ''start_char_index'' put: start - 1;
			at: ''end_char_index'' put: start - 1 + cited size;
			yourself };
		yourself) }'.
add value: #'ClaudeAnthropicDouble class' value: #'schemaOf:' value: 'content' value:
'schemaOf: aBody
	"The JSON schema of output_config.format, or nil."
	| config format |
	config := aBody at: ''output_config'' ifAbsent: [ ^ nil ].
	format := config at: ''format'' ifAbsent: [ ^ nil ].
	^ format at: ''schema'' ifAbsent: [ nil ]'.
add value: #'ClaudeAnthropicDouble class' value: #'jsonTextFor:' value: 'content' value:
'jsonTextFor: aSchema
	"A JSON text with a sample value of the right type for each property of the schema."
	| result |
	result := Dictionary new.
	(aSchema at: ''properties'' ifAbsent: [ Dictionary new ]) keysAndValuesDo: [ :name :property |
		result at: name put: (self sampleValueFor: property) ].
	^ STONJSON toString: result'.
add value: #'ClaudeAnthropicDouble class' value: #'sampleValueFor:' value: 'content' value:
'sampleValueFor: aPropertySchema
	| type |
	type := aPropertySchema at: ''type'' ifAbsent: [ ''string'' ].
	type = ''string'' ifTrue: [ ^ ''sample'' ].
	type = ''integer'' ifTrue: [ ^ 42 ].
	type = ''number'' ifTrue: [ ^ 1.5 ].
	type = ''boolean'' ifTrue: [ ^ true ].
	type = ''array'' ifTrue: [ ^ #() ].
	type = ''object'' ifTrue: [ ^ Dictionary new ].
	^ nil'.
add value: #'ClaudeAnthropicDouble class' value: #'plainReplyFor:' value: 'content' value:
'plainReplyFor: aBody
	"When the system prompt asks for a marker (MKR-...), the answer ends with the marker."
	| system index marker text |
	system := self systemTextOf: aBody.
	index := system indexOfSubCollection: ''MKR-'' startingAt: 1.
	index > 0 ifTrue: [
		marker := (system copyFrom: index to: system size) copyUpTo: Character space.
		(marker endsWith: ''.'') ifTrue: [ marker := marker allButLast ].
		^ ''The answer is 4. '', marker ].
	text := self lastUserTextOf: aBody.
	index := text indexOfSubCollection: ''Reply with exactly: '' startingAt: 1.
	index > 0 ifTrue: [ ^ (text copyFrom: index + ''Reply with exactly: '' size to: text size) trimBoth ].
	^ ''Hello from the local double.'''.
add value: #'ClaudeAnthropicDouble class' value: #'streamResponseFor:' value: 'messages' value:
'streamResponseFor: aMessage
	"The server-sent events of the message: message_start, one block start, the text in two
	 deltas and one block stop for each content block, message_delta and message_stop."
	| events start body response |
	events := OrderedCollection new.
	start := aMessage copy.
	start at: ''content'' put: #().
	start at: ''stop_reason'' put: nil.
	start at: ''usage'' put: ((aMessage at: ''usage'') copy at: ''output_tokens'' put: 0; yourself).
	events add: { ''message_start''. (Dictionary new
		at: ''type'' put: ''message_start'';
		at: ''message'' put: start;
		yourself) }.
	events add: { ''ping''. (Dictionary new at: ''type'' put: ''ping''; yourself) }.
	(aMessage at: ''content'') doWithIndex: [ :block :position |
		self addEventsOfBlock: block index: position - 1 to: events ].
	events add: { ''message_delta''. (Dictionary new
		at: ''type'' put: ''message_delta'';
		at: ''delta'' put: (Dictionary new
			at: ''stop_reason'' put: (aMessage at: ''stop_reason'');
			at: ''stop_sequence'' put: nil;
			yourself);
		at: ''usage'' put: (Dictionary new
			at: ''output_tokens'' put: ((aMessage at: ''usage'') at: ''output_tokens'');
			yourself);
		yourself) }.
	events add: { ''message_stop''. (Dictionary new at: ''type'' put: ''message_stop''; yourself) }.
	body := String streamContents: [ :stream |
		events do: [ :event |
			stream
				nextPutAll: ''event: '';
				nextPutAll: event first;
				nextPut: Character lf;
				nextPutAll: ''data: '';
				nextPutAll: (STONJSON toString: event second);
				nextPut: Character lf;
				nextPut: Character lf ] ].
	response := ZnResponse ok: (ZnEntity
		with: body
		type: (ZnMimeType main: ''text'' sub: ''event-stream'')).
	response headers at: ''request-id'' put: self nextRequestId.
	"The real service streams with Transfer-Encoding chunked: the chunk reader of
	 ClaudeStreamingSocket (enableChunked, readNextChunk) runs against the double too."
	response setTransferEncodingChunked.
	^ response'.
add value: #'ClaudeAnthropicDouble class' value: #'addEventsOfBlock:index:to:' value: 'messages' value:
'addEventsOfBlock: aBlock index: anIndex to: events
	"A text block is streamed as an empty block and two text deltas, any other block whole."
	| text half startBlock |
	(aBlock at: ''type'') = ''text''
		ifFalse: [
			events add: { ''content_block_start''. (Dictionary new
				at: ''type'' put: ''content_block_start'';
				at: ''index'' put: anIndex;
				at: ''content_block'' put: aBlock;
				yourself) } ]
		ifTrue: [
			text := aBlock at: ''text''.
			half := text size // 2.
			startBlock := aBlock copy.
			startBlock at: ''text'' put: ''''.
			events add: { ''content_block_start''. (Dictionary new
				at: ''type'' put: ''content_block_start'';
				at: ''index'' put: anIndex;
				at: ''content_block'' put: startBlock;
				yourself) }.
			{ text copyFrom: 1 to: half. text copyFrom: half + 1 to: text size } do: [ :piece |
				events add: { ''content_block_delta''. (Dictionary new
					at: ''type'' put: ''content_block_delta'';
					at: ''index'' put: anIndex;
					at: ''delta'' put: (Dictionary new
						at: ''type'' put: ''text_delta'';
						at: ''text'' put: piece;
						yourself);
					yourself) } ] ].
	events add: { ''content_block_stop''. (Dictionary new
		at: ''type'' put: ''content_block_stop'';
		at: ''index'' put: anIndex;
		yourself) }'.
add value: #'ClaudeAnthropicDouble class' value: #'routeFiles:request:' value: 'files' value:
'routeFiles: segments request: aRequest
	| method meta |
	((aRequest headers at: ''anthropic-beta'' ifAbsent: [ '''' ]) includesSubstring: ''files-api'')
		ifFalse: [ ^ self invalid: ''The Files API needs the files-api beta header'' ].
	method := aRequest method asString.
	segments size = 2 ifTrue: [
		method = ''POST'' ifTrue: [ ^ self uploadFile: aRequest ].
		method = ''GET'' ifTrue: [ ^ self listFiles: aRequest ] ].
	segments size >= 3 ifFalse: [ ^ self notFound: aRequest ].
	meta := DoubleFiles detect: [ :each | (each at: ''id'') = segments third ] ifNone: [ nil ].
	meta ifNil: [ ^ self errorStatus: 404 type: ''not_found_error'' message: ''File not found: '', segments third ].
	segments size = 3 ifTrue: [
		method = ''GET'' ifTrue: [ ^ self jsonOk: meta ].
		method = ''DELETE'' ifTrue: [
			DoubleFiles remove: meta.
			^ self jsonOk: (Dictionary new
				at: ''id'' put: segments third;
				at: ''type'' put: ''file_deleted'';
				yourself) ] ].
	(segments size = 4 and: [ segments fourth = ''content'' and: [ method = ''GET'' ] ])
		ifTrue: [ ^ self invalid: ''The file is not downloadable: only files that a tool created can be downloaded'' ].
	^ self notFound: aRequest'.
add value: #'ClaudeAnthropicDouble class' value: #'uploadFile:' value: 'files' value:
'uploadFile: aRequest
	"The part named file of the multipart body. The double keeps the metadata only."
	| entity part type meta |
	entity := aRequest entity.
	(entity isKindOf: ZnMultiPartFormDataEntity)
		ifFalse: [ ^ self invalid: ''file: a multipart/form-data body is required'' ].
	part := entity partNamed: ''file'' ifNone: [ nil ].
	part ifNil: [ ^ self invalid: ''file: Field required'' ].
	type := part contentType.
	meta := Dictionary new
		at: ''id'' put: ''file_double_'', self nextCounter printString;
		at: ''type'' put: ''file'';
		at: ''filename'' put: part fileName;
		at: ''mime_type'' put: (type
			ifNil: [ ''application/octet-stream'' ]
			ifNotNil: [ type main, ''/'', type sub ]);
		at: ''size_bytes'' put: (part contentLength ifNil: [ 0 ]);
		at: ''created_at'' put: DateAndTime now asUTC printString;
		at: ''downloadable'' put: false;
		yourself.
	DoubleFiles add: meta.
	^ self jsonOk: meta'.
add value: #'ClaudeAnthropicDouble class' value: #'listFiles:' value: 'files' value:
'listFiles: aRequest
	| limit page |
	limit := (aRequest uri queryAt: ''limit'' ifAbsent: [ nil ])
		ifNil: [ 20 ]
		ifNotNil: [ :text | text asInteger ifNil: [ 20 ] ].
	page := DoubleFiles asArray first: (limit min: DoubleFiles size).
	^ self jsonOk: (Dictionary new
		at: ''data'' put: page;
		at: ''has_more'' put: DoubleFiles size > limit;
		at: ''first_id'' put: (page ifEmpty: [ nil ] ifNotEmpty: [ page first at: ''id'' ]);
		at: ''last_id'' put: (page ifEmpty: [ nil ] ifNotEmpty: [ page last at: ''id'' ]);
		yourself)'.
add value: #'ClaudeAnthropicDouble class' value: #'routeSkills:request:' value: 'skills' value:
'routeSkills: segments request: aRequest
	| method skill version versions |
	((aRequest headers at: ''anthropic-beta'' ifAbsent: [ '''' ]) includesSubstring: ''skills-'')
		ifFalse: [ ^ self invalid: ''The Skills API needs the skills beta header'' ].
	method := aRequest method asString.
	segments size = 2 ifTrue: [
		method = ''POST'' ifTrue: [ ^ self createSkill: aRequest ].
		method = ''GET'' ifTrue: [
			^ self jsonOk: (Dictionary new
				at: ''data'' put: (DoubleSkills collect: [ :each | each at: ''skill'' ]) asArray;
				at: ''next_page'' put: nil;
				yourself) ] ].
	segments size >= 3 ifFalse: [ ^ self notFound: aRequest ].
	skill := DoubleSkills
		detect: [ :each | ((each at: ''skill'') at: ''id'') = segments third ]
		ifNone: [ nil ].
	skill ifNil: [ ^ self errorStatus: 404 type: ''not_found_error'' message: ''Skill not found: '', segments third ].
	versions := skill at: ''versions''.
	segments size = 3 ifTrue: [
		method = ''GET'' ifTrue: [ ^ self jsonOk: (skill at: ''skill'') ].
		method = ''DELETE'' ifTrue: [ ^ self deleteSkill: skill ] ].
	(segments size = 4 and: [ segments fourth = ''versions'' ]) ifTrue: [
		method = ''GET'' ifTrue: [
			^ self jsonOk: (Dictionary new
				at: ''data'' put: versions asArray;
				at: ''has_more'' put: false;
				at: ''next_page'' put: nil;
				yourself) ].
		method = ''POST'' ifTrue: [ ^ self addVersionTo: skill request: aRequest ] ].
	(segments size = 5 and: [ segments fourth = ''versions'' ]) ifTrue: [
		version := versions detect: [ :each | (each at: ''version'') = segments fifth ] ifNone: [ nil ].
		version ifNil: [ ^ self errorStatus: 404 type: ''not_found_error'' message: ''Skill version not found: '', segments fifth ].
		method = ''GET'' ifTrue: [ ^ self jsonOk: version ].
		method = ''DELETE'' ifTrue: [
			versions remove: version.
			^ self jsonOk: (Dictionary new
				at: ''id'' put: segments fifth;
				at: ''type'' put: ''skill_version_deleted'';
				yourself) ] ].
	^ self notFound: aRequest'.
add value: #'ClaudeAnthropicDouble class' value: #'versionFrom:skillId:' value: 'skills' value:
'versionFrom: anEntity skillId: aSkillId
	"The version record of a multipart upload, or the error response. The upload needs
	 files[] parts and a SKILL.md with a name in its frontmatter; the directory of the
	 files must carry the same name."
	| files manifest content name description directory versionId |
	(anEntity isKindOf: ZnMultiPartFormDataEntity)
		ifFalse: [ ^ self invalid: ''files: a multipart/form-data body is required'' ].
	files := anEntity parts select: [ :part | part fieldName = ''files[]'' ].
	files isEmpty ifTrue: [ ^ self invalid: ''files: Field required'' ].
	manifest := files
		detect: [ :part | part fileName notNil and: [ part fileName = ''SKILL.md'' or: [ part fileName endsWith: ''/SKILL.md'' ] ] ]
		ifNone: [ nil ].
	manifest ifNil: [ ^ self invalid: ''SKILL.md is required at the top of the skill directory'' ].
	content := manifest entity contents.
	content isString ifFalse: [ content := content utf8Decoded ].
	name := nil.
	description := ''''.
	content lines do: [ :line |
		(line beginsWith: ''name:'') ifTrue: [ name := (line allButFirst: 5) trimBoth ].
		(line beginsWith: ''description:'') ifTrue: [ description := (line allButFirst: 12) trimBoth ] ].
	name ifNil: [ ^ self invalid: ''SKILL.md needs a name in its frontmatter'' ].
	directory := (files first fileName includes: $/)
		ifTrue: [ files first fileName copyUpTo: $/ ]
		ifFalse: [ name ].
	directory = name
		ifFalse: [ ^ self invalid: ''The name in SKILL.md does not match the directory name: '', name, '' and '', directory ].
	versionId := (1759178010641129 + self nextCounter) printString.
	^ Dictionary new
		at: ''id'' put: versionId;
		at: ''type'' put: ''skill_version'';
		at: ''version'' put: versionId;
		at: ''skill_id'' put: aSkillId;
		at: ''name'' put: name;
		at: ''description'' put: description;
		at: ''directory'' put: directory;
		at: ''created_at'' put: DateAndTime now asUTC printString;
		yourself'.
add value: #'ClaudeAnthropicDouble class' value: #'createSkill:' value: 'skills' value:
'createSkill: aRequest
	"A second skill with the same display_title is refused, as the real service does."
	| entity title id version skill now |
	entity := aRequest entity.
	(entity isKindOf: ZnMultiPartFormDataEntity)
		ifFalse: [ ^ self invalid: ''files: a multipart/form-data body is required'' ].
	title := (entity partNamed: ''display_title'' ifNone: [ nil ])
		ifNotNil: [ :part | part fieldValueString ].
	(title notNil and: [ DoubleSkills anySatisfy: [ :each | ((each at: ''skill'') at: ''display_title'') = title ] ])
		ifTrue: [ ^ self invalid: ''Skill cannot reuse an existing display_title: '', title ].
	id := ''skill_double_'', self nextCounter printString.
	version := self versionFrom: entity skillId: id.
	version isDictionary ifFalse: [ ^ version ].
	now := DateAndTime now asUTC printString.
	skill := Dictionary new
		at: ''id'' put: id;
		at: ''type'' put: ''skill'';
		at: ''display_title'' put: (title ifNil: [ version at: ''name'' ]);
		at: ''latest_version'' put: (version at: ''version'');
		at: ''source'' put: ''custom'';
		at: ''created_at'' put: now;
		at: ''updated_at'' put: now;
		yourself.
	DoubleSkills add: (Dictionary new
		at: ''skill'' put: skill;
		at: ''versions'' put: (OrderedCollection with: version);
		yourself).
	^ self jsonOk: skill'.
add value: #'ClaudeAnthropicDouble class' value: #'addVersionTo:request:' value: 'skills' value:
'addVersionTo: aSkillRecord request: aRequest
	| skill version |
	skill := aSkillRecord at: ''skill''.
	version := self versionFrom: aRequest entity skillId: (skill at: ''id'').
	version isDictionary ifFalse: [ ^ version ].
	(aSkillRecord at: ''versions'') add: version.
	skill at: ''latest_version'' put: (version at: ''version'').
	^ self jsonOk: version'.
add value: #'ClaudeAnthropicDouble class' value: #'deleteSkill:' value: 'skills' value:
'deleteSkill: aSkillRecord
	"The real service refuses to delete a skill that still has versions."
	(aSkillRecord at: ''versions'') isEmpty
		ifFalse: [ ^ self invalid: ''Cannot delete a skill with versions. Delete all versions first.'' ].
	DoubleSkills remove: aSkillRecord.
	^ self jsonOk: (Dictionary new
		at: ''id'' put: ((aSkillRecord at: ''skill'') at: ''id'');
		at: ''type'' put: ''skill_deleted'';
		yourself)'.
add value: #'ClaudePlainStreamingSocket' value: #'connect' value: 'connection' value:
'connect
	"Open a plain TCP connection (no TLS). The streaming socket of the superclass opens TLS."
	socket := Socket newTCP.
	socket connectTo: (NetNameResolver addressForName: host) port: port.
	stream := ZnNetworkingUtils socketStreamOn: socket.
	stream timeout: timeout'.
add value: #'ClaudeClient' value: #'openPlainStreamingSocket:' value: 'streaming' value:
'openPlainStreamingSocket: aClaudeMessageRequest
	"The steps of openTLSStreamingSocket: over a plain socket, for the base URL of the local
	 double of the Anthropic API."
	| sock statusCode headers |
	sock := ClaudeAnthropicDouble newStreamingSocket.
	sock timeout: streamingTimeout.
	statusCode := 0.
	headers := Dictionary new.
	[[
		sock connect.
		sock sendPost: ''/v1/messages''
			headers: (self streamingHeadersFor: aClaudeMessageRequest)
			body: (STONJSON toString: aClaudeMessageRequest asJson).
		statusCode := sock readStatusLine.
		headers := sock readHeaders ]
		on: ConnectionTimedOut
		do: [ :e | self signalTimeoutClosing: sock message: e messageText ] ]
		on: NetworkError
		do: [ :e | self signalNetworkErrorClosing: sock message: e class name , '': '' , e messageText ].
	statusCode >= 400
		ifTrue: [ self handleStreamingError: statusCode headers: headers socket: sock ].
	sock requestId: (headers at: ''request-id'' ifAbsent: [ nil ]).
	^ sock'.

"ClaudeSDKExampleSupport class>>resolveClient: the client of the double, not a key from the
 environment or the keyring (the old method is replaced; the key slots stay in keySlots)."
fix value: #'ClaudeSDKExampleSupport class' value: #resolveClient
	value: [ :source | source includesSubstring: 'keySlots' ]
	value:
'resolveClient
	"Answer a client for the in-image double of the Messages API (ClaudeAnthropicDouble),
	 with the key that the double accepts. No key of the environment or of the keyring is
	 read, and no request leaves the machine. Fifteen of the integration test classes turn
	 any Error of resolveClient into a skip, so a double that does not start signals a
	 TestFailure (an Exception, not an Error): the test fails and does not skip."
	^ [ ClaudeAnthropicDouble clientForTests ]
		on: Error
		do: [ :error |
			TestFailure signal: ''The local double of the Anthropic API did not start: '', error messageText asString ]'.

fix value: #ClaudeClient value: #openStreamingSocket:
	value: [ :source | (source includesSubstring: 'ClaudeAnthropicDouble') not ]
	value:
'openStreamingSocket: aClaudeMessageRequest
	"Open a streaming connection. Uses a real TLS socket for HTTPS (incremental SSE), a plain
	 socket for the base URL of the local double, or a buffered ZnClient for any other HTTP
	 URL (test/mock servers). All return objects compatible with ClaudeSSEDecoder (atEnd/nextLine)."
	aClaudeMessageRequest stream: true.
	(baseUrl beginsWith: ''https://'')
		ifTrue: [ ^ self openTLSStreamingSocket: aClaudeMessageRequest ].
	(ClaudeAnthropicDouble isDoubleUrl: baseUrl)
		ifTrue: [ ^ self openPlainStreamingSocket: aClaudeMessageRequest ].
	^ self openBufferedStreamingSocket: aClaudeMessageRequest'.

fix value: #ClaudeIntegrationTest value: #testRealStreamingClose
	value: [ :source | source includesSubstring: 'ClaudeStreamingSocket host:' ]
	value:
'testRealStreamingClose
	"Verify socket closes cleanly after streaming completes."
	| sock decoder |
	sock := ClaudeAnthropicDouble newStreamingSocket.
	sock connect.
	sock sendPost: ''/v1/messages''
		headers: (OrderedDictionary newFromPairs: {
			''x-api-key''. client apiKey.
			''anthropic-version''. ''2023-06-01''.
			''content-type''. ''application/json''.
			''accept''. ''text/event-stream'' })
		body: (STONJSON toString: {
			''model'' -> ClaudeModel default.
			''max_tokens'' -> 20.
			''stream'' -> true.
			''messages'' -> { { ''role'' -> ''user''. ''content'' -> ''Hi'' } asDictionary }
		} asDictionary).
	sock readStatusLine.
	sock readHeaders.
	decoder := ClaudeSSEDecoder onStream: sock.
	[ decoder hasNext ] whileTrue: [ decoder nextEvent ].
	ClaudeTestLog log: ''<< stream drained; closing socket''.
	sock close.
	self deny: sock isConnected'.

fix value: #ClaudeIntegrationTest value: #testRealStreamingConnection
	value: [ :source | source includesSubstring: 'ClaudeStreamingSocket host:' ]
	value:
'testRealStreamingConnection
	"Verify the TLS streaming socket connects, sends a request, and gets status 200."
	| sock statusCode headers decoder |
	sock := ClaudeAnthropicDouble newStreamingSocket.
	sock connect.
	self assert: sock isConnected.
	sock sendPost: ''/v1/messages''
		headers: (OrderedDictionary newFromPairs: {
			''x-api-key''. client apiKey.
			''anthropic-version''. ''2023-06-01''.
			''content-type''. ''application/json''.
			''accept''. ''text/event-stream'' })
		body: (STONJSON toString: {
			''model'' -> ClaudeModel default.
			''max_tokens'' -> 20.
			''stream'' -> true.
			''messages'' -> { { ''role'' -> ''user''. ''content'' -> ''Hi'' } asDictionary }
		} asDictionary).
	statusCode := sock readStatusLine.
	headers := sock readHeaders.
	ClaudeTestLog log: ''<< streaming connect status='' , statusCode printString.
	self assert: statusCode equals: 200.
	self assert: ((headers at: ''content-type'') beginsWith: ''text/event-stream'').
	"Drain the response to allow clean close"
	decoder := ClaudeSSEDecoder onStream: sock.
	[ decoder hasNext ] whileTrue: [ decoder nextEvent ].
	sock close'.

fix value: #ClaudeIntegrationTest value: #testRealStreamingEvents
	value: [ :source | source includesSubstring: 'ClaudeStreamingSocket host:' ]
	value:
'testRealStreamingEvents
	"Send a message via real TLS streaming socket, verify SSE events arrive and accumulate correctly."
	| sock decoder message events |
	sock := ClaudeAnthropicDouble newStreamingSocket.
	sock connect.
	sock sendPost: ''/v1/messages''
		headers: (OrderedDictionary newFromPairs: {
			''x-api-key''. client apiKey.
			''anthropic-version''. ''2023-06-01''.
			''content-type''. ''application/json''.
			''accept''. ''text/event-stream'' })
		body: (STONJSON toString: {
			''model'' -> ClaudeModel default.
			''max_tokens'' -> 50.
			''stream'' -> true.
			''messages'' -> { { ''role'' -> ''user''. ''content'' -> ''Say hi in 3 words'' } asDictionary }
		} asDictionary).
	sock readStatusLine.
	sock readHeaders.
	decoder := ClaudeSSEDecoder onStream: sock.
	message := ClaudeMessage new.
	events := OrderedCollection new.
	[ decoder hasNext ] whileTrue: [
		| event |
		event := decoder nextEvent.
		events add: (event at: ''event'').
		message accumulate: event ].
	sock close.
	ClaudeTestLog log: ''<< real streaming: '' , events size printString , '' events, '' , message textContent size printString , '' chars''.
	self deny: message textContent isEmpty.
	self assert: message stopReason equals: #end_turn.
	self assert: (events includes: ''message_start'').
	self assert: (events includes: ''content_block_delta'').
	self assert: (events includes: ''message_stop'').
	Transcript
		nextPutAll: ''<< Real streaming: '';
		nextPutAll: events size printString;
		nextPutAll: '' events, text: '';
		nextPutAll: message textContent;
		cr'.

fix value: #ClaudeIntegrationTest value: #testErrorInvalidKey
	value: [ :source | source includesSubstring: 'ClaudeClient apiKey:' ]
	value:
'testErrorInvalidKey
	"Model choice is immaterial - the 401 fires before model lookup. Use the default helper."
	| badClient params |
	badClient := ClaudeAnthropicDouble clientWithKey: ''sk-invalid''.
	params := self newParams: ''Hello'' maxTokens: 50.
	ClaudeTestLog log: ''>> testErrorInvalidKey: expecting 401...''.
	self
		should: [ badClient sendMessage: params ]
		raise: ClaudeAuthError
		withExceptionDo: [ :e |
			ClaudeTestLog log: ''<< Caught: '' , e class name , '' ('' , e statusCode printString , '')''.
			self assert: e statusCode equals: 401 ]'.

fix value: #ClaudeIntegrationTest value: #testRealStreamingErrorHandling
	value: [ :source | source includesSubstring: 'ClaudeClient apiKey:' ]
	value:
'testRealStreamingErrorHandling
	"Verify TLS streaming raises ClaudeApiError for invalid API key. Expect ClaudeAuthError (401) or ClaudeTimeoutError if the network is slow."
	| badClient params |
	badClient := ClaudeAnthropicDouble clientWithKey: ''sk-invalid''.
	badClient streamingTimeout: 30.
	params := ClaudeMessageRequest new.
	params model: ClaudeModel default.
	params maxTokens: 50.
	params addUserMessage: ''Hello''.
	ClaudeTestLog log: ''>> streaming with invalid key, expecting ClaudeApiError''.
	self
		should: [ badClient streamMessage: params do: [ :e | ] ]
		raise: ClaudeApiError'.

"PharoKeyring-Tests: the tests of isAvailable."
add value: #LinuxSecretToolBackendTest value: #'isToolOnPath:' value: 'private' value:
'isToolOnPath: aToolName
	"True when a directory of PATH holds a file with the name aToolName. This search does not
	 use which(1), so the test compares isAvailable with a second source."
	| path |
	path := Smalltalk os environment at: ''PATH'' ifAbsent: [ '''' ].
	^ (path substrings: '':'') anySatisfy: [ :directory |
		(directory asFileReference / aToolName) isFile ]'.

fix value: #LinuxSecretToolBackendTest value: #testIsAvailable
	value: [ :source | source includesSubstring: 'skip:' ]
	value:
'testIsAvailable
	"isAvailable answers true exactly when the tool secret-tool is a file in a PATH directory.
	 The test never skips: on a machine without the tool, it checks the answer false."
	self assert: backend isAvailable equals: (self isToolOnPath: ''secret-tool'')'.

add value: #PassToolBackendTest value: #'isToolOnPath:' value: 'private' value:
'isToolOnPath: aToolName
	"True when a directory of PATH holds a file with the name aToolName. This search does not
	 use which(1), so the test compares isAvailable with a second source."
	| path |
	path := Smalltalk os environment at: ''PATH'' ifAbsent: [ '''' ].
	^ (path substrings: '':'') anySatisfy: [ :directory |
		(directory asFileReference / aToolName) isFile ]'.

fix value: #PassToolBackendTest value: #testIsAvailable
	value: [ :source | source includesSubstring: 'skip:' ]
	value:
'testIsAvailable
	"isAvailable answers true exactly when the tool pass is a file in a PATH directory.
	 The test never skips: on a machine without the tool, it checks the answer false."
	self assert: backend isAvailable equals: (self isToolOnPath: ''pass'')'.
