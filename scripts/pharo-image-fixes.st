"pharo-image-fixes.st -- fixes for defects in the Pharo 13 and Pharo 14 images
and their tests.  Each defect fails the same way on stock Cog with a pristine
image; see docs/image_issues.md.  File this in when an image is prepared for
the SUnit sweep (after setup_fake_gui.st).

Every fix is guarded on the defect being present in the CURRENT method source.
A fix whose class is absent, or whose defect upstream already repaired, does
nothing and prints one SKIP line.  A fix that applies recompiles one method in
its own protocol and prints one FIXED line.  The file looks up class names by
symbol, so it compiles on an image that lacks a class.  The same file
therefore works on Pharo 13 and on Pharo 14."

| fix swapped |
"fix: class symbol, selector, a guard block that answers true when the defect
 is in the current source, and the replacement source."
fix := [ :clsName :sel :guard :src |
	| cls |
	cls := Smalltalk globals at: clsName ifAbsent: [ nil ].
	(cls notNil and: [ cls includesSelector: sel ])
		ifFalse: [ Transcript show: 'SKIP ', clsName, '>>', sel, ' (absent)'; cr ]
		ifTrue: [
			(guard value: (cls >> sel) sourceCode)
				ifFalse: [ Transcript show: 'SKIP ', clsName, '>>', sel, ' (defect not present)'; cr ]
				ifTrue: [
					cls compile: src classified: (cls >> sel) protocolName.
					Transcript show: 'FIXED ', clsName, '>>', sel; cr ] ] ].

"True when the source reads the assertion's actual object from index 2 of the
 description block (fixes 5 and 6)."
swapped := [ :source | | i |
	i := source findString: 'actualObject'.
	i > 0 and: [ (source copyFrom: i to: (i + 120 min: source size)) includesSubstring: 'at: 2' ] ].

"1. The trait composition names TAsStringCommaAndDelimiterTest, which the
image renamed to TAsStringOnDelimiterTest; the compiler refuses the
undeclared name (OCClassBuilderTest>>testCreateNormalClassWithTraitComposition).
Pharo 14 no longer names the old class."
fix value: #OCClassBuilderTest value: #testCreateNormalClassWithTraitComposition
	value: [ :source | (source includesSubstring: 'TAsStringCommaAndDelimiterTest')
		and: [ (Smalltalk globals includesKey: #TAsStringCommaAndDelimiterTest) not ] ]
	value:
'testCreateNormalClassWithTraitComposition

	| class |
	class := self buildClassFromDefinition:
		         ''Class << #TestSubClass traits: TAddForUniquenessTest + TIncludesWithIdentityCheckTest + TCloneTest + TCopyTest + TRemoveTest + TGrowableTest + TStructuralEqualityTest + TPrintTest + TAsStringOnDelimiterTest + TConvertTest + TConvertAsSortedTest + TConcatenationEqualElementsRemovedTest + TOccurrencesTest + (TCreationWithTest - {#testOfSize. #collectionClass}) + (TSetArithmetic - {#collectionClass})''.

	self assert: (class isComposedBy: TAddForUniquenessTest).
	self assert: (class isComposedBy: TIncludesWithIdentityCheckTest)'.

"2. The UI layer's external dependencies now include Reflectivity; the
allowlist predates it (SystemDependenciesTest>>testExternalUIDependencies).
Pharo 14 has no SystemDependenciesTest."
fix value: #SystemDependenciesTest value: #knownUIDependencies
	value: [ :source | (source includesSubstring: '#Reflectivity') not ]
	value:
'knownUIDependencies
	"ideally this list should be empty"

	^ #( ''Athens-Cairo'' ''Athens-Core'' #''Refactoring-Critics'' #''Refactoring-Environment'' #Reflectivity ''Reflectivity-Tools'' #Shout #''Tool-Diff'' #''HeuristicCompletion-Model''
	     ''NECompletion-Morphic'' #VariablesLibrary #''Tools-CodeNavigation'' #''Spec2-CommonWidgets'' #''NewTools-Scopes'' #''NewTools-FileBrowser'' )'.

"3. The argument homeContext shadows the instance variable homeContext
(ReleaseTest>>testNoShadowedVariablesInMethods).  Renamed; the method used
the argument.  Pharo 14 renamed the argument itself."
fix value: #FastStepThroughController value: #findNextContext:having:fixing:
	value: [ :source | source includesSubstring: 'having: homeContext fixing:' ]
	value:
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
its wait first (StDebuggerTest>>testDynamicVariableEvaluation).  Pharo 14
rewrote the method with a Semaphore."
fix value: #StTestDebuggerProvider value: #debuggerWithDynamicVariableSuspendedContext
	value: [ :source | (source includesSubstring: 'Semaphore') not ]
	value:
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
'Got 4 instead of 2').  The inspector's answer is right.  Pharo 14 reads
index 1 for the actual object already."
fix value: #StDebuggerTest value: #testUpdateLayoutForContextsIfAssertionFailure
	value: swapped
	value:
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
fix value: #StDebuggerInspectorTest value: #testUpdateLayoutForContextsWithAssertionFailure
	value: swapped
	value:
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

"7. UIManager>>nonInteractiveManager answers NonInteractiveUIManager
beDefault, a send to the CLASS, and the class has no #beDefault.  Every default
manager but NonInteractiveUIManager itself then fails in UIManager
class>>nonInteractiveDuring: (TFUFFICallbackTest>>testCallbackWithErrorHanlded).
Stock Cog fails the same way once MorphicUIManager is the default, which the
fake-GUI prep and the runner install.  Pharo 13 has the same text."
fix value: #UIManager value: #nonInteractiveManager
	value: [ :source | source includesSubstring: '^NonInteractiveUIManager beDefault' ]
	value:
'nonInteractiveManager
	"Answer an instance of non-interactive manager, which will be used when image runs headless.
	 We put it here, so subclasses can override it."

	^ NonInteractiveUIManager new beDefault'.
