"deep_recursion.st -- recursions that are deeper than 56000 frames.  Each line of
 the answer is one check: a name, a colon and the answer.  Stock Cog has no depth
 limit, so each line must be the same on Cog and on our VM.

   scripts/vmrun.sh eval cog  scripts/cogdiff/deep_recursion.st
   scripts/vmrun.sh eval cogp scripts/cogdiff/deep_recursion.st
   scripts/vmrun.sh eval ours scripts/cogdiff/deep_recursion.st

 The recursion with no end that ends in OutOfMemory is in lowspace_recursion.st.

 Measured 2026-11-17.  Stock Cog, patched Cog (cogp) and our VM (JIT on, and with
 PHARO_NO_JIT=1, and with PHARO_NO_MCF=1) give the same eleven lines:
   block 1000000 deep with an end: 1000000
   method 1000000 deep with an end: 1000000
   method 300000 deep, thisContext at each level: 300000
   non-local return at each of 200000 levels: 200000
   error at depth 300000, caught at the top: #caught
   ensure blocks run: 100000 levels: 100000
   non-local return from depth 300000 to the top: 42
   non-local return from depth 100000 runs 100000 ensure blocks: 7->100000
   sender chain at depth 500000: true
   shouldOverflow set at depth 200000, then 100000 frames more: error caught: #caught
   process stopped by a watcher at depth 1000000: true
 Before the change our VM stopped each recursion deeper than 56000 frames with
 'Error: stack overflow: recursion deeper than 56000 frames' (measured for the
 method recursion of 1000000 frames), and the watcher line could not be true.
 The process with shouldOverflow true, which gets #stackOverflow at the depth
 limit, is ProcessTest>>testStackOverflow."

| results run |
(Object << #DeepProbe) slots: { #count }; package: 'DeepProbe'; install.
(Smalltalk at: #DeepProbe) compile: 'plain: n
	n = 0 ifTrue: [ ^ 0 ].
	^ (self plain: n - 1) + 1'.
(Smalltalk at: #DeepProbe) compile: 'withContext: n
	n = 0 ifTrue: [ ^ 0 ].
	thisContext.
	^ (self withContext: n - 1) + 1'.
(Smalltalk at: #DeepProbe) compile: 'viaDo: n
	#(1) do: [ :x |
		n = 0 ifTrue: [ ^ 0 ].
		^ (self viaDo: n - 1) + 1 ].
	^ -1'.
(Smalltalk at: #DeepProbe) compile: 'withEnsure: n
	n = 0 ifTrue: [ ^ self error: ''bottom'' ].
	^ [ self withEnsure: n - 1 ] ensure: [ count := (count ifNil: [ 0 ]) + 1 ]'.
(Smalltalk at: #DeepProbe) compile: 'count
	^ count'.
(Smalltalk at: #DeepProbe) compile: 'chainLength: n
	n = 0 ifTrue: [ | c k | c := thisContext. k := 0. [ c notNil ] whileTrue: [ k := k + 1. c := c sender ]. ^ k ].
	^ self chainLength: n - 1'.
(Smalltalk at: #DeepProbe) compile: 'farNlr: n
	^ self farNlrWith: [ :v | ^ v ] depth: n'.
(Smalltalk at: #DeepProbe) compile: 'farNlrWith: blk depth: n
	n = 0 ifTrue: [ ^ blk value: 42 ].
	^ (self farNlrWith: blk depth: n - 1) + 1'.
(Smalltalk at: #DeepProbe) compile: 'ensureNlr: n
	^ self ensureNlrWith: [ :v | ^ v ] depth: n'.
(Smalltalk at: #DeepProbe) compile: 'ensureNlrWith: blk depth: n
	n = 0 ifTrue: [ ^ blk value: 7 ].
	^ [ (self ensureNlrWith: blk depth: n - 1) + 1 ] ensure: [ count := (count ifNil: [ 0 ]) + 1 ]'.
(Smalltalk at: #DeepProbe) compile: 'flagAt: n then: m
	n = 0 ifTrue: [ Processor activeProcess shouldOverflow: true. ^ [ self plain: m ] ensure: [ Processor activeProcess shouldOverflow: false ] ].
	^ (self flagAt: n - 1 then: m) + 1'.
(Smalltalk at: #DeepProbe) compile: 'forever: n
	^ (self forever: n + 1) + 1'.
results := OrderedCollection new.
run := [ :name :blk | | a |
	a := [ blk value printString ] on: Error do: [ :e | 'ERROR ' , e class name , ': ' , (e messageText ifNil: [ '' ]) asString ].
	results add: name , ': ' , a ].
run value: 'block 1000000 deep with an end' value: [ | rec | rec := nil. rec := [ :n | n = 0 ifTrue: [ 0 ] ifFalse: [ (rec value: n - 1) + 1 ] ]. rec value: 1000000 ].
run value: 'method 1000000 deep with an end' value: [ (Smalltalk at: #DeepProbe) new plain: 1000000 ].
run value: 'method 300000 deep, thisContext at each level' value: [ (Smalltalk at: #DeepProbe) new withContext: 300000 ].
run value: 'non-local return at each of 200000 levels' value: [ (Smalltalk at: #DeepProbe) new viaDo: 200000 ].
run value: 'error at depth 300000, caught at the top' value: [ [ (Smalltalk at: #DeepProbe) new withEnsure: 300000 ] on: Error do: [ :e | e return: #caught ] ].
run value: 'ensure blocks run: 100000 levels' value: [ | p | p := (Smalltalk at: #DeepProbe) new. [ p withEnsure: 100000 ] on: Error do: [ :e | e return: 0 ]. p count ].
run value: 'non-local return from depth 300000 to the top' value: [ (Smalltalk at: #DeepProbe) new farNlr: 300000 ].
run value: 'non-local return from depth 100000 runs 100000 ensure blocks' value: [ | p r | p := (Smalltalk at: #DeepProbe) new. r := p ensureNlr: 100000. r -> p count ].
run value: 'sender chain at depth 500000' value: [ ((Smalltalk at: #DeepProbe) new chainLength: 500000) >= 500000 ].
run value: 'shouldOverflow set at depth 200000, then 100000 frames more: error caught' value: [ [ (Smalltalk at: #DeepProbe) new flagAt: 200000 then: 100000. #noError ] on: Error do: [ :e | #caught ] ].
run value: 'process stopped by a watcher at depth 1000000' value: [
	| p stopped sem |
	sem := Semaphore new.
	stopped := false.
	p := [ (Smalltalk at: #DeepProbe) new forever: 1 ] forkAt: Processor userBackgroundPriority.
	[ | d |
	  d := 0.
	  [ (Delay forMilliseconds: 50) wait.
	    d := 0. p suspendedContext ifNotNil: [ :c | | x | x := c. [ x notNil and: [ d < 1000000 ] ] whileTrue: [ d := d + 1. x := x sender ] ].
	    d >= 1000000 or: [ p isTerminated ] ] whileFalse.
	  p terminate. stopped := p isTerminated. sem signal ] forkAt: Processor userInterruptPriority.
	(sem waitTimeoutSeconds: 60) ifTrue: [ p terminate. #timeout ] ifFalse: [ stopped ] ].
results inject: '' into: [ :acc :each | acc , each , String lf ]
