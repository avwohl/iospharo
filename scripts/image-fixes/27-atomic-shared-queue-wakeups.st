"Collections-Atomic: AtomicSharedQueue loses a wake-up when it has two or more
readers, and a reader that is terminated can leave the queue locked
(2026-11-17).  A TaskIt worker pool is such a queue with many readers.  Found
by a review of the TaskIt repair (25-taskit-worker-pool-race.st).  The three
tests of this file (a new class AtomicSharedQueueTest) fail on stock Cog and
on our VM before the repair.

The image runs with preemptionYields, so a process can lose the processor to
a process of its own priority at each method or block activation and at each
backward jump.

Q1. A reader that finds the queue locked.  WaitfreeQueue>>nextIfNone: locks
the queue for the time of one read (dummy makeCircular).  Reader B wakes for
a second item while reader A has the lock.  B gets the answer 'no item',
waits again in AtomicSharedQueue>>next, and the signal for its item is used.
A ends its read and gives no signal.  The second item stays in the queue
until the next nextPut: or until A reads again.  Repair: a reader gives a
signal after its read when items are left (AtomicSharedQueue>>nextIfNone:).

Q2. A reader that resets the signals.  A reader that finds only removed
items sends signalNoMoreItems (availSema consumeAllSignals).  The send is a
method activation.  A writer that runs there links a new item and gives a
signal.  The reader then resets that signal and waits, with an item in the
queue.  Repair: AtomicSharedQueue>>next looks again (isEmpty) before it
waits.  isEmpty answers true for a locked queue, and the reader that has the
lock gives the signal of Q1.

Q3. A reader that is terminated in nextIfNone: leaves the queue locked, and
no reader gets an item after that.  flush: and nextOrNilSuchThat: give the
queue back with ifCurtailed:, and nextIfNone: did not.  Repair: the read runs
in a block with ifCurtailed:.  No process switch can come between the lock
and the start of that block: the VM makes no interrupt check when it
activates ifCurtailed: (primitive 198) and the block of valueNoContextSwitch,
and the two lines between have no send."

fix value: #WaitfreeQueue value: #nextIfNone:
	value: [ :source | (source includesSubstring: 'ifCurtailed:') not ]
	value:
'nextIfNone: aBlock
	"Attempt to fetch the next item from queue. Evaluate a block if attempt is failed i.e. there is no items available or queue is locked by another process"

	| item result found |

	(item := dummy makeCircular) == dummy ifTrue: [
		"queue is either empty or currently locked by other feeder,
		just give a quick answer that item not found"
		 ^ aBlock value  ].

	"The queue is locked from here.  A process that is terminated in the block below gives the queue back: the chain from item has all the items that are left."
	found := false.
	[
		[ (result := item object) == dummy and: [ item isCircular not ] ] whileTrue: [ item := item next ].
		result == dummy
			ifTrue: [
				"only removed items are left"
				self signalNoMoreItems.
				dummy next: item ]
			ifFalse: [
				item object: dummy.
				dummy next: item next.
				found := true ]
	] ifCurtailed: [ dummy next: item ].

	^ found ifTrue: [ result ] ifFalse: [ aBlock value ]'.

add value: #AtomicSharedQueue value: #nextIfNone: value: 'accessing'
	value:
'nextIfNone: aBlock
	"A reader that woke for a later item can have found the queue locked by this reader.  That reader waits again, and the signal for its item is used.  Give a signal when items are left."

	| failed result |
	failed := false.
	result := super nextIfNone: [ failed := true ].
	failed ifTrue: [ ^ aBlock value ].
	self isEmpty ifFalse: [ self signalAddedNewItem ].
	^ result'.

fix value: #AtomicSharedQueue value: #next
	value: [ :source | (source includesSubstring: 'isEmpty') not ]
	value:
