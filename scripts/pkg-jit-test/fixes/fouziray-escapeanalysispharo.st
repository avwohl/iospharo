"fouziray/EscapeAnalysisPharo (row fouziray-escapeanalysispharo).
 The record of the repair: docs/upstream/package-repairs.md

 The image has Pharo 14 build 791.  The package was written for an older Pharo,
 and some code and some tests of the package are unfinished.  Cause of each entry:

 1. OCMessageNode>>selector: wants an OCSelectorNode, not a String (AST-Core
    OCMessageNode, selector:).  Five tests build nodes with a String.
 2. MetacelloMonticelloLoader>>loadProject: is now loadProject:engine:.
 3. Three methods hold a left-over breakpoint (1 halt): two tests and
    ObjectInlining>>assignObjectCreatedInlineToTmp:.  A Halt that nothing
    handles ends the headless image with exit 1.
 4. The analyzer (EscapeAnalyzerVisitor) says that an object escapes in cases
    where the object cannot outlive the method.  Causes:
    a. A block that the compiler inlines (ifTrue:, timesRepeat:, and:, to:do:,
       ifNil: with literal blocks) is no closure.  visitBlockNode: marked each
       variable that such a block names as escaping, and the analyzer never
       looked at the statements inside the block.  Also a temporary that a
       block declares was taken for the method temporary with the same name.
       Now the statements run in place, and a send that the compiler inlines
       does not make a context.
    b. A store into an instance variable of a tracked object marked the
       stored value as escaping.  The value escapes only when the tracked
       object escapes: the store is now a reference from the object to the
       value.  The root method has no tracked self: there the value escapes.
       A store of self into itself still escapes (the fixtures say so).
    c. The value of an argument message (a getter on a tracked object) was
       taken for the tracked object itself.  The analysis now follows what the
       methods can return: a method that returns only an instance variable, a
       literal, a global, or the result of sends that cannot return their
       operands does not return its receiver.
    d. The flag that tells that the result of a send is returned stayed set
       after the last return statement of the root method.  The flag is now
       the return status of the context, and a summary is reused only for the
       same return status.
    e. A send of the method that the context analyses (direct recursion) made
       a new context until the depth limit, and the limit marked the receiver
       as escaping.  The send now links the arguments to the formal arguments
       of the running context.
    f. A variable that the callee did not track was looked up in the context
       of the caller, with the name of the caller.  The name self in a callee
       was the self of the caller.  This tied unrelated objects to each other
       and made every object escape that was sent a message with some
       implementor in the image.  Now the lookup stays in the context.
    g. Methods of the kernel that only read their receiver (class, ==, ~~,
       identityHash, size, basicSize) are no cause of an escape, although they
       have a primitive or a reflection pragma.
    h. Symbol>>implementors scans every class: the analyzer keeps the answer
       for each selector.
 5. ObjectInlining>>assignObjectCreatedInlineToTmp: was a stub that answered
    self.  The method now answers a copy of the method in which each object
    that the method creates inside an expression is first assigned to a new
    temporary variable.  ObjectInlining>>refactorOccurencesOf: now takes one
    tracked variable or a collection (the test passes one variable).
 6. Two tests had a wrong expected value.  testHoneyGinger took the candidate
    point1 of the fixture foo: for HGSimulator class>>bouncingBalls, which has
    no such variable: the right value is no candidate.  The test
    testSummaryInformationReuseTypeNiltoTypedCorresponding named the method of
    its sibling in a list of keys, and it expected a summary reuse for
    arguments with another type: the summary is for typed arguments, the new
    arguments have no type, so the callee is analysed again (2 contexts).
 Open: EscapeTest>>testComplexWeighterGraph (skipped by the author).
"

fix value: #EscapeTest value: #testReduceOneAssignmentToTheVariable
	value: [ :source | source includesSubstring: 'selector: ''new''' ]
	value:
'testReduceOneAssignmentToTheVariable
| aVariable |
aVariable := OCLiteralValueNode value: ''a''.

escapeAnalyzer allowedDepth: 4.

