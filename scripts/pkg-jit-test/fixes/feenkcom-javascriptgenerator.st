"JavaScriptGenerator (feenkcom/JavaScriptGenerator, main): two tests of
JSGExtensionsTest compare the output with a string that has the elements in
the order of the image of the author.  A Dictionary and a Set enumerate in
hash order, and Pharo 14 gives another order (Got '{42 : ''b'' , 3 : 4 , ''a'' : 1}',
Got '{4,1,2,3}').  Stock Cog and our VM give the same result.  The tests now
compare the elements and not their order.  Each check of the old tests is
still there: the braces, the separators, and each element."

fix value: #JSGExtensionsTest value: #testDictionaryWriteJavaScriptOn
	value: [ :source | (source includesSubstring: 'splitOn:') not ]
	value:
'testDictionaryWriteJavaScriptOn
	| dict string pairs |
	dict := {
	''a'' -> 1.
	42 -> ''b''.
	3 -> 4 } asDictionary.
	string := String streamContents: [ :s | dict writeJavaScriptOn: s ].
	"A Dictionary enumerates in hash order: compare the pairs, not their order."
	self assert: string first equals: ${.
	self assert: string last equals: $}.
	pairs := (string copyFrom: 2 to: string size - 1) splitOn: '' , ''.
	self assert: pairs size equals: 3.
	self assert: pairs asSet equals: #( ''''''a'''' : 1'' ''42 : ''''b'''''' ''3 : 4'' ) asSet'.

fix value: #JSGExtensionsTest value: #testSetWriteJavaScriptOn
	value: [ :source | (source includesSubstring: 'splitOn:') not ]
	value:
'testSetWriteJavaScriptOn
	| string elements |
	string := String streamContents: [ :s | #(1 2 3 4) asSet writeJavaScriptOn: s ].
	"A Set enumerates in hash order: compare the elements, not their order."
	self assert: string first equals: ${.
	self assert: string last equals: $}.
	elements := (string copyFrom: 2 to: string size - 1) splitOn: '',''.
	self assert: elements size equals: 4.
	self assert: elements asSet equals: #( ''1'' ''2'' ''3'' ''4'' ) asSet.

	string := String streamContents: [ :s | Set new writeJavaScriptOn: s ].
	self assert: string equals: ''set()'''.
