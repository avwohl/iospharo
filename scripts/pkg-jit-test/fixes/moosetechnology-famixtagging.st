"moosetechnology/FamixTagging (development), Pharo 14.  The load of the row does not bring in
Famix, so every test fails with UndeclaredVariableRead (the manifest row, not this file,
repairs the load: it loads the Famix baseline first, as the file .dependencies.st of the
repository does).  The two entries below are for the one test that the load repair leaves
open.

Entry 1 (FamixTag>>isLinkedTo:dependancyDirection:in:).  The test
FamixTaggingDependencyDetectorTest>>testProviderMethodToTags has the mark expectedFailure
and the comment 'client/provider methods are not tagged'.  The mark hides this defect: for an
entity E that is not a tag, MooseEntity>>isLinkedTo:dependancyDirection:in: looks at the
dependencies of E, of all children of E, and of the containers of those dependencies.  The tag
version looked only at the dependencies of the tagged entities at their own scope
(allProvidersAtMyScope or allClientsAtMyScope), and not at the children of the tagged
entities, and not at containers.  So a method m1 that calls method2 of the class Class2, with
Class2 tagged, was not a provider or a client in that tag, but the class Class1 of m1 was.  The
repair gives the tag the same levels as MooseEntity.

Entry 2 (FamixTaggingDependencyDetectorTest>>testProviderMethodToTags).  The same test
without the mark expectedFailure and without the flag TODO."

fix value: #FamixTag value: #isLinkedTo:dependancyDirection:in:
	value: [ :source | source includesSubstring: 'dependenciesAtReceiverScopeOf' ]
	value:
'isLinkedTo: aTagOrEntity dependancyDirection: aDirectionStrategy in: aMooseModel
	| dependencies |
	"The same levels as MooseEntity>>isLinkedTo:dependancyDirection:in: for each tagged entity: its own dependencies, the dependencies of all its children, and the containers of all of them."
	dependencies := Set new.
	(self allEntitiesRecursivelyIn: aMooseModel) do: [ :taggedEntity |
		| found |
		found := taggedEntity allChildrenDependenciesThrough: aDirectionStrategy.
		dependencies addAll: found.
		found do: [ :each | dependencies addAll: each allContainers ] ].

	"In the entity is a MooseEntity, with check if it is in its dependencies.
	If it is a tag, we look in the tags of its dependencies and the parents of those tags."
	^ (aTagOrEntity isTag
		ifTrue: [ dependencies flatCollectAsSet: [ :entity | entity tags , (entity tags flatCollect: #allParentTags) ] ]
		ifFalse: [ dependencies ]) includes: aTagOrEntity'.

fix value: #FamixTaggingDependencyDetectorTest value: #testProviderMethodToTags
	value: [ :source | source includesSubstring: 'expectedFailure' ]
	value:
'testProviderMethodToTags
	self createTags.
	self assert: (self m1 allProvidersIn: {self tag2}) size equals: 1.
	self assert: (self m1 allProvidersIn: {self compositeTag2}) size equals: 1'.
