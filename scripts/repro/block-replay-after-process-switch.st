"Regression repro: a block resumed by a process switch must not be re-run
 from its first bytecode.

 do: sends value: to [:s | s signal].  primitiveFullClosureValue's 1-arg
 fast path turns that into a direct #signal send, primitiveSignal resumes the
 higher-priority process p, and the chain loop in tryJITActivation used to
 see 'a frame was pushed by prim 207' and tryExecute the frame on top -- p's
 suspended block, mid-body -- from its START, replaying `count := count + 1`.

 Run:  ./build-rel/test_load_image Pharo.image eval \
         ""'scripts/repro/block-replay-after-process-switch.st' asFileReference fileIn""
 Pass: replays=0.  Before the fix: replays=39322 (count=40001 after=678)."
| sema count after p arr |
sema := Semaphore new. count := 0. after := 0.
p := [[true] whileTrue: [ [ count := count + 1. sema wait. after := after + 1 ] value ]]
      forkAt: Processor activePriority + 1.
arr := Array with: sema.
1 to: 40000 do: [:k | arr do: [:s | s signal]].
p terminate.
Stdio stdout nextPutAll: 'RESUME-REPRO count=', count printString, ' after=', after printString,
  ' replays=', (count - after - 1) printString; lf; flush.
count - after - 1
