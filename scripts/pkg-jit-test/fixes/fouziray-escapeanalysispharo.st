"fouziray/EscapeAnalysisPharo (row fouziray-escapeanalysispharo).
 The record of the repair: docs/upstream/package-repairs.md

 The image has Pharo 14 build 791. The package source is for an older AST API:
 OCMessageNode>>selector: wants an OCSelectorNode, not a String (AST-Core/OCMessageNode.class.st line 421).
 MetacelloMonticelloLoader>>loadProject: is now loadProject:engine:.
 Three methods hold a left-over breakpoint (1 halt). A Halt that nothing handles ends the headless image with exit 1."

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

fix value: #ObjectInlining value: #assignObjectCreatedInlineToTmp:
	value: [ :source | source includesSubstring: '1 halt' ]
	value:
'assignObjectCreatedInlineToTmp: method
method ast'.

fix value: #ObjectInliningTest value: #testHoneyGinger
	value: [ :source | source includesSubstring: '1 halt' ]
	value:
'testHoneyGinger
| candidates astt compiler method candidatesAssociatedWithTV newMethod|
objInliner allowedDepth: 8.
method := HGSimulator class >> #bouncingBalls.
newMethod :=	objInliner assignObjectCreatedInlineToTmp: method.
	candidates := objInliner findCandidatesOf: newMethod .
candidatesAssociatedWithTV := candidates collect: [ :c | c -> (objInliner escapeAnalyzer  findVariableByNameSymbol:  c variable name) ].
objInliner inlineRefactorCandidateObjects: candidatesAssociatedWithTV in: newMethod.
self assert: candidatesAssociatedWithTV first key equals: (ASTAssignmentNode
		 variable: (ASTVariableNode named: ''point1'')
		 value: (ASTMessageNode
				  receiver: (ASTVariableNode named: ''PointInlined'')
				  selector: (OCSelectorNode value: #new))).'.

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
