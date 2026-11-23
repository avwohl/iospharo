# Collections-Atomic: fix lost wake-ups and lock leak in AtomicSharedQueue

Local data (not part of the PR text)

- Upstream repo and base branch: pharo-project/pharo, Pharo15
- Local branch: pr15/atomic-shared-queue-wakeups
- Work tree: /Users/wohl/pharo-work/wt/pharo-atomic-shared-queue-wakeups
- Commits:
  - 4b5ef48e94 Collections-Atomic: fix lost wake-ups and lock leak in AtomicSharedQueue
  - feeee8dc9d Collections-Atomic-Tests: add AtomicSharedQueueTest
- Related issues: #3198 (WaitfreeQeue>>nextOrNilSuchThat: memory leak, closed. Same class, other defect)
- Source in this project: scripts/image-fixes/27-atomic-shared-queue-wakeups.st (docs/upstream/pharo-image-repairs-2026-11-17.md, AtomicSharedQueue-wakeups)
- Triage id: img-atomic-shared-queue
- Review notes (round 1): I accepted all findings. The tests keep the polling with a Delay. The reviewer called that acceptable, and the Notes keep the warning. I added no test for nextOrNil, and the Notes said so. I did not add a signal to the curtail path, and the Notes state the limit.
- Review notes (round 2): finding 1: the unlink and unlock are now outside ifCurtailed:. Because of that, the test of case 3 stops the reader in signalNoMoreItems (inside the protected block). Finding 2: I removed the recursion sentence, split the paragraph, and changed the Tests claim. Finding 3: I added testNextOrNilAnswersTheItemAndNilWhenEmpty. I kept the Delay polling.

## Pull request text

AtomicSharedQueue can leave an item in the queue with no signal for a reader, so a reader waits while the queue is not empty. A reader that a process terminates inside WaitfreeQueue>>nextIfNone: can leave the queue locked for ever. This change repairs both defects and adds four deterministic tests.

Related: #3198 (same class, other defect).

### The defect

A queue with two or more reader processes can lose a wake-up. A TaskIt worker pool is such a queue. The stock VM has preemptionYields on (check with Smalltalk vm preemptionYields). A process can then lose the processor to a process of the same priority at each method activation and at each backward jump.

Three cases:

1. A reader that finds the queue locked. Reader A has the lock of the queue for one read. Reader B wakes for the second item, finds the queue locked, and waits again in AtomicSharedQueue>>next. The wait uses up the signal for the item of reader B. Reader A ends its read and gives no signal. The second item stays in the queue until the next nextPut:, or until reader A reads again.
2. A reader that resets the signals. A reader that finds only removed items sends signalNoMoreItems, which does availSema consumeAllSignals. The send is a method activation. A writer that runs there links a new item and gives a signal. The reader then resets that signal and waits, with an item in the queue.
3. A reader that a process terminates. WaitfreeQueue>>nextIfNone: locks the queue with dummy makeCircular. If a process terminates a reader before the reader gives the queue back, the queue stays locked, and no reader gets an item after that. flush: and nextOrNilSuchThat: already protect the lock with ifCurtailed:. nextIfNone: did not.

Reproduction for case 3, in a clean Pharo 15 image with the stock VM. Print the last line. The message dummy is the public accessor of the lock item.

```smalltalk
| queue reader steps |
queue := AtomicSharedQueue new.
queue nextPut: 1.
reader := [ queue next ] newProcess.
steps := 0.
[ (queue dummy isCircular and: [ reader suspendedContext selector == #object: ]) or: [ steps > 5000 ] ]
	whileFalse: [ reader step. steps := steps + 1 ].
reader terminate.
queue nextOrNil
```

Observed: nil (the queue is still locked and the item is lost to every reader). Expected: 1.

The tests of cases 1 and 2 show the other interleavings. They step the processes by hand, so they run the same way each time.

### The cause

- WaitfreeQueue>>nextIfNone: has no ifCurtailed: between the lock and the unlock.
- AtomicSharedQueue>>next waits for a signal after each failed read, even when the queue has items.
- No method gives a signal after a read when items are left.

### The change

- WaitfreeQueue>>nextIfNone: runs the loop that skips removed items, and the send of signalNoMoreItems, in a block with ifCurtailed: [ dummy next: item ]. The unlink and the unlock come after the block. A curtail block then never runs on a queue that another reader has locked or changed.
- New AtomicSharedQueue>>nextIfNone: gives a signal (signalAddedNewItem) after a successful read when the queue is not empty.
- AtomicSharedQueue>>next sends isEmpty before waitForNewItems. isEmpty answers true for a locked queue, so a reader does not spin while another reader has the lock. The reader that has the lock gives a signal after its read (see the previous item).

