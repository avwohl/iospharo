"bench-suite harness — runs the bench-suite benchmarks, writes results to
 /tmp/bench_suite_result.txt, exits.  Each benchmark is run best-of-3 (lets the
 JIT warm up) and reports BOTH in-image CPU ms (prim 247, our VM only — Cog
 returns 0) AND wall ms (Time microsecondClockValue, works on both VMs).  The
 wall number is the cross-VM comparison metric; CPU tracks our VM's own
 progress with less machine/load variance.  Line format:
   <label> = <cpu> ms cpu, <wall> ms wall
   <label> = ERROR: <msg>"

"Route Array>>atAllPut: through prim 145 (constantFill).  Without this Array
 inherits SequenceableCollection's bytecode loop which dominates stringHash
 100K + dict 50K via from:to:put: doubling."
Array compile: 'atAllPut: anObject
  <primitive: 145>
  ^ super atAllPut: anObject' classified: 'accessing'!

ShiftClassInstaller make: [:b |
  b name: #PharoBenchSuite;
    superclass: Object;
    package: 'BenchSuite']!

PharoBenchSuite class compile: 'startUp: resuming
  resuming ifTrue: [self runAsync]' classified: 'system startup'!

PharoBenchSuite class compile: 'runAsync
  [
    (Delay forSeconds: 2) wait.
    [self runAll]
      on: Error
      do: [:e |
        ''/tmp/bench_suite_result.txt'' asFileReference writeStreamDo: [:f |
          f nextPutAll: ''ERROR: ''; nextPutAll: e messageText; lf]].
    Smalltalk exitSuccess
  ] forkAt: Processor highestPriority - 1' classified: 'running'!

PharoBenchSuite class compile: 'cpuMicros
  "Process CPU time in microseconds (primitive 247 — our VM extension).
   Returns 0 on stock Cog (unsupported)."
  <primitive: 247>
  ^ 0' classified: 'running'!

PharoBenchSuite class compile: 'wallMicros
  "Wall-clock microseconds; available on both VMs."
  ^ Time microsecondClockValue' classified: 'running'!

PharoBenchSuite class compile: 'bench: label do: aBlock on: f
  "Run aBlock 3x; report best (min) CPU ms and best wall ms."
  | bestCpu bestWall |
  bestCpu := nil. bestWall := nil.
  [ 1 to: 3 do: [:i | | c0 w0 cpu wall |
      c0 := self cpuMicros. w0 := self wallMicros.
      aBlock value.
      cpu := (self cpuMicros - c0) // 1000.
      wall := (self wallMicros - w0) // 1000.
      (bestCpu isNil or: [cpu < bestCpu]) ifTrue: [bestCpu := cpu].
      (bestWall isNil or: [wall < bestWall]) ifTrue: [bestWall := wall] ].
    f nextPutAll: label; nextPutAll: '' = ''; print: bestCpu;
      nextPutAll: '' ms cpu, ''; print: bestWall; nextPutAll: '' ms wall''; lf. f flush ]
  on: Error
  do: [:e | f nextPutAll: label; nextPutAll: '' = ERROR: '';
      nextPutAll: e messageText; lf. f flush]' classified: 'running'!

PharoBenchSuite class compile: 'runAll
  ''/tmp/bench_suite_result.txt'' asFileReference writeStreamDo: [:f |
    f flush.
    [f nextPutAll: ''tinyBenchmarks: ''; nextPutAll: 0 tinyBenchmarks; lf. f flush]
      on: Error do: [:e | f nextPutAll: ''tinyBenchmarks: ERROR: ''; nextPutAll: e messageText; lf. f flush].
    self bench: ''fib(28)'' do: [28 benchFib] on: f.
    self bench: ''sieve x100'' do: [(1 to: 100) inject: 0 into: [:s :i | s + i]] on: f.
    self bench: ''sort 100K'' do: [| a | a := (1 to: 100000) collect: [:i | 100000 - i]. a sort] on: f.
    self bench: ''dict 50K'' do: [| d | d := Dictionary new. 1 to: 50000 do: [:i | d at: i put: i]. 1 to: 50000 do: [:i | d at: i]] on: f.
    self bench: ''sum 1M'' do: [(1 to: 1000000) inject: 0 into: [:s :i | s + i]] on: f.
    self bench: ''5000 factorial'' do: [5000 factorial] on: f.
    self bench: ''1M blocks'' do: [| x | x := 0. 1 to: 1000000 do: [:i | [x := x + 1] value]] on: f.
    self bench: ''1M getter+yourself'' do: [| pt | pt := 1 @ 2. 1 to: 1000000 do: [:i | pt x]] on: f.
    self bench: ''100K allocations'' do: [| arr | arr := Array new: 100000. 1 to: 100000 do: [:i | arr at: i put: Object new]] on: f.
    self bench: ''floatSum 1M'' do: [(1 to: 1000000) inject: 0.0 into: [:s :i | s + i asFloat]] on: f.
    self bench: ''stringHash 100K'' do: [| arr | arr := Array new: 100000 withAll: ''hello''. arr do: [:s | s hash]] on: f.
    self bench: ''collect 10x100K'' do: [| a | a := 1 to: 100000. 1 to: 10 do: [:i | a collect: [:x | x * 2]]] on: f.
    self bench: ''select 10x100K'' do: [| a | a := 1 to: 100000. 1 to: 10 do: [:i | a select: [:x | x even]]] on: f.
    f nextPutAll: ''DONE''; lf]' classified: 'running'!

SessionManager default register:
  (ClassSessionHandler forClassNamed: #PharoBenchSuite)!
