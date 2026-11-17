"TaskIt: the worker pool stops a worker that holds a task (2026-11-17).

TKTCommonQueueWorkerPool>>privateRemoveIdleWorkerIfNeeded stops one free
worker.  A worker is free (currentTaskExecution isNil) from the moment when
its task ends to the moment when TKTWorkerProcess>>executeTask: records the
next task.  In that time the worker can be in AtomicSharedQueue>>next, in the
code that takes an item, or can hold the task that it took.  The pool also
makes the list of the free workers some time before it stops one of them, and
the worker can take and start a task in that time.  The pool then terminates
a process that holds a task: the task never runs, and its future never gets a
value.  A process that is terminated in WaitfreeQueue>>nextIfNone: also
leaves the queue in the state that a reader holds the queue.

The processes of a pool have the priority of the process that made them, so a
test and its workers run at one priority.  The image runs with
preemptionYields: each time a process of a higher priority runs (a timer, the
UI cycle), the VM puts the active process behind the other processes of its
priority.  Stock Cog and our VM do the same, and the defect shows on the two.

Tests that fail from this cause, in scripts/cogdiff/taskit_pool_stress.st:
  TKTCommonQueueWorkerPoolTest>>testWorkerPoolDoesNotExceedPoolSizeWhenSchedulingTasksInParallel
      (TKTTimeoutException: 83 of 600 runs on our VM, 2 of 600 on stock Cog)

The repair uses one fact that the pool can read at one moment: the process of
the worker waits on the semaphore of the task queue.  A process in that state
is in AtomicSharedQueue>>next and holds no item.  The process leaves that
state only when an item comes into the queue.  Items come in under the mutex
of the pool, and the pool holds the mutex when it stops a worker.

A test of two facts is not enough.  With 'the worker is free' and then 'the
process of the worker waits', a worker passed that took a task between the
two reads, started the task, and then waited for the mutex of the pool (the
event log of scripts/cogdiff/taskit_pool_diag.st showed that case).

P1. AtomicSharedQueue>>isProcessWaiting:
P2, P3. isWaitingForTask of the worker process and of the worker.
P4. The pool stops a worker only when the worker waits for a task."

add value: #AtomicSharedQueue value: #isProcessWaiting: value: 'testing'
	value:
'isProcessWaiting: aProcess
	"Answer if aProcess waits in #next for an item.  A process in that state holds no item of the queue, and leaves the state only when an item comes into the queue."

	^ aProcess suspendingList == availSema'.

add value: #TKTWorkerProcess value: #isWaitingForTask value: 'testing'
	value:
'isWaitingForTask
	"The process of the worker waits in the task queue for a task.  A worker that is free (isFree) can be in the code that takes a task from the queue, or can hold a task that executeTask: did not record yet."

	^ process isNotNil and: [ taskQueue isNotNil and: [ taskQueue isProcessWaiting: process ] ]'.

add value: #TKTWorker value: #isWaitingForTask value: 'testing'
	value:
'isWaitingForTask

	^ process ifNil: [ false ] ifNotNil: [ process isWaitingForTask ]'.

fix value: #TKTCommonQueueWorkerPool value: #privateRemoveIdleWorkerIfNeeded
	value: [ :source | (source includesSubstring: 'isWaitingForTask') not ]
	value:
'privateRemoveIdleWorkerIfNeeded
	"Stop one worker that waits for a task.  A free worker can hold a task that it did not record yet, and can start a task after the list of the free workers is made: the stop of such a worker loses the task.  A worker that waits in the task queue holds no task.  Tasks come into the queue under the mutex, which the sender holds, so that worker stays in the queue until the stop."

	| removing free |
	free := self privateFreeWorkers.
	free size < taskQueue size
		ifTrue: [ ^ self ].
	removing := workers detect: [ :each | each isWaitingForTask ] ifNone: [ ^ self ].
	workers remove: removing.
	removing stop'.
