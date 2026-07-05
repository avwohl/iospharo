| r t1 t2 t3 baseline |
"Activation-cost micro-bench (2026-07-05).  Isolates the J2J METHOD-SEND
 activation cost (the ~210-cycle / ~45-67x-Cog wall — see memory
 block-invocation-perf.md).  The callee body is deliberately NON-TRIVIAL
 for the IC classifier: two statements with a temp, so it cannot be
 stolen by retLit (bit 58), TempReturn (54), IntCmpReturn (53),
 IntArithReturn (52), getter/setter/returnsSelf (63/62/61) — the send
 MUST take the real J2J activation path.
 NOTE: methods are compiled onto Association (in-memory only; eval mode
 never saves).  Class creation and Object compile: worked when retried
 on a quieter machine — the earlier silent-failure was load-dependent
 (concurrent VMs), so leaf-class compile: is just the more robust
 pattern, not a hard requirement.  Beware Smalltalk comments cannot
 contain a double-quote: an embedded one ends the comment and the rest
 parses as code, failing the whole DoIt silently.
 Run per the README (cat this file into the eval argument)."
Association compile: 'perfActCallee: x
	| t |
	t := x + 1.
	t := t + 0.
	^ t'.
Association compile: 'perfActBlock
	^ [ :x | | t | t := x + 1. t + 0 ]'.
Association compile: 'perfActBenchSend: n
	| r |
	r := 0.
	1 to: n do: [ :i | r := self perfActCallee: i ].
	^ r'.
Association compile: 'perfActBenchBlock: n
	| r b |
	r := 0.
	b := self perfActBlock.
	1 to: n do: [ :i | r := b value: i ].
	^ r'.
Association compile: 'perfActBenchEmpty: n
	| r |
	r := 0.
	1 to: n do: [ :i | r := r + i ].
	^ r'.
r := Association new.
"warm: get driver + callee T1-compiled and J2J-linked"
r perfActBenchSend: 400000. r perfActBenchBlock: 400000. r perfActBenchEmpty: 400000.
t1 := Time millisecondsToRun: [ r perfActBenchSend: 5000000 ].
t2 := Time millisecondsToRun: [ r perfActBenchBlock: 5000000 ].
baseline := Time millisecondsToRun: [ r perfActBenchEmpty: 5000000 ].
t3 := Time millisecondsToRun: [ r perfActBenchSend: 5000000 ].
'ACTBENCH send5M=', t1 printString, 'ms send5M(2)=', t3 printString,
	'ms block5M=', t2 printString, 'ms loop-baseline=', baseline printString,
	'ms  per-send-ns~=', ((t1 - baseline) * 1000000 // 5000000) printString
