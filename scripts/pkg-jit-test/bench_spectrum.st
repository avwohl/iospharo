"Cross-VM JIT speed spectrum benchmark.

 Runs the SAME workloads on our custom JIT VM and on stock Cog, emitting one
 `BENCH <name> <ms>` line per workload (to stdout) so a driver can diff the two
 runs and compute slowdown ratios.  The workloads are chosen to span the whole
 performance surface — from the JIT's inlinable sweet spot (integer arithmetic
 loops, method self-recursion) out to the dispatch/allocation tax that dominates
 real packages (non-inlined polymorphic sends, first-class block evaluation,
 collection protocols, stream building, allocation).

 Usage (both VMs read the literal eval keyword form): pass this whole file as the
 eval argument to either VM.  See run_bench.sh in this directory for the driver.

 Each workload is sized so Cog runs it in tens-to-hundreds of ms; the custom VM
 may take many seconds on the send/alloc-heavy ones — that gap is the point."

| log bench mfibDefined |
log := Stdio stdout.
bench := [:name :reps :blk |
  | t |
  blk value.                                    "warm-up (let the JIT compile)"
  t := [ reps timesRepeat: blk ] timeToRun.
  log nextPutAll: 'BENCH '; nextPutAll: name;
      nextPutAll: ' ms='; print: t asMilliSeconds;
      nextPutAll: ' reps='; print: reps; lf; flush ].

"--- compute-bound: integer arithmetic loop (JIT inlines SmallInteger arith) ---"
bench value: 'int_loop' value: 20 value: [
  | s | s := 0. 1 to: 1000000 do: [:i | s := s + (i * 3) - 1 \\ 65536]. s ].

"--- compute-bound: float arithmetic loop ---"
bench value: 'float_loop' value: 20 value: [
  | s | s := 0.0. 1 to: 1000000 do: [:i | s := s + (i asFloat * 1.5) sqrt ]. s ].

"--- method self-recursion (the inline-J2J cross-method sweet spot) ---"
SmallInteger compile: 'mfibBench ^self < 2 ifTrue: [self] ifFalse: [(self - 1) mfibBench + (self - 2) mfibBench]'.
bench value: 'method_recursion_fib30' value: 20 value: [ 30 mfibBench ].

"--- first-class block recursion via #value: (NOT an inline-J2J target) ---"
bench value: 'block_recursion_fib28' value: 20 value: [
  | fib | fib := nil.
  fib := [:n | n < 2 ifTrue: [n] ifFalse: [(fib value: n - 1) + (fib value: n - 2)]].
  fib value: 28 ].

"--- non-inlined polymorphic sends (the dispatch tax) ---"
bench value: 'polymorphic_sends' value: 20 value: [
  | objs s | objs := { 1. 2.5. $a. 'x'. #sym. nil. true. (1/2) }.
  s := 0. 200000 timesRepeat: [ objs do: [:o | s := s + o class name size ] ]. s ].

"--- collection iteration: do:/collect:/inject:into:/select: ---"
bench value: 'collection_protocols' value: 20 value: [
  | a b | a := (1 to: 50000) asArray.
  b := a collect: [:x | x * 2].
  b := b select: [:x | x \\ 3 = 0].
  b inject: 0 into: [:acc :x | acc + x] ].

"--- OrderedCollection add/remove churn ---"
bench value: 'orderedcollection_churn' value: 20 value: [
  | oc | oc := OrderedCollection new.
  1 to: 100000 do: [:i | oc addLast: i].
  1 to: 50000 do: [:i | oc removeFirst]. oc size ].

"--- Dictionary at:put:/at: ---"
bench value: 'dictionary_ops' value: 20 value: [
  | d s | d := Dictionary new.
  1 to: 30000 do: [:i | d at: i put: i * i].
  s := 0. 1 to: 30000 do: [:i | s := s + (d at: i)]. s ].

"--- Set membership ---"
bench value: 'set_ops' value: 20 value: [
  | st c | st := Set new.
  1 to: 30000 do: [:i | st add: i \\ 10000].
  c := 0. 1 to: 30000 do: [:i | (st includes: i) ifTrue: [c := c + 1]]. c ].

"--- string / stream building (alloc + sends, NeoJSON-like) ---"
bench value: 'string_build' value: 20 value: [
  | s | s := WriteStream on: String new.
  1 to: 20000 do: [:i | s nextPutAll: i printString; nextPut: $,]. s contents size ].

"--- object allocation + instance-variable writes ---"
bench value: 'allocation' value: 20 value: [
  | last | last := nil.
  1 to: 200000 do: [:i | last := Association key: i value: last]. last key ].

"--- LargeInteger arithmetic (factorial) ---"
bench value: 'largeint_factorial' value: 20 value: [ (1 to: 2000) inject: 1 into: [:a :b | a * b] ].

"--- sorting ---"
bench value: 'sort' value: 20 value: [
  | a | a := (1 to: 20000) asArray collect: [:i | (i * 7919) \\ 20011].
  a asSortedCollection size ].

log nextPutAll: 'BENCH-DONE'; lf; flush.
