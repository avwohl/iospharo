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

    self log: ''DONE'' to: stream
  ] ensure: [stream close].
  Transcript show: ''Benchmarks written to /tmp/pharo_benchmarks.txt''; cr' classified: 'running'
!

PharoBenchmarkRunner class compile: 'log: aString to: aStream
  aStream nextPutAll: aString; lf; flush.
  Transcript show: aString; cr' classified: 'private'
!

PharoBenchmarkRunner class compile: 'runTinyBenchmarks
  | t1 t2 r1 r2 n1 n2 |
  n1 := 1. t1 := Time millisecondClockValue.
  [Time millisecondClockValue - t1 < 1000] whileTrue: [n1 benchmark. n1 := n1 + 1].
  t1 := Time millisecondClockValue - t1.
  r1 := (n1 * 500000 * 1000) // t1.
  n2 := 1. t2 := Time millisecondClockValue.
  [Time millisecondClockValue - t2 < 1000] whileTrue: [28 benchFib. n2 := n2 + 1].
  t2 := Time millisecondClockValue - t2.
  r2 := (n2 * 9227465 * 1000) // t2.
  ^ r1 printString, '' bytecodes/sec; '', r2 printString, '' sends/sec''' classified: 'benchmarks'
!

PharoBenchmarkRunner class compile: 'runFibonacci
  | t |
  t := Time millisecondClockValue.
  28 benchFib.
  t := Time millisecondClockValue - t.
  ^ ''fib(28) = '', t printString, '' ms''' classified: 'benchmarks'
!

PharoBenchmarkRunner class compile: 'runSieve
  | t count |
  t := Time millisecondClockValue.
  count := 100 benchmark.
  t := Time millisecondClockValue - t.
  ^ ''sieve x100 = '', t printString, '' ms ('', count printString, '' primes)''' classified: 'benchmarks'
!

PharoBenchmarkRunner class compile: 'runSort
  | a t |
  a := (1 to: 100000) asArray copy shuffled.
  t := Time millisecondClockValue.
  a sort.
  t := Time millisecondClockValue - t.
  ^ ''sort 100K = '', t printString, '' ms''' classified: 'benchmarks'
!

PharoBenchmarkRunner class compile: 'runDict
  | d keys t |
  keys := (1 to: 50000) collect: [:i | ''key'', i printString].
  d := Dictionary new: 75000.
  t := Time millisecondClockValue.
  keys do: [:k | d at: k put: k].
  keys do: [:k | d at: k].
  t := Time millisecondClockValue - t.
  ^ ''dict 50K put+get = '', t printString, '' ms''' classified: 'benchmarks'
!

PharoBenchmarkRunner class compile: 'runSum
  | a sum t |
  a := (1 to: 1000000) asArray.
  sum := 0.
  t := Time millisecondClockValue.
  a do: [:each | sum := sum + each].
  t := Time millisecondClockValue - t.
  ^ ''sum 1M = '', t printString, '' ms (sum='', sum printString, '')''' classified: 'benchmarks'
!

PharoBenchmarkRunner class compile: 'runFactorial
  | t |
  t := Time millisecondClockValue.
  5000 factorial.
  t := Time millisecondClockValue - t.
  ^ ''5000 factorial = '', t printString, '' ms''' classified: 'benchmarks'
!

PharoBenchmarkRunner class compile: 'runBlock
  | t counter |
  counter := 0.
  t := Time millisecondClockValue.
  1000000 timesRepeat: [counter := counter + 1].
  t := Time millisecondClockValue - t.
  ^ ''1M blocks = '', t printString, '' ms''' classified: 'benchmarks'
!

PharoBenchmarkRunner class compile: 'runInstVar
  | t obj |
  obj := OrderedCollection new.
  t := Time millisecondClockValue.
  1000000 timesRepeat: [obj size. obj yourself].
  t := Time millisecondClockValue - t.
  ^ ''1M getter+yourself = '', t printString, '' ms''' classified: 'benchmarks'
!

PharoBenchmarkRunner class compile: 'runCreate
  | t |
  t := Time millisecondClockValue.
  100000 timesRepeat: [Array new: 10].
  t := Time millisecondClockValue - t.
  ^ ''100K allocations = '', t printString, '' ms''' classified: 'benchmarks'
!

"Register as session handler"
SessionManager default register:
  (ClassSessionHandler forClassNamed: #PharoBenchmarkRunner)
!
