"reugalabf/DatasetUtilities4Pharo (main, 3638c6e).  Four defects.  The record
of the repair: docs/upstream/package-repairs.md.

1. DatasetAggregator>>sumAggregationBlock, maxAggregationBlock and
   minAggregationBlock call Number readFrom: on the value of the row.  A row of
   a JSON source (NeoJSON) holds a Number, and Number class>>readFrom: wants a
   String or a stream: NumberParser>>on: keeps the Float as the source stream
   and peekSignIsMinus sends #peekFor: to it.  A row of a CSV source holds a
   String.  The repair parses a String and passes a Number on.  A String that
   is not a number (the 'foo' of testSingleBadDataset) still signals an error.
   (2 errors of DictionaryAggregatorTest)
2. DictionaryAggregatorTest>>testSingleValueDataset asserts 'col size = 3' for
   a CSV text of 4 data rows and a trailing blank line, and then only calls
   #inspect.  The repair reads the text without the trailing blank line, asserts
   4 rows and asserts the hourly minimum and maximum values.  (1 failure)
   DictionaryAggregatorTest>>testMultiMappingsFilter ends with 'aggregated
   inspect', a debug leftover that opens a window.  The repair removes the line.
3. ObjectAggregatorTest (8 tests) and DatasetAggregatorTest>>getCollectionToAggregate
   use the classes FlareSenseDevelopment and FlareSenseRawEndpointClient.  Neither
   class is in the repository, in the baseline or in the image.  The repair
   makes the two classes from the JSON text and the expected values of the
   tests.  (8 errors)"

fix value: #DatasetAggregator value: #sumAggregationBlock
	value: [ :source | source includesSubstring: 'Number readFrom:' ]
	value:
'sumAggregationBlock
"
Classes that takes a CSV file and tranform it to Dictionary, JSON or Domain Object (JSONSchema)
"

	^ [ :bulk :outReg :assocKey | 
	  | value |
	  value := self numberFrom: (self computeValueOf: assocKey key asSymbol from: bulk).
	  value isNumber ifTrue: [ 
		  outReg
			  at: assocKey value
			  ifPresent: [ :v | outReg at: assocKey value put: value + v ]
			  ifAbsent: [ outReg at: assocKey value put: value ] ].
	  outReg ]'.

fix value: #DatasetAggregator value: #maxAggregationBlock
	value: [ :source | source includesSubstring: 'Number readFrom:' ]
	value:
'maxAggregationBlock

	^ [ :bulk :outReg :assocKey | 
	  | value |
	  value := self numberFrom: (self computeValueOf: assocKey key asSymbol from: bulk).
	  value isNumber ifTrue: [ 
		  outReg
			  at: assocKey value
			  ifPresent: [ :v | outReg at: assocKey value  put: (value max: v) ]
			  ifAbsent: [ outReg at: assocKey value put: value ] ].
	  outReg ]'.

fix value: #DatasetAggregator value: #minAggregationBlock
	value: [ :source | source includesSubstring: 'Number readFrom:' ]
	value:
'minAggregationBlock

	^ [ :bulk :outReg :assocKey | 
	  | value |
	  value := self numberFrom: (self computeValueOf: assocKey key asSymbol from: bulk).
	  value isNumber ifTrue: [ 
		  outReg
			  at: assocKey value
			  ifPresent: [ :v | 
				  v ifNotNil: [ 
					  outReg at: assocKey value put: (value min: v);
							   yourself ] ]
			  ifAbsent: [ 
				  value ifNotNil: [ 
					  outReg at: assocKey value put: value ] ] ].
	  outReg ]'.

add value: #DatasetAggregator value: #numberFrom: value: 'private' value:
'numberFrom: aValue
	"A row of a CSV source holds a String, a row of a JSON source or of a domain object holds a Number.  Number class>>readFrom: takes a String or a stream only."

	^ aValue isString
		  ifTrue: [ Number readFrom: aValue ]
		  ifFalse: [ aValue ]'.

fix value: #DictionaryAggregatorTest value: #testSingleValueDataset
	value: [ :source | source includesSubstring: 'col size = 3' ]
	value:
