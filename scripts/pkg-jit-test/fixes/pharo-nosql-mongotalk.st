"pharo-nosql-mongotalk: the tests need a MongoDB server on 127.0.0.1:27017, and this machine has none.
No MongoDB may be installed or downloaded, so the class MongoServerDouble is a stand-in server inside the
image. It listens on 127.0.0.1:27017 and speaks the OP_MSG wire protocol that the client (MongoDriverV5)
uses, with the BSON classes of the package. It keeps databases and collections in memory. The classes:
MongoServerDouble (sockets, commands, cursors, SCRAM-SHA-1), MongoDoubleCollection (documents and unique
indexes), MongoDoubleMatcher (query operators, sort, projection, update), MongoDoubleBSON (a BSON reader
that keeps the order of the fields), MongoDoubleError (a command error with a code).
The five setUp methods that open a connection (MongoTest, MongoWholeDatabaseTest, MongoCommandCursorTest,
MongoAuthTest, MQQueryTest) start the stand-in first. No assertion changes. The limits of the stand-in
are in docs/upstream/package-repairs.md.

Three entries change a test, each for a defect of the test and not of the server:
- MongoTest>>testAddExistingCollection starts with self skip, and the comment says that the server
  answers OK for a collection that exists. The driver has code for the answer of MongoDB to create
  on an existing collection, error 48 (NamespaceExists): MongoDatabase>>addCollection:capped:size:max:
  and MongoCollection>>ensureAdded. The stand-in answers error 48, so the entry removes the skip.
- MongoAuthTest>>testSCRAMSHA1AuthMechanism starts with self skip, because it needs a user that a person
  creates by hand. The stand-in has the user pharounittest with the password test in the database test.
  The entry removes the skip.
- MQQueryTest>>testWhere sends the JavaScript this.key1 = ""value1"". In JavaScript = is an assignment
  and gives a true value, so a real server matches every document, and the test passes only because
  the wanted document is the first one. The entry writes == so that the query tests the equality."

