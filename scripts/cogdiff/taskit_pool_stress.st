"taskit_pool_stress.st -- run the TaskIt tests that fail from time to time, many
 times in one image, and count the failures.

   scripts/vmrun.sh eval --timeout 1800 ours scripts/cogdiff/taskit_pool_stress.st
   scripts/vmrun.sh eval --timeout 1800 cog  scripts/cogdiff/taskit_pool_stress.st

 The defects are races between processes of the same priority.  A process of a
 higher priority that wakes often makes the VM rotate the processes of the test
 priority (the image runs with preemptionYields), so a ticker process at
 priority 60 with a 1 ms delay runs during the loops.  A timer in a normal
 image does the same thing less often.

 Output: one line for each test, with the number of runs and the number of
 runs that did not pass, then each failure text with its count."
| ticker run out |
[ (UIManager default class == MorphicUIManager) ifFalse: [ MorphicUIManager new beDefault ] ] on: Error do: [ :e | ].
[ (Smalltalk globals at: #FakeGUI ifAbsent: [ nil ]) ifNotNil: [ :fg | fg perform: #startCycleLoop ] ] on: Error do: [ :e | ].
ticker := [ [ (Delay forMilliseconds: 1) wait ] repeat ] forkAt: 60.
out := WriteStream on: String new.
run := [ :cls :sel :n | | texts |
	texts := Bag new.
	1 to: n do: [ :i | | text |
		text := [ (cls selector: sel) runCaseManaged. nil ]
			on: Error , TestFailure
			do: [ :ex | | t | t := ex class name , ': ' , (ex messageText ifNil: [ '' ]). ex return: t ].
		text ifNotNil: [ texts add: (text copyFrom: 1 to: (text size min: 120)) ] ].
	out nextPutAll: cls name; nextPutAll: '>>'; nextPutAll: sel;
		nextPutAll: ' runs='; print: n; nextPutAll: ' bad='; print: texts size; lf.
	texts asSet do: [ :t | out nextPutAll: '    '; print: (texts occurrencesOf: t); nextPutAll: ' x '; nextPutAll: t; lf ] ].
run value: TKTCommonQueueWorkerPoolTest value: #testWorkerPoolDoesNotExceedPoolSizeWhenSchedulingTasksInParallel value: 600.
run value: TKTWorkerPoolTest value: #testWorkerPoolDoesNotExceedPoolSizeWhenSchedulingTasksInParallel value: 300.
run value: TKTFutureTest value: #testFutureFirstCompleteOfSuccessIfFirstSuccessFaster value: 600.
run value: TKTFutureTest value: #testFutureFirstCompleteOfSuccessIfSecondSuccessFaster value: 300.
run value: TKTFutureTest value: #testFutureFirstCompleteOfFailsIfFirstFailsFaster value: 300.
run value: TKTFutureTest value: #testFutureFirstCompleteOfFailsIfSecondFailsFaster value: 300.
ticker terminate.
out contents
