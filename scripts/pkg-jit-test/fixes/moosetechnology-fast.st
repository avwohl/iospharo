"moosetechnology/FAST (v3): the package code and its tests call two selectors
of the old Moose API: Object>>mooseDescription and
ClassDescription>>asMooseDescription.  The Famix development branch moved to
metadescription and asMetadescription (package Moose-Query), and keeps the old
names only in the package Famix-Deprecated.  The baseline of FAST loads the
Famix groups that do not include Famix-Deprecated, so the old names do not
exist in the image: FamixModelComparator>>compareProperties:to: and five
tests of FASTCopyVisitorCodeGeneratorTest fail with doesNotUnderstand.  The
repair changes the six methods of FAST to the new names.  The class that
receives the message is a class (node1 class, FmxTraitsTest... class), so the
right message is asMetadescription (ClassDescription) and not metadescription
(Object>>metadescription would ask the class of the class)."

fix value: #FamixModelComparator value: #compareProperties:to:
	value: [ :source | source includesSubstring: 'mooseDescription' ]
	value:
'compareProperties: node1 to: node2
	"compare the values of the ''properties'' (attributes with primitive types) of the two nodes
	 since the two nodes should be the same class, they have the same properties"

	(node1 class asMetadescription allPrimitiveProperties)
		select: [ :property | self propertyToCompare: property ]
		thenDo:  [ :property || value1 value2 |
			value1 := node1 perform: property implementingSelector.
			value2 := node2 perform: property implementingSelector.

			(value1 = value2) ifFalse: [
			self ast: node1 differ: node2 property: property
		]
	]
'.

fix value: #FASTCopyVisitorCodeGeneratorTest value: #testShouldCopyMultiValuedDerivedProperty
	value: [ :source | source includesSubstring: 'asMooseDescription' ]
	value:
'testShouldCopyMultiValuedDerivedProperty
	| property |

	property := FmxTraitsTestGenerateAccessorBClassB asMetadescription
		allProperties detect: [ :prop | prop name = ''relationToA'' ].

	self assert: property isDerived.
	self assert: (generator shouldCopyProperty: property)'.

fix value: #FASTCopyVisitorCodeGeneratorTest value: #testShouldCopyNotMultiValuedProperty
	value: [ :source | source includesSubstring: 'asMooseDescription' ]
	value:
'testShouldCopyNotMultiValuedProperty
	| property |

	property := FmxTraitsTestGenerateAccessorBClassB asMetadescription
		allProperties detect: [ :prop | prop name = ''relationToA'' ].

	self deny: property isMultivalued.
	self assert: (generator shouldCopyProperty: property)'.

fix value: #FASTCopyVisitorCodeGeneratorTest value: #testShouldCopyPropertyToOtherEntity
	value: [ :source | source includesSubstring: 'asMooseDescription' ]
	value:
'testShouldCopyPropertyToOtherEntity
	| property |

	property := FmxTraitsTestGenerateAccessorBClassA asMetadescription
		allProperties detect: [ :prop | prop name = ''relationToB'' ].

	self assert: (generator shouldCopyProperty: property)'.

fix value: #FASTCopyVisitorCodeGeneratorTest value: #testShouldNotCopyMultivaluedDerivedProperty
	value: [ :source | source includesSubstring: 'asMooseDescription' ]
	value:
'testShouldNotCopyMultivaluedDerivedProperty
	| property |

	property := FmxTraitsTestGenerateAccessorBClassB asMetadescription
		allProperties detect: [ :prop | prop name = ''relationToA'' ].

	"forcing it to be multivalued"
	property isMultivalued: true.
	[
		self assert: property isMultivalued.
		self deny: (generator shouldCopyProperty: property)
	]
	ensure: [ property isMultivalued: false ]'.

fix value: #FASTCopyVisitorCodeGeneratorTest value: #testShouldNotCopyPropertyParent
	value: [ :source | source includesSubstring: 'asMooseDescription' ]
	value:
'testShouldNotCopyPropertyParent
	"property pointing to parent should not be copied"
	| property |

	property := FmxTraitsTestGenerateAccessorBClassA asMetadescription
		allProperties detect: [ :prop | prop name = ''relationToB'' ].

	"forcing it to be to parent"
	property isContainer: true.
	[ self assert: property isContainer.
	  self deny: (generator shouldCopyProperty: property)
	]
	ensure: [ property isContainer: false ]'.