escapeAnalyzer allowedWorklistSize: 100.
escapeAnalyzer context associatedMethod: Object >> #yourself.
escapeAnalyzer reduceAssignmentToItsVar:
	(OCAssignmentNode variable: aVariable value: (OCMessageNode
			  receiver: (OCLiteralNode value: ''Object'')
			  selector: (OCSelectorNode value: #new)))'.

fix value: #EscapeTest value: #testReduceMultipleAssignmentAndTrackAllAssignedWithEach
	value: [ :source | source includesSubstring: 'selector: ''new''' ]
	value:
'testReduceMultipleAssignmentAndTrackAllAssignedWithEach
| aVariable |
escapeAnalyzer allowedDepth: 4.
escapeAnalyzer allowedWorklistSize: 100.

escapeAnalyzer context associatedMethod: Object >> #yourself.
aVariable := OCLiteralValueNode value: ''a''.
escapeAnalyzer reduceAssignmentToItsVar:
	(OCAssignmentNode variable: aVariable value: (OCAssignmentNode
			  variable: (OCLiteralValueNode value: ''b'')
			  value: (OCMessageNode
					   receiver: (OCLiteralNode value: ''Object'')
					   selector: (OCSelectorNode value: #new)))).

self
	assert: (escapeAnalyzer reduceAssignmentToItsVar:
			 (OCAssignmentNode variable: aVariable value: (OCMessageNode
					   receiver: (OCLiteralNode value: ''Object'')
					   selector: (OCSelectorNode value: #new))))
	equals: aVariable.

self assert: escapeAnalyzer context trackedMethodArgs first name
	= ''Object_yourself_a'''.

fix value: #EscapeTest value: #testLoadMetacelloProject
	value: [ :source | source includesSubstring: '#loadProject:.' ]
	value:
'testLoadMetacelloProject
escapeAnalyzer allowedDepth: 30.
escapeAnalyzer allowedWorklistSize: 100.
escapeAnalyzer escapeAnalyze:
	MetacelloMonticelloLoader >> #loadProject:engine:.
self assert:
	(escapeAnalyzer findVariableByNameSymbol: #project) escapeStatus.
self assert:
	(escapeAnalyzer findVariableByNameSymbol: #projectClassInstance)
		escapeStatus'.

fix value: #ReferencingTest value: #testInlinedCandidateReferences
	value: [ :source | source includesSubstring: '1 halt' ]
	value:
'testInlinedCandidateReferences
"escapeAnalyzer := EscapeAnalyzer new."

| objInliner method candidates |
objInliner := ObjectInlining new.
objInliner allowedDepth: 8.
method := FixtureEscapeAnalysis >> #foo:.
candidates := objInliner findCandidatesOf: method.
(objInliner escapeAnalyzer currentTrackedVars select: [ :e |
	 e escapeStatus not ]) do: [ :nonEscapingVar |
	nonEscapingVar trackedVarsReferencingSelf do: [:tvReferencingNonEscapingVar | objInliner refactorOccurencesOf: tvReferencingNonEscapingVar  ] ]'.

fix value: #ObjectInliningTest value: #testInliningObjectPoint
	value: [ :source | source includesSubstring: 'selector: ''new''' ]
	value:
'testInliningObjectPoint
| candidates astt compiler method candidatesAssociatedWithTV |
objInliner allowedDepth: 8.
method := FixtureEscapeAnalysis >> #foo:.
candidates := objInliner findCandidatesOf: method.
candidatesAssociatedWithTV := candidates collect: [ :c |
		                              c
		                              ->
		                              (objInliner escapeAnalyzer
			                               findVariableByNameSymbol:
			                               c variable name) ].
objInliner
	inlineRefactorCandidateObjects: candidatesAssociatedWithTV
	in: method.
self
	assert: candidatesAssociatedWithTV first key
	equals: (ASTAssignmentNode
			 variable: (ASTVariableNode named: ''point1'')
			 value: (ASTMessageNode
					  receiver: (ASTVariableNode named: ''PointInlined'')
					  selector: (OCSelectorNode value: #new)))'.

fix value: #ObjectInliningTest value: #testInliningObjectPoint2
	value: [ :source | source includesSubstring: 'selector: ''new''' ]
	value:
'testInliningObjectPoint2
| candidates astt compiler method candidatesAssociatedWithTV|
objInliner allowedDepth: 8.
method := FixtureEscapeAnalysis >> #fooNotReturn:.
candidates := objInliner findCandidatesOf: method.
candidatesAssociatedWithTV := candidates collect: [ :c | c -> (objInliner escapeAnalyzer  findVariableByNameSymbol:  c variable name) ].
objInliner inlineRefactorCandidateObjects: candidatesAssociatedWithTV in: method.

self assert: candidatesAssociatedWithTV first key equals: (ASTAssignmentNode
		 variable: (ASTVariableNode named: ''point1'')
		 value: (ASTMessageNode
				  receiver: (ASTVariableNode named: ''PointInlined'')
				  selector: (OCSelectorNode value: #new)))'.

fix value: #ObjectInliningTest value: #testReturnedAssigment
	value: [ :source | source includesSubstring: 'selector: ''new''' ]
	value:
'testReturnedAssigment
| candidates |
objInliner allowedDepth: 8.
candidates := objInliner findCandidatesOf:
	              HGSimulator class >> #lint2000 .

self assert: candidates first equals: (ASTAssignmentNode
		 variable: (ASTVariableNode named: ''random'')
		 value: (ASTMessageNode
				  receiver: (ASTVariableNode named: ''Random'')
				  selector: (OCSelectorNode value: #new)))'.

fix value: #EscapeAnalyzerVisitor value: #visitBlockNode:
	value: [ :source | (source includesSubstring: 'isInlined') not ]
	value:
'visitBlockNode: aBlockNode
" treat following cases:
1- when the value returned is tracked you must follow the blockNode as a variable and see where it is assigned 
2- add track to a variable if it is assigned to a tracked variable

then whenever a variable is escaped this must bubble up until the returned variable in the block node
"

| referencedTrackedVariables trackedBlockNode |
"A block that the compiler inlines is no closure and cannot outlive the method: visit its statements in place."
aBlockNode isInlined ifTrue: [ ^ self visitNode: aBlockNode body ].
(referencedTrackedVariables := self blockReferencesTVs: aBlockNode)
	ifNotEmpty: [
		trackedBlockNode := self trackVariable: aBlockNode.
		trackedBlockNode referencesTV:
			(referencedTrackedVariables collect: [ :v |
				 self findVariableByNameSymbol: (self unravelLocalName: v name) ]).
		self markVariableAsEscaped: aBlockNode ]'.

fix value: #EscapeAnalyzerVisitor value: #visitMessageNode:
	value: [ :source | (source includesSubstring: 'isInlined') not ]
	value:
'visitMessageNode: aMessageNode
| implementors nodesToTreat listOfNewTrackedVariableTables followTheseNodes |


"The compiler inlines this send: no method runs, and the blocks of the send are no closures."
aMessageNode isInlined ifTrue: [
	^ aMessageNode children do: [ :node | node acceptVisitor: self ] ].
self flag:
	'' check nodes states before stacking a method to the worklist''.
"Treat only the arguments that are variables and the receiver"
nodesToTreat := { aMessageNode receiver } , aMessageNode arguments.
self reduceAssignments: nodesToTreat .
implementors  := self implementorsOfAstMessage: aMessageNode.
listOfNewTrackedVariableTables := OrderedCollection new.
followTheseNodes := self selectFollowedAstNodes: nodesToTreat.
followTheseNodes isNotEmpty ifTrue: [
	(self isSpecialMethod: aMessageNode selector)
		ifFalse: [
			(self shouldConsiderImplementors: implementors)
				ifTrue: [
					self
						updateReturnFlagAndWorklistOfMessage: aMessageNode
						with: implementors
						andNodes: nodesToTreat 
						]
				ifFalse: [
					(nodesToTreat select: [ :n |
						 (self findVariableByName: n) isNotNil ]) do: [ :node |
						self markVariableAsEscaped: node ] ] ]
		ifTrue: [
			aMessageNode selector = #value ifTrue: [
				self markVariableAsEscaped: aMessageNode receiver ] ] ].

aMessageNode children do: [ :node | node acceptVisitor: self ]'.

fix value: #EscapeAnalyzerVisitor value: #findVariableByName:
	value: [ :source | (source includesSubstring: 'isDefinedByBlock') not ]
	value:
'findVariableByName: anASTNode

	anASTNode isVariable ifFalse: [
		anASTNode isBlock ifTrue: [
			^ context findVariableByName: anASTNode asString ].
		^ nil ].
	"A temporary or an argument that a block declares is not the method temporary with the same name."
	anASTNode isDefinedByBlock ifTrue: [ ^ nil ].
	^ context findVariableByName: anASTNode variable name'.

fix value: #EscapeAnalyzerVisitor value: #visitReturnNode:
	value: [ :source | source includesSubstring: 'self currentMethodIsReturned' ]
	value:
'visitReturnNode: aNode
| returnsThisVar |
returnsThisVar := nil.
aNode children do: [ :node |
	(node isVariable and: [
		 (context returnStatus or: [
			  aNode allParents anySatisfy: #isBlock ]) and: [
			 self isTrackedNode: node ] ])
		ifTrue: [
			returnsThisVar := node variable name.
			context returnStatus ifTrue: [
				self markVariableAsEscaped: node variable
				"Further checks to allow for returned variables in case of local senders." ] ]
		ifFalse: [ self visitNode: node ] ].

^ returnsThisVar'.

fix value: #EscapeAnalyzerVisitor value: #updateReturnFlagAndWorklistOfMessage:with:andNodes:
	value: [ :source | source includesSubstring: 'self isAtTopLevel' ]
	value:
'updateReturnFlagAndWorklistOfMessage: aMessageNode with: implementors andNodes: nodesToTreat
| returnStateOfMessage |
returnStateOfMessage := false.
(aMessageNode allParents anySatisfy: #isReturn) ifTrue: [
	self setReturnedFlag: true.
	returnStateOfMessage := context returnStatus ].
self checkMessageAlreadyAddedToWorkingList: aMessageNode.
self
	addContext: returnStateOfMessage
	forImplementors: implementors
	andCallSiteNodes: nodesToTreat.
^ self addMethodsToWorkList: implementors'.

fix value: #EscapeAnalyzerVisitor value: #visitAssignmentNode:
	value: [ :source | (source includesSubstring: 'storeIntoInstanceVariable') not ]
	value:
'visitAssignmentNode: aNode
(aNode value isVariable and:[self isTrackedNode: aNode value])
	ifFalse: [
		self visitNode: aNode value.
		( self isTrackedNode: aNode variable ) ifTrue: [
			|abstractVariable|
			abstractVariable := self findVariableByName: aNode variable.
			
			(aNode value isMessage
 and: [ aNode value selector = #new ] ) ifTrue: [ 
aNode value receiver isGlobalVariable ifTrue: [ self setTypeOf: abstractVariable as: (Smalltalk at: aNode value receiver name ifAbsent: nil). ].
	
	aNode value receiver isSelfOrSuperVariable ifTrue: [ self setTypeForPseudoVariable: aNode value receiver inRepresentingObject: abstractVariable ]
  ] ifFalse: [
	aNode value isSelfOrSuperVariable ifTrue: [ 
			self setTypeForPseudoVariable: aNode value inRepresentingObject: abstractVariable .
		
		 ]
].

].
		(aNode value isBlock and: [ self isTrackedNode: aNode value ])
			ifTrue: [
				| variableNode |
				(variableNode := self findVariableByName: aNode variable) ifNil: [
					variableNode := self trackVariable: aNode variable ].
				variableNode addReferenceToTrackedVariable:
					(self findVariableByName: aNode value) ] ]
	ifTrue: [
	|abstractVariable abstractValue|
		aNode variable isClassVariable ifTrue: [
			self markVariableAsEscaped: aNode value.
			^ self ].
		aNode variable isInstanceVariable ifTrue: [
			self storeIntoInstanceVariable: aNode.
			^ self ].
		aNode variable isGlobalVariable ifTrue: [
			self markVariableAsEscaped: aNode value.
			^ self ]. 
		abstractVariable := 	self resetThenTrackVariable: aNode variable.
			abstractValue := self findVariableByName: aNode value.
			abstractValue hasType ifTrue: [ self setTypeOf: abstractVariable as: (abstractValue) type . ].
 
		 ]'.

add value: #EscapeAnalyzerVisitor value: #storeIntoInstanceVariable:
	value: 'visiting'
	value:
'storeIntoInstanceVariable: anAssignmentNode
	"The value goes into an instance variable of self. When the analysis tracks self (the receiver of the send that made this context), the value escapes only when self escapes: self gets a reference to the value. When the analysis does not track self (the root method), anybody can reach self and the value escapes."

	| holder value |
	value := self findVariableByName: anAssignmentNode value.
	value ifNil: [ ^ self ].
	holder := self trackedSelfOfContext.
	"Without a tracked self, or when self is stored into itself (the fixtures of the package say that this escapes), the value escapes."
	(holder isNil or: [ holder == value ])
		ifTrue: [ self markVariableAsEscaped: anAssignmentNode value ]
		ifFalse: [
			holder addReferenceToTrackedVariable: value.
			value referencedByTV: holder ]'.

add value: #EscapeAnalyzerVisitor value: #trackedSelfOfContext
	value: 'visiting'
	value:
'trackedSelfOfContext
	"The tracked variable that stands for self in the running context, or nil."

	^ context trackedMethodArgs
		  detect: [ :each | each name = (context globallyDistinctName: ''self'') ]
		  ifNone: [ nil ]'.

(Smalltalk globals at: #EscapeAnalyzerVisitor ifAbsent: [ nil ]) ifNotNil: [ :c | (c instVarNames includes: #lookupCache) ifFalse: [ c addInstVarNamed: 'lookupCache' ] ].

fix value: #EscapeAnalyzerVisitor value: #addContext:forImplementors:andCallSiteNodes:
	value: [ :source | source includesSubstring: 'self retrieveHistoryOf: implementor given: newMethodArgumentsNodes' ]
	value:
'addContext: returnStateOfMessage forImplementors: implementors andCallSiteNodes: nodes

	| newMethodArgumentsNodes |
	implementors doWithIndex: [ :implementor :i |
		| newMessageContext newMethodArguments |
		"A send of the method that runs now (direct recursion) needs no new context: the callee does what this context does."
		implementor == self context currentMethod
			ifTrue: [
				self
					linkRecursiveCallTo: implementor
					withCallSiteNodes: nodes ]
			ifFalse: [
				newMethodArgumentsNodes := OrderedCollection new.
				newMessageContext := self
					                     createMessageContextReturned:
					                     returnStateOfMessage
					                     withMethod: implementor.
				newMethodArguments := implementor argumentNames asOrderedCollection.
				newMethodArguments addFirst: #self.
				newMethodArguments doWithIndex: [ :argName :index |
					| newTrackedVariable sendSiteArg |
					sendSiteArg := nodes at: index.

					(self isTrackedNode: sendSiteArg)
						ifTrue: [
							newTrackedVariable := newMessageContext trackVariable: argName.
							newTrackedVariable referencesTV:
								(self findVariableByName: sendSiteArg).
							newMethodArgumentsNodes add: newTrackedVariable ]
						ifFalse: [
							(self isAllocationMessage: sendSiteArg)
								ifTrue: [
									newTrackedVariable := newMessageContext trackVariable: argName.
									newTrackedVariable type: sendSiteArg receiver.
									newTrackedVariable referencesTV:
										(self trackUnassignedObj: sendSiteArg).
									self incrementCurrentNumberOfUnassignedRefs ]
								ifFalse: [
									sendSiteArg isMessage ifTrue: [
										| listOfTrackedArgsInMessage |
										listOfTrackedArgsInMessage := OrderedCollection new.
										self
											selectFollowedAstNodes: {
													sendSiteArg arguments.
													sendSiteArg receiver } flattened asOrderedCollection
											in: listOfTrackedArgsInMessage.
										newTrackedVariable := newMessageContext trackVariable: argName.
										"The value of the argument message is the tracked object only when the method of the message can return its receiver or an argument."
										(listOfTrackedArgsInMessage isNotEmpty and: [
											 self mayHoldTrackedObject: sendSiteArg ]) ifTrue: [
											listOfTrackedArgsInMessage do: [ :e |
												newTrackedVariable type: sendSiteArg receiver.
												newTrackedVariable referencesTV: e ] ] ] ] ] ].
				(self isMethodTreated: implementor) ifTrue: [
					"A summary holds for the return status of its context only."
					((eaHistory restituteContextOf: implementor) returnStatus
					 = returnStateOfMessage and: [
						 self
							 retrieveHistoryOf: implementor
							 given: newMethodArgumentsNodes ]) ifTrue: [ ^ self ] ].
				self extendCtxList: newMessageContext ] ]'.

add value: #EscapeAnalyzerVisitor value: #linkRecursiveCallTo:withCallSiteNodes:
	value: 'visiting'
	value:
'linkRecursiveCallTo: aMethod withCallSiteNodes: nodes
	"The running context analyses aMethod and the send calls aMethod. What happens to a formal argument of the callee happens to the same formal argument of the running context. A tracked variable at the call site and the tracked formal argument of the running context get a reference to each other. The root method has no tracked formal arguments: nothing to link."

	| formalNames |
	formalNames := aMethod argumentNames asOrderedCollection.
	formalNames addFirst: #self.
	formalNames doWithIndex: [ :formalName :index |
		| formal actual |
		formal := context trackedMethodArgs
			          detect: [ :each |
			          each name = (context globallyDistinctName: formalName) ]
			          ifNone: [ nil ].
		actual := self findVariableByName: (nodes at: index).
		(formal notNil and: [ actual notNil ]) ifTrue: [
			formal addReferenceToTrackedVariable: actual.
			actual referencedByTV: formal ] ]'.

add value: #EscapeAnalyzerVisitor value: #lookupCache
	value: 'accessing'
	value:
'lookupCache
	"Results of lookups in the whole image, kept for the life of the analyzer: Symbol>>implementors scans every class and costs the same for each send."

	^ lookupCache ifNil: [ lookupCache := Dictionary new ]'.

add value: #EscapeAnalyzerVisitor value: #implementorsOfSelector:
	value: 'pointsTo'
	value:
'implementorsOfSelector: aSelector

	^ self lookupCache at: aSelector ifAbsentPut: [ aSelector implementors ]'.

fix value: #EscapeAnalyzerVisitor value: #implementorsOfAstMessage:
	value: [ :source | source includesSubstring: 'aMessageNode selector implementors' ]
	value:
'implementorsOfAstMessage: aMessageNode
| receiverAbstarctVariable |
receiverAbstarctVariable := self findVariableByName:
	                            aMessageNode receiver.
receiverAbstarctVariable
	ifNotNil: [
		receiverAbstarctVariable hasType ifTrue: [
			| aSingleMethod |
			aSingleMethod := receiverAbstarctVariable type lookupSelector:
				                 aMessageNode selector. "consider reassignment of the var to upper type, then we need to treat all implementations of the hierarchy instead of looking up the first one."
			aSingleMethod ifNotNil: [ ^ { aSingleMethod } asOrderedCollection ] ] ]
	ifNil: [
		| aSingleMethod |
		aMessageNode receiver isSelfVariable ifTrue: [
			aSingleMethod := self context currentMethod classBinding value
				                 lookupSelector: aMessageNode selector ].
		(aMessageNode receiver isSuperVariable and:[self context currentMethod classBinding value
				                 superclass isNotNil]) ifTrue: [
			aSingleMethod := self context currentMethod classBinding value
				                 superclass lookupSelector: aMessageNode selector ].
		aSingleMethod ifNotNil: [ ^ { aSingleMethod } asOrderedCollection ] ].

^ self implementorsOfSelector: aMessageNode selector'.

add value: #EscapeAnalyzerVisitor value: #mayHoldTrackedObject:
	value: 'pointsTo'
	value:
'mayHoldTrackedObject: anExpression
	"True when the value of anExpression can be an object that a tracked variable holds. A send gives such a value only when an operand of the send can hold the object and the method of the send can return an operand."

	| operands |
	anExpression isLiteralNode ifTrue: [ ^ false ].
	anExpression isVariable ifTrue: [ ^ self isTrackedNode: anExpression ].
	anExpression isBlock ifTrue: [
		^ (self blockReferencesTVs: anExpression) isNotEmpty ].
	anExpression isMessage ifFalse: [
		^ anExpression children anySatisfy: [ :each |
			  self mayHoldTrackedObject: each ] ].
	operands := { anExpression receiver } , anExpression arguments.
	(operands anySatisfy: [ :each | self mayHoldTrackedObject: each ])
		ifFalse: [ ^ false ].
	^ self resultMayAliasOperands: anExpression'.

add value: #EscapeAnalyzerVisitor value: #resultMayAliasOperands:
	value: 'pointsTo'
	value:
'resultMayAliasOperands: aMessageNode
	"True when the value of the send can be the receiver or an argument of the send."

	| implementors |
	implementors := self implementorsOfAstMessage: aMessageNode.
	(implementors isEmpty or: [ implementors size > 12 ]) ifTrue: [ ^ true ].
	^ implementors anySatisfy: [ :aMethod |
		  self methodMayReturnItsOperands: aMethod ]'.

add value: #EscapeAnalyzerVisitor value: #methodMayReturnItsOperands:
	value: 'pointsTo'
	value:
'methodMayReturnItsOperands: aCompiledMethod

	^ self lookupCache at: aCompiledMethod ifAbsentPut: [
		  [ self method: aCompiledMethod mayReturnOperandsDepth: 6 ]
			  on: Error
			  do: [ :error | true ] ]'.

add value: #EscapeAnalyzerVisitor value: #method:mayReturnOperandsDepth:
	value: 'pointsTo'
	value:
'method: aCompiledMethod mayReturnOperandsDepth: depth
	"True when aCompiledMethod can answer its receiver or one of its arguments. A method that returns self, a temporary or an argument, that ends without a return statement (it returns self), or whose return expression cannot be followed, can. A method that returns only literals, instance variables or global variables, or the result of sends that cannot return their operands, cannot. A primitive answers a new object or a slot, except for the primitives of the list."

	| statements |
	depth = 0 ifTrue: [ ^ true ].
	aCompiledMethod isRealPrimitive ifTrue: [
		^ #( #yourself #value #valueWithArguments: #at:put: #basicAt:put: #instVarAt:put: #perform: #perform:with: #perform:with:with: #perform:with:with:with: #perform:withArguments: #become: #becomeForward: )
			  includes: aCompiledMethod selector ].
	aCompiledMethod isFFIMethod ifTrue: [ ^ true ].
	((aCompiledMethod pragmas collect: #selector) includes: #reflection:)
		ifTrue: [ ^ true ].
	statements := aCompiledMethod ast body statements.
	(statements isEmpty or: [ statements last isReturn not ])
		ifTrue: [ ^ true ].
	^ (aCompiledMethod ast allChildren select: [ :node | node isReturn ])
		  anySatisfy: [ :aReturn |
			  self
				  expression: aReturn value
				  inMethod: aCompiledMethod
				  mayAliasOperandsDepth: depth - 1 ]'.

add value: #EscapeAnalyzerVisitor value: #expression:inMethod:mayAliasOperandsDepth:
	value: 'pointsTo'
	value:
'expression: anExpression inMethod: aCompiledMethod mayAliasOperandsDepth: depth
	"True when the value of anExpression, in the body of aCompiledMethod, can be the receiver or an argument of aCompiledMethod."

	| operands implementors |
	anExpression isLiteralNode ifTrue: [ ^ false ].
	anExpression isSelfOrSuperVariable ifTrue: [ ^ true ].
	anExpression isVariable ifTrue: [
		^ (anExpression isInstanceVariable or: [
			   anExpression isGlobalVariable or: [
				   anExpression isLiteralVariable ] ]) not ].
	anExpression isAssignment ifTrue: [
		^ self
			  expression: anExpression value
			  inMethod: aCompiledMethod
			  mayAliasOperandsDepth: depth ].
	anExpression isMessage ifFalse: [ ^ true ].
	operands := { anExpression receiver } , anExpression arguments.
	(operands anySatisfy: [ :each |
		 self
			 expression: each
			 inMethod: aCompiledMethod
			 mayAliasOperandsDepth: depth ]) ifFalse: [ ^ false ].
	depth = 0 ifTrue: [ ^ true ].
	implementors := self
		                implementorsOfMessage: anExpression
		                sentIn: aCompiledMethod.
	(implementors isEmpty or: [ implementors size > 12 ]) ifTrue: [ ^ true ].
	^ implementors anySatisfy: [ :each |
		  self method: each mayReturnOperandsDepth: depth - 1 ]'.

add value: #EscapeAnalyzerVisitor value: #implementorsOfMessage:sentIn:
	value: 'pointsTo'
	value:
'implementorsOfMessage: aMessageNode sentIn: aCompiledMethod

	| class |
	(aMessageNode receiver isSelfVariable or: [
		 aMessageNode receiver isSuperVariable ]) ifTrue: [
		class := aMessageNode receiver isSelfVariable
			         ifTrue: [ aCompiledMethod methodClass ]
			         ifFalse: [ aCompiledMethod methodClass superclass ].
		class ifNil: [ ^ #(  ) ].
		^ (class lookupSelector: aMessageNode selector)
			  ifNil: [ #(  ) ]
			  ifNotNil: [ :found | { found } ] ].
	^ self implementorsOfSelector: aMessageNode selector'.

fix value: #EscapeAnalyzerVisitor value: #specialMethodsRPF:
	value: [ :source | (source includesSubstring: 'primitiveRetainsNoOperand:') not ]
	value:
'specialMethodsRPF: implementors
"means reflective, primitives, ffi methods all are special and could cause escape. The methods that only read their receiver are no cause."

| isSpecial |
isSpecial := false.
implementors do: [ :i |
	(self primitiveRetainsNoOperand: i) ifFalse: [
		i isRealPrimitive ifTrue: [
			analysisStats incrNumberOfPrim.
			isSpecial := true ].

		((i pragmas collect: #selector) includes: #reflection:) ifTrue: [
			analysisStats incrNumberOfReflection.
			isSpecial := true ].

		i isFFIMethod ifTrue: [
			analysisStats incrNumberOfFFI.
			isSpecial := true ] ] ].
^ isSpecial'.

add value: #EscapeAnalyzerVisitor value: #primitiveRetainsNoOperand:
	value: 'visiting'
	value:
'primitiveRetainsNoOperand: aCompiledMethod
	"A method of the kernel that only reads its receiver (the class, an identity test, the identity hash, the size) keeps no reference to the receiver or to an argument, so the send does not make an operand escape. The methods have a primitive, and ProtoObject>>class has a reflection pragma: all three look special."

	^ (aCompiledMethod methodClass == ProtoObject or: [ aCompiledMethod methodClass == Object ]) and: [
		  #( #class #== #~~ #identityHash #basicSize #size ) includes:
			  aCompiledMethod selector ]'.

fix value: #EscapeTest value: #testSummaryInformationReuseTypeNiltoTypedCorresponding
	value: [ :source | source includesSubstring: '(FixtureEscapeAnalysis >> #secondCallFrequentMethod).' ]
	value:
'testSummaryInformationReuseTypeNiltoTypedCorresponding
escapeAnalyzer allowedDepth: 4.
escapeAnalyzer allowedWorklistSize: 100.
escapeAnalyzer escapeAnalyze:
	FixtureEscapeAnalysis >> #firstCallFrequentMethod.
self assert:
	(escapeAnalyzer findVariableByNameSymbol: #trackedTemp) escapeStatus.
self deny:
	(escapeAnalyzer findVariableByNameSymbol: #secondTrackedTemp)
		escapeStatus.

self assert: (escapeAnalyzer realNumberOfTreatedContexts) equals: 2.
self assert: escapeAnalyzer summaryData size equals: 2.

escapeAnalyzer rootContextInit.
escapeAnalyzer resetContxtCounterForCurrentMethod.
self assert: (escapeAnalyzer realNumberOfTreatedContexts) equals: 0.	

escapeAnalyzer escapeAnalyze:
	FixtureEscapeAnalysis >> #aCallWithNilNilTypes .

self assert: escapeAnalyzer summaryData size equals: 3.
self assert: (escapeAnalyzer summaryData keys intersection: {
			 (FixtureEscapeAnalysis >> #firstAgumentEscapes:second:).
			 (FixtureEscapeAnalysis >> #aCallWithNilNilTypes).
			 (FixtureEscapeAnalysis >> #firstCallFrequentMethod) }) size equals: escapeAnalyzer summaryData size.
"The first analysis typed the arguments, this one does not: the summary does not match, and the callee is analysed again (root and callee)."
self assert: (escapeAnalyzer realNumberOfTreatedContexts) equals: 2.'.

fix value: #ObjectInlining value: #refactorOccurencesOf:
	value: [ :source | source includesSubstring: 'trackedVariables do: [ :tv |' ]
	value:
'refactorOccurencesOf: trackedVariables
"Takes one tracked variable or a collection of them (the test gives one; the recursion gives the collection trackedVarsReferencingSelf of a tracked variable)."

| astClone |
(trackedVariables isCollection
	 ifTrue: [ trackedVariables ]
	 ifFalse: [ { trackedVariables } ]) do: [ :tv |
	astClone := tv associatedMethod ast copy.

	tv name = #self ifFalse: [
		self saveOriginalAst: tv associatedMethod.
		astClone := tv associatedMethod ast copy.


		(astClone allChildren select: [ :e |
			 e isVariable and: [ e name = tv name ] ]) do: [ :aCandidate |
			aCandidate replaceWith: (ASTVariableNode named: ''self'').
			self refactorOccurencesOf: tv trackedVarsReferencingSelf ].

		tv associatedMethod classBinding value compile: astClone asString ] ]'.

fix value: #ObjectInlining value: #assignObjectCreatedInlineToTmp:
	value: [ :source | (source includesSubstring: 'inlineTmp') not ]
	value:
'assignObjectCreatedInlineToTmp: aCompiledMethod
"Answers a method like aCompiledMethod in which each object that the method creates inside an expression (a send of #new to a global that is not the value of an assignment) is first assigned to a new temporary variable, so that the analysis tracks the object under a name. The new method has the selector of aCompiledMethod with the prefix inlinedTmp_ and is compiled in the class of aCompiledMethod."

| astClone counter newNodes selector |
astClone := aCompiledMethod ast copy.
counter := 0.
newNodes := astClone allChildren select: [ :each |
	            each isMessage and: [
		            each selector = #new and: [
			            each receiver isGlobalVariable and: [
				            each parent isAssignment not ] ] ] ].
newNodes do: [ :newNode |
	| statement sequence tmpName |
	counter := counter + 1.
	tmpName := ''inlineTmp'' , counter printString.
	statement := self findEnclosingStatementNodeFor: newNode.
	sequence := statement parent.
	sequence addTemporaryNamed: tmpName.
	sequence
		addNode: (ASTAssignmentNode
				 variable: (ASTVariableNode named: tmpName)
				 value: newNode copy)
		before: statement.
	newNode replaceWith: (ASTVariableNode named: tmpName) ].
astClone selector: #inlinedTmp_ , astClone selector.
selector := aCompiledMethod methodClass
	            compile: astClone newSource
	            classified: #inlining.
^ aCompiledMethod methodClass >> selector'.

fix value: #EAMethodContext value: #findVariableByName:
	value: [ :source | source includesSubstring: 'eaContext findVariableByName: aName' ]
	value:
'findVariableByName: aName
"The variables of a method are the tracked variables of its own context. The names of the context of the caller must not answer: the name self in a callee is the self of the callee, not the self of the caller."

^ super findVariableByName: aName'.

fix value: #ObjectInliningTest value: #testHoneyGinger
	value: [ :source | (source includesSubstring: 'inlineTmp1') not ]
	value:
'testHoneyGinger
 
| candidates method candidatesAssociatedWithTV newMethod |
objInliner allowedDepth: 8.
method := HGSimulator class >> #bouncingBalls.
newMethod := objInliner assignObjectCreatedInlineToTmp: method.
"The three objects that the method creates inside an argument (HGForceIntegration new, HGVelocityIntegration new, HGLocalityMap new) are assigned to temporary variables."
self
	assert: (newMethod ast body temporaryNames collect: [ :each | each asString ]) asOrderedCollection
	equals: #( ''simulator'' ''inlineTmp1'' ''inlineTmp2'' ''inlineTmp3'' ) asOrderedCollection.
candidates := objInliner findCandidatesOf: newMethod.
candidatesAssociatedWithTV := candidates collect: [ :c | c -> (objInliner escapeAnalyzer  findVariableByNameSymbol:  c variable name) ].
objInliner inlineRefactorCandidateObjects: candidatesAssociatedWithTV in: newMethod.
"The method adds each of the objects to the simulator and returns the simulator: no object stays inside the method, so there is no candidate. The old test took the candidate point1 of the test for the method foo: of the fixture: HGSimulator class >> bouncingBalls has no variable point1 and no PointInlined."
self assert: candidatesAssociatedWithTV isEmpty'.
