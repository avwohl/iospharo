fix value: #RunnerLongDurationTest value: #testRunningFromAClass value: [ :s | s includesSubstring: 'self skip' ] value: 'testRunningFromAClass

	runner runClass: self longTestClass.
	self assert: runner runCount equals: 1.
	self assert: runner passedCount equals: 1'.
fix value: #RunnerLongDurationTest value: #testRunningFromACollectionOfClasses value: [ :s | s includesSubstring: 'self skip' ] value: 'testRunningFromACollectionOfClasses

	runner runClasses: { LongTest . LongTest . LongTest }.
	self assert: runner runCount equals: 3.
	self assert: runner passedCount equals: 3'.
