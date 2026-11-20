"28-simple-formatter-comments.st -- OCSimpleFormatter prints each comment of the AST one time; the test of the simple formatter has its own expected text (2026-11-17).

Cause 1. OCSimpleFormatter printed a comment only on a statement of a sequence, on a temporary, on a block argument and on a method. It dropped the comments of a root expression (parseExpression answers the single statement, not the sequence), the own comments of a block and of a brace array, the comments of method pattern arguments, and the comments of the messages of a cascade. Cause 2. The test OCParseCodeSnippetTest>>testSimpleFormattedCode compared the simple formatter with formattedCode, which is the text of the Enlumineur formatter. Enlumineur drops or moves comments (see the remarks in OCCodeSnippet class>>badComments). Eight snippets passed only because both formatters lost the same comments. Cause 3. The group default of OCCodeSnippet class>>badComments skips the test (the simple one is too much broken), so the test never ran for the 23 snippets.

F1. basicFormatCommentFor: writes a quote inside a comment with the quote doubled.
F2. format: prints the comments of a root expression after the expression.
F3. formatStatementCommentsFor: prints all comments of the statement subtree, sorted by position (hoistedCommentsFor:, collectHoistedCommentsOf:into:). A block, a brace array, a sequence and a method print their own comments themselves, so they stop the collection.
F4. formatBlock:, formatArray:, formatCommentsFor: print the own comments of a block or a brace array after the opening bracket, sorted by position.
F5. privateFormatMethodPatternMonoLineFor: prints the comments of each pattern argument.
F6. OCCodeSnippet>>simpleFormattedCode and OCCodeSnippet class>>simpleFormattedCodes: the expected text of the simple formatter, formattedCode where the two formatters agree.
F7. The test reads simpleFormattedCode. The skip of the group goes."

fix value: #OCSimpleFormatter value: #basicFormatCommentFor:
	value: [ :source | source includesSubstring: 'nextPutAll: aComment contents;' ]
	value:
'basicFormatCommentFor: aComment

	codeStream
		nextPut: $";
		nextPutAll: (aComment contents copyReplaceAll: ''"'' with: ''""'');
		nextPut: $"'.

fix value: #OCSimpleFormatter value: #format:
	value: [ :source | (source includesSubstring: 'formatStatementCommentsFor:') not ]
	value:
'format: aParseTree

	self visitNode: aParseTree.
	(aParseTree isSequence or: [ aParseTree isMethod ]) ifFalse: [
		self formatStatementCommentsFor: aParseTree ].
	^ codeStream contents'.

fix value: #OCSimpleFormatter value: #formatStatementCommentsFor:
	value: [ :source | (source includesSubstring: 'hoistedCommentsFor:') not ]
	value:
'formatStatementCommentsFor: aStatementNode

	(self hoistedCommentsFor: aStatementNode) do: [ :each |
		self
			newLine;
			basicFormatCommentFor: each ]'.

add value: #OCSimpleFormatter value: #hoistedCommentsFor: value: 'private - formatting' value:
'hoistedCommentsFor: aNode

	| found |
	found := OrderedCollection new.
	self collectHoistedCommentsOf: aNode into: found.
	^ found asSortedCollection: [ :a :b | a start <= b start ]'.

add value: #OCSimpleFormatter value: #collectHoistedCommentsOf:into: value: 'private - formatting' value:
'collectHoistedCommentsOf: aNode into: aCollection

	"A sequence, a method, a block and a brace array print their own comments: stop there. A message of a cascade shares the receiver with the other messages: a comment is added one time."
	aNode ifNil: [ ^ self ].
	(aNode isSequence or: [ aNode isMethod or: [ aNode isBlock or: [ aNode isDynamicArray ] ] ]) ifTrue: [ ^ self ].
	aNode comments do: [ :each |
		(aCollection anySatisfy: [ :seen | seen == each ]) ifFalse: [ aCollection add: each ] ].
	aNode children ifNotNil: [ :kids |
		kids do: [ :each | self collectHoistedCommentsOf: each into: aCollection ] ]'.

fix value: #OCSimpleFormatter value: #formatCommentsFor:
	value: [ :source | (source includesSubstring: 'asSortedCollection') not ]
	value:
'formatCommentsFor: aNode

	(aNode comments asSortedCollection: [ :a :b | a start <= b start ]) do: [ :each |
		self
			basicFormatCommentFor: each;
			newLine ]'.

fix value: #OCSimpleFormatter value: #formatBlock:
	value: [ :source | (source includesSubstring: 'formatCommentsFor: aBlockNode') not ]
	value:
'formatBlock: aBlockNode

	self
		formatBlockArgumentsFor: aBlockNode;
		space;
		formatCommentsFor: aBlockNode;
		visitNode: aBlockNode body;
		space'.

fix value: #OCSimpleFormatter value: #formatArray:
	value: [ :source | (source includesSubstring: 'formatCommentsFor: anArrayNode') not ]
	value:
'formatArray: anArrayNode

	self formatCommentsFor: anArrayNode.
	anArrayNode statements
		do: [ :each |
			self visitNode: each.
			self formatStatementCommentsFor: each ]
		separatedBy: [
			codeStream nextPutAll: '' . ''.
			self newLine ]'.

fix value: #OCSimpleFormatter value: #privateFormatMethodPatternMonoLineFor:
	value: [ :source | (source includesSubstring: 'formatStatementCommentsFor: arg') not ]
	value:
'privateFormatMethodPatternMonoLineFor: aMethodNode

	self
		with: aMethodNode selectorParts
		and: aMethodNode arguments
		do: [ :key :arg |
			codeStream nextPutAll: key.
			self space.
			self visitNode: arg.
			self formatStatementCommentsFor: arg ]
		separatedBy: [ self space ]'.

"The expected text of the simple formatter. The key is the snippet source without separators."
add value: #'OCCodeSnippet class' value: #simpleFormattedCodes value: 'accessing' value:
'simpleFormattedCodes

	"Expected output of OCSimpleFormatter where it differs from formattedCode (the output of the Enlumineur formatter, which drops or moves comments). The key is the snippet source without separators. The simple formatter prints each comment one time, after the statement that holds it; a block and a brace array print their comments after the opening bracket."
	^ Dictionary new
		at: ''"com"1"ment"'' put: ''1 "com" "ment"'';
		at: ''"a"1"b"max:"c"2"d"+"e"3"f"abs"g"'' put: ''1 max: 2 + 3 abs "a" "b" "c" "d" "e" "f" "g"'';
		at: ''"a"("b"("c"1"d")"e")"f"'' put: ''1 "a" "b" "c" "d" "e" "f"'';
		at: ''"a"1"b"max:"c"2"d";"e"min:"f"3"g"'' put: ''1 max: 2; min: 3 "a" "b" "c" "d" "e" "f" "g"'';
		at: ''"a"#("b"1"c"two"d"("e"3"f")"g")"h"'' put: ''#( 1 two #( 3 ) ) "a" "b" "c" "d" "e" "f" "g" "h"'';
		at: ''"a"#["b"1"c"2"d"]"e"'' put: ''#[ 1 2 ] "a" "b" "c" "d" "e"'';
		at: ''"a"{"b"1"c"."d"2"e"}"f"'' put: ''{ "a" "b" "f" 1 "c" "d" . 2 "e" }'';
		at: ''"a"^"b"5"c"'' put: ''^ 5 "a" "b" "c"'';
		at: ''"a"foo:"b"x"c"bar:"d"y"e"^"f"x"g"'' put: ''foo: x "a" "b" "c" bar: y "d" "e" ^ x "f" "g"'';
		at: ''1"A"true"B"false"C"nil"D"self"E"super"F"thisContext"G"'' put: ''1 true false nil self super thisContext "A" "E" "F" "G"'';
		at: ''1-1"A"-1abs-1"B"-1max:-1"C"'' put: ''1 - 1 - 1 abs - 1 - 1 max: -1 "A" "B" "C"'';
		at: ''#(aa:bb:"A"cc"B"#aa:bb:cc"C"#cc"D"++"E"#++"F")'' put: ''#(aa:bb: cc #''''aa:bb:cc'''' #cc #''''++'''' #''''++'''') "A" "B" "C" "D" "E" "F"'';
		at: ''#(("A")"B"."C":"D";"E"["F"]"G"{"H"}"I"^"J")'' put: ''#(#() #''''.'''' #'''':'''' #'''';'''' #''''['''' #'''']'''' #''''{'''' #''''}'''' #''''^'''') "A" "B" "C" "D" "E" "F" "G" "H" "I" "J"'';
		at: ''#(#"A"##"B")'' put: ''#(# ##) "A" "B"'';
		at: ''#(:="A"aa:="B":=="C"cc:=="D")'' put: ''#(#'''':='''' aa: #= #'''':='''' #= cc: #''''=='''') "A" "B" "C" "D"'';
		yourself'.

add value: #OCCodeSnippet value: #simpleFormattedCode value: 'accessing' value:
'simpleFormattedCode

	"The expected output of OCSimpleFormatter: formattedCode, unless the two formatters differ."
	^ self class simpleFormattedCodes
		at: ((source ifNil: [ '''' ]) reject: #isSeparator)
		ifAbsent: [ self formattedCode ]'.

fix value: #OCParseCodeSnippetTest value: #testSimpleFormattedCode
	value: [ :source | source includesSubstring: 'snippet formattedCode reject' ]
	value: ((OCParseCodeSnippetTest >> #testSimpleFormattedCode) sourceCode
		copyReplaceAll: 'snippet formattedCode reject'
		with: 'snippet simpleFormattedCode reject').

fix value: #'OCCodeSnippet class' value: #badComments
	value: [ :source | source includesSubstring: 'skip: #testSimpleFormattedCode;' ]
	value: (((OCCodeSnippet class >> #badComments) sourceCode
		copyReplaceAll: 'skip: #testSimpleFormattedCode;' with: '')
		copyReplaceAll: '"The simple one is too much broken"' with: '').