The change reduces the window in which a termination can leave the queue locked. The window is now the time between dummy makeCircular and the start of the ifCurtailed: block, which is one send of ifCurtailed:.

The change reduces the window but does not remove the window. In the upstream source, primitive 198 of BlockClosure>>ifCurtailed: always fails, so ifCurtailed: runs as a normal method activation, and with preemptionYields a process switch can come there.

The methods flush: and nextOrNilSuchThat: have the same shape, because they also call makeCircular before their ifCurtailed:. No VM measurement checked this.

A termination after the unlink and before the unlock, which now are outside the ifCurtailed: block, leaves the queue locked. This window is as small as before the change.

The comment text of WaitfreeQueue>>isEmpty says true when an element is present, and the comment is wrong. The change does not rely on that comment, and does not edit it.

The other caller of WaitfreeQueue>>nextIfNone: is nextOrNil. On an AtomicSharedQueue, nextOrNil now also goes through the new AtomicSharedQueue>>nextIfNone:, and adds one signal for each read that leaves items. A reader can then wake without an item. That reader loops and looks again. An empty read sends signalNoMoreItems, and signalNoMoreItems resets the signal count.

I did not measure the throughput of this path. One test checks the values of nextOrNil.

### Tests

New class AtomicSharedQueueTest in Collections-Atomic-Tests, four tests:

- testSecondReaderGetsItemWhenFirstReaderHadTheLock (case 1)
- testReaderGetsItemThatCameWhileItResetTheSignals (case 2)
- testQueueIsFreeAfterReaderIsTerminatedInARead (case 3). The test stops the reader in signalNoMoreItems and terminates the reader there.
- testNextOrNilAnswersTheItemAndNilWhenEmpty (nextOrNil on an AtomicSharedQueue. This test is a regression check and passes before the change too)

The validation stage measures which tests fail without the change and pass with the change. Regression classes to run: AtomicSharedQueueTest, WaitfreeQueueTest, FIFOQueueTest, TKTWorkerPoolTest, TKTCommonQueueWorkerPoolTest (the two TaskIt classes use AtomicSharedQueue).

### How it was tested

Measured on 2026-11-23, macOS arm64, with the stock Cog VM v12.0.6-beta
(bebdbaca8) and a Pharo 15 image (build 141, b4d2dfd).  Each test ran in a
new VM.

Two images were built from the base image with the Tonel loader of this
project.  The loader reported "DONE 0 errors" for both.
- Full image: both commits of the branch.
- Test-only image: only the new test class, with no change to the code.

Proof tests (3 runs each):
- AtomicSharedQueueTest>>testSecondReaderGetsItemWhenFirstReaderHadTheLock:
  FAIL 3 of 3 before ("Got nil instead of 2."), PASS 3 of 3 after.
- AtomicSharedQueueTest>>testReaderGetsItemThatCameWhileItResetTheSignals:
  FAIL 3 of 3 before ("Got nil instead of 2."), PASS 3 of 3 after.
- AtomicSharedQueueTest>>testQueueIsFreeAfterReaderIsTerminatedInARead:
  FAIL 3 of 3 before ("Denial failed"), PASS 3 of 3 after.

The fourth test of the class, testNextOrNilAnswersTheItemAndNilWhenEmpty,
passes on the full image (1 run).  The method nextIfNone: does not exist
before the change, so no before result applies.

Regression runs, base image against full image (1 run each):
- AtomicSharedQueueTest: the class is not in the base image. 4 of 4 PASS on the
  full image.
- WaitfreeQueueTest: the base image does not have this test class.  I
  loaded the class alone into the base image and into the full image.
  Result: 5 of 5 PASS on both.
- TKTWorkerPoolTest: 9 of 9 PASS on both.
- TKTCommonQueueWorkerPoolTest: 7 of 7 PASS on both.
- No test passes on the base image and fails on the full image.

Not run: FIFOQueueTest.  The base image does not have the class FIFOQueue,
so the test class cannot run.

### Notes

- The change touches wait-free queue code. Check the window between the lock and ifCurtailed: carefully (see The change).
- A reader that a process terminates while the reader has the lock gives the queue back, but gives no signal. If items are left, readers that wait on the semaphore stay blocked until the next nextPut:. The lost wake-up stays in this rarer form, and this change does not repair that form.
- The tests poll with a Delay of up to 2 seconds when they fail. The tests depend on the stepping of a suspended process with Process>>step and on the selector names object: and signalNoMoreItems. A change of these methods changes the tests.
- The tests were not run on Windows or Linux in this stage.
- The TaskIt repository (pharo-project/taskit) has a related worker-pool race that needs this change. That repository is not part of this PR.

Generated with [Claude Code](https://claude.com/claude-code)
