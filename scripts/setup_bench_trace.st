"Bench tracing — minimal version.

 Replaces selected methods with trace-augmented bodies (no wrapper
 dance, no perform:).  Only methods on PharoBenchmarkRunner and a
 small set of suspect classes get touched.  Keeps everything else
 intact so the bench actually runs.

 Trace events go to PHARO_BENCH_TRACE path; flushed every 50 events
 + on disable + when the bench prints a benchmark line.

 Run on Cog → /tmp/trace-cog.txt
 Run on ours → /tmp/trace-ours.txt
 Then `diff` them — first divergence is the bug source."!

ShiftClassInstaller make: [:b |
  b name: #BenchTracer;
    superclass: Object;
    package: 'BenchmarkRunner']
!

BenchTracer class instanceVariableNames: 'counter tracePath enabled lines fileStream'
!

BenchTracer class compile: 'reset
  counter := 0.
  enabled := false.
  tracePath := nil.
  lines := OrderedCollection new' classified: 'control'
!

BenchTracer class compile: 'enableTo: aPath
  self reset.
  tracePath := aPath.
  fileStream := aPath asFileReference writeStream.
  enabled := true' classified: 'control'
!

BenchTracer class compile: 'disable
  enabled := false.
  self flushLines.
  fileStream ifNotNil: [fileStream close. fileStream := nil].
  tracePath := nil' classified: 'control'
!

BenchTracer class compile: 'enabled
  ^ enabled == true' classified: 'control'
!

"Write in-memory lines to the open file stream, flush OS buffer."!
BenchTracer class compile: 'flushLines
  (lines isNil or: [lines isEmpty]) ifTrue: [^ self].
  fileStream isNil ifTrue: [^ self].
  lines do: [:l | fileStream nextPutAll: l].
  fileStream flush.
  lines := OrderedCollection new' classified: 'control'
!

"Cross-VM stable summary of an object.  Returns class name + a
 brief content tag (no SHA1 — too JIT-heavy and triggers other
 bugs).  Two events with same string here means same call;
 different string means divergence."!
BenchTracer class compile: 'hashOf: obj
  obj == nil ifTrue: [^ ''nil''].
  obj == true ifTrue: [^ ''true''].
  obj == false ifTrue: [^ ''false''].
  obj class == SmallInteger ifTrue: [^ ''SmI:'' , obj printString].
  (obj isKindOf: Symbol) ifTrue: [^ ''#'' , obj].
  ((obj isKindOf: String) and: [obj isSymbol not]) ifTrue: [
    ^ ''Str:'' , (obj copyFrom: 1 to: (obj size min: 32))].
  (obj isKindOf: Character) ifTrue: [^ ''Ch:'' , obj asInteger printString].
  (obj isKindOf: Number) ifTrue: [^ obj class name , '':'' , obj printString].
  ^ obj class name' classified: 'hashing'
!

"log:event:value: — single trace event, in-memory."!
BenchTracer class compile: 'log: aTag at: aLabel value: anObj
  enabled ifFalse: [^ self].
  counter := counter + 1.
  lines add: (
    ''['' , counter printString , ''] '' , aTag , '' '' , aLabel ,
    '' sha='' , (self hashOf: anObj) , String lf).
  (counter \\ 50 = 0) ifTrue: [self flushLines]' classified: 'logging'
!

"Direct rewrite of PharoBenchmarkRunner class >> log:to: with trace
 prints inline.  No method wrapping, no perform: gymnastics."!
PharoBenchmarkRunner class compile: 'log: aString to: aStream
  BenchTracer log: ''ENTER'' at: ''log:to:'' value: aString.
  aStream nextPutAll: aString; lf; flush.
  Transcript show: aString; cr.
  BenchTracer log: ''EXIT '' at: ''log:to:'' value: aString.
  BenchTracer flushLines.   "force flush so partial benches still trace"
  ^ self' classified: 'private'
!

"Hook into bench startup: read PHARO_BENCH_TRACE env var and enable."!
PharoBenchmarkRunner class compile: 'startUp: resuming
  resuming ifTrue: [
    | path |
    path := OSEnvironment current at: ''PHARO_BENCH_TRACE'' ifAbsent: [nil].
    (path notNil and: [path isEmpty not]) ifTrue: [BenchTracer enableTo: path].
    self runBenchmarksAsync]' classified: 'system startup'
!

"Re-define runBenchmarksAsync to flush trace on completion."!
PharoBenchmarkRunner class compile: 'runBenchmarksAsync
  [
    (Delay forSeconds: 2) wait.
    [self runBenchmarks]
      on: Error
      do: [:e |
        | s |
        s := ''/tmp/pharo_benchmarks.txt'' asFileReference writeStream.
        [s nextPutAll: ''ERROR: ''. s nextPutAll: (e messageText ifNil: [''<nil>''])]
          ensure: [s close].
        BenchTracer log: ''ERROR'' at: e class name value: (e messageText ifNil: [''nil''])].
    BenchTracer disable.
    Smalltalk exitSuccess
  ] forkAt: Processor highestPriority - 1' classified: 'running'
!
