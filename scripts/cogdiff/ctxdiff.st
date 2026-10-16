"ctxdiff.st -- scenarios run on our VM and on Cog; diff the two answers.
Run: test_load_image P.image eval "$(cat ctxdiff.st)"; for Cog, arch -x86_64 Pharo --headless C.image eval ...
See docs/vm-compat-bugs.md, 2026-10-17."

| results run |
results := OrderedCollection new.
run := [:name :blk | results add: name -> ([ blk value ] on: Error do: [:e | 'ERR ', e class name, ' ', (e messageText ifNil: ['']) asString ]) ].
Object compile: 'ctxA: x | t | t := x * 2. ^ self ctxB: t'.
Object compile: 'ctxB: y | u | u := y + 1. ^ thisContext'.
Object compile: 'ctxLive: x | t | t := x + 1. ^ { thisContext pc. thisContext stackPtr. thisContext tempNamed: #t. thisContext tempNamed: #x. thisContext size. thisContext method selector. thisContext sender method selector. thisContext isDead. thisContext receiver }'.
Object compile: 'ctxBlock | a | a := 5. ^ [:b | { thisContext isBlockContext. thisContext home method selector. thisContext outerContext method selector. thisContext tempNamed: #b. thisContext numArgs. thisContext closure numArgs. a } ] value: 7'.
Object compile: 'ctxStepTarget ^ 3 + 4'.
run value: #dead value: [ | c | c := nil ctxA: 3. { c isDead. c pc. c sender. c method selector. c tempNamed: #y. c tempNamed: #u. c size. c stackPtr } ].
run value: #live value: [ nil ctxLive: 10 ].
run value: #block value: [ nil ctxBlock ].
run value: #stepping value: [ | ctx log | log := OrderedCollection new.
   ctx := Context sender: nil receiver: nil method: (Object>>#ctxStepTarget) arguments: #().
   [ ctx notNil and: [ ctx isDead not ] and: [ log size < 20 ] ] whileTrue: [ log add: ctx pc. ctx := ctx step ].
   log asArray ].
run value: #runSimulated value: [ (Context runSimulated: [ 3 + 4 ]) ].
run value: #simulatedSends value: [ (Context runSimulated: [ #(3 1 2) asSortedCollection asArray ]) ].
run value: #simulatedException value: [ Context runSimulated: [ [ 1/0 ] on: ZeroDivide do: [:e | e return: 42 ] ] ].
run value: #simulatedEnsure value: [ | l | l := OrderedCollection new. Context runSimulated: [ [ l add: 1 ] ensure: [ l add: 2 ] ]. l asArray ].
run value: #simulatedNLR value: [ Context runSimulated: [ #(1 2 3) detect: [:x | x > 1 ] ] ].
run value: #senderChain value: [ | c n | c := thisContext. n := 0. [ c notNil ] whileTrue: [ n := n + 1. c := c sender ]. n > 2 ].
run value: #contextAt value: [ | c | c := nil ctxA: 3. { c basicSize > 0. (c at: 1) } ].
run value: #copyStack value: [ | c | c := thisContext copyStack. { c method selector notNil. c sender notNil } ].
run value: #findContext value: [ (thisContext findContextSuchThat: [:c | c method selector = #value]) notNil ].
run value: #returnTo value: [ | r | r := [:k | 10 + (thisContext return: 5) ] value: 1. r ].
run value: #restartLoop value: [ | n c | n := 0. c := nil. [ n := n + 1. c := thisContext. n < 3 ifTrue: [ c restart ]. n ] value ].
run value: #unwindTo value: [ | l | l := OrderedCollection new. [ [ [ l add: 1. thisContext sender sender unwindTo: thisContext sender sender sender. l add: 2 ] ensure: [ l add: #e1 ] ] value ] value. l asArray ].
run value: #methodPcRange value: [ (Object>>#ctxA:) endPC - (Object>>#ctxA:) initialPC ].
run value: #blockPcs value: [ | b | b := [:x | x + 1 ]. { b startpc class. b numArgs. b isClean } ].
run value: #tempNamesLive value: [ (nil ctxA: 2) tempNames asArray ].
run value: #stackOfSize value: [ (thisContext stackOfSize: 3) size ].
run value: #terminateTo value: [ | c d | c := thisContext. d := [ c sender ] value. d notNil ].
run value: #printLive value: [ (nil ctxA: 3) printString ].
results asArray
