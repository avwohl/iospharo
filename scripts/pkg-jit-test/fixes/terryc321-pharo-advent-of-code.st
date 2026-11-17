"Pharo-Advent-Of-Code (terryc321): 25 tests do not pass.  The causes, with
the evidence, are in docs/upstream/package-repairs.md.  Short list:

 1. String>>asSymbolOrInteger lives in the package AdventCode, which the
    baseline does not load (11 AOC2015Day7Test tests).  An extension method
    is added to the loaded package Advent-Of-Code-2015.
 2. The tests and methods split text with Character lf, but the string
    literals of the loaded methods have CR line ends (AOC2015Day13Test x4,
    AOC2015Day15Test>>testBrute).  The two parsing methods use String>>lines,
    which accepts CR, LF and CRLF.
 3. Tests that cannot pass as written: wrong class and number (Day7 part
    tests, Day4 part 1), a call of a selector that the class does not have
    (Day7 testParseAssign), no connect step (Day7 testInterAnd), a deliberate
    failure (AOC2016Day1Test>>testFailing01, AOC2015Day15Test>>testBrute),
    an expected value of the wrong type (AOC2025Day6SimpleTest part 1), a
    template class (AOC2015DayTestTemplate).
 4. AOC2015Day8>>repoDir names /home/terry/...; the input file is in the
    clone of the repository.
 5. AOC2025Day6>>solution1Part2 and solution2Part2 use the result of
    puzzleInputParsed2 in a way that a later change of the author broke.
 6. AOC2015Day25>>computeRow:column: takes 18 million steps of a Generator
    (7.5 s on our VM, over 10 s on stock Cog); the test class gets a time
    limit of 2 minutes."

"1. the extension method of the package AdventCode, in the loaded package"
add value: #String value: #asSymbolOrInteger value: '*Advent-Of-Code-2015' value:
'asSymbolOrInteger
	"Answer an Integer when the string starts with a digit, else a Symbol."
	(self at: 1) isDigit
		ifTrue: [ ^ self asInteger ]
		ifFalse: [ ^ self asSymbol ]'.

"2. line ends"
fix value: #AOC2015Day13 value: #parse:
	value: [ :source | source includesSubstring: 'Character lf' ]
	value:
'parse: s
	| lines people connections result |
	people := Set new.
	result := Dictionary new.
	lines := s lines.
	connections := Dictionary new.
	lines do: [ :line |
		| split whoA whoB gL val |
		split := line splitOn: Character space.
		whoA := (split at: 1) asSymbol.
		whoB := ((split at: 11) allButLast) asSymbol.
		people add: whoA.
		people add: whoB.
		connections at: whoA ifAbsent: [ connections at: whoA put: (Dictionary new) ].
		connections at: whoB ifAbsent: [ connections at: whoB put: (Dictionary new) ].
		val := (split at: 4) asInteger.
		gL := (split at: 3).
		gL = ''gain''
			ifTrue: [ (connections at: whoA) at: whoB put: val ]
			ifFalse: [ (connections at: whoA) at: whoB put: (0 - val) ] ].
	result at: #people put: people.
	result at: #connections put: connections.
	^ result'.

fix value: #AOC2015Day15 value: #split:
	value: [ :source | source includesSubstring: 'Character lf' ]
	value:
'split: s
	"The lines of the text end with CR, LF or CRLF."
	| lines |
	lines := s lines.
	^ lines collect: [ :line |
		| ig in props p1 p2 dict |
		dict := Dictionary new.
		ig := line splitOn: $: .
		in := ig at: 1.
		props := ig at: 2.
		p1 := props splitOn: $, .
		p2 := p1 collect: [ :e | e splitOn: Character space ].
		p2 do: [ :pr |
			| key val |
			key := (pr at: 2) asSymbol.
			val := (pr at: 3) asInteger.
			dict at: key put: val ].
		{ in asSymbol . dict } ]'.

"AOC2015Day15>>brute: tried every (a b c d) of 0 to 100 (104 million steps) and
used only those with a sum of 100 (176851).  d is 100 - a - b - c.  The order
of the calls of bruteHelper:with: does not change."
fix value: #AOC2015Day15 value: #brute:
	value: [ :source | source includesSubstring: '0 to: 100 do: [:d |' ]
	value:
'brute: aBlock
	| in |
	in := self input.
	best := 0.
	bestCol := OrderedCollection new.
	[ 0 to: 100 do: [ :a |
		0 to: 100 - a do: [ :b |
			0 to: 100 - a - b do: [ :c |
				self bruteHelper: in with: { a . b . c . (100 - a - b - c) } ] ] ].
	"run the block (a test) after brute has run"
	aBlock value ] fork'.

"The test asserted in the forked process and ended with the assertion
true = false.  It waits for the end of brute: now."
fix value: #AOC2015Day15Test value: #testBrute
	value: [ :source | source includesSubstring: 'true = false' ]
	value:
'testBrute
	| aoc done |
	aoc := AOC2015Day15 new.
	done := Semaphore new.
	aoc brute: [ done signal ].
	self deny: (done waitTimeoutSeconds: 8) description: ''brute: did not finish''.
	self assert: ((aoc bestCol at: 1) at: 8) equals: 13882464'.

