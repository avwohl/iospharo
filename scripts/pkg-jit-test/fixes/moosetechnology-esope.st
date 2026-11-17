"moosetechnology/Esope (row 109).  Entries for the tests of EsopeImporter-Tests
and Famix-Esope-Tests.  The load itself is repaired outside this file (a patch
of the Famix-Fortran baseline and NeoJSON loaded first, see the analysis text
of the row).

1. PPEsopeRewriter>>applySlashTransformation: puts the marker S__ in front of
   the optional blank that follows the opening parenthesis.  For the source
   'A = B ( /1 )' it makes 'B ( S__ /1 )'.  The tests testTranslateEsopeSlash10
   and ...Slash20 expect 'B ( S__/1 )': the marker sits directly in front of
   the slash, as it does when no blank follows the parenthesis.  The repair
   finds the slash and puts the marker there.

2. Six tests of the package have an unconditional 'self skip.' left from the
   time that the feature was not written.  The code of the feature is in the
   package now.  The helper EsopeEditTest removes the bare 'self skip.' of a
   test (and applies the text replacements that a test needs, see 6 and 7).

3. A call does not keep its number of actual arguments.  The resolver chooses
   the procedure of an invocation by name only (the test of the number of
   parameters is a comment in the method), so a call with the wrong number of
   arguments finds a candidate.  FamixEsopeResolverTest>>
   testResolveInvocationsWrongArgumentNumber expects no candidate.  The model
   of Famix-Fortran has no entity for an actual argument (the generator has the
   comment of an Argument class, but no class), and the IAST list of arguments
   is not a count: the importer keeps only the arguments that are entities,
   so a literal is missing.  The repair counts the entries of the JSON list of
   arguments (IASTInvocation>>argumentCount, set by visitCallStatement: and
   visitFunction_call:) and the resolver compares this number with the number
   of parameters of the procedure.  An invocation that has no count (made by
   hand) uses the size of its arguments.

4. DATA statement: JsonToIASTVisitor>>visitData: is 'self shouldBeImplemented',
   and the dispatch goes to visitDataStatement: of the abstract visitor, which
   answers the raw span and groups.  The new visitDataStatement: answers the
   accesses of the statement: each name is a write, each initial value that is
   a variable is a read (one entry of the body, like an assignment).

5. IF line that holds a segment command: the rewriter turns 'IF (N.GT.0) SEGINI
   P' into the comment 'c@_   IF (N.GT.0) SEGINI P', and no class had
   visitEsopeIfComment: (the dispatch of Esope comments sends it by the first
   word of the comment).  The new method answers the accesses of the condition
   followed by the segment command with its own anchor.  The Fortran parser
   does not see the condition, so the names are found in the text.

6. testBlockDataWithOneCommonOneData asks for 3 entries in the body of a block
   data with one COMMON and one DATA statement.  One statement gives one entry
   of the body (testBlockDataWithOneCommon: one COMMON gives 1), so the right
   number is 2.

7. testEsopeIfCommentStatement asks for the end column 29 of the segment
   command in the 26 characters wide line 'c@_   IF (N.GT.0) SEGINI P'.  The
   end column is the column after the last character (testSeginiCommand:
   'segini,lb' ends in column 15 and the anchor ends in 16), so the right
   number is 27.

8. 'segini new=old' is a copy of a segment.  No code made a
   FamixEsopeCommandSegcop.  The visitor makes a Segcop that accesses the old
   pointer and has the write access to the new pointer as newAccess.

9. 'pointeur a2(n).aseg' inside a segment: the declarations of a segment
   were all sent #typeSpec, which a pointer does not have.  The visitor makes
   an Esope attribute with the name of the pointer and the name of its segment.
   testFamixEsopeSegmentWithPointeur was a copy of testFamixEsopeSegment (it
   asked for the attributes uname, status and age, but built a1, status and
   age) and is written again for the pointer.

10. FamixFortranImporterTest>>testFortranToJsonAST was skipped because the
   parser is an outside program (fortran-src-extras, written in Haskell).  The
   test also has two defects of its own: it reads its sources from a memory
   file system, so an outside program could never open the file, and it expects
   the JSON under tmpJsonImport/project/src while the code writes
   tmpJsonImport/src.  The class FamixFortranImporterStandIn answers a parser
   command that prints a fixed JSON text, and the test uses the stand-in.  The
   test checks the folder layout and the command line of the step, not the
   parser.

