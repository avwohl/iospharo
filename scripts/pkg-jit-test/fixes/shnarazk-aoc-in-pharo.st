"shnarazk/AoC-in-Pharo (row 119).

1. The last commit of the repository (20b89cf) renamed #at: to #atGrid: also
in code whose receiver is not a GridArray: a Dictionary, an Array and the
OSEnvironment (AoCSolver class>>projectDirectoryAnd: sent atGrid: to the
environment).  Only GridArray has #atGrid:.  The entries send #at: again in
the 10 methods.  The three methods of AoCSolver class use OSEnvironment
current (OSPlatform>>environment is deprecated in Pharo 14).

2. GridArray>>x:y: and x:y:put: gave an Integer index to atGrid:, which takes
a Point (GridArrayTest>>testXY: SmallInteger does not understand #y).

Not in this file: the 6 tests of Y2025Day05Test and Y2025Day08Test read the
puzzle input files data/2025/input-dayNN-0.txt.  The repository does not have
them (the inputs of Advent of Code are not for redistribution), so these
tests cannot run on a machine that is not the author's:
scripts/pkg-jit-test/not-applicable/shnarazk-aoc-in-pharo.txt."

fix value: #'AoCSolver class' value: #projectDirectory
	value: [ :source | source includesSubstring: 'atGrid:' ]
	value:
'projectDirectory
	"return AoC top directory"

	| env |
	env := OSEnvironment current.
	^ env at: ''AOC_DIR'' ifAbsent: [ env at: ''HOME'' ]'.

fix value: #'AoCSolver class' value: #'projectDirectory:'
	value: [ :source | source includesSubstring: 'atGrid:' ]
	value:
'projectDirectory: aDirectory
	"set AoC top directory"

	| env |
	env := OSEnvironment current.
	env at: ''AOC_DIR'' put: aDirectory'.

fix value: #'AoCSolver class' value: #'projectDirectoryAnd:'
	value: [ :source | source includesSubstring: 'atGrid:' ]
	value:
'projectDirectoryAnd: subDirectory
	"return AoC top directory"

	| env base |
	env := OSEnvironment current.
	base := env at: ''AOC_DIR'' ifAbsent: [ env at: ''HOME'' ].
	^ base , ''/'' , subDirectory'.

fix value: #GridArray value: #'x:y:'
	value: [ :source | source includesSubstring: 'atGrid:' ]
	value:
'x: xIndex y: yIndex

	^ self at: (yIndex - 1) * width + xIndex'.

fix value: #GridArray value: #'x:y:put:'
	value: [ :source | source includesSubstring: 'atGrid:' ]
	value:
'x: xIndex y: yIndex put: aValue

	^ self at: (yIndex - 1) * width + xIndex put: aValue'.

fix value: #ILPSolver value: #'buildSubProblemOf:by:'
	value: [ :source | source includesSubstring: 'atGrid:' ]
	value:
'buildSubProblemOf: targetEquations by: eliminator
	"return the reduced myself of `targetEquations` stripped the first elements from all arrays in #coeffs based on `eliminator`"

	| c1 e1 v1 eqs instance |
	c1 := (eliminator at: #coeffs) first.
	e1 := (eliminator at: #coeffs) allButFirst.
	v1 := eliminator at: #value.
	eqs := targetEquations collect: [ :equation |
		(equation at: #coeffs) first == 0
			ifTrue: [
				{ #coeffs -> (equation at: #coeffs) allButFirst. #value -> (equation at: #value) } asDictionary ]
			ifFalse: [
				| scale coeffs s1 s2 value |
				scale := c1 lcm: (equation at: #coeffs) first.
				s1 := scale / c1.
				s2 := scale / (equation at: #coeffs) first.
				coeffs := (equation at: #coeffs) allButFirst with: e1 collect: [ :a :e | a * s2 - (e * s1) ].
				value := (equation at: #value) * s2 - (v1 * s1).
				{ #coeffs -> coeffs. #value -> value } asDictionary ] ].
	instance := self class new.
	instance numberOfVariables: numberOfVariables - 1; equations: eqs.
	^ instance'.

fix value: #ILPSolver value: #solveOneVariableEquation
	value: [ :source | source includesSubstring: 'atGrid:' ]
	value:
'solveOneVariableEquation

	| coeff value |
	coeff := (equations first at: #coeffs) first.
	value := equations first at: #value.
	(value isDivisibleBy: coeff)
		ifTrue: [ ^ value / coeff ]
		ifFalse: [ self error: ''not integer problem'' ]'.

fix value: #ILPSolver value: #sortEquations
	value: [ :source | source includesSubstring: 'atGrid:' ]
	value:
'sortEquations
	"reorder equations so that all equation with non-zero coefficient for the first variable should be at the head"

	| found notFound |
	found := OrderedCollection new.
	notFound := OrderedCollection new.
	equations do: [ :equation |
		(equation at: #coeffs) first ~= 0
			ifTrue: [ found add: equation ]
			ifFalse: [ notFound add: equation ] ].
	found addAllLast: notFound.
	equations := found asArray'.

fix value: #Y2024Day07 value: #'line:'
	value: [ :source | source includesSubstring: 'atGrid:' ]
	value:
'line: anInteger

	^ lines at: anInteger'.

fix value: #Y2024Day07 value: #'searchFrom:with:to:'
	value: [ :source | source includesSubstring: 'atGrid:' ]
	value:
'searchFrom: aNumber with: numbers to: target

	| toVisit goalState |
	goalState := { target. numbers size + 1 }.
	toVisit := Heap new sortBlock: [ :a :b | a first > b first ]; add: { aNumber. 1 }; yourself.
	[ toVisit isNotEmpty ] whileTrue: [
		| state |
		state := toVisit removeFirst.
		state last = goalState last
			ifTrue: [ state first = goalState first ifTrue: [ ^ target ] ]
			ifFalse: [
				| operand |
				operand := numbers at: state last.
				toVisit addAll: ({ state first + operand. state first * operand. self shift: state first and: operand }
					select: [ :next | next <= target ]
					thenCollect: [ :val | { val. state last + 1 } ]) ] ].
	^ 0'.

fix value: #Y2025Day05 value: #part2
	value: [ :source | source includesSubstring: 'atGrid:' ]
	value:
'part2

	| result |
	ids ifNil: [ self parse: self inputData ].
	result := self nodeHeap inject: { #level -> 0. #start -> nil. #sum -> 0 } asDictionary into: [ :acc :node |
		node y == 0
			ifTrue: [ "start a new segment"
				acc at: #level update: [ :level | level + 1 ].
				(acc at: #level) == 1 ifTrue: [ acc at: #start put: node x ] ]
			ifFalse: [ "end of a segment"
				acc at: #level update: [ :level | level - 1 ].
				(acc at: #level) == 0 ifTrue: [ acc at: #sum update: [ :sum | sum + node x - (acc at: #start) + 1 ] ] ].
		acc ].
	^ result at: #sum'.

"---- Y2025Day08: the puzzle is not finished in the package -----------------"
