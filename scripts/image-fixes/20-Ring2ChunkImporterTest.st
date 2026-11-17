
"The chunk parser reads '!Foo classTrait methodsFor: ...' but stores only isMeta,
 so the importer cannot tell a class-side method from a classTrait method and
 creates a class where a trait is meant.  Keep the distinction in the chunk."
(BehaviorOwnedChunk instVarNames includes: #isMetaclassTrait)
	ifFalse: [
		BehaviorOwnedChunk addInstVarNamed: 'isMetaclassTrait'.
		Stdio stdout nextPutAll: 'ADDED BehaviorOwnedChunk instVar isMetaclassTrait'; lf; flush ]
	ifTrue: [ Stdio stdout nextPutAll: 'SKIP BehaviorOwnedChunk instVar (already present)'; lf; flush ].
add value: #BehaviorOwnedChunk value: #isMetaclassTrait value: 'accessing'
	value: 'isMetaclassTrait
	"True when the chunk was declared as ''Foo classTrait methodsFor: ...'' (the class side of a trait)."

	^ isMetaclassTrait ifNil: [ false ]'.
add value: #BehaviorOwnedChunk value: #isMetaclassTrait: value: 'accessing'
	value: 'isMetaclassTrait: aBoolean

	isMetaclassTrait := aBoolean'.
fix value: #ChunkFileFormatParserV100 value: #parseMethodDeclarations:
	value: [ :s | (s includesSubstring: 'isMetaclassTrait') not ]
	value: 'parseMethodDeclarations: methodsPreamble

	| behaviorName isMeta isMetaclassTrait protocol stamp methodSource methodMetadata |
	"The method preable is an array with the following structure:
	If instance side method:

	#(#CodeImportTestCaseTestClass #methodsFor: ''some protocol'' #stamp: ''GuillermoPolito 5/2/2012 13:35'')

	if class side:

	#(#CodeImportTestCaseTestClass #class #methodsFor: ''some protocol'' #stamp: ''GuillermoPolito 5/2/2012 13:35'')

	if class side of a trait:

	#(#TSomeTrait #classTrait #methodsFor: ''some protocol'' #stamp: ''GuillermoPolito 5/2/2012 13:35'')

	Sometimes there is no timestamp: 

	#(#CodeImportTestCaseTestClass #methodsFor: ''some protocol'')

	"
	behaviorName := methodsPreamble first.
	isMetaclassTrait := methodsPreamble second = #classTrait.
	isMeta := methodsPreamble second = #class or: [ isMetaclassTrait ].
	protocol := isMeta
		            ifTrue: [ methodsPreamble at: 4 ]
		            ifFalse: [ methodsPreamble at: 3 ].
	stamp := ''''.
	methodsPreamble size > 4 ifTrue: [
			stamp := isMeta
				         ifTrue: [ methodsPreamble at: 6 ]
				         ifFalse: [ methodsPreamble at: 5 ] ].

	methodSource := readStream next.
	methodMetadata := readStream next.
	self addDeclaration: ((self methodChunkClass
			  contents: methodSource
			  behaviorName: behaviorName asSymbol
			  isMeta: isMeta
			  protocol: protocol
			  stamp: stamp)
			 isMetaclassTrait: isMetaclassTrait;
			 metadata: methodMetadata;
			 yourself)'.
fix value: #RGChunkImporter value: #visitMethodChunk:
	value: [ :s | (s includesSubstring: 'isMetaclassTrait') not ]
	value: 'visitMethodChunk: aChunk

	| theClass theMethod theProtocol |
	theClass := aChunk isMetaclassTrait
		ifTrue: [ self environment ensureMetaclassTraitNamed: (aChunk behaviorName , '' classTrait'') asSymbol ]
		ifFalse: [ self classNamed: aChunk behaviorName ].
	(aChunk isMeta and: [ aChunk isMetaclassTrait not ]) ifTrue: [ theClass := theClass classSide makeResolved ].

	theMethod := theClass ensureLocalMethodNamed: aChunk methodSelector asSymbol.
	theProtocol := theClass ensureProtocolNamed: aChunk protocol asSymbol.
	theMethod protocol: theProtocol.
	theMethod sourceCode: aChunk contents.
	theMethod author: (RGStampParser authorForStamp: aChunk stamp).
	theMethod time: (RGStampParser timeForStamp: aChunk stamp)'.
fix value: #Ring2ChunkImporterTest value: #testStandardMethodInNewMetaclassTrait
	value: [ :s | s includesSubstring: '<expectedFailure>' ]
	value: 'testStandardMethodInNewMetaclassTrait

	| importer trait method code |
	importer := RGChunkImporter new.

	code := ''!SomeTrait classTrait methodsFor: ''''some protocol'''' stamp: ''''GuillermoPolito 2012-05-02T13:35''''!someMethod
	^true''.

	importer fileInFrom: code readStream.

	trait := importer environment ask behaviorNamed: #SomeTrait.
	method := trait classTrait methodNamed: #someMethod.

	self assert: trait isTrait.
	self assert: trait classTrait isMetaclassTrait.
	self assert: method isNotNil.
	self assert: method isRingResolved.
	self assert: (method hasResolvedAll: #(name tags sourceCode author time)).

	self assert: method name equals: #someMethod.
	self assert: method protocol equals: ''some protocol''.
	self assert: method author equals: ''GuillermoPolito''.
	self assert: method time equals: (DateAndTime fromString: ''2012-05-02T13:35'').
	self assert: method sourceCode equals: ''someMethod
	^true'''.
fix value: #Ring2ChunkImporterTest value: #testMultipleMixedChunksInStrangeOrderWithMetaclassTrait
	value: [ :s | s includesSubstring: '<expectedFailure>' ]
	value: 'testMultipleMixedChunksInStrangeOrderWithMetaclassTrait

	| importer trait method |
	importer := RGChunkImporter new.

	importer fileInFrom: ''!TEmpty classTrait methodsFor: ''''some protocol'''' stamp: ''''GuillermoPolito 5/2/2012 13:35''''!
someMethod
	^true! !
Trait named: #TEmpty
	uses: {}
	package: ''''Traits-Kernel-Traits''''!'' readStream.

	trait := importer environment ask behaviorNamed: #''TEmpty classTrait''.
	self assert: trait isNotNil.
	self assert: trait isMetaclassTrait.
	self assert: (importer environment ask behaviorNamed: #TEmpty) isTrait.
	method := trait methodNamed: #someMethod.
	self assert: method isNotNil.
	self assert: (method sourceCode endsWith: ''^true'')'.
