"Split defect #6's whole-image scan (NoUnusedVariablesLeftTest >>
 testNoUnusedTemporaryVariablesLeft, i.e. m hasTemporaries and: [m ast ...])
 into its phases, so the per-VM ratio can be read per phase rather than for
 the whole 5.8 s (Cog) vs 217 s (ours) scan.  On a quiet machine:
   arch -x86_64 /tmp/harness-x86/pharo /tmp/harness-x86/Pharo.image eval THIS-FILE-TEXT
   ./build-rel/test_load_image /tmp/harness/Pharo.image eval THIS-FILE-TEXT
 where THIS-FILE-TEXT is the shell expansion of cat on this file.
 Answers one PHASE line per phase; n methods, the first n with temps."
| n methods sources t out |
n := 3000.
methods := OrderedCollection new.
t := Time millisecondsToRun: [
	SystemNavigation default allMethodsDo: [ :m |
		(methods size < n and: [ m hasTemporaries ]) ifTrue: [ methods add: m ] ] ].
out := String new writeStream.
out nextPutAll: 'PHASE enumerate '; print: t; cr.
t := Time millisecondsToRun: [ sources := methods collect: [ :m | m sourceCode ] ].
out nextPutAll: 'PHASE sourceCode '; print: t; cr.
t := Time millisecondsToRun: [ sources do: [ :s | RBScanner scanTokens: s ] ].
out nextPutAll: 'PHASE scan '; print: t; cr.
t := Time millisecondsToRun: [ sources do: [ :s | RBParser parseMethod: s ] ].
out nextPutAll: 'PHASE parse '; print: t; cr.
t := Time millisecondsToRun: [ methods do: [ :m | m parseTree doSemanticAnalysis ] ].
out nextPutAll: 'PHASE parse+semantic '; print: t; cr.
t := Time millisecondsToRun: [ methods do: [ :m | m ast temporaries anySatisfy: [ :x | x binding isUsed not ] ] ].
out nextPutAll: 'PHASE testBody '; print: t; cr.
t := Time millisecondsToRun: [ 1 to: 3000000 do: [ :i | i printString ] ].
out nextPutAll: 'PHASE printString3M '; print: t; cr.
out contents
