"KendrickOrg/kendrick.  Entries for the tests of the packages Kendrick-Core,
Kendrick-DSL and Kendrick-Tests.  The load itself is repaired by the patch of
the baseline (patches/kendrickorg-kendrick.patch); this file repairs the tests
and the code that the tests call.  Every entry has a guard on the old source:
the guard is true for the source as the package has it, and false after the
repair, so the file can be filed in again with no change.

CAUSES, in plain words.  The letters are the letters of the sections below.

A. testEquivalence of eight DSL test classes.  In January 2020 the class
KETimeSeries was removed and the data frames of DataFrame took its place.  The
tests still send #data to a KEChart (a KEChart has #dataFrames) and
#peakOfEpidemic to a time series (nothing implements the selector any more).
The tests that were moved half way compare the collections of data frames one to
one, but the chart before the DSL has one data frame with a column for each
compartment, and the chart after the DSL has one data frame for each
compartment.  Each test now compares the sorted peaks (the largest value of a
column, rounded down) of all columns of all data frames of the two charts.  This
is what the old test compared: one peak for each time series.  The calls that
open an inspector on the models have no assertion and are not in the new bodies.

B. Code of the part before that gives KEChart>>addDataFrame: a block, an
OrderedCollection or an Array instead of a data frame (TestOneSpeciesSpatialScripts,
TestDependantConcern, TestMultiSpeciesSpatial): the chart then sends #columnNames
to the collection.  And three tests whose chart before holds one data frame with
a column for each patch, where the DSL chart after holds the sum over the patches
for each species or strain (TestMultiSpeciesSpatialDependant, TestsModel5, and
TestMultiSpeciesSpatial above).  The code before now adds the sum, as the DSL does.
TestsModel5>>before had no ^ and answered the test, not the three results.