"3. tests"
fix value: #AOC2015Day4Test value: #testSolution1Part1
	value: [ :source | (source includesSubstring: 'md5driver:') not ]
	value:
'testSolution1Part1
	"solution1Part1 starts a background process and answers nil; md5driver: runs the search in this process.  609043 is the example for the key abcdef; the puzzle key of the class is bgvyzdsv."
	self assert: (AOC2015Day4 new md5driver: ''bgvyzdsv'') equals: 254575'.

fix value: #AOC2015Day7Test value: #testParseAssign
	value: [ :source | source includesSubstring: 'aoc parse: ' ]
	value:
'testParseAssign
	| aoc p |
	aoc := AOC2015Day7 new.
	p := aoc parseAssign: ''lf -> gf''.
	self assert: (p at: 1) = #lf.
	self assert: (p at: 3) = #gf.

	aoc parseLine: ''3 -> ge''.
	self assert: (aoc dict at: #ge) isNode'.

fix value: #AOC2015Day7Test value: #testInterAnd
	value: [ :source | (source includesSubstring: 'interAnd:') not ]
	value:
'testInterAnd
	| aoc |
	aoc := AOC2015Day7 new.
	aoc reset.
	self assert: (aoc interAnd: (aoc parseAnd: ''lf AND hs -> gf'')).
	self assert: (aoc lookup: #gf) isAndNode'.

fix value: #AOC2015Day7Test value: #testSolution1Part1
	value: [ :source | source includesSubstring: 'AOC2015Day5 new' ]
	value:
'testSolution1Part1
	"see testPart1: the answer for day 7 part 1 is 16076"
	self testPart1'.

fix value: #AOC2015Day7Test value: #testSolution1Part2
	value: [ :source | source includesSubstring: 'AOC2015Day5 new' ]
	value:
'testSolution1Part2
	"see testPart2: the answer for day 7 part 2 is 2797"
	self testPart2'.

fix value: #AOC2016Day1Test value: #testFailing01
	value: [ :source | (source includesSubstring: 'should:') not ]
	value:
'testFailing01
	"a deliberate failing assertion: the assertion must signal a TestFailure"
	self should: [ self assert: true equals: false ] raise: TestFailure'.

"The template of the test class of a new day: its two tests are placeholders
(notYetImplemented), and the test runner skips an abstract class."
add value: #'AOC2015DayTestTemplate class' value: #isAbstract value: 'testing' value:
'isAbstract
	"A template to copy for the test class of a new day: the tests are placeholders."
	^ true'.

fix value: #AOC2025Day6SimpleTest value: #testSolution1Part1
	value: [ :source | source includesSubstring: 'solution1Part1) = 4277556' ]
	value:
'testSolution1Part1
	"solution1Part1 answers an Array: the list of the problems and the grand total"
	self assert: ((AOC2025Day6Simple new solution1Part1) at: 2) = 4277556'.

"4. the input file of day 8"
fix value: #AOC2015Day8 value: #repoDir
	value: [ :source | source includesSubstring: '''home''' ]
	value:
'repoDir
	"The directory of the puzzle input of day 8 in the local clone of the repository."
	| repository |
	repository := IceRepository registry detect: [ :each | each name = ''Pharo-Advent-Of-Code'' ].
	^ repository location / ''docs'' / ''advent-code'' / ''2015'' / ''day8'''.

"5. AOC2025Day6 part 2"
fix value: #AOC2025Day6 value: #solution1Part2
	value: [ :source | source includesSubstring: 'fixup 7737' ]
	value:
'solution1Part2
	"The numbers are composed from the digits of the columns of the worksheet.  The worksheet is the lines of puzzleInput, with one space added to each line, so that the last problem ends with a blank column.  A problem ends at a blank column."
	| in width n tot out op vals subtot |
	in := self puzzleInput lines collect: [ :line | line , '' '' ].
	width := (in at: 1) size.
	n := 1.
	tot := 0.
	[ n < width ] whileTrue: [
		out := self takeValsOp: (self takeRows: in row: n).
		op := out at: #op.
		vals := out at: #vals.
		subtot := op = $+
			ifTrue: [ vals inject: 0 into: [ :acc :each | acc + each ] ]
			ifFalse: [
				op = $*
					ifTrue: [ vals inject: 1 into: [ :acc :each | acc * each ] ]
					ifFalse: [ self error: ''no operator in the problem at column '' , n printString ] ].
		tot := tot + subtot.
		n := out at: #next ].
	^ tot'.

fix value: #AOC2025Day6 value: #solution2Part2
	value: [ :source | (source includesSubstring: 'last at: 4') not ]
	value:
'solution2Part2
	"The grand total of the problems read column by column: the fourth slot of the last entry of bypass is the running total."
	self reset.
	self puzzleInputParsed2.
	^ self bypass last at: 4'.

"6. day 25: AOC2015Day25>>computeRow:column: walks the diagonals with a
Generator, one step for each cell.  The cell of testPart1 (row 3010, column
3019) is number 18171059.  The walk took 7.5 s on our VM and more than the
10 s default time limit of SUnit on stock Cog.  The code of the author stays,
and the test class gets a time limit that fits the work."
add value: #AOC2015Day25Test value: #defaultTimeLimit value: 'accessing' value:
'defaultTimeLimit
	"testPart1 makes 18 million Generator steps."

	^ 2 minutes'.
