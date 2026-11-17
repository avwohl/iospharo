"taskit_pool_diag.st -- run
 TKTCommonQueueWorkerPoolTest>>testWorkerPoolDoesNotExceedPoolSizeWhenSchedulingTasksInParallel
 many times as SUnit runs it (runCaseManaged), with an event log, and write
 the state of the pool and the events of the run each time a future passes
 the 100 ms limit of the test.

   scripts/vmrun.sh eval --timeout 2400 ours scripts/cogdiff/taskit_pool_diag.st

 The image is not saved: the methods that this file compiles are for this run.

 Events (an Array and an index in the global TKTDiag; the index step and the
 store have no interrupt point between them, so the log needs no lock, and a
 lock would put a worker in a wait that the pool reads as 'waits for a task'):
   got    a worker took a task from the queue        (task hash, process hash)
   start  executeTask: recorded the task             (task hash, process hash)
   end    the task ended                             (task hash, process hash)
   stop   TKTWorkerProcess>>stop: has-task, state of the process, the two
          senders above TKTWorker>>stop, process hash
   restart TKTWorker>>restart

 For each time limit: the pool state, then each stop of the run, each task
 with got and no start, and each task with start and no end."
| ticker out bad runs sel diag event |
[ (UIManager default class == MorphicUIManager) ifFalse: [ MorphicUIManager new beDefault ] ] on: Error do: [ :e | ].
[ (Smalltalk globals at: #FakeGUI ifAbsent: [ nil ]) ifNotNil: [ :fg | fg perform: #startCycleLoop ] ] on: Error do: [ :e | ].
diag := Array with: (Array new: 20000) with: 0.
Smalltalk globals at: #TKTDiag put: diag.
TKTWorkerProcess compile: 'diagEvent: rec
	| d i |
	d := Smalltalk globals at: #TKTDiag.
	i := (d at: 2) + 1.
	d at: 2 put: i.
	(d at: 1) at: i put: rec'.
TKTWorkerProcess compile: 'privateNextTask
	| task |
	task := taskQueue next.
	self diagEvent: { #got. task identityHash. Processor activeProcess identityHash }.
	^ task'.
TKTWorkerProcess compile: 'executeTask: aTaskExecution
	currentTaskExecution := aTaskExecution.
	self diagEvent: { #start. aTaskExecution identityHash. Processor activeProcess identityHash }.
	super executeTask: aTaskExecution.
	self diagEvent: { #end. aTaskExecution identityHash. Processor activeProcess identityHash }.
	currentTaskExecution := nil'.
TKTWorkerProcess compile: 'stop
	process ifNotNil: [
		self diagEvent: { #stop. currentTaskExecution notNil. process state. thisContext sender sender selector. thisContext sender sender sender selector. process identityHash }.
		process terminate.
		process := nil ]'.
TKTWorker compile: 'restart
	(Smalltalk globals at: #TKTDiag) ifNotNil: [ :d | | i | i := (d at: 2) + 1. d at: 2 put: i. (d at: 1) at: i put: { #restart. process isNil. thisContext sender selector } ].
	self stop.
	self start'.
ticker := [ [ (Delay forMilliseconds: 1) wait ] repeat ] forkAt: 60.
out := WriteStream on: String new.
sel := #testWorkerPoolDoesNotExceedPoolSizeWhenSchedulingTasksInParallel.
bad := 0. runs := 3000.
1 to: runs do: [ :run |
	diag at: 2 put: 0.
	[ (TKTCommonQueueWorkerPoolTest selector: sel) runCaseManaged ]
		on: TKTTimeoutException
		do: [ :ex | | ctx pool queue future late events got started ended |
			bad := bad + 1.
			future := ex signalerContext receiver.
			ctx := ex signalerContext.
			[ ctx notNil and: [ ctx selector ~~ sel or: [ ctx isBlockContext ] ] ] whileTrue: [ ctx := ctx sender ].
			pool := ctx ifNotNil: [ ctx tempNamed: 'pool' ].
			out nextPutAll: 'run '; print: run.
			pool ifNotNil: [
				queue := pool instVarNamed: 'taskQueue'.
				out nextPutAll: ' queue='; print: queue size;
					nextPutAll: ' held='; print: queue dummy isCircular;
					nextPutAll: ' workers='; print: pool size; nextPutAll: ' ['.
				pool workers do: [ :w |
					out nextPutAll: (w isFree ifTrue: [ 'F' ] ifFalse: [ 'B' ]); nextPut: $:;
						nextPutAll: (w process ifNil: [ 'none' ] ifNotNil: [ :wp | wp process ifNil: [ 'none' ] ifNotNil: [ :p | p state , '/' , p identityHash printString ] ]);
						space ].
				out nextPutAll: '] done='; print: (ctx tempNamed: 'totalTasks') ].
			late := [ future waitForCompletion: 2 seconds. true ] on: TKTTimeoutException do: [ :ex2 | ex2 return: false ].
			out nextPutAll: (late ifTrue: [ ' late' ] ifFalse: [ ' lost' ]).
			queue ifNotNil: [ out nextPutAll: ' queue-after='; print: queue size ].
			out lf.
			events := (diag at: 1) copyFrom: 1 to: ((diag at: 2) min: 20000).
			got := Dictionary new. started := Dictionary new. ended := Set new.
			events doWithIndex: [ :ev :i |
				ev ifNotNil: [
					ev first == #got ifTrue: [ got at: ev second put: i -> ev third ].
					ev first == #start ifTrue: [ started at: ev second put: i -> ev third ].
					ev first == #end ifTrue: [ ended add: ev second ].
					ev first == #stop ifTrue: [ out nextPutAll: '    '; print: i; space; print: ev; lf ].
					ev first == #restart ifTrue: [ out nextPutAll: '    '; print: i; space; print: ev; lf ] ] ].
			got keysAndValuesDo: [ :task :where |
				(started includesKey: task) ifFalse: [
					out nextPutAll: '    task '; print: task; nextPutAll: ' got at event '; print: where key;
						nextPutAll: ' by process '; print: where value; nextPutAll: ', no start'; lf ] ].
			started keysAndValuesDo: [ :task :where |
				(ended includes: task) ifFalse: [
					out nextPutAll: '    task '; print: task; nextPutAll: ' start at event '; print: where key;
						nextPutAll: ' by process '; print: where value; nextPutAll: ', no end'; lf ] ].
			out nextPutAll: '    events='; print: events size; lf.
			ex return: nil ] ].
ticker terminate.
'runs=' , runs printString , ' bad=' , bad printString , String lf , out contents
