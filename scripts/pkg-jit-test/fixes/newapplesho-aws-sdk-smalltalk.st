"newapplesho-aws-sdk-smalltalk: the live integration tests need DynamoDB Local
(127.0.0.1:8000) and floci (127.0.0.1:4566), which this machine does not run.
No emulator may be downloaded, so the class AWSLocalEmulatorDouble answers the
requests inside the image with two ZnServer instances.  See
docs/upstream/package-repairs.md for the evidence and the limits of the double.
The three setUp methods of the live tests start the double first; their
assertions are not changed."

(Smalltalk globals includesKey: #AWSLocalEmulatorDouble) ifFalse: [
	(Object << #AWSLocalEmulatorDouble
		sharedVariables: { #Servers. #Tables. #Buckets };
		package: 'AWS-Local-Emulator-Double';
		yourself) install ].

add value: #'AWSLocalEmulatorDouble class' value: #'ensureServers' value: 'lifecycle' value:
'ensureServers
	"Start the two local servers when they do not run. Called by setUp of the
	 live integration tests."
	(Servers notNil and: [ Servers allSatisfy: [ :each | each isRunning ] ])
		ifTrue: [ ^ self ].
	self startServers'.

add value: #'AWSLocalEmulatorDouble class' value: #'startServers' value: 'lifecycle' value:
'startServers
	"DynamoDB Local listens on 127.0.0.1:8000, floci on 127.0.0.1:4566."
	Servers ifNotNil: [ Servers do: [ :each | each stop ] ].
	Tables := Dictionary new.
	Buckets := OrderedCollection new.
	Servers := {
		self serverOn: 8000 selector: #handleDynamoDB:.
		self serverOn: 4566 selector: #handleS3: }'.

add value: #'AWSLocalEmulatorDouble class' value: #'serverOn:selector:' value: 'lifecycle' value:
'serverOn: aPort selector: aSelector
	| server |
	server := ZnServer on: aPort.
	server bindingAddress: #[127 0 0 1].
	server delegate: (ZnValueDelegate with: [ :request | self perform: aSelector with: request ]).
	server start.
	^ server'.

add value: #'AWSLocalEmulatorDouble class' value: #'bodyOf:' value: 'http' value:
'bodyOf: aRequest
	| contents |
	contents := aRequest contents.
	contents ifNil: [ ^ '''' ].
	^ contents isString ifTrue: [ contents ] ifFalse: [ contents utf8Decoded ]'.

add value: #'AWSLocalEmulatorDouble class' value: #'jsonResponse:status:' value: 'http' value:
'jsonResponse: aDictionary status: aStatusLine
	^ (ZnResponse statusLine: aStatusLine)
		entity: ((ZnEntity bytes: (NeoJSONWriter toString: aDictionary) utf8Encoded)
			contentType: ''application/x-amz-json-1.0'';
			yourself);
		yourself'.

add value: #'AWSLocalEmulatorDouble class' value: #'xmlResponse:status:' value: 'http' value:
'xmlResponse: aString status: aStatusLine
	^ (ZnResponse statusLine: aStatusLine)
		entity: ((ZnEntity bytes: aString utf8Encoded)
			contentType: ''application/xml'';
			yourself);
		yourself'.

add value: #'AWSLocalEmulatorDouble class' value: #'ddbError:message:' value: 'dynamodb' value:
'ddbError: aTypeString message: aMessage
	^ self
		jsonResponse: (Dictionary new
			at: ''__type'' put: ''com.amazonaws.dynamodb.v20120810#'', aTypeString;
			at: ''message'' put: aMessage;
			yourself)
		status: ZnStatusLine badRequest'.

add value: #'AWSLocalEmulatorDouble class' value: #'checkKeysOf:allowed:' value: 'dynamodb' value:
'checkKeysOf: aBody allowed: anArray
	"Nil when every parameter is one that this double implements, else the
	 error response. A parameter that the double ignores would hide a defect."
	| unknown |
	unknown := aBody keys reject: [ :each | anArray includes: each ].
	unknown isEmpty ifTrue: [ ^ nil ].
	^ self
		ddbError: ''ValidationException''
		message: ''The local double does not implement the parameters '', ('', '' join: unknown asSortedCollection)'.

add value: #'AWSLocalEmulatorDouble class' value: #'tableFor:' value: 'dynamodb' value:
'tableFor: aBody
	^ Tables at: (aBody at: ''TableName'' ifAbsent: [ nil ]) ifAbsent: [ nil ]'.

add value: #'AWSLocalEmulatorDouble class' value: #'missingTableError' value: 'dynamodb' value:
'missingTableError
	^ self ddbError: ''ResourceNotFoundException'' message: ''Cannot do operations on a non-existent table'''.

add value: #'AWSLocalEmulatorDouble class' value: #'itemIn:keyedBy:' value: 'dynamodb' value:
'itemIn: aTable keyedBy: aKeyDictionary
	| keyName |
	keyName := aTable at: ''key''.
	^ (aTable at: ''items'')
		detect: [ :each | (each at: keyName) = (aKeyDictionary at: keyName ifAbsent: [ 0 ]) ]
		ifNone: [ nil ]'.

add value: #'AWSLocalEmulatorDouble class' value: #'descriptionOfTable:' value: 'dynamodb' value:
'descriptionOfTable: aName
	| table |
	table := Tables at: aName.
	^ Dictionary new
		at: ''TableName'' put: aName;
		at: ''TableStatus'' put: ''ACTIVE'';
		at: ''KeySchema'' put: {
			Dictionary new
				at: ''AttributeName'' put: (table at: ''key'');
				at: ''KeyType'' put: ''HASH'';
				yourself };
		at: ''ItemCount'' put: (table at: ''items'') size;
		yourself'.

add value: #'AWSLocalEmulatorDouble class' value: #'evaluateCondition:on:' value: 'dynamodb' value:
'evaluateCondition: anExpression on: anItem
	"True or false for attribute_exists(name) and attribute_not_exists(name),
	 nil for any other expression."
	| text name present |
	text := anExpression trimmed.
	((text beginsWith: ''attribute_not_exists('') or: [ text beginsWith: ''attribute_exists('' ])
		ifFalse: [ ^ nil ].
	((text endsWith: '')'') and: [ (text occurrencesOf: $() = 1 and: [ (text occurrencesOf: $)) = 1 ] ])
		ifFalse: [ ^ nil ].
	name := ((text copyAfter: $() copyUpTo: $)) trimmed.
	(name isEmpty or: [ name includes: Character space ]) ifTrue: [ ^ nil ].
	present := anItem notNil and: [ anItem includesKey: name ].
	^ (text beginsWith: ''attribute_not_exists('')
		ifTrue: [ present not ]
		ifFalse: [ present ]'.

add value: #'AWSLocalEmulatorDouble class' value: #'handleDynamoDB:' value: 'dynamodb' value:
'handleDynamoDB: aRequest
	| target operation selector result |
	target := aRequest headers at: ''X-Amz-Target'' ifAbsent: [ nil ].
	target ifNil: [
		^ self ddbError: ''MissingAuthenticationTokenException'' message: ''Missing the X-Amz-Target header'' ].
	operation := target copyAfterLast: $..
	selector := (''ddb'', operation, '':'') asSymbol.
	(self respondsTo: selector) ifFalse: [
		^ self ddbError: ''UnknownOperationException'' message: ''The local double does not implement '', operation ].
	result := self perform: selector with: (NeoJSONReader fromString: (self bodyOf: aRequest)).
	^ result isDictionary
		ifTrue: [ self jsonResponse: result status: ZnStatusLine ok ]
		ifFalse: [ result ]'.

add value: #'AWSLocalEmulatorDouble class' value: #'ddbCreateTable:' value: 'dynamodb' value:
'ddbCreateTable: aBody
	| name hash |
	(self checkKeysOf: aBody allowed: #(''TableName'' ''AttributeDefinitions'' ''KeySchema'' ''ProvisionedThroughput''))
		ifNotNil: [ :error | ^ error ].
	name := aBody at: ''TableName'' ifAbsent: [ nil ].
	name ifNil: [ ^ self ddbError: ''ValidationException'' message: ''TableName is missing'' ].
	(Tables includesKey: name)
		ifTrue: [ ^ self ddbError: ''ResourceInUseException'' message: ''Cannot create preexisting table'' ].
	hash := (aBody at: ''KeySchema'' ifAbsent: [ #() ])
		detect: [ :each | (each at: ''KeyType'') = ''HASH'' ]
		ifNone: [ nil ].
	hash ifNil: [ ^ self ddbError: ''ValidationException'' message: ''KeySchema has no HASH key'' ].
	Tables at: name put: (Dictionary new
		at: ''key'' put: (hash at: ''AttributeName'');
		at: ''items'' put: OrderedCollection new;
		yourself).
	^ Dictionary new at: ''TableDescription'' put: (self descriptionOfTable: name); yourself'.

add value: #'AWSLocalEmulatorDouble class' value: #'ddbDeleteTable:' value: 'dynamodb' value:
'ddbDeleteTable: aBody
	| description |
	(self checkKeysOf: aBody allowed: #(''TableName'')) ifNotNil: [ :error | ^ error ].
	(self tableFor: aBody) ifNil: [ ^ self missingTableError ].
	description := self descriptionOfTable: (aBody at: ''TableName'').
	Tables removeKey: (aBody at: ''TableName'').
	^ Dictionary new at: ''TableDescription'' put: description; yourself'.

add value: #'AWSLocalEmulatorDouble class' value: #'ddbDescribeTable:' value: 'dynamodb' value:
'ddbDescribeTable: aBody
	(self checkKeysOf: aBody allowed: #(''TableName'')) ifNotNil: [ :error | ^ error ].
	(self tableFor: aBody) ifNil: [ ^ self missingTableError ].
	^ Dictionary new at: ''Table'' put: (self descriptionOfTable: (aBody at: ''TableName'')); yourself'.

add value: #'AWSLocalEmulatorDouble class' value: #'ddbListTables:' value: 'dynamodb' value:
'ddbListTables: aBody
	"The client puts TableName in each request (DynamoDBTable>>action:), also in this one.  ListTables has no such member.  The test of the package for this request is made for DynamoDB Local, so that server accepts the member."
	(self checkKeysOf: aBody allowed: #(''TableName'')) ifNotNil: [ :error | ^ error ].
	^ Dictionary new at: ''TableNames'' put: Tables keys asSortedCollection asArray; yourself'.

add value: #'AWSLocalEmulatorDouble class' value: #'ddbPutItem:' value: 'dynamodb' value:
'ddbPutItem: aBody
	| table item existing condition result items |
	(self checkKeysOf: aBody allowed: #(''TableName'' ''Item'' ''ConditionExpression'' ''ExpressionAttributeValues''))
		ifNotNil: [ :error | ^ error ].
	table := self tableFor: aBody.
	table ifNil: [ ^ self missingTableError ].
	item := aBody at: ''Item'' ifAbsent: [ nil ].
	(item isNil or: [ (item includesKey: (table at: ''key'')) not ]) ifTrue: [
		^ self ddbError: ''ValidationException''
			message: ''One or more parameter values were invalid: Missing the key '', (table at: ''key''), '' in the item'' ].
	existing := self itemIn: table keyedBy: item.
	condition := aBody at: ''ConditionExpression'' ifAbsent: [ nil ].
	condition ifNotNil: [
		result := self evaluateCondition: condition on: existing.
		result ifNil: [
			^ self ddbError: ''ValidationException''
				message: ''The local double does not implement the ConditionExpression '', condition ].
		result ifFalse: [
			^ self ddbError: ''ConditionalCheckFailedException'' message: ''The conditional request failed'' ] ].
	items := table at: ''items''.
	existing
		ifNil: [ items add: item ]
		ifNotNil: [ items at: (items indexOf: existing) put: item ].
	^ Dictionary new'.

add value: #'AWSLocalEmulatorDouble class' value: #'ddbGetItem:' value: 'dynamodb' value:
'ddbGetItem: aBody
	| table item |
	(self checkKeysOf: aBody allowed: #(''TableName'' ''Key'')) ifNotNil: [ :error | ^ error ].
	table := self tableFor: aBody.
	table ifNil: [ ^ self missingTableError ].
	item := self itemIn: table keyedBy: (aBody at: ''Key'' ifAbsent: [ Dictionary new ]).
	^ item
		ifNil: [ Dictionary new ]
		ifNotNil: [ Dictionary new at: ''Item'' put: item; yourself ]'.

add value: #'AWSLocalEmulatorDouble class' value: #'ddbDeleteItem:' value: 'dynamodb' value:
'ddbDeleteItem: aBody
	| table item |
	(self checkKeysOf: aBody allowed: #(''TableName'' ''Key'')) ifNotNil: [ :error | ^ error ].
	table := self tableFor: aBody.
	table ifNil: [ ^ self missingTableError ].
	item := self itemIn: table keyedBy: (aBody at: ''Key'' ifAbsent: [ Dictionary new ]).
	item ifNotNil: [ (table at: ''items'') remove: item ].
	^ Dictionary new'.

add value: #'AWSLocalEmulatorDouble class' value: #'ddbQuery:' value: 'dynamodb' value:
'ddbQuery: aBody
	| table tokens values wanted items |
	(self checkKeysOf: aBody allowed: #(''TableName'' ''Select'' ''KeyConditionExpression'' ''ExpressionAttributeValues''))
		ifNotNil: [ :error | ^ error ].
	table := self tableFor: aBody.
	table ifNil: [ ^ self missingTableError ].
	tokens := (aBody at: ''KeyConditionExpression'' ifAbsent: [ '''' ]) substrings.
	((tokens size = 3 and: [ (tokens at: 2) = ''='' ]) and: [ (tokens at: 1) = (table at: ''key'') ])
		ifFalse: [
			^ self ddbError: ''ValidationException''
				message: ''The local double implements only a KeyConditionExpression of the form <hash key> = :value'' ].
	values := aBody at: ''ExpressionAttributeValues'' ifAbsent: [ Dictionary new ].
	wanted := values at: (tokens at: 3) ifAbsent: [
		^ self ddbError: ''ValidationException''
			message: ''An expression attribute value is used in an expression but not defined: '', (tokens at: 3) ].
	items := (table at: ''items'') select: [ :each | (each at: (table at: ''key'')) = wanted ].
	^ Dictionary new
		at: ''Items'' put: items asArray;
		at: ''Count'' put: items size;
		at: ''ScannedCount'' put: items size;
		yourself'.

add value: #'AWSLocalEmulatorDouble class' value: #'ddbScan:' value: 'dynamodb' value:
'ddbScan: aBody
	"Like DynamoDB, a page that reaches Limit carries a LastEvaluatedKey even
	 when no item follows; the next page is then empty."
	| table items start found limit page result keyName |
	(self checkKeysOf: aBody allowed: #(''TableName'' ''Select'' ''Limit'' ''ExclusiveStartKey''))
		ifNotNil: [ :error | ^ error ].
	table := self tableFor: aBody.
	table ifNil: [ ^ self missingTableError ].
	(aBody at: ''Select'' ifAbsent: [ ''ALL_ATTRIBUTES'' ]) = ''ALL_ATTRIBUTES'' ifFalse: [
		^ self ddbError: ''ValidationException'' message: ''The local double implements only Select ALL_ATTRIBUTES'' ].
	keyName := table at: ''key''.
	items := table at: ''items''.
	start := 0.
	(aBody at: ''ExclusiveStartKey'' ifAbsent: [ nil ]) ifNotNil: [ :startKey |
		found := self itemIn: table keyedBy: startKey.
		found ifNil: [
			^ self ddbError: ''ValidationException'' message: ''The provided starting key is invalid'' ].
		start := items indexOf: found ].
	limit := aBody at: ''Limit'' ifAbsent: [ nil ].
	(limit notNil and: [ limit < 1 ]) ifTrue: [
		^ self ddbError: ''ValidationException'' message: ''Limit must be greater than or equal to 1'' ].
	page := (items copyFrom: start + 1 to: items size) asOrderedCollection.
	(limit notNil and: [ limit < page size ]) ifTrue: [ page := page first: limit ].
	result := Dictionary new
		at: ''Items'' put: page asArray;
		at: ''Count'' put: page size;
		at: ''ScannedCount'' put: page size;
		yourself.
	(limit notNil and: [ page size = limit ]) ifTrue: [
		result at: ''LastEvaluatedKey'' put: (Dictionary new
			at: keyName put: (page last at: keyName);
			yourself) ].
	^ result'.

add value: #'AWSLocalEmulatorDouble class' value: #'s3Error:code:message:' value: 's3' value:
's3Error: aStatusLine code: aCode message: aMessage
	^ self
		xmlResponse: ''<?xml version="1.0" encoding="UTF-8"?><Error><Code>'', aCode, ''</Code><Message>'', aMessage, ''</Message></Error>''
		status: aStatusLine'.

add value: #'AWSLocalEmulatorDouble class' value: #'handleS3:' value: 's3' value:
'handleS3: aRequest
	"The bucket is in the host name (virtual-hosted style): <bucket>.localhost:4566."
	| host bucket isRoot method entry xml |
	host := (aRequest headers at: ''Host'' ifAbsent: [ '''' ]) copyUpTo: $:.
	bucket := (host endsWith: ''.localhost'')
		ifTrue: [ host copyFrom: 1 to: host size - ''.localhost'' size ]
		ifFalse: [ nil ].
	isRoot := aRequest uri printString = ''/''.
	method := aRequest method.
	(bucket notNil and: [ bucket isEmpty or: [ bucket anySatisfy: [ :each |
			(each isLowercase or: [ each isDigit or: [ ''-.'' includes: each ] ]) not ] ] ])
		ifTrue: [ ^ self s3Error: ZnStatusLine badRequest code: ''InvalidBucketName'' message: ''The specified bucket is not valid'' ].
	(bucket isNil and: [ isRoot and: [ method = #GET ] ]) ifTrue: [
		xml := WriteStream on: String new.
		xml nextPutAll: ''<?xml version="1.0" encoding="UTF-8"?><ListAllMyBucketsResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/"><Owner><ID>local</ID><DisplayName>local</DisplayName></Owner><Buckets>''.
		Buckets do: [ :each |
			xml nextPutAll: ''<Bucket><Name>'', each key, ''</Name><CreationDate>'', each value, ''</CreationDate></Bucket>'' ].
		xml nextPutAll: ''</Buckets></ListAllMyBucketsResult>''.
		^ self xmlResponse: xml contents status: ZnStatusLine ok ].
	(bucket notNil and: [ isRoot and: [ method = #PUT ] ]) ifTrue: [
		(Buckets anySatisfy: [ :each | each key = bucket ]) ifTrue: [
			^ self s3Error: (ZnStatusLine code: 409) code: ''BucketAlreadyOwnedByYou'' message: ''Your previous request to create the named bucket succeeded and you already own it'' ].
		Buckets add: bucket -> DateAndTime now asUTC printString.
		^ ZnResponse ok: (ZnEntity text: '''') ].
	(bucket notNil and: [ isRoot and: [ method = #DELETE ] ]) ifTrue: [
		entry := Buckets detect: [ :each | each key = bucket ] ifNone: [ nil ].
		entry ifNil: [
			^ self s3Error: ZnStatusLine notFound code: ''NoSuchBucket'' message: ''The specified bucket does not exist'' ].
		Buckets remove: entry.
		^ ZnResponse noContent ].
	^ self s3Error: ZnStatusLine notImplemented code: ''NotImplemented'' message: ''The local double implements only the bucket operations ListBuckets, CreateBucket and DeleteBucket'''.

"The live tests: setUp starts the double, then runs the unchanged old setUp
under the name setUpAgainstEmulator.  The guard is the absence of the call."
#(#DynamoDBTableTest #DynamoDBRawClientTest #AWSS3IntegrationTest) do: [ :className |
	| cls oldSource |
	cls := Smalltalk globals at: className ifAbsent: [ nil ].
	(cls notNil and: [ cls includesSelector: #setUp ])
		ifFalse: [ Stdio stdout nextPutAll: 'SKIP ', className, '>>setUp (absent)'; lf; flush ]
		ifTrue: [
			oldSource := (cls >> #setUp) sourceCode.
			(oldSource includesSubstring: 'AWSLocalEmulatorDouble')
				ifTrue: [ Stdio stdout nextPutAll: 'SKIP ', className, '>>setUp (defect not present)'; lf; flush ]
				ifFalse: [
					cls compile: 'setUpAgainstEmulator', (oldSource allButFirst: 'setUp' size) classified: 'running'.
					cls compile: 'setUp
	AWSLocalEmulatorDouble ensureServers.
	self setUpAgainstEmulator' classified: 'running'.
					Stdio stdout nextPutAll: 'FIXED ', className, '>>setUp'; lf; flush ] ] ].
