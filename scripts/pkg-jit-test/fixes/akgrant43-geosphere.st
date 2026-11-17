"akgrant43/GeoSphere: 5 tests fail the same way on Cog and on our VM.  No VM defect.

Group 1 (testDestinationBearingDistance, testDestinationBearingDistance2,
testHaversineDistanceTo, testDMSRoundTrip): the tests write the expected value
with 4 or 5 decimals (53.1884, 102.08, -38.28697) and compare with
'assert: x closeTo: y'.  The package was written for a Pharo where closeTo:
had an absolute margin of 0.0001.  In Pharo 14 TestAsserter>>assert:closeTo:
uses Float defaultComparisonPrecision, which is 1.49e-8.  The computed value
53.1884337736 differs from 53.1884 by 3.4e-5, so the test fails.  The package
code is right.  The repair gives each comparison the margin that the digits of
the expected value allow: 0.0001 for degrees written with 4 or 5 decimals,
0.01 for metres written with 2 decimals.

Group 2 (testDmsOnFromLabels): the test expects '50° 30′ 0.0″'.  The method
GSCoordinates class>>dms:labels:to: prints the seconds with
printShowingDecimalPlaces: 2, which in Pharo 14 gives '0.00'
(Kernel-Tests NumberTest checks that 2 places are always printed).  The test
text is old.  The repair changes the expected text to '0.00'."

fix value: #GSCoordinatesTest value: #testDestinationBearingDistance
	value: [ :source | (source includesSubstring: 'precision:') not ]
	value:
'testDestinationBearingDistance
	| c1 c2 |

	c1 := GSParser parseCoordinates: ''53 deg 19'''' 14" N, 1 deg 43'''' 47" W''.
	c2 := c1 destinationBearing: (GSParser parseDmsAngle: ''96 deg 1'''' 18"'') distance: 124800.
	self assert: c2 latitude closeTo: 53.1884 precision: 0.0001.
	self assert: c2 longitude closeTo: 0.1312 precision: 0.0001.
	^ c2'.

fix value: #GSCoordinatesTest value: #testDestinationBearingDistance2
	value: [ :source | (source includesSubstring: 'precision:') not ]
	value:
'testDestinationBearingDistance2
	| c1 c2 |

	c1 := GSParser parseCoordinates: ''53 deg 19'''' 14" N, 1 deg 43'''' 47" W''.
	c2 := c1 destinationBearing: (GSParser parseDmsAngle: ''196 deg 0'''' 0"'') distance: 124800.
	self assert: c2 latitude closeTo: 52.2418 precision: 0.0001.
	self assert: c2 longitude closeTo: -2.2344 precision: 0.0001.
	^ c2'.

fix value: #GSCoordinatesTest value: #testHaversineDistanceTo
	value: [ :source | (source includesSubstring: 'precision:') not ]
	value:
'testHaversineDistanceTo

	self
		assert: ((13.80351 @ 50.35158) asGSCoordinates haversineDistanceTo: 
			(13.8044 @ 50.3523))
		closeTo: 102.08
		precision: 0.01.

	self
		assert: ((144.61437 @ -38.27304) asGSCoordinates haversineDistanceTo: 
			(144.62024 @ -38.2759))
		closeTo: 603.75
		precision: 0.01'.

fix value: #GSParserTest value: #testDMSRoundTrip
	value: [ :source | (source includesSubstring: 'precision:') not ]
	value:
'testDMSRoundTrip
	"Check that we can parse the generated string format"

	| latitude longitude coordinates |

	latitude := -38.28697.
	longitude := 144.61025.
	coordinates := GSParser parseCoordinates: 
			(longitude @ latitude) asGSCoordinates asDegreesMinutesSeconds.
	self
		assert: coordinates latitude
		closeTo: latitude
		precision: 0.0001.

	self
		assert: coordinates longitude
		closeTo: longitude
		precision: 0.0001'.

fix value: #GSCoordinatesTest value: #testDmsOnFromLabels
	value: [ :source | source includesSubstring: '0.0' , (String with: (Character value: 16r2033)) , '''' ]
	value:
'testDmsOnFromLabels

	| dms |

	dms := String streamContents: [ :stream |
		GSCoordinates dms: 50.5 labels: #(''° '' ''′ '' ''″'') to: stream ].
	self assert: dms equals: ''50° 30′ 0.00″'''.
