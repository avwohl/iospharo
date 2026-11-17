"Buoy (ba-st/Buoy, release-candidate): OrderedDictionaryExtensionsTest>>testEquals
is a copy of an old OrderedDictionaryTest>>testEquals of Pharo.  It fills two
dictionaries with the same associations in opposite orders and expects that
the two are equal ('For equality, order will not matter').

OrderedDictionary>>= of Pharo 14 compares indexOfKey: for each key, so the
order matters.  The test of Pharo 14 itself (OrderedDictionaryTest>>testEquals)
fills the two dictionaries in the same order.  The test of Buoy fails on stock
Cog and on our VM with 'Got an OrderedDictionary(...) instead of an
OrderedDictionary(...)'.

The repair: the two dictionaries get the associations in the same order, as in
the test of Pharo 14, and a third dictionary with the opposite order must not
be equal."

fix value: #OrderedDictionaryExtensionsTest value: #testEquals
	value: [ :source | source includesSubstring: 'with: self orderedAssociations reversed' ]
	value:
'testEquals
	| dictionaryOne dictionaryTwo dictionaryThree |

	dictionaryOne := self emptyDictionary.
	dictionaryTwo := self emptyDictionary.
	self
		assertDictionary: dictionaryOne
		equals: dictionaryTwo.

	"Pharo 14: the order of the associations matters for equality"
	self orderedAssociations
		with: self orderedAssociations
		do: [:firstAssociation :secondAssociation |
			dictionaryOne add: firstAssociation.
			dictionaryTwo add: secondAssociation.
			self
				assertDictionary: dictionaryOne
				doesNotEqual: self emptyDictionary.
			self
				assertDictionary: dictionaryTwo
				doesNotEqual: self emptyDictionary.
			self
				assertDictionary: dictionaryOne
				equals: dictionaryTwo].

	"The same associations in the opposite order are not equal"
	dictionaryThree := self emptyDictionary.
	self orderedAssociations reversed do: [:each | dictionaryThree add: each].
	self assert: dictionaryThree size equals: dictionaryOne size.
	self
		assertDictionary: dictionaryOne
		doesNotEqual: dictionaryThree.

	self
		assertDictionary: dictionaryOne
		equals: dictionaryTwo'.