11. IASTToFamixVisitorTest>>testFamixEsopeSlashWithLitteralDim describes the
   IAST in the old form (ptr and attr as two plain entities of the s__) and
   asks for an attribute #pointer that no code sets.  The JSON visitor puts the
   two names in one d__ inside the s__ (testEsoSlWithDot), and the Famix visitor
   links the two accesses with #parentAccess (testFamixEsopeAccessors).  The
   test also asks #target of an access that is not resolved; #target is a
   collection of links in the present Famix, and the accessed variable is
   #variable.  The entry writes the test again in the current form, and each
   assertion stays: two accesses, the entities ptr and attr, no variable, and
   the attribute access is linked to the pointer access."


"EsopeEditTest: removes the bare 'self skip.' statement of a test method, then applies the text replacements (a list of old/new pairs, each old text occurs once in the source).  A test that has no 'self skip' is not changed, so the entry does nothing a second time."
Smalltalk globals at: #EsopeEditTest put: [ :clsName :sel :pairs | | cls src |
	cls := resolve value: clsName.
	(cls notNil and: [ cls includesSelector: sel ])
		ifFalse: [ Stdio stdout nextPutAll: 'SKIP ', clsName, '>>', sel, ' (absent)'; lf; flush ]
		ifTrue: [
			src := (cls >> sel) sourceCode.
			(src includesSubstring: 'self skip')
				ifFalse: [ Stdio stdout nextPutAll: 'SKIP ', clsName, '>>', sel, ' (no skip)'; lf; flush ]
				ifTrue: [
					src := src copyReplaceAll: 'self skip.' with: ''.
					pairs do: [ :pair | src := src copyReplaceAll: pair first with: pair last ].
					cls compile: src classified: (cls >> sel) protocolName.
					Stdio stdout nextPutAll: 'FIXED ', clsName, '>>', sel; lf; flush ] ] ].

fix value: #PPEsopeRewriter value: #applySlashTransformation:
	value: [ :source | source includesSubstring: 'interval first + 1 to: interval last' ]
	value:
'applySlashTransformation: inputSource

	^String new writeStreamDo: [ :oStream |  | regex index |

		regex := ''\(\s?(\/)[^/]'' asRegexIgnoringCase.
		index := 1.
		(regex matchingRangesIn: inputSource)
		do: [ :interval | | slashPosition |
			"the marker goes directly in front of the slash, after an optional blank"
			slashPosition := inputSource indexOf: $/ startingAt: interval first.
			oStream << (inputSource copyFrom: index to: slashPosition - 1);
				  	  << ''S__'';
				     << (inputSource copyFrom: slashPosition to: interval last).
			index := interval last + 1
		].
	   oStream << (inputSource copyFrom: index to: inputSource size).
		oStream contents
	]'.


(Smalltalk globals at: #EsopeEditTest) value: #EsopeRewriterTest value: #testTranslateEsopeIFLogicalWithLineContinuation value: #().
(Smalltalk globals at: #EsopeEditTest) value: #EsopeRewriterTest value: #testTranslateEsopeIFLogicalWithSlash value: #().
(Smalltalk globals at: #EsopeEditTest) value: #EsopeRewriterTest value: #testTranslateEsopeSlash10 value: #().
(Smalltalk globals at: #EsopeEditTest) value: #EsopeRewriterTest value: #testTranslateEsopeSlash20 value: #().
(Smalltalk globals at: #EsopeEditTest) value: #JsonToIASTVisitorTest value: #testESOAtWithArrayArgument value: #().
(Smalltalk globals at: #EsopeEditTest) value: #IASTToFamixVisitorTest value: #testFamixFortran77ESOAtWithArrayArgument value: #().

(Smalltalk globals at: #IASTInvocation ifAbsent: [ nil ]) ifNotNil: [ :invocationClass |
	(invocationClass instVarNames includes: #argumentCount) ifFalse: [
		invocationClass addInstVarNamed: 'argumentCount'.
		Stdio stdout nextPutAll: 'ADDED IASTInvocation instance variable argumentCount'; lf; flush ] ].

add value: #IASTInvocation value: #argumentCount value: 'accessing' value:
'argumentCount
	"The number of actual arguments of the call.  Without a count that the JSON visitor set, the number of entities of #arguments."

	^ argumentCount ifNil: [ self arguments size ]'.

add value: #IASTInvocation value: #argumentCount: value: 'accessing' value:
'argumentCount: aNumber

	argumentCount := aNumber'.


add value: #JsonToIASTVisitor value: #argumentCountOf: value: 'private - utilities' value:
'argumentCountOf: aCallNode
	"The number of actual arguments of a CALL or of a function reference, counted on the JSON node.  The list #arguments: of the IAST does not give it: a literal argument is not an entity of the IAST."

	^ (aCallNode at: ''arguments'' ifAbsent: [ nil ])
		ifNil: [ 0 ]
		ifNotNil: [ :arguments | (arguments at: ''list'' ifAbsent: [ #() ]) size ]'.

fix value: #JsonToIASTVisitor value: #visitCallStatement:
	value: [ :source | (source includesSubstring: 'argumentCount') not ]
	value:
'visitCallStatement: aCallNode

	| astNode data |
	data := super visitCallStatement: aCallNode.

	astNode := IASTInvocation new
		           entityName: data second;
		           argumentCount: (self argumentCountOf: aCallNode);
		           yourself.

	^ astNode
		  sourceAnchor: (self makeIndexedAnchor: data first);
		  arguments: data third;
		  yourself'.

fix value: #JsonToIASTVisitor value: #visitFunction_call:
	value: [ :source | (source includesSubstring: 'argumentCount') not ]
	value:
'visitFunction_call: aFunctionCallNode

	| astNode data functionName |
	data := super visitFunction_call: aFunctionCallNode.

	functionName := data second.
	astNode := (self isSpecialEsopeFunctionName: functionName)
		ifTrue: [
			(IASTVarEso newFrom: functionName)
				entities: data third ;
				yourself
		]
		ifFalse: 	[IASTInvocation new
			arguments: data third ;
			argumentCount: (self argumentCountOf: aFunctionCallNode) ;
			yourself
	].

	^astNode
		entityName: functionName ;
		sourceAnchor: (self makeIndexedAnchor: data first) ;
		yourself'.

fix value: #FamixEsopeResolver value: #resolveInvocables:
	value: [ :source | (source includesSubstring: 'argumentCount') not ]
	value:
'resolveInvocables: invocable
	"A procedure is a candidate when its name is the name of the invocation and its number of parameters is the number of actual arguments of the invocation"

	^(model allWithSubTypesOf: FamixF77PUProcedure)
		detect: [ :entity |
			(entity name sameAs: invocable entityName)
				and: [ entity parameters size = invocable argumentCount ]
		]
		ifNone: [ self searchInStatementFunction: invocable ]'.

(Smalltalk globals at: #EsopeEditTest) value: #FamixEsopeResolverTest value: #testResolveInvocationsWrongArgumentNumber value: { { 'self skip: ''Famix does not model arguments, so we cannot resolve based on their number''.' . '' } }.

add value: #JsonToIASTVisitor value: #visitDataStatement: value: 'visiting statement' value:
'visitDataStatement: aDataStatementNode
	"DATA i /12345/ gives a value to i, so each name of the statement is a write access.  The initial values that are variables are read accesses.  The answer is the list of the accesses of the statement (one entry of the body, like an assignment)."

	| data accesses |
	data := super visitDataStatement: aDataStatementNode.
	accesses := OrderedCollection new.
	data second second do: [ :group |
		group second do: [ :name |
			(name isKindOf: IASTVarAccess) ifTrue: [ name isWrite: true. accesses add: name ] ].
		group third do: [ :initializer |
			(initializer isKindOf: IASTVarAccess) ifTrue: [ accesses add: initializer ] ] ].
	^ accesses'.

(Smalltalk globals at: #EsopeEditTest) value: #JsonToIASTVisitorTest value: #testBlockDataWithOneCommonOneData value: { { 'body size equals: 3' . 'body size equals: 2' } }.

add value: #JsonToIASTVisitor value: #argumentCountIn:openingAt: value: 'private - utilities' value:
'argumentCountIn: aText openingAt: anIndex
	"The number of arguments of the call whose opening parenthesis is at anIndex of aText: the commas of the first level, plus one.  0 for ''()''."

	| depth count i char |
	depth := 0.
	count := 1.
	i := anIndex.
	[ i <= aText size ] whileTrue: [
		char := aText at: i.
		char = $( ifTrue: [ depth := depth + 1 ].
		char = $) ifTrue: [
			depth := depth - 1.
			depth = 0 ifTrue: [ ^ (aText copyFrom: anIndex + 1 to: i - 1) trim isEmpty ifTrue: [ 0 ] ifFalse: [ count ] ] ].
		(char = $, and: [ depth = 1 ]) ifTrue: [ count := count + 1 ].
		i := i + 1 ].
	^ count'.

add value: #JsonToIASTVisitor value: #esopeIfConditionEntities:line:column: value: 'private - utilities' value:
'esopeIfConditionEntities: aConditionText line: aLine column: aBaseColumn
	"The Fortran parser does not see the condition of an IF line that was turned into a comment, so we find the names in the text: an identifier followed by ''('' is an invocation (a function or an array, as for a function_call of the parser), any other identifier is a variable access.  Skipped: numbers, quoted strings and the dot words (.GT. .AND. .TRUE.)."

	| entities i size char next start name anchor |
	entities := OrderedCollection new.
	size := aConditionText size.
	i := 1.
	[ i <= size ] whileTrue: [
		char := aConditionText at: i.
		next := i < size ifTrue: [ aConditionText at: i + 1 ] ifFalse: [ $  ].
		(char = $'' or: [ char = $" ])
			ifTrue: [
				i := (aConditionText indexOf: char startingAt: i + 1) max: i.
				i := i + 1 ]
			ifFalse: [
		(char = $. and: [ next isDigit not ])
			ifTrue: [
				i := (aConditionText indexOf: $. startingAt: i + 1) max: i.
				i := i + 1 ]
			ifFalse: [
		(char isDigit or: [ char = $. ])
			ifTrue: [
				"a number: digits, a fraction, an exponent"
				[ i <= size and: [ (aConditionText at: i) isDigit ] ] whileTrue: [ i := i + 1 ].
				(i < size and: [ (aConditionText at: i) = $. and: [ (aConditionText at: i + 1) isLetter not ] ]) ifTrue: [
					i := i + 1.
					[ i <= size and: [ (aConditionText at: i) isDigit ] ] whileTrue: [ i := i + 1 ] ].
				(i < size and: [ ''eEdD'' includes: (aConditionText at: i) ]) ifTrue: [
					((aConditionText at: i + 1) isDigit or: [ ''+-'' includes: (aConditionText at: i + 1) ]) ifTrue: [
						i := i + 2.
						[ i <= size and: [ (aConditionText at: i) isDigit ] ] whileTrue: [ i := i + 1 ] ] ] ]
			ifFalse: [
		char isLetter
			ifTrue: [
				start := i.
				[ i <= size and: [ (aConditionText at: i) isAlphaNumeric or: [ (aConditionText at: i) = $_ ] ] ] whileTrue: [ i := i + 1 ].
				name := (aConditionText copyFrom: start to: i - 1) asLowercase.
				anchor := self makeIndexedAnchor: { (aLine @ (aBaseColumn + start)) . (aLine @ (aBaseColumn + i)) }.
				entities add: ((i <= size and: [ (aConditionText at: i) = $( ])
					ifTrue: [ IASTInvocation new entityName: name; sourceAnchor: anchor; argumentCount: (self argumentCountIn: aConditionText openingAt: i); yourself ]
					ifFalse: [ IASTVarAccess new entityName: name; sourceAnchor: anchor; yourself ]) ]
			ifFalse: [ i := i + 1 ] ] ] ] ].
	^ entities'.

add value: #JsonToIASTVisitor value: #visitEsopeIfComment: value: 'visiting esope' value:
'visitEsopeIfComment: anEsopeCommentNode
	"An IF line that holds a segment command, written by the rewriter as a comment: c@_   IF (N.GT.0) SEGINI P
	 The answer is the accesses of the condition followed by the segment command, as one entry of the body."

	| comment span depth conditionStart conditionEnd rest offset commandText commandSpan command extraSpan data segment entities |
	comment := anEsopeCommentNode at: ''comment''.
	conditionStart := comment indexOf: $(.
	conditionStart = 0 ifTrue: [ ^ self visitFortranComment: anEsopeCommentNode ].
	depth := 0.
	conditionEnd := 0.
	conditionStart to: comment size do: [ :i |
		conditionEnd = 0 ifTrue: [
			(comment at: i) = $( ifTrue: [ depth := depth + 1 ].
			(comment at: i) = $) ifTrue: [
				depth := depth - 1.
				depth = 0 ifTrue: [ conditionEnd := i ] ] ] ].
	conditionEnd = 0 ifTrue: [ ^ self visitFortranComment: anEsopeCommentNode ].

	span := self visitSpan: (anEsopeCommentNode at: ''span'').
	rest := comment copyFrom: conditionEnd + 1 to: comment size.
	offset := conditionEnd + (rest findFirst: [ :char | char isSeparator not ]).
	commandText := ''@_   '' , rest trim.
	"commandSpan starts where the ''c'' of ''c@_   '' would be if the command followed it: #esopeExtraSpan:comment:command: adds the 6 characters of the text before the command"
	commandSpan := { (span first x @ (span first y + offset - 6)) . span second }.
	command := self esopeCommand: commandText.
	extraSpan := self esopeExtraSpan: commandSpan comment: commandText command: command.
	data := { commandSpan . extraSpan } , command.
	segment := IASTEsopeSegCommand new
		sourceAnchor: (self makeIndexedAnchor: data second);
		esopeCommand: data third asLowercase;
		entityName: data fourth asLowercase;
		yourself.

	entities := self esopeIfConditionEntities: (comment copyFrom: conditionStart + 1 to: conditionEnd - 1) line: span first x column: span first y + conditionStart.
	^ entities , { segment }'.

(Smalltalk globals at: #EsopeEditTest) value: #JsonToIASTVisitorTest value: #testEsopeIfCommentStatement value: { { 'endColumn equals: 29' . 'endColumn equals: 27' } }.

add value: #IASTToFamixEsopeVisitor value: #visitSeginiCopy:named: value: 'visiting' value:
'visitSeginiCopy: aSegmentCommand named: aPairOfNames
	"segini new=old : the new segment is a copy of the old one.  The command is a FamixEsopeCommandSegcop that accesses the old pointer; newAccess is the access (a write) to the new pointer."

	| oldCommand esopeCommand newAccess |
	oldCommand := IASTEsopeSegCommand new
		sourceAnchor: aSegmentCommand sourceAnchor;
		esopeCommand: aSegmentCommand esopeCommand;
		entityName: aPairOfNames second;
		yourself.
	esopeCommand := (self newEsopeCommand: ''segcop'')
		sourceAnchor: (self visitIndexedFileAnchor: aSegmentCommand sourceAnchor);
		attributeAt: #entity put: oldCommand;
		accessor: stack top;
		yourself.
	self checkForModSuffix: esopeCommand.
	newAccess := self createFamixF77Access: (IASTVarAccess new
		entityName: aPairOfNames first;
		sourceAnchor: aSegmentCommand sourceAnchor;
		isWrite: true;
		yourself).
	newAccess isWrite: true.
	esopeCommand newAccess: newAccess.
	^ esopeCommand'.

fix value: #IASTToFamixEsopeVisitor value: #visitIASTEsopeSegCommand:
	value: [ :source | (source includesSubstring: 'visitSeginiCopy:') not ]
	value:
'visitIASTEsopeSegCommand: aSegmentCommand

	| esopeCommand names |
	names := $= split: aSegmentCommand entityName.
	(aSegmentCommand esopeCommand asLowercase = ''segini'' and: [ names size = 2 ])
		ifTrue: [ ^ self visitSeginiCopy: aSegmentCommand named: names ].

	esopeCommand := (self newEsopeCommand: aSegmentCommand esopeCommand)
		  sourceAnchor:
			  (self visitIndexedFileAnchor: aSegmentCommand sourceAnchor) ;
		  attributeAt: #entity put: aSegmentCommand ;
		  accessor: stack top ;
		  yourself.
		
	self checkForModSuffix: esopeCommand.
	^esopeCommand '.

(Smalltalk globals at: #EsopeEditTest) value: #IASTToFamixVisitorTest value: #testFamixEsopeCommandSeginiCopy value: { { 'self skip: #TODO.' . '' } }.

add value: #IASTToFamixEsopeVisitor value: #newPointerAttribute: value: 'private-creation' value:
'newPointerAttribute: aPointer
	"pointeur bar(n, m).titi in a segment: an attribute named bar, that is an Esope pointer to the segment titi.  The resolver gives it its declaredType."

	| text dot name attribute |
	text := aPointer entityName.
	dot := text lastIndexOf: $..
	name := (dot = 0
		ifTrue: [ text ]
		ifFalse: [ text copyFrom: 1 to: dot - 1 ]) copyUpTo: $(.
	attribute := (self newEntity: FamixFortranAttribute)
		sourceAnchor: (self visitIndexedFileAnchor: aPointer sourceAnchor);
		name: name trim;
		isEsope: true;
		yourself.
	dot > 0 ifTrue: [ attribute segment: (text copyFrom: dot + 1 to: text size) trim ].
	^ attribute'.

fix value: #IASTToFamixEsopeVisitor value: #visitIASTEsopeSegment:
	value: [ :source | (source includesSubstring: 'newPointerAttribute:') not ]
	value:
'visitIASTEsopeSegment: aSegment

	| segment |
	
	self flag: ''#TODO: if we have accesses in the segment, we have to deal with them
		see visitIASTVariable:
	''.
	
	segment := self model newSegment
		           sourceAnchor:
			           (self visitIndexedFileAnchor: aSegment sourceAnchor) ;
		           name: aSegment entityName asLowercase ;
		           yourself.
	stack push: segment.
	self createSymbolTableFor: segment.
	segment attributes: (aSegment declarations collect: [ :declaration |
			(declaration isKindOf: IASTEsopePointer)
				ifTrue: [ self newPointerAttribute: declaration ]
				ifFalse: [
					declaration isEsope: (self isAFortranIntrinsicType: declaration typeSpec entityName).
					declaration accept: (self spawn: IASTToFamixFortranVisitor) ] ]).

	^ stack pop'.

fix value: #IASTToFamixVisitorTest value: #testFamixEsopeSegmentWithPointeur
	value: [ :source | source includesSubstring: 'self skip:' ]
	value:
'testFamixEsopeSegmentWithPointeur

	"
      segment myseg
        integer a1(n)
c@_      pointeur a2(n).aseg
      end segment
	"

	| segment pointer entity attribute |
	segment := self defaultSegment: ''myseg'' with: { (#a1 -> #integer) }.
	pointer := IASTEsopePointer new
		           sourceAnchor: self defaultAnchor;
		           entityName: ''a2(n).aseg'';
		           yourself.
	segment declarations: { segment declarations first . pointer }.

	programFile body: { IASTSubroutine new
			 entityName: ''sub'';
			 body: { segment };
			 yourself }.

	entity := programFile accept: visitor.

	self assert: entity programUnits size equals: 1.

	"the model has the stub segment PSTR and the segment myseg"
	self assert: (visitor model allWithType: FamixEsopeSegment) size equals: 2.
	entity := (visitor model allWithType: FamixEsopeSegment) second.
	self assert: entity name equals: ''myseg''.
	self assert: entity isType.
	self assert: entity attributes size equals: 2.

	attribute := entity attributes first.
	self assert: attribute class equals: FamixFortranAttribute.
	self assert: attribute name equals: ''a1''.
	self assert: attribute declaredType name equals: ''integer''.

	attribute := entity attributes second.
	self assert: attribute class equals: FamixFortranAttribute.
	self assert: attribute name equals: ''a2''.
	self assert: attribute isEsope.
	self assert: attribute segment equals: ''aseg'''.

fix value: #IASTToFamixVisitorTest value: #testFamixEsopeSlashWithLitteralDim
	value: [ :source | source includesSubstring: 'target isNil' ]
	value:
'testFamixEsopeSlashWithLitteralDim

	"c  ptr.attr(/1)
	 s__(d__(ptr,attr),1)
	 The IAST of JsonToIASTVisitor for this source: the literal 1 is dropped,
	 and the pointer and the attribute are the two entities of one d__ inside
	 the s__.  The link between the two accesses is the attribute #parentAccess
	 (the old text of the test asked for #pointer, a name that no code sets).
	 An access that is not resolved has no #variable (the old text asked #target,
	 which is a collection of links in the present Famix)."

	| s__ entity pointerAccess attributeAccess |
	s__ := IASTVarEsoSl new
		sourceAnchor: self defaultAnchor;
		entities: { self defaultVarEsoAt: #( #ptr #attr ) };
		yourself.

	programFile body: { (IASTSubroutine new
			 entityName: ''sub'';
			 body: { s__ };
			 yourself) }.

	entity := programFile accept: visitor.

	self assert: entity programUnits size equals: 1.
	self assert: entity programUnits first accesses size equals: 2.

	entity programUnits first accesses do: [ :access | | var |
		var := access attributeAt: #entity ifAbsent: [ nil ].
		self assert: var isNotNil.
		var entityName = ''ptr''
			ifTrue: [ pointerAccess := access ]
			ifFalse: [ attributeAccess := access ] ].

	self assert: pointerAccess isNotNil.
	self assert: pointerAccess variable isNil.
	self assert: (pointerAccess attributeAt: #parentAccess ifAbsent: [ nil ]) isNil.

	self assert: attributeAccess isNotNil.
	self assert: attributeAccess variable isNil.
	self assert: (attributeAccess attributeAt: #entity ifAbsent: [ nil ]) entityName equals: ''attr''.
	self assert: (attributeAccess attributeAt: #parentAccess ifAbsent: [ nil ]) equals: pointerAccess'.

fix value: #JsonToIASTVisitor value: #visitBlock_data:
	value: [ :source | (source includesSubstring: 'makeIndexedAnchor') not ]
	value:
'visitBlock_data: aBlockDataNode
	| data |
	data := super visitBlock_data: aBlockDataNode.
	^IASTBlockData new
		sourceAnchor: (self makeIndexedAnchor: data first) ;
		entityName: data second ;
		body: data third ;
		yourself'.

fix value: #IASTToFamixFortranVisitor value: #visitIASTBlockData:
	value: [ :source | source includesSubstring: 'addAccess' ]
	value:
'visitIASTBlockData: aBlockData

	| blockdata |
	blockdata := self model newPUBlockdata
		sourceAnchor: (self visitIndexedFileAnchor: aBlockData sourceAnchor);
		name: aBlockData entityName;
		yourself.
	stack push: blockdata.

	"The accesses of the body (the names of a DATA statement) are not in the model: FamixF77PUBlockdata does not have accesses."

	"prossess the comments inside this progUnit"
	aBlockData localComments do: [ :comment |
		blockdata addComment: (comment accept: self) ].

	stack pop.
	^ blockdata'.

(Smalltalk globals includesKey: #FamixFortranImporterStandIn) ifFalse: [
	(FamixFortranImporter << #FamixFortranImporterStandIn
		package: 'EsopeImporter-Tests';
		yourself) install ].

add value: #FamixFortranImporterStandIn value: #f77parser value: 'accessing' value:
'f77parser
	"A stand-in for the outside program fortran-src-extras.  The command line of
	 the step is: <parser> ""<source file>"" > ""<json file>"" 2>&1.  The shell
	 gets the source file name as $0 and prints a fixed JSON text."

	^ ''sh -c ''''cat ./tmpStandInParser.json'''' '''.

fix value: #FamixFortranImporterTest value: #testFortranToJsonAST
	value: [ :source | source includesSubstring: 'self skip:' ]
	value:
'testFortranToJsonAST
	"The parser is an outside program (fortran-src-extras).  The stand-in class
	 answers a parser command that prints a fixed JSON text, so the test checks
	 the folder layout of the step."

	| standIn tempFolder jsonFile |
	jsonFile := ''./tmpStandInParser.json'' asFileReference.
	jsonFile writeStreamDo: [ :stream |
		stream nextPutAll: ''{"meta":{"miVersion":"fortran77","miFilename":"stand-in"},"program_units":[]}'' ].
	[
		self prepareWorkspace.
		standIn := FamixFortranImporterStandIn new.
		standIn errorHandler: FortranErrorManager new.
		standIn sourceFolder: importer sourceFolder.
		standIn includeFolder: importer includeFolder.

		standIn fortranToJsonAST.

		tempFolder := ''./tmpJsonImport'' asFileReference.
		self assert: tempFolder exists.
		self assert: tempFolder children size equals: 1.
		self assert: tempFolder children anyOne basename equals: ''src''.

		tempFolder := tempFolder / ''src''.
		self
			assertCollection: (tempFolder children collect: #basename)
			hasSameElements: #( ''f3.json'' ).
		self assert: (tempFolder / ''f3.json'') contents equals: jsonFile contents ]
			ensure: [ jsonFile ensureDelete ]'.


Smalltalk globals removeKey: #EsopeEditTest ifAbsent: [ ].
