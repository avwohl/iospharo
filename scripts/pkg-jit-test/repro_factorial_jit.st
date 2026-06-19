"GENUINE JIT bug repro — Integer>>factorial (2-partition divide-and-conquer)
 miscompiles under the JIT. Surfaced by PolyMath/kernel IntegerTest>>testSlowFactorial.

 ONE value comes out wrong per run; the failing n VARIES run-to-run (164-181),
 so it is non-deterministic. slowFactorial (plain recursion) is always correct,
 so the bug is in `factorial` specifically.

 JIT-specific: PHARO_NO_JIT=1 -> count=0 (clean).
 NOT inline-J2J: PHARO_T1_NO_INLINE_J2J=1 still fails.
 NOT scheduler-timing: PHARO_DET_SCHED=1 does not pin it.
 => likely a GC-safety bug in JIT'd factorial (a live oop / raw value held across
 an allocation point in the allocation-heavy LargeInteger 2-partition code).

 Run (any image with Integer>>factorial, i.e. any Pharo image):
   build-rel/test_load_image IMAGE eval \"$(cat repro_factorial_jit.st)\"
 Want: FACTORIAL-bad count=0.  Bug: count=1 at a varying n around 170."

| log ref bad |
log := Stdio stdout. ref := 1. bad := OrderedCollection new.
1 to: 1000 do: [:i |
  ref := ref * i.                                 "oracle: running product = i!"
  (i factorial) = ref ifFalse: [ bad add: i ] ].
log nextPutAll: 'FACTORIAL-bad ';
    nextPutAll: (bad asArray first: (bad size min: 8)) printString;
    nextPutAll: ' count='; nextPutAll: bad size printString; lf; flush.
