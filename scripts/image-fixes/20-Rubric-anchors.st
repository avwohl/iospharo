fix value: #RubEditingAreaTest value: #testEmbeddedAnchorInTextAreDrawn
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testEmbeddedAnchorInTextAreDrawn
	| circle text canvas|
	circle := CircleMorph new extent: 50 @ 50;color: Color blue.

	self assert: circle owner isNil.
	self assert: circle position equals: 0@0.
	"add the circle to a text string"
	text := (''aaaaa'',(String value: 1),''bbbbbbb'') asText
			addAttribute: (TextAnchor new anchoredMorph: circle).
	"render the text in a RubEditingArea to place the circle in a layout"
	area updateTextWith: text.
	canvas := FormCanvas extent: 500@500.
	canvas fullDrawMorph: area.
	self assert: (circle hasProperty: #hasBeenPositioned).
	self deny: circle position equals: 0@0'.

fix value: #RubEditingAreaTest value: #testEmbeddedAnchorInTextAreDrawnAtRightPosition
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testEmbeddedAnchorInTextAreDrawnAtRightPosition
	| circle longTextWithCircleAtEnd canvas|
	circle := CircleMorphCounter new extent: 50 @ 50;color: Color blue.
	longTextWithCircleAtEnd := ((String loremIpsum: 10000),
		(String value: 1)) asText
			addAttribute: (TextAnchor new anchoredMorph: circle).

	self assert: circle owner isNil.
	self assert: circle position equals: 0@0.
	self assert: circle counter equals: 0.
	area updateTextWith: longTextWithCircleAtEnd.
	canvas := FormCanvas extent: 500@500.
	canvas fullDrawMorph: area.

	self assert: circle counter equals: 1.
	self assert: circle position y > 500'.

fix value: #RubScrolledTextMorphTest value: #testEmbeddedAnchorInTextAreDrawnAtRightPosition
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testEmbeddedAnchorInTextAreDrawnAtRightPosition
	| circle longTextWithCircleAtEnd canvas|
	circle := CircleMorphCounter new extent: 50 @ 50;color: Color blue.
	longTextWithCircleAtEnd := ((String loremIpsum: 10000),
		(String value: 1)) asText
			addAttribute: (TextAnchor new anchoredMorph: circle).

	self assert: circle owner isNil.
	self assert: circle position equals: 0@0.
	morph updateTextWith: longTextWithCircleAtEnd.
	canvas := FormCanvas extent: 500@500.
	canvas fullDrawMorph: morph.

	self assert: circle owner equals: nil.
	self assert: circle counter equals: 0.

	morph scrollToEndOfText.
	canvas := FormCanvas extent: 500@500.
	canvas fullDrawMorph: morph.
	self assert: circle counter > 0'.
