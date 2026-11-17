"TaskIt: the tests of firstCompleteOf: depend on the order of two callbacks
(2026-11-17).

TKTFuture>>firstCompleteOf: gives each of the two futures a callback, and the
callback that runs first decides the result.  A callback is a task of the
runner, and the runner of the test profile is a pool of 4 workers.  The four
tests give the first future its result and give the second future its result
at once.  They then expect the result of the first.  When a process of a
higher priority runs between the two callbacks, the VM puts the worker of the
first callback behind the worker of the second (preemptionYields), and the
second callback decides.  The tests then fail with 'Got nil instead of 5'
(one failure in the Pharo 14 sweep of 2026-11-17 on our VM; 7 of 900 runs in
scripts/cogdiff/taskit_pool_stress.st before the repair).

'The first future is faster' means that its callback ran before the second
future has a result.  Each test now waits for the result of the combined
future (at most 1 second) before the second future gets its result.  The
assertions are the same."

fix value: #TKTFutureTest value: #testFutureFirstCompleteOfSuccessIfFirstSuccessFaster
	value: [ :source | (source includesSubstring: 'waitForCompletion') not ]
	value:
'testFutureFirstCompleteOfSuccessIfFirstSuccessFaster
	| future got secondFuture error first |
	future := TKTFuture new.
	secondFuture := TKTFuture new.
	first := future firstCompleteOf: secondFuture.
	first onSuccessDo: [ :v | got := v ].
	future deploySuccess: 5.
	"The callbacks of the two futures are tasks of a worker pool, with no fixed order. The first future is faster when its callback ran before the second future has a result."
	first waitForCompletion: 1 second.
	error := Error new.
	[ error signal ]
		on: Error
		do: [ :e | secondFuture deployFailure: e ].
	50 milliSecond wait.
	self assert: got equals: 5'.

fix value: #TKTFutureTest value: #testFutureFirstCompleteOfSuccessIfSecondSuccessFaster
	value: [ :source | (source includesSubstring: 'waitForCompletion') not ]
	value:
'testFutureFirstCompleteOfSuccessIfSecondSuccessFaster

	| future got secondFuture first |
	future := TKTFuture new.
	secondFuture := TKTFuture new.

	first := future firstCompleteOf: secondFuture.
	first onSuccessDo: [ :v | got := v ].

	secondFuture deploySuccess: 5.
	"The callbacks of the two futures are tasks of a worker pool, with no fixed order. The second future is faster when its callback ran before the first future has a result."
	first waitForCompletion: 1 second.
	future deployFailure: Error new.
	50 milliSecond wait.

	self assert: got equals: 5'.

fix value: #TKTFutureTest value: #testFutureFirstCompleteOfFailsIfFirstFailsFaster
	value: [ :source | (source includesSubstring: 'waitForCompletion') not ]
	value:
'testFutureFirstCompleteOfFailsIfFirstFailsFaster

	| future got secondFuture error first |
	future := TKTFuture new.
	secondFuture := TKTFuture new.

	first := future firstCompleteOf: secondFuture.
	first onFailureDo: [ :v | got := v ].

	error := Error new.
	[ error signal ]
		on: Error
		do: [ :e | future deployFailure: e ].
	"The callbacks of the two futures are tasks of a worker pool, with no fixed order. The first future is faster when its callback ran before the second future has a result."
	first waitForCompletion: 1 second.
	secondFuture deploySuccess: 1.
	50 milliSecond wait.

	self assert: got equals: error'.

fix value: #TKTFutureTest value: #testFutureFirstCompleteOfFailsIfSecondFailsFaster
	value: [ :source | (source includesSubstring: 'waitForCompletion') not ]
	value:
'testFutureFirstCompleteOfFailsIfSecondFailsFaster

	| future got secondFuture error first |
	future := TKTFuture new.
	secondFuture := TKTFuture new.

	first := future firstCompleteOf: secondFuture.
	first onFailureDo: [ :v | got := v ].

	error := Error new.
	[ error signal ]
		on: Error
		do: [ :e | secondFuture deployFailure: e ].
	"The callbacks of the two futures are tasks of a worker pool, with no fixed order. The second future is faster when its callback ran before the first future has a result."
	first waitForCompletion: 1 second.
	future deploySuccess: 1.
	50 milliSecond wait.

	self assert: got equals: error'.
