Integer compile: 'incc ^self + 1'.
Integer compile: 'cfibx ^self < 2 ifTrue: [self] ifFalse: [((self - 1) cfibx + (self - 2) cfibx) incc]'.
Object compile: 'cogRunBench
  | bt loop fib cfx |
  bt := [:blk | | best | best := 999999999. 5 timesRepeat: [ | t | t := Time millisecondsToRun: blk. t < best ifTrue: [best := t]]. best].
  loop := [1 to: 20000000 do: [:i | i + i]].
  fib := [30 benchFib].
  cfx := [30 cfibx].
  5 timesRepeat: [loop value. fib value. cfx value].
  ^ ''loop20M='' , (bt value: loop) printString , '' fib30='' , (bt value: fib) printString , '' cfibx30='' , (bt value: cfx) printString'.
nil cogRunBench