'next
	"Fetch the next item from queue. If queue is locked or empty, block the sender until operation can complete"

	| result |

	[ | keepWaiting |
		keepWaiting := false.
		result := self nextIfNone: [ keepWaiting := true ].
		"Look again before the wait: a writer can have put an item in after this reader reset the signals.  isEmpty answers true for a locked queue, and the reader that has the lock gives a signal after its read."
		keepWaiting ] whileTrue: [ self isEmpty ifTrue: [ self waitForNewItems ] ].

	^ result'.

"The released image has no test class for AtomicSharedQueue (the package Collections-Atomic-Tests of the sources is not in the image)."
(Smalltalk globals includesKey: #AtomicSharedQueueTest)
	ifTrue: [ Stdio stdout nextPutAll: 'SKIP AtomicSharedQueueTest (class already present)'; lf; flush ]
	ifFalse: [
		(TestCase << #AtomicSharedQueueTest) package: 'Collections-Atomic-Tests'; install.
		Stdio stdout nextPutAll: 'ADDED AtomicSharedQueueTest'; lf; flush ].

add value: #AtomicSharedQueueTest value: #testSecondReaderGetsItemWhenFirstReaderHadTheLock value: 'tests'
	value:
'testSecondReaderGetsItemWhenFirstReaderHadTheLock
	"Reader A has the lock of the queue.  Reader B wakes for the second item, finds the queue locked and waits again.  When reader A is done, reader B must get the second item."

	| queue readerA readerB gotA gotB steps deadline |
	queue := AtomicSharedQueue new.
	queue nextPut: 1; nextPut: 2.
	readerA := [ gotA := queue next ] newProcess.
	steps := 0.
	[ queue dummy isCircular or: [ steps > 5000 ] ] whileFalse: [ readerA step. steps := steps + 1 ].
	self assert: queue dummy isCircular.
	readerB := [ gotB := queue next ] forkAt: Processor activePriority + 1.
	[
		self assert: gotB isNil.
		readerA resume.
		deadline := Time millisecondClockValue + 2000.
		[ gotB isNil and: [ Time millisecondClockValue < deadline ] ] whileTrue: [ (Delay forMilliseconds: 5) wait ].
		self assert: gotA equals: 1.
		self assert: gotB equals: 2 ]
		ensure: [ readerA terminate. readerB terminate ]'.

add value: #AtomicSharedQueueTest value: #testReaderGetsItemThatCameWhileItResetTheSignals value: 'tests'
	value:
'testReaderGetsItemThatCameWhileItResetTheSignals
	"A reader finds only a removed item and is about to reset the signals.  A writer puts an item in at that moment.  The reader must get the item."

	| queue reader result steps deadline |
	queue := AtomicSharedQueue new.
	queue nextPut: 1.
	self assert: queue next equals: 1.
	reader := [ result := queue next ] newProcess.
	steps := 0.
	[ reader suspendedContext selector == #signalNoMoreItems or: [ steps > 5000 ] ] whileFalse: [ reader step. steps := steps + 1 ].
	self assert: reader suspendedContext selector equals: #signalNoMoreItems.
	queue nextPut: 2.
	[
		reader resume.
		deadline := Time millisecondClockValue + 2000.
		[ result isNil and: [ Time millisecondClockValue < deadline ] ] whileTrue: [ (Delay forMilliseconds: 5) wait ].
		self assert: result equals: 2 ]
		ensure: [ reader terminate ]'.

add value: #AtomicSharedQueueTest value: #testQueueIsFreeAfterReaderIsTerminatedInARead value: 'tests'
	value:
'testQueueIsFreeAfterReaderIsTerminatedInARead
	"A reader is terminated while it has the lock of the queue.  The queue must be free after that, and the item must still be there."

	| queue reader steps |
	queue := AtomicSharedQueue new.
	queue nextPut: 1.
	reader := [ queue next ] newProcess.
	steps := 0.
	[ (queue dummy isCircular and: [ reader suspendedContext selector == #object: ]) or: [ steps > 5000 ] ]
		whileFalse: [ reader step. steps := steps + 1 ].
	self assert: queue dummy isCircular.
	self assert: reader suspendedContext selector equals: #object:.
	reader terminate.
	self assert: queue nextOrNil equals: 1'.
