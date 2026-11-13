"nlrhome.st -- non-local return home search and the ensure shortcut, run on our VM and on Cog; diff the two answers.
Run: pass the text of nlrhome.st as the eval argument (test_load_image P.image eval TEXT; for Cog, arch -x86_64 pharo C.image eval TEXT).
Each scenario runs cold and then 2000 times in a loop (machine code); the answer is the cold printString, whether every loop answer
equalled the cold answer, and the last loop answer.  Scenarios a..g: see docs/vm-compat-bugs.md, 2026-11-12 (NLR home by context identity)."

| results run trait cls src |
results := OrderedCollection new.
"globals are reached with Smalltalk at: because the whole eval text is compiled before it runs"
Smalltalk at: #NlrBox put: (Array new: 4).
trait := Smalltalk at: #TNlrHome ifAbsent: [ (Trait << #TNlrHome) package: 'NlrProbe'; install ].
cls := Smalltalk at: #NlrHomeC ifAbsent: [ (Object << #NlrHomeC) slots: { #keep }; package: 'NlrProbe'; install ].
cls setTraitComposition: trait asTraitComposition.

"Methods are compiled twice: once on the class (cXxx) and once in the trait (tXxx), so the trait copy keeps CompiledBlocks naming the trait's method."
#(
'Down: b  b value. ^ #fell'
'A1 ^ self NAMEDown: [ ^ #a1 ]'
'A2 ^ self NAMEDown: [ [ ^ #a2 ] value ]'
'A3 ^ self NAMEDown: [ [ [ ^ #a3 ] value ] value ]'
'A4 | blk | blk := [ :x | [ ^ x + 1 ] value ]. self NAMEDown: [ blk value: 4 ]. ^ #fell'
'Deeper: b  ^ self NAMEDeeper2: b'
'Deeper2: b  | c | c := thisContext sender. c sender. ^ b value'
'B1 | blk | blk := [ ^ #b1 ]. thisContext. self NAMEDeeper: blk. ^ #fell'
'B1b | blk | blk := [ [ ^ #b1b ] value ]. thisContext sender. thisContext. self NAMEDeeper: blk. ^ #fell'
'B2 ^ self NAMEDown: [ [ Error signal: #x ] on: Error do: [ :e | ^ #b2 ] ]'
'B3 | blk | blk := [ ^ #b3 ]. [ Error signal: #x ] on: Error do: [ :e | blk value ]. ^ #fell'
'B3b | blk | blk := [ ^ #b3b ]. thisContext. [ Error signal: #x ] on: Error do: [ :e | self NAMEDown: blk ]. ^ #fell'
'B4 | blk | blk := [ ^ #b4 ]. [ self NAMEDown: blk ] ensure: [ 2 ]. ^ #fell'
'B5 | blk | blk := [ ^ #b5 ]. [ thisContext. self NAMEDown: blk ] ifCurtailed: [ 2 ]. ^ #fell'
'B5b | blk | blk := [ ^ #b5b ]. [ 3 ] ifCurtailed: [ 2 ]. [ self NAMEDown: blk ] ifCurtailed: [ 2 ]. ^ #fell'
'B6 | blk sem | blk := [ ^ #b6 ]. sem := Semaphore new. [ sem signal ] fork. sem wait. self NAMEDown: blk. ^ #fell'
'B6b | blk sem | blk := [ ^ #b6b ]. thisContext. sem := Semaphore new. [ sem signal ] fork. Processor yield. sem wait. [ self NAMEDown: blk ] value. ^ #fell'
'B7 | blk sem r | blk := [ ^ #b7 ]. sem := Semaphore new. [ r := [ blk value ] on: Error do: [ :e | e class name ]. sem signal ] fork. sem wait. ^ r'
'Make: n with: blk | b | b := [ ^ #dead ]. n < 0 ifTrue: [ ^ b ]. n = 0 ifTrue: [ ^ blk value ]. ^ self NAMEMake: n - 1 with: blk'
'Make2: n with: blk | b | b := [ [ ^ #dead2 ] value ]. n < 0 ifTrue: [ ^ b ]. n = 0 ifTrue: [ ^ blk value ]. ^ self NAMEMake2: n - 1 with: blk'
'E1: log  ^ self NAMEDown: [ [ ^ #e1 ] ensure: [ log add: 1 ] ]'
'E1m: log  [ ^ #e1m ] ensure: [ log add: 1 ]. ^ #fell'
'E2: log  ^ self NAMEDown: [ [ [ ^ #e2 ] ensure: [ log add: 1 ] ] ensure: [ log add: 2 ] ]'
'E2m: log  [ [ ^ #e2m ] ensure: [ log add: 1 ] ] ensure: [ log add: 2 ]. ^ #fell'
'E3: log  [ [ [ ^ #e3 ] ifCurtailed: [ log add: 1 ] ] ensure: [ log add: 2 ] ] ifCurtailed: [ log add: 3 ]. ^ #fell'
'E4: log  [ self NAMEDown: [ ^ #e4 ] ] ensure: [ log add: 1. [ ^ #e4inner ] ensure: [ log add: 2 ] ]. ^ #fell'
'F ^ [ ^ 7 ] ensure: [ 2 ]'
'G1: log  [ :e1 | log add: 1. [ :e2 | log add: 2. e2 value. log add: #no ] valueWithExit. log add: 3. e1 value. log add: #no2 ] valueWithExit. log add: 4. ^ log'
'G2: log  [ :e1 | [ :e2 | log add: 1. e1 value. log add: #no ] valueWithExit. log add: #no2 ] valueWithExit. log add: 2. ^ log'
'G3: n log: log  [ :e | log add: n. n > 0 ifTrue: [ self NAMEG3: n - 1 log: log ]. log add: -1 - n. e value. log add: #no ] valueWithExit. log add: 100 + n. ^ log'
'G4: n exits: ex log: log  [ :e | ex add: e. n > 0 ifTrue: [ self NAMEG4: n - 1 exits: ex log: log ] ifFalse: [ ex first value ]. log add: #no ] valueWithExit. log add: 100 + n. ^ log'
'G5: n exits: ex log: log  [ :e1 | [ :e2 | ex add: e1. ex add: e2. n > 0 ifTrue: [ self NAMEG5: n - 1 exits: ex log: log ] ifFalse: [ (ex at: 1) value ]. log add: #no ] valueWithExit. log add: #no2 ] valueWithExit. log add: 100 + n. ^ log'
'G6: n exits: ex log: log  [ :e1 | [ :e2 | ex add: e2. n > 0 ifTrue: [ self NAMEG6: n - 1 exits: ex log: log ] ifFalse: [ (ex at: ex size) value ]. log add: #no ] valueWithExit. log add: 200 + n ] valueWithExit. log add: 100 + n. ^ log'
) do: [ :s |
	{ cls. trait } do: [ :target |
		| prefix |
		prefix := target == cls ifTrue: [ 'c' ] ifFalse: [ 't' ].
		"selector is NAME = prefix, then the text; method heads are written as 'A1 ^ ...' so prefix goes in front of the head and of every NAME"
		src := (prefix , (s copyReplaceAll: 'NAME' with: prefix)).
		target compile: src ] ].

