"VM Performance Benchmarks.
 Registers a startup handler that runs benchmarks on image resume.
 Results written to /tmp/pharo_benchmarks.txt (incrementally)."!

"Create the benchmark runner class"
ShiftClassInstaller make: [:b |
  b name: #PharoBenchmarkRunner;
    superclass: Object;
    package: 'BenchmarkRunner']
!

PharoBenchmarkRunner class compile: 'startUp: resuming
  resuming ifTrue: [self runBenchmarksAsync]' classified: 'system startup'
!

PharoBenchmarkRunner class compile: 'runBenchmarksAsync
  [
    (Delay forSeconds: 2) wait.
    [self runBenchmarks]
      on: Error
      do: [:e |
        | s |
        s := ''/tmp/pharo_benchmarks.txt'' asFileReference writeStream.
        [s nextPutAll: ''ERROR: ''; nextPutAll: e messageText] ensure: [s close]].
    Smalltalk exitSuccess
  ] forkAt: Processor highestPriority - 1' classified: 'running'
!

PharoBenchmarkRunner class compile: 'runBenchmarks
  "Write results incrementally so partial results are visible"
  | stream |
  stream := ''/tmp/pharo_benchmarks.txt'' asFileReference writeStream.
  [
    self log: ''VM Performance Benchmarks'' to: stream.
    self log: ''========================'' to: stream.
    self log: ''Date: '', DateAndTime now printString to: stream.
    self log: ''Image: '', SystemVersion current imageVersionString to: stream.
    self log: '''' to: stream.

    self log: ''--- tinyBenchmarks ---'' to: stream.
    self log: self runTinyBenchmarks to: stream.
    self log: '''' to: stream.

    self log: ''--- fibonacci(28) ---'' to: stream.
    self log: self runFibonacci to: stream.
    self log: '''' to: stream.

    self log: ''--- sieve(100) ---'' to: stream.
    self log: self runSieve to: stream.
    self log: '''' to: stream.

    self log: ''--- sort(100K) ---'' to: stream.
    self log: self runSort to: stream.
    self log: '''' to: stream.

    self log: ''--- dict(50K) ---'' to: stream.
    self log: self runDict to: stream.
    self log: '''' to: stream.

    self log: ''--- sum(1M) ---'' to: stream.
    self log: self runSum to: stream.
    self log: '''' to: stream.

    self log: ''--- factorial(5K) ---'' to: stream.
    self log: self runFactorial to: stream.
    self log: '''' to: stream.

    self log: ''--- block(1M) ---'' to: stream.
    self log: self runBlock to: stream.
    self log: '''' to: stream.

    self log: ''--- instVar(1M) ---'' to: stream.
    self log: self runInstVar to: stream.
    self log: '''' to: stream.

    self log: ''--- create(100K) ---'' to: stream.
    self log: self runCreate to: stream.
    self log: '''' to: stream.

    self log: ''--- floatSum(1M) ---'' to: stream.
    self log: self runFloatSum to: stream.
    self log: '''' to: stream.

    self log: ''--- stringHash(100K) ---'' to: stream.
    self log: self runStringHash to: stream.
    self log: '''' to: stream.

    self log: ''--- collect(100K) ---'' to: stream.
    self log: self runCollect to: stream.
    self log: '''' to: stream.

    self log: ''--- select(100K) ---'' to: stream.
    self log: self runSelect to: stream.
    self log: '''' to: stream.

    self log: ''DONE'' to: stream
  ] ensure: [stream close].
  Transcript show: ''Benchmarks written to /tmp/pharo_benchmarks.txt''; cr' classified: 'running'
!

PharoBenchmarkRunner class compile: 'log: aString to: aStream
  aStream nextPutAll: aString; lf; flush.
  Transcript show: aString; cr' classified: 'private'
!

"Process CPU time (user+system) in microseconds.  Our VM exposes
this via primitiveProcessCpuMicroseconds.  Cog will fail the
primitive and we'll fall back to -1 (printed as ?).  Used to
compare runs without wall-clock noise."
PharoBenchmarkRunner class compile: 'cpuUsec
  <primitive: ''primitiveProcessCpuMicroseconds'' module: ''''>
  ^ -1' classified: 'private'
!

"Time the block; return ''WALLms ms (cpu=CPUms ms)'' (or just
''WALLms ms'' if CPU unavailable).  CPU is rounded to ms."
PharoBenchmarkRunner class compile: 'timeBlock: aBlock
  | w1 w2 c1 c2 wallMs cpuMs |
  c1 := self cpuUsec.
  w1 := Time millisecondClockValue.
  aBlock value.
  w2 := Time millisecondClockValue.
  c2 := self cpuUsec.
  wallMs := w2 - w1.
  (c1 < 0 or: [c2 < 0])
    ifTrue: [^ wallMs printString, '' ms''].
  cpuMs := (c2 - c1) // 1000.
  ^ wallMs printString, '' ms (cpu='', cpuMs printString, '' ms)''' classified: 'private'
!

PharoBenchmarkRunner class compile: 'runTinyBenchmarks
  | w1 w2 c1 c2 n1 n2 r1 r2 r1c r2c useCpu c1a c2a c1b c2b |
  c1a := self cpuUsec.
  n1 := 1. w1 := Time millisecondClockValue.
  [Time millisecondClockValue - w1 < 1000] whileTrue: [n1 benchmark. n1 := n1 + 1].
  w1 := Time millisecondClockValue - w1.
  c2a := self cpuUsec.
  r1 := (n1 * 500000 * 1000) // w1.
  c1b := self cpuUsec.
  n2 := 1. w2 := Time millisecondClockValue.
  [Time millisecondClockValue - w2 < 1000] whileTrue: [28 benchFib. n2 := n2 + 1].
  w2 := Time millisecondClockValue - w2.
  c2b := self cpuUsec.
  r2 := (n2 * 9227465 * 1000) // w2.
  useCpu := c1a >= 0 and: [c2a >= 0 and: [c1b >= 0 and: [c2b >= 0]]].
  useCpu ifFalse: [
    ^ r1 printString, '' bytecodes/sec; '', r2 printString, '' sends/sec''
  ].
  r1c := (n1 * 500000 * 1000000) // (c2a - c1a max: 1).
  r2c := (n2 * 9227465 * 1000000) // (c2b - c1b max: 1).
  ^ r1 printString, '' bytecodes/sec; '', r2 printString, '' sends/sec (cpu='', r1c printString, '' bytecodes/sec, '', r2c printString, '' sends/sec)''' classified: 'benchmarks'
