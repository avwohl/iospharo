"jordanmontt/pharo-instrumentation: three defects of the package (the two VMs
fail the same 17 - 4 tests the same way; the defects are in the package code
and in its test, not in a VM).

1. SourceCodeInstrumentator builds the source text of an instrumented method
from the two action blocks with 'aBlock asString'.  In Pharo 14
BlockClosure>>printOn: prints '[ ] in Class>>selector', not the source of the
block.  The compiled method then holds the text 'runInMetaLevel: [ ] in
Class>>setUp ]': the empty block [ ] (a ConstantBlockClosure) gets the unary
message #in, and every call of an instrumented method fails with
'ConstantBlockClosure does not understand #in'.  This gives the 6 errors
(testInstrument, testProfile, testProfileMethodWithTempVariables,
testProfileSeveralMethods, testProfileSeveralMethodsAndClasses,
testAfterHasLocalReturn) and the 2 failures with a counter of 0
(testAfterRaisesError, testProfileWithException: the test only asks for
'raise: Error', and the DNU is an Error, so the call of the method fails
before any action ran).  The repair adds SourceCodeInstrumentator>>actionSource:
(a String stays as it is; a block gives the source text of its AST node) and
uses it in instrument: and wrapBeforeActionFor:.  The tests that give strings
(the prototype trap tests) get the same text as before.

2. GraphCallerTest>>setUp calls MpCallGraphInstrumentator new and
RwiCallGraphInstrumentator new.  Both classes refuse new with 'Use
onProfiler: instead of new' since the refactoring of the package; the test
was not changed.  The repair makes the two profilers first and gives each
profiler an instrumentator made with onProfiler:.

3. The instrumentators send #unproxifiedMethod to each CompiledMethod of the
set of methods.  No loaded package and no method of the image defines
CompiledMethod>>unproxifiedMethod (the MethodProxies of the baseline, master,
has no such method).  The repair adds CompiledMethod>>unproxifiedMethod
(answers the receiver: an ordinary method is not a proxy).

4. The five GraphCallerTest tests run whole test packages two times, with a
proxy on each method of a package.  The time limit of SUnit is 10 s a test.
GraphCallerTest class>>defaultTimeLimit gives 10 minutes.  Only a run shows
whether the limit is needed."

add value: #SourceCodeInstrumentator value: #actionSource: value: 'profiling' value:
'actionSource: anAction
	"Answer the Smalltalk source text of anAction.  A String is the text already.
	 A block gives the source of its AST node: BlockClosure>>asString prints
	 ''[ ] in Class>>selector'' in Pharo 14, which is not code."

	^ anAction isString
		  ifTrue: [ anAction ]
		  ifFalse: [ anAction sourceNode sourceCode ]'.

fix value: #SourceCodeInstrumentator value: #instrument:
	value: [ :source | source includesSubstring: 'afterAction asString' ]
	value:
'instrument: aMethod

	| beforeActionString afterActionString pragmasString originalCodeString instrumentedMethodCode |
	beforeActionString := self wrapBeforeActionFor: aMethod.
	afterActionString := self wrapWithMetaSafeMechanism: (self actionSource: afterAction).
	originalCodeString := ''[ '' , aMethod methodNode body sourceCode , '' ]''.
	pragmasString := aMethod pragmas inject: '''' into: [ :sum :node | sum , node asString , String cr].

	instrumentedMethodCode := String streamContents: [ :stream |
		stream << pragmasString <<
			beforeActionString << String cr
			<< ''    '' << originalCodeString << '' ensure: ['' << String cr <<
			''    '' << afterActionString << '' ]'' ].
	
	instrumentedMethodCode := aMethod sourceNode selectorAndArgumentNames , String cr , instrumentedMethodCode.
	
	aMethod methodClass compile: instrumentedMethodCode'.

fix value: #SourceCodeInstrumentator value: #wrapBeforeActionFor:
	value: [ :source | source includesSubstring: 'beforeAction asString' ]
	value:
'wrapBeforeActionFor: aMethod

	^ self wrapWithMetaSafeMechanism:
		  ''[ '' , (self actionSource: beforeAction) , '' cull: '' , aMethod nameForProfiler , '' ]'''.

fix value: #GraphCallerTest value: #setUp
	value: [ :source | source includesSubstring: 'MpCallGraphInstrumentator new' ]
	value:
'setUp

	super setUp.
	mpProfiler := CallGraphProfiler new.
	mpProfiler instrumentator: (MpCallGraphInstrumentator onProfiler: mpProfiler).
	rwiProfiler := CallGraphProfiler new.
	rwiProfiler instrumentator: (RwiCallGraphInstrumentator onProfiler: rwiProfiler)'.

add value: #CompiledMethod value: #unproxifiedMethod value: '*InstrumentationProfilers' value:
'unproxifiedMethod
	"An ordinary method is not a proxy: it is the unproxified method."

	^ self'.

add value: #'GraphCallerTest class' value: #defaultTimeLimit value: 'accessing' value:
'defaultTimeLimit
	"A test runs whole test packages twice, with a method proxy on each method of a package."

	^ 10 minutes'.