'testSingleValueDataset

	| col agregator agregated |
	"The text has a blank line at its end: the reader would make one more record from the blank line"
	col := (CSV2DictionaryStream source: (ReadStream on: self completeDataset trimRight)) contents collect: [ :each |
		       each at: #key put: (each at: #h1).
		       each ].

	self assert: col size equals: 4.

	agregator := DictionaryAggregator new.
	agregator
		hourly;
		keyAspect: #key;
		addMinMappingFor: #h2 as: #minH2;
		addMaxMappingFor: #h3 as: #maxH3.

	agregated := agregator aggregate: col.

	self assert: agregated size equals: 2.
	self assert: (agregated first value at: #minH2) equals: -2.
	self assert: (agregated first value at: #maxH3) equals: 4.
	self assert: (agregated last value at: #minH2) equals: -3.
	self assert: (agregated last value at: #maxH3) equals: 3'.

fix value: #DictionaryAggregatorTest value: #testMultiMappingsFilter
	value: [ :source | source includesSubstring: 'inspect' ]
	value:
'testMultiMappingsFilter

	| col agregator aggregated |
	col := self getCollectionToAggregate.


	self should: [ col size = 3 ].
	self should: [ 
		((col first at: #when) printString) = ''2023-11-28T13:03:38.897+00:00'' ].
	agregator := DictionaryAggregator new.
	agregator
		afterDateandTime: (DateAndTime fromString: ''2023-11-28T13:03:38.897Z'') ;
		keyAspect: #when;
		addAverageMappingFor: #hum;
		addSumMappingFor: #tem;
		addMaxMappingFor: #wind_ws_gust;
		addMinMappingFor: #temperature_ws.


	aggregated := agregator aggregate: col.
	self should: [ aggregated size = 2 ].
	self should: [ 
		(aggregated first value at: ''when'')
		= ''2023-11-28T13:38:45.393+00:00'' ].
	self should: [ (aggregated first value at: #hum) = 66.7 ]'.

"The two classes of the FlareSense project that the tests use."
(Smalltalk globals includesKey: #FlareSenseDevelopment) ifFalse: [
	(Object << #FlareSenseDevelopment
		slots: { #wind_ws_gust. #when. #timeDeltaM. #humidity_ws. #nodeId. #wind_wspeed_s. #tem. #hum. #pressure_ws. #raing. #temperature_ws. #fw. #wind_direction_ws };
		package: 'DatasetUtilities-Tests';
		yourself) install ].

(Smalltalk globals includesKey: #FlareSenseRawEndpointClient) ifFalse: [
	(Object << #FlareSenseRawEndpointClient
		package: 'DatasetUtilities-Tests';
		yourself) install ].

#(#wind_ws_gust #when #timeDeltaM #humidity_ws #nodeId #wind_wspeed_s #tem #hum #pressure_ws #raing #temperature_ws #fw #wind_direction_ws) do: [ :name |
	add value: #FlareSenseDevelopment value: name value: 'accessing' value:
		name, String lf, String tab, '^ ', name ].

add value: #'FlareSenseDevelopment class' value: #neoJsonMapping: value: 'json' value:
'neoJsonMapping: mapper

	mapper for: self do: [ :mapping |
		mapping mapAllInstVars.
		(mapping mapInstVar: #when) valueSchema: DateAndTime ].
	mapper for: DateAndTime customDo: [ :mapping |
		mapping decoder: [ :string | DateAndTime fromString: string ] ]'.

add value: #FlareSenseRawEndpointClient value: #transformJSONFrom: value: 'json' value:
'transformJSONFrom: aReadStream
	"The raw text has one record for each reading period, and each measure is an Array with one value for each sample of the period.  Answer the JSON text with one record for each sample.  The time of the record is the time of the period, and a period of n samples starts (n - 1) steps before it (step: timeDeltaM minutes)."

	| raw flat |
	raw := (NeoJSONReader on: aReadStream) next.
	flat := OrderedCollection new.
	raw do: [ :entry |
		| count when step |
		count := (entry at: ''hum'') size.
		when := DateAndTime fromString: (entry at: ''when'').
		step := Duration minutes: (entry at: ''timeDeltaM'').
		1 to: count do: [ :index |
			| row |
			row := Dictionary new.
			entry keysAndValuesDo: [ :key :value |
				(value isCollection and: [ value isString not ])
					ifTrue: [ row at: key put: (value at: index) ]
					ifFalse: [ row at: key put: value ] ].
			row at: ''when'' put: (when - (step * (count - index))) printString.
			flat add: row ] ].
	^ NeoJSONWriter toString: flat'.
