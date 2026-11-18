"TaskIt: the andThen: callbacks of one future run in any order, and a callback can be lost (2026-11-17).

Part 1: the order of andThen:.  The class comment of TKTFuture says: 'All
futures chained using the andThen: combinator are guaranteed to be executed
sequentially (in contrast to normal callbacks)', and the example of the
comment is a cascade (andThen: ...; andThen: ...).
TKTFutureTest>>testFutureAndThenExecutesInOrder does the same: a cascade of
three andThen: sends to ONE future, then deploySuccess:, and it asserts
#(1 2 3).  TKTFuture>>andThen: registers its block as a normal callback
(onSuccessDo:) of the receiver, so in a cascade all three blocks are callbacks
of the same future.  deploySuccess: schedules one task for each callback on
the runner of the future, and the common runner is a pool with several
workers: the three blocks run in parallel and the order depends on the
workers.  The test failed once in a full sweep (patched Cog, 2026-11-17: 'Got
#(2 3 1) instead of #(1 2 3)') and passed in about ten sweeps before.  A loop
that runs the test body 1000 times does not show the race on either VM
(1000 of 1000 give #(1 2 3), stock Cog and our VM), so the test below forces
the order.

Repair (T2): a future remembers the future that its last andThen: answered
(instance variable andThenTail).  The next andThen: of the same future runs
its block only when that earlier future has its value, that is, after the
block of the earlier andThen: ended.  A chain (a andThen: ...) andThen: ...
is unchanged: each future there has one andThen: only.  The future of a block
gets its value in an ensure:, so a block that ends by something that is not
an Error (a Halt, the end of its process) does not stop the later blocks (the
pristine code runs the later blocks in that case).

Part 2: callback registration and deployment are not atomic.  onSuccessDo:
reads 'result' (TKTFutureNotFinished) and then the state adds the block to
resultCallbacks; deploySuccess: installs the result and then walks
resultCallbacks.  A deploy by another process between the read and the add
leaves the callback unscheduled for ever.  The pristine cascade registers all
callbacks in the process of the caller, before the deploy, so the window is
not used there; the chaining of Part 1 registers 'previous onSuccessDo:' in a
worker while another worker can deploy 'previous', so Part 1 needs Part 2.
Repair: instance variable lock (a Mutex; made in initialize, and on demand by
criticalLock for a future that is older than the entry).  deploySuccess: and
deployFailure: install the result and copy the callbacks inside the critical
section, and schedule the tasks and signal valueSemaphore outside of it (a
TKTLocalProcessTaskRunner runs a task in the calling process).  onSuccessDo:
and onFailureDo: read the state inside the section; a not-finished state
registers inside the section, a finished state is asked after the section
(it does not change after that).  Other users of result, resultCallbacks and
exceptionCallbacks: installResult: is called only by the two deploy methods
(now inside the section, and its error for a second deploy leaves the section
through the critical: unwinding); isFinished, isSuccess, isFailure,
printOn:, synchronizeTimeout: and waitForCompletion: do one read of result and
no write, and a finished state is final; addSuccessCallback: and
addFailureCallback: are called by the not-finished state only, from
onSuccessDo:/onFailureDo: (inside the section); the combinators use only
onSuccessDo:, onFailureDo:, deploySuccess: and deployFailure:.  Not changed:
valueSemaphore gives one signal for one waiter, so two processes in
waitForCompletion: on one future are not both woken (a defect that is not in
the scope of this entry).

Tests (3 new methods of TKTFutureTest), measured with this entry
(scripts/vmrun.sh prep, then test --repeat; 'before' = an image with the
three tests and none of the repairs, 5 runs; 'after' = 20 runs):
  testFutureAndThenExecutesInOrderWithSlowFirstCallback (the first block waits
      100 ms): before FAIL 5 of 5 on stock Cog and on our VM ('Got #(2 3)
      instead of #(1 2 3)'; a loop that waits for all three adds saw #(2 3 1)
      in 20 of 20 runs on each VM); after PASS 20 of 20 on stock Cog, patched
      Cog and our VM.
  testCallbackRegisteredWhileAnotherProcessDeploysIsScheduled (the state of
      the future is a TKTFutureStateWithRegistrationHook, a new subclass of
      TKTFutureNotFinished in TaskIt-Tests, that forks a deploy and waits
      200 ms for the end of the fork before it registers; the first form of
      the test made an anonymous subclass in the test, and the numbers are of
      that form):
      before FAIL 5 of 5 on each VM ('Denial failed': the callback is never
      scheduled, so it does not run within 2 s); after PASS 20 of 20 on each VM.
  testFutureAndThenRunsLaterBlocksWhenABlockEndsItsProcess (the first block
      terminates its process; the runner of the future makes a process for
      each task): PASS 5 of 5 before (the pristine code does not chain) and
      PASS 20 of 20 after on each VM.  With the chaining and no ensure: the
      test gives an error (TKTTimeoutException) on both VMs.
  Stress of the entry: 3000 cascades of three short blocks, waiting for
  the last future: 3000 of 3000 give #(1 2 3) and 0 futures stay unfinished,
  on stock Cog and on our VM.
  The final form of the entry (with the class
  TKTFutureStateWithRegistrationHook): TKTFutureTest as one suite, 3 runs
  on each VM: 52 of 52 pass on stock Cog and on our VM.  Full sweeps with
  entries 1 to 35 (2026-11-17): our VM (sweep14-new13) and the patched Cog
  (sweepcogp14-new10) each have 39465 tests, 39377 pass, 0 failures, 0
  errors, and the same 88 tests that do not pass as the sweeps before.

Why the state class is a class of TaskIt-Tests and not an anonymous class
made in the test: the first form compiled the two methods in the test.  The
compile makes a system announcement.  In a run of the whole class as one
suite from an eval, the block of EpMonitor>>subscribeToSystemAnnouncer then
sent shouldLogWithEpicea to nil (the test environment answered no test case),
and the test had an error on stock Cog and on our VM.  The runners of the
sweeps set the test case of the environment and did not show it.  The cause
is not found.  The tests of entry 33, which compile into an anonymous class,
pass in the same kind of suite run."


#( 'andThenTail' 'lock' ) do: [ :each |
	(TKTFuture hasInstVarNamed: each) ifFalse: [ TKTFuture addInstVarNamed: each ] ].

(Smalltalk globals includesKey: #TKTFutureStateWithRegistrationHook) ifFalse: [
	(TKTFutureNotFinished << #TKTFutureStateWithRegistrationHook
		slots: { #hook };
		tag: 'Futures';
		package: 'TaskIt-Tests';
		yourself) install.
	(Smalltalk globals at: #TKTFutureStateWithRegistrationHook) comment:
		'A not-finished state of a future for a test: before it registers a success callback it runs a block with the future.  The test uses the block to deploy the result from another process at exactly that point.' ].

add value: #TKTFutureStateWithRegistrationHook value: #hook: value: 'accessing'
	value:
'hook: aBlock
	hook := aBlock'.

add value: #TKTFutureStateWithRegistrationHook value: #onSuccessDo:inFuture: value: 'callbacks'
	value:
'onSuccessDo: aBlock inFuture: aFuture
	hook value: aFuture.
	^ super onSuccessDo: aBlock inFuture: aFuture'.

add value: #TKTFuture value: #criticalLock value: 'private'
	value:
'criticalLock
	"The mutex that makes the registration of a callback and the deployment of the result atomic.  A future that was made before this method existed has no mutex: the first reader makes the mutex, at the highest priority, so two processes cannot make two."

	lock ifNil: [ [ lock ifNil: [ lock := Mutex new ] ] valueUnpreemptively ].
	^ lock'.

fix value: #TKTFuture value: #initialize value: [ :s | (s includesSubstring: 'Mutex') not ]
	value:
'initialize

	super initialize.
	result := TKTFutureNotFinished new.
	runner := TKTConfiguration runner.

	valueSemaphore := Semaphore new.
	lock := Mutex new.

	exceptionCallbacks := OrderedCollection new.
	resultCallbacks := OrderedCollection new'.

fix value: #TKTFuture value: #deploySuccess: value: [ :s | (s includesSubstring: 'criticalLock') not ]
	value:
'deploySuccess: aValue
	"The result and the list of callbacks are taken in one step.  The callback tasks are scheduled outside of the critical section: a runner can run a task in the calling process."

	| callbacks |
	self criticalLock critical: [
		self installResult: (TKTFutureSuccess new value: aValue).
		callbacks := resultCallbacks copy ].
	callbacks
		do: [ :each | self scheduleCallbackTask: [ each value: aValue ] ].
	valueSemaphore signal'.

fix value: #TKTFuture value: #deployFailure: value: [ :s | (s includesSubstring: 'criticalLock') not ]
	value:
'deployFailure: aValue
	"See deploySuccess:."

	| callbacks |
	self criticalLock critical: [
		self installResult: (TKTFutureFailure new value: aValue).
		callbacks := exceptionCallbacks copy ].
	callbacks
		do: [ :each | self scheduleCallbackTask: [ each value: aValue ] ].
	valueSemaphore signal'.

fix value: #TKTFuture value: #onSuccessDo: value: [ :s | (s includesSubstring: 'criticalLock') not ]
	value:
'onSuccessDo: aCallback
	"When the result is not finished, the callback is registered in the critical section of the deployment, so the deployment cannot walk the callbacks between the test and the registration.  When the result is finished, the finished state schedules the callback (the state does not change after that)."

	| state |
	self criticalLock critical: [
		state := result.
		state isFinished ifFalse: [
			state onSuccessDo: aCallback inFuture: self.
			^ self ] ].
	state onSuccessDo: aCallback inFuture: self'.

fix value: #TKTFuture value: #onFailureDo: value: [ :s | (s includesSubstring: 'criticalLock') not ]
	value:
'onFailureDo: aBlockClosure
	"See onSuccessDo:."

	| state |
	self criticalLock critical: [
		state := result.
		state isFinished ifFalse: [
			state onFailureDo: aBlockClosure inFuture: self.
			^ self ] ].
	state onFailureDo: aBlockClosure inFuture: self'.

fix value: #TKTFuture value: #andThen: value: [ :s | (s includesSubstring: 'andThenTail') not ]
	value:
'andThen: aBlockClosure
	"The blocks of several andThen: sends to one future run one after the other, in the order of the sends.  The tail is the future that the last andThen: of the receiver answered: it has its value when the block of that andThen: ended.  The future of a block gets its value in an ensure:, so a block that ends by something that is not an Error (a Halt, the end of its process) does not stop the later blocks."

	| future previous |
	future := TKTFuture new.
	previous := andThenTail.
	andThenTail := future.
	self onSuccessDo: [ :got | | run |
		run := [
			[ [ aBlockClosure value: got ]
				on: Error do: [ :e | "Nothing?" ] ]
				ensure: [ future isFinished ifFalse: [ future deploySuccess: got ] ] ].
		previous
			ifNil: [ run value ]
			ifNotNil: [ previous onSuccessDo: [ :ignored | run value ] ] ].
	self onFailureDo: [ :error | future deployFailure: error freeze ].
	^ future'.

add value: #TKTFutureTest value: #testFutureAndThenExecutesInOrderWithSlowFirstCallback value: 'tests - combinators'
	value:
'testFutureAndThenExecutesInOrderWithSlowFirstCallback

	| future collection last |
	future := TKTFuture new.
	collection := OrderedCollection new.

	last := future
		andThen: [ :v | 100 milliSecond wait. collection add: v + 1 ];
		andThen: [ :v | collection add: v + 2 ];
		andThen: [ :v | collection add: v + 3 ].

	future deploySuccess: 0.
	last waitForCompletion: 5 seconds.

	self assert: collection asArray equals: #( 1 2 3 )'.

add value: #TKTFutureTest value: #testFutureAndThenRunsLaterBlocksWhenABlockEndsItsProcess value: 'tests - combinators'
	value:
'testFutureAndThenRunsLaterBlocksWhenABlockEndsItsProcess
	"The first block ends the process that runs the block (not an Error: the handler of andThen: does not catch the end).  The runner of the future makes a new process for each task, so no worker of the common pool ends."

	| future collection last |
	future := TKTFuture new.
	future runner: TKTNewProcessTaskRunner new.
	collection := OrderedCollection new.

	last := future
		andThen: [ :v | Processor activeProcess terminate. collection add: v + 1 ];
		andThen: [ :v | collection add: v + 2 ];
		andThen: [ :v | collection add: v + 3 ].

	future deploySuccess: 0.
	last waitForCompletion: 5 seconds.

	self assert: collection asArray equals: #( 2 3 )'.

add value: #TKTFutureTest value: #testCallbackRegisteredWhileAnotherProcessDeploysIsScheduled value: 'tests - callbacks'
	value:
'testCallbackRegisteredWhileAnotherProcessDeploysIsScheduled
	"Forces the interleaving: the state of the future is a not-finished state that, when it gets the callback, starts a process that deploys the result, waits 200 ms for the end of that process, and then registers the callback.  Without a critical section the deployment ends at once and walks the callbacks before the registration: the callback is never scheduled.  With the critical section the deployment waits for the registration."

	| future forkDone ran ranSignal |
	forkDone := Semaphore new.
	ranSignal := Semaphore new.
	future := TKTFuture new.
	future instVarNamed: ''result'' put: (TKTFutureStateWithRegistrationHook new hook: [ :aFuture |
		[ aFuture deploySuccess: 1. forkDone signal ] fork.
		forkDone waitTimeoutMilliseconds: 200 ]; yourself).

	future onSuccessDo: [ :v | ran := v. ranSignal signal ].

	self deny: (ranSignal waitTimeoutMilliseconds: 2000).
	self assert: ran equals: 1'.
