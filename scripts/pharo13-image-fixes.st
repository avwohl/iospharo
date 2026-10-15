"pharo13-image-fixes.st -- fixes for defects in the Pharo 13 image and its
tests.  Each defect fails the same way on stock Cog with a pristine image; see
docs/image_issues.md.  File this in when an image is prepared for the SUnit
sweep (after setup_fake_gui.st).  Each fix recompiles one method in its own
protocol and prints one FIXED line."

| fix |
fix := [ :cls :sel :src |
	| proto |
	proto := (cls includesSelector: sel)
		ifTrue: [ (cls >> sel) protocolName ]
		ifFalse: [ 'as yet unclassified' ].
	cls compile: src classified: proto.
	Transcript show: 'FIXED ', cls name, '>>', sel; cr ].

"1. The trait composition names TAsStringCommaAndDelimiterTest, which the
image renamed to TAsStringOnDelimiterTest; the compiler refuses the
undeclared name (OCClassBuilderTest>>testCreateNormalClassWithTraitComposition)."
(Smalltalk globals includesKey: #TAsStringCommaAndDelimiterTest) ifFalse: [
	fix value: OCClassBuilderTest value: #testCreateNormalClassWithTraitComposition value:
'testCreateNormalClassWithTraitComposition

	| class |
	class := self buildClassFromDefinition:
		         ''Class << #TestSubClass traits: TAddForUniquenessTest + TIncludesWithIdentityCheckTest + TCloneTest + TCopyTest + TRemoveTest + TGrowableTest + TStructuralEqualityTest + TPrintTest + TAsStringOnDelimiterTest + TConvertTest + TConvertAsSortedTest + TConcatenationEqualElementsRemovedTest + TOccurrencesTest + (TCreationWithTest - {#testOfSize. #collectionClass}) + (TSetArithmetic - {#collectionClass})''.

	self assert: (class isComposedBy: TAddForUniquenessTest).
	self assert: (class isComposedBy: TIncludesWithIdentityCheckTest)' ].

"2. The UI layer's external dependencies now include Reflectivity; the
allowlist predates it (SystemDependenciesTest>>testExternalUIDependencies)."
fix value: SystemDependenciesTest value: #knownUIDependencies value:
'knownUIDependencies
	"ideally this list should be empty"

	^ #( ''Athens-Cairo'' ''Athens-Core'' #''Refactoring-Critics'' #''Refactoring-Environment'' #Reflectivity ''Reflectivity-Tools'' #Shout #''Tool-Diff'' #''HeuristicCompletion-Model''
	     ''NECompletion-Morphic'' #VariablesLibrary #''Tools-CodeNavigation'' #''Spec2-CommonWidgets'' #''NewTools-Scopes'' #''NewTools-FileBrowser'' )'.

"3. The argument homeContext shadows the instance variable homeContext
(ReleaseTest>>testNoShadowedVariablesInMethods).  Renamed; the method used
the argument."
fix value: FastStepThroughController value: #findNextContext:having:fixing: value:
'findNextContext: originContext having: aHomeContext fixing: buggyContext

	| blockContext fixedContext |
	"Find the next context, restoring blocks, removing HaltingBlocks"
	blockContext := self findNextContext: originContext.

	"a. Here step through exits the block is being evaluated."
	(self stepOutOfBlock: blockContext from: originContext to: aHomeContext)
		ifNotNil: [:targetContext | ^ targetContext ].

	"b. Here step through behaves like step over."
	blockContext == originContext ifTrue: [ ^ originContext ].

	"c. Here step through steps into a block."
	fixedContext := self findFirstContext: buggyContext
								  withReceiverOfClass: HaltingBlock.
	blockContext sender: (fixedContext ifNotNil: [fixedContext sender ]).

	^ blockContext'.

"4. The forked process sets the dynamic variable only when it first runs, and
fork does not run it: the test then passed only if something preempted the
test process while the debugger was built (a Delay firing).  Let it run to
its wait first (StDebuggerTest>>testDynamicVariableEvaluation)."
fix value: StTestDebuggerProvider value: #debuggerWithDynamicVariableSuspendedContext value:
'debuggerWithDynamicVariableSuspendedContext
	|process|
	process := [
	           TestDynamicVariable value: 42 during: [
		           self assert:
			           (thisProcess psValueAt: TestDynamicVariable soleInstance index) = 42.
					  100 milliSeconds wait.
		           thisProcess suspend ] ] fork.
	1 to: 1000 do: [ :i |
		(process psValueAt: TestDynamicVariable soleInstance index) ifNil: [ Processor yield ] ].
	session := DebugSession
		           named: ''test session providerSessionFor:''
		           on: process
		           startedAt: process suspendedContext.
	session exception:
		(OupsNullException fromSignallerContext: process suspendedContext).
	^ self newDebugger'.

"5 and 6. The tests read actual and expected from the assertion's description
block by index: actual at 2, expected at 1.  The block's copied values are
(actual, expected) in this image, so both tests compared swapped values
(StDebuggerTest>>testUpdateLayoutForContextsIfAssertionFailure and
StDebuggerInspectorTest>>testUpdateLayoutForContextsWithAssertionFailure,
'Got 4 instead of 2').  The inspector's answer is right."
fix value: StDebuggerTest value: #testUpdateLayoutForContextsIfAssertionFailure value:
'testUpdateLayoutForContextsIfAssertionFailure

	| assertionFailure currentLayout expectedLayout |
	debugger := StTestDebuggerProvider new
		            debuggerWithFailingAssertionContext.
	session := debugger session.
	debugger application: StDebugger currentApplication.
	debugger initialize.
	debugger updateInspectorFromContext: session interruptedContext.
	assertionFailure := debugger inspector assertionFailure.
	currentLayout := debugger inspector layout.
	expectedLayout := StDebuggerInspector new assertionFailureLayout.
	StDebuggerInspector maximizeAssertionSpec: false.
	self assert: currentLayout direction equals: expectedLayout direction.
	self
		assertCollection: currentLayout children first class
		equals: expectedLayout children first class.
	self
		assertCollection: currentLayout children first direction
		equals: expectedLayout children first direction.
	self
		assertCollection: currentLayout children second
		equals: expectedLayout children second.
	self
		assert: currentLayout positionOfSlider
		equals: expectedLayout positionOfSlider.
	self
		assert: currentLayout children size
		equals: expectedLayout children size.
	self
		assert: assertionFailure actualObject
		equals:
		((session interruptedContext tempNamed: #aStringOrBlock) at: 1).
	self
		assert: assertionFailure expectedObject
		equals:
		((session interruptedContext tempNamed: #aStringOrBlock) at: 2)'.
fix value: StDebuggerInspectorTest value: #testUpdateLayoutForContextsWithAssertionFailure value:
'testUpdateLayoutForContextsWithAssertionFailure

	| assertionFailure |
	debugger := StTestDebuggerProvider new
		            debuggerWithFailingAssertionContext.
	session := debugger session.

	inspector
		updateLayoutForContexts: session interruptedContext
		isAssertionFailure: true.
	assertionFailure := inspector assertionFailure.
	self
		assert: inspector currentLayoutSelector
		equals: inspector assertionLayoutSelector.
	self
		assert: assertionFailure actualObject
		equals:
		((session interruptedContext tempNamed: #aStringOrBlock) at: 1).
	self
		assert: assertionFailure expectedObject
		equals:
		((session interruptedContext tempNamed: #aStringOrBlock) at: 2)'.