C. Visualization>>sumOverTimeSeries: says in its own comment that it does not
work (it sends #name: to a number).  KESimulator gets #sumOfTimeSeriesOutputsAt:
(a data frame with one column that is the sum of the columns of
#timeSeriesOutputsAt:), Simulation delegates to the simulator, and the
visualization uses the method.

D. KEMapBuilder builds the picture with Roassal 2 (RTMapBuilder and others) that
Pharo 14 does not have, and the class has no #plot that Visualization>>canvas
sends.  The picture is now a Roassal 3 canvas: a labelled box for each country, a
grid layout (the data has no geography), a line for each route.  The test
TestsModel6SpatialMap took the first row of the series where the other test and
the DSL take the peak; it takes the peak now.  Both map tests asked a map for
#dataFrames; a map has #data (numbers).

E. TestsIST is the root of the DSL tests and its testScript skips for the root
class itself.  TestsIST is an abstract test class now (no test instance), and
shouldInheritSelectors keeps the old rule for the subclasses.

F. KEChart sends RSChart>>addPlot:.  In Roassal 3 of Pharo 14 RSChart is a
deprecated subclass of RSCompositeChart, and the selector is add:.

G. Composition>>doesNotUnderstand: looks for a parameter (lambda: #(beta*I/N sum))
in the parameters of the Composition, which stay empty.  The parameters that the
concerns bring are in the ModelComponent of the Composition (its #model).  The
method looks in both.

H. KEComponent>>attributes: did not look at the names of the attributes, only at
the values, so two attributes with the same name replaced each other with no
error.  It also answers for the Dictionary that KEModelPart class>>dependOn: gives.
KEComponent>>transitions turned any non-empty #equations into transitions, but
after #transitionsToEquations the instance variable holds a Dictionary of
transitions, not a list of equations, and #generateEvents then sent
#generateEvents to an OrderedCollection.  KEComponent>>transitionsToEquations
dropped every transition from or to the empty compartment (births and deaths) and
stopped with KeyNotFound for a compartment that no attribute names (a model built
from transitions only).

I. KEUnaryExpression>>removeVariable: changed the receiver (it set the expression
of the receiver) instead of answering a copy.

J. KEModel>>equations2 builds the symbolic equations from the probabilities of the
transitions, and a probability may be a block or a number (KETransition accepts a
block).  A block in a binary expression has no #normalize.  A KEBlockExpression
wraps the block, and a number becomes a KENumerical.

K. Four tests have the mark expectedFailure.  Each hides one of the defects of
H, I and J, or the wrong number of the test below.  The marks go.
KEEventTest>>testVerifyNumberOfEventOfSumOfTwoConcerns asked a model built from
transitions for its events.  Events exist only as the step between equations and
transitions (equationsToTransitions makes one transition for each event), so a
model built from transitions has its events as transitions: the count is 4
transitions (2 transitions of the first concern, 2 species of the second).

L. KESimulatorTest>>testKEDeterministicSimulationGiveAlwaysSameMax sent #max to
the #contents of a data frame.  #contents of DataFrame answers an internal object
that is not a collection.  The peak of the data frame is its #max (one value for
each column), and the frame has one column.

M. KEPopulation>>initialSize: was removed in January 2020 (the commit says that
only KEPopulation class>>size: stays, for the DSL) but that class method, and so
ModelComponent>>population:, sends #initialSize:.

N. KEDeterministicSimulator>>executeOnWithKEEquations: evaluated the equations at
the compartments of the start of the step for each stage of the solver, whatever
the stage vector was: the Runge-Kutta solver was the method of Euler.  The
compartments now take the values of the stage vector before the equations are
evaluated, as executeOnWithTransitions: does.  Proof: the maxima of a measles
model with the old code are the maxima of a hand written Euler method, and with
the new code they are the maxima of a hand written fourth order Runge-Kutta method.

O. Simulation>>forModel: sent #doesNotUnderstand: with a String to the class
Composition (the class side method of KendrickInternalDSLEntities reads #selector
of its argument).  The simulation of a stand-alone model takes the ModelComponent
of that name, and a ModelComponent answers itself to #model, as the Simulation
and the Visualization expect of a Composition.

P. Test code that the DSL changes of 2020 to 2022 left behind.
TestMeaslesScripts>>after defines the stand-alone model as a KModel (a concern
that has no population); it is a ModelComponent.  TestDependantConcern and
TestMosquitoScripts send #asArray to one value (a number, a symbol) to make an
Array of one element: Object has no #asArray, and the symbol would give an Array
of characters.  They send Array with: instead.  TestsModel5, TestsModel6SpatialMap,
TestsModel6SpatialMapWithEntity and TestMosquitoScripts look for the
ModelComponent 'Influenza' or 'Mosquito' by name, and no such entity exists; they
ask the simulation for its model.  TestMosquitoScripts>>after defines the mosquito
model as a ModelComponent that the composition does not integrate (Composition>>model:
takes a KModel); it is a KModel integrated by the composition, without the population:
line that a KModel does not understand.

Q. KendrickEquations>>convertToCompatibleBackendRepresentation read the equations
from the print string of the Array of tokens.  In Pharo 14 a symbol like S:t prints
with quotes, and the tokens were cut at the quotes into pieces that are not
equations (PPFailure).  The tokens are now joined one by one, and a period token
ends an equation.

R. Scenario>>addCompositeParameter:value: has two gaps in the DSL syntax that the
tests use.  A value of an attribute after the underscore (rho_species_Q: #(0.03 0.1
0)) adds one more value to the array, for the compartments that have that attribute
value (the quarantine status Q has no migration); the method ignored it, so the
quarantined individuals moved.  The word next after an attribute key
(sigma_species_status_next) selects the value of the compartment that the
transition goes to; the method has no code for it (Composition and an older
method have), so the rate E to I1 was zero and no epidemic started.

S. The simulation tests need more time than the SUnit limit (10 s for a test).
No defect makes them slow.  Each test runs a Runge-Kutta or a Gillespie
simulation twice (the model before the DSL and the model after the DSL), and the
time is the work of the simulation: 500 steps, four stages for each step, 50
compartments, and for each transition a call of the probability block and a sum
over the compartments.  A sample of the running process (every 10 ms, 445 samples
of the model of TestsModel5 before the DSL) shows no hot spot and no time in
Dictionary>>scanFor:, so the hash of a Dictionary as a key is not the cause: the
time is spread over the copy of the key dictionaries, the sums, the probability
blocks and, for Gillespie, Random>>nextInteger:.  The tests need 3 s to 21 s on
the machine of the run, so the classes of the simulation tests get a class-side
defaultTimeLimit of 3 minutes (room for a machine that is 3 times slower).  The
limit of the other tests of the image does not change."


"Two helpers live in globals for the time of the file-in: the entries file is one
chunk, and a line of temporaries in the middle of a chunk is a syntax error.  The
globals are removed at the end of the file.

KendrickRangeFix has one argument, an Array of class name, selector, start text,
end text and new text; the text from the start text to the end text is replaced.
KendrickEdit has one argument, an Array of class name, selector, a text that the
old source has, a text that the new source has (empty when the first text goes away),
and an Array of old text and new text, in pairs; each old text is replaced.  A block
has no value:value:value:value:value: (five arguments), so each call gives one Array."
Smalltalk globals at: #KendrickRangeFix put: [ :args | | clsName sel startText endText newText cls source i j |
	clsName := args at: 1.
	sel := args at: 2.
	startText := args at: 3.
	endText := args at: 4.
	newText := args at: 5.
	cls := resolve value: clsName.
	(cls notNil and: [ cls includesSelector: sel ])
		ifTrue: [
			source := (cls >> sel) sourceCode.
			i := source indexOfSubCollection: startText.
			j := i = 0 ifTrue: [ 0 ] ifFalse: [ source indexOfSubCollection: endText startingAt: i ].
			fix value: clsName value: sel
				value: [ :each | i > 0 and: [ j > 0 ] ]
				value: ((i > 0 and: [ j > 0 ])
					ifTrue: [ (source copyFrom: 1 to: i - 1) , newText , (source copyFrom: j + endText size to: source size) ]
					ifFalse: [ '' ]) ]
		ifFalse: [ fix value: clsName value: sel value: [ :each | false ] value: '' ] ].

Smalltalk globals at: #KendrickEdit put: [ :args | | clsName sel guardText doneText pairs cls source ok |
	clsName := args at: 1.
	sel := args at: 2.
	guardText := args at: 3.
	doneText := args at: 4.
	pairs := args at: 5.
	cls := resolve value: clsName.
	source := (cls notNil and: [ cls includesSelector: sel ])
		ifTrue: [ (cls >> sel) sourceCode ]
		ifFalse: [ '' ].
	ok := (source includesSubstring: guardText) and: [ doneText isEmpty or: [ (source includesSubstring: doneText) not ] ].
	pairs pairsDo: [ :old :new | (source includesSubstring: old) ifFalse: [ ok := false ] ].
	ok ifTrue: [ pairs pairsDo: [ :old :new | source := source copyReplaceAll: old with: new ] ].
	fix value: clsName value: sel value: [ :each | ok ] value: source ].

