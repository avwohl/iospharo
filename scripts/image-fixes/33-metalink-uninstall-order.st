"Reflectivity: the uninstall of an object-specific link depends on the order
of a set, and can remove the method of the object two times (2026-11-17).

Symptom: LinkInstallerTest>>testLinkOnTempVarForObject failed in some sweeps
of our VM (2 of 13 runs of its batch, never in 80 runs of the class alone)
with 'KeyNotFound: key #methodWithTempVarAccess not found in
MethodDictionary', from ClassDescription>>removeSelector: on the anonymous
class of the object.  Each later test of the class then failed in the same
way.

Cause:

U1. MetaLinkInstaller>>uninstallFromAllAnonymousNodes: walks 'aMetaLink
nodes' (an IdentitySet).  For each node it calls uninstall:fromNode:forObject:,
which ends with 'aMetaLink removeNode: node': the loop removes elements from
the set that it walks.  Set>>remove: moves a later element of the same
collision chain back into the slot that the walk has left, and the walk does
not see that element.  MetaLink>>uninstall then takes the link off the node
that the walk skipped, so 'link nodes' is empty and a test sees no defect.
But the installer still has the node in its node mapper for the object, the
anonymous class of the object keeps its method, and the object keeps the
anonymous class.

U2. MetaLinkNodesMapper>>findNodeLike:forObject: answers the first node of
the object that is equivalent to the given node (the same selector, source
and position).  A node that U1 left in the mapper is equivalent to the node
at the same position of a later installation for the same object.  When the
set order gives the old node first, the installer removes the old node.  The
old node is in an old AST with no link, so canRemoveMethodNodeContaining:
answers true and the method of the anonymous class goes.  The uninstall of the
last new node removes the method again, and removeSelector: signals
KeyNotFound.  testLinkOnTempVarForObject makes three installations for one
object (read, write, all), so it can reach this state.

Measured with scripts/cogdiff/metalink_uninstall_order.st on an image with no
repair.  The part of the script that forces the order of the set gives the
KeyNotFound on stock Cog and on our VM.  The part with the order of the
identity hashes gives the KeyNotFound in 12 of 300 rounds on stock Cog and
in 9 of 300 on our VM, and 332 and 368 uninstalls leave a node in the mapper.

The repair: the walk uses a copy of the nodes and of the objects (U1), and
the mapper answers the given node itself when the object has it (U2).

The two new tests force the order with an IdentitySet subclass that gives
each element the same first slot.  Measured on stock Cog and on our VM: the
two tests fail before the repair (40 other tests of the class pass), and the
42 tests of the class pass after the repair.  After the repair the script
finds no node left in the mapper in 300 rounds."

fix value: #MetaLinkInstaller value: #uninstallFromAllAnonymousNodes:
	value: [ :source | (source includesSubstring: 'asArray') not ]
	value:
'uninstallFromAllAnonymousNodes: aMetaLink

	| objectsForLink |
	(linksRegistry isLinkObjectSpecific: aMetaLink) ifFalse: [ ^ self ].
	objectsForLink := linksRegistry objectsForLink: aMetaLink.
	"Walk copies.  uninstall:fromNode:forObject: removes the node from aMetaLink nodes (an IdentitySet).  A removal from a hashed collection moves later entries of the collision chain back, so a walk over the collection itself skipped a node: the node kept its entry in the mapper and its link until the end of MetaLink>>uninstall, and a later install found the old node."
	objectsForLink asArray do: [ :object |
		aMetaLink nodes asArray do: [ :node |
			((aMetaLink nodes includes: node) and: [ linkToNodesMapper isNode: node ownedBy: object ])
				ifTrue: [ self uninstall: aMetaLink fromNode: node forObject: object ] ] ]'.

fix value: #MetaLinkNodesMapper value: #findNodeLike:forObject:
	value: [ :source | (source includesSubstring: 'includes: aNode') not ]
	value:
'findNodeLike: aNode forObject: anObject
	"The node itself when the object has it.  A node of an earlier installation at the same source position is equivalent too, and must not win."

	| nodesForObject |
	nodesForObject := nodesForObjects at: anObject ifAbsent: [ #() ].
	(nodesForObject includes: aNode) ifTrue: [ ^ aNode ].
	^ nodesForObject detect: [ :node | node isEquivalentTo: aNode ] ifNone: [ nil ]'.

add value: #LinkInstallerTest value: #nodeSetHomedAt: value: 'helpers'
	value:
'nodeSetHomedAt: aSlotExpression
	"An IdentitySet in which every element has the same home slot, so the layout does not depend on the identity hashes of the VM.  aSlotExpression is the source of the home slot: ''1'' (the first slot) or ''array size'' (the last slot)."

	| setClass |
	setClass := IdentitySet newAnonymousSubclass.
	setClass compile: ''scanFor: anObject
	| index start |
	index := start := '', aSlotExpression, ''.
	[ | element |
		((element := array at: index) == nil or: [ element enclosedElement == anObject ])
			ifTrue: [ ^ index ].
		(index := index \\ array size + 1) = start ] whileFalse.
	self errorNoFreeSpace''.
	^ setClass new'.

add value: #LinkInstallerTest value: #testUninstallVisitsEveryNodeWhenTheNodeSetCollides value: 'tests'
	value:
'testUninstallVisitsEveryNodeWhenTheNodeSetCollides
	"Two nodes of one link have the same home slot in the node set of the link.  MetaLink>>uninstall removes each node from this set while it walks the set; the walk used to skip the second node.  The object then stayed registered for the old node."

	| link obj temp reads |
	link := MetaLink new.
	obj := ReflectivityExamples2 new.
	temp := (ReflectivityExamples2 >> #methodWithTempVarAccess) lookupVar: #temp.
	reads := (temp accessingNodesFor: #read) asSortedCollection: [ :a :b | a start <= b start ].
	link instVarNamed: #nodes put: (self nodeSetHomedAt: ''1'').
	reads do: [ :each | each link: link forObject: obj ].
	self assert: link nodes size equals: 2.
	link uninstall.
	self assert: link nodes isEmpty.
	self assert: (MetaLink linkInstaller linkToNodesMapper findNodesForObject: obj) isEmpty'.

add value: #LinkInstallerTest value: #testUninstallDoesNotRemoveTheMethodTwiceAfterASkippedNode value: 'tests'
	value:
'testUninstallDoesNotRemoveTheMethodTwiceAfterASkippedNode
	"The three rounds of testLinkOnTempVarForObject with a node set that has a fixed layout.  Round 1: the two nodes have the same home slot (the old walk skipped the second node).  Round 2: the nodes leave in the other order and the method of the anonymous class is removed.  Round 3: the install finds the old node of round 1, which belongs to the old AST, and the uninstall removed the method a second time (KeyNotFound)."

	| link obj temp reads writes |
	link := MetaLink new.
	obj := ReflectivityExamples2 new.
	temp := (ReflectivityExamples2 >> #methodWithTempVarAccess) lookupVar: #temp.
	reads := (temp accessingNodesFor: #read) asSortedCollection: [ :a :b | a start <= b start ].
	writes := (temp accessingNodesFor: #write) asSortedCollection: [ :a :b | a start <= b start ].

	[ link instVarNamed: #nodes put: (self nodeSetHomedAt: ''1'').
	reads do: [ :each | each link: link forObject: obj ].
	link uninstall.

	link instVarNamed: #nodes put: (self nodeSetHomedAt: ''array size'').
	writes do: [ :each | each link: link forObject: obj ].
	link uninstall.

	link instVarNamed: #nodes put: (self nodeSetHomedAt: ''array size'').
	reads do: [ :each | each link: link forObject: obj ].
	self shouldnt: [ link uninstall ] raise: KeyNotFound.
	self assert: link nodes isEmpty.
	self assert: (MetaLink linkInstaller linkToNodesMapper findNodesForObject: obj) isEmpty ]
		ensure: [ link instVarNamed: #nodes put: IdentitySet new ]'.