run := [ :name :blk |
	| cold loop same last |
	cold := [ blk value printString ] on: Error do: [ :e | 'ERR ', e class name, ' ', (e messageText ifNil: [ '' ]) asString ].
	same := true. last := nil.
	"last is the last loop answer, or the first one that differed from the cold answer"
	2000 timesRepeat: [ | a |
		a := [ blk value printString ] on: Error do: [ :e | 'ERR ', e class name, ' ', (e messageText ifNil: [ '' ]) asString ].
		(same and: [ a ~= cold ]) ifTrue: [ same := false. last := a ].
		same ifTrue: [ last := a ] ].
	results add: name -> { cold. same. last } ].

"Every scenario below is run for the class copy (c) and the trait copy (t)."
{ 'c'. 't' } do: [ :p |
	| o |
	o := cls new.
	"(a) block ^ passed down, home still on the stack"
	#( #A1 #A2 #A3 #A4 ) do: [ :sel | run value: p, sel value: [ o perform: (p, sel) asSymbol ] ].
	"(b) home materialized, handlers, ensure, process switch"
	#( #B1 #B1b #B2 #B3 #B3b #B4 #B5 #B5b #B6 #B6b #B7 ) do: [ :sel | run value: p, sel value: [ o perform: (p, sel) asSymbol ] ].
	"(c)/(d) dead home: no other activation, then another live activation of the same method on the stack"
	run value: p, 'DeadNone' value: [ | d | d := o perform: (p, 'Make:with:') asSymbol with: -1 with: nil. d value ].
	run value: p, 'DeadRec' value: [ | d | d := o perform: (p, 'Make:with:') asSymbol with: -1 with: nil. o perform: (p, 'Make:with:') asSymbol with: 3 with: d ].
	run value: p, 'DeadNone2' value: [ | d | d := o perform: (p, 'Make2:with:') asSymbol with: -1 with: nil. d value ].
	run value: p, 'DeadRec2' value: [ | d | d := o perform: (p, 'Make2:with:') asSymbol with: -1 with: nil. o perform: (p, 'Make2:with:') asSymbol with: 3 with: d ].
	run value: p, 'DeadRecHandled' value: [ | d | d := o perform: (p, 'Make:with:') asSymbol with: -1 with: nil. [ o perform: (p, 'Make:with:') asSymbol with: 2 with: d ] on: BlockCannotReturn do: [ :e | e return: #handled ] ].
	"(e) ^ through ensure: blocks, answer and log"
	#( #E1: #E1m: #E2: #E2m: #E3: #E4: ) do: [ :sel | run value: p, sel value: [ | log r | log := OrderedCollection new. r := o perform: (p, sel) asSymbol with: log. { r. log asArray } ] ].
	"(g) valueWithExit nested"
	run value: p, 'G1' value: [ o perform: (p, 'G1:') asSymbol with: OrderedCollection new ].
	run value: p, 'G2' value: [ o perform: (p, 'G2:') asSymbol with: OrderedCollection new ].
	run value: p, 'G3' value: [ o perform: (p, 'G3:log:') asSymbol with: 2 with: OrderedCollection new ].
	run value: p, 'G4' value: [ o perform: (p, 'G4:exits:log:') asSymbol with: 2 with: OrderedCollection new with: OrderedCollection new ].
	run value: p, 'G5' value: [ o perform: (p, 'G5:exits:log:') asSymbol with: 2 with: OrderedCollection new with: OrderedCollection new ].
	run value: p, 'G6' value: [ o perform: (p, 'G6:exits:log:') asSymbol with: 2 with: OrderedCollection new with: OrderedCollection new ] ].

"(f) a wrapper on Context>>aboutToReturn:through: must see every ^ through ensure:"
[ | o orig loop box |
	o := cls new.
	box := Smalltalk at: #NlrBox.
	box at: 1 put: 0.
	loop := [ :n | | last | last := nil. n timesRepeat: [ last := o cF ]. last ].
	"warm the loop first so the shortcut is hot before the wrapper goes in"
	results add: #fWarm -> (loop value: 3000) printString.
	orig := Context compiledMethodAt: #aboutToReturn:through:.
	Context compile: ((orig sourceCode copyReplaceAll: 'aboutToReturn:' with: 'aboutToReturnOrigNlr:')) classified: 'nlrprobe'.
	Context compile: 'aboutToReturn: result through: firstUnwindContext
	(Smalltalk at: #NlrBox) at: 1 put: ((Smalltalk at: #NlrBox) at: 1) + 1.
	^ self aboutToReturnOrigNlr: result through: firstUnwindContext' classified: 'nlrprobe'.
	box at: 1 put: 0.
	results add: #fWrapped -> { (loop value: 3000) printString. (box at: 1) }.
	box at: 1 put: 0.
	results add: #fWrappedAgain -> { (loop value: 3000) printString. (box at: 1) }.
	box at: 1 put: 0.
	results add: #fWrappedCold -> { o cF printString. (box at: 1) }.
	"restore"
	Context addSelector: #aboutToReturn:through: withMethod: orig.
	Context removeSelector: #aboutToReturnOrigNlr:through:.
	box at: 1 put: 0.
	results add: #fRestored -> { (loop value: 3000) printString. (box at: 1) }.
	results add: #fRestoredSame -> ((Context compiledMethodAt: #aboutToReturn:through:) == orig) printString ] on: Error do: [ :e | results add: #fERR -> (e class name, ' ', e messageText asString) ].

String lf join: (results collect: [ :a | a printString ])