"=== A. testEquivalence of the DSL tests ==="

#(#TestMeaslesScripts #TestMeaslesScriptsBMC #TestMeaslesScriptsODEsBMC #TestDependantConcern
  #TestMultiSpeciesSpatial #TestMultiSpeciesSpatialDependant #TestOneSpeciesSpatialScripts #TestsModel5)
	do: [ :name |
		add value: name value: #peaksOf: value: 'private' value:
'peaksOf: aChart
	"The peak of every time series of the chart, rounded down, in ascending order.  A data frame of the chart has one column for each time series."
	^ (aChart dataFrames gather: [ :frame | frame max values collect: [ :peak | peak floor ] ]) asSortedCollection asOrderedCollection'.
		fix value: name value: #testEquivalence
			value: [ :source | (source includesSubstring: 'peaksOf:') not and: [ source includesSubstring: 'Diagram' ] ]
			value:
'testEquivalence
	| resultsBefore resultsAfter |
	self createModels.
	resultsBefore := self peaksOf: beforeDiagram.
	resultsAfter := self peaksOf: afterDiagram.
	self assert: resultsBefore equals: resultsAfter' ].

"=== B. chart code of the part before ==="

(Smalltalk globals at: #KendrickRangeFix) value: { #TestOneSpeciesSpatialScripts. #before.
	'chart addDataFrame: [|tmp| tmp := OrderedCollection new.'.
	'tmp] value.'.
	'(1 to: 5) do: [ :patch |
		chart addDataFrame: (simulator timeSeriesOutputsAt: { #status -> #I. #patch -> patch }) ].' }.

(Smalltalk globals at: #KendrickRangeFix) value: { #TestDependantConcern. #before.
	'chart addDataFrame: [|tmp| tmp := DataFrame new.'.
	'tmp] value.'.
	'(1 to: 5) do: [ :patch |
		chart addDataFrame: (simulator timeSeriesOutputsAt: { #status -> #I. #patch -> patch }) ].' }.

(Smalltalk globals at: #KendrickRangeFix) value: { #TestMultiSpeciesSpatial. #before.
	'f := [:name| |df ds|'.
	'#human}) }.'.
	'chart
		addDataFrame: (simulator sumOfTimeSeriesOutputsAt: { #status -> #I. #species -> #bird });
		addDataFrame: (simulator sumOfTimeSeriesOutputsAt: { #status -> #I. #species -> #human }).' }.

"=== C. sum of time series ==="

add value: #KESimulator value: #sumOfTimeSeriesOutputsAt: value: 'public' value:
'sumOfTimeSeriesOutputsAt: aCompartmentName
	"A data frame with one column.  The column has, for each time, the sum of the columns that #timeSeriesOutputsAt: answers for aCompartmentName.  The column is named aCompartmentName, the rows are the times."
	| frame sums |
	frame := self timeSeriesOutputsAt: aCompartmentName.
	sums := frame rowNames collect: [ :time |
		(frame row: time) values inject: 0 into: [ :sum :each | sum + each ] ].
	^ DataFrame
		withColumns: { sums asArray }
		rowNames: frame rowNames asArray
		columnNames: { aCompartmentName }'.

add value: #Simulation value: #sumOfTimeSeriesOutputsAt: value: 'protected' value:
'sumOfTimeSeriesOutputsAt: anIdentifier
	^ simulator sumOfTimeSeriesOutputsAt: anIdentifier'.

fix value: #Visualization value: #sumOverTimeSeries:
	value: [ :source | source includesSubstring: 'df column: each' ]
	value:
'sumOverTimeSeries: name
	"Answer a data frame with one column: the sum over the time series of name"
	^ simulation sumOfTimeSeriesOutputsAt: name asArray'.

"=== D. KEMapBuilder in Roassal 3, and the two map tests ==="

fix value: #KEMapBuilder value: #build
	value: [ :source | source includesSubstring: 'RTMapBuilder' ]
	value:
'build
	"A Roassal 3 canvas of the map: a labelled box for each country, white to red by the value of the country relative to the largest value, a grid layout, and a line for each route."
	| canvas values max shapes |
	canvas := RSCanvas new.
	values := countries withIndexCollect: [ :country :i | data at: i ifAbsent: [ 0 ] ].
	max := values inject: 1 into: [ :largest :each | largest max: each ].
	shapes := RSGroup withAll: (countries withIndexCollect: [ :country :i | | box label |
		box := RSBox new size: 60; color: (Color white mixed: ((values at: i) / max) with: Color red); yourself.
		label := RSLabel new text: country asString; color: Color black; yourself.
		RSComposite new shapes: (RSGroup withAll: { box. label }); model: country; yourself ]).
	canvas addAll: shapes.
	RSGridLayout on: shapes.
	routes do: [ :route | | from to |
		from := shapes detect: [ :each | each model = route key ] ifNone: [ nil ].
		to := shapes detect: [ :each | each model = route value ] ifNone: [ nil ].
		(from notNil and: [ to notNil ])
			ifTrue: [ canvas add: (RSLine new color: Color gray; from: from; to: to; yourself) ] ].
	canvas @ RSCanvasController.
	^ canvas'.

fix value: #KEMapBuilder value: #open
	value: [ :source | source includesSubstring: 'b build' ]
	value:
'open
	^ self build open'.

fix value: #KEMapBuilder value: #view
	value: [ :source | source includesSubstring: 'b build' ]
	value:
'view
	^ self build'.

add value: #KEMapBuilder value: #plot value: 'as yet unclassified' value:
'plot
	^ self open'.

(Smalltalk globals at: #KendrickRangeFix) value: { #TestsModel6SpatialMap. #before.
	'rowAt:1) max'.
	'rowAt:1) max'.
	'columnAt: 1) max' }.

#(#TestsModel6SpatialMap #TestsModel6SpatialMapWithEntity) do: [ :name |
	fix value: name value: #testEquivalence
		value: [ :source | source includesSubstring: 'dataFrames sorted' ]
		value:
'testEquivalence
	| beforeResults afterResults |
	self createModels.
	beforeResults := (beforeDiagram data collect: [ :val | val roundDownTo: 0.001 ]) asSortedCollection asOrderedCollection.
	afterResults := (afterDiagram data collect: [ :val | val roundDownTo: 0.001 ]) asSortedCollection asOrderedCollection.
	self assert: beforeResults equals: afterResults' ].

"=== E. TestsIST ==="

add value: #'TestsIST class' value: #isAbstract value: 'testing' value:
'isAbstract
	^ self == TestsIST'.

"The subclasses of TestsIST that have tests of their own must NOT inherit the
tests of TestsIST (testScript) because TestsIST is abstract now: TestCase class>>
shouldInheritSelectors is true for the subclass of an abstract class.  This keeps the
old rule (a subclass inherits only when it has no test of its own)."
add value: #'TestsIST class' value: #shouldInheritSelectors value: 'testing' value:
'shouldInheritSelectors
	^ self ~~ TestsIST and: [ self testSelectors isEmpty ]'.


"=== B (more). the chart before holds the sum over the patches, as the chart after does ==="

(Smalltalk globals at: #KendrickRangeFix) value: { #TestMultiSpeciesSpatialDependant. #before.
	'addDataFrame: (simulator timeSeriesOutputsAt: {#status -> #I. #species -> #bird});'.
	'#species ->#human}).'.
	'addDataFrame: (simulator sumOfTimeSeriesOutputsAt: { #status -> #I. #species -> #bird });
		addDataFrame: (simulator sumOfTimeSeriesOutputsAt: { #status -> #I. #species -> #human }).' }.

(Smalltalk globals at: #KendrickRangeFix) value: { #TestsModel5. #before.
	'addDataFrame: (simulator timeSeriesOutputsAt: {#status -> #I1.#species -> #bird})'.
	'#I2. #species -> #human}).'.
	'addDataFrame: (simulator sumOfTimeSeriesOutputsAt: { #status -> #I1. #species -> #bird });
		addDataFrame: (simulator sumOfTimeSeriesOutputsAt: { #status -> #I1. #species -> #human });
		addDataFrame: (simulator sumOfTimeSeriesOutputsAt: { #status -> #I2. #species -> #bird });
		addDataFrame: (simulator sumOfTimeSeriesOutputsAt: { #status -> #I2. #species -> #human }).' }.

(Smalltalk globals at: #KendrickEdit) value: { #TestsModel5. #before. 'chart plot'. '^ { simulator . model . chart }'.
	#( 'chart plot' 'chart plot.
	^ { simulator . model . chart }' ) }.

"=== F. KEChart: RSChart>>addPlot: is RSCompositeChart>>add: ==="
#(#plot #plotHistogram #plotLog #plotScatter #plotSqrt) do: [ :sel |
	(Smalltalk globals at: #KendrickEdit) value: { #KEChart. sel. 'c addPlot: p'. 'c add: p'.
		#( 'c addPlot: p' 'c add: p' ) } ].

"=== G. Composition: a parameter that an integrated model has ==="
(Smalltalk globals at: #KendrickEdit) value: { #Composition. #doesNotUnderstand:.
	'(self parameters keys includes: variable)'. 'self model parameters keys includes: variable'.
	#( '(self parameters keys includes: variable)'
		'((self parameters keys includes: variable) or: [ self model parameters keys includes: variable ])' ) }.

"=== H. KEComponent: attribute names, transitions and equations ==="
fix value: #KEComponent value: #attributes:
	value: [ :source | (source includesSubstring: 'duplicateValues:') and: [ (source includesSubstring: 'asSet size') not ] ]
	value:
'attributes: attributesArray

	"attributesArray is an array of associations like {#status->#(#S #I #R). #age->#(#child #adult)}, or a Dictionary.  The names of the attributes must be different, and no value may be in two attributes."

	(attributesArray isDictionary not
		and: [ (attributesArray collect: [ :each | each key ]) asSet size ~= attributesArray size ])
		ifTrue: [ self error: ''Attribute names are duplicated.'' ].
	(self duplicateValues: attributesArray)
		ifTrue: [ self error: ''Attributes are duplicated.'' ]
		ifFalse: [ attributes := Dictionary newFrom: attributesArray ]'.

(Smalltalk globals at: #KendrickEdit) value: { #KEComponent. #transitions.
	'equations isNotEmpty ifTrue:'. '' .
	#( 'equations isNotEmpty ifTrue:' '(equations isNotEmpty and: [ equations isDictionary not ]) ifTrue:' ) }.

fix value: #KEComponent value: #transitionsToEquations
	value: [ :source | (source includesSubstring: '(eq at: tr from) add:') and: [ (source includesSubstring: 'ifAbsentPut:') not ] ]
	value:
'transitionsToEquations

	"Return the suitable Dictionary for KESimulatorDeterministic in order to solve the equations.
	This method should be merged with equations method in the same class: issue #40
	The Dictionary has, for each compartment, the list of the transitions that leave the compartment (with the sign -1) or enter the compartment (with the sign 1).  A transition that comes from the empty compartment (a birth) or goes to the empty compartment (a death) belongs to the equation of the compartment at its other end.  A compartment that is not in the attributes of the model, because the model is built from transitions only, gets an equation when a transition names the compartment."

	| eq |
	eq := Dictionary new.
	self finestCompartmentNames do: [ :k | eq at: k put: OrderedCollection new ].
	self transitions do: [ :tr |
		(tr from at: #status) ~= #empty ifTrue: [
			(eq at: tr from ifAbsentPut: [ OrderedCollection new ])
				add: (Array with: tr with: -1) ].
		(tr to at: #status) ~= #empty ifTrue: [
			(eq at: tr to ifAbsentPut: [ OrderedCollection new ])
				add: (Array with: tr with: 1) ] ].
	equations := eq'.

"=== I. KEUnaryExpression>>removeVariable: changed the receiver ==="
fix value: #KEUnaryExpression value: #removeVariable:
	value: [ :source | source includesSubstring: '(self op: #-) expression:' ]
	value:
'removeVariable: aVariable
	"This function is reserved for the reformulation of rate expression to probability of contact of an event generated from an equation.  The receiver does not change: the answer is a copy."

	| new |
	new := self copy.
	new op: #-.
	new expression: (expression removeVariable: aVariable).
	^ new'.

"=== J. equations2 of a model whose probabilities are blocks or numbers ==="
(Smalltalk globals includesKey: #KEBlockExpression)
	ifFalse: [
		(KEExpression << #KEBlockExpression)
			slots: { #block };
			package: 'Kendrick-Core-MM1';
			install.
		Stdio stdout nextPutAll: 'ADDED class KEBlockExpression'; lf; flush ].

add value: #'KEBlockExpression class' value: #block: value: 'instance creation' value:
'block: aBlock
	^ self new block: aBlock'.
add value: #KEBlockExpression value: #block value: 'accessing' value: 'block
	^ block'.
add value: #KEBlockExpression value: #block: value: 'accessing' value: 'block: aBlock
	block := aBlock'.
add value: #KEBlockExpression value: #evaluateWithModel: value: 'evaluating' value:
'evaluateWithModel: aModel
	^ block value: aModel'.
add value: #KEBlockExpression value: #isSimple value: 'testing' value: 'isSimple
	^ true'.
add value: #KEBlockExpression value: #normalize value: 'converting' value: 'normalize
	^ self'.
add value: #KEBlockExpression value: #removeIndexedVariable value: 'evaluating' value: 'removeIndexedVariable
	^ self'.
add value: #KEBlockExpression value: #removeVariable: value: 'removing' value: 'removeVariable: aVariable
	^ self'.
add value: #KEBlockExpression value: #sign value: 'testing' value: 'sign
	^ 1'.
add value: #KEBlockExpression value: #= value: 'comparing' value: '= anExpression
	^ anExpression class == self class and: [ anExpression block == block ]'.
add value: #KEBlockExpression value: #hash value: 'comparing' value: 'hash
	^ block hash'.
add value: #KEBlockExpression value: #printOn: value: 'printing' value: 'printOn: aStream
	aStream nextPutAll: ''[:aModel | ... ]'''.

add value: #KEModel value: #probabilityExpressionOf: value: 'as yet unclassified' value:
'probabilityExpressionOf: aTransition
	"The probability of a transition is a KEExpression, a number or a block.  A number or a block becomes an expression, so that the equations can be built from it."

	| probability |
	probability := aTransition probability.
	probability isBlock ifTrue: [ ^ KEBlockExpression block: probability ].
	probability isNumber ifTrue: [ ^ KENumerical new number: probability ].
	^ probability'.

(Smalltalk globals at: #KendrickEdit) value: { #KEModel. #equations2.
	'leftHandSide: tr probability;'. ''.
	#( 'leftHandSide: tr probability;' 'leftHandSide: (self probabilityExpressionOf: tr);' ) }.

"=== K. the marks expectedFailure of four tests: the defects are repaired ==="
#( #(#KEEquationTest #testNumberOfEquationsOfSIRModelWithTransitions)
   #(#KEModelTest #testSIRModelBasedOnTransitionsHasThreeEquations)
   #(#KEExpressionTest #testRemoveAVariableShouldNotModifyTheExpression) ) do: [ :pair |
	(Smalltalk globals at: #KendrickEdit) value: { pair first. pair second.
		'<expectedFailure>'. ''. #( '<expectedFailure>' '' ) } ].

(Smalltalk globals at: #KendrickEdit) value: { #KEEventTest. #testVerifyNumberOfEventOfSumOfTwoConcerns.
	'<expectedFailure>'. ''.
	#( '<expectedFailure>' '' 'model generateEvents size equals: 4' 'model transitions size equals: 4' ) }.

"=== L. KESimulatorTest: the peak of a data frame ==="
(Smalltalk globals at: #KendrickEdit) value: { #KESimulatorTest. #testKEDeterministicSimulationGiveAlwaysSameMax.
	'contents max closeTo:'. ''.
	#( 'contents max closeTo:' 'max first closeTo:' ) }.

"=== M. KEPopulation>>initialSize: ==="
add value: #KEPopulation value: #initialSize value: 'accessing' value:
'initialSize
	^ initialSize'.
add value: #KEPopulation value: #initialSize: value: 'accessing' value:
'initialSize: aNumber
	initialSize := aNumber'.

"=== N. the stage vector of the ODE solver ==="
fix value: #KEDeterministicSimulator value: #executeOnWithKEEquations:
	value: [ :source | (source includesSubstring: 'system := PMExplicitSystem block:') and: [ (source includesSubstring: 'put: (v at: i)') not ] ]
	value:
'executeOnWithKEEquations: aModel

	"Suppose that aModel is built from equations
	Ask a model its equations then solve them with an ODE solver.  The solver calls the block of the system with its own intermediate states (the argument v).  The compartments of the model must hold the values of v before the equations are evaluated: if they do not, the solver evaluates every stage at the start of the step, and the method is the method of Euler."

	| keys values system |
	super executeOn: aModel.
	equations := model equations.
	keys := equations collect: [ :each | #status -> each variable symbol ].
	system := PMExplicitSystem block: [ :v :t |
		keys doWithIndex: [ :each :i |
			model population compartments
				at: (Dictionary with: each)
				put: (v at: i) ].
		equations collect: [ :each |
			each evaluateWithModel: aModel ] ].
	self stepper: (self algorithm onSystem: system).
	values := keys collect: [ :each |
		model population compartments at: (Dictionary with: each) ].
	(tMin to: tMax by: step) do: [ :t |
		self saveCompartmentsValueAtTime: t.
		model t: t.
		values := stepper doStep: values time: t stepSize: step.
		keys doWithIndex: [ :each :i |
			model population compartments
				at: (Dictionary with: each)
				put: (values at: i) ] ]'.

"=== O. Simulation>>forModel: and ModelComponent>>model ==="
(Smalltalk globals at: #KendrickEdit) value: { #Simulation. #forModel:.
	'Composition doesNotUnderstand: aModelID'. 'ModelComponent withId: aModelID'.
	#( 'Composition doesNotUnderstand: aModelID' 'ModelComponent withId: aModelID' ) }.

add value: #ModelComponent value: #model value: 'protected' value:
'model
	"A model that is simulated by itself is its own model.  A Composition answers the ModelComponent that it integrates."

	^ self'.

"=== P. test code that the changes of the DSL left behind ==="
(Smalltalk globals at: #KendrickEdit) value: { #TestMeaslesScripts. #after.
	'KModel'. 'ModelComponent'. #( 'KModel' 'ModelComponent' ) }.

#( #TestDependantConcern #TestMosquitoScripts ) do: [ :name |
	(Smalltalk globals at: #KendrickEdit) value: { name. #before.
		'sizeOfPopulation: c asArray'. '(Array with: c)'.
		#( 'sizeOfPopulation: c asArray' 'sizeOfPopulation: (Array with: c)' ) } ].

#( #TestsModel5 #TestsModel6SpatialMap #TestsModel6SpatialMapWithEntity #TestMosquitoScripts ) do: [ :name |
	(Smalltalk globals at: #KendrickEdit) value: { name. #createModels.
		'afterModel := ModelComponent withId:'. 'afterSimulator model'.
		(name = #TestMosquitoScripts
			ifTrue: [ #( 'ModelComponent withId: ''Mosquito''' 'afterSimulator model' ) ]
			ifFalse: [ #( 'ModelComponent withId: ''Influenza''' 'afterSimulator model' ) ]) } ].

(Smalltalk globals at: #KendrickEdit) value: { #TestMosquitoScripts. #after.
	'ModelComponent Mosquito'. 'KModel Mosquito'.
	#( 'ModelComponent Mosquito' 'KModel Mosquito' 'population: 13000;' ''
		'model: ''SpeciesConcern'';' 'model: ''Mosquito'';
	model: ''SpeciesConcern'';' ) }.

"=== Q. KendrickEquations: the tokens of an equation are joined, not read from the print string ==="
fix value: #KendrickEquations value: #convertToCompatibleBackendRepresentation
	value: [ :source | source includesSubstring: 'findTokens:' ]
	value:
'convertToCompatibleBackendRepresentation
	"The equations are an Array of tokens like #(S:t = mu * N - beta * S * I - mu * S . E:t = ...).  A token that is a period ends one equation.  Answer the equations, each one as a String.  The tokens are used one by one: the print string of an Array of symbols differs between versions of Pharo (a symbol like #S:t has quotes in the print string of Pharo 14), so the print string must not be used."

	| equations current |
	equations := OrderedCollection new.
	current := WriteStream on: String new.
	eqAsSymExpr do: [ :token |
		token asString = ''.''
			ifTrue: [
				current contents trimBoth ifNotEmpty: [ :each | equations add: each ].
				current := WriteStream on: String new ]
			ifFalse: [ current nextPutAll: token asString; space ] ].
	current contents trimBoth ifNotEmpty: [ :each | equations add: each ].
	^ equations'.

"=== R. Scenario>>addCompositeParameter:value: ==="
(Smalltalk globals at: #KendrickEdit) value: { #Scenario. #addCompositeParameter:value:.
	'secondOccInd|'. ''.
	#( 'secondOccInd|' 'secondOccInd valueOverrides nextKeys|'
		'ind := (aModel attributes at: k) indexOf: (aModel currentCompartment at: k).'
		'ind := (aModel attributes at: k) indexOf: (((nextKeys includes: k) ifTrue: [ aModel nextCompartment ] ifFalse: [ aModel currentCompartment ]) at: k).'
		'"translate from underscore syntax to back end"'
		'"The word next after an attribute key (sigma_species_status_next) means that the value of that attribute is the one of the compartment that the transition goes to, not the one that it leaves.  A value of an attribute after the underscore (rho_species_Q: #(0.03 0.1 0)) adds one more value to the array: the value for the compartments that have that attribute value, here the compartments with the status Q.  The position of the value in the array is after the values of the first attribute key."
	nextKeys := Set new.
	variablePlusAttributes doWithIndex: [ :token :position |
		(token = #next and: [ position > 1 ]) ifTrue: [ nextKeys add: (variablePlusAttributes at: position - 1) ] ].
	valueOverrides := OrderedCollection new.
	variablePlusAttributes allButFirst doWithIndex: [ :attrValue :position | | owner |
		owner := backEndEntity attributes keys
			detect: [ :k | (backEndEntity attributes at: k) includes: attrValue ]
			ifNone: [ nil ].
		(owner notNil and: [ (attrKeys includes: attrValue) not ])
			ifTrue: [ valueOverrides add: { owner. attrValue. (backEndEntity attributes at: attrKeys first) size + valueOverrides size + 1 } ] ].
	"translate from underscore syntax to back end"'
		'res ].' 'valueOverrides do: [ :each |
				(aModel currentCompartment at: each first) = each second
					ifTrue: [ res := value at: each third ] ].
			res ].' ) }.

"=== S. a time limit for the simulation tests ==="

add value: #'TestsIST class' value: #defaultTimeLimit value: 'accessing' value:
'defaultTimeLimit
	"The tests of the DSL scripts run a simulation twice (before and after the DSL).  Measured on one machine (arm64, 2026-11-17), the time of one test: TestsModel3 3.4 s, TestsModel4 4.1 to 5.8 s, TestsModel5 9.6 s and more than 10 s on stock Cog, TestsModel6SpatialMap 7.3 s, TestsModel6SpatialMapWithEntity 7.4 s.  The time is the work of the simulation, not a defect.  3 minutes leave room for a machine that is 3 times slower."

	^ 3 minutes'.

#( #TestMosquitoScripts #TestMultiSpeciesSpatial #TestMultiSpeciesSpatialDependant #TestOneSpeciesSpatialScripts #TestDependantConcern ) do: [ :name |
	add value: (name , ' class') asSymbol value: #defaultTimeLimit value: 'accessing' value:
'defaultTimeLimit
	"The test runs a simulation twice (before and after the DSL).  Measured on one machine (arm64, 2026-11-17), testEquivalence: TestMosquitoScripts 20.9 s (Gillespie), TestMultiSpeciesSpatial 5.3 s, TestMultiSpeciesSpatialDependant 5.1 s, TestOneSpeciesSpatialScripts 2.0 s, TestDependantConcern 1.9 s.  The time is the work of the simulation, not a defect.  3 minutes leave room for a machine that is 3 times slower."

	^ 3 minutes' ].

Smalltalk globals removeKey: #KendrickRangeFix ifAbsent: [ nil ].
Smalltalk globals removeKey: #KendrickEdit ifAbsent: [ nil ].