!

PharoBenchmarkRunner class compile: 'runFibonacci
  ^ ''fib(28) = '', (self timeBlock: [28 benchFib])' classified: 'benchmarks'
!

PharoBenchmarkRunner class compile: 'runSieve
  | count r |
  r := self timeBlock: [count := 100 benchmark].
  ^ ''sieve x100 = '', r, '' ('', count printString, '' primes)''' classified: 'benchmarks'
!

PharoBenchmarkRunner class compile: 'runSort
  | a |
  a := (1 to: 100000) asArray copy shuffled.
  ^ ''sort 100K = '', (self timeBlock: [a sort])' classified: 'benchmarks'
!

PharoBenchmarkRunner class compile: 'runDict
  | d keys |
  keys := (1 to: 50000) collect: [:i | ''key'', i printString].
  d := Dictionary new: 75000.
  ^ ''dict 50K put+get = '', (self timeBlock: [
    keys do: [:k | d at: k put: k].
    keys do: [:k | d at: k]])' classified: 'benchmarks'
!

PharoBenchmarkRunner class compile: 'runSum
  | a sum |
  a := (1 to: 1000000) asArray.
  sum := 0.
  ^ ''sum 1M = '', (self timeBlock: [a do: [:each | sum := sum + each]]), '' (sum='', sum printString, '')''' classified: 'benchmarks'
!

PharoBenchmarkRunner class compile: 'runFactorial
  ^ ''5000 factorial = '', (self timeBlock: [5000 factorial])' classified: 'benchmarks'
!

PharoBenchmarkRunner class compile: 'runBlock
  | counter |
  counter := 0.
  ^ ''1M blocks = '', (self timeBlock: [1000000 timesRepeat: [counter := counter + 1]])' classified: 'benchmarks'
!

PharoBenchmarkRunner class compile: 'runInstVar
  | obj |
  obj := OrderedCollection new.
  ^ ''1M getter+yourself = '', (self timeBlock: [1000000 timesRepeat: [obj size. obj yourself]])' classified: 'benchmarks'
!

PharoBenchmarkRunner class compile: 'runCreate
  ^ ''100K allocations = '', (self timeBlock: [100000 timesRepeat: [Array new: 10]])' classified: 'benchmarks'
!

PharoBenchmarkRunner class compile: 'runFloatSum
  "Float arithmetic intensive — covers a path different from SmI fast
   paths.  Sums a 1M-element Float Array."
  | arr s |
  arr := (Array new: 1000000) collect: [:_ | 1.5].
  s := 0.0.
  ^ ''floatSum 1M = '', (self timeBlock: [arr do: [:e | s := s + e]]), '' (sum='', s printString, '')''' classified: 'benchmarks'
!

PharoBenchmarkRunner class compile: 'runStringHash
  "Symbol/String hash — exercises the same dispatch that dict 50K uses
   but isolates the hash + equality cost."
  | keys h |
  keys := (1 to: 100000) collect: [:i | ''key'', i printString].
  h := 0.
  ^ ''stringHash 100K = '', (self timeBlock: [keys do: [:k | h := h bitXor: k hash]])' classified: 'benchmarks'
!

PharoBenchmarkRunner class compile: 'runCollect
  "collect: with SmI arith — exercises the canonical-shape collect:
   resume helper.  Result Array is allocated each iter; bench drives
   the alloc + per-iter SmI arith path."
  | arr r |
  arr := (1 to: 100000) asArray.
  ^ ''collect 10x100K = '', (self timeBlock: [1 to: 10 do: [:i | r := arr collect: [:e | e + 1]]])' classified: 'benchmarks'
!

PharoBenchmarkRunner class compile: 'runSelect
  "select: with SmI predicate — exercises a different block-dispatch
   pattern than collect: (boolean-returning block, conditional store)."
  | arr r |
  arr := (1 to: 100000) asArray.
  ^ ''select 10x100K = '', (self timeBlock: [1 to: 10 do: [:i | r := arr select: [:e | e even]]]), '' (kept='', r size printString, '')''' classified: 'benchmarks'
!

"Register as session handler"
SessionManager default register:
  (ClassSessionHandler forClassNamed: #PharoBenchmarkRunner)
!
