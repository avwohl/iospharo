Smalltalk at: #FLCBSkip put: 'Smalltalk compiler compilationContext optionCleanBlockClosure ifTrue: [ ^ self skip ].'.
{ {#FLFullHeaderSerializationTest. #testPostMaterializationActions}.
  {#FLFullHeaderSerializationTest. #testPreMaterializationActions}.
  {#FLHeaderSerializationTest. #testPostMaterializationActions}.
  {#FLHeaderSerializationTest. #testPreMaterializationActions}.
  {#FLSortedCollectionSerializationTest. #testSortedCollection}.
  {#FLSortedCollectionSerializationTest. #testSortedCollectionWithClassVariableChanges}.
  {#FLSortedCollectionSerializationTest. #testSortedCollectionWithClassVariableReference}.
  {#FLUserGuidesTest. #testDemo} } do: [ :p |
	fix value: p first value: p second
		value: [ :s | s includesSubstring: (Smalltalk at: #FLCBSkip) ]
		value: (((Smalltalk at: p first) >> p second) sourceCode copyReplaceAll: (Smalltalk at: #FLCBSkip) with: (String empty)) ].

"With optionCleanBlockClosure the method 'm ^ [ 42 yourself ]' compiles to the same
 bytecodes as 'm ^ 42' (push literal, return top): the clean block is a literal.  The
 test must compare a method without a closure against a method with a different
 bytecode sequence; sending #value to the block makes the bytecodes differ."
fix value: #FLGlobalEnvironmentTest value: #testCompiledMethodChanged
	value: [ :s | s includesSubstring: 'optionCleanBlockClosure ifTrue: [ ^ self skip ]' ]
	value: 'testCompiledMethodChanged
	"A compiled methods should be serialized as global by default.
	On materialization, it must be found in the global environment, and the bytecodes hash must be the same.
	Else, raise a proper error."
	
	| classA classB |
	classA := self classFactory silentlyNewClass.
	self classFactory
		silentlyCompile:  ''m  ^ 42''
		in: classA.
	classB := self classFactory silentlyNewClass.
	self classFactory
		silentlyCompile: ''m  ^ [ 42 yourself ] value''
		in: classB.

	self materializer environment: (Dictionary new
		at: classA name put: classB;
		yourself).

	self serialize: classA >> #m.

	self
		should: [ self materialized ]
		raise: FLMethodChanged 
		description: ''Serialized and materialized methods should have the same bytecodes.''' .
