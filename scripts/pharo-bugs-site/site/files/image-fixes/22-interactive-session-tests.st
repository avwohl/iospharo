"Fixes for the tests that skip because the test run is not an interactive
 session (2026-11-17).  With these entries and an interactive session
 (PHARO_INTERACTIVE=1 on our VM, a start without --headless on stock Cog) the
 28 tests of the group run.

 1. SpTextPresenterTest (and SpCodePresenterTest, which inherits the tests):
    testPropagateNaturalHeight, testPropagateNaturalHeightWithMultipleLines,
    testPropagateNaturalWidth began with `Smalltalk isHeadless ifTrue: [ ^ self skip ]`.
    In Pharo 14 SmalltalkImage>>isHeadless is deprecated and always answers
    true, so the 6 tests never ran on any VM.  The tests open a presenter in
    the Morphic world and compare widget sizes against font metrics; they need
    no native window.  Two of the three need no interactive session: the guard is
    removed.  testPropagateNaturalHeightWithMultipleLines needs a real display
    size (the fake-GUI world is 240@120, too small for six lines): its guard
    becomes isInteractive, as the deprecation text asks.
 2. MorphicWindowManagerTest>>testDeleteAWindowAndTaskBarActualized:
    TaskbarMorphForManager answers the manager's MorphicNativeWindow objects as
    its windows; TaskbarMorph>>updateTasks asks each task's morph
    `hasProperty: #isClosed`, which a MorphicNativeWindow does not understand
    (stock Cog: same DNU).  A native window answers #isClosed itself.
 3. SimulateMouseTest>>testSimulateRightClick: `Smalltalk tools toolNamed:
    #workspace) open` answers an StWindowPresenter in Pharo 14, not a Morph, so
    `simulateRightClick` is not understood (stock Cog: same DNU).  The test
    right-clicks the morph of the window: the window presenter's widget."

"These two need no interactive session: they pass in the fake-GUI world."
#( #testPropagateNaturalHeight #testPropagateNaturalWidth ) do: [ :sel |
	fix value: #SpTextPresenterTest value: sel
		value: [ :src | src includesSubstring: 'Smalltalk isHeadless ifTrue: [ ^ self skip ].' ]
		value: ((SpTextPresenterTest >> sel) sourceCode
			copyReplaceAll: 'Smalltalk isHeadless ifTrue: [ ^ self skip ].' with: '') ].

"This one needs a real display: it asserts a widget height of six text lines,
 and the fake-GUI world is 240@120 (widget height 47 on both VMs).  It runs in an
 interactive session, where the display is large enough."
fix value: #SpTextPresenterTest value: #testPropagateNaturalHeightWithMultipleLines
	value: [ :src | src includesSubstring: 'Smalltalk isHeadless ifTrue: [ ^ self skip ].' ]
	value: ((SpTextPresenterTest >> #testPropagateNaturalHeightWithMultipleLines) sourceCode
		copyReplaceAll: 'Smalltalk isHeadless ifTrue: [ ^ self skip ].'
		with: 'Smalltalk isInteractive ifFalse: [ ^ self skip ].').

"The new text holds the old text, so the guard also asks that the fix is not there."
fix value: #TaskbarMorph value: #updateTasks
	value: [ :src | (src includesSubstring: 'task morph hasProperty: #isClosed')
		and: [ (src includesSubstring: 'task morph isMorph') not ] ]
	value: ((TaskbarMorph >> #updateTasks) sourceCode
		copyReplaceAll: 'task morph hasProperty: #isClosed'
		with: '(task morph isMorph
					                              ifTrue: [ task morph hasProperty: #isClosed ]
					                              ifFalse: [ task morph isClosed ])').

fix value: #SimulateMouseTest value: #testSimulateRightClick
	value: [ :src | (src includesSubstring: 'adapter widget') not ]
	value: 'testSimulateRightClick

	| menu |
	morph := (Smalltalk tools toolNamed: #workspace) open adapter widget.
	menu := self menuOpenedDuring: [ morph simulateRightClick ].
	self assert: menu isNil not'.
