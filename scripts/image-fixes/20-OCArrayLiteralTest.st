"OCArrayLiteralTest>>testLiteralArrayLongHavingAnotherArrayLong (pharo issue 19058):
 a brace array of up to 32 elements is compiled with push-cons-array, so it needs one stack slot
 per element.  A 32-element brace array as last element of a 32-element brace array needs
 31 + 32 = 63 slots, but a method frame has 56 slots (CompiledCode class>>fullFrameSize), and the
 compiler fails with 'stack including temps is too deep'.  The translator now takes the at:put:
 path when the estimated stack depth of the array, nested brace arrays included, is over 32."
add value: #OCASTTranslator value: #stackDepthOfBraceArrayElement: value: 'private' value: 'stackDepthOfBraceArrayElement: aNode
	"Conservative estimate of the stack slots that the push-cons-array code of aNode needs.  Blocks run in their own frame."

	| deepest |
	aNode isBlock ifTrue: [ ^ 1 ].
	deepest := 0.
	aNode isDynamicArray
		ifTrue: [
			aNode children withIndexDo: [ :each :index |
				deepest := deepest max: index - 1 + (self stackDepthOfBraceArrayElement: each) ].
			^ deepest max: aNode children size ].
	aNode children do: [ :each | deepest := deepest max: (self stackDepthOfBraceArrayElement: each) ].
	^ deepest + 1'.
fix value: #OCASTTranslator value: #visitArrayNode: value: [ :s | (s includesSubstring: 'statements size > 32') and: [ (s includesSubstring: 'stackDepthOfBraceArrayElement:') not ] ] value: 'visitArrayNode: anArrayNode
	| elementNodes |

	"A brace array that needs more stack than the frame has is built with at:put: (pharo issue 19058)."
	(anArrayNode statements size > 32 or: [ (self stackDepthOfBraceArrayElement: anArrayNode) > 32 ]) ifTrue: [ ^ self visitLargeArrayNode: anArrayNode ].

	elementNodes := anArrayNode children.
	elementNodes do: [ :node | self visitNode: node ].
	methodBuilder pushConsArray: elementNodes size'.
fix value: #OCArrayLiteralTest value: #testLiteralArrayLongHavingAnotherArrayLong value: [ :s | s includesSubstring: 'self skip' ] value: 'testLiteralArrayLongHavingAnotherArrayLong
	| method array elements |

	elements := ((1 to: 32) inject: '' '' into: [ :r :e | r , '' . '' , e asString ]).
	array := '' { '' , elements , '' }''.
	array := array copyReplaceAll: ''32'' with: array.

	method := self compile: ''array ^'' , array.
	array := method valueWithReceiver: #().

	self assert: ( array isKindOf: Array).
	self assert: array size equals: 32.
	1 to: 31 do: [ :index | self assert: (array at: index) equals: index ].
	1 to: 32 do: [ :index | self assert: ((array at: 32) at: index) equals: index ]'.
