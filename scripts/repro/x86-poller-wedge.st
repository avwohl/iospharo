"Reproducer of an OPEN defect: on x86_64 the whole VM wedges in 2 to 5 percent
 of runs of this script.  arm64 does not show the defect.

 What the script does: it makes 14000 processes and resumes up to 16 of them.
 Each resumed process recurses 20000 levels and yields every 500 levels.  A
 priority 70 process waits on a 1 ms Delay in a loop (the poller).  No process
 overflows the stack.

 A good run ends in 1 to 3 s and answers 'groupsize=N k=K finished=K'.
 A wedged run does not end: the VM is idle, only the idle process is queued,
 the Delay timer is not armed, and a waitTimeoutMilliseconds: does not fire.
 Use a timeout, and count the runs that end with exit status 124.

 Measured 2026-11-15, wedged runs with a 14 s timeout:
   build-x86 (JIT) at commit 93352576            5 of 150
   build-x86 (JIT) with the stack-depth fix      6 of 150
   build-nojit-x86 at commit 93352576            6 of 300
   build-nojit-x86 with the stack-depth fix      5 of 300
   build-rel and build-nojit (arm64)             0 of 120
 The two build-x86 rows are one run of this file, with the two binaries in
 turn.  The build-nojit-x86 rows are from the same script before it had this
 header.  The arm64 row is from a variant in which the recursion overflows the
 stack.  With a 90 s timeout on build-x86, 7 of 150 runs did not end, and no
 run ended between 5 s and 90 s.  A wedged run does not recover in 90 s.

 The defect is therefore older than the stack-depth fix of 2026-11-15.  The
 lead is the timer path or the idle path on x86_64.  All x86_64 runs were
 under Rosetta.  See docs/deferred.md, 'x86_64 wedge'.

 The 14000 processes and the grouping by identity hash come from the test that
 found the wedge (a test of the process depth table).  They were not reduced.
 The script was not shown to need them.

 Run, in a new directory that holds a copy of a pristine Pharo 14 image:
   rm -f startup.st
   timeout 14 <repo>/build-nojit-x86/test_load_image Pharo.image eval \
     ""Smalltalk compiler evaluate: ('<repo>/scripts/repro/x86-poller-wedge.st' asFileReference contents)""
   rm -f /tmp/sunit_run_completed.txt
 The script compiles four methods in the protocol 'tb' of Object and sets the
 global TW.  Use a copy of the image and delete the copy after the run."
| procs grp k sem results ok |
Object compile: 'tbL: n lim: m (n \\ 500) = 0 ifTrue: [Processor yield]. n >= m ifTrue: [^ 0]. ^ 1 + (self tbL: n + 1 lim: m)' classified: 'tb'.
Object compile: 'tbMk: count ^ (1 to: count) collect: [:i | | p | p := [ ((Smalltalk at: #TW) at: p identityHash ifAbsent: [nil]) ifNotNil: [:w | w value] ] newProcess]' classified: 'tb'.
Object compile: 'tbGroup: procs | by best | by := Dictionary new. procs do: [:p | | h | h := p identityHash. (by at: ((h bitXor: (h >> 11)) bitAnd: 2047) ifAbsentPut: [OrderedCollection new]) add: p]. best := by values inject: OrderedCollection new into: [:a :c | c size > a size ifTrue: [c] ifFalse: [a]]. ^ best' classified: 'tb'.
[[(Delay forMilliseconds: 1) wait] repeat] forkAt: 70.
Smalltalk at: #TW put: Dictionary new.
procs := nil tbMk: 14000.
grp := (nil tbGroup: procs) asArray.
k := grp size min: 16.
results := Array new: k. sem := Semaphore new.
1 to: k do: [:i | | p | p := grp at: i.
  (Smalltalk at: #TW) at: p identityHash put: [ | r | r := [nil tbL: 0 lim: 20000] on: Error do: [:e | 'SIG']. Processor yield. 1 to: 3 do: [:j | Processor yield. nil tbL: 0 lim: 300]. results at: i put: r. sem signal ].
  p resume].
ok := 0. k timesRepeat: [(sem waitTimeoutMilliseconds: 120000) ifFalse: [ok := ok + 1]].
'groupsize=', grp size printString, ' k=', k printString, ' finished=', ok printString
