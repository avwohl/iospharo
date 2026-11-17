"KendrickOrg/kendrick.  Entries for the tests of the packages Kendrick-Core,
Kendrick-DSL and Kendrick-Tests.  The load itself is repaired by the patch of
the baseline (patches/kendrickorg-kendrick.patch); this file repairs the tests
and the code that the tests call.  Every entry has a guard on the old source.

A. Tests that compare a chart before and after the DSL (TestMeaslesScripts,
TestMeaslesScriptsBMC, TestMeaslesScriptsODEsBMC, TestDependantConcern,
TestMultiSpeciesSpatial, TestMultiSpeciesSpatialDependant,
TestOneSpeciesSpatialScripts, TestsModel5, method testEquivalence).  In January
2020 the class KETimeSeries was removed and the data frames of DataFrame took
its place.  The tests still send #data to a KEChart (a KEChart has no #data;
it has #dataFrames) and #peakOfEpidemic to a time series (nothing implements
the selector any more).  The tests that were moved half way send #dataFrames
and compare the collections one to one: the chart before the DSL has one data
frame with a column for each compartment, and the chart after the DSL has one
data frame for each compartment, so the two collections never have the same
shape.  Each test now compares the sorted peaks (the largest value of each
column, rounded down) of all columns of all data frames of the two charts.
This is what the old test compared: one peak for each time series.  The tests
also opened an inspector on the two models.  The calls have no assertion and
are meant for a person who looks at the window; they are not in the new
bodies.

B. Three tests whose chart code in the method before gives KEChart>>addDataFrame:
a block, an OrderedCollection or an Array instead of one data frame
(TestOneSpeciesSpatialScripts, TestDependantConcern, TestMultiSpeciesSpatial).
KEChart>>plot then sends #columnNames to the collection and fails.  The code
now adds one data frame for each series.

C. Visualization>>sumOverTimeSeries: says in its own comment that it does not
work (it sends #name: to a number, and adds a DataSeries to a number).  The
composite data of a visualization (I_species) needs it.  KESimulator gets
#sumOfTimeSeriesOutputsAt:, a data frame with one column that is the sum of
the columns of #timeSeriesOutputsAt:, and the visualization uses it.  The test
TestMultiSpeciesSpatial uses the same method in its part before.

D. KEMapBuilder (TestsModel6SpatialMap, TestsModel6SpatialMapWithEntity).  The
class builds its picture with Roassal 2 (RTMapBuilder, RTMultiLinearColor,
RTColorPalette, RTZoomableView).  Pharo 14 has no Roassal 2, so #open fails,
and the class has no #plot that Visualization>>canvas sends.  The picture is
now a Roassal 3 canvas: one labelled box for each country (white to red by the
value of the country), a grid layout (there is no geography in the data) and a
line for each route.  The test TestsModel6SpatialMap took the first row of the
series where the other test and the DSL take the peak (#columnAt: 1, then
#max); it takes the peak now.  Both tests asked a map for #dataFrames; a map
has #data (numbers).  They compare the sorted data of the maps.

E. TestsIST is the root of the DSL tests.  Its testScript skips when the
class is TestsIST itself.  TestsIST is now an abstract test class (no test instance), so
the skip goes away; shouldInheritSelectors keeps the old rule for the subclasses.  The
three TestsDSLSyntax classes and the two TestsModel6 classes have tests of their own and
never ran the inherited testScript, so nothing else changes."

"The helper block lives in a global for the time of the file-in: the entries file is one
chunk, and a line of temporaries in the middle of a chunk is a syntax error.  The block has
one argument, an Array of class name, selector, start text, end text and new text; the text
from the start text to the end text is replaced.  BlockClosure has no value:value:value:value:value:
(five arguments), so the calls use valueWithArguments:."
Smalltalk at: #KendrickRangeFix put: [ :args | | clsName sel startText endText newText cls source i j |
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

(Smalltalk at: #KendrickRangeFix) valueWithArguments: { #TestOneSpeciesSpatialScripts. #before.
	'chart addDataFrame: [|tmp| tmp := OrderedCollection new.'.
	'tmp] value.'.
	'(1 to: 5) do: [ :patch |
		chart addDataFrame: (simulator timeSeriesOutputsAt: { #status -> #I. #patch -> patch }) ].' }.

(Smalltalk at: #KendrickRangeFix) valueWithArguments: { #TestDependantConcern. #before.
	'chart addDataFrame: [|tmp| tmp := DataFrame new.'.
	'tmp] value.'.
	'(1 to: 5) do: [ :patch |
		chart addDataFrame: (simulator timeSeriesOutputsAt: { #status -> #I. #patch -> patch }) ].' }.

(Smalltalk at: #KendrickRangeFix) valueWithArguments: { #TestMultiSpeciesSpatial. #before.
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

(Smalltalk at: #KendrickRangeFix) valueWithArguments: { #TestsModel6SpatialMap. #before.
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

Smalltalk removeKey: #KendrickRangeFix ifAbsent: [ nil ].