(Smalltalk globals includesKey: #MongoDoubleError) ifFalse: [
	(Error << #MongoDoubleError
		slots: { #code. #codeName };
		package: 'MongoTalk-ServerDouble';
		yourself) install ].

(Smalltalk globals includesKey: #MongoDoubleBSON) ifFalse: [
	(BSON << #MongoDoubleBSON
		package: 'MongoTalk-ServerDouble';
		yourself) install ].

(Smalltalk globals includesKey: #MongoDoubleMatcher) ifFalse: [
	(Object << #MongoDoubleMatcher
		package: 'MongoTalk-ServerDouble';
		yourself) install ].

(Smalltalk globals includesKey: #MongoDoubleCollection) ifFalse: [
	(Object << #MongoDoubleCollection
		slots: { #name. #namespace. #documents. #indexes. #ids };
		package: 'MongoTalk-ServerDouble';
		yourself) install ].

(Smalltalk globals includesKey: #MongoServerDouble) ifFalse: [
	(Object << #MongoServerDouble
		slots: { #listener. #acceptor. #databases. #cursors. #connections. #conversations. #users. #nextCursorId. #nextRequestId };
		sharedVariables: { #Current };
		package: 'MongoTalk-ServerDouble';
		yourself) install ].

add value: #'MongoDoubleError class' value: #'signalCode:name:message:' value: 'signalling' value:
'signalCode: aCode name: aName message: aString
	^ self new
		code: aCode;
		codeName: aName;
		signal: aString'.

add value: #'MongoDoubleError' value: #'code' value: 'accessing' value:
'code
	^ code'.

add value: #'MongoDoubleError' value: #'code:' value: 'accessing' value:
'code: anInteger
	code := anInteger'.

add value: #'MongoDoubleError' value: #'codeName' value: 'accessing' value:
'codeName
	^ codeName'.

add value: #'MongoDoubleError' value: #'codeName:' value: 'accessing' value:
'codeName: aString
	codeName := aString'.

add value: #'MongoDoubleBSON' value: #'nextDictionary' value: 'reading' value:
'nextDictionary
	"Like BSON>>nextDictionary, but the answer keeps the order of the fields:
	 the first field of a command is the name of the command."
	| result |
	result := OrderedDictionary new.
	stream nextUInt32.
	[ | type |
	type := stream nextByte.
	type = 0 ifTrue: [ ^ result ].
	self addObjectType: type to: result ] repeat'.

add value: #'MongoDoubleMatcher class' value: #'bsonTypeOf:' value: 'types' value:
'bsonTypeOf: aValue
	"The BSON type number that the value gets when the client writes it."
	aValue isNil ifTrue: [ ^ 10 ].
	(aValue == true or: [ aValue == false ]) ifTrue: [ ^ 8 ].
	aValue isString ifTrue: [ ^ 2 ].
	aValue isFloat ifTrue: [ ^ 1 ].
	aValue isInteger ifTrue: [
		^ (aValue between: -2147483648 and: 2147483647)
			ifTrue: [ 16 ]
			ifFalse: [ 18 ] ].
	aValue isDictionary ifTrue: [ ^ 3 ].
	(aValue isKindOf: ByteArray) ifTrue: [ ^ 5 ].
	aValue class == OID ifTrue: [ ^ 7 ].
	(aValue isKindOf: DateAndTime) ifTrue: [ ^ 9 ].
	aValue class == BSONRegex ifTrue: [ ^ 11 ].
	aValue class == MongoTimeStamp ifTrue: [ ^ 17 ].
	aValue isCollection ifTrue: [ ^ 4 ].
	^ 0'.

add value: #'MongoDoubleMatcher class' value: #'isArrayValue:' value: 'types' value:
'isArrayValue: aValue
	^ (self bsonTypeOf: aValue) = 4'.

add value: #'MongoDoubleMatcher class' value: #'bracketOf:' value: 'types' value:
'bracketOf: aValue
	"The comparison order of MongoDB: null, numbers, strings, documents, arrays, binary data, object ids, booleans, dates."
	| code |
	code := self bsonTypeOf: aValue.
	code = 10 ifTrue: [ ^ 0 ].
	(code = 1 or: [ code = 16 or: [ code = 18 ] ]) ifTrue: [ ^ 1 ].
	code = 2 ifTrue: [ ^ 2 ].
	code = 3 ifTrue: [ ^ 3 ].
	code = 4 ifTrue: [ ^ 4 ].
	code = 5 ifTrue: [ ^ 5 ].
	code = 7 ifTrue: [ ^ 6 ].
	code = 8 ifTrue: [ ^ 7 ].
	code = 9 ifTrue: [ ^ 8 ].
	^ 9 + code'.

add value: #'MongoDoubleMatcher class' value: #'compare:to:' value: 'types' value:
'compare: a to: b
	"Answer -1, 0 or 1 for two values of the same bracket."
	| bracket |
	bracket := self bracketOf: a.
	bracket = 0 ifTrue: [ ^ 0 ].
	bracket = 7 ifTrue: [
		^ a = b
			ifTrue: [ 0 ]
			ifFalse: [ a ifTrue: [ 1 ] ifFalse: [ -1 ] ] ].
	bracket = 6 ifTrue: [ ^ self compare: a value to: b value ].
	(bracket = 1 or: [ bracket = 2 or: [ bracket = 8 ] ]) ifTrue: [
		^ a < b
			ifTrue: [ -1 ]
			ifFalse: [ a > b ifTrue: [ 1 ] ifFalse: [ 0 ] ] ].
	^ (self valuesEqual: a to: b) ifTrue: [ 0 ] ifFalse: [ 1 ]'.

add value: #'MongoDoubleMatcher class' value: #'sortCompare:to:' value: 'types' value:
'sortCompare: a to: b
	| bracketA bracketB |
	bracketA := self bracketOf: a.
	bracketB := self bracketOf: b.
	bracketA = bracketB ifFalse: [
		^ bracketA < bracketB ifTrue: [ -1 ] ifFalse: [ 1 ] ].
	^ self compare: a to: b'.

add value: #'MongoDoubleMatcher class' value: #'valuesEqual:to:' value: 'types' value:
'valuesEqual: a to: b
	| bracket |
	bracket := self bracketOf: a.
	bracket = (self bracketOf: b) ifFalse: [ ^ false ].
	bracket = 3 ifTrue: [ ^ self documentsEqual: a to: b ].
	bracket = 4 ifTrue: [
		^ a size = b size and: [
			(1 to: a size) allSatisfy: [ :i | self valuesEqual: (a at: i) to: (b at: i) ] ] ].
	bracket = 5 ifTrue: [ ^ a asByteArray = b asByteArray ].
	^ a = b'.

add value: #'MongoDoubleMatcher class' value: #'documentsEqual:to:' value: 'types' value:
'documentsEqual: a to: b
	| keysA keysB |
	keysA := a keys.
	keysB := b keys.
	keysA size = keysB size ifFalse: [ ^ false ].
	1 to: keysA size do: [ :i |
		(keysA at: i) = (keysB at: i) ifFalse: [ ^ false ].
		(self valuesEqual: (a at: (keysA at: i)) to: (b at: (keysB at: i)))
			ifFalse: [ ^ false ] ].
	^ true'.

add value: #'MongoDoubleMatcher class' value: #'isTruthy:' value: 'types' value:
'isTruthy: aValue
	(aValue == true or: [ aValue == false ]) ifTrue: [ ^ aValue ].
	aValue isNumber ifTrue: [ ^ aValue ~= 0 ].
	^ aValue notNil'.

add value: #'MongoDoubleMatcher class' value: #'valuesAt:in:' value: 'paths' value:
'valuesAt: aPath in: aDocument
	"The values that the dotted path reaches. Arrays are traversed. A missing field gives an empty collection."
	| result |
	result := OrderedCollection new.
	self resolve: (aPath substrings: ''.'') from: 1 in: aDocument into: result.
	^ result'.

add value: #'MongoDoubleMatcher class' value: #'resolve:from:in:into:' value: 'paths' value:
'resolve: parts from: index in: aValue into: result
	| part |
	index > parts size ifTrue: [
		result add: aValue.
		^ self ].
	part := parts at: index.
	aValue isDictionary ifTrue: [
		(aValue includesKey: part) ifTrue: [
			self resolve: parts from: index + 1 in: (aValue at: part) into: result ].
		^ self ].
	(self isArrayValue: aValue) ifTrue: [
		part isAllDigits ifTrue: [
			(part asInteger + 1 between: 1 and: aValue size) ifTrue: [
				self resolve: parts from: index + 1 in: (aValue at: part asInteger + 1) into: result ] ].
		aValue do: [ :each |
			(each isDictionary and: [ each includesKey: part ]) ifTrue: [
				self resolve: parts from: index + 1 in: (each at: part) into: result ] ] ]'.

add value: #'MongoDoubleMatcher class' value: #'document:matches:' value: 'matching' value:
'document: aDocument matches: aQuery
	aQuery keysAndValuesDo: [ :key :condition |
		(self document: aDocument matchesKey: key condition: condition)
			ifFalse: [ ^ false ] ].
	^ true'.

add value: #'MongoDoubleMatcher class' value: #'document:matchesKey:condition:' value: 'matching' value:
'document: aDocument matchesKey: aKey condition: aCondition
	aKey = ''$and'' ifTrue: [
		^ aCondition allSatisfy: [ :each | self document: aDocument matches: each ] ].
	aKey = ''$or'' ifTrue: [
		^ aCondition anySatisfy: [ :each | self document: aDocument matches: each ] ].
	aKey = ''$nor'' ifTrue: [
		^ (aCondition anySatisfy: [ :each | self document: aDocument matches: each ]) not ].
	aKey = ''$where'' ifTrue: [ ^ self document: aDocument satisfiesWhere: aCondition ].
	aKey = ''$comment'' ifTrue: [ ^ true ].
	(aKey beginsWith: ''$'') ifTrue: [
		^ MongoDoubleError signalCode: 2 name: ''BadValue'' message: ''unknown top level operator: '', aKey ].
	^ self values: (self valuesAt: aKey in: aDocument) matchCondition: aCondition'.

add value: #'MongoDoubleMatcher class' value: #'isOperatorDocument:' value: 'matching' value:
'isOperatorDocument: aValue
	^ aValue isDictionary and: [
		aValue notEmpty and: [
			aValue keys allSatisfy: [ :each | each beginsWith: ''$'' ] ] ]'.

add value: #'MongoDoubleMatcher class' value: #'values:matchCondition:' value: 'matching' value:
'values: aValueList matchCondition: aCondition
	(self isOperatorDocument: aCondition) ifTrue: [
		^ self values: aValueList matchOperators: aCondition ].
	^ self values: aValueList equalTo: aCondition'.

add value: #'MongoDoubleMatcher class' value: #'values:equalTo:' value: 'matching' value:
'values: aValueList equalTo: aLiteral
	"A missing field is equal to null. An array field is equal to a value that one of the elements equals."
	aValueList isEmpty ifTrue: [ ^ aLiteral isNil ].
	^ aValueList anySatisfy: [ :each |
		(self valuesEqual: each to: aLiteral) or: [
			(self isArrayValue: each) and: [
				each anySatisfy: [ :element | self valuesEqual: element to: aLiteral ] ] ] ]'.

add value: #'MongoDoubleMatcher class' value: #'values:matchOperators:' value: 'matching' value:
'values: aValueList matchOperators: anOperatorDocument
	anOperatorDocument keysAndValuesDo: [ :operator :operand |
		(self values: aValueList operator: operator operand: operand options: anOperatorDocument)
			ifFalse: [ ^ false ] ].
	^ true'.

add value: #'MongoDoubleMatcher class' value: #'values:operator:operand:options:' value: 'matching' value:
'values: aValueList operator: anOperator operand: anOperand options: aDocument
	anOperator = ''$eq'' ifTrue: [ ^ self values: aValueList equalTo: anOperand ].
	anOperator = ''$ne'' ifTrue: [ ^ (self values: aValueList equalTo: anOperand) not ].
	anOperator = ''$gt'' ifTrue: [
		^ self values: aValueList compare: anOperand satisfying: [ :order | order > 0 ] ].
	anOperator = ''$gte'' ifTrue: [
		^ self values: aValueList compare: anOperand satisfying: [ :order | order >= 0 ] ].
	anOperator = ''$lt'' ifTrue: [
		^ self values: aValueList compare: anOperand satisfying: [ :order | order < 0 ] ].
	anOperator = ''$lte'' ifTrue: [
		^ self values: aValueList compare: anOperand satisfying: [ :order | order <= 0 ] ].
	anOperator = ''$in'' ifTrue: [
		^ anOperand anySatisfy: [ :each | self values: aValueList equalTo: each ] ].
	anOperator = ''$nin'' ifTrue: [
		^ (anOperand anySatisfy: [ :each | self values: aValueList equalTo: each ]) not ].
	anOperator = ''$exists'' ifTrue: [
		^ aValueList notEmpty = (self isTruthy: anOperand) ].
	anOperator = ''$type'' ifTrue: [ ^ self values: aValueList haveType: anOperand ].
	anOperator = ''$size'' ifTrue: [
		^ aValueList anySatisfy: [ :each | (self isArrayValue: each) and: [ each size = anOperand ] ] ].
	anOperator = ''$regex'' ifTrue: [
		^ self values: aValueList matchRegex: anOperand options: (aDocument at: ''$options'' ifAbsent: [ '''' ]) ].
	anOperator = ''$options'' ifTrue: [ ^ true ].
	anOperator = ''$not'' ifTrue: [ ^ (self values: aValueList matchCondition: anOperand) not ].
	anOperator = ''$all'' ifTrue: [
		^ anOperand allSatisfy: [ :each | self values: aValueList equalTo: each ] ].
	anOperator = ''$elemMatch'' ifTrue: [ ^ self values: aValueList matchElement: anOperand ].
	^ MongoDoubleError signalCode: 2 name: ''BadValue'' message: ''unknown operator: '', anOperator'.

add value: #'MongoDoubleMatcher class' value: #'values:compare:satisfying:' value: 'matching' value:
'values: aValueList compare: anOperand satisfying: aBlock
	^ aValueList anySatisfy: [ :each |
		(self item: each compare: anOperand satisfying: aBlock) or: [
			(self isArrayValue: each) and: [
				each anySatisfy: [ :element | self item: element compare: anOperand satisfying: aBlock ] ] ] ]'.

add value: #'MongoDoubleMatcher class' value: #'item:compare:satisfying:' value: 'matching' value:
'item: aValue compare: anOperand satisfying: aBlock
	^ (self bracketOf: aValue) = (self bracketOf: anOperand) and: [
		aBlock value: (self compare: aValue to: anOperand) ]'.

add value: #'MongoDoubleMatcher class' value: #'typeCodesFor:' value: 'matching' value:
'typeCodesFor: aType
	| table |
	aType isNumber ifTrue: [ ^ Array with: aType asInteger ].
	aType = ''number'' ifTrue: [ ^ #(1 16 18) ].
	table := Dictionary new
		at: ''double'' put: 1;
		at: ''string'' put: 2;
		at: ''object'' put: 3;
		at: ''array'' put: 4;
		at: ''binData'' put: 5;
		at: ''objectId'' put: 7;
		at: ''bool'' put: 8;
		at: ''date'' put: 9;
		at: ''null'' put: 10;
		at: ''regex'' put: 11;
		at: ''timestamp'' put: 17;
		at: ''int'' put: 16;
		at: ''long'' put: 18;
		yourself.
	^ Array with: (table
		at: aType
		ifAbsent: [ MongoDoubleError signalCode: 2 name: ''BadValue'' message: ''Unknown type name alias: '', aType printString ])'.

add value: #'MongoDoubleMatcher class' value: #'values:haveType:' value: 'matching' value:
'values: aValueList haveType: aType
	| codes |
	codes := self typeCodesFor: aType.
	^ aValueList anySatisfy: [ :each |
		(codes includes: (self bsonTypeOf: each)) or: [
			(self isArrayValue: each) and: [
				each anySatisfy: [ :element | codes includes: (self bsonTypeOf: element) ] ] ] ]'.

add value: #'MongoDoubleMatcher class' value: #'values:matchRegex:options:' value: 'matching' value:
'values: aValueList matchRegex: aPattern options: anOptionString
	| matcher |
	matcher := (anOptionString includes: $i)
		ifTrue: [ aPattern asRegexIgnoringCase ]
		ifFalse: [ aPattern asRegex ].
	^ aValueList anySatisfy: [ :each |
		(self string: each matches: matcher) or: [
			(self isArrayValue: each) and: [
				each anySatisfy: [ :element | self string: element matches: matcher ] ] ] ]'.

add value: #'MongoDoubleMatcher class' value: #'string:matches:' value: 'matching' value:
'string: aValue matches: aMatcher
	^ aValue isString and: [ (aMatcher matchesIn: aValue) notEmpty ]'.

add value: #'MongoDoubleMatcher class' value: #'values:matchElement:' value: 'matching' value:
'values: aValueList matchElement: aCondition
	^ aValueList anySatisfy: [ :each |
		(self isArrayValue: each) and: [
			each anySatisfy: [ :element |
				(self isOperatorDocument: aCondition)
					ifTrue: [ self values: (OrderedCollection with: element) matchOperators: aCondition ]
					ifFalse: [ element isDictionary and: [ self document: element matches: aCondition ] ] ] ] ]'.

add value: #'MongoDoubleMatcher class' value: #'document:satisfiesWhere:' value: 'javascript' value:
'document: aDocument satisfiesWhere: aScript
	"The stand-in does not run JavaScript. It reads comparisons of this.field with a literal, joined with && and ||."
	(aScript includesSubstring: ''||'') ifTrue: [
		^ (aScript splitOn: ''||'') anySatisfy: [ :part | self document: aDocument satisfiesWhere: part ] ].
	(aScript includesSubstring: ''&&'') ifTrue: [
		^ (aScript splitOn: ''&&'') allSatisfy: [ :part | self document: aDocument satisfiesWhere: part ] ].
	^ self document: aDocument satisfiesComparison: aScript trimmed'.

add value: #'MongoDoubleMatcher class' value: #'document:satisfiesComparison:' value: 'javascript' value:
'document: aDocument satisfiesComparison: aText
	| operator index left right values literal |
	operator := #(''==='' ''!=='' ''=='' ''!='' ''<='' ''>='' ''<'' ''>'' ''='')
		detect: [ :each | aText includesSubstring: each ]
		ifNone: [ nil ].
	operator ifNil: [ ^ self unsupportedScript: aText ].
	index := aText indexOfSubCollection: operator.
	left := (aText copyFrom: 1 to: index - 1) trimmed.
	right := (aText copyFrom: index + operator size to: aText size) trimmed.
	((left beginsWith: ''this.'') and: [ right notEmpty ]) ifFalse: [ ^ self unsupportedScript: aText ].
	values := self valuesAt: (left allButFirst: 5) in: aDocument.
	literal := self literalFromJavaScript: right.
	operator = ''='' ifTrue: [ ^ self isTruthy: literal ].
	(operator = ''=='' or: [ operator = ''==='' ]) ifTrue: [ ^ self values: values equalTo: literal ].
	(operator = ''!='' or: [ operator = ''!=='' ]) ifTrue: [ ^ (self values: values equalTo: literal) not ].
	operator = ''<'' ifTrue: [
		^ self values: values compare: literal satisfying: [ :order | order < 0 ] ].
	operator = ''<='' ifTrue: [
		^ self values: values compare: literal satisfying: [ :order | order <= 0 ] ].
	operator = ''>'' ifTrue: [
		^ self values: values compare: literal satisfying: [ :order | order > 0 ] ].
	^ self values: values compare: literal satisfying: [ :order | order >= 0 ]'.

add value: #'MongoDoubleMatcher class' value: #'literalFromJavaScript:' value: 'javascript' value:
'literalFromJavaScript: aText
	| first |
	first := aText first.
	((first = $") or: [ first = $'' ]) ifTrue: [ ^ aText copyFrom: 2 to: aText size - 1 ].
	aText = ''true'' ifTrue: [ ^ true ].
	aText = ''false'' ifTrue: [ ^ false ].
	aText = ''null'' ifTrue: [ ^ nil ].
	(aText allSatisfy: [ :each | each isDigit or: [ ''.-+eE'' includes: each ] ])
		ifFalse: [ ^ self unsupportedScript: aText ].
	^ aText asNumber'.

add value: #'MongoDoubleMatcher class' value: #'unsupportedScript:' value: 'javascript' value:
'unsupportedScript: aText
	^ MongoDoubleError
		signalCode: 139
		name: ''JSInterpreterFailure''
		message: ''The stand-in server runs only comparisons of this.field with a literal, not: '', aText'.

add value: #'MongoDoubleMatcher class' value: #'sort:by:' value: 'sorting' value:
'sort: aCollection by: aSpecification
	"A stable sort of the documents by the fields of the specification (field -> 1 or -1)."
	| indexed |
	indexed := aCollection withIndexCollect: [ :each :index | each -> index ].
	^ (indexed asSortedCollection: [ :a :b |
		| order |
		order := self compareDocument: a key to: b key by: aSpecification.
		order = 0
			ifTrue: [ a value <= b value ]
			ifFalse: [ order < 0 ] ]) asOrderedCollection collect: [ :each | each key ]'.

add value: #'MongoDoubleMatcher class' value: #'compareDocument:to:by:' value: 'sorting' value:
'compareDocument: a to: b by: aSpecification
	aSpecification keysAndValuesDo: [ :field :direction |
		| order |
		order := self
			sortCompare: (self firstValueAt: field in: a)
			to: (self firstValueAt: field in: b).
		order ~= 0 ifTrue: [
			^ direction < 0 ifTrue: [ order negated ] ifFalse: [ order ] ] ].
	^ 0'.

add value: #'MongoDoubleMatcher class' value: #'firstValueAt:in:' value: 'sorting' value:
'firstValueAt: aField in: aDocument
	| values |
	values := self valuesAt: aField in: aDocument.
	^ values isEmpty ifTrue: [ nil ] ifFalse: [ values first ]'.

add value: #'MongoDoubleMatcher class' value: #'project:with:' value: 'projection' value:
'project: aDocument with: aSpecification
	"Top-level fields only: either a list of fields to keep or a list of fields to drop."
	| keeps result |
	aSpecification isEmpty ifTrue: [ ^ aDocument ].
	aSpecification keys do: [ :each |
		(each includes: $.) ifTrue: [
			^ MongoDoubleError signalCode: 2 name: ''BadValue'' message: ''The stand-in server projects only top-level fields, not: '', each ] ].
	keeps := aSpecification associations anySatisfy: [ :each |
		each key ~= ''_id'' and: [ self isTruthy: each value ] ].
	result := OrderedDictionary new.
	keeps
		ifTrue: [
			(self isTruthy: (aSpecification at: ''_id'' ifAbsent: [ 1 ])) ifTrue: [
				aDocument at: ''_id'' ifPresent: [ :id | result at: ''_id'' put: id ] ].
			aSpecification keysAndValuesDo: [ :field :flag |
				(field ~= ''_id'' and: [ self isTruthy: flag ]) ifTrue: [
					aDocument at: field ifPresent: [ :value | result at: field put: value ] ] ] ]
		ifFalse: [
			aDocument keysAndValuesDo: [ :field :value |
				(aSpecification includesKey: field) ifFalse: [ result at: field put: value ] ] ].
	^ result'.

add value: #'MongoDoubleMatcher class' value: #'applyUpdate:to:' value: 'updating' value:
'applyUpdate: anUpdate to: aDocument
	"A document without operators replaces the document, and keeps the _id. Else $set, $unset and $inc change the fields."
	| result |
	result := OrderedDictionary new.
	(self isOperatorDocument: anUpdate) ifFalse: [
		aDocument at: ''_id'' ifPresent: [ :id | result at: ''_id'' put: id ].
		anUpdate keysAndValuesDo: [ :field :value |
			field = ''_id'' ifFalse: [ result at: field put: value ] ].
		^ result ].
	aDocument keysAndValuesDo: [ :field :value | result at: field put: value ].
	anUpdate keysAndValuesDo: [ :operator :fields |
		self applyOperator: operator fields: fields to: result ].
	^ result'.

add value: #'MongoDoubleMatcher class' value: #'applyOperator:fields:to:' value: 'updating' value:
'applyOperator: anOperator fields: aDocument to: aResult
	anOperator = ''$setOnInsert'' ifTrue: [ ^ self ].
	aDocument keysDo: [ :each |
		(each includes: $.) ifTrue: [
			^ MongoDoubleError signalCode: 2 name: ''BadValue'' message: ''The stand-in server updates only top-level fields, not: '', each ] ].
	anOperator = ''$set'' ifTrue: [
		aDocument keysAndValuesDo: [ :field :value | aResult at: field put: value ].
		^ self ].
	anOperator = ''$unset'' ifTrue: [
		aDocument keysDo: [ :field | aResult removeKey: field ifAbsent: [ nil ] ].
		^ self ].
	anOperator = ''$inc'' ifTrue: [
		aDocument keysAndValuesDo: [ :field :value |
			aResult at: field put: (aResult at: field ifAbsent: [ 0 ]) + value ].
		^ self ].
	^ MongoDoubleError signalCode: 9 name: ''FailedToParse'' message: ''Unknown modifier: '', anOperator'.

add value: #'MongoDoubleMatcher class' value: #'upsertDocumentFor:update:' value: 'updating' value:
'upsertDocumentFor: aQuery update: anUpdate
	"The document that an upsert inserts: the equality fields of the query, changed by the update."
	| base result |
	base := OrderedDictionary new.
	aQuery keysAndValuesDo: [ :field :value |
		((field beginsWith: ''$'') or: [ self isOperatorDocument: value ]) ifFalse: [
			base at: field put: value ] ].
	(self isOperatorDocument: anUpdate) ifFalse: [
		result := OrderedDictionary new.
		(base includesKey: ''_id'') ifTrue: [ result at: ''_id'' put: (base at: ''_id'') ].
		anUpdate keysAndValuesDo: [ :field :value | result at: field put: value ].
		^ result ].
	result := self applyUpdate: anUpdate to: base.
	anUpdate at: ''$setOnInsert'' ifPresent: [ :fields |
		fields keysAndValuesDo: [ :field :value | result at: field put: value ] ].
	^ result'.

add value: #'MongoDoubleCollection class' value: #'named:namespace:' value: 'instance creation' value:
'named: aString namespace: aNamespace
	^ self new setName: aString namespace: aNamespace'.

add value: #'MongoDoubleCollection' value: #'setName:namespace:' value: 'initialization' value:
'setName: aString namespace: aNamespace
	name := aString.
	namespace := aNamespace.
	documents := OrderedCollection new.
	ids := Dictionary new.
	indexes := OrderedCollection new.
	indexes add: (OrderedDictionary new
		at: ''name'' put: ''_id_'';
		at: ''key'' put: (OrderedDictionary new at: ''_id'' put: 1; yourself);
		at: ''unique'' put: true;
		yourself)'.

add value: #'MongoDoubleCollection' value: #'name' value: 'accessing' value:
'name
	^ name'.

add value: #'MongoDoubleCollection' value: #'namespace' value: 'accessing' value:
'namespace
	^ namespace'.

add value: #'MongoDoubleCollection' value: #'documents' value: 'accessing' value:
'documents
	^ documents'.

add value: #'MongoDoubleCollection' value: #'indexes' value: 'accessing' value:
'indexes
	^ indexes'.

add value: #'MongoDoubleCollection' value: #'tupleOf:index:' value: 'indexing' value:
'tupleOf: aDocument index: anIndex
	^ (anIndex at: ''key'') keys collect: [ :field |
		MongoDoubleMatcher firstValueAt: field in: aDocument ]'.

add value: #'MongoDoubleCollection' value: #'tuple:equals:' value: 'indexing' value:
'tuple: a equals: b
	^ (1 to: a size) allSatisfy: [ :i |
		MongoDoubleMatcher valuesEqual: (a at: i) to: (b at: i) ]'.

add value: #'MongoDoubleCollection' value: #'violationMessageFor:index:' value: 'indexing' value:
'violationMessageFor: aDocument index: anIndex
	^ ''E11000 duplicate key error collection: '', namespace, '' index: '', (anIndex at: ''name''), '' dup key: '', (self tupleOf: aDocument index: anIndex) printString'.

add value: #'MongoDoubleCollection' value: #'uniqueViolationFor:ignoring:' value: 'indexing' value:
'uniqueViolationFor: aDocument ignoring: anExistingDocument
	"Answer nil, or the message of the duplicate key error."
	| tuple |
	(ids at: (aDocument at: ''_id'') ifAbsent: [ nil ]) ifNotNil: [ :other |
		other == anExistingDocument ifFalse: [
			^ self violationMessageFor: aDocument index: indexes first ] ].
	indexes allButFirst do: [ :index |
		(index at: ''unique'') ifTrue: [
			tuple := self tupleOf: aDocument index: index.
			documents do: [ :other |
				(other ~~ anExistingDocument and: [
					self tuple: tuple equals: (self tupleOf: other index: index) ]) ifTrue: [
					^ self violationMessageFor: aDocument index: index ] ] ] ].
	^ nil'.

add value: #'MongoDoubleCollection' value: #'addIndexNamed:key:unique:' value: 'indexing' value:
'addIndexNamed: aName key: aKey unique: aBoolean
	"Answer true when the index is new."
	| existing entry |
	existing := indexes detect: [ :each | (each at: ''name'') = aName ] ifNone: [ nil ].
	existing ifNotNil: [
		(MongoDoubleMatcher valuesEqual: (existing at: ''key'') to: aKey) ifFalse: [
			^ MongoDoubleError signalCode: 86 name: ''IndexKeySpecsConflict'' message: ''An index with the same name and a different key already exists: '', aName ].
		^ false ].
	entry := OrderedDictionary new
		at: ''name'' put: aName;
		at: ''key'' put: aKey;
		at: ''unique'' put: aBoolean;
		yourself.
	aBoolean ifTrue: [
		documents do: [ :each |
			| tuple |
			tuple := self tupleOf: each index: entry.
			(documents anySatisfy: [ :other |
				other ~~ each and: [ self tuple: tuple equals: (self tupleOf: other index: entry) ] ]) ifTrue: [
				^ MongoDoubleError signalCode: 11000 name: ''DuplicateKey'' message: (self violationMessageFor: each index: entry) ] ] ].
	indexes add: entry.
	^ true'.

add value: #'MongoDoubleCollection' value: #'withGeneratedIdFor:' value: 'writing' value:
'withGeneratedIdFor: aDocument
	| result |
	result := OrderedDictionary new.
	result at: ''_id'' put: OID nextOID.
	aDocument keysAndValuesDo: [ :field :value | result at: field put: value ].
	^ result'.

add value: #'MongoDoubleCollection' value: #'insert:' value: 'writing' value:
'insert: aDocument
	"Add the document, with an _id when it has none. Answer the stored document."
	| document |
	document := (aDocument includesKey: ''_id'')
		ifTrue: [ aDocument ]
		ifFalse: [ self withGeneratedIdFor: aDocument ].
	(self uniqueViolationFor: document ignoring: nil) ifNotNil: [ :message |
		^ MongoDoubleError signalCode: 11000 name: ''DuplicateKey'' message: message ].
	documents add: document.
	ids at: (document at: ''_id'') put: document.
	^ document'.

add value: #'MongoDoubleCollection' value: #'replace:with:' value: 'writing' value:
'replace: anOldDocument with: aNewDocument
	"Answer nothing. Signal the duplicate key error when the new document breaks a unique index."
	(self uniqueViolationFor: aNewDocument ignoring: anOldDocument) ifNotNil: [ :message |
		^ MongoDoubleError signalCode: 11000 name: ''DuplicateKey'' message: message ].
	documents at: (documents identityIndexOf: anOldDocument) put: aNewDocument.
	ids at: (aNewDocument at: ''_id'') put: aNewDocument'.

add value: #'MongoDoubleCollection' value: #'removeDocuments:' value: 'writing' value:
'removeDocuments: aCollection
	aCollection do: [ :each | ids removeKey: (each at: ''_id'') ifAbsent: [ nil ] ].
	documents := documents reject: [ :each | aCollection identityIncludes: each ]'.

add value: #'MongoDoubleCollection' value: #'select:' value: 'querying' value:
'select: aQuery
	^ documents select: [ :each | MongoDoubleMatcher document: each matches: aQuery ]'.

add value: #'MongoServerDouble class' value: #'current' value: 'lifecycle' value:
'current
	^ Current ifNil: [ Current := self new ]'.

add value: #'MongoServerDouble class' value: #'ensureRunning' value: 'lifecycle' value:
'ensureRunning
	"Start the stand-in on 127.0.0.1:27017 when it does not run. Called by setUp of the tests."
	^ self current ensureRunning'.

add value: #'MongoServerDouble class' value: #'stop' value: 'lifecycle' value:
'stop
	Current ifNotNil: [ :server | server stop ]'.

add value: #'MongoServerDouble' value: #'initialize' value: 'initialization' value:
'initialize
	super initialize.
	connections := Set new.
	conversations := Dictionary new.
	users := Dictionary new.
	nextCursorId := 16r100000000.
	nextRequestId := 1000.
	self addUser: ''pharounittest'' password: ''test'' database: ''test''.
	self reset'.

add value: #'MongoServerDouble' value: #'reset' value: 'initialization' value:
'reset
	"Drop all data. The users stay."
	databases := Dictionary new.
	cursors := Dictionary new.
	conversations := Dictionary new'.

add value: #'MongoServerDouble' value: #'addUser:password:database:' value: 'initialization' value:
'addUser: aName password: aPassword database: aDatabase
	"SCRAM-SHA-1 credentials, with the same derivation that the client uses (MongoPBKDF2).
	 The iteration count is low so that the PBKDF2 of the client stays fast in Smalltalk."
	| salt salted clientKey |
	salt := #(12 34 56 78 90 123 45 67 89 10 111 22 33 44 55 66) asByteArray.
	salted := MongoPBKDF2 new
		hashFunction: SHA1;
		password: (MD5 hashMessage: aName , '':mongo:'' , aPassword) hex;
		salt: salt;
		iterations: 1000;
		length: 20;
		deriveKey.
	clientKey := (HMAC on: SHA1 new) key: salted; digestMessage: ''Client Key'' asByteArray.
	(users at: aDatabase ifAbsentPut: [ Dictionary new ])
		at: aName
		put: (OrderedDictionary new
			at: ''salt'' put: salt;
			at: ''iterations'' put: 1000;
			at: ''storedKey'' put: (SHA1 hashMessage: clientKey);
			at: ''serverKey'' put: ((HMAC on: SHA1 new) key: salted; digestMessage: ''Server Key'' asByteArray);
			yourself)'.

add value: #'MongoServerDouble' value: #'port' value: 'lifecycle' value:
'port
	^ 27017'.

add value: #'MongoServerDouble' value: #'isRunning' value: 'lifecycle' value:
'isRunning
	^ listener notNil and: [
		listener isValid and: [ acceptor notNil and: [ acceptor isTerminated not ] ] ]'.

add value: #'MongoServerDouble' value: #'ensureRunning' value: 'lifecycle' value:
'ensureRunning
	"Without an open connection the data of an earlier test is dropped, as a new server would have no data."
	self isRunning ifTrue: [
		connections isEmpty ifTrue: [ self reset ].
		^ self ].
	self start'.

add value: #'MongoServerDouble' value: #'start' value: 'lifecycle' value:
'start
	self stop.
	self reset.
	listener := ZnNetworkingUtils serverSocketOn: self port interface: #[127 0 0 1].
	acceptor := self forkOutsideTest: [ self acceptLoopOn: listener ] named: ''MongoServerDouble acceptor'''.

add value: #'MongoServerDouble' value: #'forkOutsideTest:named:' value: 'lifecycle' value:
'forkOutsideTest: aBlock named: aName
	"The process runs in the default execution environment. A process that a test forks is in the test
	 environment, and SUnit terminates it at the end of the test: the stand-in would stop after one test."
	| process |
	DefaultExecutionEnvironment beActiveDuring: [
		process := aBlock forkAt: Processor userSchedulingPriority + 1 named: aName ].
	^ process'.

add value: #'MongoServerDouble' value: #'stop' value: 'lifecycle' value:
'stop
	acceptor ifNotNil: [ acceptor terminate ].
	acceptor := nil.
	listener ifNotNil: [ listener closeAndDestroy ].
	listener := nil.
	connections do: [ :each | [ each close ] on: NetworkError do: [ :error | nil ] ].
	connections := Set new'.

add value: #'MongoServerDouble' value: #'acceptLoopOn:' value: 'connections' value:
'acceptLoopOn: aServerSocket
	[ [ aServerSocket isValid ] whileTrue: [
		| socket |
		socket := aServerSocket waitForAcceptFor: 1.
		socket ifNotNil: [
			self forkOutsideTest: [ self handleConnection: socket ] named: ''MongoServerDouble connection'' ] ] ]
		on: Error
		do: [ :error | self logFailure: error ]'.

add value: #'MongoServerDouble' value: #'logFailure:' value: 'connections' value:
'logFailure: anError
	Stdio stderr
		nextPutAll: ''MongoServerDouble: '', anError class name, '': '', (anError messageText ifNil: [ anError description ]);
		lf;
		flush'.

add value: #'MongoServerDouble' value: #'handleConnection:' value: 'connections' value:
'handleConnection: aSocket
	| stream |
	aSocket setOption: ''TCP_NODELAY'' value: 1.
	stream := SocketStream on: aSocket.
	stream binary.
	stream timeout: 7200.
	connections add: stream.
	[ [ [ [ self serveRequestOn: stream ] whileTrue ]
		on: NetworkError
		do: [ :error | nil ] ]
		on: Error
		do: [ :error | self logFailure: error ] ]
		ensure: [
			connections remove: stream ifAbsent: [ nil ].
			[ stream close ] on: NetworkError do: [ :error | nil ] ]'.

add value: #'MongoServerDouble' value: #'uint32:at:' value: 'connections' value:
'uint32: aByteArray at: anIndex
	^ (aByteArray at: anIndex)
		+ ((aByteArray at: anIndex + 1) bitShift: 8)
		+ ((aByteArray at: anIndex + 2) bitShift: 16)
		+ ((aByteArray at: anIndex + 3) bitShift: 24)'.

add value: #'MongoServerDouble' value: #'serveRequestOn:' value: 'connections' value:
'serveRequestOn: aStream
	"Read one message, answer it. Answer false when the connection ends or the message is not OP_MSG."
	| header length body reply |
	header := aStream next: 16.
	header size < 16 ifTrue: [ ^ false ].
	length := self uint32: header at: 1.
	body := aStream next: length - 16.
	body size < (length - 16) ifTrue: [ ^ false ].
	(self uint32: header at: 13) = 2013 ifFalse: [ ^ false ].
	reply := self replyForMessageBody: body.
	self writeReply: reply responseTo: (self uint32: header at: 5) on: aStream.
	^ true'.

add value: #'MongoServerDouble' value: #'encode:' value: 'connections' value:
'encode: aDocument
	^ BSON writer nextOrderedDictionaryPut: aDocument; contents'.

add value: #'MongoServerDouble' value: #'writeReply:responseTo:on:' value: 'connections' value:
'writeReply: aDocument responseTo: anId on: aStream
	| body out |
	body := [ self encode: aDocument ]
		on: Error
		do: [ :error |
			self encode: (self
				errorDocument: 8000
				name: ''StandInFailure''
				message: ''stand-in server cannot write the reply: '', error class name, '': '', (error messageText ifNil: [ error description ])) ].
	nextRequestId := nextRequestId + 1.
	out := LittleEndianStream on: ByteArray new writeStream.
	out
		nextInt32Put: body size + 21;
		nextInt32Put: nextRequestId;
		nextInt32Put: anId;
		nextInt32Put: 2013;
		nextInt32Put: 0;
		nextBytePut: 0;
		nextBytesPut: body.
	aStream nextPutAll: out contents.
	aStream flush'.

add value: #'MongoServerDouble' value: #'commandFromBody:' value: 'protocol' value:
'commandFromBody: aBody
	"OP_MSG: flag bits, then sections. Section kind 0 is the command, kind 1 a sequence of documents of one field."
	| flags stream reader command sequences |
	flags := self uint32: aBody at: 1.
	stream := ((flags bitAnd: 1) = 1 ifTrue: [ aBody allButLast: 4 ] ifFalse: [ aBody ]) readStream.
	stream skip: 4.
	reader := LittleEndianStream on: stream.
	sequences := OrderedDictionary new.
	[ stream atEnd ] whileFalse: [
		| kind |
		kind := stream next.
		kind = 0 ifTrue: [ command := MongoDoubleBSON read: stream ].
		kind = 1 ifTrue: [
			| start size identifier documents |
			start := stream position.
			size := reader nextUInt32.
			identifier := (stream upTo: 0) asString.
			documents := OrderedCollection new.
			[ stream position < (start + size) ] whileTrue: [
				documents add: (MongoDoubleBSON read: stream) ].
			sequences at: identifier put: documents asArray ] ].
	sequences keysAndValuesDo: [ :field :documents | command at: field put: documents ].
	^ command'.

add value: #'MongoServerDouble' value: #'replyForMessageBody:' value: 'protocol' value:
'replyForMessageBody: aBody
	| command name selector |
	^ [ [
		command := self commandFromBody: aBody.
		name := command keys first.
		selector := self commandTable at: name asLowercase ifAbsent: [ nil ].
		selector isNil
			ifTrue: [ self errorDocument: 59 name: ''CommandNotFound'' message: ''no such command: '''''', name, '''''''' ]
			ifFalse: [ self perform: selector with: command with: (command at: ''$db'' ifAbsent: [ ''admin'' ]) ] ]
			on: MongoDoubleError
			do: [ :error | self errorDocument: error code name: error codeName message: error messageText ] ]
		on: Error
		do: [ :error |
			self
				errorDocument: 8000
				name: ''StandInFailure''
				message: ''stand-in server failure: '', error class name, '': '', (error messageText ifNil: [ error description ]) ]'.

add value: #'MongoServerDouble' value: #'commandTable' value: 'protocol' value:
'commandTable
	^ Dictionary new
		at: ''ismaster'' put: #cmdIsmaster:db:;
		at: ''hello'' put: #cmdIsmaster:db:;
		at: ''ping'' put: #cmdPing:db:;
		at: ''buildinfo'' put: #cmdBuildinfo:db:;
		at: ''listdatabases'' put: #cmdListdatabases:db:;
		at: ''listcollections'' put: #cmdListcollections:db:;
		at: ''create'' put: #cmdCreate:db:;
		at: ''drop'' put: #cmdDrop:db:;
		at: ''dropdatabase'' put: #cmdDropdatabase:db:;
		at: ''insert'' put: #cmdInsert:db:;
		at: ''find'' put: #cmdFind:db:;
		at: ''getmore'' put: #cmdGetmore:db:;
		at: ''killcursors'' put: #cmdKillcursors:db:;
		at: ''update'' put: #cmdUpdate:db:;
		at: ''delete'' put: #cmdDelete:db:;
		at: ''count'' put: #cmdCount:db:;
		at: ''createindexes'' put: #cmdCreateindexes:db:;
		at: ''validate'' put: #cmdValidate:db:;
		at: ''saslstart'' put: #cmdSaslstart:db:;
		at: ''saslcontinue'' put: #cmdSaslcontinue:db:;
		yourself'.

add value: #'MongoServerDouble' value: #'errorDocument:name:message:' value: 'protocol' value:
'errorDocument: aCode name: aName message: aMessage
	^ OrderedDictionary new
		at: ''ok'' put: 0.0;
		at: ''errmsg'' put: aMessage;
		at: ''code'' put: aCode;
		at: ''codeName'' put: aName;
		yourself'.

add value: #'MongoServerDouble' value: #'okDocument' value: 'protocol' value:
'okDocument
	^ OrderedDictionary new at: ''ok'' put: 1.0; yourself'.

add value: #'MongoServerDouble' value: #'checkFieldsOf:allowed:' value: 'protocol' value:
'checkFieldsOf: aCommand allowed: anArray
	"A field that the stand-in does not know is an error, as in the real server: a field that is ignored would hide a defect."
	aCommand keysDo: [ :each |
		(anArray includes: each) ifFalse: [
			^ MongoDoubleError
				signalCode: 40415
				name: ''Location40415''
				message: ''BSON field '''''', aCommand keys first, ''.'', each, '''''' is an unknown field.'' ] ]'.

add value: #'MongoServerDouble' value: #'checkReadConcern:' value: 'protocol' value:
'checkReadConcern: aCommand
	aCommand at: ''readConcern'' ifPresent: [ :concern |
		(#(''local'' ''available'' ''majority'' ''linearizable'' ''snapshot'' ) includes: (concern at: ''level'' ifAbsent: [ nil ])) ifFalse: [
			^ MongoDoubleError signalCode: 9 name: ''FailedToParse'' message: ''unknown readConcern level: '', (concern at: ''level'' ifAbsent: [ nil ]) printString ] ]'.

add value: #'MongoServerDouble' value: #'writeConcernErrorFor:' value: 'protocol' value:
'writeConcernErrorFor: aCommand
	"Nil, or the writeConcernError document that a standalone server answers."
	| concern w |
	concern := aCommand at: ''writeConcern'' ifAbsent: [ ^ nil ].
	concern keysDo: [ :each |
		(#(''w'' ''j'' ''wtimeout'' ''fsync'' ''provenance'') includes: each) ifFalse: [
			^ MongoDoubleError signalCode: 72 name: ''InvalidOptions'' message: ''unrecognized write concern field: '', each ] ].
	w := concern at: ''w'' ifAbsent: [ 1 ].
	(w isNumber and: [ w > 1 ]) ifFalse: [ ^ nil ].
	^ OrderedDictionary new
		at: ''code'' put: 2;
		at: ''codeName'' put: ''BadValue'';
		at: ''errmsg'' put: ''cannot use w > 1 when a host is not replicated'';
		yourself'.

add value: #'MongoServerDouble' value: #'existingCollection:in:' value: 'storage' value:
'existingCollection: aCollectionName in: aDatabaseName
	^ (databases at: aDatabaseName ifAbsent: [ ^ nil ]) at: aCollectionName ifAbsent: [ nil ]'.

add value: #'MongoServerDouble' value: #'ensureCollection:in:' value: 'storage' value:
'ensureCollection: aCollectionName in: aDatabaseName
	^ (databases at: aDatabaseName ifAbsentPut: [ Dictionary new ])
		at: aCollectionName
		ifAbsentPut: [ MongoDoubleCollection named: aCollectionName namespace: aDatabaseName, ''.'', aCollectionName ]'.

add value: #'MongoServerDouble' value: #'dropCollection:in:' value: 'storage' value:
'dropCollection: aCollectionName in: aDatabaseName
	"Answer the dropped collection, or nil when there was none. A database without collections is gone."
	| collections collection |
	collections := databases at: aDatabaseName ifAbsent: [ ^ nil ].
	collection := collections removeKey: aCollectionName ifAbsent: [ ^ nil ].
	collections isEmpty ifTrue: [ databases removeKey: aDatabaseName ].
	^ collection'.

add value: #'MongoServerDouble' value: #'cmdIsmaster:db:' value: 'commands' value:
'cmdIsmaster: aCommand db: aName
	^ OrderedDictionary new
		at: ''ismaster'' put: true;
		at: ''isWritablePrimary'' put: true;
		at: ''maxBsonObjectSize'' put: 16777216;
		at: ''maxMessageSizeBytes'' put: 48000000;
		at: ''maxWriteBatchSize'' put: 100000;
		at: ''localTime'' put: DateAndTime now;
		at: ''minWireVersion'' put: 0;
		at: ''maxWireVersion'' put: 17;
		at: ''readOnly'' put: false;
		at: ''ok'' put: 1.0;
		yourself'.

add value: #'MongoServerDouble' value: #'cmdPing:db:' value: 'commands' value:
'cmdPing: aCommand db: aName
	^ self okDocument'.

add value: #'MongoServerDouble' value: #'cmdBuildinfo:db:' value: 'commands' value:
'cmdBuildinfo: aCommand db: aName
	^ OrderedDictionary new
		at: ''version'' put: ''7.0.0'';
		at: ''versionArray'' put: #(7 0 0 0);
		at: ''gitVersion'' put: ''in-image stand-in'';
		at: ''ok'' put: 1.0;
		yourself'.

add value: #'MongoServerDouble' value: #'cmdListdatabases:db:' value: 'commands' value:
'cmdListdatabases: aCommand db: aName
	| names list |
	names := (databases keys asSet addAll: #(''admin'' ''config'' ''local''); yourself) asSortedCollection asArray.
	list := names collect: [ :each |
		OrderedDictionary new
			at: ''name'' put: each;
			at: ''sizeOnDisk'' put: 8192;
			at: ''empty'' put: (databases includesKey: each) not;
			yourself ].
	^ OrderedDictionary new
		at: ''databases'' put: list;
		at: ''totalSize'' put: 8192 * names size;
		at: ''ok'' put: 1.0;
		yourself'.

add value: #'MongoServerDouble' value: #'cmdListcollections:db:' value: 'commands' value:
'cmdListcollections: aCommand db: aName
	| collections names batch |
	self checkFieldsOf: aCommand allowed: #(''listCollections'' ''nameOnly'' ''filter'' ''authorizedCollections'' ''cursor'' ''$db'' ''lsid'' ''comment'').
	collections := databases at: aName ifAbsent: [ Dictionary new ].
	names := collections keys asSortedCollection asArray.
	batch := names collect: [ :each |
		(aCommand at: ''nameOnly'' ifAbsent: [ false ])
			ifTrue: [ OrderedDictionary new at: ''name'' put: each; at: ''type'' put: ''collection''; yourself ]
			ifFalse: [
				OrderedDictionary new
					at: ''name'' put: each;
					at: ''type'' put: ''collection'';
					at: ''options'' put: OrderedDictionary new;
					at: ''info'' put: (OrderedDictionary new at: ''readOnly'' put: false; yourself);
					yourself ] ].
	batch := batch select: [ :each |
		MongoDoubleMatcher document: each matches: (aCommand at: ''filter'' ifAbsent: [ OrderedDictionary new ]) ].
	^ self cursorReply: batch id: 0 namespace: aName, ''.$cmd.listCollections'' batchKey: ''firstBatch'''.

add value: #'MongoServerDouble' value: #'cmdCreate:db:' value: 'commands' value:
'cmdCreate: aCommand db: aName
	| collectionName |
	self checkFieldsOf: aCommand allowed: #(''create'' ''capped'' ''size'' ''max'' ''autoIndexId'' ''validator'' ''writeConcern'' ''comment'' ''$db'' ''lsid'').
	collectionName := aCommand at: ''create''.
	(self existingCollection: collectionName in: aName) ifNotNil: [
		^ MongoDoubleError
			signalCode: 48
			name: ''NamespaceExists''
			message: ''Collection '', aName, ''.'', collectionName, '' already exists.'' ].
	self ensureCollection: collectionName in: aName.
	^ self okDocument'.

add value: #'MongoServerDouble' value: #'cmdDrop:db:' value: 'commands' value:
'cmdDrop: aCommand db: aName
	| collectionName collection |
	collectionName := aCommand at: ''drop''.
	collection := self dropCollection: collectionName in: aName.
	collection ifNil: [
		^ MongoDoubleError signalCode: 26 name: ''NamespaceNotFound'' message: ''ns not found'' ].
	^ OrderedDictionary new
		at: ''nIndexesWas'' put: collection indexes size;
		at: ''ns'' put: collection namespace;
		at: ''ok'' put: 1.0;
		yourself'.

add value: #'MongoServerDouble' value: #'cmdDropdatabase:db:' value: 'commands' value:
'cmdDropdatabase: aCommand db: aName
	databases removeKey: aName ifAbsent: [ nil ].
	^ OrderedDictionary new
		at: ''dropped'' put: aName;
		at: ''ok'' put: 1.0;
		yourself'.

add value: #'MongoServerDouble' value: #'cmdInsert:db:' value: 'commands' value:
'cmdInsert: aCommand db: aName
	| collectionName documents collection inserted errors ordered concernError reply |
	self checkFieldsOf: aCommand allowed: #(''insert'' ''documents'' ''ordered'' ''writeConcern'' ''bypassDocumentValidation'' ''comment'' ''$db'' ''lsid'').
	collectionName := aCommand at: ''insert''.
	documents := aCommand at: ''documents'' ifAbsent: [
		^ MongoDoubleError signalCode: 40414 name: ''Location40414'' message: ''BSON field ''''insert.documents'''' is missing but a required field'' ].
	(documents isArray and: [ documents notEmpty ]) ifFalse: [
		^ MongoDoubleError signalCode: 16 name: ''InvalidLength'' message: ''Write batch sizes must be between 1 and 100000.'' ].
	concernError := self writeConcernErrorFor: aCommand.
	ordered := self isOrdered: aCommand.
	collection := self ensureCollection: collectionName in: aName.
	inserted := 0.
	errors := OrderedCollection new.
	documents withIndexDo: [ :each :index |
		(errors isEmpty or: [ ordered not ]) ifTrue: [
			[ collection insert: each. inserted := inserted + 1 ]
				on: MongoDoubleError
				do: [ :error | errors add: (self writeErrorAt: index - 1 error: error) ] ] ].
	reply := self writeResult: inserted errors: errors.
	concernError ifNotNil: [ reply at: ''writeConcernError'' put: concernError ].
	^ reply'.

add value: #'MongoServerDouble' value: #'isOrdered:' value: 'commands' value:
'isOrdered: aCommand
	^ MongoDoubleMatcher isTruthy: (aCommand at: ''ordered'' ifAbsent: [ true ])'.

add value: #'MongoServerDouble' value: #'writeErrorAt:error:' value: 'commands' value:
'writeErrorAt: anIndex error: anError
	^ OrderedDictionary new
		at: ''index'' put: anIndex;
		at: ''code'' put: anError code;
		at: ''errmsg'' put: anError messageText;
		yourself'.

add value: #'MongoServerDouble' value: #'writeResult:errors:' value: 'commands' value:
'writeResult: aCount errors: anErrorList
	| reply |
	reply := OrderedDictionary new.
	reply at: ''n'' put: aCount.
	anErrorList isEmpty ifFalse: [ reply at: ''writeErrors'' put: anErrorList asArray ].
	reply at: ''ok'' put: 1.0.
	^ reply'.

add value: #'MongoServerDouble' value: #'cursorReply:id:namespace:batchKey:' value: 'commands' value:
'cursorReply: aDocumentList id: anId namespace: aNamespace batchKey: aKey
	^ OrderedDictionary new
		at: ''cursor'' put: (OrderedDictionary new
			at: aKey put: aDocumentList asArray;
			at: ''id'' put: anId;
			at: ''ns'' put: aNamespace;
			yourself);
		at: ''ok'' put: 1.0;
		yourself'.

add value: #'MongoServerDouble' value: #'cmdFind:db:' value: 'commands' value:
'cmdFind: aCommand db: aName
	"The first batch has 101 documents, as in the real server, unless batchSize says another size."
	| collectionName collection matched skip limit batchSize firstBatch rest id |
	self checkFieldsOf: aCommand allowed: #(''find'' ''filter'' ''sort'' ''projection'' ''skip'' ''limit'' ''batchSize'' ''singleBatch'' ''readConcern'' ''maxTimeMS'' ''comment'' ''hint'' ''$db'' ''lsid'' ''$readPreference'').
	self checkReadConcern: aCommand.
	collectionName := aCommand at: ''find''.
	collection := self existingCollection: collectionName in: aName.
	collection ifNil: [
		^ self cursorReply: #() id: 0 namespace: aName, ''.'', collectionName batchKey: ''firstBatch'' ].
	matched := collection select: (aCommand at: ''filter'' ifAbsent: [ OrderedDictionary new ]).
	aCommand at: ''sort'' ifPresent: [ :specification |
		matched := MongoDoubleMatcher sort: matched by: specification ].
	skip := aCommand at: ''skip'' ifAbsent: [ 0 ].
	matched := matched allButFirst: (skip min: matched size).
	limit := aCommand at: ''limit'' ifAbsent: [ 0 ].
	limit > 0 ifTrue: [ matched := matched first: (limit min: matched size) ].
	aCommand at: ''projection'' ifPresent: [ :specification |
		matched := matched collect: [ :each | MongoDoubleMatcher project: each with: specification ] ].
	batchSize := aCommand at: ''batchSize'' ifAbsent: [ 101 ].
	firstBatch := matched first: (batchSize min: matched size).
	rest := matched allButFirst: firstBatch size.
	id := (rest isEmpty or: [ MongoDoubleMatcher isTruthy: (aCommand at: ''singleBatch'' ifAbsent: [ false ]) ])
		ifTrue: [ 0 ]
		ifFalse: [ self newCursorOn: collection namespace documents: rest ].
	^ self cursorReply: firstBatch id: id namespace: collection namespace batchKey: ''firstBatch'''.

add value: #'MongoServerDouble' value: #'newCursorOn:documents:' value: 'commands' value:
'newCursorOn: aNamespace documents: aDocumentList
	| id |
	nextCursorId := nextCursorId + 1.
	id := nextCursorId.
	cursors at: id put: (Array with: aNamespace with: aDocumentList).
	^ id'.

add value: #'MongoServerDouble' value: #'cmdGetmore:db:' value: 'commands' value:
'cmdGetmore: aCommand db: aName
	"A batchSize of 0 or none means all the documents that remain. maxTimeMS is an error: the cursors of find are not awaitData cursors."
	| id cursor batchSize remaining batch rest |
	self checkFieldsOf: aCommand allowed: #(''getMore'' ''collection'' ''batchSize'' ''maxTimeMS'' ''comment'' ''$db'' ''lsid'').
	id := aCommand at: ''getMore''.
	cursor := cursors at: id ifAbsent: [
		^ MongoDoubleError signalCode: 43 name: ''CursorNotFound'' message: ''cursor id '', id printString, '' not found'' ].
	(aCommand includesKey: ''maxTimeMS'') ifTrue: [
		^ MongoDoubleError
			signalCode: 2
			name: ''BadValue''
			message: ''cannot set maxTimeMS on getMore command for a non-awaitData cursor'' ].
	remaining := cursor last.
	batchSize := aCommand at: ''batchSize'' ifAbsent: [ 0 ].
	batchSize = 0 ifTrue: [ batchSize := remaining size ].
	batch := remaining first: (batchSize min: remaining size).
	rest := remaining allButFirst: batch size.
	rest isEmpty
		ifTrue: [ cursors removeKey: id. id := 0 ]
		ifFalse: [ cursors at: id put: (Array with: cursor first with: rest) ].
	^ self cursorReply: batch id: id namespace: cursor first batchKey: ''nextBatch'''.

add value: #'MongoServerDouble' value: #'cmdKillcursors:db:' value: 'commands' value:
'cmdKillcursors: aCommand db: aName
	| killed missing |
	killed := OrderedCollection new.
	missing := OrderedCollection new.
	(aCommand at: ''cursors'' ifAbsent: [ #() ]) do: [ :each |
		(cursors removeKey: each ifAbsent: [ nil ])
			ifNil: [ missing add: each ]
			ifNotNil: [ killed add: each ] ].
	^ OrderedDictionary new
		at: ''cursorsKilled'' put: killed asArray;
		at: ''cursorsNotFound'' put: missing asArray;
		at: ''cursorsAlive'' put: #();
		at: ''cursorsUnknown'' put: #();
		at: ''ok'' put: 1.0;
		yourself'.

add value: #'MongoServerDouble' value: #'cmdCount:db:' value: 'commands' value:
'cmdCount: aCommand db: aName
	| collection matched skip limit |
	self checkFieldsOf: aCommand allowed: #(''count'' ''query'' ''limit'' ''skip'' ''hint'' ''readConcern'' ''maxTimeMS'' ''comment'' ''$db'' ''lsid'' ''$readPreference'').
	self checkReadConcern: aCommand.
	collection := self existingCollection: (aCommand at: ''count'') in: aName.
	collection ifNil: [ ^ OrderedDictionary new at: ''n'' put: 0; at: ''ok'' put: 1.0; yourself ].
	matched := collection select: (aCommand at: ''query'' ifAbsent: [ OrderedDictionary new ]).
	skip := aCommand at: ''skip'' ifAbsent: [ 0 ].
	matched := matched allButFirst: (skip min: matched size).
	limit := aCommand at: ''limit'' ifAbsent: [ 0 ].
	limit > 0 ifTrue: [ matched := matched first: (limit min: matched size) ].
	^ OrderedDictionary new
		at: ''n'' put: matched size;
		at: ''ok'' put: 1.0;
		yourself'.

add value: #'MongoServerDouble' value: #'cmdUpdate:db:' value: 'commands' value:
'cmdUpdate: aCommand db: aName
	| collectionName updates collection matchedCount modifiedCount upserted errors ordered concernError reply |
	self checkFieldsOf: aCommand allowed: #(''update'' ''updates'' ''ordered'' ''writeConcern'' ''bypassDocumentValidation'' ''comment'' ''$db'' ''lsid'').
	collectionName := aCommand at: ''update''.
	updates := aCommand at: ''updates''.
	concernError := self writeConcernErrorFor: aCommand.
	ordered := self isOrdered: aCommand.
	collection := self existingCollection: collectionName in: aName.
	matchedCount := 0.
	modifiedCount := 0.
	upserted := OrderedCollection new.
	errors := OrderedCollection new.
	updates withIndexDo: [ :spec :index |
		(errors isEmpty or: [ ordered not ]) ifTrue: [
			self checkFieldsOf: spec allowed: #(''q'' ''u'' ''upsert'' ''multi'' ''collation'' ''arrayFilters'' ''hint'' ''c'').
			[ | matches query |
			query := spec at: ''q''.
			matches := collection ifNil: [ OrderedCollection new ] ifNotNil: [ collection select: query ].
			(MongoDoubleMatcher isTruthy: (spec at: ''multi'' ifAbsent: [ false ])) ifFalse: [
				matches := matches first: (1 min: matches size) ].
			matches isEmpty
				ifTrue: [
					(MongoDoubleMatcher isTruthy: (spec at: ''upsert'' ifAbsent: [ false ])) ifTrue: [
						| document |
						collection := self ensureCollection: collectionName in: aName.
						document := collection insert: (MongoDoubleMatcher upsertDocumentFor: query update: (spec at: ''u'')).
						upserted add: (OrderedDictionary new
							at: ''index'' put: index - 1;
							at: ''_id'' put: (document at: ''_id'');
							yourself) ] ]
				ifFalse: [
					matches do: [ :each |
						collection replace: each with: (MongoDoubleMatcher applyUpdate: (spec at: ''u'') to: each).
						matchedCount := matchedCount + 1.
						modifiedCount := modifiedCount + 1 ] ] ]
				on: MongoDoubleError
				do: [ :error | errors add: (self writeErrorAt: index - 1 error: error) ] ] ].
	reply := self writeResult: matchedCount + upserted size errors: errors.
	reply removeKey: ''ok''.
	reply at: ''nModified'' put: modifiedCount.
	upserted isEmpty ifFalse: [ reply at: ''upserted'' put: upserted asArray ].
	concernError ifNotNil: [ reply at: ''writeConcernError'' put: concernError ].
	reply at: ''ok'' put: 1.0.
	^ reply'.

add value: #'MongoServerDouble' value: #'cmdDelete:db:' value: 'commands' value:
'cmdDelete: aCommand db: aName
	| collection deleted errors concernError reply |
	self checkFieldsOf: aCommand allowed: #(''delete'' ''deletes'' ''ordered'' ''writeConcern'' ''comment'' ''$db'' ''lsid'').
	concernError := self writeConcernErrorFor: aCommand.
	collection := self existingCollection: (aCommand at: ''delete'') in: aName.
	deleted := 0.
	errors := OrderedCollection new.
	collection ifNotNil: [
		(aCommand at: ''deletes'') do: [ :spec | | matches |
			self checkFieldsOf: spec allowed: #(''q'' ''limit'' ''collation'' ''hint'').
			matches := collection select: (spec at: ''q'').
			(spec at: ''limit'') = 1 ifTrue: [ matches := matches first: (1 min: matches size) ].
			collection removeDocuments: matches.
			deleted := deleted + matches size ] ].
	reply := self writeResult: deleted errors: errors.
	concernError ifNotNil: [ reply at: ''writeConcernError'' put: concernError ].
	^ reply'.

add value: #'MongoServerDouble' value: #'cmdCreateindexes:db:' value: 'commands' value:
'cmdCreateindexes: aCommand db: aName
	| collectionName created collection before |
	self checkFieldsOf: aCommand allowed: #(''createIndexes'' ''indexes'' ''writeConcern'' ''commitQuorum'' ''comment'' ''$db'' ''lsid'').
	collectionName := aCommand at: ''createIndexes''.
	created := (self existingCollection: collectionName in: aName) isNil.
	collection := self ensureCollection: collectionName in: aName.
	before := collection indexes size.
	(aCommand at: ''indexes'') do: [ :spec |
		collection
			addIndexNamed: (spec at: ''name'')
			key: (spec at: ''key'')
			unique: (MongoDoubleMatcher isTruthy: (spec at: ''unique'' ifAbsent: [ false ])) ].
	^ OrderedDictionary new
		at: ''createdCollectionAutomatically'' put: created;
		at: ''numIndexesBefore'' put: before;
		at: ''numIndexesAfter'' put: collection indexes size;
		at: ''ok'' put: 1.0;
		yourself'.

add value: #'MongoServerDouble' value: #'cmdValidate:db:' value: 'commands' value:
'cmdValidate: aCommand db: aName
	| collection |
	collection := self existingCollection: (aCommand at: ''validate'') in: aName.
	collection ifNil: [
		^ MongoDoubleError signalCode: 26 name: ''NamespaceNotFound'' message: ''Collection '''''', aName, ''.'', (aCommand at: ''validate''), '''''' does not exist to validate.'' ].
	^ OrderedDictionary new
		at: ''ns'' put: collection namespace;
		at: ''nInvalidDocuments'' put: 0;
		at: ''nrecords'' put: collection documents size;
		at: ''nIndexes'' put: collection indexes size;
		at: ''valid'' put: true;
		at: ''warnings'' put: #();
		at: ''errors'' put: #();
		at: ''ok'' put: 1.0;
		yourself'.

add value: #'MongoServerDouble' value: #'authenticationFailed' value: 'authentication' value:
'authenticationFailed
	^ MongoDoubleError signalCode: 18 name: ''AuthenticationFailed'' message: ''Authentication failed.'''.

add value: #'MongoServerDouble' value: #'cmdSaslstart:db:' value: 'authentication' value:
'cmdSaslstart: aCommand db: aName
	"SCRAM-SHA-1, as in RFC 5802 with the MongoDB variants. The first message of the client is n,,n=<user>,r=<nonce>."
	| payload bare fields user clientNonce credentials nonce serverFirst id |
	(aCommand at: ''mechanism'') = ''SCRAM-SHA-1'' ifFalse: [
		^ MongoDoubleError signalCode: 2 name: ''BadValue'' message: ''The stand-in server supports only SCRAM-SHA-1.'' ].
	payload := (aCommand at: ''payload'') asString.
	(payload beginsWith: ''n,,'') ifFalse: [ ^ self authenticationFailed ].
	bare := payload allButFirst: 3.
	fields := bare substrings: '',''.
	user := ((fields detect: [ :each | each beginsWith: ''n='' ] ifNone: [ ^ self authenticationFailed ]) allButFirst: 2).
	clientNonce := ((fields detect: [ :each | each beginsWith: ''r='' ] ifNone: [ ^ self authenticationFailed ]) allButFirst: 2).
	credentials := (users at: aName ifAbsent: [ Dictionary new ]) at: user ifAbsent: [ ^ self authenticationFailed ].
	nonce := clientNonce, (UUID new asString copyWithout: $-).
	serverFirst := ''r='', nonce, '',s='', (credentials at: ''salt'') base64Encoded, '',i='', (credentials at: ''iterations'') printString.
	id := conversations size + 1.
	[ conversations includesKey: id ] whileTrue: [ id := id + 1 ].
	conversations at: id put: (OrderedDictionary new
		at: ''stage'' put: 1;
		at: ''bare'' put: bare;
		at: ''serverFirst'' put: serverFirst;
		at: ''nonce'' put: nonce;
		at: ''credentials'' put: credentials;
		yourself).
	^ OrderedDictionary new
		at: ''conversationId'' put: id;
		at: ''done'' put: false;
		at: ''payload'' put: serverFirst asByteArray;
		at: ''ok'' put: 1.0;
		yourself'.

add value: #'MongoServerDouble' value: #'cmdSaslcontinue:db:' value: 'authentication' value:
'cmdSaslcontinue: aCommand db: aName
	| id conversation payload |
	id := aCommand at: ''conversationId''.
	conversation := conversations at: id ifAbsent: [
		^ MongoDoubleError signalCode: 17 name: ''ProtocolError'' message: ''No SASL session state found'' ].
	payload := (aCommand at: ''payload'') asString.
	(conversation at: ''stage'') = 1 ifTrue: [ ^ self scramFinalOf: conversation id: id payload: payload ].
	conversations removeKey: id.
	^ OrderedDictionary new
		at: ''conversationId'' put: id;
		at: ''done'' put: true;
		at: ''payload'' put: ByteArray new;
		at: ''ok'' put: 1.0;
		yourself'.

add value: #'MongoServerDouble' value: #'scramFinalOf:id:payload:' value: 'authentication' value:
'scramFinalOf: aConversation id: anId payload: aPayload
	"The client final message is c=biws,r=<nonce>,p=<proof>."
	| proofIndex withoutProof proof credentials authMessage clientSignature clientKey serverSignature |
	proofIndex := aPayload indexOfSubCollection: '',p=''.
	proofIndex = 0 ifTrue: [ ^ self authenticationFailed ].
	withoutProof := aPayload copyFrom: 1 to: proofIndex - 1.
	proof := (aPayload copyFrom: proofIndex + 3 to: aPayload size) base64Decoded.
	(withoutProof endsWith: ''r='', (aConversation at: ''nonce'')) ifFalse: [
		conversations removeKey: anId.
		^ self authenticationFailed ].
	credentials := aConversation at: ''credentials''.
	authMessage := (aConversation at: ''bare''), '','', (aConversation at: ''serverFirst''), '','', withoutProof.
	clientSignature := (HMAC on: SHA1 new) key: (credentials at: ''storedKey''); digestMessage: authMessage asByteArray.
	clientKey := proof bitXor: clientSignature.
	(SHA1 hashMessage: clientKey) = (credentials at: ''storedKey'') ifFalse: [
		conversations removeKey: anId.
		^ self authenticationFailed ].
	serverSignature := ((HMAC on: SHA1 new) key: (credentials at: ''serverKey''); digestMessage: authMessage asByteArray) base64Encoded.
	aConversation at: ''stage'' put: 2.
	^ OrderedDictionary new
		at: ''conversationId'' put: anId;
		at: ''done'' put: false;
		at: ''payload'' put: (''v='', serverSignature) asByteArray;
		at: ''ok'' put: 1.0;
		yourself'.

"The five setUp methods: setUp starts the stand-in, then runs the unchanged old setUp
under the name setUpAgainstDouble. The guard is the name of the stand-in in the source."
#(#MongoTest #MongoWholeDatabaseTest #MongoCommandCursorTest #MongoAuthTest #MQQueryTest) do: [ :className |
	| cls oldSource |
	cls := Smalltalk globals at: className ifAbsent: [ nil ].
	(cls notNil and: [ cls includesSelector: #setUp ])
		ifFalse: [ Stdio stdout nextPutAll: 'SKIP ', className, '>>setUp (absent)'; lf; flush ]
		ifTrue: [
			oldSource := (cls >> #setUp) sourceCode.
			(oldSource includesSubstring: 'MongoServerDouble')
				ifTrue: [ Stdio stdout nextPutAll: 'SKIP ', className, '>>setUp (defect not present)'; lf; flush ]
				ifFalse: [
					cls compile: 'setUpAgainstDouble', (oldSource allButFirst: 'setUp' size) classified: 'running'.
					cls compile: 'setUp
	MongoServerDouble ensureRunning.
	self setUpAgainstDouble' classified: 'running'.
					Stdio stdout nextPutAll: 'FIXED ', className, '>>setUp'; lf; flush ] ] ].

fix value: #MongoTest value: #testAddExistingCollection
	value: [ :source | source includesSubstring: 'self skip' ]
	value: 'testAddExistingCollection
	"The server answers error 48 (NamespaceExists) for a collection that exists, and the driver
	 turns the error into MongoCollectionAlreadyExists."

	self
		should: [ database addCollection: ''testCollection'' ]
		raise: MongoCollectionAlreadyExists'.

fix value: #MongoAuthTest value: #testSCRAMSHA1AuthMechanism
	value: [ :source | source includesSubstring: 'self skip' ]
	value: 'testSCRAMSHA1AuthMechanism
	"The server of the tests has the user pharounittest with the password test in the database
	 test (the in-image MongoServerDouble). A real mongod needs the user, made by hand with
	 db.createUser."

	root authMechanism: (SCRAMSHA1AuthMechanism root: root).

	self assert: (root authenticateUsername: ''pharounittest'' password: ''test'' database: database).
	self assert: (root authenticateUsername: ''pharounittest'' password: ''invalid'' database: database) not'.

fix value: #MQQueryTest value: #testWhere
	value: [ :source | (source includesSubstring: '==') not ]
	value: 'testWhere
	| selected |
	collection add: (Dictionary new at: ''key1'' put: ''value1''; yourself).
	collection add: (Dictionary new at: ''key2'' put: ''value2''; yourself).
	selected := collection detect: [ :each | each where: ''this.key1 == "value1"''].
	self assert: (selected at: ''key1'') equals: ''value1'''.
